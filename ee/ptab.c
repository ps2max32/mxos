/* The kernel's table of processes (mx OS plan, step 4a): one entry for each
   program the kernel started, with its state (in front, waiting for the one
   it started, beside it), whether it is the service, the process that
   started it, and once it ended its exit status. Kept over the kernel's own
   restart. The places where a waiting or beside program's memory and kernel
   state are kept (exec.c's park slots, kproc.c's sides) are not here yet:
   later parts of step 4 move them into the entries. */

#include <mx/cpu.h>
#include <mx/proc.h>
#include <mx/string.h>
#include <mx/vm.h>

#include "mxlink.h"

#define KEPT __attribute__((section(".kept")))
#define PTAB_N 8

struct ptab {
  u8 used, state; /* MX_PROC_*, 0: ended */
  u8 service, shell;
  int proc, parent, status;
  u32 seq; /* the order of starts: the oldest ended entry is used again first */
  u32 core_lo, core_hi;
  u32 fast_lo; /* its core's fast part: fast_lo to core_hi */
  u32 space; /* its VM's address space (vm.c's number), 0: none of its own */
  char name[48];
};
static struct ptab tab[PTAB_N] KEPT;
static u32 seq KEPT;

static struct ptab *find(int proc) {
  for (int i = 0; proc > 0 && i < PTAB_N; i++)
    if (tab[i].used && tab[i].proc == proc)
      return &tab[i];
  return 0;
}

/* a program's start: its process (1-255), the one that started it (0: the
   kernel or the host), its path */
void ptab_start(int proc, int parent, const char *path) {
  if (proc <= 0)
    return;
  struct ptab *e = find(proc); /* a number used again (after a restart) */
  for (int i = 0; !e && i < PTAB_N; i++)
    if (!tab[i].used)
      e = &tab[i];
  for (int i = 0; !e && i < PTAB_N; i++) /* the oldest that ended */
    if (!tab[i].state && (!e || tab[i].seq < e->seq))
      e = &tab[i];
  if (!e)
    return; /* all 8 alive: cannot be today (at most 3) */
  memset(e, 0, sizeof *e);
  e->used = 1, e->state = MX_PROC_FRONT;
  e->proc = proc, e->parent = parent;
  e->seq = ++seq;
  u32 n = (u32)strlen(path), max = sizeof e->name - 1;
  if (n > max)
    path += n - max, n = max; /* its end: the file's name */
  memcpy(e->name, path, n);
}

void ptab_core(int proc, u32 lo, u32 fast, u32 hi) {
  struct ptab *e = find(proc);
  if (e)
    e->core_lo = lo, e->fast_lo = fast, e->core_hi = hi;
}
void ptab_space(int proc, u32 space) {
  struct ptab *e = find(proc);
  if (e)
    e->space = space;
}

/* <mx/proc.h>'s mx_proc_memory (kernel version 37; fast: 38): the kernel
   first, then the processes as ptab_list. */
void mx__vm_frames_of(u32 space, const u32 *skip, u32 nskip, u32 *pinned, u32 *paged);
int ptab_memory(struct mx_proc_mem *m, int max) {
  static struct mx_proc_info info[PTAB_N];
  static u32 cores[2 * PTAB_N];
  int np = ptab_list(info, PTAB_N), n = 0;
  if (np > PTAB_N)
    np = PTAB_N;
  u32 ncores = 0;
  for (int i = 0; i < np; i++) {
    struct ptab *e = find(info[i].id);
    if (e && e->core_hi > e->core_lo)
      cores[2 * ncores] = e->core_lo, cores[2 * ncores + 1] = e->core_hi, ncores++;
  }
  struct mx_vm_stats v;
  mx_vm_stats(&v);
  u32 others = 0, kp = 0, kg = 0;
  if (max > 0) {
    memset(&m[0], 0, sizeof m[0]);
    m[0].core = (MX_KERNEL_HI - MX_KERNEL_LO) / MX_VM_PAGE;
    mx__vm_frames_of(0, cores, ncores, &kp, &kg);
    m[0].paged = kg;
    n = 1;
  }
  for (int i = 0; i < np; i++) {
    struct ptab *e = find(info[i].id);
    u32 core = e ? (e->core_hi - e->core_lo) / MX_VM_PAGE : 0, pinned = 0, paged = 0;
    if (e && e->space)
      mx__vm_frames_of(e->space, cores, ncores, &pinned, &paged);
    others += core + pinned;
    if (n < max) {
      m[n].id = info[i].id, m[n].state = info[i].state, m[n].flags = info[i].flags;
      m[n].core = core, m[n].pinned = pinned, m[n].paged = paged;
      m[n].fast = e && e->core_hi > e->fast_lo ? (e->core_hi - e->fast_lo) / MX_VM_PAGE : 0;
      n++;
    }
  }
  if (max > 0)
    m[0].pinned = v.pinned_frames > others ? v.pinned_frames - others : 0;
  return np + 1;
}

void ptab_state(int proc, int state) {
  struct ptab *e = find(proc);
  if (e && e->state)
    e->state = (u8)state;
}
void ptab_service(int proc) {
  struct ptab *e = find(proc);
  if (e)
    e->service = 1;
}
/* kernel version 26: the process the kernel started as its shell */
void ptab_shell(int proc) {
  struct ptab *e = find(proc);
  if (e)
    e->shell = 1;
}
void ptab_end(int proc, int status) {
  struct ptab *e = find(proc);
  if (e && e->state)
    e->state = 0, e->status = status;
}

/* the host's reset: every process ends (the service and the shell start
   again with new entries); the table outlives the restart, so a program
   left in front would block the host link's commands */
void ptab_end_all(int status) {
  for (int i = 0; i < PTAB_N; i++)
    if (tab[i].used && tab[i].state)
      tab[i].state = 0, tab[i].status = status;
}

/* its file's name (without the path), "" for none: a notification's source */
void ptab_name(int proc, char *to, u32 room) {
  struct ptab *e = find(proc);
  to[0] = 0;
  if (!e || !room)
    return;
  const char *n = e->name;
  for (const char *p = e->name; *p; p++)
    if (*p == '/' || *p == ':')
      n = p + 1;
  u32 l = (u32)strlen(n);
  if (l >= room)
    l = room - 1;
  memcpy(to, n, l);
  to[l] = 0;
}

/* 1: proc ended, its status in *status; 0: it runs; -1: not known */
int ptab_status(int proc, int *status) {
  struct ptab *e = find(proc);
  if (!e)
    return -1;
  if (e->state)
    return 0;
  if (status)
    *status = e->status;
  return 1;
}
int ptab_waiting(int proc) {
  struct ptab *e = find(proc);
  return e && e->state == MX_PROC_WAITING;
}
int ptab_parent(int proc) {
  struct ptab *e = find(proc);
  return e ? e->parent : -1;
}

/* <mx/proc.h>'s list: the one in front first, then the others, the last
   started first */
int ptab_list(struct mx_proc_info *p, int max) {
  int n = 0;
  for (int pass = 0; pass < 2; pass++) {
    u32 below = ~0u;
    for (;;) {
      struct ptab *e = 0;
      for (int i = 0; i < PTAB_N; i++) {
        struct ptab *t = &tab[i];
        if (!t->used || !t->state || (t->state == MX_PROC_FRONT) != (pass == 0) || t->seq >= below)
          continue;
        if (!e || t->seq > e->seq)
          e = t;
      }
      if (!e)
        break;
      below = e->seq;
      if (n < max) {
        struct mx_proc_info *i = &p[n];
        memset(i, 0, sizeof *i);
        i->id = (u8)e->proc, i->state = e->state;
        i->flags = (u16)((e->shell ? MX_PROC_SHELL : 0) | (e->service ? MX_PROC_SERVICE : 0));
        i->core_lo = e->core_lo, i->core_hi = e->core_hi;
        memcpy(i->name, e->name, sizeof i->name);
      }
      n++;
    }
  }
  return n;
}
