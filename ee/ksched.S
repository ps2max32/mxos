/* The kernel's thread switch (mx OS plan, step 2d; ksched.c). Both ways in
   end in kctx_resume, which loads a thread's whole context and ERETs into
   it: the switch syscall (KSCHED_SYSCALL, a thread that waits or gives way)
   and the kernel's interrupt entry (kirq.S, a thread of higher priority
   woken by a handler). The context: ksched.h. */

#include "ksched.h"

	.set noreorder
	.set noat
	.text

/* the FPU and its accumulator, into or out of the context at $26 (k0); k1
   is free. ACC: madd.s fd = ACC + 0 * 0 reads it, adda.s ACC = fs + 0
   sets it. */
	.macro save_fpu
	swc1 $f0, KC_F + 0x00($26)
	swc1 $f1, KC_F + 0x04($26)
	swc1 $f2, KC_F + 0x08($26)
	swc1 $f3, KC_F + 0x0C($26)
	swc1 $f4, KC_F + 0x10($26)
	swc1 $f5, KC_F + 0x14($26)
	swc1 $f6, KC_F + 0x18($26)
	swc1 $f7, KC_F + 0x1C($26)
	swc1 $f8, KC_F + 0x20($26)
	swc1 $f9, KC_F + 0x24($26)
	swc1 $f10, KC_F + 0x28($26)
	swc1 $f11, KC_F + 0x2C($26)
	swc1 $f12, KC_F + 0x30($26)
	swc1 $f13, KC_F + 0x34($26)
	swc1 $f14, KC_F + 0x38($26)
	swc1 $f15, KC_F + 0x3C($26)
	swc1 $f16, KC_F + 0x40($26)
	swc1 $f17, KC_F + 0x44($26)
	swc1 $f18, KC_F + 0x48($26)
	swc1 $f19, KC_F + 0x4C($26)
	swc1 $f20, KC_F + 0x50($26)
	swc1 $f21, KC_F + 0x54($26)
	swc1 $f22, KC_F + 0x58($26)
	swc1 $f23, KC_F + 0x5C($26)
	swc1 $f24, KC_F + 0x60($26)
	swc1 $f25, KC_F + 0x64($26)
	swc1 $f26, KC_F + 0x68($26)
	swc1 $f27, KC_F + 0x6C($26)
	swc1 $f28, KC_F + 0x70($26)
	swc1 $f29, KC_F + 0x74($26)
	swc1 $f30, KC_F + 0x78($26)
	swc1 $f31, KC_F + 0x7C($26)
	cfc1 $27, $31
	sw $27, KC_FCR31($26)
	mtc1 $0, $f1
	madd.s $f0, $f1, $f1
	swc1 $f0, KC_ACC($26)
	.endm

	.macro load_fpu
	lwc1 $f0, KC_ACC($26)
	mtc1 $0, $f1
	adda.s $f0, $f1
	lw $27, KC_FCR31($26)
	ctc1 $27, $31
	lwc1 $f0, KC_F + 0x00($26)
	lwc1 $f1, KC_F + 0x04($26)
	lwc1 $f2, KC_F + 0x08($26)
	lwc1 $f3, KC_F + 0x0C($26)
	lwc1 $f4, KC_F + 0x10($26)
	lwc1 $f5, KC_F + 0x14($26)
	lwc1 $f6, KC_F + 0x18($26)
	lwc1 $f7, KC_F + 0x1C($26)
	lwc1 $f8, KC_F + 0x20($26)
	lwc1 $f9, KC_F + 0x24($26)
	lwc1 $f10, KC_F + 0x28($26)
	lwc1 $f11, KC_F + 0x2C($26)
	lwc1 $f12, KC_F + 0x30($26)
	lwc1 $f13, KC_F + 0x34($26)
	lwc1 $f14, KC_F + 0x38($26)
	lwc1 $f15, KC_F + 0x3C($26)
	lwc1 $f16, KC_F + 0x40($26)
	lwc1 $f17, KC_F + 0x44($26)
	lwc1 $f18, KC_F + 0x48($26)
	lwc1 $f19, KC_F + 0x4C($26)
	lwc1 $f20, KC_F + 0x50($26)
	lwc1 $f21, KC_F + 0x54($26)
	lwc1 $f22, KC_F + 0x58($26)
	lwc1 $f23, KC_F + 0x5C($26)
	lwc1 $f24, KC_F + 0x60($26)
	lwc1 $f25, KC_F + 0x64($26)
	lwc1 $f26, KC_F + 0x68($26)
	lwc1 $f27, KC_F + 0x6C($26)
	lwc1 $f28, KC_F + 0x70($26)
	lwc1 $f29, KC_F + 0x74($26)
	lwc1 $f30, KC_F + 0x78($26)
	lwc1 $f31, KC_F + 0x7C($26)
	.endm

/* HI/LO, HI1/LO1 and SA of the context at $26, through $2 (saved before) */
	.macro save_mul
	mfhi $2
	sd $2, KC_HI($26)
	mflo $2
	sd $2, KC_LO($26)
	mfhi1 $2
	sd $2, KC_HI1($26)
	mflo1 $2
	sd $2, KC_LO1($26)
	mfsa $2
	sw $2, KC_SA($26)
	.endm

/* ---- a thread's context in, ERET into it: a0 the context. Kernel mode. */
	.globl kctx_resume
	.type kctx_resume, @function
	.ent kctx_resume
kctx_resume:
	move $26, $4
	load_fpu
	lw $2, KC_SA($26)
	mtsa $2
	ld $2, KC_HI($26)
	mthi $2
	ld $2, KC_LO($26)
	mtlo $2
	ld $2, KC_HI1($26)
	mthi1 $2
	ld $2, KC_LO1($26)
	mtlo1 $2
	lw $2, KC_EPC($26)
	mtc0 $2, $14
	/* EXL (the ERET's), user mode, EIE and IE as the thread had them
	   (KC_IE: 0x10001 or 0; mx_di's di clears EIE) */
	mfc0 $2, $12
	lui $3, 1
	ori $3, $3, 1
	or $2, $2, $3
	xor $2, $2, $3
	ori $2, $2, 0x12
	lw $3, KC_IE($26)
	or $2, $2, $3
	mtc0 $2, $12
	sync.p
	lq $1, KC_R + 0x010($26)
	lq $2, KC_R + 0x020($26)
	lq $3, KC_R + 0x030($26)
	lq $4, KC_R + 0x040($26)
	lq $5, KC_R + 0x050($26)
	lq $6, KC_R + 0x060($26)
	lq $7, KC_R + 0x070($26)
	lq $8, KC_R + 0x080($26)
	lq $9, KC_R + 0x090($26)
	lq $10, KC_R + 0x0A0($26)
	lq $11, KC_R + 0x0B0($26)
	lq $12, KC_R + 0x0C0($26)
	lq $13, KC_R + 0x0D0($26)
	lq $14, KC_R + 0x0E0($26)
	lq $15, KC_R + 0x0F0($26)
	lq $16, KC_R + 0x100($26)
	lq $17, KC_R + 0x110($26)
	lq $18, KC_R + 0x120($26)
	lq $19, KC_R + 0x130($26)
	lq $20, KC_R + 0x140($26)
	lq $21, KC_R + 0x150($26)
	lq $22, KC_R + 0x160($26)
	lq $23, KC_R + 0x170($26)
	lq $24, KC_R + 0x180($26)
	lq $25, KC_R + 0x190($26)
	lq $28, KC_R + 0x1C0($26)
	lq $29, KC_R + 0x1D0($26)
	lq $30, KC_R + 0x1E0($26)
	lq $31, KC_R + 0x1F0($26)
	sync.l
	eret
	nop
	.end kctx_resume
	.size kctx_resume, .-kctx_resume

/* ---- the switch syscall (KSCHED_SYSCALL): the BIOS's dispatcher runs it in
   kernel mode on its stack, interrupts off; the caller's sp, ra and at are
   in the vector's save area (0x8000_10C0), the address after the syscall at
   8($sp), v1 was the number. The caller's context saved, ksched_pick()
   chooses, kctx_resume goes on with that one. */
	.globl ksched_switch_k
	.type ksched_switch_k, @function
	.ent ksched_switch_k
ksched_switch_k:
	lui $26, %hi(ksched_cur)
	lw $26, %lo(ksched_cur)($26)
	sq $2, KC_R + 0x020($26)
	sq $4, KC_R + 0x040($26)
	sq $5, KC_R + 0x050($26)
	sq $6, KC_R + 0x060($26)
	sq $7, KC_R + 0x070($26)
	sq $8, KC_R + 0x080($26)
	sq $9, KC_R + 0x090($26)
	sq $10, KC_R + 0x0A0($26)
	sq $11, KC_R + 0x0B0($26)
	sq $12, KC_R + 0x0C0($26)
	sq $13, KC_R + 0x0D0($26)
	sq $14, KC_R + 0x0E0($26)
	sq $15, KC_R + 0x0F0($26)
	sq $16, KC_R + 0x100($26)
	sq $17, KC_R + 0x110($26)
	sq $18, KC_R + 0x120($26)
	sq $19, KC_R + 0x130($26)
	sq $20, KC_R + 0x140($26)
	sq $21, KC_R + 0x150($26)
	sq $22, KC_R + 0x160($26)
	sq $23, KC_R + 0x170($26)
	sq $24, KC_R + 0x180($26)
	sq $25, KC_R + 0x190($26)
	sq $28, KC_R + 0x1C0($26)
	sq $30, KC_R + 0x1E0($26)
	lui $27, 0x8000
	lq $2, 0x10C0($27)
	sq $2, KC_R + 0x1D0($26)
	lq $2, 0x10D0($27)
	sq $2, KC_R + 0x1F0($26)
	lq $2, 0x10E0($27)
	sq $2, KC_R + 0x010($26)
	lw $2, 8($29)
	sw $2, KC_EPC($26)
	/* back with interrupts on (EIE and IE), as the BIOS returns from a
	   syscall */
	lui $2, 1
	ori $2, $2, 1
	sw $2, KC_IE($26)
	save_mul
	save_fpu
	jal ksched_pick
	nop
	j kctx_resume
	move $4, $2
	.end ksched_switch_k
	.size ksched_switch_k, .-ksched_switch_k

/* ---- the interrupt entry's switch (kirq.S): the interrupted thread's
   registers are in kirq_regs (the ones C may change; sp, ra, at, EPC as
   saved there), the rest live; Status back at EXL = 1. */
	.globl ksched_switch_irq
	.type ksched_switch_irq, @function
	.ent ksched_switch_irq
ksched_switch_irq:
	lui $26, %hi(ksched_cur)
	lw $26, %lo(ksched_cur)($26)
	lui $27, %hi(kirq_regs)
	addiu $27, $27, %lo(kirq_regs)
	/* the live ones first */
	sq $16, KC_R + 0x100($26)
	sq $17, KC_R + 0x110($26)
	sq $18, KC_R + 0x120($26)
	sq $19, KC_R + 0x130($26)
	sq $20, KC_R + 0x140($26)
	sq $21, KC_R + 0x150($26)
	sq $22, KC_R + 0x160($26)
	sq $23, KC_R + 0x170($26)
	sq $30, KC_R + 0x1E0($26)
	/* kirq_regs: KR_* offsets (ksched.h) */
	lq $2, KR_R2($27)
	sq $2, KC_R + 2 * 16($26)
	lq $2, KR_R3($27)
	sq $2, KC_R + 3 * 16($26)
	lq $2, KR_R4($27)
	sq $2, KC_R + 4 * 16($26)
	lq $2, KR_R5($27)
	sq $2, KC_R + 5 * 16($26)
	lq $2, KR_R6($27)
	sq $2, KC_R + 6 * 16($26)
	lq $2, KR_R7($27)
	sq $2, KC_R + 7 * 16($26)
	lq $2, KR_R8($27)
	sq $2, KC_R + 8 * 16($26)
	lq $2, KR_R9($27)
	sq $2, KC_R + 9 * 16($26)
	lq $2, KR_R10($27)
	sq $2, KC_R + 10 * 16($26)
	lq $2, KR_R11($27)
	sq $2, KC_R + 11 * 16($26)
	lq $2, KR_R12($27)
	sq $2, KC_R + 12 * 16($26)
	lq $2, KR_R13($27)
	sq $2, KC_R + 13 * 16($26)
	lq $2, KR_R14($27)
	sq $2, KC_R + 14 * 16($26)
	lq $2, KR_R15($27)
	sq $2, KC_R + 15 * 16($26)
	lq $2, KR_AT($27)	/* at as the vector saved it */
	sq $2, KC_R + 0x010($26)
	lq $2, KR_R24($27)
	sq $2, KC_R + 0x180($26)
	lq $2, KR_R25($27)
	sq $2, KC_R + 0x190($26)
	lq $2, KR_R28($27)
	sq $2, KC_R + 0x1C0($26)
	lq $2, KR_SP($27)
	sq $2, KC_R + 0x1D0($26)
	lq $2, KR_RA($27)
	sq $2, KC_R + 0x1F0($26)
	ld $2, KR_HI($27)
	sd $2, KC_HI($26)
	ld $2, KR_LO($27)
	sd $2, KC_LO($26)
	ld $2, KR_HI1($27)
	sd $2, KC_HI1($26)
	ld $2, KR_LO1($27)
	sd $2, KC_LO1($26)
	lw $2, KR_SA($27)
	sw $2, KC_SA($26)
	lw $2, KR_EPC($27)
	sw $2, KC_EPC($26)
	lui $2, 1		/* interrupted: interrupts were on */
	ori $2, $2, 1
	sw $2, KC_IE($26)
	save_fpu		/* last: it uses k1 */
	lui $29, %hi(kirq_stack + 4096)
	addiu $29, $29, %lo(kirq_stack + 4096)
	jal ksched_pick
	nop
	j kctx_resume
	move $4, $2
	.end ksched_switch_irq
	.size ksched_switch_irq, .-ksched_switch_irq
