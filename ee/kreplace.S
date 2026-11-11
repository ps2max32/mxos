# The kernel replaced by another (exec.c, kreplace): copied to KREPLACE_AT
# (free program memory, kept by ExecPS2) and started by ExecPS2 there, after
# the BIOS has set up its thread again. It copies the new kernel's segments
# from where the old one read them to their own addresses (over the old one),
# clears what is past each one's file size, writes the data cache back and
# drops the instruction cache, and jumps to the new kernel's entry with
# ExecPS2's a0 and a1 (argc, argv). Position independent, no stack; the
# table at KREPLACE_TABLE (kreplace.h): u32 n, u32 entry, then n times
# {dst, src, filesz, memsz}. Once the BIOS is gone (mx OS plan 2h-5) the
# kernel starts it itself instead of ExecPS2.

#include "kreplace.h"

	.set noreorder
	.set noat
	.text
	.globl kreplace_tramp
	.globl kreplace_tramp_end
	.type kreplace_tramp, @function
	.ent kreplace_tramp
kreplace_tramp:
	move $16, $4                # argc, argv for the new kernel
	move $17, $5
	lui $8, (KREPLACE_TABLE >> 16)
	ori $8, $8, (KREPLACE_TABLE & 0xffff)
	lw $9, 0($8)                # n
	lw $18, 4($8)               # entry
	addiu $8, $8, 8
1:	beqz $9, 4f
	nop
	lw $10, 0($8)               # dst
	lw $11, 4($8)               # src
	lw $12, 8($8)               # filesz
	lw $13, 12($8)              # memsz
	move $14, $0
2:	sltu $15, $14, $12          # the file's bytes
	beqz $15, 3f
	addu $24, $11, $14
	lbu $25, 0($24)
	addu $24, $10, $14
	sb $25, 0($24)
	b 2b
	addiu $14, $14, 1
3:	sltu $15, $14, $13          # the rest zero
	beqz $15, 5f
	addu $24, $10, $14
	sb $0, 0($24)
	b 3b
	addiu $14, $14, 1
5:	addiu $8, $8, 16
	b 1b
	addiu $9, $9, -1

4:	# the data cache written back and dropped, by index (64 sets of 2
	# ways; the addresses in kuseg: user mode, maxsdk's cache.c)
	lui $8, 0x0008
	ori $9, $8, 0x1000
6:	sync.l
	cache 0x14, 0($8)
	sync.l
	cache 0x14, 1($8)
	sync.l
	addiu $8, $8, 64
	bne $8, $9, 6b
	nop
	# the instruction cache dropped (128 sets of 2 ways), the BTAC flushed
	lui $8, 0x0008
	ori $9, $8, 0x2000
7:	sync.p
	cache 0x07, 0($8)
	sync.p
	cache 0x07, 1($8)
	sync.p
	addiu $8, $8, 64
	bne $8, $9, 7b
	nop
	sync.p
	cache 0x0c, 0($0)
	sync.p
	move $4, $16
	move $5, $17
	jr $18
	nop
kreplace_tramp_end:
	.end kreplace_tramp
	.size kreplace_tramp, .-kreplace_tramp
