/* Ports and messages between processes (kernel version 13, <mx/msg.h>;
   docs/mx-os-apps-plan.md step 2e-3).

   A message is copied twice, each time in the thread of the process it
   belongs to, so in its address space (threads run in user mode: the kernel
   cannot reach another's memory): the sender's request into a slot here, in
   the kernel's memory that every process sees; the receiver's copy out of it
   in mx_msg_receive. The reply goes the same way back in the same slot. A
   slot is the sender's from its send to the copy of its reply.

   MX_MSG_SLOTS slots of MX_MSG_MAX bytes: the room the kernel has below its
   code now (step 2e-4 gives it more). Senders wait for a free slot
   (free_sema), receivers for a request on their port (its semaphore, the
   receiver's process's: it goes with that), a sender for its reply (its
   slot's).

   A process's end (kmsg_proc_end, end.c): its ports go, the requests to them
   are answered with -2; its requests not yet received are dropped, those
   received are dropped at their reply. */

#include <mx/bios.h>
#include <mx/cpu.h>
#include <mx/msg.h>
#include <mx/string.h>

#include "ksched.h"
#include "mxlink.h"

#define NPORTS 8

enum { S_FREE, S_FILL, S_QUEUED, S_TAKEN, S_REPLYING, S_REPLIED };

static struct kport {
  u8 used, proc;
  s16 sema;
  char name[MX_PORT_NAME];
} ports[NPORTS];

static struct kslot {
  u8 state, port, from, gen;
  u8 orphan; /* its sender is gone: dropped at the reply */
  u8 post;   /* the kernel's (an event): no sender, free once received */
  u8 pad[2];
  s16 sema;  /* the sender waits on it for its reply */
  u16 len, max;
  s32 result;
  u32 seq;
  u8 data[MX_MSG_MAX];
} slots[MX_MSG_SLOTS];

static int free_sema = -1;

#define NEVENTS 4
static struct {
  u8 proc, port;
} evport[NEVENTS];

static u32 seq;

struct sema_param {
  s32 count, max_count, init_count, wait_threads;
  u32 attr, option;
};
static int sema_new(int count, int max) {
  struct sema_param p = {0, max, count, 0, 0, 0};
  return mx_bios_create_sema(&p);
}

static int self_proc(void) { return ksched_thread_proc(mx_bios_get_thread_id()); }

/* after ksched_init, in a kernel thread: the semaphores are the kernel's */
void kmsg_init(void) {
  memset(ports, 0, sizeof ports);
  memset(slots, 0, sizeof slots);
  memset(evport, 0, sizeof evport);
  free_sema = sema_new(MX_MSG_SLOTS, MX_MSG_SLOTS);
  for (int i = 0; i < MX_MSG_SLOTS; i++)
    slots[i].sema = (s16)sema_new(0, 1);
}

static int name_ok(const char *name) {
  if (!name)
    return 0;
  for (u32 i = 0; i < MX_PORT_NAME; i++)
    if (!name[i])
      return i > 0;
  return 0;
}

/* interrupts off */
static int find(const char *name) {
  for (int i = 0; i < NPORTS; i++)
    if (ports[i].used && !strcmp(ports[i].name, name))
      return i;
  return -1;
}
static struct kport *port_of(int port) {
  return port >= 1 && port <= NPORTS && ports[port - 1].used ? &ports[port - 1] : 0;
}

int kmsg_port_create(const char *name) {
  int proc = self_proc();
  if (free_sema < 0 || proc <= 0 || !name_ok(name))
    return -1;
  char n[MX_PORT_NAME];
  strcpy(n, name); /* the caller's memory may fault: not with interrupts off */
  int sema = sema_new(0, MX_MSG_SLOTS); /* the caller's process's */
  if (sema < 0)
    return -1;
  int was = mx_di();
  int i = find(n), r = -1;
  if (i < 0)
    for (i = 0; i < NPORTS; i++)
      if (!ports[i].used) {
        ports[i].used = 1, ports[i].proc = (u8)proc, ports[i].sema = (s16)sema;
        strcpy(ports[i].name, n);
        r = i + 1;
        break;
      }
  mx_irq_restore(was);
  if (r < 0)
    mx_bios_delete_sema(sema);
  return r;
}

int kmsg_port_find(const char *name) {
  if (!name_ok(name))
    return -1;
  char n[MX_PORT_NAME];
  strcpy(n, name);
  int was = mx_di();
  int i = find(n);
  mx_irq_restore(was);
  return i < 0 ? -1 : i + 1;
}

/* interrupts off: the port gone, its requests answered with -2; one being
   replied to only when its replier is gone too (dead: the process's end),
   else the reply goes on */
static void port_gone(int i, int dead) {
  ports[i].used = 0;
  for (int k = 0; k < MX_MSG_SLOTS; k++) {
    struct kslot *s = &slots[k];
    if (s->port == i && s->post && s->state == S_QUEUED) {
      s->state = S_FREE, s->post = 0;
      mx_bios_isignal_sema(free_sema);
      continue;
    }
    if (s->port == i && !s->orphan && !s->post &&
        (s->state == S_QUEUED || s->state == S_TAKEN || (dead && s->state == S_REPLYING))) {
      s->result = -2, s->state = S_REPLIED;
      mx_bios_isignal_sema(s->sema);
    }
  }
}

int kmsg_port_delete(int port) {
  int was = mx_di();
  struct kport *p = port_of(port);
  int sema = -1;
  if (p && p->proc == self_proc()) {
    sema = p->sema;
    port_gone(port - 1, 0);
  }
  mx_irq_restore(was);
  if (sema < 0)
    return -1;
  mx_bios_delete_sema(sema); /* a receiver waiting gets -2 */
  return 0;
}

int kmsg_send(int port, const void *req, u32 len, void *reply, u32 max) {
  if (free_sema < 0 || len > MX_MSG_MAX || !port_of(port))
    return -1;
  if (mx_bios_wait_sema(free_sema) < 0)
    return -1;
  int was = mx_di();
  struct kslot *s = 0;
  for (int k = 0; k < MX_MSG_SLOTS && !s; k++)
    if (slots[k].state == S_FREE)
      s = &slots[k], s->state = S_FILL;
  mx_irq_restore(was);
  if (!s) { /* cannot be: free_sema counts them */
    mx_bios_signal_sema(free_sema);
    return -1;
  }
  s->from = (u8)self_proc(), s->orphan = 0;
  memcpy(s->data, req, len);
  was = mx_di();
  struct kport *p = port_of(port);
  if (!p) {
    s->state = S_FREE;
    mx_irq_restore(was);
    mx_bios_signal_sema(free_sema);
    return -1;
  }
  s->port = (u8)(port - 1), s->len = (u16)len, s->max = (u16)(max > MX_MSG_MAX ? MX_MSG_MAX : max);
  s->seq = ++seq, s->state = S_QUEUED;
  mx_bios_isignal_sema(p->sema);
  mx_irq_restore(was);
  mx_bios_wait_sema(s->sema);
  int r = s->result;
  if (r > 0)
    memcpy(reply, s->data, (u32)r);
  was = mx_di();
  s->state = S_FREE;
  mx_irq_restore(was);
  mx_bios_signal_sema(free_sema);
  return r;
}

int kmsg_receive(int port, void *buf, u32 max, struct mx_msg *m) {
  int was = mx_di();
  struct kport *p = port_of(port);
  int sema = p && p->proc == self_proc() ? p->sema : -1;
  mx_irq_restore(was);
  if (sema < 0)
    return -1;
  struct kslot *s = 0;
  while (!s) {
    /* a count without a request: one its sender's end dropped */
    if (mx_bios_wait_sema(sema) < 0)
      return -2; /* deleted: the port went */
    was = mx_di();
    if (!port_of(port)) {
      mx_irq_restore(was);
      return -2;
    }
    for (int k = 0; k < MX_MSG_SLOTS; k++) {
      struct kslot *t = &slots[k];
      if (t->state == S_QUEUED && t->port == port - 1 && (!s || (s32)(t->seq - s->seq) < 0))
        s = t;
    }
    if (s)
      s->state = S_TAKEN;
    mx_irq_restore(was);
  }
  u32 n = s->len < max ? s->len : max;
  memcpy(buf, s->data, n);
  if (m)
    m->len = s->len, m->from = s->from;
  int id = (int)(s - slots) + 1 + (s->gen << 8);
  if (s->post) {
    /* an event: nothing waits for a reply */
    was = mx_di();
    s->gen++, s->post = 0, s->state = S_FREE;
    mx_irq_restore(was);
    mx_bios_signal_sema(free_sema);
  }
  return id;
}

/* ---- events (kernel version 15): the kernel's messages to a process ----- */

int kmsg_events(int port) {
  int proc = self_proc(), r = -1;
  int was = mx_di();
  struct kport *p = port_of(port);
  if (proc > 0 && (port == 0 || (p && p->proc == proc))) {
    int free = -1;
    for (int i = 0; i < NEVENTS; i++) {
      if (evport[i].proc == proc)
        evport[i].proc = 0;
      if (!evport[i].proc && free < 0)
        free = i;
    }
    if (!port)
      r = 0;
    else if (free >= 0)
      evport[free].proc = (u8)proc, evport[free].port = (u8)port, r = 0;
  }
  mx_irq_restore(was);
  return r;
}

/* An event to proc's event port, if it has one: in a kernel thread (the
   command thread), without waiting: a full queue drops it (-1). */
int kmsg_post(int proc, const void *data, u32 len) {
  if (free_sema < 0 || proc <= 0 || len > MX_MSG_MAX)
    return -1;
  int was = mx_di(), port = 0;
  for (int i = 0; i < NEVENTS; i++)
    if (evport[i].proc == proc)
      port = evport[i].port;
  struct kport *p = port_of(port);
  struct kslot *s = 0;
  if (p && p->proc == proc && mx_bios_ipoll_sema(free_sema) >= 0)
    for (int k = 0; k < MX_MSG_SLOTS && !s; k++)
      if (slots[k].state == S_FREE)
        s = &slots[k];
  if (s) {
    memcpy(s->data, data, len);
    s->from = 0, s->orphan = 0, s->post = 1;
    s->port = (u8)(port - 1), s->len = (u16)len, s->max = 0;
    s->seq = ++seq, s->state = S_QUEUED;
    mx_bios_isignal_sema(p->sema);
  }
  mx_irq_restore(was);
  return s ? 0 : -1;
}

int kmsg_reply(int msg, const void *reply, u32 len) {
  int k = (msg & 0xFF) - 1;
  if (msg <= 0 || k >= MX_MSG_SLOTS)
    return -1;
  struct kslot *s = &slots[k];
  int was = mx_di();
  int ok = s->state == S_TAKEN && s->gen == (u8)(msg >> 8) && port_of(s->port + 1) &&
           ports[s->port].proc == self_proc();
  if (ok)
    s->state = S_REPLYING; /* the replier's, while it copies */
  mx_irq_restore(was);
  if (!ok)
    return -1;
  u32 n = len < s->max ? len : s->max;
  if (!s->orphan)
    memcpy(s->data, reply, n);
  was = mx_di();
  s->gen++;
  if (s->orphan) {
    s->state = S_FREE;
    mx_bios_isignal_sema(free_sema);
  } else {
    s->result = (s32)n, s->state = S_REPLIED;
    mx_bios_isignal_sema(s->sema);
  }
  mx_irq_restore(was);
  return 0;
}

/* end.c, a process's end, its threads gone and its semaphores not yet */
void kmsg_proc_end(int proc) {
  if (proc <= 0 || free_sema < 0)
    return;
  int was = mx_di();
  for (int i = 0; i < NEVENTS; i++)
    if (evport[i].proc == proc)
      evport[i].proc = 0;
  for (int i = 0; i < NPORTS; i++)
    if (ports[i].used && ports[i].proc == proc)
      port_gone(i, 1);
  for (int k = 0; k < MX_MSG_SLOTS; k++) {
    struct kslot *s = &slots[k];
    if (s->from != proc || s->state == S_FREE)
      continue;
    if (s->state == S_TAKEN || s->state == S_REPLYING)
      s->orphan = 1;
    else {
      /* a request not received, a reply not taken, or one the sender was
         filling: the slot is free again */
      s->state = S_FREE;
      mx_bios_ipoll_sema(s->sema); /* a reply signalled, not taken */
      mx_bios_isignal_sema(free_sema);
    }
  }
  mx_irq_restore(was);
}
