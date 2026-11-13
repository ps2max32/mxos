/* The kernel's syscall entry (mx OS plan steps 2h-3 and 2h-4): the common
   level 1 vector (0x8000_0180: the kernel's, kvec.S, 2h-4d-1) jumps through
   the exception table (the kernel's, kvec_exc_table; the BIOS's at
   0x8001_5340 has its syscall dispatcher at 0x8000_0280 in entry 8,
   docs/bios.md) with only k0 and k1 changed; kirq.c points entry 8 here,
   through kseg0.

   First the count (2h-3): a syscall of the watched program (EntryHi's ASID
   that of the program the kernel started last, ksys_watch) from its own code
   (EPC from 1 MiB up, not in kseg0 or kseg1: not the kernel's or the BIOS's)
   counts in ksys_counts[v1 & 0xFF] (an i-call's negative number at
   0x81-0xFF); one from the kernel's own code (below 1 MiB) that goes on
   into the BIOS in ksys_kcounts, since its boot (what 2h-4 has left).

   Then the kernel's own numbers (2h-4a; ksys_handlers[|v1| & 0x7F], set by
   ksys_set: the scheduler's switch, the poke, the VM's resume) run here, as
   the BIOS's dispatcher runs a SetSyscall handler, so that the handlers are
   the same:
   - a positive number: the caller's sp, ra and at in the kernel's save area
     (kvec_save; the BIOS's dispatcher uses 0x8000_10C0), Status's ERL and
     KSU cleared (EXL stays), on the kernel's stack (ksys_stack; the BIOS's
     is at 0x8001_8E70) a frame as the BIOS's: ra, the caller's sp, EPC + 4;
     EPC + 4; back with Status | 0x13 (user mode, EXL, IE) by ERET (2h-4d-2);
   - a negative number (an i-call): v1 made positive, a 16-byte frame on the
     caller's stack (ra, EPC + 4), EPC + 4; back by ERET, Status as it is.
   Any other number goes on into the BIOS's dispatcher (ksys_orig), which
   needs nothing of k0, k1. */

#include "ksched.h"

	.set noreorder
	.set noat
	.text

	.globl ksys_entry
	.type ksys_entry, @function
	.ent ksys_entry
ksys_entry:
	mfc0 $26, $14			/* EPC */
	srl $27, $26, 31
	bnez $27, 3f			/* kseg0, kseg1: the BIOS's own */
	srl $27, $26, 20
	beqz $27, 3f			/* below 1 MiB: the kernel's own (at 9) */
	nop
	mfc0 $26, $10			/* EntryHi: the ASID */
	lui $27, %hi(ksys_watch + 0x80000000)
	lw $27, %lo(ksys_watch + 0x80000000)($27)
	andi $26, $26, 0xff
	bne $26, $27, 3f
	nop
	lui $27, %hi(ksys_counts + 0x80000000)
	addiu $27, $27, %lo(ksys_counts + 0x80000000)
	andi $26, $3, 0xff
	sll $26, $26, 2
	addu $26, $26, $27
	lw $27, 0($26)
	addiu $27, $27, 1
	sw $27, 0($26)

	/* the kernel's own handler of this number? (by |v1|, as the BIOS's
	   table: an i-call's handler is its positive number's) */
3:	bgez $3, 4f
	andi $26, $3, 0x7f
	subu $26, $0, $3
	andi $26, $26, 0x7f
4:	sll $26, $26, 2
	lui $27, %hi(ksys_handlers + 0x80000000)
	addiu $27, $27, %lo(ksys_handlers + 0x80000000)
	addu $26, $26, $27
	lw $26, 0($26)
	beqz $26, 9f
	nop
	bltz $3, 5f
	nop

	/* a positive number, as the BIOS's dispatcher (0x8000_029C) */
	lui $27, %hi(kvec_save + 0x80000000)
	sq $29, %lo(kvec_save + 0x80000000)($27)
	sq $31, %lo(kvec_save + 0x80000010)($27)
	sq $1, %lo(kvec_save + 0x80000020)($27)
	mfc0 $1, $12
	addiu $27, $0, -0x1c
	and $1, $1, $27
	mtc0 $1, $12
	sync.p
	move $27, $29
	lui $29, %hi(ksys_stack + KSYS_STACK - 0x10)
	addiu $29, $29, %lo(ksys_stack + KSYS_STACK - 0x10)
	sw $31, 0($29)
	sw $27, 4($29)
	mfc0 $27, $14
	addiu $27, $27, 4
	sw $27, 8($29)
	mtc0 $27, $14
	sync.p
	jalr $26
	nop
	lw $26, 8($29)
	lw $31, 0($29)
	lw $29, 4($29)
	mtc0 $26, $14
	sync.p
	mfc0 $26, $12
	ori $26, $26, 0x13
	mtc0 $26, $12
	sync.l		# the handler's stores done: the instruction returned to may be a CACHE
	sync.p
	eret
	nop

	/* a negative number, as the BIOS's dispatcher (0x8000_032C) */
5:	negu $3, $3
	addiu $29, $29, -0x10
	sw $31, 0($29)
	mfc0 $27, $14
	addiu $27, $27, 4
	sw $27, 4($29)
	mtc0 $27, $14
	sync.p
	jalr $26
	nop
	lw $26, 4($29)
	lw $31, 0($29)
	addiu $29, $29, 0x10
	mtc0 $26, $14
	sync.l		# the handler's stores done: the instruction returned to may be a CACHE
	sync.p
	eret
	nop

	/* on into the BIOS: one of the kernel's own (EPC below 1 MiB) counts
	   in ksys_kcounts (what 2h-4 has left to replace) */
9:	mfc0 $26, $14
	srl $27, $26, 20
	bnez $27, 8f
	srl $27, $26, 31
	bnez $27, 8f
	andi $26, $3, 0xff
	sll $26, $26, 2
	lui $27, %hi(ksys_kcounts + 0x80000000)
	addiu $27, $27, %lo(ksys_kcounts + 0x80000000)
	addu $26, $26, $27
	lw $27, 0($26)
	addiu $27, $27, 1
	sw $27, 0($26)
8:	lui $26, %hi(ksys_orig + 0x80000000)
	lw $26, %lo(ksys_orig + 0x80000000)($26)
	jr $26
	nop
	.end ksys_entry
	.size ksys_entry, .-ksys_entry
