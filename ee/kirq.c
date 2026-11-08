/* The kernel first at every INTC and DMAC interrupt (mx OS plan, step 2d-1;
   docs/bios.md, probe/p37_irqhook.c): the BIOS's interrupt table
   (0x8001_5380) points its INTC and DMAC entries at kirq.S, which runs
   kirq() and goes on into the BIOS's handlers. The base of the kernel's own
   interrupt dispatch and thread switch (2d-2).

   Only while the kernel or a program built for it runs: before a program
   without the kernel's note (ps2sdk's, libmx, older builds), which may load
   another loader over the kernel, and before ExecPS2 of a loader, the table
   is the BIOS's again (kirq_off). */

#include <mx/bios.h>
#include <mx/cpu.h>

#include "mxlink.h"

#define TABLE 0x80015380u
#define ENTRY_INTC (TABLE + 4 * 2)
#define ENTRY_DMAC (TABLE + 4 * 3)
#define POKE_SYSCALL 0x5C

void kirq_intc(void);
void kirq_dmac(void);

/* kirq.S's: the registers it keeps, its stack, the BIOS's handlers (kept
   over the restart: the table may still point here then) */
u32 kirq_regs[128] __attribute__((aligned(16)));
u8 kirq_stack[4096] __attribute__((aligned(16))); /* kirq.S uses its size */
u32 kirq_orig[2] __attribute__((section(".kept"))) = {0, 0};

static volatile u32 counts[2];

/* at EXL = 1, interrupts off: line 0 INTC, 1 DMAC */
void kirq(int line) { counts[line & 1]++; }

void kirq_counts(u32 *intc, u32 *dmac) {
  *intc = counts[0];
  *dmac = counts[1];
}

/* kernel memory written or read in kernel mode, by a syscall handler */
static u32 k_poke(u32 a, u32 v, u32 write) {
  if (write)
    *(volatile u32 *)a = v;
  return *(volatile u32 *)a;
}
static u32 kcall(u32 a, u32 v, u32 write) {
  register u32 a0 __asm__("$4") = a;
  register u32 a1 __asm__("$5") = v;
  register u32 a2 __asm__("$6") = write;
  register u32 v1 __asm__("$3") = POKE_SYSCALL;
  register u32 v0 __asm__("$2");
  __asm__ volatile("syscall" : "=r"(v0), "+r"(v1) : "r"(a0), "r"(a1), "r"(a2) : "memory");
  return v0;
}

static u32 kseg0(void (*f)(void)) { return 0x80000000u | ((u32)(uintptr_t)f & 0x1FFFFFFFu); }

static int on;

void kirq_on(void) {
  if (on)
    return;
  mx_bios_set_syscall(POKE_SYSCALL, (void *)k_poke);
  u32 intc = kcall(ENTRY_INTC, 0, 0), dmac = kcall(ENTRY_DMAC, 0, 0);
  u32 my_intc = kseg0(kirq_intc), my_dmac = kseg0(kirq_dmac);
  /* not ours already (a restart left them): the BIOS's */
  if (intc != my_intc)
    kirq_orig[0] = intc;
  if (dmac != my_dmac)
    kirq_orig[1] = dmac;
  if ((kirq_orig[0] >> 28) != 8 || (kirq_orig[1] >> 28) != 8)
    return; /* not the table expected: the BIOS's stays */
  mx_bios_flush_cache(0);
  mx_bios_flush_cache(2);
  int was = mx_di();
  kcall(ENTRY_INTC, my_intc, 1);
  kcall(ENTRY_DMAC, my_dmac, 1);
  mx_irq_restore(was);
  on = 1;
}

void kirq_off(void) {
  if (!on && kirq_orig[0] == 0)
    return;
  mx_bios_set_syscall(POKE_SYSCALL, (void *)k_poke);
  int was = mx_di();
  if ((kirq_orig[0] >> 28) == 8)
    kcall(ENTRY_INTC, kirq_orig[0], 1);
  if ((kirq_orig[1] >> 28) == 8)
    kcall(ENTRY_DMAC, kirq_orig[1], 1);
  mx_irq_restore(was);
  on = 0;
}
