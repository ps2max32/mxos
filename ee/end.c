/* A libmxk program ends without the restart (libs/maxsdk/docs/mx-os-plan.md,
   step 2a). What ExecPS2 did, done here, in the command thread:

   - the BIOS's threads and semaphores that were not there when the program
     started (end_snapshot, just before) are terminated and deleted: the
     program's, and what the kernel made for it (the VM's pager and writer,
     its clients' semaphores);
   - the program's interrupt handlers, from the slots its libmxk showed the
     kernel (irq_tables): their BIOS ids read, no code of the program run;
     a cause or channel the kernel has no handler of its own for is disabled;
   - the units: the DMA channels (not SIF's) stopped, VIF0/VIF1, GIF, IPU,
     VU0/VU1 reset, timers 0 and 1 stopped (the GS: the screen sets it up);
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

void end_snapshot(void) {
  for (int i = 0; i < NIDS / 32; i++)
    threads_before[i] = semas_before[i] = 0;
  for (int id = 0; id < NIDS; id++) {
    if (thread_there(id))
      threads_before[id / 32] |= 1u << (id % 32);
    if (sema_there(id))
      semas_before[id / 32] |= 1u << (id % 32);
  }
  tables_known = 0;
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
    if (id == self || (threads_before[id / 32] >> (id % 32) & 1) || !thread_there(id))
      continue;
    mx_bios_terminate_thread(id);
    mx_bios_delete_thread(id);
    ended_threads++;
  }
}

static void semas_off(void) {
  ended_semas = 0;
  for (int id = 0; id < NIDS; id++)
    if (!(semas_before[id / 32] >> (id % 32) & 1) && sema_there(id)) {
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

int end_possible(void) { return __prog_data_end != 0 && __prog_bss_end != 0; }

void end_cleanup(void) {
  threads_off();
  handlers_off();
  mx__vm_unhook();
  home_unhook();
  semas_off();
  mx__sif_program_end();
  units_off();
}

void end_reset_state(void) {
  memcpy(__prog_data_start, __data_copy + (__prog_data_start - __data_start),
         (u32)(__prog_data_end - __prog_data_start));
  memset(__prog_bss_start, 0, (u32)(__prog_bss_end - __prog_bss_start));
  tables_known = 0;
}

/* the semaphores and threads now, against before the program: a check */
void end_counts(u32 *threads_left, u32 *semas_left) {
  *threads_left = *semas_left = 0;
  for (int id = 0; id < NIDS; id++) {
    int tb = threads_before[id / 32] >> (id % 32) & 1, sb = semas_before[id / 32] >> (id % 32) & 1;
    *threads_left += !tb && thread_there(id);
    *semas_left += !sb && sema_there(id);
  }
}
