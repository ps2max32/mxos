/* The kernel's own level 1 vectors (mx OS plan step 2h-4d-1): kirq.c copies
   kvec_exc to the TLB refill vector (0x8000_0000) and the common one
   (0x8000_0180), kvec_irq to the interrupt vector (0x8000_0200), over the
   BIOS's, and puts the BIOS's back in kirq_off. They do what the BIOS's do
   (docs/bios.md), with the kernel's tables (kirq.c: kvec_exc_table by
   ExcCode, kvec_irq_table by the highest pending interrupt line, filled
   from the BIOS's at kirq_on), so that the handlers, the kernel's and the
   BIOS's left in them, see the same:
   - kvec_exc: only k0 and k1 changed (t9 kept in kvec_t9 meanwhile, not in
     the BIOS's table);
   - kvec_irq: the interrupted sp, ra and at in the BIOS's save area
     (0x8000_10C0), at the line's handler with k0 its address.
   Everything through kseg0: they run at EXL = 1, before any TLB refill can.
   No PC-relative jumps out: the code runs where it is copied to. */

	.set noreorder
	.set noat
	.text

	/* each in whole cache lines (64 bytes), the rest nops: kirq.c copies
	   the lines */
	.balign 64
	.globl kvec_exc
	.globl kvec_exc_end
kvec_exc:
	lui $26, %hi(kvec_t9 + 0x80000000)
	sd $25, %lo(kvec_t9 + 0x80000000)($26)
	mfc0 $25, $13			/* Cause */
	lui $26, %hi(kvec_exc_table + 0x80000000)
	andi $25, $25, 0x7c		/* ExcCode * 4 */
	addu $26, $26, $25
	lw $26, %lo(kvec_exc_table + 0x80000000)($26)
	lui $25, %hi(kvec_t9 + 0x80000000)
	jr $26
	ld $25, %lo(kvec_t9 + 0x80000000)($25)
	.balign 64, 0
kvec_exc_end:

	.globl kvec_irq
	.globl kvec_irq_end
kvec_irq:
	lui $26, 0x8000
	sq $29, 0x10c0($26)
	sq $31, 0x10d0($26)
	sq $1, 0x10e0($26)
	mfc0 $1, $13			/* Cause */
	mfc0 $26, $12			/* Status */
	and $1, $1, $26
	srl $1, $1, 8
	andi $1, $1, 0xff		/* IP & IM */
	plzcw $26, $1
	andi $26, $26, 0xff
	addiu $1, $0, 0x1e
	subu $1, $1, $26		/* the highest line pending */
	sll $1, $1, 2
	lui $26, %hi(kvec_irq_table + 0x80000000)
	addu $26, $26, $1
	lw $26, %lo(kvec_irq_table + 0x80000000)($26)
	jr $26
	nop
	.balign 64, 0
kvec_irq_end:
