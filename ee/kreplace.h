#ifndef KREPLACE_H
#define KREPLACE_H

/* The kernel replaced by another (exec.c, kreplace.S): where its trampoline
   and table go, and where the new kernel's segments are read to, in free
   program memory that ExecPS2 keeps (no program runs then; the services'
   cores are at the top of memory) */
#define KREPLACE_AT 0x00F00000
#define KREPLACE_TABLE 0x00F00800
#define KREPLACE_STAGE 0x01000000
#define KREPLACE_MAX 8 /* segments */

/* The table: n, the entry, what is ORed into each segment's address when it
   is written (0, or 0x80000000: kseg0, when the trampoline runs in kernel
   mode), a spare word; then n times {dst, src, filesz, memsz}. */

/* Without the BIOS (mx OS plan step 4a, part 2: kirq_bios_gone) there is no
   ExecPS2: the old kernel starts the trampoline itself, in kernel mode, and
   the new kernel is entered in kernel mode too, with this block of what the
   old one had from the BIOS at its first start. The new one finds it by its
   magic and being in kernel mode (main.c's mx__kernel_kept), puts in its own
   vectors and goes on in user mode as after its own restart. */
#define KREPLACE_HANDOFF 0x00F00C00
#define KREPLACE_MAGIC 0x4B48584Du /* "MXHK" */
#ifndef __ASSEMBLER__
struct kreplace_handoff {
  u32 magic, size;
  u32 status;       /* COP0 Status of the old kernel's thread, interrupts off */
  u32 bios_delta;   /* kirq.c */
  u32 video[5];     /* kvideo.c: known, has_dxdy, seen[3] */
  s16 dxdy[0x72][2];
  u32 dxdy_known;
  u32 sysregs[32];  /* the SIF's software registers (maxsdk's sifdma.c) */
  char path[256];   /* the new kernel's argv[0] */
};
#endif

#endif
