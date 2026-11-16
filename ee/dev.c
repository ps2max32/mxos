/* Storage devices and notifications (kernel version 26, mx OS plan 4e;
   <mx/dev.h>, <mx/notify.h>).

   mxio watches the devices on the IOP (USB drives, MX4SIO and the HDD
   through bdm, the memory cards by looking every second) and writes its
   list by DMA to an area here (MXIO_WATCH), with a counter that changes when
   a device came or went. The kernel's vblank hook sees the counter change
   and wakes the command thread, which tells every process that asked for
   events (MX_EVENT_DEVICE) and adds a notification.

   The notifications: a ring of 32 in the area above the kernel's code, where
   programs have no right to write (written through the uncached alias). A
   new or changed one is MX_EVENT_NOTIFY to every process with events. */

#include <mx/cpu.h>
#include <mx/dev.h>
#include <mx/notify.h>
#include <mx/proc.h>
#include <mx/sif.h>
#include <mx/string.h>
#include <mx/bios.h>
#include <mx/timer.h>

#include <mxio.h>

#include "ksched.h"
#include "mxlink.h"

/* ---- notifications ----------------------------------------------------- */

#define NNOTIFY 32
static struct {
  struct mx_notify n[NNOTIFY];
} ring __attribute__((section(".padarea"), aligned(64)));
static u32 notify_ids, notify_seq_now;
static u32 next_slot;
static u64 ticks;
static u32 tick_last;

/* seconds since the boot (the vblank's count; settings.c too) */
u32 kseconds(void) { return (u32)(ticks / MX_EE_HZ); }

static void copy_text(char *to, const char *from, u32 max) {
  u32 n = 0;
  while (from && from[n] && n < max - 1) {
    to[n] = from[n];
    n++;
  }
  to[n] = 0;
}

static void notify_event(void) {
  struct mx_event e = {MX_EVENT_NOTIFY, 0, (s32)notify_seq_now, 0};
  kmsg_post_all(&e, sizeof e);
}

/* a notification of the kernel's (source "") or of process proc: its id */
int knotify_add(u32 kind, int urgent, u32 action, const char *source, const char *title,
                const char *text) {
  struct mx_notify n;
  memset(&n, 0, sizeof n);
  int was = mx_di();
  n.id = ++notify_ids, n.seq = ++notify_seq_now;
  u32 slot = next_slot++ % NNOTIFY;
  mx_irq_restore(was);
  n.time = kseconds();
  n.kind = (u8)kind, n.urgent = urgent ? 1 : 0, n.action = (u8)action;
  copy_text(n.source, source, sizeof n.source);
  copy_text(n.title, title, sizeof n.title);
  copy_text(n.text, text, sizeof n.text);
  volatile u32 *to = MX_UNCACHED((u32 *)&ring.n[slot]);
  const u32 *from = (const u32 *)&n;
  for (u32 i = 0; i < sizeof n / 4; i++)
    to[i] = from[i];
  con_log(urgent ? CON_RED : CON_YELLOW, "%s: %s", n.title, n.text);
  notify_event();
  return (int)n.id;
}

/* an urgent one no longer urgent (the swap device may go now) */
void knotify_resolve(int id) {
  for (u32 i = 0; i < NNOTIFY; i++) {
    volatile struct mx_notify *n = MX_UNCACHED(&ring.n[i]);
    if ((int)n->id == id && n->urgent) {
      n->urgent = 0;
      n->seq = ++notify_seq_now;
      notify_event();
    }
  }
}

int knotify(u32 kind, int urgent, const char *title, const char *text) {
  char src[16];
  ptab_name(ksched_thread_proc(mx_bios_get_thread_id()), src, sizeof src);
  return knotify_add(kind ? kind : MX_NOTIFY_PROGRAM, urgent, 0, src, title, text);
}

int knotify_read(u32 after, struct mx_notify *out, int max) {
  int got = 0;
  /* oldest first: by seq */
  for (u32 below = 0; got < max;) {
    u32 best = ~0u;
    int bi = -1;
    for (u32 i = 0; i < NNOTIFY; i++) {
      volatile struct mx_notify *n = MX_UNCACHED(&ring.n[i]);
      if (n->id && n->seq > after && n->seq > below && n->seq < best)
        best = n->seq, bi = (int)i;
    }
    if (bi < 0)
      break;
    const volatile u32 *from = MX_UNCACHED((u32 *)&ring.n[bi]);
    u32 *to = (u32 *)&out[got++];
    for (u32 i = 0; i < sizeof *out / 4; i++)
      to[i] = from[i];
    below = best;
  }
  return got;
}

u32 knotify_seq(void) { return notify_seq_now; }

/* ---- devices ------------------------------------------------------------- */

static struct {
  struct mxio_devs d;
  u32 in[16], out[16];
} area __attribute__((section(".padarea"), aligned(64)));
static struct mx_sif_client io_rpc;
static int dev_on;
static volatile int dev_wake;
static u32 dev_seen; /* the counter the command thread acted on */
/* the devices that were there at the last change: their names */
#define KNOWN 8
static char known[KNOWN][12];
static u8 known_kind[KNOWN];
static int dev_first = 1;
static u32 nknown;

static volatile struct mxio_devs *devs(void) { return MX_UNCACHED(&area.d); }

static u32 vblanks;
static void dev_vblank(void) {
  u32 now = mx_count();
  ticks += now - tick_last;
  tick_last = now;
  /* a device's change at once; the controllers twice a second */
  if (!dev_wake && ((dev_on && devs()->gen != dev_seen) || knet_changed() || ++vblanks % 30 == 0)) {
    dev_wake = 1;
    cmd_iwake();
  }
}

int mx__irq_vbon_hook(void (*fn)(void));

/* after the IOP's set-up (main.c): mxio writes its list here from now */
void dev_start(void) {
  static int hooked;
  if (!hooked && mx__irq_vbon_hook(dev_vblank) >= 0)
    hooked = 1, tick_last = mx_count();
  dev_on = 0;
  volatile u32 *w = (volatile u32 *)devs();
  for (u32 i = 0; i < sizeof area.d / 4; i++)
    w[i] = 0;
  if (mx_sif_bind_tries(&io_rpc, MXIO_SID, 2) < 0 || !io_rpc.server)
    return;
  volatile u32 *in = MX_UNCACHED(area.in), *out = MX_UNCACHED(area.out);
  in[0] = mx_paddr_of(&area.d).v;
  out[0] = 0;
  if (mx_sif_call(&io_rpc, MXIO_WATCH, (void *)in, 16, (void *)out, 16) < 0 ||
      out[0] != MXIO_DEVS_VERSION)
    return;
  dev_seen = 0, dev_first = 1, nknown = 0;
  dev_on = 1;
}

int dev_pending(void) { return dev_wake; }

/* kernel version 29: the PS2's clock through mxio (MXIO_CLOCK, cdvdman's
   sceCdReadClock / sceCdWriteClock): its 8 bytes (BCD, Japan's time) into
   or from c; cdvdman's result (1: done), or < 0 */
int dev_clock(int set, u8 c[8]) {
  if (!io_rpc.server)
    return -1;
  volatile u32 *in = MX_UNCACHED(area.in), *out = MX_UNCACHED(area.out);
  u32 w[2];
  memcpy(w, c, 8);
  in[0] = w[0], in[1] = w[1], in[2] = in[3] = 0;
  out[0] = (u32)-1;
  if (mx_sif_call(&io_rpc, set ? MXIO_CLOCK_SET : MXIO_CLOCK, (void *)in, 16, (void *)out, 16) < 0)
    return -1;
  w[0] = out[1], w[1] = out[2];
  memcpy(c, w, 8);
  return (int)out[0];
}

static const char *kind_name(u32 kind) {
  switch (kind) {
  case MX_DEV_MC:
    return "Memory card";
  case MX_DEV_USB:
    return "USB drive";
  case MX_DEV_MX4SIO:
    return "SD card";
  case MX_DEV_HDD:
    return "Hard disk";
  default:
    return "Device";
  }
}

static void size_text(char *to, u32 room, u64 bytes) {
  u32 mib = (u32)(bytes >> 20);
  if (!bytes)
    to[0] = 0;
  else if (mib >= 1024)
    mx_snprintf(to, room, " (%u.%u GB)", mib / 1024, mib % 1024 * 10 / 1024);
  else
    mx_snprintf(to, room, " (%u MB)", mib);
}

/* the command thread: what came and went since the last time, against
   the names known (mxio keeps one that went for one change only: the
   kernel may see the list after that). The first list after the start is
   taken as it is: what is there at the boot is not news. */
void dev_now(void) {
  dev_wake = 0;
  pad_watch();
  settings_tick();
  knet_now();
  kvideo_now();
  if (!dev_on)
    return;
  volatile struct mxio_devs *d = devs();
  u32 gen = d->gen;
  if (gen == dev_seen)
    return;
  dev_seen = gen;
  char names[KNOWN][12];
  u8 kinds[KNOWN];
  u32 n = 0;
  char title[MX_NOTIFY_TITLE], text[MX_NOTIFY_TEXT], sz[24];
  for (u32 i = 0; i < d->n && i < MXIO_DEVS; i++) {
    volatile struct mxio_dev *v = &d->d[i];
    if (!v->present || n >= KNOWN)
      continue;
    char name[12];
    for (u32 k = 0; k < sizeof name; k++)
      name[k] = v->name[k];
    name[sizeof name - 1] = 0;
    memcpy(names[n], name, sizeof name);
    kinds[n++] = v->kind;
    int was_there = 0;
    for (u32 k = 0; k < nknown; k++)
      was_there |= !strcmp(known[k], name);
    if (!was_there && !dev_first) {
      size_text(sz, sizeof sz, (u64)v->size_hi << 32 | v->size_lo);
      mx_snprintf(title, sizeof title, "%s connected", kind_name(v->kind));
      mx_snprintf(text, sizeof text, "%s%s", name, sz);
      knotify_add(MX_NOTIFY_DEVICE, 0, MX_NOTIFY_ACTION_STORAGE, "", title, text);
    }
    if (!was_there)
      kswap_device(name, 1);
  }
  /* those known that are not there now */
  for (u32 k = 0; k < nknown; k++) {
    int there = 0;
    for (u32 i = 0; i < n; i++)
      there |= !strcmp(names[i], known[k]);
    if (there)
      continue;
    mx_snprintf(title, sizeof title, "%s removed", kind_name(known_kind[k]));
    mx_snprintf(text, sizeof text, "%s", known[k]);
    knotify_add(MX_NOTIFY_DEVICE, 0, MX_NOTIFY_ACTION_STORAGE, "", title, text);
    kswap_device(known[k], 0);
  }
  memcpy(known, names, sizeof names);
  memcpy(known_kind, kinds, sizeof kinds);
  nknown = n;
  dev_first = 0;
  struct mx_event e = {MX_EVENT_DEVICE, 0, (s32)gen, 0};
  kmsg_post_all(&e, sizeof e);
}

/* is name there now (the swap file's device) */
int dev_present(const char *name) {
  for (u32 k = 0; k < nknown; k++)
    if (!strncmp(known[k], name, strlen(known[k])))
      return 1;
  return 0;
}

int kdev_list(struct mx_dev_info *out, int max) {
  if (!dev_on)
    return 0;
  volatile struct mxio_devs *d = devs();
  int n = (int)(d->n < MXIO_DEVS ? d->n : MXIO_DEVS);
  for (int i = 0; i < n && i < max; i++) {
    volatile struct mxio_dev *v = &d->d[i];
    struct mx_dev_info t;
    memset(&t, 0, sizeof t);
    for (u32 k = 0; k < sizeof t.name - 1; k++)
      t.name[k] = v->name[k];
    for (u32 k = 0; k < sizeof t.label - 1; k++)
      t.label[k] = v->label[k];
    t.kind = v->kind, t.present = v->present, t.fs = v->fs;
    t.flags = (u8)((v->files ? MX_DEV_BUSY : 0) | (kswap_on(t.name) ? MX_DEV_SWAP : 0));
    t.size = (u64)v->size_hi << 32 | v->size_lo;
    t.free = (u64)v->free_hi << 32 | v->free_lo;
    out[i] = t;
  }
  return n;
}

u32 kdev_gen(void) { return dev_on ? devs()->gen : 0; }

int kdev_eject(const char *name) {
  volatile struct mxio_devs *d = devs();
  for (u32 i = 0; dev_on && i < d->n && i < MXIO_DEVS; i++) {
    volatile struct mxio_dev *v = &d->d[i];
    char nm[12];
    for (u32 k = 0; k < sizeof nm; k++)
      nm[k] = v->name[k];
    nm[sizeof nm - 1] = 0;
    if (strcmp(nm, name))
      continue;
    char text[MX_NOTIFY_TEXT];
    if (kswap_on(nm)) {
      mx_snprintf(text, sizeof text, "%s holds the swap file: choose another place for it first", nm);
      knotify_add(MX_NOTIFY_SWAP, 0, MX_NOTIFY_ACTION_SWAP, "", "Cannot remove it", text);
      return -1;
    }
    if (v->files) {
      mx_snprintf(text, sizeof text, "%s: %u files are open on it", nm, v->files);
      knotify_add(MX_NOTIFY_DEVICE, 0, MX_NOTIFY_ACTION_STORAGE, "", "Cannot remove it yet", text);
      return -1;
    }
    return 0;
  }
  return -1;
}
