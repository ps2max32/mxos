# VU0's registers of a process (step 2e-2, kproc.c): VF1-VF31, and VI1-VI15,
# the status and clip flags, R, I and Q. Not the accumulator (no instruction
# reads it) nor the MAC flag (read only). Kernel mode, interrupts off: CU2 on
# while here. The first move waits for a program VU0 runs (qmfc2.i).
#
#   struct kvu0 { u32 vf[32][4]; u32 vi[24]; }   16-byte aligned
#   void kvu0_save(struct kvu0 *to), void kvu0_load(const struct kvu0 *from)

	.set noreorder
	.set noat
	.text

#define VI_AT(n) (512 + 4 * (n))

	.macro cu2_on
	mfc0	$t0, $12
	lui	$t1, 0x4000
	or	$t1, $t0, $t1
	mtc0	$t1, $12
	sync.p
	.endm

	.macro cu2_back
	mtc0	$t0, $12
	sync.p
	.endm

	.globl	kvu0_save
	.ent	kvu0_save
kvu0_save:
	cu2_on
	qmfc2.i	$t2, $vf1
	sq	$t2, 16($a0)
	.irp n, 2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
	qmfc2	$t2, $vf\n
	sq	$t2, \n*16($a0)
	.endr
	.irp n, 1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,18,20,21,22
	cfc2	$t2, $vi\n
	sw	$t2, VI_AT(\n)($a0)
	.endr
	cu2_back
	jr	$ra
	nop
	.end	kvu0_save

	.globl	kvu0_load
	.ent	kvu0_load
kvu0_load:
	cu2_on
	lq	$t2, 16($a0)
	qmtc2.i	$t2, $vf1
	.irp n, 2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
	lq	$t2, \n*16($a0)
	qmtc2	$t2, $vf\n
	.endr
	.irp n, 1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,18,20,21,22
	lw	$t2, VI_AT(\n)($a0)
	ctc2	$t2, $vi\n
	.endr
	cu2_back
	jr	$ra
	nop
	.end	kvu0_load
