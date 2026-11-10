/* The kernel first at every INTC and DMAC interrupt (mx OS plan, step 2d-1;
   docs/bios.md, probe/p37_irqhook.c): the BIOS's interrupt table
   (0x8001_5380) points its INTC and DMAC entries at kirq.S, which runs
   kirq(): with the kernel's threads on (2d-2) the kernel's dispatch handles
   the lines whole (ksched.c), else it goes on into the BIOS's handlers.

   Only while the kernel or a program built for it runs: before a program
   without the kernel's note (ps2sdk's, libmx, older builds), which may load
   another loader over the kernel, and before ExecPS2 of a loader, the table
   is the BIOS's again (kirq_off). */

#include <mx/bios.h>
#include <mx/cpu.h>
#include <mx/vm.h>

#include "ksched.h"
#include "mxlink.h"

#define TABLE 0x80015380u
#define ENTRY_INTC (TABLE + 4 * 2)
#define ENTRY_DMAC (TABLE + 4 * 3)
#define POKE_SYSCALL 0x69 /* RFU105: unused by the BIOS (0x5C is EnableIntcHandler) */

void kirq_intc(void);
void kirq_dmac(void);

/* kirq.S's: the registers it keeps, its stack, the BIOS's handlers (kept
   over the restart: the table may still point here then) */
u32 kirq_regs[128] __attribute__((aligned(16)));
u8 kirq_stack[4096] __attribute__((aligned(16))); /* kirq.S uses its size */
u32 kirq_orig[2] __attribute__((section(".kept"))) = {0, 0};

static volatile u32 counts[2];

/* kernel mode, interrupts off: line 0 INTC, 1 DMAC, epc the interrupted
   thread's. The kernel's handlers (ksched.c) of the pending causes; returns
   what is left for the BIOS's handler (non-zero: go on into it). */
u32 kirq(int line, u32 epc) {
  counts[line & 1]++;
  return ksched_active() ? ksched_dispatch(line, epc) : 1;
}

void kirq_counts(u32 *intc, u32 *dmac) {
  *intc = counts[0];
  *dmac = counts[1];
}

/* kernel memory written or read in kernel mode, by a syscall handler: op 0
   reads the word at a, 1 writes v there; 2 copies n bytes from physical v
   (through kseg0) to a, 3 from a to physical v (a: the caller's memory,
   mapped under its ASID in kernel mode too) */
static u32 k_poke(u32 a, u32 v, u32 op, u32 n) {
  if (op >= 2) {
    volatile u8 *m = (volatile u8 *)(uintptr_t)(0x80000000u | v), *u = (volatile u8 *)(uintptr_t)a;
    if (op == 2)
      for (u32 i = 0; i < n; i++)
        u[i] = m[i];
    else
      for (u32 i = 0; i < n; i++)
        m[i] = u[i];
    return n;
  }
  if (op)
    *(volatile u32 *)a = v;
  return *(volatile u32 *)a;
}
static int on;

static u32 kcall4(u32 a, u32 v, u32 op, u32 n) {
  register u32 a0 __asm__("$4") = a;
  register u32 a1 __asm__("$5") = v;
  register u32 a2 __asm__("$6") = op;
  register u32 a3 __asm__("$7") = n;
  register u32 v1 __asm__("$3") = POKE_SYSCALL;
  register u32 v0 __asm__("$2");
  __asm__ volatile("syscall"
                   : "=r"(v0), "+r"(v1)
                   : "r"(a0), "r"(a1), "r"(a2), "r"(a3)
                   : "memory");
  return v0;
}
static u32 kcall(u32 a, u32 v, u32 write) { return kcall4(a, v, write, 0); }

/* kernel version 21 (<mx/proc.h>'s mx_phys_read, mx_phys_write): the EE's
   physical memory to or from the caller's core memory, in pieces of 4 KiB
   (interrupts are off in the syscall). Writing is a service's only (the
   host link's writemem). 0, or -1. */
int kmem_copy(u32 phys, void *buf, u32 n, int write) {
  u32 size = MX_MAIN_BYTES, a = (u32)(uintptr_t)buf;
  if (!on || phys >= size || n > size - phys || !n || mx_is_vm(buf) || a < 0x100000u ||
      a + n < a || a + n > 0x02000000u || (write && !exec_service_caller()))
    return -1;
  for (u32 done = 0; done < n;) {
    u32 k = n - done > 4096 ? 4096 : n - done;
    /* touched in user mode first: a buffer that is not the caller's faults
       there, in its thread, not in the syscall */
    (void)*(volatile u8 *)(uintptr_t)(a + done);
    (void)*(volatile u8 *)(uintptr_t)(a + done + k - 1);
    kcall4(a + done, phys + done, write ? 3 : 2, k);
    done += k;
  }
  if (write) {
    mx_dcache_flush_all();
    mx_icache_invalidate_all();
  }
  return 0;
}

static u32 kseg0(void (*f)(void)) { return 0x80000000u | ((u32)(uintptr_t)f & 0x1FFFFFFFu); }

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
  mx_dcache_flush_all();
  mx_icache_invalidate_all();
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

/* The BIOS's SetupThread (docs/bios.md) copies into the argument block it is
   given the argc strings at the block the calling BIOS thread's last
   SetupThread was given (its thread table, 0x8001_A650, 0x4C bytes a thread:
   argc at +0x24, that block at +0x28), then keeps the new block there. The
   kernel's threads all run on one BIOS thread, so a program's start-up read
   the block of the program before it: after one at its own addresses (one
   that takes all memory), an address the next one, under its own ASID, does
   not have (a TLB miss in SetupThread before its first line). Before each
   program: argc 0, nothing copied. maxsdk's programs take their arguments
   from the kernel's (struct mx_loader_args); SetupThread's were the kernel's
   own anyway. */
#define BIOS_THREAD_NOW 0x800155ACu
#define BIOS_THREADS 0x8001A650u
#define BIOS_THREAD_SIZE 0x4Cu
#define BIOS_THREAD_ARGC 0x24u
void kirq_setup_args_clear(void) {
  if (!on)
    return;
  u32 tid = kcall(BIOS_THREAD_NOW, 0, 0);
  if (tid > 0 && tid < 256)
    kcall(BIOS_THREADS + tid * BIOS_THREAD_SIZE + BIOS_THREAD_ARGC, 0, 1);
}
