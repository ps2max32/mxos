/* The pad is the kernel's (<mx/kernel.h>, version 6): libmxk's mx_pad_*
   read it here, with the pad's settings applied.

   Two sources: the SIO2 ports, through maxsdk's own reading (mx__pad_init,
   mx__pad_read) with the area in mxlink's memory: the ports are opened when
   a program first asks and closed when it ends (main.c's cleanup), so a
   program with pad code of its own finds them free; and the USB pads of
   mxpad.irx (maxsdk's iop/usb/mxpad, common/include/mxpad.h), which writes
   each pad's state into usb_area by SIF DMA.

   The ports a program reads: first the SIO2 pads that are there, in port
   order, then the USB pads in the order they came. A pad plugged into a SIO2
   port moves the USB pads up a port. A USB pad's Share (Create) is Select,
   Options is Start (mxpad.irx), and its PS button is MX_PAD_HOME, which a
   SIO2 pad gives for Select + Start + L1 + R1 (mx__pad_read). */

#include <mx/timer.h>
#include <mx/kernel.h>
#include <mx/pad.h>
#include <mx/sif.h>

#include <mxpad.h>

#include "mxlink.h"

_Static_assert(PAD_PORTS == 2 + MXPAD_PADS, "PAD_PORTS: the SIO2 ports and the USB slots");

/* kept over the restarts: the settings last until a reset */
struct pad_settings pad_settings __attribute__((section(".kept"))) = {0};

/* Above the code (ee/resident.ld: .padarea, not loaded), where the upper
   half of mxlink's TLB entry has no right to write: mxpad writes it by DMA
   and mxlink through the uncached alias (MX_UNCACHED: the BIOS's uncached
   entries, which may write; mxlink runs in user mode, so not KSEG1). The
   area and the call's buffers, on lines of their own, never touched cached. */
static struct {
  struct mxpad_area area;
  u32 in[16], out[16];
} usb __attribute__((section(".padarea"), aligned(64)));
static struct mx_sif_client usb_rpc;
static int usb_on;

/* After the IOP's set-up (main.c): mxpad told where the area is, if it runs. */
void pad_usb_start(void) {
  usb_on = 0;
  volatile u32 *w = (volatile u32 *)MX_UNCACHED((u32 *)&usb.area);
  for (u32 i = 0; i < sizeof usb.area / 4; i++)
    w[i] = 0;
  if (mx_sif_bind_tries(&usb_rpc, MXPAD_SID, 2) < 0 || !usb_rpc.server)
    return;
  volatile u32 *in = MX_UNCACHED(usb.in), *out = MX_UNCACHED(usb.out);
  in[0] = mx_paddr_of(&usb.area).v;
  out[0] = 0;
  if (mx_sif_call(&usb_rpc, MXPAD_AREA, (void *)in, 16, (void *)out, 16) < 0 || out[0] != MXPAD_VERSION)
    return;
  usb_on = 1;
}

/* the newer of the slot's two records */
static struct mxpad_record usb_newest(int slot) {
  volatile struct mxpad_record *r = MX_UNCACHED(usb.area.r[slot]);
  struct mxpad_record a = r[0], b = r[1];
  return (int)(b.seq - a.seq) > 0 ? b : a;
}

/* ---- the SIO2 ports, held ----
   A SIO2 pad counts as there for half a second after it was last seen, so
   that the pads after it keep their ports while it is plugged in or out (a
   port flickers then). When a program opens the ports, a pad that was there
   before keeps its port while it is found again (up to 1.5 s). */
#define SIO_HOLD (MX_EE_HZ / 2)
#define SIO_OPEN_HOLD (MX_EE_HZ * 3 / 2)
static u8 sio_was[2];      /* seen, and not yet given up */
static u32 sio_until[2];   /* held until (mx_count) */

/* 2: a pad there; 1: held for one; 0: none */
static int sio_state(int p) {
  struct mx_pad_state t;
  u32 now = mx_count();
  if (mx__pad_read(p, &t, 0)) {
    sio_was[p] = 1, sio_until[p] = now + SIO_HOLD;
    return 2;
  }
  if (sio_was[p] && (int)(sio_until[p] - now) > 0)
    return 1;
  sio_was[p] = 0;
  return 0;
}

/* <mx/kernel.h>'s pad_init: the ports open, a pad that was there held */
int pad_init(void) {
  int r = mx__pad_init();
  for (int p = 0; p < 2; p++)
    if (sio_was[p])
      sio_until[p] = mx_count() + SIO_OPEN_HOLD;
  return r;
}

/* What a program's port is: a SIO2 port (*sio 1 with a pad, 2 held for one;
   *n the port) or a USB or Bluetooth pad (*sio 0, *n its slot of mxpad.irx,
   *rec its state); 0 with no pad there. The SIO2 pads first, then the others
   by the order they came. */
static int port_source(int port, int *sio, int *n, struct mxpad_record *rec) {
  int k = 0;
  for (int p = 0; p < 2; p++) {
    int st = sio_state(p);
    if (st && k++ == port) {
      *sio = st == 2 ? 1 : 2, *n = p;
      return 1;
    }
  }
  if (!usb_on || (unsigned)port >= PAD_PORTS)
    return 0;
  struct mxpad_record r[MXPAD_PADS];
  int slot[MXPAD_PADS], m = 0;
  for (int i = 0; i < MXPAD_PADS; i++) {
    struct mxpad_record x = usb_newest(i);
    if (!x.kind)
      continue;
    int j = m++;
    for (; j > 0 && r[j - 1].connected > x.connected; j--)
      r[j] = r[j - 1], slot[j] = slot[j - 1];
    r[j] = x, slot[j] = i;
  }
  if (port - k >= m)
    return 0;
  *sio = 0, *n = slot[port - k], *rec = r[port - k];
  return 1;
}

/* The pad of a program's port, from both sources, without the settings: 1
   with a pad there. */
int pad_merged(int port, struct mx_pad_state *s, int sticks) {
  int sio, n;
  struct mxpad_record rec;
  int there = port_source(port, &sio, &n, &rec);
  if (there && sio == 1)
    return mx__pad_read(n, s, sticks);
  s->buttons = 0;
  s->analog = 0;
  for (int i = 0; i < 4; i++)
    s->sticks[i] = 128;
  if (there && sio == 2)
    return 1; /* held: a pad at rest */
  if (!usb_on || (unsigned)port >= PAD_PORTS || !port_source(port, &sio, &n, &rec))
    return 0;
  const struct mxpad_record *x = &rec;
  s->buttons = x->buttons & 0xFFFF;
  if (x->buttons & MXPAD_PS)
    s->buttons |= MX_PAD_HOME;
  if (sticks) {
    s->analog = 1;
    s->sticks[0] = x->rx, s->sticks[1] = x->ry, s->sticks[2] = x->lx, s->sticks[3] = x->ly;
  }
  return 1;
}

/* ---- rumble (<mx/kernel.h> version 11) ---- */

/* what each slot's motors were told last (a call only on a change) */
static u8 rumble_small[MXPAD_PADS], rumble_big[MXPAD_PADS];
static u8 sio_rumbling[2];

static int usb_output(int slot, u32 small, u32 big) {
  volatile u32 *in = MX_UNCACHED(usb.in), *out = MX_UNCACHED(usb.out);
  in[0] = (u32)slot, in[1] = (small & 0xff) | (big & 0xff) << 8, in[2] = 0, in[3] = 0;
  if (mx_sif_call(&usb_rpc, MXPAD_OUTPUT, (void *)in, 16, (void *)out, 16) < 0)
    return -1;
  return (int)out[0];
}

int pad_rumble(int port, u32 small, u32 big) {
  int sio, n;
  struct mxpad_record rec;
  if (pad_settings.swap_ports && (unsigned)port <= 1)
    port ^= 1;
  if (!port_source(port, &sio, &n, &rec) || sio == 2)
    return -1;
  if (sio) {
    sio_rumbling[n] = small || big;
    return mx__pad_act(n, small, big);
  }
  small = small ? 0xff : 0, big = big > 255 ? 255 : big;
  if (rumble_small[n] == small && rumble_big[n] == big)
    return 0;
  rumble_small[n] = (u8)small, rumble_big[n] = (u8)big;
  return usb_output(n, small, big);
}

/* the end of a program: every motor it left running stops */
void pad_rumble_off(void) {
  for (int p = 0; p < 2; p++)
    if (sio_rumbling[p]) {
      sio_rumbling[p] = 0;
      mx__pad_act(p, 0, 0);
    }
  for (int i = 0; usb_on && i < MXPAD_PADS; i++)
    if (rumble_small[i] || rumble_big[i]) {
      rumble_small[i] = rumble_big[i] = 0;
      usb_output(i, 0, 0);
    }
}

static u32 swap_bits(u32 b, u32 x, u32 y) {
  u32 bx = b & x, by = b & y;
  return (b & ~(x | y)) | (bx ? y : 0) | (by ? x : 0);
}

/* the home button of any pad: it is the system's, whichever port a program reads */
static int home_anywhere(void) {
  struct mx_pad_state t;
  for (int p = 0; p < 2; p++)
    if (mx__pad_read(p, &t, 0) && (t.buttons & MX_PAD_HOME))
      return 1;
  for (int i = 0; usb_on && i < MXPAD_PADS; i++) {
    struct mxpad_record x = usb_newest(i);
    if (x.kind && (x.buttons & MXPAD_PS))
      return 1;
  }
  return 0;
}

int pad_read(int port, struct mx_pad_state *s, int sticks) {
  if (pad_settings.swap_ports && (unsigned)port <= 1)
    port ^= 1;
  int r = pad_merged(port, s, sticks);
  if (home_anywhere())
    s->buttons |= MX_PAD_HOME;
  if (pad_settings.swap_xo)
    s->buttons = swap_bits(s->buttons, MX_PAD_CROSS, MX_PAD_CIRCLE);
  s->buttons = home_filter(s->buttons);
  return r;
}
