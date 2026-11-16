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

#include <mx/notify.h>
#include <mx/proc.h>

#include <mx/setting.h>
#include <mx/string.h>

#include "mxlink.h"

_Static_assert(PAD_PORTS == 2 + MXPAD_PADS && PAD_PORTS == MX_PAD_PORTS, "PAD_PORTS: the SIO2 ports and the USB slots");

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
  /* the command thread's and the shell's calls (kernel version 28), each
     on a client of its own: a program's rumble may be under way meanwhile */
  u32 cin[32], cout[32], sin[32], sout[32];
} usb __attribute__((section(".padarea"), aligned(64)));
static struct mx_sif_client usb_rpc, cmd_rpc, shell_rpc;
static int usb_on;
/* the player each USB slot's light shows, one byte a slot (MXPAD_PLAYERS) */
static u32 players_sent;

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
  if (mx_sif_bind_tries(&cmd_rpc, MXPAD_SID, 2) < 0 || mx_sif_bind_tries(&shell_rpc, MXPAD_SID, 2) < 0)
    return;
  usb_on = 1;
  players_sent = 0x03020100; /* mxpad's own after its start: each slot its number */
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

/* the SIO2 ports are open (a program's mx_pad_init until pads_close) */
static int ports_open;
/* checks (twice a second) in which the SIO2 ports' state is taken as it is,
   not told: after a switch between processes or the ports opened or
   closed, a port reads as empty for a moment (pad_watch) */
static int sio_quiet;
void pad_watch_quiet(void) { sio_quiet = 6; }
void pad_ports_closed(void) { ports_open = 0, sio_quiet = 6; }

/* <mx/kernel.h>'s pad_init: the ports open, a pad that was there held */
int pad_init(void) {
  int r = mx__pad_init();
  ports_open = 1, sio_quiet = 6;
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

/* The light of each USB and Bluetooth pad: the player (port) it is, which
   changes when a SIO2 pad comes or goes, with the swap of ports 0 and 1 */
static void pad_players(void) {
  _Static_assert(MXPAD_PADS == 4, "pad_players: a byte a slot in one word");
  u32 want = 0x03020100;
  for (int port = 0; port < PAD_PORTS; port++) {
    int sio, n;
    struct mxpad_record rec;
    if (!port_source(port, &sio, &n, &rec) || sio)
      continue;
    int player = pad_settings.swap_ports && port <= 1 ? port ^ 1 : port;
    want = (want & ~(0xFFu << 8 * n)) | (u32)player << 8 * n;
  }
  if (want == players_sent)
    return;
  volatile u32 *in = MX_UNCACHED(usb.in), *out = MX_UNCACHED(usb.out);
  in[0] = want, in[1] = in[2] = in[3] = 0;
  out[0] = (u32)-1;
  /* an mxpad.irx before MXPAD_PLAYERS answers -1: not asked again */
  if (mx_sif_call(&usb_rpc, MXPAD_PLAYERS, (void *)in, 16, (void *)out, 16) >= 0)
    players_sent = want;
}

int pad_read(int port, struct mx_pad_state *s, int sticks) {
  if (port == 0 && usb_on)
    pad_players();
  if (pad_settings.swap_ports && (unsigned)port <= 1)
    port ^= 1;
  int r = pad_merged(port, s, sticks);
  kvideo_pad(s->buttons);
  if (home_anywhere())
    s->buttons |= MX_PAD_HOME;
  if (pad_settings.swap_xo)
    s->buttons = swap_bits(s->buttons, MX_PAD_CROSS, MX_PAD_CIRCLE);
  s->buttons = home_filter(s->buttons);
  return r;
}

/* ---- the controllers for the shell (kernel version 28, <mx/pad.h>) ------ */

static int kind_of(int mxpad_kind) {
  return mxpad_kind == MXPAD_DS3   ? MX_PAD_KIND_DS3
         : mxpad_kind == MXPAD_DS4 ? MX_PAD_KIND_DS4
         : mxpad_kind == MXPAD_DS5 ? MX_PAD_KIND_DS5
                                   : MX_PAD_KIND_NONE;
}

/* a call to mxpad on the client c with buffers in and out (uncached, 128
   bytes each): out[0], or -1 */
static int call(struct mx_sif_client *c, u32 *in_, u32 *out_, int fn, u32 inlen, u32 outlen) {
  if (!usb_on)
    return -1;
  volatile u32 *out = MX_UNCACHED(out_);
  out[0] = (u32)-1;
  if (mx_sif_call(c, fn, (void *)MX_UNCACHED(in_), inlen, (void *)out, outlen) < 0)
    return -1;
  return (int)out[0];
}
#define SHELL_CALL(fn, inlen, outlen) call(&shell_rpc, usb.sin, usb.sout, fn, inlen, outlen)
#define CMD_CALL(fn, inlen, outlen) call(&cmd_rpc, usb.cin, usb.cout, fn, inlen, outlen)

/* each slot's light bar as sent (MXPAD_COLOURS) */
static u8 colour_sent[MXPAD_PADS][3];

int kpad_info(int port, struct mx_pad_info *i) {
  int sio, n;
  struct mxpad_record r;
  if ((unsigned)port >= MX_PAD_PORTS)
    return -1;
  if (pad_settings.swap_ports && (unsigned)port <= 1)
    port ^= 1;
  struct mx_pad_info t = {0};
  if (!port_source(port, &sio, &n, &r)) {
    *i = t;
    return 0;
  }
  t.hw = (u8)n;
  if (sio) {
    t.kind = MX_PAD_KIND_PS2, t.link = MX_PAD_LINK_PORT, t.battery = 255;
  } else {
    t.kind = (u8)kind_of(r.kind);
    t.link = r.flags & MXPAD_BT ? MX_PAD_LINK_BT : MX_PAD_LINK_USB;
    t.battery = r.battery;
    t.flags = (r.flags & MXPAD_CABLE ? MX_PAD_INFO_CABLE : 0) | (r.flags & MXPAD_CHARGING ? MX_PAD_INFO_CHARGING : 0);
    t.id = r.id;
    if (r.kind == MXPAD_DS4 || r.kind == MXPAD_DS5)
      for (int k = 0; k < 3; k++)
        t.rgb[k] = colour_sent[n][k];
  }
  *i = t;
  return 1;
}

int kpad_disconnect(int port) {
  int sio, n;
  struct mxpad_record r;
  if (!kproc_shell())
    return -1;
  if (pad_settings.swap_ports && (unsigned)port <= 1)
    port ^= 1;
  if (!port_source(port, &sio, &n, &r) || sio || !(r.flags & MXPAD_BT))
    return -1;
  MX_UNCACHED(usb.sin)[0] = (u32)n;
  return SHELL_CALL(MXPAD_DISCONNECT, 16, 16) < 0 ? -1 : 0;
}

/* the pairing's progress is watched (pad_watch) for this many checks */
static u32 pair_watch;
static u32 pair_seq_seen;

int kpad_pair(u32 seconds) {
  if (!kproc_shell())
    return -1;
  MX_UNCACHED(usb.sin)[0] = seconds;
  pair_watch = seconds * 2 + 20;
  return SHELL_CALL(MXPAD_PAIR, 16, 16) < 0 ? -1 : 0;
}

static void pairing_of(volatile u32 *out, struct mx_pad_pairing *p) {
  struct mxpad_pairing x;
  u32 w[sizeof x / 4];
  for (u32 k = 0; k < sizeof x / 4; k++)
    w[k] = out[k];
  memcpy(&x, w, sizeof x);
  p->state = x.state, p->seq = x.seq, p->kind = (u8)kind_of(x.kind);
  p->id = x.state == MXPAD_PAIR_CONNECTING || x.state == MXPAD_PAIR_DONE || x.state == MXPAD_PAIR_FAILED
              ? mxpad_id(x.addr)
              : 0;
  p->pad[0] = p->pad[1] = p->pad[2] = 0;
}

int kpad_pair_state(struct mx_pad_pairing *p) {
  if (SHELL_CALL(MXPAD_PAIR_STATE, 16, 16) < 0 && MX_UNCACHED(usb.sout)[0] == (u32)-1)
    return -1;
  pairing_of(MX_UNCACHED(usb.sout), p);
  return 0;
}

static int paired_list(struct mx_pad_paired *p, int max) {
  int n = SHELL_CALL(MXPAD_PAIRED, 16, 4 + 8 * MXPAD_PAIRED_MAX);
  if (n < 0)
    return -1;
  volatile u32 *out = MX_UNCACHED(usb.sout);
  int got = 0;
  for (int i = 0; i < n && i < MXPAD_PAIRED_MAX && got < max; i++) {
    u32 w[2] = {out[1 + 2 * i], out[2 + 2 * i]};
    struct mxpad_paired x;
    memcpy(&x, w, sizeof x);
    struct mx_pad_paired t;
    t.id = mxpad_id(x.addr), t.kind = (u8)kind_of(x.kind), t.connected = x.connected;
    for (int k = 0; k < 6; k++)
      t.addr[k] = x.addr[5 - k];
    p[got++] = t;
  }
  return got;
}

int kpad_paired(struct mx_pad_paired *p, int max) { return paired_list(p, max); }

int kpad_forget(u32 id) {
  if (!kproc_shell())
    return -1;
  struct mx_pad_paired l[MXPAD_PAIRED_MAX];
  int n = paired_list(l, MXPAD_PAIRED_MAX);
  for (int i = 0; i < n; i++)
    if (l[i].id == id) {
      volatile u32 *in = MX_UNCACHED(usb.sin);
      u8 a[8] = {0};
      for (int k = 0; k < 6; k++)
        a[k] = l[i].addr[5 - k]; /* HCI byte order */
      u32 w[2];
      memcpy(w, a, 8);
      in[0] = w[0], in[1] = w[1];
      return SHELL_CALL(MXPAD_FORGET, 16, 16) < 0 ? -1 : 0;
    }
  return -1;
}

/* The light bars (pad_watch): pad.<id>.colour, else the player's colour,
   by pad.brightness (25 by default: mxpad's own player colours) */
static const u8 player_colour[4][3] = {{0, 0, 255}, {255, 0, 0}, {0, 255, 0}, {255, 0, 128}};

static int hex_rgb(const char *v, u8 rgb[3]) {
  u32 x = 0;
  int n = 0;
  if (*v == '#')
    v++;
  for (; n < 6; n++, v++) {
    int d = *v >= '0' && *v <= '9' ? *v - '0' : *v >= 'a' && *v <= 'f' ? *v - 'a' + 10 : *v >= 'A' && *v <= 'F' ? *v - 'A' + 10 : -1;
    if (d < 0)
      return -1;
    x = x << 4 | (u32)d;
  }
  rgb[0] = (u8)(x >> 16), rgb[1] = (u8)(x >> 8), rgb[2] = (u8)x;
  return 0;
}

static void pad_colours(void) {
  char v[16];
  u32 bright = 25;
  if (ksetting_get("pad.brightness", v, sizeof v) > 0) {
    bright = 0;
    for (const char *c = v; *c >= '0' && *c <= '9'; c++)
      bright = bright * 10 + (u32)(*c - '0');
    if (bright > 100)
      bright = 100;
  }
  u8 want[MXPAD_PADS][3];
  for (int i = 0; i < MXPAD_PADS; i++) {
    struct mxpad_record r = usb_newest(i);
    u8 c[3];
    char key[32];
    mx_snprintf(key, sizeof key, "pad.%08x.colour", r.id);
    if (!r.kind || !r.id || ksetting_get(key, v, sizeof v) <= 0 || hex_rgb(v, c) < 0)
      memcpy(c, player_colour[players_sent >> 8 * i & 3], 3);
    for (int k = 0; k < 3; k++)
      want[i][k] = (u8)((c[k] * bright + 50) / 100);
    /* 0 0 0 means "the player's colour" to mxpad: the light off is 0 0 1 */
    if (!want[i][0] && !want[i][1] && !want[i][2])
      want[i][2] = 1;
  }
  if (!memcmp(want, colour_sent, sizeof want))
    return;
  volatile u32 *in = MX_UNCACHED(usb.cin);
  u32 w[3];
  memcpy(w, want, sizeof want);
  in[0] = w[0], in[1] = w[1], in[2] = w[2];
  if (CMD_CALL(MXPAD_COLOURS, 16, 16) == 0)
    memcpy(colour_sent, want, sizeof want);
}

/* pad.idle_off: a Bluetooth pad on battery that sent nothing for that many
   minutes is turned off */
static u32 idle_seq[MXPAD_PADS], idle_since[MXPAD_PADS];

static void pad_idle(void) {
  char v[16];
  u32 minutes = 0;
  if (ksetting_get("pad.idle_off", v, sizeof v) > 0)
    for (const char *c = v; *c >= '0' && *c <= '9'; c++)
      minutes = minutes * 10 + (u32)(*c - '0');
  u32 now = kseconds();
  for (int i = 0; i < MXPAD_PADS; i++) {
    struct mxpad_record r = usb_newest(i);
    if (!r.kind || r.seq != idle_seq[i] || (r.flags & MXPAD_CABLE) || !(r.flags & MXPAD_BT)) {
      idle_seq[i] = r.seq, idle_since[i] = now;
      continue;
    }
    if (minutes && now - idle_since[i] >= minutes * 60) {
      idle_since[i] = now;
      MX_UNCACHED(usb.cin)[0] = (u32)i;
      CMD_CALL(MXPAD_DISCONNECT, 16, 16);
    }
  }
}

/* ---- notifications (kernel version 26) ----------------------------------
   The command thread (dev.c, twice a second): a controller came or went, a
   wireless one's battery runs low. USB and Bluetooth pads always; the SIO2
   ports only while a program has them open (a pad there is not looked for
   otherwise: closing them is not a pad that went). */
static u8 usb_kind[MXPAD_PADS], usb_low[MXPAD_PADS], sio_known[2], sio_seen[2];
static u32 pad_changes; /* mx_pad_gen */
static u32 usb_sig[MXPAD_PADS]; /* kind, battery, flags but the touch: a change counts */
u32 kpad_gen(void) { return pad_changes; }

static const char *pad_name(int kind) {
  return kind == MXPAD_DS3   ? "DualShock 3"
         : kind == MXPAD_DS4 ? "DualShock 4"
         : kind == MXPAD_DS5 ? "DualSense"
                             : "Controller";
}

/* its player: the program's port it is on, + 1 */
static int player_of(int sio, int n) {
  for (int port = 0; port < PAD_PORTS; port++) {
    int s, m;
    struct mxpad_record r;
    if (port_source(port, &s, &m, &r) && (s != 0) == (sio != 0) && m == n)
      return port + 1;
  }
  return 0;
}

static void tell(const char *title, const char *what, int player) {
  char text[MX_NOTIFY_TEXT];
  if (player)
    mx_snprintf(text, sizeof text, "%s, player %d", what, player);
  else
    mx_snprintf(text, sizeof text, "%s", what);
  knotify_add(MX_NOTIFY_PAD, 0, MX_NOTIFY_ACTION_PADS, "", title, text);
}

void pad_watch(void) {
  int changed = 0;
  char what[64];
  for (int i = 0; usb_on && i < MXPAD_PADS; i++) {
    struct mxpad_record r = usb_newest(i);
    int k = r.kind;
    if (k && !usb_kind[i]) {
      mx_snprintf(what, sizeof what, "%s (%s)", pad_name(k), r.flags & MXPAD_BT ? "Bluetooth" : "USB");
      tell("Controller connected", what, player_of(0, i));
      changed = 1;
      if (!(r.flags & MXPAD_BT) && pair_watch < 20)
        pair_watch = 20; /* it pairs with a dongle by its cable */
    } else if (!k && usb_kind[i]) {
      tell("Controller disconnected", pad_name(usb_kind[i]), 0);
      changed = 1;
    }
    usb_kind[i] = (u8)k;
    u32 sig = (u32)k | (u32)r.battery << 8 | (u32)(r.flags & ~MXPAD_TOUCH) << 16 | (r.id ? 1u << 24 : 0);
    if (sig != usb_sig[i])
      usb_sig[i] = sig, changed = 1;
    if (!k || r.battery == 255 || r.battery >= 4)
      usb_low[i] = 0;
    else if (r.battery <= 2 && !usb_low[i] && !(r.flags & MXPAD_CABLE)) {
      usb_low[i] = 1;
      mx_snprintf(what, sizeof what, "%s: battery at %d%%", pad_name(k), r.battery * 10);
      knotify_add(MX_NOTIFY_PAD, 0, MX_NOTIFY_ACTION_PADS, "", "Battery low", what);
      changed = 1;
    }
  }
  if (sio_quiet)
    sio_quiet--;
  for (int p = 0; ports_open && p < 2; p++) {
    int there = sio_state(p) != 0;
    /* a change counts when two checks in a row see it, and not right after
       a switch or the ports opened (sio_quiet) */
    int steady = there == sio_seen[p];
    sio_seen[p] = (u8)there;
    if (sio_quiet) {
      sio_known[p] = (u8)there;
      continue;
    }
    if (there != sio_known[p] && steady) {
      mx_snprintf(what, sizeof what, "Controller port %d", p + 1);
      tell(there ? "Controller connected" : "Controller disconnected", what,
           there ? player_of(1, p) : 0);
      sio_known[p] = (u8)there;
      changed = 1;
    }
  }
  if (usb_on) {
    pad_colours();
    pad_idle();
    /* the pairing's progress: while pairing, and after a USB pad came (it
       pairs by its cable) */
    if (pair_watch) {
      pair_watch--;
      if (CMD_CALL(MXPAD_PAIR_STATE, 16, 16) >= 0 || MX_UNCACHED(usb.cout)[0] != (u32)-1) {
        struct mx_pad_pairing p;
        pairing_of(MX_UNCACHED(usb.cout), &p);
        if (p.seq != pair_seq_seen) {
          pair_seq_seen = p.seq;
          changed = 1;
          if (p.state == MXPAD_PAIR_DONE)
            knotify_add(MX_NOTIFY_PAD, 0, MX_NOTIFY_ACTION_PADS, "", "Controller paired",
                        pad_name(p.kind == MX_PAD_KIND_DS3 ? MXPAD_DS3 : p.kind == MX_PAD_KIND_DS4 ? MXPAD_DS4 : p.kind == MX_PAD_KIND_DS5 ? MXPAD_DS5 : 0));
          else if (p.state == MXPAD_PAIR_FAILED)
            knotify_add(MX_NOTIFY_PAD, 0, MX_NOTIFY_ACTION_PADS, "", "Pairing failed", "Try again: hold the pad's pairing buttons");
        }
      }
    }
  }
  if (changed) {
    struct mx_event e = {MX_EVENT_PAD, 0, (s32)++pad_changes, 0};
    kmsg_post_all(&e, sizeof e);
  }
}
