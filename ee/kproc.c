/* Processes side by side (mx OS apps plan, step 2e-2): the one in front
   (a game, the program the kernel started last) and those beside it (up to
   two: a service, and above it one that started the front's), each a
   program with its core at a run of its own and without VM that started the
   next with mx_loader_launch_beside (or became the service) and goes on
   running.

   All cores are in the TLB at once, each under its ASID (tlb.c keeps the
   sides' slots), so a switch from one to another is (through the front's):
   - the ASID (EntryHi) and mx__core_off;
   - the part of the kernel's state of a program that each has of its own
     (resident.ld's __proc_*: heaps, memory, exceptions, threads, the pad),
     exchanged with the side's copy (end_state_swap); the rest (the VM, the
     image, the home menu) is the one in front's;
   - VU0's registers (kvu0.S), at a switch between threads only.
   The scheduler switches when the next thread is the other's (ksched_pick):
   the side's threads run in its space, the others (the kernel's included,
   which act on the one in front: the command thread) in the front's. The idle
   thread runs in either. An interrupt handler runs in its process's space,
   the kernel's in the front's, and the switch back follows
   (kproc_handler_enter, ksched_dispatch).

   Not yet: the side ending or crashing while the other runs (its mx_exit ends
   the one in front), its own pad (its ports are closed while the other runs),
   a VM of its own. */

#include <mx/bios.h>
#include <mx/cpu.h>
#include <mx/kernel.h>
#include <mx/string.h>
#include <mx/vm.h>

#include "ksched.h"
#include "mxlink.h"

struct kvu0 {
  u32 vf[32][4];
  u32 vi[24];
} __attribute__((aligned(16)));
void kvu0_save(struct kvu0 *to);
void kvu0_load(const struct kvu0 *from);

/* those beside, a stack as exec.c's parks (a service, one above it) */
#define MAX_SIDES 2
static struct side {
  int proc;
  u32 asid, core_off;
  u8 *state; /* its copy of the kernel's state of a program, in its core
                (its address: read only while its ASID is set; threads run
                in user mode, so not through kseg0): its own while another
                space is live, the front's while its own is */
  u32 keep_n, keep_end; /* tlb_core_keep with it (all below it and its own) */
  struct kvu0 vu;
  struct vm_space *space; /* its VM's address space (phase 3a), 0: none */
} sides[MAX_SIDES];
static int nsides;
static int loaded = -1; /* the side whose space, state and VU0 are live; -1 the front's */
static int handler_prev = -1;
static u32 front_asid, front_core_off;
static struct kvu0 vu_front;
static u32 switches;
#ifdef MXLINK_EMU
#define EMU_ASID(a) tlb_emu_asid(a) /* PCSX2's TLB: tlb.c */
#else
#define EMU_ASID(a)
#endif

/* to the space of side to (-1: the front's), through the front's: each
   side's copy of the state is exchanged with the front's */
static void switch_to(int to, int vu) {
  if (to == loaded)
    return;
  if (vu) {
    kvu0_save(loaded >= 0 ? &sides[loaded].vu : &vu_front);
    kvu0_load(to >= 0 ? &sides[to].vu : &vu_front);
  }
  if (loaded >= 0) {
    /* back to the front's: the side's copy is read at its own addresses */
    struct side *d = &sides[loaded];
    EMU_ASID(d->asid);
    end_state_swap(d->state);
    mx__asid_set(front_asid);
    mx__core_off = front_core_off;
    EMU_ASID(front_asid);
  }
  if (to >= 0) {
    struct side *d = &sides[to];
    front_asid = mx__asid, front_core_off = mx__core_off;
    mx__asid_set(d->asid);
    mx__core_off = d->core_off;
    EMU_ASID(d->asid);
    end_state_swap(d->state);
  }
  loaded = to;
  /* the VM's address space with it (vm.c: its TLB entries out, its fast
     refill on the new one); a process without VM leaves the VM as it is */
  struct vm_space *vsp = to >= 0 ? sides[to].space : exec_front_space();
  if (vsp)
    mx__vm_space_switch(vsp);
  switches++;
}

static int side_of(int proc) {
  for (int i = 0; proc > 0 && i < nsides; i++)
    if (sides[i].proc == proc)
      return i;
  return -1;
}

/* the last one beside (0: none) */
int kproc_side(void) { return nsides ? sides[nsides - 1].proc : 0; }
u32 kproc_switches(void) { return switches; }

/* ksched_pick, interrupts off: the next thread is of proc */
void kproc_thread(int proc) {
  if (nsides)
    switch_to(side_of(proc), 1);
}

/* ksched_dispatch: a handler of proc runs in its process: a side's in the
   side's, the others in the front's (the kernel's too: the VM's vblank
   works on the front's VM). The kernel's state of a program with it: what a
   handler calls of the kernel may use it. 1: switched, kproc_handler_leave
   after it. */
int kproc_handler_enter(int proc) {
  if (!nsides)
    return 0;
  int want = side_of(proc);
  if (want == loaded)
    return 0;
  handler_prev = loaded;
  switch_to(want, 0);
  return 1;
}
void kproc_handler_leave(void) { switch_to(handler_prev, 0); }

/* exec_park, in the side's thread under its ASID (the front's space live),
   its kernel state saved in state (in its core): the live state becomes the
   front's new one, as linked, and the side's part of it its own again at
   once, the thread going on. Its core's TLB entries (core_slots, after
   those of the sides below it) kept. */
void kproc_side_begin(int proc, void *state, u32 core_slots, u32 core_end, struct vm_space *space) {
  int was = mx_di();
  switch_to(-1, 1); /* its thread is the front's: so it is already */
  struct side *d = &sides[nsides];
  u32 below_n = nsides ? sides[nsides - 1].keep_n : 0;
  u32 below_end = nsides ? sides[nsides - 1].keep_end : 0;
  d->proc = proc;
  d->space = space;
  d->asid = mx__asid, d->core_off = mx__core_off;
  d->state = state;
  d->keep_n = below_n + core_slots;
  d->keep_end = core_end > below_end ? core_end : below_end;
  tlb_core_keep(d->keep_n, d->keep_end);
  end_reset_state();
  end_state_swap(d->state);
  /* the front's space is its, from here: the next program's */
  front_asid = 0, front_core_off = 0;
  memset(&vu_front, 0, sizeof vu_front);
  loaded = nsides;
  nsides++;
  mx_irq_restore(was);
}

/* exec_unpark and exec_park_drop, in the command thread (the front's space):
   the last side's threads held (ksched_proc_hold) until it is the one in
   front again, its VU0 registers live; its TLB entries no longer kept. */
void kproc_side_end(void) {
  if (!nsides)
    return;
  int was = mx_di();
  struct side *d = &sides[nsides - 1];
  ksched_proc_hold(d->proc, 1);
  switch_to(-1, 1);
  kvu0_load(&d->vu);
  nsides--;
  if (nsides)
    tlb_core_keep(sides[nsides - 1].keep_n, sides[nsides - 1].keep_end);
  else
    tlb_core_keep(0, 0);
  mx_irq_restore(was);
}

/* maxsdk's timers (src/irq/timer.c): the process a timer is added for, and
   its callback run in that process's address space (as a handler of it) */
int mx__timer_proc(void) {
  int p = ksched_active() ? ksched_thread_proc(mx_bios_get_thread_id()) : 0;
  return p > 0 ? p : 0;
}
int mx__timer_enter(int proc) { return kproc_handler_enter(proc); }
void mx__timer_leave(void) { kproc_handler_leave(); }
