/* The kernel's table of processes (mx OS plan, step 4a): one entry for each
   program the kernel started, with its state (in front, waiting for the one
   it started, beside it), whether it is the service, the process that
   started it, and once it ended its exit status. Kept over the kernel's own
   restart. The places where a waiting or beside program's memory and kernel
   state are kept (exec.c's park slots, kproc.c's sides) are not here yet:
   later parts of step 4 move them into the entries. */

#include <mx/proc.h>
#include <mx/string.h>

#include "mxlink.h"

#define KEPT __attribute__((section(".kept")))
#define PTAB_N 8

struct ptab {
  u8 used, state; /* MX_PROC_*, 0: ended */
  u8 service, pad;
  int proc, parent, status;
  u32 seq; /* the order of starts: the oldest ended entry is used again first */
  u32 core_lo, core_hi;
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

void ptab_core(int proc, u32 lo, u32 hi) {
  struct ptab *e = find(proc);
  if (e)
    e->core_lo = lo, e->core_hi = hi;
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
void ptab_end(int proc, int status) {
  struct ptab *e = find(proc);
  if (e && e->state)
    e->state = 0, e->status = status;
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
        i->core_lo = e->core_lo, i->core_hi = e->core_hi;
        memcpy(i->name, e->name, sizeof i->name);
      }
      n++;
    }
  }
  return n;
}
