/*
 * xor_spe.S — SPE-accelerated XOR for PowerPC e500v2
 *
 * Uses SPE (Signal Processing Extension) 64-bit operations:
 *   evldd/evstdd: 8-byte aligned load/store
 *   evxor: 64-bit XOR
 *
 * GCC 9+ removed SPE intrinsics, but gas still supports the opcodes.
 * Follows the Linux kernel pattern (arch/powerpc/crypto/aes-spe-core.S).
 *
 * Requires CONFIG_SPE=y in the kernel for context save/restore of the
 * upper 32 bits of GPRs.
 */

/* Mark stack as non-executable (suppresses ld warning) */
.section .note.GNU-stack,"",@progbits

#ifdef HAVE_PPC_SPE

.section .text
.globl xor_tile_spe
.type xor_tile_spe, @function

/*
 * void xor_tile_spe(char *data, int len, const char *tile, int tile_len)
 *
 * XOR data[0..len-1] with tile[0..tile_len-1] repeating.
 *
 * REQUIREMENTS (enforced by C caller in packet_cook.cpp):
 *   - data MUST be 8-byte aligned
 *   - tile MUST be 8-byte aligned
 *   - tile_len MUST be a multiple of 8 (COOK_VEC_WIDTH)
 *   - Tile offset always starts at 0 (caller pre-rotates if needed)
 *
 * PPC calling convention:
 *   r3 = data pointer (8-byte aligned)
 *   r4 = len
 *   r5 = tile pointer (8-byte aligned)
 *   r6 = tile_len (multiple of 8)
 *
 * Clobbers: r0, r7, r8, r9, r10, r11, r12, ctr
 * Uses SPE: upper halves of r7, r8, r9, r10, r11, r12
 */
.align 4
xor_tile_spe:
	/* Return immediately if len <= 0 */
	cmpwi	%r4, 0
	blelr

	/* r0 = tile offset (always starts at 0) */
	li	%r0, 0

	/* Process 32 bytes per iteration (4x evldd). */
	srwi.	%r7, %r4, 5	/* r7 = len / 32 = iteration count */
	beq	.Ltail8		/* < 32 bytes remaining */
	mtctr	%r7

.Lmain_loop:
	/* Compute tile pointer: r8 = tile + offset */
	add	%r8, %r5, %r0

	/* Load 4 doublewords from data */
	evldd	%r7, 0(%r3)
	evldd	%r9, 8(%r3)
	evldd	%r11, 16(%r3)

	/* Load and XOR first tile doubleword */
	evldd	%r10, 0(%r8)
	evxor	%r7, %r7, %r10

	/* Advance tile offset, check wrap for each doubleword */
	addic	%r0, %r0, 8
	cmpw	%r0, %r6
	blt	.Lnw1
	li	%r0, 0
.Lnw1:
	add	%r8, %r5, %r0
	evldd	%r10, 0(%r8)
	evxor	%r9, %r9, %r10

	addic	%r0, %r0, 8
	cmpw	%r0, %r6
	blt	.Lnw2
	li	%r0, 0
.Lnw2:
	add	%r8, %r5, %r0
	evldd	%r10, 0(%r8)
	evxor	%r11, %r11, %r10

	addic	%r0, %r0, 8
	cmpw	%r0, %r6
	blt	.Lnw3
	li	%r0, 0
.Lnw3:
	add	%r8, %r5, %r0
	evldd	%r12, 24(%r3)
	evldd	%r10, 0(%r8)
	evxor	%r12, %r12, %r10

	addic	%r0, %r0, 8
	cmpw	%r0, %r6
	blt	.Lnw4
	li	%r0, 0
.Lnw4:

	/* Store 4 XORed doublewords */
	evstdd	%r7, 0(%r3)
	evstdd	%r9, 8(%r3)
	evstdd	%r11, 16(%r3)
	evstdd	%r12, 24(%r3)

	addi	%r3, %r3, 32
	bdnz	.Lmain_loop

.Ltail8:
	/* Process remaining 8-byte chunks */
	andi.	%r7, %r4, 24	/* r7 = (len % 32) & ~7 = remaining 8-byte blocks * 8 */
	beq	.Ltail1
	srwi	%r7, %r7, 3
	mtctr	%r7

.Ltail8_loop:
	add	%r8, %r5, %r0
	evldd	%r7, 0(%r3)
	evldd	%r10, 0(%r8)
	evxor	%r7, %r7, %r10
	evstdd	%r7, 0(%r3)
	addi	%r3, %r3, 8
	addic	%r0, %r0, 8
	cmpw	%r0, %r6
	blt	.Ltail8_nowrap
	li	%r0, 0
.Ltail8_nowrap:
	bdnz	.Ltail8_loop

.Ltail1:
	/* Process remaining bytes (0-7) */
	andi.	%r7, %r4, 7
	beqlr
	mtctr	%r7

.Ltail1_loop:
	lbz	%r8, 0(%r3)
	lbzx	%r9, %r5, %r0
	xor	%r8, %r8, %r9
	stb	%r8, 0(%r3)
	addi	%r3, %r3, 1
	addic	%r0, %r0, 1
	cmpw	%r0, %r6
	blt	.Ltail1_nowrap
	li	%r0, 0
.Ltail1_nowrap:
	bdnz	.Ltail1_loop

	blr

.size xor_tile_spe, .-xor_tile_spe

#endif /* HAVE_PPC_SPE */
