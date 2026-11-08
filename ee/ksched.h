/* The kernel's threads (mx OS plan, step 2d; ksched.c, ksched.S). */
#ifndef KSCHED_H
#define KSCHED_H

/* a thread's context (struct kctx): the GPRs at KC_R + 16 n (zero, k0 and
   k1 unused), HI/LO, HI1/LO1, SA, EPC, IE (the Status bit it runs with),
   the FPU's control register, f0-f31 and the accumulator. KC_IE holds the
   Status bits EIE and IE it runs with (0x10001 or 0). */
#define KC_R 0x000
#define KC_HI 0x200
#define KC_LO 0x208
#define KC_HI1 0x210
#define KC_LO1 0x218
#define KC_SA 0x220
#define KC_EPC 0x224
#define KC_IE 0x228
#define KC_FCR31 0x22C
#define KC_F 0x230
#define KC_ACC 0x2B0
#define KC_SIZE 0x2C0

/* kirq.S's save area (kirq_regs): what C may change, and the interrupted
   thread's sp, ra, at (from the vector's area), EPC and Status */
#define KR_R1 0x000
#define KR_R2 0x010
#define KR_R3 0x020
#define KR_R4 0x030
#define KR_R5 0x040
#define KR_R6 0x050
#define KR_R7 0x060
#define KR_R8 0x070
#define KR_R9 0x080
#define KR_R10 0x090
#define KR_R11 0x0A0
#define KR_R12 0x0B0
#define KR_R13 0x0C0
#define KR_R14 0x0D0
#define KR_R15 0x0E0
#define KR_R24 0x0F0
#define KR_R25 0x100
#define KR_R28 0x110
#define KR_HI 0x120
#define KR_LO 0x128
#define KR_HI1 0x130
#define KR_LO1 0x138
#define KR_SA 0x140
#define KR_SP 0x150
#define KR_RA 0x160
#define KR_AT 0x170
#define KR_EPC 0x180
#define KR_STATUS 0x184
#define KR_LINE 0x1F0

/* the switch syscall: RFU059, unused by the BIOS */
#define KSCHED_SYSCALL 0x3B

#ifndef __ASSEMBLER__
#include <mx/types.h>

struct kctx {
  u32 r[32][4];
  u64 hi, lo, hi1, lo1;
  u32 sa, epc, ie, fcr31;
  u32 f[32];
  u32 acc, pad[3];
} __attribute__((aligned(16)));

_Static_assert(sizeof(struct kctx) == KC_SIZE, "struct kctx");

#include <mx/bios.h>
extern const struct mx_kthreads ksched_kthreads;
extern struct kctx *ksched_cur;
extern volatile int ksched_need; /* a better thread is ready: switch at the next chance */
int ksched_active(void);
void ksched_init(void);
struct kctx *ksched_pick(void);
u32 ksched_dispatch(int line, u32 epc);
/* processes (step 2d-3): a new number (1-255, -1: none free), a thread's
   process set, the process of a thread or a semaphore (-1: none there) */
int ksched_proc_new(void);
int ksched_set_proc(int id, int proc);
int ksched_thread_proc(int id);
int ksched_sema_proc(int id);
#endif
#endif
