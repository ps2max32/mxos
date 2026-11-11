/* Where mxlink starts, every time. The first start saves .data; the others
   restore it. ExecPS2 does not load the ELF again: a variable's start value
   would be what the last run left (a semaphore's id, say, which the BIOS gives
   out again). What must survive is in .kept. a0-a3 are the start's. */

	/* the branches' delay slots are filled by hand */
	.set noreorder

	.section .kept, "aw"
	.align 2
mxlink_saved:
	.word 0

	.text
	.globl mxlink_start
	.type mxlink_start, @function
	.ent mxlink_start
mxlink_start:
	la $8, __data_start
	la $9, __data_end
	la $10, __data_copy
	la $11, mxlink_saved
	lw $12, 0($11)
	bnez $12, 2f
	addiu $12, $0, 1
	sw $12, 0($11)
1:	lw $12, 0($8)
	addiu $8, $8, 4
	sw $12, 0($10)
	bne $8, $9, 1b
	addiu $10, $10, 4
	j __start
	nop
2:	lw $12, 0($10)
	addiu $10, $10, 4
	sw $12, 0($8)
	addiu $8, $8, 4
	bne $8, $9, 2b
	nop
	j __start
	nop
	.end mxlink_start
	.size mxlink_start, .-mxlink_start
