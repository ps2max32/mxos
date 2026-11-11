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

#endif
