/* The kernel first at every INTC and DMAC interrupt (mx OS plan, step 2d-1;
   docs/bios.md). The BIOS's interrupt vector (0x8000_0200) saved sp, ra and
   at at 0x8000_10C0 and jumps through its table (0x8001_5380: INTC entry 2,
   DMAC entry 3), which kirq.c points here, through kseg0. At EXL = 1 with
   interrupts off: the registers C may change are saved, kirq() runs on a
   stack of its own, everything is put back, and the BIOS's handler follows
   with k0 its address, as the vector would have it. */

	.set noreorder
	.set noat
	.text

	.globl kirq_intc
	.type kirq_intc, @function
	.ent kirq_intc
kirq_intc:
	b kirq_common
	addiu $26, $0, 0	/* the line: 0 INTC */
	.end kirq_intc

	.globl kirq_dmac
	.type kirq_dmac, @function
	.ent kirq_dmac
kirq_dmac:
	addiu $26, $0, 1	/* 1 DMAC */
	.end kirq_dmac

kirq_common:
	lui $27, %hi(kirq_regs)
	addiu $27, $27, %lo(kirq_regs)
	sw $26, 0x1F0($27)	/* the line, for the way out */
	sq $1, 0x000($27)
	sq $2, 0x010($27)
	sq $3, 0x020($27)
	sq $4, 0x030($27)
	sq $5, 0x040($27)
	sq $6, 0x050($27)
	sq $7, 0x060($27)
	sq $8, 0x070($27)
	sq $9, 0x080($27)
	sq $10, 0x090($27)
	sq $11, 0x0A0($27)
	sq $12, 0x0B0($27)
	sq $13, 0x0C0($27)
	sq $14, 0x0D0($27)
	sq $15, 0x0E0($27)
	sq $24, 0x0F0($27)
	sq $25, 0x100($27)
	sq $28, 0x110($27)
	mfhi $2
	mflo $3
	sd $2, 0x120($27)
	sd $3, 0x128($27)
	mfhi1 $2
	mflo1 $3
	sd $2, 0x130($27)
	sd $3, 0x138($27)
	mfsa $2
	sw $2, 0x140($27)

	lui $29, %hi(kirq_stack + 4096)
	addiu $29, $29, %lo(kirq_stack + 4096)
	jal kirq
	move $4, $26

	lui $27, %hi(kirq_regs)
	addiu $27, $27, %lo(kirq_regs)
	lw $2, 0x140($27)
	mtsa $2
	ld $2, 0x130($27)
	ld $3, 0x138($27)
	mthi1 $2
	mtlo1 $3
	ld $2, 0x120($27)
	ld $3, 0x128($27)
	mthi $2
	mtlo $3
	lq $1, 0x000($27)
	lq $2, 0x010($27)
	lq $3, 0x020($27)
	lq $4, 0x030($27)
	lq $5, 0x040($27)
	lq $6, 0x050($27)
	lq $7, 0x060($27)
	lq $8, 0x070($27)
	lq $9, 0x080($27)
	lq $10, 0x090($27)
	lq $11, 0x0A0($27)
	lq $12, 0x0B0($27)
	lq $13, 0x0C0($27)
	lq $14, 0x0D0($27)
	lq $15, 0x0E0($27)
	lq $24, 0x0F0($27)
	lq $25, 0x100($27)
	lq $28, 0x110($27)
	/* sp and ra as the vector saved them */
	lui $26, 0x8000
	lq $29, 0x10C0($26)
	lq $31, 0x10D0($26)
	/* on to the BIOS's handler of the line, k0 its address */
	lw $26, 0x1F0($27)
	sll $26, $26, 2
	lui $27, %hi(kirq_orig)
	addiu $27, $27, %lo(kirq_orig)
	addu $27, $27, $26
	lw $26, 0($27)
	jr $26
	nop
