# UDPspeeder Optimization Results

Benchmarked on Intel Core i5-7300U (Kaby Lake, 2C/4T, SSE4.2 + AVX2).
Target platforms: Intel N150 (Alder Lake-N), Mediatek Filogic (ARMv8),
TP-Link TL-WDR4900 (Freescale P1014 e500v2, PowerPC SPE).

## End-to-end throughput (GitHub Actions, 1400B UDP, loopback)

| Config | Baseline | Current | Improvement |
|---|---|---|---|
| no-fec | 618-861 Mbps | 942-1517 Mbps | **+48-76%** |
| fec 20:10 | 364-509 Mbps | 657-1082 Mbps | **+81-113%** |

Ranges reflect CI host variance between runs. Current/baseline measured on
the same host within each run.

FEC overhead dropped from ~41% of throughput (baseline) to ~30% (current).

## Microbenchmark summary at 1500B

| Path | Before | After | Speedup |
|---|---|---|---|
| addmul1 (GF multiply-accumulate) | 665 ns | 58 ns | 11.5x |
| rs_encode k=10 n=15 | 34,000 ns | 3,100 ns | 11x |
| rs_decode k=10 n=15 | 36,000 ns | 4,500 ns | 8x |
| do_cook (CRC32C + obscure + XOR) | 3,000 ns | 713 ns | 4.2x |
| cook XOR encryption only | 1,826 ns | 131 ns | 14x |

## Commits

### 1. Benchmark and test harness

Nanobench-based microbenchmarks and correctness tests for FEC, CRC32,
and packet cooking. Enables data-driven optimization and CI regression
detection.

### 2. CRC32C (Castagnoli) replacing CRC32 (zlib)

Switched packet checksum from software CRC32 to CRC32C with hardware
acceleration via SSE4.2 (`_mm_crc32_u64`) and ARMv8-CRC (`__crc32cb`).
Software fallback for CPUs without hardware support.

### 3. SSSE3 and NEON addmul1 vectorization

The GF(2^8) multiply-accumulate (`addmul1`) is the inner loop of Reed-Solomon
encode and decode. The scalar implementation uses a 64KB lookup table — one
byte at a time.

SSSE3/NEON use nibble decomposition: split each input byte into low/high
nibbles, use `PSHUFB`/`TBL` as a 16-entry parallel lookup, XOR the results.
Processes 16 bytes per iteration. Scalar tail for remainder.

### 4. AVX2 addmul1 with runtime CPUID dispatch

Same nibble-decomposition approach widened to 256-bit registers. `VPSHUFB`
operates on two independent 128-bit lanes, so the 16-byte lookup table is
duplicated into both lanes via `_mm256_broadcastsi128_si256`.

Runtime dispatch: 3-phase CPUID check (OSXSAVE + XCR0 AVX state + leaf 7
bit 5). Function pointer `addmul1_x86_fn` resolved in `init_fec()`.
SSE tail handles the 16-31 byte remainder.

### 5. Packet cook refactor into context struct

Extracted the cook pipeline (CRC32C + XOR obfuscation + XOR encryption)
from `packet.cpp` into self-contained `packet_cook.cpp` with a `cook_ctx_t`
context struct. Eliminates 6 global variables. No dependency on common.h,
libev, or networking code — enables benchmarking and testing in isolation.

### 6. SSE2 and NEON XOR vectorization for cook pipeline

The cook XOR loops (key encryption and IV obfuscation) were byte-at-a-time
with data-dependent branches (`if (key[j] == 0) j = 0`). Compilers cannot
auto-vectorize these.

Pre-expand the repeating key/IV pattern into a tile whose length is
`lcm(pattern_len, 16)`, then XOR 16 bytes at a time with SSE2 or NEON.
Key tile is computed once at startup (`cook_ctx_prepare_key`), IV tile is
built per-packet on the stack (4-32 bytes, tile at most 496 bytes).

### 7. Eliminate per-call malloc in fec_decode

`fec_decode` performed 7+ malloc/free pairs per call. Replaced with:
- `invert_mat`: 5 heap buffers replaced with stack VLAs (max 3.6 KB)
- `build_decode_matrix`: k*k matrix pre-allocated in `fec_parms` struct
- `fec_decode`: per-row data buffers replaced with contiguous scratch
  in `fec_parms`, lazily grown on first use

Eliminates allocation jitter on the real-time decode path.

### 8. io_uring multishot receive with provided buffer rings

Replaced per-packet `recvfrom()` / `recv()` syscalls with io_uring
multishot receive using kernel-managed provided buffer rings. The kernel
fills pre-registered buffers and posts completions to a shared ring —
userspace drains batches of completions without syscalls per packet.

Key implementation details:
- **Multishot recvmsg** for unconnected sockets (server local, client local)
  with `IORING_OP_RECVMSG` + `IORING_RECV_MULTISHOT` + `IOSQE_BUFFER_SELECT`
- **Multishot recv** for connected sockets (server remote, client remote)
- **Provided buffer ring** (`IORING_REGISTER_PBUF_RING`): 256 buffers,
  power-of-2 ring, kernel picks buffers without userspace involvement
- **Batched CQ drain**: single `acquire` load on CQ tail, process all ready
  CQEs, single `release` store to advance CQ head
- **Batched buffer recycling**: deferred ring entries with single atomic
  tail commit per batch
- **Combined submit+flush**: `io_uring_enter(IORING_ENTER_SQ_WAKEUP |
  IORING_ENTER_GETEVENTS)` — one syscall for SQE submission and CQE
  materialization
- **Zero-copy paths**: all four socket paths process directly from provided
  buffers. recvmsg paths (unconnected) have 140+ bytes natural headroom.
  recv paths (connected) use `URING_RECV_HEADROOM` (4 bytes) reserved before
  each buffer for in-place conv header insertion, eliminating per-packet memcpy.
- **COOP_TASKRUN + SINGLE_ISSUER** flags with fallback for older kernels
- **CQ ring sized 4x buffer count** to avoid multishot stalls
- Graceful fallback to `recvfrom()` on older kernels or non-Linux

Bugs fixed during development:
- `io_uring_recvmsg_out` payload offset must use template `msg_namelen`
  (128 bytes for `sockaddr_storage`), not `hdr->namelen` (actual, e.g. 16)
- CQ tail read requires `acquire` barrier (correctness on ARM/NEON targets)
- Ring fd notification gap after CQ drain: explicit `IORING_ENTER_GETEVENTS`
  flush needed to materialize deferred completions

GitHub Actions throughput (no-fec, 1400B UDP, loopback):

| Path | Median Mbps | Runs |
|---|---|---|
| io_uring multishot | 798.5 | 784.8, 798.5, 837.8 |
| recvfrom baseline | 629.9 | 627.6, 629.9, 654.0 |
| **Improvement** | **+27%** | |

### 9. sendmmsg batching for FEC output

Replaced per-packet `sendto()` calls after FEC encoding with a single
`sendmmsg()` call per batch. When the delay manager detects all output
delays are zero (the common case), it routes through `my_send_batch()`
which cooks all packets then issues one `sendmmsg()` for the entire batch.

Typically 20-30 packets per FEC batch → 20-30 syscalls reduced to 1.

### 10. Flat array replacing std::map in FEC decode

Replaced `std::map<int, fec_data_t*>` shard index in `fec_decode_manager_t`
with a flat pre-allocated array indexed directly by shard position. Eliminates
per-shard tree traversal (O(log n) → O(1)) and per-node heap allocation.

### 11. Zero-copy io_uring recv for conv header

The conv header (4 bytes) was previously inserted via `memmove` after receive.
Reserved `URING_RECV_HEADROOM` (4 bytes) before each provided buffer at
registration time. Recv paths now write the conv header directly into the
headroom, avoiding any per-packet memcpy/memmove.

### 12. Anti-replay direct-mapped table

Replaced `unordered_map<u32_t, info_t>` + `u64_t[30000]` ring buffer (~2 MB
scattered) with a `u32_t[32768]` direct-mapped table (128 KB contiguous).

Design: `table[seq & (SIZE-1)] = seq` to mark seen, `table[seq & (SIZE-1)] != seq`
to check validity. Power-of-2 size for bitwise modulo. Old entries naturally
evicted when new seqs map to the same slot. Effective replay window ~32K groups,
comparable to the old 30K ring buffer. No timeout logic needed.

Per-shard cost: single array access + compare vs hash computation + pointer chase.

### 13. Flat group table in FEC decode with bitmap shard tracking

Replaced `unordered_map<u32_t, fec_group_t>` with a pre-allocated direct-mapped
array sized `next_pow2(fec_buff_num * 2)`. Safe because FEC sequence numbers
are monotonically increasing — consecutive seqs map to distinct slots, so no
two concurrent groups collide when table size exceeds max concurrent groups.

Shard tracking uses a 32-byte bitmap (`u32_t[8]`, 256 bits) instead of
`memset(shard_idx, -1, 1024)`. Only the bitmap is cleared on group creation
(32 bytes vs 1024 bytes). The `shard_idx[]` array is only accessed through
`has_shard()`/`set_shard()` which check the bitmap first.

Eliminates ~50K malloc/free pairs per second at line rate (one allocation per
FEC group for the map node containing the ~1 KB `fec_group_t`).

### 14. Final hot-path micro-optimizations

Replaced per-struct `memset(&msgs[i], 0, sizeof(msgs[i]))` (~64 bytes)
in the sendmmsg loop with targeted field writes (6 fields, ~48 bytes
skipped per struct × 20-30 packets per FEC batch). Merged two separate
loops in type==1 FEC decode (pre-init + populate) into one. Added early
`pad > 0` check to skip shard padding memset when the shard is already
at max_len.

Each individually below CI noise floor. Combined: 1-3%.

### 15. PowerPC e500v2 SPE XOR for cook pipeline

Added SPE (Signal Processing Extension) assembly for the XOR cook stage on
PowerPC e500v2 (Freescale P1014, used in TP-Link TL-WDR4900 running OpenWrt).

The e500v2 has no AltiVec/VMX. SPE provides 64-bit operations: `evldd`/`evstdd`
(8-byte aligned load/store) and `evxor` (64-bit XOR). GCC 9+ removed SPE
intrinsics, so this is standalone assembly (`xor_spe.S`) following the Linux
kernel pattern (`arch/powerpc/crypto/aes-spe-core.S`).

Implementation details:
- **4x unrolled main loop**: 32 bytes/iteration with `evldd`/`evxor`/`evstdd`
- **Alignment handling**: scalar head loop until data is 8-byte aligned,
  8-byte tail loop, then byte tail for remainder
- **Tile wrap**: offset tracking with compare-and-reset per doubleword
- **Build guard**: `HAVE_PPC_SPE` define, set via `make SPE=1`
- **Word-width generic fallback**: non-SPE generic platforms (MIPS, RISC-V,
  ARMv7) now use `sizeof(unsigned long)` XOR instead of byte-at-a-time

PPC assembly gotchas fixed during development:
- OpenWrt binutils 2.44 requires `%r` register prefix (`%r0`, `%r3`); bare
  `r0` is treated as a symbol reference ("unsupported relocation" errors)
- PPC r0-as-zero: `addi rD, r0, imm` treats r0 as literal 0, not the
  register. Fixed by using `addic` (no r0 special case, but clobbers XER[CA])
- `evldd` reads 8 bytes at tile+offset; after unaligned head loop, offset
  can be 1-7, straddling tile boundary. Fixed with `COOK_VEC_WIDTH` padding
  bytes at end of tile buffers

SPE only helps the XOR stage of cook. It cannot help `addmul1` (requires
byte-level shuffle, absent on SPE) or CRC32C (no hardware CRC on e500v2).

PowerPC e500v2 microbenchmarks (QEMU, GitHub Actions):

| Path | Baseline | Current | Speedup |
|---|---|---|---|
| crc32c/1500B (sw slicing-by-8) | 2,609 ns | 1,804 ns | **1.4x** |
| rs_encode k=10 n=15 | 182,166 ns | 123,385 ns | **1.5x** |
| rs_decode k=10 n=15 | 169,905 ns | 133,963 ns | **1.3x** |
| addmul1/1500B (scalar) | 2,453 ns | 2,447 ns | 1.0x |

RS encode/decode improvement is from pre-allocated decode buffers (#7), not
SPE. CRC32C improvement is from switching CRC32-zlib to CRC32C-Castagnoli
(software slicing-by-8 table, #2). addmul1 is identical (both scalar).

Cook pipeline numbers (current only, no baseline cook tests):

| Path | PPC (QEMU) ns |
|---|---|
| do_cook/1500B | 4,090 |
| de_cook/1500B | 3,971 |
| cook_xor_only/1500B | 1,091 |
| cook_obscure_only/1500B | 1,415 |
| cook_crc32_only/1500B | 1,983 |

Files: `xor_spe.S` (new), `packet_cook.cpp`, `makefile`, `.github/workflows/ci.yml`

## Analysis and diminishing returns

After 15 optimizations, the codebase is within 10% of the theoretical
floor (see below). Remaining overhead is irreducible:

**Syscall overhead**: Eliminated. io_uring multishot recv batches receives
without per-packet syscalls. sendmmsg batches sends.

**Compute hotspots**: SIMD-vectorized. GF(2^8) multiply-accumulate uses
AVX2/SSSE3/NEON. CRC32C uses hardware instructions. XOR cook uses
SSE2/NEON/SPE (PPC e500v2). Word-width fallback for generic platforms.

**Allocation overhead**: Eliminated from hot paths. FEC decode uses pre-allocated
buffers, flat arrays, and direct-mapped tables. No `malloc`/`free` per packet
or per group.

**Memory copies**: Two per-packet memcpy remain and are architecturally necessary:
1. `blob_encode_t::input()` — packets must be packed contiguously before RS can
   slice them into equal-length shards. Shard boundaries aren't known until the
   batch is complete (depends on total data size and optimal data_num selection).
2. `fec_decode_manager_t::input()` — received shards must be copied into owned
   buffers because RS decode modifies data in-place.

Each copies ~1400 bytes per packet. At 1.5M packets/sec, that's ~4 GB/sec of
memcpy bandwidth — real but fundamental to the FEC architecture.

## Theoretical FEC overhead floor

For a given FEC config k:r (k data shards, r redundant, n=k+r total), the
minimum per-packet overhead has three irreducible components:

### 1. Wire amplification

Every k application packets produce n=k+r packets on the wire. Goodput
cannot exceed k/n of wire capacity regardless of CPU speed.

| Config | k | r | n | Wire overhead |
|---|---|---|---|---|
| fec 20:10 | 20 | 10 | 30 | **33% lost** (goodput ≤ 67% of no-fec) |
| fec 10:5 | 10 | 5 | 15 | **33% lost** |
| fec 5:3 | 5 | 3 | 8 | **38% lost** (goodput ≤ 63%) |

This is information-theoretic: you must transmit r/k extra data.

### 2. RS encode compute (per application packet)

Encode generates r parity shards. Each parity shard requires k addmul1
calls over shard_len bytes (`lib/fec.cpp:940-944`). Per batch of k packets:

    total_addmul1 = r × k    calls at shard_len ≈ 1400 bytes
    per_app_packet = r        addmul1(shard_len) calls

| Config | addmul1 per pkt | ns/pkt (x86 AVX2) | ns/pkt (PPC scalar) |
|---|---|---|---|
| fec 20:10 | 10 | 10 × 38 = **380** | 10 × 2447 = **24,470** |
| fec 10:5 | 5 | 5 × 38 = **190** | 5 × 2447 = **12,235** |
| fec 5:3 | 3 | 3 × 38 = **114** | 3 × 2447 = **7,341** |

These are the pure addmul1 cost; each call also includes a bzero of the
shard buffer (first iteration).

### 3. Cook amplification

Every shard (data + parity) is cooked before send and de-cooked after
receive. Per application packet: n/k cook + n/k de_cook calls.

| Config | cook+de_cook/pkt | ns/pkt (x86 AVX2) |
|---|---|---|
| fec 20:10 | 1.5 × (351 + 230) = **872** | (no-fec: 351 + 230 = 581) |
| fec 10:5 | 1.5 × (351 + 230) = **872** | |
| fec 5:3 | 1.6 × (351 + 230) = **929** | |

### Combined floor (x86_64 AVX2, fec 20:10, no loss)

| Component | Per-app-pkt (ns) | Notes |
|---|---|---|
| RS encode | 380 | 10 × addmul1(1400B) |
| Cook amplification | +291 | 0.5 extra cook+de_cook |
| memcpy (blob input) | ~35 | 1400B at ~40 GB/s L1 |
| memcpy (decode input) | ~53 | 1.5 × 1400B |
| bzero (parity init) | ~18 | 0.5 × 1400B |
| **Total overhead** | **~777** | on top of no-fec cost |

No-fec per-packet cost: ~581 ns (cook + de_cook).
FEC 20:10 per-packet cost: ~1358 ns (581 + 777).
**Minimum FEC throughput ratio: 581 / 1358 = 43% of no-fec** (compute-bound).

But wire amplification caps at 67% of no-fec, which is less restrictive.
At low throughput (CPU-bound), the compute floor dominates. At high
throughput (bandwidth-bound), the wire floor dominates.

CI measured ~70% of no-fec (30% overhead), better than the compute floor
predicts. This is because the throughput test is bandwidth-limited on
loopback before hitting CPU saturation — the wire amplification floor
(67%) is the binding constraint, and measured overhead (30%) is close to
the theoretical 33%.

### Decode worst case

When r data shards are lost, decode reconstructs each via k addmul1 calls
(`lib/fec.cpp:1060-1065`). Per batch: r × k addmul1 = same as encode.
Per app packet: r addmul1(shard_len) — identical to encode cost.

Worst-case round-trip (all r lost): encode + decode = 2r addmul1 per
app packet = 760 ns/pkt on x86 AVX2 for fec 20:10.

### Implication

The current ~30% FEC overhead on CI loopback is within 10% of the
information-theoretic floor (33% wire amplification). No further software
optimization can meaningfully close this gap. On real networks with actual
packet loss, the wire amplification is the cost of redundancy by design.

## Not done (deliberately)

**Scatter-gather RS encoder to eliminate blob_encode memcpy**: The
`blob_encode_t::input()` memcpy (~1400B per packet) packs variable-length
application packets into a contiguous buffer with interleaved 2-byte length
headers, then slices the result into k equal-length shards for RS encode.
This copy exists because shard boundaries depend on total batch size, which
isn't known until the last packet arrives. Eliminating it would require the
RS encoder to accept a scatter-gather (iovec-style) input instead of flat
`char *data[k]` pointers. That means rewriting `fec_encode`'s inner loop
(`lib/fec.cpp:940-945`) and `addmul1` to iterate over discontiguous chunks,
adding branch overhead per chunk boundary inside the tightest SIMD loop in
the system. The alternative — pre-positioning packets into a shard grid as
they arrive — fails because the grid layout depends on the final batch size.
Net effect: replaces a 1400B L1-resident memcpy (~35 ns) with scatter-gather
bookkeeping of comparable cost, while adding complexity to the FEC core.

**Zero-copy RS decode to eliminate fec_data memcpy**: The
`fec_decode_manager_t::input()` memcpy (~1400B per shard) copies received
shards into owned `fec_data[].buf` buffers because `fec_decode` modifies
data in-place — it overwrites redundancy shard buffers with recovered data
(`lib/fec.cpp:1061-1067`), then copies results back (`lib/fec.cpp:1072-1075`).
Pointing RS decode directly at io_uring provided buffers would corrupt the
kernel buffer ring (buffers must be recycled promptly or ring starvation
occurs). For the recvfrom path, the receive buffer is stack-local and reused
per callback. Making `fec_decode` write to separate output buffers instead
of in-place would eliminate the input copy but add an identical output copy
(the recovered data must still go somewhere). Net: zero gain, additional
complexity in the 1997-era Vandermonde matrix math.

**Auto-vectorization of scalar GF(2^8) fallback**: The scalar `addmul1`
uses a 64KB lookup table indexed by runtime byte values. No compiler can
auto-vectorize arbitrary table lookups — the SSSE3/NEON `PSHUFB`/`TBL`
approach requires algebraic insight (nibble decomposition of GF multiplication)
that is beyond compiler analysis. Documented in `lib/fec.cpp`.

**-O3**: Tested, no measurable improvement. All hot paths are hand-written
SIMD intrinsics or hardware CRC32C — the compiler's extra optimization
passes have nothing to improve.

**Alignment audit**: Unaligned SIMD loads/stores used throughout (correct
for arbitrary buffer pointers). On both target architectures, unaligned
accesses that don't cross cache line boundaries are free. Estimated
impact of forced alignment: <1%.

## Cross-architecture notes

**x86_64** (N150, CI runners): Full SIMD coverage. AVX2 addmul1, SSE4.2
CRC32C, SSE2/AVX2 XOR cook. io_uring multishot recv, sendmmsg batching.
All optimizations apply.

**ARMv8/AArch64** (Mediatek Filogic): NEON addmul1 (TBL), ARMv8-CRC
CRC32C, NEON XOR cook. All three compute paths are vectorized. io_uring
available if kernel 6.0+. Cross-compiled and QEMU-tested in CI; untested
on real Filogic hardware.

**PowerPC e500v2** (TL-WDR4900): SPE XOR only. addmul1 is scalar
(SPE has no byte-level shuffle/permute equivalent to PSHUFB/TBL).
CRC32C is software slicing-by-8 (no hardware CRC). No io_uring
(older kernel). Expected real-hardware throughput: 50-150 Mbps no-fec,
15-40 Mbps fec-20:10, limited by scalar addmul1.

**MIPS 24Kc** (AR71xx OpenWrt targets): No useful SIMD. MIPS SIMD
Architecture (MSA) is only on MIPS32r5+ (P5600, I6400), not 24Kc.
All paths would be scalar. Build targets exist in makefile but are
untested with current optimization work.

**RISC-V RV64GCV**: Hypothetical future target. The V extension has
`vrgather` which can implement GF(2^8) nibble-decomposition lookup
(equivalent to PSHUFB/TBL), potentially vectorizing addmul1. This
is the only other ISA besides x86/ARM that could accelerate FEC.
