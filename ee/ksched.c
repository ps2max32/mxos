/* The kernel's threads, semaphores and interrupt handlers (mx OS plan, step
   2d). Every thread of the kernel and of programs built for it runs on one
   BIOS thread, the carrier: the BIOS never switches, the kernel does.
   maxsdk's mx_bios_* calls for threads, semaphores and handlers come here
   (its src/base/ksys.c, mx__kthreads). A thread that waits or gives way
   switches by a syscall (ksched_switch_k); a handler that wakes one of
   higher priority asks for a switch at the end of the kernel's interrupt
   entry (kirq.S, ksched_switch_irq). Both end in kctx_resume.

   Priorities as the BIOS's: lower is first; the ready thread of the lowest
   number runs, in turn among equals. The idle thread (below all) spins
   when nothing is ready. */

#include <mx/bios.h>
#include <mx/debug.h>
#include <mx/cpu.h>
#include <mx/string.h>

#include "ksched.h"
#include "mxlink.h"

#define NTHREADS 24
#define NSEMAS 128
#define NHANDLERS 32
#define IDLE_PRIO 255

/* the BIOS's thread status values (ReferThreadStatus) */
#define THS_RUN 0x01
#define THS_READY 0x02
#define THS_WAIT 0x04
#define THS_SUSPEND 0x08
#define THS_DORMANT 0x10

enum { T_FREE, T_DORMANT, T_READY, T_RUN, T_SLEEP, T_SEMA };

struct kth {
  struct kctx c; /* first: ksched_cur points at it */
  u8 state, suspended, prio, init_prio;
  s16 sema;      /* waited for */
  s16 result;    /* WaitSema's: its id, or -1 when it was deleted */
  u32 seq;       /* order among equals */
  s32 wakeups;
  void *func, *stack;
  u32 stack_size, gp, attr, option;
};

struct ksema {
  s32 count, max, init, waiting, attr, option;
  u8 used;
};

static struct kth th[NTHREADS + 1]; /* ids 1..NTHREADS */
static struct ksema sema[NSEMAS];
struct kctx *ksched_cur;
volatile int ksched_need;
static u32 seq;
static int active;

int ksched_active(void) { return active; }

static struct kth *cur(void) { return (struct kth *)ksched_cur; }
static int id_of(struct kth *t) { return (int)(t - th); }
static struct kth *thread(int id) {
  return id >= 1 && id <= NTHREADS && th[id].state != T_FREE ? &th[id] : 0;
}

static int runnable(struct kth *t) {
  return (t->state == T_READY || t->state == T_RUN) && !t->suspended;
}

/* t is ready now: a switch wanted if it comes before the running one */
static void made_ready(struct kth *t) {
  t->state = T_READY;
  t->seq = ++seq;
  struct kth *c = cur();
  if (!t->suspended && (!runnable(c) || t->prio < c->prio))
    ksched_need = 1;
}

/* with interrupts off (or at the switch): the next thread */
struct kctx *ksched_pick(void) {
  struct kth *c = cur(), *best = 0;
  ksched_need = 0;
  if (c->state == T_RUN)
    c->state = T_READY; /* keeps its seq: first among its equals */
  for (int i = 1; i <= NTHREADS; i++) {
    struct kth *t = &th[i];
    if (!runnable(t))
      continue;
    if (!best || t->prio < best->prio || (t->prio == best->prio && t->seq < best->seq))
      best = t;
  }
  best->state = T_RUN;
  ksched_cur = &best->c;
  return ksched_cur;
}

/* in a thread, interrupts as they are: switch now (the switch syscall) */
static void switch_now(void) {
  __asm__ volatile("li $3, %0\n\tsyscall" : : "i"(KSCHED_SYSCALL) : "$2", "$3", "memory");
}

/* in a thread after a change: give way if a better one is ready */
static void give_way(int was) {
  if (ksched_need)
    switch_now(); /* returns with interrupts on */
  else
    mx_irq_restore(was);
}

/* ---- threads ------------------------------------------------------------- */

struct thread_param {
  int status;
  void *func, *stack;
  int stack_size;
  void *gp;
  int initial_priority, current_priority;
  u32 attr, option;
};

static void thread_exit_stub(void);

static int k_create_thread(void *param) {
  const struct thread_param *p = param;
  int was = mx_di();
  for (int i = 1; i <= NTHREADS; i++)
    if (th[i].state == T_FREE) {
      struct kth *t = &th[i];
      memset(t, 0, sizeof *t);
      t->state = T_DORMANT;
      t->func = p->func, t->stack = p->stack, t->stack_size = (u32)p->stack_size;
      t->gp = (u32)(uintptr_t)p->gp, t->attr = p->attr, t->option = p->option;
      t->prio = t->init_prio = (u8)p->initial_priority;
      t->sema = -1;
      mx_irq_restore(was);
      return i;
    }
  mx_irq_restore(was);
  return -1;
}

static int k_start_thread(int id, void *arg) {
  int was = mx_di();
  struct kth *t = thread(id);
  if (!t || t->state != T_DORMANT) {
    mx_irq_restore(was);
    return -1;
  }
  memset(&t->c, 0, sizeof t->c);
  t->c.epc = (u32)(uintptr_t)t->func;
  t->c.r[4][0] = (u32)(uintptr_t)arg;
  t->c.r[28][0] = t->gp;
  t->c.r[29][0] = ((u32)(uintptr_t)t->stack + t->stack_size - 16) & ~15u;
  t->c.r[31][0] = (u32)(uintptr_t)thread_exit_stub;
  t->c.ie = 0x10001; /* EIE and IE */
  t->c.fcr31 = 0x01000000; /* flush to zero, as the BIOS starts them */
  t->prio = t->init_prio;
  t->wakeups = 0;
  made_ready(t);
  give_way(was);
  return id;
}

/* the running thread leaves: dormant, or gone */
static void leave(int free) {
  mx_di();
  struct kth *c = cur();
  c->state = free ? T_FREE : T_DORMANT;
  switch_now();
  for (;;)
    ;
}
static void k_exit_thread(void) { leave(0); }
static void k_exit_delete_thread(void) { leave(1); }
static void thread_exit_stub(void) { leave(0); }

/* out of a semaphore's queue (it was waiting) */
static void unwait(struct kth *t) {
  if (t->state == T_SEMA && t->sema >= 0)
    sema[t->sema].waiting--;
  t->sema = -1;
}

static int k_terminate_thread(int id) {
  int was = mx_di();
  struct kth *t = thread(id);
  if (!t || t == cur() || t->state == T_DORMANT) {
    mx_irq_restore(was);
    return -1;
  }
  unwait(t);
  t->state = T_DORMANT;
  t->suspended = 0;
  mx_irq_restore(was);
  return id;
}

static int k_delete_thread(int id) {
  int was = mx_di();
  struct kth *t = thread(id);
  int r = t && t->state == T_DORMANT ? (t->state = T_FREE, id) : -1;
  mx_irq_restore(was);
  return r;
}

static int k_suspend_thread(int id) {
  int was = mx_di();
  struct kth *t = thread(id);
  int r = -1;
  if (t && t != cur() && t->state != T_DORMANT) {
    t->suspended = 1;
    r = id;
  }
  mx_irq_restore(was);
  return r;
}

static int k_resume_thread(int id) {
  int was = mx_di();
  struct kth *t = thread(id);
  if (!t || !t->suspended) {
    mx_irq_restore(was);
    return -1;
  }
  t->suspended = 0;
  if (t->state == T_READY)
    made_ready(t);
  give_way(was);
  return id;
}

static int k_change_thread_priority(int id, int prio) {
  int was = mx_di();
  struct kth *t = id ? thread(id) : cur();
  if (!t) {
    mx_irq_restore(was);
    return -1;
  }
  int old = t->prio;
  t->prio = (u8)prio;
  if (t == cur()) {
    /* lower now: a ready one may come first */
    for (int i = 1; i <= NTHREADS; i++)
      if (runnable(&th[i]) && &th[i] != t && th[i].prio < t->prio)
        ksched_need = 1;
  } else if (t->state == T_READY && !t->suspended && t->prio < cur()->prio)
    ksched_need = 1;
  give_way(was);
  return old;
}

static int k_rotate_thread_ready_queue(int prio) {
  int was = mx_di();
  struct kth *c = cur();
  if (c->prio == prio) {
    c->seq = ++seq; /* last among its equals */
    for (int i = 1; i <= NTHREADS; i++)
      if (&th[i] != c && runnable(&th[i]) && th[i].prio == prio)
        ksched_need = 1;
  } else {
    /* the first ready one of that priority to the end */
    struct kth *first = 0;
    for (int i = 1; i <= NTHREADS; i++)
      if (th[i].state == T_READY && !th[i].suspended && th[i].prio == prio &&
          (!first || th[i].seq < first->seq))
        first = &th[i];
    if (first)
      first->seq = ++seq;
  }
  give_way(was);
  return 0;
}

static int k_get_thread_id(void) { return id_of(cur()); }

/* as the BIOS's (probe/p34_ids.c): an id without a thread gives 0 and a
   status of 0, one out of range -1 */
static int k_refer_thread_status(int id, void *status) {
  if (id < 0 || id > 255)
    return -1;
  struct kth *t = id ? thread(id) : cur();
  if (!t) {
    if (status)
      *(u32 *)status = 0;
    return 0;
  }
  int s = t->state == T_RUN       ? THS_RUN
          : t->state == T_READY   ? THS_READY
          : t->state == T_DORMANT ? THS_DORMANT
                                  : THS_WAIT;
  if (t->suspended)
    s = s == THS_WAIT ? THS_WAIT | THS_SUSPEND : THS_SUSPEND;
  if (status) {
    u32 *o = status;
    o[0] = (u32)s, o[1] = (u32)(uintptr_t)t->func, o[2] = (u32)(uintptr_t)t->stack;
    o[3] = t->stack_size, o[4] = t->gp, o[5] = t->init_prio, o[6] = t->prio;
    o[7] = t->attr, o[8] = t->option;
  }
  return s;
}

static int k_sleep_thread(void) {
  int was = mx_di();
  struct kth *c = cur();
  if (c->wakeups > 0) {
    c->wakeups--;
    mx_irq_restore(was);
    return id_of(c);
  }
  c->state = T_SLEEP;
  switch_now();
  mx_irq_restore(was);
  return id_of(c);
}

/* interrupts off */
static int wakeup(int id) {
  struct kth *t = thread(id);
  if (!t || t->state == T_DORMANT)
    return -1;
  if (t->state == T_SLEEP)
    made_ready(t);
  else
    t->wakeups++;
  return id;
}
static int k_wakeup_thread(int id) {
  int was = mx_di();
  int r = wakeup(id);
  give_way(was);
  return r;
}
static int k_iwakeup_thread(int id) { return wakeup(id); }

/* ---- semaphores ------------------------------------------------------------ */

struct sema_param {
  s32 count, max_count, init_count, wait_threads;
  u32 attr, option;
};

static struct ksema *sema_of(int id) {
  return id >= 0 && id < NSEMAS && sema[id].used ? &sema[id] : 0;
}

static int k_create_sema(void *param) {
  const struct sema_param *p = param;
  int was = mx_di();
  for (int i = 0; i < NSEMAS; i++)
    if (!sema[i].used) {
      struct ksema *s = &sema[i];
      s->used = 1;
      s->count = p->init_count, s->max = p->max_count, s->init = p->init_count;
      s->waiting = 0, s->attr = (s32)p->attr, s->option = (s32)p->option;
      mx_irq_restore(was);
      return i;
    }
  mx_irq_restore(was);
  return -1;
}

/* the first waiter of semaphore id ready, with result r */
static int wake_first(int id, int r) {
  struct kth *first = 0;
  for (int i = 1; i <= NTHREADS; i++)
    if (th[i].state == T_SEMA && th[i].sema == id && (!first || th[i].seq < first->seq))
      first = &th[i];
  if (!first)
    return 0;
  unwait(first);
  first->result = (s16)r;
  made_ready(first);
  return 1;
}

static int k_delete_sema(int id) {
  int was = mx_di();
  struct ksema *s = sema_of(id);
  if (!s) {
    mx_irq_restore(was);
    return -1;
  }
  while (wake_first(id, -1))
    ;
  s->used = 0;
  give_way(was);
  return id;
}

/* interrupts off */
static int signal(int id) {
  struct ksema *s = sema_of(id);
  if (!s)
    return -1;
  if (!wake_first(id, id) && s->count < s->max)
    s->count++;
  return id;
}
static int k_signal_sema(int id) {
  int was = mx_di();
  int r = signal(id);
  give_way(was);
  return r;
}
static int k_isignal_sema(int id) { return signal(id); }

static int k_wait_sema(int id) {
  int was = mx_di();
  struct ksema *s = sema_of(id);
  if (!s) {
    mx_irq_restore(was);
    return -1;
  }
  if (s->count > 0) {
    s->count--;
    mx_irq_restore(was);
    return id;
  }
  struct kth *c = cur();
  c->state = T_SEMA;
  c->sema = (s16)id;
  c->seq = ++seq; /* queued in order */
  s->waiting++;
  switch_now();
  mx_irq_restore(was);
  return c->result;
}

static int poll(int id) {
  struct ksema *s = sema_of(id);
  if (!s || s->count <= 0)
    return -1;
  s->count--;
  return id;
}
static int k_poll_sema(int id) {
  int was = mx_di();
  int r = poll(id);
  mx_irq_restore(was);
  return r;
}
static int k_ipoll_sema(int id) { return poll(id); }

static int k_refer_sema_status(int id, void *status) {
  struct ksema *s = sema_of(id);
  if (!s)
    return -1;
  if (status) {
    s32 *o = status;
    o[0] = s->count, o[1] = s->max, o[2] = s->init, o[3] = s->waiting;
    o[4] = s->attr, o[5] = s->option;
  }
  return id;
}

/* ---- interrupt handlers: run by kirq() ------------------------------------- */

typedef int (*khandler)(int cause, void *arg, void *addr);
static struct {
  khandler fn;
  void *arg;
  s16 line, n; /* 0 INTC / 1 DMAC, its cause or channel */
} handler[NHANDLERS + 1];  /* ids 1..NHANDLERS */
static u32 owned[2];       /* the causes and channels with a handler here */

static void owned_again(void) {
  u32 o[2] = {0, 0};
  for (int i = 1; i <= NHANDLERS; i++)
    if (handler[i].fn)
      o[handler[i].line] |= 1u << handler[i].n;
  owned[0] = o[0], owned[1] = o[1];
}

/* the handlers in the order they are called (the BIOS's: next 0 first, -1
   last, else before the handler next) */
static u8 order[NHANDLERS];
static int norder;

static int add_handler(int line, int n, khandler fn, int next, void *arg) {
  int was = mx_di();
  for (int i = 1; i <= NHANDLERS; i++)
    if (!handler[i].fn) {
      handler[i].fn = fn, handler[i].arg = arg, handler[i].line = (s16)line, handler[i].n = (s16)n;
      int at = 0;
      if (next == -1)
        at = norder;
      else if (next > 0)
        for (int k = 0; k < norder; k++)
          if (order[k] == next) {
            at = k;
            break;
          }
      for (int k = norder; k > at; k--)
        order[k] = order[k - 1];
      order[at] = (u8)i;
      norder++;
      owned_again();
      mx_irq_restore(was);
      return i;
    }
  mx_irq_restore(was);
  return -1;
}
static int remove_handler(int line, int n, int id) {
  int was = mx_di();
  int r = -1;
  if (id >= 1 && id <= NHANDLERS && handler[id].fn && handler[id].line == line && handler[id].n == n) {
    handler[id].fn = 0;
    int k = 0;
    while (k < norder && order[k] != id)
      k++;
    for (; k + 1 < norder; k++)
      order[k] = order[k + 1];
    if (norder)
      norder--;
    owned_again();
    r = id;
  }
  mx_irq_restore(was);
  return r;
}
static int k_add_intc_handler(int cause, khandler fn, int next, void *arg) {
  return add_handler(0, cause, fn, next, arg);
}
static int k_remove_intc_handler(int cause, int id) { return remove_handler(0, cause, id); }
static int k_add_dmac_handler(int ch, khandler fn, int next, void *arg) {
  return add_handler(1, ch, fn, next, arg);
}
static int k_remove_dmac_handler(int ch, int id) { return remove_handler(1, ch, id); }

#define I_STAT (*(volatile u32 *)0xB000F000u)
#define I_MASK (*(volatile u32 *)0xB000F010u)
#define D_STAT (*(volatile u32 *)0xB000E010u)

/* kirq.S, interrupts off, kernel mode: the pending causes or channels with
   a handler here acknowledged and handled, until none is left. The lines
   are the kernel's whole: one going on into the BIOS's handler met a
   channel of ours that ended in between (the BIOS takes the highest pending
   one), found no handler of its own and turned it off (the frame
   pipeline's VIF1 stopped so). Programs on the kernel's threads use no
   interrupt of the BIOS's, so what is pending and not ours is acknowledged
   and dropped: a cause in I_STAT, a channel's end, and on the DMAC line
   stall and MFIFO empty when their masks are on, a bus error. Returns 0:
   nothing for the BIOS. */
#define D_STAT_SIM (1u << 29)
#define D_STAT_MEIM (1u << 30)
u32 ksched_dispatch(int line, u32 epc) {
  for (int rounds = 0; rounds < 64; rounds++) {
    u32 st = line ? D_STAT : 0;
    u32 pending = line ? (st & (st >> 16) & 0x3FFu) : (I_STAT & I_MASK & 0x7FFFu);
    u32 mine = pending & owned[line];
    if (!mine) {
      u32 other = pending;
      if (line) {
        if ((st & (1u << 13)) && (st & D_STAT_SIM))
          other |= 1u << 13;
        if ((st & (1u << 14)) && (st & D_STAT_MEIM))
          other |= 1u << 14;
        if (st & (1u << 15))
          other |= 1u << 15;
      }
      if (!other)
        return 0;
      if (line)
        D_STAT = other; /* CIS, SIS, MEIS, BEIS: written 1 clears */
      else
        I_STAT = other;
      continue;
    }
    for (int n = 0; mine >> n; n++) {
      if (!(mine >> n & 1))
        continue;
      if (line)
        D_STAT = 1u << n; /* its CIS bit; CIM untouched */
      else
        I_STAT = 1u << n;
      /* in order; one returning -1 skips the rest (the BIOS's) */
      for (int k = 0; k < norder; k++) {
        int i = order[k];
        if (handler[i].fn && handler[i].line == line && handler[i].n == n &&
            handler[i].fn(n, handler[i].arg, (void *)(uintptr_t)epc) == -1)
          break;
      }
    }
  }
  return 0;
}

/* The causes and channels on or off: the BIOS's EnableIntc leaves a cause
   off that has no handler of the BIOS's own (seen: VIF1 never came), so
   the kernel writes the masks itself. A 1 written to I_MASK or to a CIM bit
   of D_STAT turns the bit over. Through the I/O mapping: in a thread these
   run in user mode. */
#define U_I_MASK (*(volatile u32 *)0x1000F010u)
#define U_D_STAT (*(volatile u32 *)0x1000E010u)
static int mask_set(int dmac, int n, int on) {
  if (n < 0 || n > (dmac ? 9 : 14))
    return -1;
  int was = mx_di();
  u32 bit = dmac ? 1u << (16 + n) : 1u << n;
  u32 now = dmac ? U_D_STAT : U_I_MASK;
  int was_on = (now & bit) != 0;
  if (was_on != on) {
    if (dmac)
      U_D_STAT = bit;
    else
      U_I_MASK = bit;
  }
  mx_irq_restore(was);
  return was_on;
}
static int k_enable_intc(int cause) { return mask_set(0, cause, 1); }
static int k_disable_intc(int cause) { return mask_set(0, cause, 0); }
static int k_enable_dmac(int ch) { return mask_set(1, ch, 1); }
static int k_disable_dmac(int ch) { return mask_set(1, ch, 0); }

/* ---- the table maxsdk's calls come to ---------------------------------------- */

const struct mx_kthreads ksched_kthreads = {
    .add_intc_handler = k_add_intc_handler,
    .remove_intc_handler = k_remove_intc_handler,
    .add_dmac_handler = k_add_dmac_handler,
    .remove_dmac_handler = k_remove_dmac_handler,
    .enable_intc = k_enable_intc,
    .disable_intc = k_disable_intc,
    .enable_dmac = k_enable_dmac,
    .disable_dmac = k_disable_dmac,
    .ienable_intc = k_enable_intc,
    .idisable_intc = k_disable_intc,
    .ienable_dmac = k_enable_dmac,
    .idisable_dmac = k_disable_dmac,
    .create_thread = k_create_thread,
    .delete_thread = k_delete_thread,
    .start_thread = k_start_thread,
    .exit_thread = k_exit_thread,
    .exit_delete_thread = k_exit_delete_thread,
    .terminate_thread = k_terminate_thread,
    .change_thread_priority = k_change_thread_priority,
    .rotate_thread_ready_queue = k_rotate_thread_ready_queue,
    .get_thread_id = k_get_thread_id,
    .suspend_thread = k_suspend_thread,
    .resume_thread = k_resume_thread,
    .refer_thread_status = k_refer_thread_status,
    .sleep_thread = k_sleep_thread,
    .wakeup_thread = k_wakeup_thread,
    .iwakeup_thread = k_iwakeup_thread,
    .create_sema = k_create_sema,
    .delete_sema = k_delete_sema,
    .signal_sema = k_signal_sema,
    .isignal_sema = k_isignal_sema,
    .wait_sema = k_wait_sema,
    .poll_sema = k_poll_sema,
    .ipoll_sema = k_ipoll_sema,
    .refer_sema_status = k_refer_sema_status,
};

static u8 idle_stack[1024] __attribute__((aligned(16)));
static void idle(void *arg) {
  (void)arg;
  for (;;)
    ;
}

void ksched_switch_k(void);

/* The thread calling it (the kernel's main) becomes thread 1, running; the
   idle thread is made; from here maxsdk's thread calls are the kernel's. */
void ksched_init(void) {
  memset(th, 0, sizeof th);
  memset(sema, 0, sizeof sema);
  memset(handler, 0, sizeof handler);
  norder = 0;
  owned[0] = owned[1] = 0;
  struct kth *m = &th[1];
  m->state = T_RUN;
  m->prio = m->init_prio = 64;
  m->sema = -1;
  ksched_cur = &m->c;
  extern char _gp[];
  struct thread_param ip = {0, (void *)idle, idle_stack, sizeof idle_stack, _gp, IDLE_PRIO, 0, 0, 0};
  mx_bios_set_syscall(KSCHED_SYSCALL, (void *)ksched_switch_k);
  mx__kthreads = &ksched_kthreads;
  active = 1;
  k_start_thread(k_create_thread(&ip), 0);
}
