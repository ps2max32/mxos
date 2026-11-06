/* The BIOS's Exit (syscall 4) replaced, for programs that do not call
   <mx/loader.h>'s ret themselves (ps2sdk's, older maxsdk ones): the handler
   runs in kernel mode, so it only changes where the syscall returns to, the
   EPC the kernel saved in its frame at 8($sp) (as Open PS2 Loader's
   Hook_Exit). The kernel then returns to mxlink_return in user mode, in the
   program's thread, a0 still the exit status. */

	.set noreorder
	.text
	.globl mxlink_exit_hook
	.type mxlink_exit_hook, @function
	.ent mxlink_exit_hook
mxlink_exit_hook:
	la $3, mxlink_return
	jr $31
	sw $3, 8($29)
	.end mxlink_exit_hook
	.size mxlink_exit_hook, .-mxlink_exit_hook

/* Where mxlink starts, every time. The first start saves .data; the others
   restore it. ExecPS2 does not load the ELF again: a variable's start value
   would be what the last run left (a semaphore's id, say, which the BIOS gives
   out again). What must survive is in .kept. a0-a3 are the start's. */

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
