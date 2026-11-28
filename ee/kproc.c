/* Processes side by side (mx OS apps plan, step 2e-2): the one in front
   (a game, the program the kernel started last) and those beside it (up to
   two: a service, and above it one that started the front's), each a
   program with its core at a run of its own and without VM that started the
   next with mx_loader_launch_beside (or became the service) and goes on
   running.

   Only the front's core has TLB entries of its own (mx OS plan step 6, part
   5, 2026-11-27): a side's core is reached by refills (maxsdk's vm.c asks
   mx__core_pa below), 32 KiB entries of the VM's ring under its ASID, which
   leave the TLB when the kernel switches away from it (mx__vm_core_out).
   Its frames stay where they are. Nothing of a side runs at interrupt level
   (its handlers are out, end.c, and its timers wait: kproc_handler_enter),
   and what a switch reads and writes of it is in the kernel's memory (its
   record here, side_mem, since 2026-11-28), none in its own. A switch from
   one to another is (through the front's):
   - the ASID (EntryHi) and mx__core_off;
   - the part of the kernel's state of a program that each has of its own
     (resident.ld's __proc_*: heaps, memory, exceptions, threads, the pad),
     exchanged with the side's record (end_state_swap; into its whole copy,
     in its core, when it leaves the list); the rest (the VM, the image, the
     home menu) is the one in front's;
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
void mx__vm_core_out(void); /* maxsdk's vm.c */
u32 mx__core_pa(u32 va);
void kvu0_save(struct kvu0 *to);
void kvu0_load(const struct kvu0 *from);

/* those beside, a stack as exec.c's parks (a service, one above it) */
#define MAX_SIDES 4
/* what a process beside has of its own in the kernel's state (end.c, the
   __proc_* of resident.ld): at most this much (resident.ld checks it) */
#define SIDE_STATE 4096
struct side {
  int proc;
  u32 asid, core_off;
  u8 *state; /* its whole copy of the kernel's state, in its core (exec_park's
                record, through MX_PHYS_WIN): read again when it is in front */
  u32 keep_end;  /* tlb_core_side with it (the end of its core and of those below) */
  u32 own_end;   /* the end of its own core */
  struct kvu0 vu;
  struct vm_space *space; /* its VM's address space (phase 3a), 0: none */
  u32 runs;               /* how often its space was made live (mx OS plan step 6: what wakes it) */
  /* its part of the kernel's state while another's is live, the other's
     while its own is (end_state_swap) */
  u8 ps[SIDE_STATE] __attribute__((aligned(16)));
};
/* in the kernel's memory (mx OS plan step 6): a switch, at interrupt level,
   reads and writes nothing of a side's own memory, which may be paged out */
static struct side side_mem[MAX_SIDES];
static struct side *sides[MAX_SIDES];
static int nsides;
/* a paused program's VU0 registers, in exec.c's record (step 4d) */
u32 kproc_rec_size(void) { return (sizeof(struct kvu0) + 63) & ~63u; }
static int loaded = -1; /* the side whose space, state and VU0 are live; -1 the front's */
/* the spaces handlers left, the last one's on top: a handler entered inside
   another (the kernel's timer handler runs in the front's, and a process's
   timer callback, called by it, in that process's) */
static int handler_prev[4];
static int handler_depth;
static u32 front_asid, front_core_off;
static struct kvu0 vu_front;
static u32 switches;
/* PCSX2's TLB (tlb.c: the emu build, or the PS2 build where the kernel's
   start found the ASID ignored) */
extern int tlb_emu;
void tlb_emu_hide(void);
void tlb_emu_unhide(void);
#define EMU_ASID(a)                                                                        \
  do {                                                                                     \
    if (tlb_emu)                                                                           \
      tlb_emu_asid(a);                                                                     \
  } while (0)

/* to the space of side to (-1: the front's), through the front's: each
   side's copy of the state is exchanged with the front's */
static void switch_to(int to, int vu) {
  if (to == loaded)
    return;
  if (vu) {
    kvu0_save(loaded >= 0 ? &sides[loaded]->vu : &vu_front);
    kvu0_load(to >= 0 ? &sides[to]->vu : &vu_front);
  }
  if (loaded >= 0) {
    /* back to the front's: the side's core's entries out first (on PCSX2
       too before the front's are written again) */
    struct side *d = sides[loaded];
    mx__vm_core_out();
    end_state_swap(d->ps);
    mx__asid_set(front_asid);
    mx__core_off = front_core_off;
    if (tlb_emu)
      tlb_emu_unhide();
    EMU_ASID(front_asid);
  }
  if (to >= 0) {
    struct side *d = sides[to];
    front_asid = mx__asid, front_core_off = mx__core_off;
    if (tlb_emu)
      tlb_emu_hide();
    mx__asid_set(d->asid);
    mx__core_off = d->core_off;
    EMU_ASID(d->asid);
    end_state_swap(d->ps);
    d->runs++;
  }
  loaded = to;
  /* the VM's address space with it (vm.c: its TLB entries out, its fast
     refill on the new one); a process without VM leaves the VM as it is */
  struct vm_space *vsp = to >= 0 ? sides[to]->space : exec_front_space();
  if (vsp)
    mx__vm_space_switch(vsp);
  switches++;
}

static int side_of(int proc) {
  for (int i = 0; proc > 0 && i < nsides; i++)
    if (sides[i]->proc == proc)
      return i;
  return -1;
}

/* maxsdk's vm.c, at a TLB refill (interrupts off): va's physical address
   in the core of the side whose space is live, 0 if it is not in one */
u32 mx__core_pa(u32 va) {
  if (loaded < 0)
    return 0;
  struct side *d = sides[loaded];
  if (mx__asid != d->asid || va < 0x00100000u || va >= d->own_end)
    return 0;
  return va + d->core_off;
}

/* the last one beside (0: none) */
int kproc_side(void) { return nsides ? sides[nsides - 1]->proc : 0; }
int kproc_is_side(int proc) { return side_of(proc) >= 0; }
u32 kproc_switches(void) { return switches; }
/* how often process proc ran beside since it went there; ~0u: not beside */
u32 kproc_side_runs(int proc) {
  int i = side_of(proc);
  return i < 0 ? ~0u : sides[i]->runs;
}

/* ksched_pick, interrupts off: the next thread is of proc */
void kproc_thread(int proc) {
  if (nsides)
    switch_to(side_of(proc), 1);
}

/* ksched_dispatch: a handler of proc runs in its process: a side's in the
   side's, the others in the front's (the kernel's too: the VM's vblank
   works on the front's VM). The kernel's state of a program with it: what a
   handler calls of the kernel may use it. 1: switched, kproc_handler_leave
   after it: back to the space it was entered from. One entered inside
   another has its own way back (with one way back for both, a side's thread
   whose own timer came in went on in the front's space, and the PS2 hung). */
int kproc_handler_enter(int proc) {
  /* a process that waits (paused, or for the one it started): not mapped,
     its handler must not run (a libgl program's timer ran in the other's
     memory and crashed it: mx OS plan step 4d) */
  if (proc > 0 && ptab_waiting(proc))
    return -1;
  if (!nsides)
    return 0;
  int want = side_of(proc);
  /* a side's core has no entries of its own: nothing of it at interrupt
     level (a timer of it waits until it is in front again) */
  if (want >= 0)
    return -1;
  if (want == loaded || handler_depth == (int)(sizeof handler_prev / sizeof handler_prev[0]))
    return 0;
  handler_prev[handler_depth++] = loaded;
  switch_to(want, 0);
  return 1;
}
void kproc_handler_leave(void) {
  if (handler_depth)
    switch_to(handler_prev[--handler_depth], 0);
}

/* exec_park, in the side's thread under its ASID (the front's space live),
   its kernel state saved in state (in its core): the live state becomes the
   front's new one, as linked, and the side's part of it its own again at
   once, the thread going on. Its core's TLB entries (core_slots, after
   those of the sides below it) kept. */
void kproc_side_begin(int proc, void *state, u32 core_slots, u32 core_end, struct vm_space *space) {
  int was = mx_di();
  switch_to(-1, 1); /* its thread is the front's: so it is already */
  struct side *d = 0;
  for (int i = 0; i < MAX_SIDES && !d; i++) {
    d = &side_mem[i];
    for (int k = 0; k < nsides; k++)
      if (sides[k] == d)
        d = 0;
  }
  memset(d, 0, sizeof *d - sizeof d->ps);
  sides[nsides] = d;
  u32 below_end = nsides ? sides[nsides - 1]->keep_end : 0;
  d->proc = proc;
  d->space = space;
  d->asid = mx__asid, d->core_off = mx__core_off;
  d->state = (u8 *)(uintptr_t)(MX_PHYS_WIN + mx_paddr_of(state).v);
  end_proc_get(d->ps, d->state);
  (void)core_slots; /* its entries are not kept: the next core's take them */
  d->own_end = core_end;
  d->keep_end = core_end > below_end ? core_end : below_end;
  tlb_core_side(d->keep_end);
  end_reset_state();
  end_state_swap(d->ps);
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
  struct side *d = sides[nsides - 1];
  ksched_proc_hold(d->proc, 1);
  switch_to(-1, 1);
  end_proc_put(d->state, d->ps); /* its own part into its copy: exec.c loads that */
  kvu0_load(&d->vu);
  nsides--;
  if (nsides)
    tlb_core_side(sides[nsides - 1]->keep_end);
  else
    tlb_core_side(0);
  mx_irq_restore(was);
}

/* A process beside that ended alone (step 4c), in the command thread: its
   threads held, the front's space and state live, and it out of the list;
   the TLB entries kept are the top one's (those of one in the middle stay
   kept until the ones above it end). */
void kproc_side_remove(int proc) {
  int i = side_of(proc);
  if (i < 0)
    return;
  int was = mx_di();
  ksched_proc_hold(proc, 1);
  switch_to(-1, 1);
  for (int k = i; k + 1 < nsides; k++)
    sides[k] = sides[k + 1];
  nsides--;
  if (nsides)
    tlb_core_side(sides[nsides - 1]->keep_end);
  else
    tlb_core_side(0);
  mx_irq_restore(was);
}

/* step 4d: VU0's registers of a program paused in front into its record's
   kproc.c part, and back (the record through MX_PHYS_WIN) */
void kproc_vu_save(void *rec) { kvu0_save((struct kvu0 *)rec); }
void kproc_vu_load(void *rec) { kvu0_load((const struct kvu0 *)rec); }

/* step 4d: a process beside comes in front (exec_front_switch, the command
   thread): out of the list, its threads running in the front's space from
   now; the front's state live (its own part is in its copy, put back by
   exec.c with end_state_load). */
void kproc_side_front(int proc) {
  int i = side_of(proc);
  if (i < 0)
    return;
  int was = mx_di();
  switch_to(-1, 1);
  end_proc_put(sides[i]->state, sides[i]->ps); /* its own part into its copy: exec.c loads that */
  kvu0_load(&sides[i]->vu); /* its VU0 registers live: it is the front's now */
  for (int k = i; k + 1 < nsides; k++)
    sides[k] = sides[k + 1];
  nsides--;
  if (nsides)
    tlb_core_side(sides[nsides - 1]->keep_end);
  else
    tlb_core_side(0);
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
