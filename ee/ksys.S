/* The BIOS syscalls a program still makes, counted (mx OS plan step 2h-3):
   the BIOS's level 1 vector (0x8000_0180) jumps through its exception table
   (0x8001_5340, entry 8 the syscall dispatcher at 0x8000_0280, docs/bios.md)
   with only k0 and k1 changed; kirq.c points entry 8 here, through kseg0.
   A syscall of the watched program (EntryHi's ASID that of the program the
   kernel started last, ksys_watch) from its own code (EPC from 1 MiB up, not
   in kseg0 or kseg1: not the kernel's or the BIOS's) counts in
   ksys_counts[v1 & 0xFF] (an i-call's negative number at 0x81-0xFF). Then on
   into the BIOS's dispatcher (ksys_orig), which needs nothing of k0, k1. */

	.set noreorder
	.set noat
	.text

	.globl ksys_entry
	.type ksys_entry, @function
	.ent ksys_entry
ksys_entry:
	mfc0 $26, $10			/* EntryHi: the ASID */
	lui $27, %hi(ksys_watch + 0x80000000)
	lw $27, %lo(ksys_watch + 0x80000000)($27)
	andi $26, $26, 0xff
	bne $26, $27, 9f
	nop
	mfc0 $26, $14			/* EPC */
	srl $27, $26, 20
	beqz $27, 9f			/* below 1 MiB: the kernel's */
	srl $27, $26, 31
	bnez $27, 9f			/* kseg0, kseg1: the BIOS's */
	andi $26, $3, 0xff
	sll $26, $26, 2
	lui $27, %hi(ksys_counts + 0x80000000)
	addiu $27, $27, %lo(ksys_counts + 0x80000000)
	addu $26, $26, $27
	lw $27, 0($26)
	addiu $27, $27, 1
	sw $27, 0($26)
9:	lui $26, %hi(ksys_orig + 0x80000000)
	lw $26, %lo(ksys_orig + 0x80000000)($26)
	jr $26
	nop
	.end ksys_entry
	.size ksys_entry, .-ksys_entry
