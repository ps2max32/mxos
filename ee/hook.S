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
