#ifndef BENCH_CRC32C_H
#define BENCH_CRC32C_H

#include <stdint.h>
#include <stddef.h>

/*
 * CRC32C (Castagnoli) implementation — polynomial 0x82F63B78
 *
 * Three paths:
 *   crc32c_sw()  — software slicing-by-8, works everywhere
 *   crc32c_hw()  — hardware intrinsics (SSE4.2 / ARMv8-CRC)
 *   crc32c()     — runtime dispatch to hw or sw
 */

/* ---- Software slicing-by-8 -------------------------------------------- */

/*
 * Lookup table for CRC32C polynomial 0x82F63B78 (bit-reversed 0x1EDC6F41).
 * 8 slices x 256 entries. Generated at init time by crc32c_init_sw_table().
 */
static uint32_t crc32c_table[8][256];
static int crc32c_table_ready = 0;

static void crc32c_init_sw_table(void) {
    if (crc32c_table_ready) return;
    const uint32_t poly = 0x82F63B78;
    for (int i = 0; i < 256; i++) {
        uint32_t crc = (uint32_t)i;
        for (int j = 0; j < 8; j++)
            crc = (crc >> 1) ^ (poly & (-(int32_t)(crc & 1)));
        crc32c_table[0][i] = crc;
    }
    for (int i = 0; i < 256; i++) {
        uint32_t crc = crc32c_table[0][i];
        for (int s = 1; s < 8; s++) {
            crc = crc32c_table[0][crc & 0xFF] ^ (crc >> 8);
            crc32c_table[s][i] = crc;
        }
    }
    crc32c_table_ready = 1;
}

static uint32_t crc32c_sw(const void *data, size_t length, uint32_t previousCrc32 = 0) {
    crc32c_init_sw_table();
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = ~previousCrc32;

    /* Process 8 bytes at a time */
    while (length >= 8) {
        crc ^= (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
               ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
        crc = crc32c_table[7][crc & 0xFF] ^
              crc32c_table[6][(crc >> 8) & 0xFF] ^
              crc32c_table[5][(crc >> 16) & 0xFF] ^
              crc32c_table[4][(crc >> 24) & 0xFF] ^
              crc32c_table[3][p[4]] ^
              crc32c_table[2][p[5]] ^
              crc32c_table[1][p[6]] ^
              crc32c_table[0][p[7]];
        p += 8;
        length -= 8;
    }

    /* Remaining bytes */
    while (length--)
        crc = crc32c_table[0][(crc ^ *p++) & 0xFF] ^ (crc >> 8);

    return ~crc;
}

/* ---- Hardware: x86_64 SSE4.2 ----------------------------------------- */

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <nmmintrin.h>

#ifdef __GNUC__
__attribute__((target("sse4.2")))
#endif
static uint32_t crc32c_hw(const void *data, size_t length, uint32_t previousCrc32 = 0) {
    const uint8_t *p = (const uint8_t *)data;
    uint64_t crc = ~(uint64_t)previousCrc32;

#if defined(__x86_64__) || defined(_M_X64)
    /* Process 8 bytes at a time on 64-bit */
    while (length >= 8) {
        uint64_t val;
        __builtin_memcpy(&val, p, 8);
        crc = _mm_crc32_u64(crc, val);
        p += 8;
        length -= 8;
    }
#endif
    /* Process 4 bytes at a time */
    while (length >= 4) {
        uint32_t val;
        __builtin_memcpy(&val, p, 4);
        crc = _mm_crc32_u32((uint32_t)crc, val);
        p += 4;
        length -= 4;
    }
    /* Remaining bytes */
    while (length--)
        crc = _mm_crc32_u8((uint32_t)crc, *p++);

    return ~(uint32_t)crc;
}

static int crc32c_has_hw(void) {
    uint32_t eax, ebx, ecx, edx;
    __asm__ __volatile__("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                         : "a"(1));
    return (ecx >> 20) & 1; /* SSE4.2 bit */
}

/* ---- Hardware: ARMv8-A CRC extension ---------------------------------- */

#elif defined(__aarch64__) || defined(__arm__)
#ifdef __ARM_FEATURE_CRC32
#include <arm_acle.h>

static uint32_t crc32c_hw(const void *data, size_t length, uint32_t previousCrc32 = 0) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = ~previousCrc32;

#ifdef __aarch64__
    while (length >= 8) {
        uint64_t val;
        __builtin_memcpy(&val, p, 8);
        crc = __crc32cd(crc, val);
        p += 8;
        length -= 8;
    }
#endif
    while (length >= 4) {
        uint32_t val;
        __builtin_memcpy(&val, p, 4);
        crc = __crc32cw(crc, val);
        p += 4;
        length -= 4;
    }
    while (length--)
        crc = __crc32cb(crc, *p++);

    return ~crc;
}

static int crc32c_has_hw(void) {
    return 1; /* If __ARM_FEATURE_CRC32 is defined, the compiler guarantees it */
}

#else /* ARM without CRC extension */

static uint32_t crc32c_hw(const void *data, size_t length, uint32_t previousCrc32 = 0) {
    return crc32c_sw(data, length, previousCrc32); /* fallback */
}

static int crc32c_has_hw(void) {
    return 0;
}

#endif /* __ARM_FEATURE_CRC32 */

/* ---- No hardware support ---------------------------------------------- */

#else

static uint32_t crc32c_hw(const void *data, size_t length, uint32_t previousCrc32 = 0) {
    return crc32c_sw(data, length, previousCrc32);
}

static int crc32c_has_hw(void) {
    return 0;
}

#endif /* architecture selection */

/* ---- Runtime dispatch ------------------------------------------------- */

typedef uint32_t (*crc32c_fn)(const void *, size_t, uint32_t);

static uint32_t crc32c_resolve(const void *data, size_t length, uint32_t previousCrc32);
static crc32c_fn crc32c_impl = crc32c_resolve;

static uint32_t crc32c_resolve(const void *data, size_t length, uint32_t previousCrc32) {
    crc32c_impl = crc32c_has_hw() ? crc32c_hw : crc32c_sw;
    return crc32c_impl(data, length, previousCrc32);
}

static inline uint32_t crc32c(const void *data, size_t length, uint32_t previousCrc32 = 0) {
    return crc32c_impl(data, length, previousCrc32);
}

#endif /* BENCH_CRC32C_H */
