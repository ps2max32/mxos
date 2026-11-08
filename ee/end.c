/* A libmxk program ends without the restart (libs/maxsdk/docs/mx-os-plan.md,
   step 2a). What ExecPS2 did, done here, in the command thread:

   - the program's threads and semaphores are terminated and deleted: on the
     kernel's threads those of its process (ksched.c, step 2d-3), on the
     BIOS's those that were not there when it started (end_snapshot, just
     before); the program's, and what the kernel made for it in its threads
     (the VM's pager and writer, its clients' semaphores);
   - the program's interrupt handlers, from the slots its libmxk showed the
     kernel (irq_tables): their BIOS ids read, no code of the program run;
     a cause or channel the kernel has no handler of its own for is disabled;
   - the units: the DMA channels (not SIF's) stopped, VIF0/VIF1, GIF, IPU,
     VU0/VU1 reset, timers 0 and 1 stopped (the GS: the screen sets it up);
   - the program's calls to the IOP that were under way waited for, and the
     caches written back and emptied;
   - the kernel's state of the program (the VM, heaps, exceptions, threads,
     the pad, the home menu: their .data and .bss, apart in resident.ld) set
     back as linked.
   The rest (files, sockets, the pad's ports, sound) is main.c's cleanup, as
   after a restart. */

#include <mx/bios.h>
#include <mx/hw/regs.h>
#include <mx/irq.h>
#include <mx/kernel.h>
#include <mx/string.h>

#include "ksched.h"
#include "mxlink.h"

#define NIDS 256
static u32 threads_before[NIDS / 32], semas_before[NIDS / 32];

/* ReferThreadStatus returns the thread's status (1 running, 2 ready, 4
   waiting, ...), 0 for an id with no thread, and takes 0 for the caller
   (probe/p34_ids.c, docs/bios.md) */
static int thread_there(int id) {
  u32 st[16];
  return id > 0 && mx_bios_refer_thread_status(id, st) > 0;
}
static int sema_there(int id) {
  u32 st[8];
  return mx_bios_refer_sema_status(id, st) >= 0;
}

static struct mxk_irq_tables tables;
static int tables_known;

/* on the kernel's threads: the program's process, and the one of a program
   given up that ends with it (0: none; the snapshot then) */
static int proc_now, proc_dropped;

static int thread_mine(int id) {
  if (proc_now > 0) {
    int p = ksched_thread_proc(id);
    return p > 0 && (p == proc_now || p == proc_dropped);
  }
  return !(threads_before[id / 32] >> (id % 32) & 1) && thread_there(id);
}
static int sema_mine(int id) {
  if (proc_now > 0) {
    int p = ksched_sema_proc(id);
    return p > 0 && (p == proc_now || p == proc_dropped);
  }
  return !(semas_before[id / 32] >> (id % 32) & 1) && sema_there(id);
}

/* step 2c: what a program that waits for another (exec_park) had here */
static struct {
  u32 threads_before[NIDS / 32], semas_before[NIDS / 32];
  struct mxk_irq_tables tables;
  u32 intc_mask, dmac_mask; /* its slots that had a BIOS handler */
  u8 threads[64];           /* its threads suspended */
  u32 nthreads;
  int proc, dropped;        /* proc_now and proc_dropped */
  int drop; /* given up: the next snapshot is its, so its end takes it too */
} parked;

void end_snapshot(int proc) {
  tables_known = 0;
  proc_now = proc, proc_dropped = 0;
  if (parked.drop) {
    parked.drop = 0;
    proc_dropped = parked.proc;
    for (int i = 0; i < NIDS / 32; i++)
      threads_before[i] = parked.threads_before[i], semas_before[i] = parked.semas_before[i];
    return;
  }
  if (proc > 0)
    return; /* its process tells */
  for (int i = 0; i < NIDS / 32; i++)
    threads_before[i] = semas_before[i] = 0;
  for (int id = 0; id < NIDS; id++) {
    if (thread_there(id))
      threads_before[id / 32] |= 1u << (id % 32);
    if (sema_there(id))
      semas_before[id / 32] |= 1u << (id % 32);
  }
}

/* in core memory of a program, and the whole range */
static int program_range(const void *p, u32 bytes) {
  u32 a = (u32)(uintptr_t)p;
  return a >= 0x00100000u && a < 0x02000000u && bytes <= 0x02000000u - a;
}

void end_irq_tables(const struct mxk_irq_tables *t) {
  if (!program_range(t, sizeof *t) || t->nintc > 15 || t->ndmac > 10 || t->slot_size > 64 ||
      t->id_offset + 4 > t->slot_size || !program_range(t->intc, t->nintc * t->slot_size) ||
      !program_range(t->dmac, t->ndmac * t->slot_size))
    return;
  tables = *t;
  tables_known = 1;
}

static int slot_id(const void *base, u32 i) {
  return *(const volatile int *)((const u8 *)base + i * tables.slot_size + tables.id_offset);
}

/* the kernel's own handler for INTC cause / DMAC channel n (maxsdk's irq.c) */
int mx__irq_slot_used(int dmac, int n);

static void handlers_off(void) {
  if (!tables_known)
    return;
  for (u32 i = 0; i < tables.nintc; i++) {
    int id = slot_id(tables.intc, i);
    if (id <= 0)
      continue;
    mx_bios_remove_intc_handler((int)i, id);
    if (!mx__irq_slot_used(0, (int)i))
      mx_bios_disable_intc((int)i);
  }
  for (u32 i = 0; i < tables.ndmac; i++) {
    int id = slot_id(tables.dmac, i);
    if (id <= 0)
      continue;
    mx_bios_remove_dmac_handler((int)i, id);
    /* SIF's channels are the kernel's (its own handlers, not irq.c's) */
    if (i != MX_DMAC_SIF0 && i != MX_DMAC_SIF1 && i != MX_DMAC_SIF2 && !mx__irq_slot_used(1, (int)i))
      mx_bios_disable_dmac((int)i);
  }
}

static u32 ended_threads, ended_semas;
void end_ended(u32 *threads, u32 *semas) {
  *threads = ended_threads;
  *semas = ended_semas;
}

static void threads_off(void) {
  int self = mx_bios_get_thread_id();
  ended_threads = 0;
  for (int id = 1; id < NIDS; id++) {
    if (id == self || !thread_mine(id))
      continue;
    mx_bios_terminate_thread(id);
    mx_bios_delete_thread(id);
    ended_threads++;
  }
}

static void semas_off(void) {
  ended_semas = 0;
  for (int id = 0; id < NIDS; id++)
    if (sema_mine(id)) {
      mx_bios_delete_sema(id);
      ended_semas++;
    }
}

/* DMA channels 0-4, 8, 9 stopped (5-7 are SIF's) */
static const u32 chcr[] = {MX_DMA_VIF0, MX_DMA_VIF1, MX_DMA_GIF, MX_DMA_FROMIPU, MX_DMA_TOIPU,
                           MX_DMA_FROMSPR, MX_DMA_TOSPR};
#define IPU_CTRL 0x10002010u /* RST: bit 30 */

static void units_off(void) {
  int was = mx_di();
  for (u32 i = 0; i < sizeof chcr / sizeof chcr[0]; i++)
    MX_R32(chcr[i]) = 0;
  /* their end flags cleared (writing 1 clears; the masks are the BIOS's,
     handlers_off disabled the program's) */
  u32 sif = 1u << MX_DMAC_SIF0 | 1u << MX_DMAC_SIF1 | 1u << MX_DMAC_SIF2;
  MX_R32(MX_D_STAT) = MX_R32(MX_D_STAT) & 0x3FFu & ~sif;
  /* D_CTRL: DMA on, no MFIFO, no stall control */
  MX_R32(MX_D_CTRL) = 1;
  MX_R32(MX_VIF_VIF0_FBRST) = 1; /* RST */
  MX_R32(MX_VIF_VIF1_FBRST) = 1;
  MX_R32(MX_GIF_CTRL) = 1; /* RST */
  MX_R32(IPU_CTRL) = 1u << 30;
  /* VU0 and VU1: force break and reset (FBRST, vi28: FB0 RS0, FB1 RS1) */
  __asm__ volatile("move $8, %0\n\t.word 0x48c8e000\n\tsync.p" /* ctc2 $8, vi28 */
                   :
                   : "r"(0x0303u)
                   : "$8");
  /* timers 0 and 1 stopped, their EQUF and OVFF flags cleared */
  MX_R32(MX_TIMER_T0_MODE) = 0xC00;
  MX_R32(MX_TIMER_T1_MODE) = 0xC00;
  mx_irq_restore(was);
}

/* resident.ld: the kernel's state of a program, and .data's copy */
extern char __prog_data_start[] __attribute__((weak)), __prog_data_end[] __attribute__((weak));
extern char __prog_bss_start[] __attribute__((weak)), __prog_bss_end[] __attribute__((weak));
extern char __data_start[], __data_copy[];

int end_tables_known(void) { return tables_known; }

int end_possible(void) { return __prog_data_end != 0 && __prog_bss_end != 0; }

static int calls_left;
void end_cleanup(void) {
  threads_off();
  /* their calls to the IOP ended, before the next call or semas_off */
  calls_left = mx__sif_calls_finish();
  handlers_off();
  mx__vm_unhook();
  home_unhook();
  semas_off();
  /* the clients of a deleted semaphore told, before an id comes back */
  end_kernel_clients();
  mx__sif_program_end();
  units_off();
  /* The data cache still holds the program's dirty lines: written back
     later (evicted, or the flush after the next load), they would land over
     what the next program's load brought in by DMA (a game in VM, then the
     shell: the shell ran its font as code). ExecPS2 did this. Both caches
     now, before anything is loaded. */
  mx_bios_flush_cache(0);
  mx_bios_flush_cache(2);
}

void end_reset_state(void) {
  memcpy(__prog_data_start, __data_copy + (__prog_data_start - __data_start),
         (u32)(__prog_data_end - __prog_data_start));
  memset(__prog_bss_start, 0, (u32)(__prog_bss_end - __prog_bss_start));
  tables_known = 0;
}

/* the semaphores and threads now, against before the program: a check */
void end_counts(u32 *threads_left, u32 *semas_left, u32 *calls) {
  *calls = (u32)calls_left;
  *threads_left = *semas_left = 0;
  for (int id = 0; id < NIDS; id++) {
    *threads_left += id > 0 && thread_mine(id);
    *semas_left += sema_mine(id);
  }
}

/* ---- step 2c: a program waits for one it started (exec_park) ---------------- */

static void slot_set(const void *base, u32 i, int id) {
  *(volatile int *)((const u8 *)base + i * tables.slot_size + tables.id_offset) = id;
}

/* In the waiting program's thread (self), under its ASID: its handlers taken
   from the BIOS (they would run in the other's address space), its other
   threads suspended, what the kernel knows of it kept. -1: not possible. */
int end_park(int self) {
  if (!tables_known || !tables.intc_entry || !tables.dmac_entry ||
      !program_range(tables.intc_entry, 4) || !program_range(tables.dmac_entry, 4))
    return -1;
  for (int i = 0; i < NIDS / 32; i++)
    parked.threads_before[i] = threads_before[i], parked.semas_before[i] = semas_before[i];
  parked.tables = tables;
  parked.proc = proc_now, parked.dropped = proc_dropped;
  parked.intc_mask = parked.dmac_mask = 0;
  for (u32 i = 0; i < tables.nintc; i++) {
    int id = slot_id(tables.intc, i);
    if (id > 0) {
      mx_bios_remove_intc_handler((int)i, id);
      parked.intc_mask |= 1u << i;
    }
  }
  for (u32 i = 0; i < tables.ndmac; i++) {
    int id = slot_id(tables.dmac, i);
    if (id > 0) {
      mx_bios_remove_dmac_handler((int)i, id);
      parked.dmac_mask |= 1u << i;
    }
  }
  parked.nthreads = 0;
  for (int id = 1; id < NIDS && parked.nthreads < sizeof parked.threads; id++)
    if (id != self && thread_mine(id)) {
      mx_bios_suspend_thread(id);
      parked.threads[parked.nthreads++] = (u8)id;
    }
  parked.drop = 0;
  return 0;
}

/* In the command thread, under its ASID again, its kernel state back: its
   handlers added again (their new ids in its slots), its causes and
   channels on, its threads resumed. */
void end_unpark(void) {
  for (int i = 0; i < NIDS / 32; i++)
    threads_before[i] = parked.threads_before[i], semas_before[i] = parked.semas_before[i];
  tables = parked.tables;
  tables_known = 1;
  proc_now = parked.proc, proc_dropped = parked.dropped;
  for (u32 i = 0; i < tables.nintc; i++)
    if (parked.intc_mask >> i & 1) {
      slot_set(tables.intc, i, mx_bios_add_intc_handler((int)i, (int (*)(int, void *, void *))(uintptr_t)tables.intc_entry, 0, 0));
      mx_bios_enable_intc((int)i);
    }
  for (u32 i = 0; i < tables.ndmac; i++)
    if (parked.dmac_mask >> i & 1) {
      slot_set(tables.dmac, i, mx_bios_add_dmac_handler((int)i, (int (*)(int, void *, void *))(uintptr_t)tables.dmac_entry, 0, 0));
      mx_bios_enable_dmac((int)i);
    }
  for (u32 i = 0; i < parked.nthreads; i++)
    mx_bios_resume_thread(parked.threads[i]);
}

/* Given up (the other could not have a run of its own and takes all
   memory): its threads and semaphores go with the other's end. */
void end_drop(void) { parked.drop = 1; }

/* The kernel's state of a program (resident.ld): its size, kept and put back */
u32 end_state_size(void) {
  return (u32)(__prog_data_end - __prog_data_start) + (u32)(__prog_bss_end - __prog_bss_start);
}
void end_state_save(void *to) {
  u32 d = (u32)(__prog_data_end - __prog_data_start);
  memcpy(to, __prog_data_start, d);
  memcpy((u8 *)to + d, __prog_bss_start, (u32)(__prog_bss_end - __prog_bss_start));
}
void end_state_load(const void *from) {
  u32 d = (u32)(__prog_data_end - __prog_data_start);
  memcpy(__prog_data_start, from, d);
  memcpy(__prog_bss_start, (const u8 *)from + d, (u32)(__prog_bss_end - __prog_bss_start));
}
