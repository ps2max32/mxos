/* The kernel first at every INTC and DMAC interrupt (mx OS plan, step 2d-1;
   docs/bios.md, probe/p37_irqhook.c): the INTC and DMAC entries of the
   interrupt table point at kirq.S, which runs kirq(): with the kernel's
   threads on (2d-2) the kernel's dispatch handles the lines whole
   (ksched.c), else it goes on into the BIOS's handlers.

   Since step 2h-4d-1 the level 1 vectors are the kernel's (kvec.S) and so
   are the tables they jump through (kvec_exc_table, kvec_irq_table, filled
   from the BIOS's at kirq_on): the BIOS's tables are not written.

   Only while the kernel or a program built for it runs: before a program
   without the kernel's note (ps2sdk's, libmx, older builds), which may load
   another loader over the kernel, and before ExecPS2 of a loader, the table
   is the BIOS's again (kirq_off). */

#include <mx/bios.h>
#include <mx/cpu.h>
#include <mx/vm.h>

#include "ksched.h"
#include <mx/sifdma.h>

#include "mxlink.h"

#define TABLE 0x80015380u
#define POKE_SYSCALL 0x69 /* RFU105: unused by the BIOS (0x5C is EnableIntcHandler) */
/* the BIOS's exception table (entry 8: the syscall dispatcher) */
#define EXC_TABLE 0x80015340u

void kirq_intc(void);
void kirq_dmac(void);
void ksys_entry(void);
void kvec_irq_bios(void);
void mx__exception_vector(void);

/* kvec.S's vectors and the tables they jump through (through kseg0; by
   ExcCode, by interrupt line), and where t9 waits meanwhile */
extern const u32 kvec_exc[], kvec_exc_end[], kvec_irq[], kvec_irq_end[];
/* kept over the kernel's own restart (2h-4e-1): the vectors stay then */
u32 kvec_exc_table[16] __attribute__((section(".kept"), aligned(64)));
u32 kvec_irq_table[8] __attribute__((section(".kept"), aligned(64)));
u64 kvec_t9 __attribute__((aligned(64)));
/* the interrupted (or calling) thread's sp, ra and at, as the BIOS's save
   area at 0x8000_10C0 has them, and kvec_irq_bios's word (2h-4d-2); the
   BIOS's handlers of the lines that are not the kernel's */
u32 kvec_save[16] __attribute__((aligned(64)));
u32 kvec_irq_orig[8] __attribute__((section(".kept"), aligned(64)));
/* ksys.S's stack for the kernel's own syscalls (the BIOS's at 0x8001_8E70) */
u8 ksys_stack[KSYS_STACK] __attribute__((aligned(16)));
/* the vectors: TLB refill, common, interrupt; the BIOS's code there (kept
   over the restart, which puts it back first) */
#define VEC_BYTES 0x80u
static const u32 vec_at[3] = {0x000u, 0x180u, 0x200u};
static u32 kvec_bios[3][VEC_BYTES / 4] __attribute__((section(".kept"), aligned(64)));
static int kvec_on __attribute__((section(".kept")));

/* ksys.S's: the BIOS syscalls of the program watched (mx OS plan step
   2h-3), by v1 & 0xFF; the ASID watched (none: above 0xFF); the BIOS's
   dispatcher. Kept over the restart: a program that ends in one is
   reported after it. */
u32 ksys_counts[256] __attribute__((section(".kept"))) = {0};
u32 ksys_watch __attribute__((section(".kept"))) = 0x100;
u32 ksys_orig __attribute__((section(".kept"))) = 0;
/* the kernel's own, since its boot */
u32 ksys_kcounts[256] __attribute__((section(".kept"))) = {0};
/* the kernel's own syscall handlers, by |number| (2h-4a); 0: the BIOS's.
   Kept over the kernel's own restart, with the vectors (2h-4e-1) */
u32 ksys_handlers[128] __attribute__((section(".kept")));
static int ksys_on __attribute__((section(".kept"))); /* entry 8 is ksys.S */

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

/* the interrupts kvec_irq_bios sent on to the BIOS's handlers (2h-4d-2) */
u32 kirq_bios_count(void) {
  mx_dcache_flush_all(); /* the stub counts through kseg0 */
  return kvec_save[13];
}

/* kernel memory written or read in kernel mode, by a syscall handler: op 0
   reads the word at a, 1 writes v there; 2 copies n bytes from physical v
   (through kseg0) to a, 3 from a to physical v (a: the caller's memory,
   mapped under its ASID in kernel mode too); 4 reads the halfword at a, 5
   writes v there (registers in kseg1, kvideo.c); 6 writes code at
   physical v (kvec_write) */
static u32 k_poke(u32 a, u32 v, u32 op, u32 n) {
  if (op == 6) {
    /* code: n bytes (whole lines) from a to physical v, then out of the
       data cache there (a vector, 2h-4d-1), and the whole instruction
       cache and BTAC invalidated by index: their CACHE operations take no
       address with bit 31 set (the hardware restrictions, (13)), so not the
       vector's kseg0 address */
    volatile u32 *m = (volatile u32 *)(uintptr_t)(0x80000000u | v);
    const u32 *u = (const u32 *)(uintptr_t)a;
    for (u32 i = 0; i < n / 4; i++)
      m[i] = u[i];
    for (u32 o = 0; o < n; o += 64) {
      uintptr_t l = (uintptr_t)m + o;
      __asm__ volatile("sync.l\n\tcache 0x18, 0(%0)\n\tsync.l" : : "r"(l) : "memory");
    }
    mx_icache_invalidate_all();
    return n;
  }
  if (op >= 4) {
    if (op == 5)
      *(volatile u16 *)a = (u16)v;
    return *(volatile u16 *)a;
  }
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
  /* a function call to the handler: what a C function may change, it may
     (a0-a3 too: k_poke's loop changed a0 under a caller that kept it, 2h-4d) */
  __asm__ volatile("syscall"
                   : "=r"(v0), "+r"(v1), "+r"(a0), "+r"(a1), "+r"(a2), "+r"(a3)
                   :
                   : "memory", "$1", "$8", "$9", "$10", "$11", "$12", "$13", "$14", "$15", "$24",
                     "$25", "hi", "lo");
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

/* maxsdk's stores to the SIF's DMA registers (sifdma.c): the syscall stays
   set after kirq_off, until the ExecPS2 */
static void sif_poke(u32 a, u32 v) { kcall(a, v, 1); }
/* and its reads of the BIOS's software registers (2h-4b) */
static u32 sif_peek(u32 a) { return kcall(a, 0, 0); }

/* a vector's code, in one syscall (the common vector itself takes it) */
static void kvec_write(u32 at, const u32 *code, u32 bytes) {
  kcall4((u32)(uintptr_t)code, at, 6, bytes);
}

/* the vector's code and its size */
static const u32 *kvec_code(int i, u32 *bytes) {
  const u32 *s = i == 2 ? kvec_irq : kvec_exc, *e = i == 2 ? kvec_irq_end : kvec_exc_end;
  *bytes = (u32)((const u8 *)e - (const u8 *)s);
  return s;
}

/* SIF0's interrupt off (D_STAT's mask bit, a 1 written turns it over), as
   the BIOS's DisableDmac, without the BIOS */
void kirq_sif0_off(void) {
  volatile u32 *d_stat = (volatile u32 *)0x1000E010u;
  u32 bit = 1u << (16 + MX_DMAC_SIF0);
  if (*d_stat & bit)
    *d_stat = bit;
}

/* The kernel's vectors are still in after its own restart (2h-4e-1:
   kirq_keep, not kirq_off): its start-up sets its handlers in its tables,
   without the BIOS's thread and handler calls (start.c's mx__kernel_kept) */
int kirq_kept(void) { return kvec_on && ksys_on; }

/* Before the kernel's own restart (restart() after its first): the vectors
   and tables stay, so its start-up calls no BIOS; interrupts off until it
   is back, and they go on to the BIOS's handlers (kirq) until ksched_init */
void kirq_keep(void) {
  mx_di();
  /* every INTC cause and DMAC channel off (a 1 written turns a mask bit
     over): the BIOS has no handler of the kernel's (the vblank's, after
     its start-up's EI: "INTC(2) Handler does not exist" and a break); the
     kernel's handlers turn theirs on again, and what is pending stays */
  volatile u32 *i_mask = (volatile u32 *)0x1000F010u, *d_stat = (volatile u32 *)0x1000E010u;
  *i_mask = *i_mask;
  *d_stat = *d_stat & 0x03FF0000u;
  on = 0;
}

void kirq_on(void) {
  if (on)
    return;
  /* SIF0's interrupt off until the kernel's handler is there (cmd_init):
     after an ExecPS2 (the first start, by mxboot or a loader) the BIOS has
     no handler of the channel but leaves it on, and a transfer of the IOP's
     before the kernel's threads ends in the BIOS's "DMAC(5) Handler does
     not exist" and its break (restart does the same before kirq_off) */
  kirq_sif0_off();
  mx__kpoke32 = sif_poke;
  mx__kpeek32 = sif_peek;
  if (kirq_kept()) {
    /* after the kernel's own restart: all as it was (2h-4e-1) */
    on = 1;
    return;
  }
  /* the poke's bootstrap; it stays the BIOS's syscall after kirq_off */
  mx_bios_set_syscall(POKE_SYSCALL, (void *)k_poke);
  u32 my_intc = kseg0(kirq_intc), my_dmac = kseg0(kirq_dmac), my_sys = kseg0(ksys_entry);
  /* the BIOS's tables, the kernel's from them; its handlers of the kernel's
     entries, where they go on to (not the kernel's: an older kernel's
     kirq_off put them back) */
  for (u32 i = 0; i < 16; i++)
    kvec_exc_table[i] = kcall(EXC_TABLE + 4 * i, 0, 0);
  for (u32 i = 0; i < 8; i++)
    kvec_irq_table[i] = kcall(TABLE + 4 * i, 0, 0);
  if (kvec_irq_table[2] != my_intc)
    kirq_orig[0] = kvec_irq_table[2];
  if (kvec_irq_table[3] != my_dmac)
    kirq_orig[1] = kvec_irq_table[3];
  if (kvec_exc_table[8] != my_sys)
    ksys_orig = kvec_exc_table[8];
  if ((kirq_orig[0] >> 28) != 8 || (kirq_orig[1] >> 28) != 8 || (ksys_orig >> 28) != 8) {
    con_log(CON_RED, "irq: the BIOS's tables not as expected: no kernel interrupts");
    return;
  }
  kvec_irq_table[2] = my_intc;
  kvec_irq_table[3] = my_dmac;
  kvec_exc_table[8] = my_sys;
  /* the other lines with a handler (the COP0 timer's, 7; the BIOS has none
     of the rest, docs/bios.md) to the BIOS's handlers through
     kvec_irq_bios, which gives them the BIOS's save area and counts them
     (2h-4d-2) */
  for (int i = 0; i < 8; i++)
    if (i != 2 && i != 3 && kvec_irq_table[i]) {
      kvec_irq_orig[i] = kvec_irq_table[i];
      kvec_irq_table[i] = kseg0(kvec_irq_bios);
    }
  /* the exception codes the BIOS leaves to its default handler (its "does
     not exist" and a break): 0 (never through the common vector), 11 (a
     coprocessor unusable) and 14, 15 (none on the R5900; the BIOS's table
     keeps t9 there) to the kernel's crash report, as the others (start.c's
     mx_exception_init set those before) */
  kvec_exc_table[0] = kvec_exc_table[11] = kvec_exc_table[14] = kvec_exc_table[15] =
      kseg0(mx__exception_vector);
  /* the BIOS's vectors kept (unless they are the kernel's: left by a crash
     before kirq_off, the copy is the one before) */
  for (int i = 0; i < 3; i++) {
    u32 bytes, now[VEC_BYTES / 4];
    const u32 *code = kvec_code(i, &bytes);
    kcall4((u32)(uintptr_t)now, vec_at[i], 2, VEC_BYTES);
    int ours = 1; /* the whole stub: the interrupt vector's first word is the BIOS's too */
    for (u32 k = 0; k < bytes / 4; k++)
      ours &= now[k] == code[k];
    if (!ours) {
      for (u32 k = 0; k < VEC_BYTES / 4; k++)
        kvec_bios[i][k] = now[k];
    } else if (!kvec_bios[i][0]) {
      con_log(CON_RED, "irq: the vectors are not the BIOS's: no kernel interrupts");
      return;
    }
  }
  /* the poke the kernel's own before the vectors are: the later writes go
     through its syscall entry already */
  ksys_handlers[POKE_SYSCALL] = (u32)(uintptr_t)k_poke;
  mx_dcache_flush_all();
  mx_icache_invalidate_all();
  int was = mx_di();
  for (int i = 0; i < 3; i++) {
    u32 bytes;
    const u32 *code = kvec_code(i, &bytes);
    kvec_write(vec_at[i], code, bytes);
  }
  kvec_on = 1;
  ksys_on = 1;
  mx_irq_restore(was);
  on = 1;
}

/* A syscall handler of the kernel's own (2h-4a), run by ksys.S as the BIOS's
   dispatcher runs a SetSyscall one. 0, or -1 while entry 8 is not ksys.S. */
int ksys_set(int number, void *handler) {
  if (!ksys_on)
    return -1;
  u32 n = (u32)(number < 0 ? -number : number) & 0x7F;
  ksys_handlers[n] = (u32)(uintptr_t)handler;
  mx_dcache_flush_all(); /* ksys.S reads the table through kseg0 */
  return 0;
}

/* maxsdk's src/base/excset.c: the VM's and the exception code's handlers */
int mx__kernel_syscall_set(int number, void *handler) { return ksys_set(number, handler); }

/* the kernel's exception table (by ExcCode: 1-3 the TLB's, 4-13 the
   others; its level 1 vectors jump through it, 2h-4d-1), as the BIOS's
   SetVTLBRefillHandler and SetVCommonHandler set the BIOS's. A handler below
   32 MiB goes in through kseg0: the vectors run at EXL = 1. */
int mx__kernel_handler_set(int code, void (*handler)(void), void **old) {
  if (!kvec_on || code < 1 || code > 13 || code == 8)
    return -1;
  u32 h = (u32)(uintptr_t)handler;
  if (h < 0x02000000u)
    h |= 0x80000000u;
  *old = (void *)(uintptr_t)kvec_exc_table[code];
  kvec_exc_table[code] = h;
  mx_dcache_flush_all(); /* the vectors read it through kseg0 */
  return 0;
}

/* The program started now is watched (its ASID; 0 at its own addresses):
   its counts from 0 */
void ksys_watch_set(u32 asid) {
  for (int i = 0; i < 256; i++)
    ksys_counts[i] = 0;
  ksys_watch = asid & 0xFF;
  mx_dcache_flush_all();
}

/* the kernel's own counts since its boot (256, by v1 & 0xFF) */
const u32 *ksys_kernel_counts(void) {
  mx_dcache_flush_all();
  return ksys_kcounts;
}

/* The program watched has ended: no more counting; its counts (256, by
   v1 & 0xFF), written back by the stub through kseg0 */
const u32 *ksys_watch_end(void) {
  ksys_watch = 0x100;
  mx_dcache_flush_all();
  return ksys_counts;
}

/* the BIOS's vectors back (its tables were never written): the poke is the
   BIOS's syscall again, as kirq_on's SetSyscall left it */
void kirq_off(void) {
  if (!on)
    return;
  int was = mx_di();
  if (kvec_on)
    for (int i = 0; i < 3; i++)
      kvec_write(vec_at[i], kvec_bios[i], VEC_BYTES);
  kvec_on = 0;
  on = 0;
  ksys_on = 0;
  mx_irq_restore(was);
  for (int i = 0; i < 128; i++)
    ksys_handlers[i] = 0;
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

/* kernel mode loads and stores for kvideo.c (0 while the syscall is not there) */
int kirq_is_on(void) { return on; }
u32 kpeek32(u32 a) { return on ? kcall(a, 0, 0) : 0; }
void kpoke32(u32 a, u32 v) {
  if (on)
    kcall(a, v, 1);
}
u32 kpeek16(u32 a) { return on ? kcall4(a, 0, 4, 0) : 0; }
void kpoke16(u32 a, u32 v) {
  if (on)
    kcall4(a, v, 5, 0);
}
