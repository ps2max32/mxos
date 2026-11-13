/* Programs: the ELF read through the IOP's file server and put in place by
   the EE (maxsdk's loader/mxload.c, which this replaces), where ps2link has
   the IOP do it (SifLoadElf), then started in a thread of its own, as
   ps2link starts one.

   Segments in maxsdk's VM (a program linked with ld/maxsdk-vm.ld) are left
   to the program's start-up, which reads them from argv[0]; such a program
   gets MX_LOADER_MAGIC in place of ps2link's pid (<mx/loader.h>), so it
   knows it may. Every other program gets ps2link's argument block as it
   is: ps2sdk programs run as under ps2link. */

#include <mx/bios.h>
#include <mx/debug.h>
#include <mx/iop.h>
#include <mx/kernel.h>
#include <mx/loader.h>
#include <mx/proc.h>
#include <mx/sif.h>
#include <mx/string.h>
#include <mx/thread.h>
#include <mx/timer.h>
#include <mx/vm.h>

#include "ksched.h"
#include "kreplace.h"
#include "mxlink.h"

struct ehdr {
  u8 ident[16];
  u16 type, machine;
  u32 version, entry, phoff, shoff, flags;
  u16 ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};

struct phdr {
  u32 type, offset, vaddr, paddr, filesz, memsz, flags, align;
};

/* EE kernel CreateThread parameters */
struct thread_param {
  int status;
  void *func;
  void *stack;
  int stack_size;
  void *gp;
  int initial_priority;
  int current_priority;
  u32 attr, option;
};

#define PT_LOAD 1
#define PT_NOTE 4
#define PF_X 1
#define MAX_PHDRS 16
#define LOW 0x00100000u
#define TOP 0x02000000u
/* below LOW: loaders (ps2link, mxlink itself at 0x88000), above the EE
   kernel; started by ExecPS2 */
#define LOADER_LOW 0x00088000u
#define USER_PRIO 64

static int user_tid; /* 0: no program */
/* the running program is on libmxk (it called program()): it can end
   without the restart (end.c) */
static int on_libmxk;
/* its core at a run of its own (step 2b, core_place) */
static int core_moved;
int exec_on_libmxk(void) { return on_libmxk; }
static struct mx_loader_args largs;
static u8 user_stack[16 * 1024] __attribute__((aligned(16)));

static int shell, next_shell;

int exec_running(void) { return user_tid != 0; }
int exec_in_shell(void) { return user_tid && shell; }
void exec_shell_next(void) { next_shell = 1; }
/* exec_elf decided on a relaunch: was it to start the shell (main.c keeps it over the restart) */
int exec_shell_was(void) { return shell; }
/* What mxlink takes: ee/resident.ld says, mxlink-high (maxsdk's script) is
   its code up to its stack's end. */
void exec_self(u32 *start, u32 *end) {
  extern char __self_start[] __attribute__((weak)), __self_end[] __attribute__((weak));
  extern char __text_start[], __stack[], __stack_size[];
  *start = (u32)(uintptr_t)(__self_end ? __self_start : __text_start);
  *end = __self_end ? (u32)(uintptr_t)__self_end
                    : (u32)(uintptr_t)__stack + (u32)(uintptr_t)__stack_size;
}

int exec_resident(void) {
  u32 start, end;
  exec_self(&start, &end);
  return end <= LOW;
}

/* The program ended without the restart (main.c): its thread is gone. */
static void kvm_front_gone(void);
void exec_ended(void) {
  kvm_front_gone();
  exec_park_forget();
  core_moved = 0;
  user_tid = 0;
  on_libmxk = 0;
}

void exec_kill(void) {
  if (!user_tid)
    return;
  mx_bios_terminate_thread(user_tid);
  mx_bios_delete_thread(user_tid);
  user_tid = 0;
}

/* Step 2b (docs/mx-os-plan.md): a libmxk program of version 9 with VM has its
   core at a physical run of its own, under its ASID: [LOW, core_end) at LOW +
   mx__core_off, the top of main memory; the VM's frames are the memory below
   it, from LOW. CORE_HEAP: the core heap after the program (the VM's tables,
   the physical allocator); MIN_FRAMES: less than that left, the program runs
   at its own addresses. */
/* the ASIDs given out, in turn (1-255; 0 is the kernel's and the programs'
   not on libmxk). Stale entries of an earlier one are gone: the TLB is the
   BIOS's again at each end (tlb_restore). */
static u32 last_asid;
#define CORE_HEAP (256u << 10)
#define MIN_FRAMES (4u << 20)
#define MIB (1u << 20)
static u32 core_end, frames_hi;
static u32 core_slots; /* the TLB entries of its core (tlb_core_map) */

/* A libmxk program's notes (maxsdk's src/base/kstubs.c, <mx/loader.h>'s
   MX_CORE_HEAP): where its mx__core_off is, if it was built for a kernel of
   version 9 or later, and the core heap it asks for (0: none). */
static u32 *mxk_note(int fd, const struct phdr *ph, u32 n, u32 *heap, u32 *version) {
  u32 *at = 0;
  *heap = *version = 0;
  for (u32 i = 0; i < n; i++) {
    const struct phdr *p = &ph[i];
    u32 w[16];
    if (p->type != PT_NOTE || p->filesz < 16 || p->filesz > sizeof w ||
        mx_fio_lseek(fd, (s32)p->offset, MX_FIO_SEEK_SET) != (int)p->offset ||
        mx_fio_read(fd, w, p->filesz) != (int)p->filesz)
      continue;
    /* notes of name "MXK": {namesz 4, descsz, type, "MXK\0", desc} */
    for (u32 k = 0; k + 4 <= p->filesz / 4;) {
      u32 dw = (w[k + 1] + 3) / 4;
      if (w[k] != 4 || k + 4 + dw > p->filesz / 4)
        break;
      const u32 *d = &w[k + 4];
      if (!memcmp(&w[k + 3], "MXK", 4)) {
        if (w[k + 2] == MXK_NOTE_TYPE && w[k + 1] == 8 && d[0] >= 9 && d[1] >= LOW && d[1] < TOP &&
            !(d[1] & 3)) {
          at = (u32 *)(uintptr_t)d[1];
          *version = d[0];
        }
        else if (w[k + 2] == MXK_NOTE_HEAP && w[k + 1] == 4)
          *heap = d[0];
      }
      k += 4 + dw;
    }
  }
  return at;
}

/* The core's run: the ASID set and the TLB written, before the load. 0, or
   -1: the program at its own addresses. */
static u32 core_phys; /* where its run is */
static int core_vm;    /* the program has VM segments */
static u32 mem_top(void);
/* phase 3a (below, exec_park): parked programs with VM, and the core of
   the one in front when it is frames of the VM's pool */
static int vm_kept;
static void *core_pin;
static u32 core_pin_bytes;

static int core_place(u32 end, u32 heap) {
  u32 va_end = (end + heap + MIB - 1) & ~(MIB - 1), run = va_end - LOW;
  if (va_end > TOP)
    return -1;
  u32 phys;
  if (vm_kept) {
    /* a program beside has VM: the core is frames of its pool (phase 3a) */
    void *p = mx__vm_frames_take_aligned(run, MIB);
    if (!p)
      return -1;
    phys = (u32)(uintptr_t)p - MX_PHYS_WIN;
    core_pin = p, core_pin_bytes = run;
  } else {
    u32 top = mem_top();
    if (top < LOW + MIN_FRAMES + run)
      return -1;
    phys = top - run;
  }
  last_asid = last_asid % 255 + 1;
  mx__asid_set(last_asid);
  mx__core_off = phys - LOW;
  int n = tlb_core_map(va_end, mx__core_off);
  if (n >= 0)
    mx__vm_tlb_fix(); /* the VM on (another's): not its ring's any more */
  if (n < 0) {
    tlb_restore();
    if (core_pin)
      mx__vm_frames_give(core_pin, core_pin_bytes);
    core_pin = 0, core_pin_bytes = 0;
    return -1;
  }
  core_slots = (u32)n;
  core_moved = 1;
  core_end = va_end;
  core_phys = frames_hi = phys;
  return 0;
}

/* ---- step 2c: a program waits for one it started ---------------------------

   mx_loader_launch from a program whose core is at a run of its own and
   without VM (mxshell): it stays. Its other threads are suspended and its
   interrupt handlers taken from the BIOS (end_park), the kernel's state of it
   kept in its own core heap; the other's run goes below its own. At the
   other's end it is mapped again, its kernel state put back, its handlers
   added again, and its call returns the other's exit status. */
/* A stack of them (since 2026-11-12): a service at the bottom, and
   one above it that waits for or runs beside the program in front. */
#define MAX_PARK 2
struct park_slot {
  int on, tid, shell, beside;
  int proc;   /* its process */
  int waiter; /* a thread waiting for its end or its start (0: none) */
  int child;  /* the process of the program it started, once it runs; -1:
                 it did not start (a service's spawn) */
  int service; /* it stays beside every program in front (proc_service) */
  u32 asid, core_off, core_end, core_phys, frames_hi, core_slots;
  struct mx_loader_args largs;
  u8 *state; /* the kernel's state of it, in its core heap */
  struct vm_space *space; /* its VM's address space (vm.c), 0: none */
  void *core_pin;         /* its core, when frames of the VM's pool */
  u32 core_pin_bytes;
};
static struct park_slot parks[MAX_PARK];
static int npark;
/* the one the program in front was started by (parks[0] when none waits) */
#define park parks[npark ? npark - 1 : 0]
/* the last one back in front (exec_unpark), for its waiting thread */
static int park_done, park_status;

/* ---- VM programs side by side (mx OS plan, phase 3a step 3) ----------------------

   A program with VM may wait for or run beside another: its address space
   (vm.c) stays, the VM's state is not set back (end.c, exec_vm_kept), and
   the next program's mx_vm_init makes it a space of its own on the same
   frames and swap file (kvm_init); its core is a run of those frames,
   pinned (core_place). Its end gives both back (kvm_front_gone). */
static struct vm_space *front_space; /* the program in front's, while vm_kept */
int exec_vm_kept(void) { return vm_kept; }
struct vm_space *exec_front_space(void) { return front_space; }
static struct vm_space *parked_space(void) {
  for (int i = npark - 1; i >= 0; i--)
    if (parks[i].space)
      return parks[i].space;
  return 0;
}
/* the program in front is gone (its mx_exit, a crash, a kill): its space and
   core back to the VM, the one below current */
static void kvm_front_gone(void) {
  struct vm_space *s = parked_space();
  if (front_space && s) {
    mx__vm_space_switch(s);
    mx__vm_space_free(front_space);
  }
  front_space = 0;
  if (core_pin)
    mx__vm_frames_give(core_pin, core_pin_bytes);
  core_pin = 0, core_pin_bytes = 0;
}
static int kvm_init(const struct mx_vm_config *c) {
  if (!vm_kept)
    return mx_vm_init(c);
  if (front_space)
    return -1; /* on already */
  /* with a swap file (its own or MXOS's) its VM is the size it asks for,
     128 MiB if it does not say; without one, the first space's (the frames) */
  struct vm_space *s = mx__vm_space_new(c->swap[0] ? (c->size ? c->size : 128u << 20) : c->size);
  if (!s)
    return -1;
  mx__vm_space_limit(s, c->frames); /* its frames: at most what it asks for */
  mx__vm_space_switch(s);
  front_space = s;
  /* its swap file, if the VM has none yet (the one that turned it on) */
  if (mx__vm_swap_open(c->swap) < 0) {
    kvm_front_gone();
    return -1;
  }
  return 0;
}
static void kvm_shutdown(void) {
  if (vm_kept)
    kvm_front_gone(); /* the VM goes on, the others' */
  else
    mx__vm_shutdown();
}

/* the program's core heap (maxsdk's src/vm/heap.c, the kernel's copy) */
void *mx__phys_alloc(u32 bytes, u32 align);

static u32 mem_top(void) {
  return npark ? park.core_phys : MX_MAIN_BYTES & ~(MIB - 1);
}
int exec_parked(void) { return npark; }
/* the physical memory of the program that ends (sound_off): all from 1 MiB
   up but the cores of those that wait or run beside it, at the top */
void exec_end_range(u32 *lo, u32 *hi) {
  *lo = LOW;
  *hi = npark ? park.core_phys : MX_MAIN_BYTES;
}

/* the program a parked one started (proc_spawn, proc_wait; kernel version
   14): its process once it runs, and the last one's end */
static int last_child, last_child_status;
/* before a start: its process, or -1, comes in park.child (exec_park_started,
   exec_park_start_failed); a failed start's -1 is not the next one's */
/* the caller's own (a thread of a parked program: the service's may ask
   after the one above it parked too), else the last */
static struct park_slot *own_park(void) {
  int proc = ksched_thread_proc(mx_bios_get_thread_id());
  for (int i = npark - 1; i >= 0; i--)
    if (parks[i].proc == proc)
      return &parks[i];
  return &park;
}
void exec_park_wait_set(void) {
  struct park_slot *q = own_park();
  q->waiter = mx_bios_get_thread_id();
  if (q->child < 0)
    q->child = 0;
}
void exec_park_wait_clear(void) {
  int was = mx_di();
  struct park_slot *q = own_park();
  if (q->waiter == mx_bios_get_thread_id())
    q->waiter = 0;
  mx_irq_restore(was);
}
int exec_park_child(void) { return npark ? own_park()->child : 0; }
/* the command thread, the program started */
void exec_park_started(void) {
  park.child = end_proc_front();
  if (park.waiter)
    mx_bios_wakeup_thread(park.waiter);
}

/* ---- a service (kernel version 18): it stays beside each program in front -- */

void exec_park_service_set(void) { park.service = 1; }
/* the program in front was started by the service (none waits above it) */
int exec_park_service(void) { return npark && park.service; }
/* in the service's threads: the one beside is the caller */
int exec_service_caller(void) {
  int caller = ksched_thread_proc(mx_bios_get_thread_id());
  for (int i = 0; i < npark; i++)
    if (parks[i].service && parks[i].proc == caller)
      return 1;
  return 0;
}
/* the command thread: the service's program did not start */
void exec_park_start_failed(void) {
  park.child = -1;
  if (park.waiter)
    mx_bios_wakeup_thread(park.waiter);
}
/* the command thread: the program in front ended, the service stays */
void exec_front_ended(int status) {
  kvm_front_gone(); /* also when it did not end by its mx_exit (a kill, a crash) */
  last_child = park.child, last_child_status = status;
  if (park.child > 0) {
    struct mx_event e = {MX_EVENT_END, park.child, status, 0};
    kmsg_post(park.proc, &e, sizeof e);
  }
  park.child = 0;
  if (park.waiter)
    mx_bios_wakeup_thread(park.waiter);
}
/* 1: ended, its status in *status; 0: it runs (nohang); -1: not a program
   this one started */
int kproc_wait(int id, int *status, int nohang) {
  if (id <= 0)
    return -1;
  if (npark && park.child == id) {
    if (nohang)
      return 0;
    int was = mx_di();
    park.waiter = mx_bios_get_thread_id();
    mx_irq_restore(was);
    while (npark && park.child == id)
      mx_bios_sleep_thread();
  }
  if ((!npark || park.service) && last_child == id) {
    if (status)
      *status = last_child_status;
    return 1;
  }
  return -1;
}

/* <mx/proc.h>'s list (kernel version 13): the program in front and those
   that wait for it or run beside it */
static void proc_name(char *to, const char *path) {
  u32 n = (u32)strlen(path), max = sizeof((struct mx_proc_info *)0)->name - 1;
  if (n > max)
    path += n - max, n = max; /* its end: the file's name */
  memcpy(to, path, n);
  to[n] = 0;
}
int kproc_list(struct mx_proc_info *p, int max) {
  int n = 0;
  int front = end_proc_front();
  if (user_tid && front > 0) {
    if (n < max) {
      struct mx_proc_info *i = &p[n];
      memset(i, 0, sizeof *i);
      i->id = (u8)front, i->state = MX_PROC_FRONT;
      if (core_moved)
        i->core_lo = core_phys, i->core_hi = core_phys + (core_end - LOW);
      proc_name(i->name, status_program());
    }
    n++;
  }
  for (int k = npark - 1; k >= 0; k--) {
    const struct park_slot *q = &parks[k];
    if (q->proc <= 0)
      continue;
    if (n < max) {
      struct mx_proc_info *i = &p[n];
      memset(i, 0, sizeof *i);
      i->id = (u8)q->proc, i->state = q->beside ? MX_PROC_BESIDE : MX_PROC_WAITING;
      i->core_lo = q->core_phys, i->core_hi = q->core_phys + (q->core_end - LOW);
      proc_name(i->name, parked_program(k));
    }
    n++;
  }
  return n;
}
int kproc_self(void) { return ksched_thread_proc(mx_bios_get_thread_id()); }
int exec_park_done(void) { return park_done; }
int exec_park_status(void) { return park_status; }

/* in the program's thread: 0, it waits now; -1, it cannot. beside (step
   2e-2, kproc.c): its other threads go on beside the other program, its
   core's TLB entries kept. */
int exec_park(int beside) {
  /* one without VM waits above another only beside: it crashed at once
     (its stack in the next one's core) and takes the restart's way; one
     with VM waits above a service as alone (its core pinned frames) */
  if (npark == MAX_PARK || (npark && !beside && !core_vm) || !core_moved || !on_libmxk ||
      !end_tables_known())
    return -1;
  struct park_slot *q = &parks[npark];
  if (!q->state)
    q->state = mx__phys_alloc(end_state_size(), 64);
  if (!q->state || end_park(mx_bios_get_thread_id(), beside) < 0)
    return -1;
  q->tid = user_tid, q->shell = shell, q->beside = beside;
  /* its VM stays: not saved or set back with the rest (end.c) */
  q->space = core_vm && mx_vm_active() ? mx__vm_space_now() : 0;
  vm_kept += q->space != 0;
  front_space = 0;
  q->core_pin = core_pin, q->core_pin_bytes = core_pin_bytes;
  core_pin = 0, core_pin_bytes = 0;
  q->proc = end_proc_front();
  q->asid = mx__asid, q->core_off = mx__core_off, q->core_end = core_end;
  q->core_phys = core_phys, q->frames_hi = frames_hi, q->core_slots = core_slots;
  q->largs = largs;
  end_state_save(q->state);
  park_done = 0, q->waiter = 0, q->child = 0, q->service = 0;
  q->on = 1;
  npark++;
  /* the next program starts with the kernel's state as linked; beside, it
     goes on with its own part of it (kproc_side_begin) */
  if (beside) {
    kproc_side_begin(ksched_thread_proc(mx_bios_get_thread_id()), q->state, core_slots, core_end,
                     q->space);
  } else
    end_reset_state();
  user_tid = 0, shell = 0, on_libmxk = 0, core_moved = 0;
  return 0;
}

/* in the command thread, the other ended (or did not start: status -1): the
   one it was started by is in front again */
void exec_unpark(int status) {
  kvm_front_gone();
  int side = 0;
  if (park.beside) {
    side = kproc_side();
    kproc_side_end(); /* its threads held until it is in front again */
  }
  mx__asid_set(park.asid);
  mx__core_off = park.core_off;
  int n = tlb_core_map(park.core_end, park.core_off);
  if (n >= 0)
    mx__vm_tlb_fix();
  core_slots = n < 0 ? 0 : (u32)n;
  if (n < 0) {
    /* cannot be: it was mapped so before */
    mx__core_off = 0;
    mxlink_restart();
  }
  end_state_load(park.state);
  mx__pad_forget(); /* its ports were closed: its next mx_pad_init opens them */
  user_tid = park.tid, shell = park.shell, on_libmxk = 1, core_moved = 1;
  core_vm = park.space != 0;
  core_end = park.core_end, core_phys = park.core_phys, frames_hi = park.frames_hi;
  largs = park.largs;
  /* its address space in front again */
  if (park.space) {
    mx__vm_space_switch(park.space);
    front_space = park.space;
    vm_kept--;
    if (!vm_kept)
      front_space = 0; /* the VM is its alone again */
  }
  core_pin = park.core_pin, core_pin_bytes = park.core_pin_bytes;
  if (!park.beside)
    end_unpark();
  else
    end_unpark_beside();
  park.on = 0;
  int child = park.child, waiter = park.waiter;
  npark--;
  park_status = status;
  park_done = 1;
  last_child = child, last_child_status = status;
  if (child) {
    /* its end to this one's event port (kernel version 15) */
    struct mx_event e = {MX_EVENT_END, child, status, 0};
    kmsg_post(end_proc_front(), &e, sizeof e);
  }
  if (side)
    ksched_proc_hold(side, 0);
  if (waiter)
    mx_bios_wakeup_thread(waiter);
}

/* a program ended: what was kept in its heap is gone with it (the slots
   above those in use) */
void exec_park_forget(void) {
  for (int i = npark; i < MAX_PARK; i++)
    parks[i].state = 0;
}

/* the other takes all memory: the waiting one ends with it. Only one that
   waits alone: one above a service cannot give the service's memory */
int exec_park_drop(void) {
  if (npark != 1 || vm_kept)
    return -1;
  if (park.beside)
    kproc_side_end(); /* held: its threads end with the other's */
  end_drop();
  park.on = 0;
  park.state = 0;
  npark = 0;
  return 0;
}

static int fail(int fd, const char *path, const char *what) {
  if (core_moved) {
    core_moved = 0;
    tlb_restore();
  }
  if (fd >= 0)
    mx_fio_close(fd);
  con_log(CON_RED, "%s: %s", path, what);
  return -1;
}

/* argv from ps2link's packet: argc strings one after the other */
static void set_args(int argc, const char *args, u32 len) {
  struct mx_args *a = &largs.args;
  if (len > sizeof a->payload)
    len = sizeof a->payload;
  memcpy(a->payload, args, len);
  a->payload[sizeof a->payload - 1] = 0;
  if (argc > MX_MAX_ARGS)
    argc = MX_MAX_ARGS;
  u32 t = 0;
  int i = 0;
  while (i < argc && t < sizeof a->payload) {
    a->argv[i++] = &a->payload[t];
    t += strlen(&a->payload[t]) + 1;
  }
  a->argc = i;
}

static const char *next_swap;
void exec_swap(const char *swap) { next_swap = swap; }

void mx__sif_waits(u32 out[4]);
void mx__exception_vector(void);
extern u32 mx__text_lo, mx__text_hi;
extern const char *mx__loader_swap;
void mx__fio_base(const char *argv0);

/* a libmxk program's text for its crash report (mx_exception_init) */
static const char **program_note;
const char *const *exec_note(void) { return program_note; }
/* Called by every libmxk program: its SIF is the kernel's, so the kernel's
   reports may go to the tty too. note: 0, the VM being the kernel's (a
   program of version 3 has its own). */
void mx__vm_handlers_again(void);
static void exception_setup(const char **note) {
  program_note = note;
  mx_exception_init();
  /* a program beside has VM (phase 3a): its handlers in front again */
  if (vm_kept)
    mx__vm_handlers_again();
  mx_debug_sinks(MX_SINK_SIO | MX_SINK_TTY);
}

static void kernel_program(void *heap_lo, void *heap_hi, void *vm_lo, void *vm_hi) {
  on_libmxk = 1;
  /* the swap file's size (plan 3c), before the program's mx_vm_init */
  u32 swap_bytes = 0;
  if (!vm_kept && kswap_path(&swap_bytes))
    mx__vm_swap_size(swap_bytes);
  if (core_moved) {
    /* its ASID and core run since the load: the core heap ends with the
       run, the frames are below it */
    mx__heap_range(heap_lo, (void *)(uintptr_t)core_end);
    if (!vm_kept) /* else the frames are the VM's already, its core among them */
      mx__vm_frames(LOW, frames_hi);
  } else {
    last_asid = last_asid % 255 + 1;
    mx__asid_set(last_asid);
    mx__heap_range(heap_lo, heap_hi);
  }
  mx__vm_image_range(vm_lo, vm_hi);
}

static void kernel_heaps(struct mx_heap *h[3]) {
  h[0] = mx_heap_pinned, h[1] = mx_heap_stream, h[2] = mx_heap_paged;
}

static int kmem_read(u32 phys, void *to, u32 n) { return kmem_copy(phys, to, n, 0); }
static int kmem_write(u32 phys, const void *from, u32 n) {
  return kmem_copy(phys, (void *)(uintptr_t)from, n, 1);
}

/* the kernel's services for libmxk (<mx/kernel.h>) */
static const char *program_swap;
static const char *kernel_swap(void) { return program_swap; }
static const struct mxk_services kernel = {
    .magic = MX_KERNEL_MAGIC,
    .version = MX_KERNEL_VERSION,
    .size = sizeof kernel,
    .exit = mxlink_return,
    .launch = mxlink_launch,
    .swap = kernel_swap,
    .sif_bind_tries = mx_sif_bind_tries,
    .sif_bound = mx_sif_bound,
    .sif_call = mx__sif_call,
    .sif_call_async = mx_sif_call_async,
    .sif_wait = mx_sif_wait,
    .sif_serve = mx_sif_serve,
    .iop_read = mx_iop_read,
    .iop_reset = mx_iop_reset,
    .sif_waits = mx__sif_waits,
    .exception_vector = mx__exception_vector,
    .exception_setup = exception_setup,
    .crash = mxlink_crash,
    .program = kernel_program,
    .heaps = kernel_heaps,
    .mx__core_fault = mx__core_fault,
    .mx__paddr_fault = mx__paddr_fault,
    .mx_tty_write = klog_tty_write,
#define MXK_ENTRY(ret, name, params, args) .name = name,
    MXK_CALLS(MXK_ENTRY)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winitializer-overrides"
    /* VM programs side by side (phase 3a): through the kernel's own */
    .mx_vm_init = kvm_init,
    .mx__vm_shutdown = kvm_shutdown,
#pragma clang diagnostic pop
    .fio_ending = mx__fio_ending,
    .pad_init = pad_init,
    .pad_read = pad_read,
    .gs_display = home_gs_display,
    .gs_mode = home_gs_mode,
    .crtc_set = kvideo_crtc_set,
    .iop_write = mx__iop_write_phys,
    .board_offset = kvideo_board_offset,
    .home_wanted = home_wanted,
    .home_run = home_run,
    .irq_tables = end_irq_tables,
    .kthreads = &ksched_kthreads,
    .pad_rumble = pad_rumble,
    .launch_beside = mxlink_launch_beside,
    .proc_self = kproc_self,
    .proc_list = kproc_list,
    .port_create = kmsg_port_create,
    .port_find = kmsg_port_find,
    .port_delete = kmsg_port_delete,
    .msg_send = kmsg_send,
    .msg_receive = kmsg_receive,
    .msg_reply = kmsg_reply,
    .proc_spawn = kproc_spawn,
    .proc_wait = kproc_wait,
    .proc_kill = kproc_kill,
    .sleep_us = mx_sleep_us,
    .proc_events = kmsg_events,
    .log_read = klog_read,
    .iop_channel = kmsg_iop_channel,
    .proc_service = kproc_service,
    .timer_add = mx_timer_add,
    .timer_remove = mx_timer_remove,
    .timer_claim = mx_timer_claim,
    .timer_release = mx_timer_release,
    .timer_epc = mx_timer_epc,
    .phys_read = kmem_read,
    .phys_write = kmem_write,
};

/* Another kernel in place of this one (mx OS plan 2h-2; before, a new kernel
   was tried as mxkernel-high, a loader started at 0x1E0_0000): its segments
   are read to KREPLACE_STAGE, a table of them and kreplace.S's trampoline go
   to KREPLACE_AT, and ExecPS2 starts the trampoline there with the
   arguments, which copies them over this kernel and starts the new one. */
static int exec_replace(int fd, const char *path, const struct ehdr *eh, const struct phdr *ph) {
  extern char kreplace_tramp[], kreplace_tramp_end[];
  /* the BIOS's mappings, ASID 0: after a program with VM the frames' cached
     entries are gone, and the stage and the trampoline are in frames (a
     replace after k44 crashed so, and the old kernel went on) */
  tlb_restore();
  volatile u32 *t = (volatile u32 *)(uintptr_t)KREPLACE_TABLE;
  u32 n = 0, stage = KREPLACE_STAGE, bytes = 0;
  for (u32 i = 0; i < eh->phnum; i++) {
    const struct phdr *p = &ph[i];
    if (p->type != PT_LOAD || p->memsz == 0)
      continue;
    if (n == KREPLACE_MAX)
      return fail(fd, path, "too many segments for a kernel");
    if (p->filesz && (mx_fio_lseek(fd, (s32)p->offset, MX_FIO_SEEK_SET) != (int)p->offset ||
                      mx_fio_read(fd, (void *)(uintptr_t)stage, p->filesz) != (int)p->filesz))
      return fail(fd, path, "cannot read a segment");
    t[2 + 4 * n] = p->vaddr, t[3 + 4 * n] = stage, t[4 + 4 * n] = p->filesz;
    t[5 + 4 * n] = p->memsz;
    stage = (stage + p->filesz + 63) & ~63u;
    bytes += p->memsz;
    n++;
  }
  mx_fio_close(fd);
  t[0] = n, t[1] = eh->entry;
  memcpy((void *)(uintptr_t)KREPLACE_AT, kreplace_tramp, (u32)(kreplace_tramp_end - kreplace_tramp));
  con_log(CON_YELLOW, "  a kernel (%u KiB, entry %08x): it replaces this one", bytes >> 10, eh->entry);
  kirq_off();
  mx_debug_flush();
  tlb_writable();
  mx__sif_dma_give();
  mx_dcache_flush_all();
  mx_icache_invalidate_all();
  mx_bios_exec_ps2((void *)KREPLACE_AT, 0, largs.args.argc, largs.args.argv);
  return fail(-1, path, "ExecPS2 returned");
}

int exec_elf(int argc, const char *args, u32 args_len) {
  if (user_tid) {
    con_log(CON_RED, "a program is running already");
    return -1;
  }
  shell = next_shell;
  next_shell = 0;
  set_args(argc < 1 ? 1 : argc, args, args_len);
  const char *path = largs.args.argv[0];
  if (exec_returns())
    con_log(CON_WHITE, "exec %s (%u returned before, the last with status %d)", path,
            exec_returns(), (int)exec_last_status());
  else
    con_log(CON_WHITE, "exec %s", path);
  /* a hang's report (main.c), with the host listening: its next program */
  if (!strncmp(path, "host:", 5))
    hang_log();

  static struct ehdr eh;
  static struct phdr ph[MAX_PHDRS];
  int fd = mx_fio_open(path, MX_FIO_RDONLY);
  if (fd < 0)
    return fail(-1, path, "cannot open");
  if (mx_fio_read(fd, &eh, sizeof eh) != sizeof eh || eh.ident[0] != 0x7f ||
      eh.ident[1] != 'E' || eh.ident[2] != 'L' || eh.ident[3] != 'F' ||
      eh.machine != 8 /* MIPS */)
    return fail(fd, path, "not a MIPS ELF");
  if (eh.phentsize != sizeof(struct phdr) || eh.phnum == 0 || eh.phnum > MAX_PHDRS)
    return fail(fd, path, "unexpected program headers");
  u32 phbytes = eh.phnum * sizeof(struct phdr);
  if (mx_fio_lseek(fd, (s32)eh.phoff, MX_FIO_SEEK_SET) != (int)eh.phoff ||
      mx_fio_read(fd, ph, phbytes) != (int)phbytes)
    return fail(fd, path, "cannot read the program headers");

  /* check all before loading any: a bad one leaves memory as it was */
  u32 self_start, self_end;
  exec_self(&self_start, &self_end);
  int vm = 0, loader = 0, over_self = 0;
  for (u32 i = 0; i < eh.phnum; i++) {
    const struct phdr *p = &ph[i];
    if (p->type != PT_LOAD || p->memsz == 0)
      continue;
    if (p->vaddr >= MX_VM_BASE && p->vaddr < MX_VM_LIMIT) {
      vm++;
      continue;
    }
    if (p->vaddr >= LOADER_LOW && p->vaddr < LOW)
      loader = 1;
    if (p->filesz > p->memsz || p->vaddr < LOADER_LOW || p->vaddr + p->memsz > TOP ||
        p->vaddr + p->memsz < p->vaddr) {
      con_log(CON_RED, "segment %u at %08x, %u bytes: not in %08x-%08x", i, p->vaddr, p->memsz,
              LOADER_LOW, TOP);
      return fail(fd, path, "a segment is not where the kernel can load it");
    }
    /* not over mxlink itself (make high: at 0x1E00000, in the programs'
       memory), its stack included: starting mxlink-high.elf from it hung
       the PS2 */
    if (p->vaddr < self_end && p->vaddr + p->memsz > self_start) {
      if (p->vaddr < LOW) {
        over_self = 1; /* below 1 MiB: another kernel, which replaces this one */
        continue;
      }
      con_log(CON_RED, "segment %u at %08x, %u bytes: over the kernel at %08x-%08x", i, p->vaddr,
              p->memsz, self_start, self_end);
      return fail(fd, path, "it would overwrite the kernel");
    }
  }
  if (over_self && !vm) {
    /* with a service beside or a VM kept, the replace would take the TLB
       entry the VM's tables are read through (MX_PHYS_WIN) and stage the
       new kernel in the VM's frames: the old kernel crashed at the next
       vblank. The kernel restarts first and starts this again, before any
       service: nothing beside, no VM */
    if (exec_parked() || vm_kept) {
      mx_fio_close(fd);
      con_log(CON_YELLOW, "  a kernel: first the restart, without the services");
      mxlink_takeover(argc, args, args_len);
    }
    return exec_replace(fd, path, &eh, ph);
  }
  /* mx OS plan 2h-2: the kernel runs MXOS programs only (libmxk, kernel
     version 10 or later), and other kernels (above); a standalone ELF (libmx,
     ps2sdk's, a loader such as ps2link) is started by mxboot or a legacy
     loader instead */
  if (loader)
    return fail(fd, path, "a loader (below 1 MiB): not an MXOS program");
  /* step 2b: a libmxk program of version 9 with VM, its core at a physical
     run of its own (core_place), from here, so that the load goes there */
  u32 *core_off_at = 0, heap = 0, version = 0;
  core_moved = 0;
  core_vm = vm;
  core_off_at = mxk_note(fd, ph, eh.phnum, &heap, &version);
  if (version < 10)
    return fail(fd, path, "not an MXOS program (libmxk): a standalone ELF is started by mxboot or a "
                          "legacy loader");
  if (self_end > LOW)
    core_off_at = 0;
  int note_v9 = core_off_at != 0;
  if (core_off_at && !vm && !heap)
    core_off_at = 0; /* without VM: all memory, unless it asks for a core heap */
  if (core_off_at) {
    u32 end = LOW;
    for (u32 i = 0; i < eh.phnum; i++) {
      const struct phdr *p = &ph[i];
      if (p->type == PT_LOAD && p->memsz && !(p->vaddr >= MX_VM_BASE && p->vaddr < MX_VM_LIMIT) &&
          p->vaddr + p->memsz > end)
        end = p->vaddr + p->memsz;
    }
    if ((u32)(uintptr_t)core_off_at + 4 > end || core_place(end, vm ? CORE_HEAP : heap) < 0)
      core_off_at = 0;
  }
  /* a program waiting for this one (step 2c) is given up when this one
     cannot have a run of its own: it takes all memory */
  int dropped = 0;
  if (!core_off_at && exec_parked()) {
    if (exec_park_drop() < 0)
      return fail(fd, path, vm_kept ? "takes all memory, and a program with VM waits or runs beside"
                                    : "takes all memory, and a service and a program wait");
    con_log(CON_YELLOW, "  %s takes all memory: the program that started it ends", path);
    dropped = 1;
  }
  /* at its own addresses: the BIOS's mappings, ASID 0 (after a program that
     waits, the TLB and the ASID are still that one's; one beside kept its
     core's entries, tlb_core_keep) */
  if (!core_off_at && (mx__asid || mx__core_off || dropped))
    tlb_restore();
  u32 bytes = 0, text_lo = ~0u, text_hi = 0;
  for (u32 i = 0; i < eh.phnum; i++) {
    const struct phdr *p = &ph[i];
    if (p->type == PT_LOAD && (p->flags & PF_X)) {
      text_lo = p->vaddr < text_lo ? p->vaddr : text_lo;
      text_hi = p->vaddr + p->memsz > text_hi ? p->vaddr + p->memsz : text_hi;
    }
    if (p->type != PT_LOAD || p->memsz == 0 || (p->vaddr >= MX_VM_BASE && p->vaddr < MX_VM_LIMIT))
      continue;
    u8 *d = (u8 *)(uintptr_t)p->vaddr;
    if (p->filesz && (mx_fio_lseek(fd, (s32)p->offset, MX_FIO_SEEK_SET) != (int)p->offset ||
                      mx_fio_read(fd, d, p->filesz) != (int)p->filesz))
      return fail(fd, path, "cannot read a segment");
    memset(d + p->filesz, 0, p->memsz - p->filesz);
    bytes += p->memsz;
  }
  mx_fio_close(fd);
  if (core_off_at) {
    *core_off_at = mx__core_off;
    con_log(CON_GREY, "  its core at physical %08x-%08x, ASID %u", LOW + mx__core_off,
            core_end + mx__core_off, mx__asid);
  }
  if (vm)
    con_log(CON_GREY, "  %u KiB loaded, %d VM segment%s left to the program", bytes >> 10, vm,
            vm == 1 ? "" : "s");
  else
    con_log(CON_GREY, "  %u KiB loaded, entry %08x", bytes >> 10, eh.entry);

  mx_dcache_flush_all();
  mx_icache_invalidate_all();


  /* what is there before it: the rest is the program's at its end (end.c) */
  /* on the kernel's threads a process of its own (step 2d-3) */
  int proc = ksched_active() ? ksched_proc_new() : 0;
  end_snapshot(proc);
  on_libmxk = 0;
  kirq_setup_args_clear(); /* SetupThread: not the block of the program before */
  struct thread_param tp = {0};
  tp.func = (void *)(uintptr_t)eh.entry;
  tp.stack = user_stack;
  tp.stack_size = sizeof user_stack;
  tp.initial_priority = USER_PRIO;
  int tid = mx_bios_create_thread(&tp);
  if (tid < 0)
    return fail(-1, path, "cannot create its thread");
  if (proc > 0)
    ksched_set_proc(tid, proc);
  /* ps2link: the address of the thread id; programs with VM segments:
     MX_LOADER_MAGIC, so maxsdk's start-up reads them */
  largs.pid = vm ? MX_LOADER_MAGIC : (u32)(uintptr_t)&user_tid;
  /* the way back for maxsdk programs: only from the resident mxlink (below
     the programs; mxlink-high is in their memory) */
  largs.ret_magic = self_end <= LOW ? MX_LOADER_RET : 0;
  largs.ret = mxlink_return;
  largs.launch_magic = self_end <= LOW ? MX_LOADER_LAUNCH : 0;
  largs.launch = mxlink_launch;
  /* the swap file: its launch's, else MXOS's (mc0:/MXOS/SWAP.CFG, plan 3c) */
  u32 swap_bytes = 0;
  const char *mxos_swap = kswap_path(&swap_bytes);
  largs.swap = program_swap = mx__loader_swap = next_swap ? next_swap : mxos_swap;
  next_swap = 0;
  largs.kernel_magic = self_end <= LOW ? MX_KERNEL_MAGIC : 0;
  largs.kernel = &kernel;

  program_note = 0;
  mx__fio_base(path);
  mx__text_lo = text_lo;
  mx__text_hi = text_hi;
  exec_started(path);
  status_state(shell ? ST_MENU : ST_RUNNING, path);
  /* from here the program owns the IOP's SIF replies and the screen: the
     debug output to the serial port only (con_log) */
  mx_debug_flush();
  mx_debug_sinks(MX_SINK_SIO);
  /* a program not built for this kernel may load another loader over it:
     the BIOS's interrupt table then (step 2d-1) */
  if (!note_v9)
    kirq_off();
  user_tid = tid;
  ksys_watch_set(mx__asid); /* its BIOS syscalls counted (step 2h-3) */
  if (mx_bios_start_thread(tid, &largs) < 0) {
    user_tid = 0;
    mx_bios_delete_thread(tid);
    return fail(-1, path, "cannot start its thread");
  }
  return 0;
}
