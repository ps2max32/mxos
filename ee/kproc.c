/* Two processes side by side (mx OS apps plan, step 2e-2): the one in front
   (a game, the program the kernel started last) and one beside it, a
   program with its core at a run of its own and without VM that started the
   other with mx_loader_launch_beside and goes on running.

   Both cores are in the TLB at once, each under its ASID (tlb.c keeps the
   side's slots), so a switch from one to the other is:
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

static struct {
  int on, proc;
  u32 asid, core_off;
  u8 *state; /* its copy of the kernel's state of a program, in its core
                (its address: read only while its ASID is set; threads run
                in user mode, so not through kseg0) */
} side;
static int loaded; /* 1: the side's space, state and VU0 are live */
static u32 front_asid, front_core_off;
static struct kvu0 vu_side, vu_front;
static u32 switches;
#ifdef MXLINK_EMU
#define EMU_ASID(a) tlb_emu_asid(a) /* PCSX2's TLB: tlb.c */
#else
#define EMU_ASID(a)
#endif

static void switch_to(int to_side, int vu) {
  if (vu) {
    kvu0_save(loaded ? &vu_side : &vu_front);
    kvu0_load(to_side ? &vu_side : &vu_front);
  }
  /* the side's copy of the state is read at its own addresses */
  if (to_side) {
    front_asid = mx__asid, front_core_off = mx__core_off;
    mx__asid_set(side.asid);
    mx__core_off = side.core_off;
    EMU_ASID(side.asid);
    end_state_swap(side.state);
  } else {
    EMU_ASID(side.asid);
    end_state_swap(side.state);
    mx__asid_set(front_asid);
    mx__core_off = front_core_off;
    EMU_ASID(front_asid);
  }
  loaded = to_side;
  switches++;
}

int kproc_side(void) { return side.on ? side.proc : 0; }
u32 kproc_switches(void) { return switches; }

/* ksched_pick, interrupts off: the next thread is of proc */
void kproc_thread(int proc) {
  if (!side.on)
    return;
  int want = proc == side.proc;
  if (want != loaded)
    switch_to(want, 1);
}

/* ksched_dispatch: a handler of proc runs in its process: the side's in the
   side's, the others in the front's (the kernel's too: the VM's vblank
   works on the front's VM). The kernel's state of a program with it: what a
   handler calls of the kernel may use it. 1: switched, kproc_handler_leave
   after it. */
int kproc_handler_enter(int proc) {
  if (!side.on)
    return 0;
  int want = proc && proc == side.proc;
  if (want == loaded)
    return 0;
  switch_to(want, 0);
  return 1;
}
void kproc_handler_leave(void) { switch_to(!loaded, 0); }

/* exec_park, in the side's thread under its ASID, its kernel state saved in
   state (in its core): the live state becomes the front's new one, as
   linked, and the side's part of it its own again at once, the thread going
   on. */
void kproc_side_begin(int proc, void *state) {
  int was = mx_di();
  side.on = 1, side.proc = proc;
  side.asid = mx__asid, side.core_off = mx__core_off;
  side.state = state;
  end_reset_state();
  end_state_swap(side.state);
  front_asid = 0, front_core_off = 0;
  memset(&vu_front, 0, sizeof vu_front);
  loaded = 1;
  mx_irq_restore(was);
}

/* exec_unpark and exec_park_drop, in the command thread (the front's space):
   the side's threads held (ksched_proc_hold) until it is the one in front
   again, its VU0 registers live. */
void kproc_side_end(void) {
  if (!side.on)
    return;
  int was = mx_di();
  ksched_proc_hold(side.proc, 1);
  if (loaded)
    switch_to(0, 1);
  kvu0_load(&vu_side);
  side.on = 0;
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
