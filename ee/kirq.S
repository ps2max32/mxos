/* The kernel first at every INTC and DMAC interrupt (mx OS plan, step 2d;
   docs/bios.md). The BIOS's interrupt vector (0x8000_0200) saved sp, ra and
   at at 0x8000_10C0 and jumps through its table (0x8001_5380: INTC entry 2,
   DMAC entry 3), which kirq.c points here, through kseg0.

   The registers C may change are saved, and the interrupted thread's sp, ra,
   at, EPC and Status (a syscall of a handler, an i-call, overwrites the
   vector's area). Then, as the BIOS's handler does, EXL, IE and KSU are
   cleared (kernel mode, interrupts off: a handler's i-calls and TLB refills
   work) and kirq() runs on a stack of its own: the kernel's handlers of the
   pending causes (ksched.c). EPC and Status back, then
   - causes that are not the kernel's left: the BIOS's handler, k0 its
     address, the vector's area as it was;
   - a thread of higher priority made ready (ksched_need) and the
     interrupted one in user code: ksched_switch_irq (ksched.S);
   - else back by ERET. */

#include "ksched.h"

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
	sw $26, KR_LINE($27)
	sq $1, KR_R1($27)
	sq $2, KR_R2($27)
	sq $3, KR_R3($27)
	sq $4, KR_R4($27)
	sq $5, KR_R5($27)
	sq $6, KR_R6($27)
	sq $7, KR_R7($27)
	sq $8, KR_R8($27)
	sq $9, KR_R9($27)
	sq $10, KR_R10($27)
	sq $11, KR_R11($27)
	sq $12, KR_R12($27)
	sq $13, KR_R13($27)
	sq $14, KR_R14($27)
	sq $15, KR_R15($27)
	sq $24, KR_R24($27)
	sq $25, KR_R25($27)
	sq $28, KR_R28($27)
	mfhi $2
	sd $2, KR_HI($27)
	mflo $2
	sd $2, KR_LO($27)
	mfhi1 $2
	sd $2, KR_HI1($27)
	mflo1 $2
	sd $2, KR_LO1($27)
	mfsa $2
	sw $2, KR_SA($27)
	lui $3, 0x8000
	lq $2, 0x10C0($3)
	sq $2, KR_SP($27)
	lq $2, 0x10D0($3)
	sq $2, KR_RA($27)
	lq $2, 0x10E0($3)
	sq $2, KR_AT($27)
	mfc0 $5, $14
	sw $5, KR_EPC($27)
	mfc0 $2, $12
	sw $2, KR_STATUS($27)
	/* kernel mode, EXL and IE off */
	addiu $3, $0, -28
	and $2, $2, $3
	mtc0 $2, $12
	sync.p

	lui $29, %hi(kirq_stack + 4096)
	addiu $29, $29, %lo(kirq_stack + 4096)
	jal kirq		/* kirq(line, epc): what is left for the BIOS */
	move $4, $26

	lui $27, %hi(kirq_regs)
	addiu $27, $27, %lo(kirq_regs)
	lw $3, KR_STATUS($27)
	mtc0 $3, $12
	lw $3, KR_EPC($27)
	mtc0 $3, $14
	sync.p
	bnez $2, 8f
	nop
	/* a switch: wanted, the kernel's threads on, the thread in user code */
	lui $2, %hi(ksched_need)
	lw $2, %lo(ksched_need)($2)
	beqz $2, 7f
	nop
	bltz $3, 7f
	nop
	jal ksched_active
	nop
	beqz $2, 7f
	nop
	j ksched_switch_irq
	nop

	/* back where it was */
7:	jal kirq_restore
	nop
	lq $29, KR_SP($27)
	lq $31, KR_RA($27)
	lq $1, KR_AT($27)
	sync.l
	eret
	nop

	/* on into the BIOS's handler, the vector's area as it was */
8:	jal kirq_restore
	nop
	lui $26, 0x8000
	lq $29, KR_SP($27)
	sq $29, 0x10C0($26)
	lq $31, KR_RA($27)
	sq $31, 0x10D0($26)
	lq $1, KR_AT($27)
	sq $1, 0x10E0($26)
	lw $26, KR_LINE($27)
	sll $26, $26, 2
	lui $1, %hi(kirq_orig)
	addiu $1, $1, %lo(kirq_orig)
	addu $1, $1, $26
	lw $26, 0($1)
	lq $1, KR_AT($27)
	jr $26
	nop

/* the registers kirq_common saved, but sp, ra and at; $27 = kirq_regs */
kirq_restore:
	lw $2, KR_SA($27)
	mtsa $2
	ld $2, KR_HI1($27)
	mthi1 $2
	ld $2, KR_LO1($27)
	mtlo1 $2
	ld $2, KR_HI($27)
	mthi $2
	ld $2, KR_LO($27)
	mtlo $2
	lq $2, KR_R2($27)
	lq $3, KR_R3($27)
	lq $4, KR_R4($27)
	lq $5, KR_R5($27)
	lq $6, KR_R6($27)
	lq $7, KR_R7($27)
	lq $8, KR_R8($27)
	lq $9, KR_R9($27)
	lq $10, KR_R10($27)
	lq $11, KR_R11($27)
	lq $12, KR_R12($27)
	lq $13, KR_R13($27)
	lq $14, KR_R14($27)
	lq $15, KR_R15($27)
	lq $24, KR_R24($27)
	lq $25, KR_R25($27)
	jr $31
	lq $28, KR_R28($27)
