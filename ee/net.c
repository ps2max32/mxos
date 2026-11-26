/* The network's state and set-up (kernel version 30, <mx/net.h>'s
   mx_net_status, common/include/mxnet.h).

   mxsock looks at the IOP's stack twice a second and writes its state here
   by DMA when it changed (MXSOCK_NET_WATCH). The vblank hook (dev.c) sees
   the counter change and wakes the command thread: MX_EVENT_NET to the
   processes with events, a notification when the link or the address
   changed, the screen's network line.

   The set-up comes from the settings: net.mode "dhcp", or a fixed address
   (net.ip, net.mask, net.gw; mxnet starts with these, main.c's if_conf) and
   net.dns. The kernel applies it at its start when the stack differs, and a
   second after the shell's last change of a net.* key. The host link's
   sockets are bound to any address: it follows. */

#include <mx/cpu.h>
#include <mx/net.h>
#include <mx/notify.h>
#include <mx/proc.h>
#include <mx/setting.h>
#include <mx/sif.h>
#include <mx/string.h>

#include <mxnet.h>
#include <mxsock.h>

#include "mxlink.h"

#ifdef MXLINK_EMU
/* PCSX2's kernel loads no mxsock: no network (and the room for the code) */
void knet_start(void) {}
void knet_report(void) {}
void knet_setting(void) {}
int knet_changed(void) { return 0; }
void knet_now(void) {}
int knet_status(struct mx_net_status *s) {
  memset(s, 0, sizeof *s);
  return 0;
}
int knet_addr(u32 *ip, u32 *mask, u32 *gw, int *dhcp) {
  (void)ip, (void)mask, (void)gw, (void)dhcp;
  return 0;
}
#else

#define MAGIC 0x4D584E54 /* "MXNT" */

/* kept across the kernel's restarts: what was last told (no news again
   after each program) */
static struct {
  struct mxnet_status st; /* mxsock's DMA */
  struct mxnet_status told;
  u32 magic, told_at; /* told_at: kseconds() when told was taken (the lease) */
  /* SIF DMA's: 16-byte blocks, so on their own lines (after magic and
     told_at, 8 bytes past one, MXSOCK_NET_WATCH's answer was not seen and
     mxsock took magic for the address: kernel versions 30 to 50) */
  u32 in[16] __attribute__((aligned(64))), out[16] __attribute__((aligned(64)));
} area __attribute__((section(".padarea"), aligned(64)));

static struct mx_sif_client sock_rpc;
static int net_on;
static u32 net_seen; /* the gen the command thread acted on */
static u32 apply_at; /* kseconds() + 1 after a net.* change; 0 none */
static int start_check; /* the settings against the stack's first state */

static volatile struct mxnet_status *st(void) { return MX_UNCACHED(&area.st); }
static volatile typeof(area) *ar(void) { return MX_UNCACHED(&area); }

/* "a.b.c.d" in network order; 0 when it is not one */
static u32 addr_of(const char *s) {
  u32 a = 0, part = 0, n = 0, digits = 0;
  for (;; s++) {
    if (*s >= '0' && *s <= '9') {
      part = part * 10 + (u32)(*s - '0');
      if (part > 255 || ++digits > 3)
        return 0;
    } else if ((*s == '.' || !*s) && digits && n < 4) {
      a |= part << (8 * n++);
      part = digits = 0;
      if (!*s)
        break;
    } else
      return 0;
  }
  return n == 4 ? a : 0;
}

static u32 setting_addr(const char *key) {
  char v[MX_SETTING_VALUE];
  return ksetting_get(key, v, sizeof v) > 0 ? addr_of(v) : 0;
}

static int dhcp_wanted(void) {
  char v[MX_SETTING_VALUE];
  return ksetting_get("net.mode", v, sizeof v) > 0 && !strcmp(v, "dhcp");
}

/* the settings to the stack; only what differs (force: anyway) */
static void apply(int force) {
  if (!net_on)
    return;
  struct mxnet_config c;
  c.dhcp = (u32)dhcp_wanted();
  c.ip = setting_addr("net.ip"), c.mask = setting_addr("net.mask"), c.gw = setting_addr("net.gw");
  c.dns = setting_addr("net.dns");
  volatile struct mxnet_status *s = st();
  if (!c.dhcp && !c.ip) {
    /* no fixed address set: the one mxnet started with */
    c.ip = s->ip, c.mask = s->mask, c.gw = s->gw;
  }
  if (!force && s->version) {
    if (c.dhcp && s->dhcp != MXNET_DHCP_OFF && (!c.dns || c.dns == s->dns))
      return;
    if (!c.dhcp && s->dhcp == MXNET_DHCP_OFF && c.ip == s->ip && c.mask == s->mask &&
        c.gw == s->gw && c.dns == s->dns)
      return;
  }
  volatile u32 *in = MX_UNCACHED(area.in), *out = MX_UNCACHED(area.out);
  in[0] = c.dhcp, in[1] = c.ip, in[2] = c.mask, in[3] = c.gw, in[4] = c.dns;
  out[0] = (u32)-1;
  if (mx_sif_call(&sock_rpc, MXSOCK_NET_CONFIG, (void *)in, 64, (void *)out, 16) < 0)
    con_log(CON_RED, "network: the set-up did not reach the IOP");
  else
    con_log(CON_GREY, "network: %s", c.dhcp ? "DHCP" : "fixed address");
}

/* why knet_start gave up (knet_report): -1 the bind failed, -2 no mxsock
   server, -3 the call failed, -4 another answer (net_answer) */
static int net_why;
static s32 net_answer[4], net_r;

/* once the tty is up (main.c): a network that did not start, said why */
void knet_report(void) {
  static const char *const why[] = {"", "the bind to mxsock failed", "mxsock is not there",
                                    "MXSOCK_NET_WATCH failed", "MXSOCK_NET_WATCH answered"};
  if (!net_on)
    con_log(CON_YELLOW, "network: not started: %s, call %d, answer %d %d %d %d",
            why[net_why < 0 && net_why >= -4 ? -net_why : 0], net_r, (int)net_answer[0],
            (int)net_answer[1], (int)net_answer[2], (int)net_answer[3]);
}

/* after the IOP's set-up (main.c), mxsock loaded: it writes the state here
   from now */
void knet_start(void) {
  net_on = 0;
  if (ar()->magic != MAGIC) {
    memset((void *)ar(), 0, sizeof area);
    ar()->magic = MAGIC;
  }
  volatile u32 *w = (volatile u32 *)st();
  for (u32 i = 0; i < sizeof area.st / 4; i++)
    w[i] = 0;
  net_why = 0;
  if (mx_sif_bind_tries(&sock_rpc, MXSOCK_SID, 2) < 0 || !sock_rpc.server) {
    net_why = sock_rpc.server ? -1 : -2;
    return;
  }
  volatile u32 *in = MX_UNCACHED(area.in), *out = MX_UNCACHED(area.out);
  in[0] = mx_paddr_of(&area.st).v;
  out[0] = 0;
  int r = mx_sif_call(&sock_rpc, MXSOCK_NET_WATCH, (void *)in, 16, (void *)out, 16);
  net_r = r;
  for (int i = 0; i < 4; i++)
    net_answer[i] = (s32)out[i];
  if (r < 0 || out[0] != MXNET_STATUS_VERSION) {
    net_why = r < 0 ? -3 : -4;
    return;
  }
  net_seen = 0;
  net_on = 1;
  start_check = 1; /* when its first state is here (knet_now) */
}

/* settings.c: a net.* key changed (the shell's): applied a second later,
   once for all keys set together */
void knet_setting(void) { apply_at = kseconds() + 1; }

int knet_changed(void) { return net_on && st()->gen != net_seen; }

static void ip_text(char *to, u32 room, u32 a) {
  mx_snprintf(to, room, "%u.%u.%u.%u", a & 255, a >> 8 & 255, a >> 16 & 255, a >> 24);
}

static void tell(const struct mxnet_status *was, const struct mxnet_status *now) {
  char title[MX_NOTIFY_TITLE], text[MX_NOTIFY_TEXT], ip[16];
  if (!was->version)
    return; /* the first state after the boot: not news */
  if (was->link && !now->link) {
    knotify_add(MX_NOTIFY_NET, 0, MX_NOTIFY_ACTION_NET, "", "Network disconnected",
                "No cable or no link");
    return;
  }
  int addressed = now->ip && (now->dhcp != MXNET_DHCP_SEARCHING);
  if (now->link && (!was->link || now->ip != was->ip || now->dhcp != was->dhcp) && addressed) {
    ip_text(ip, sizeof ip, now->ip);
    mx_snprintf(title, sizeof title, "Network connected");
    mx_snprintf(text, sizeof text, "%s%s, %u Mbit/s", ip,
                now->dhcp == MXNET_DHCP_BOUND ? " (DHCP)" : "",
                now->link & MXNET_LINK_100 ? 100 : 10);
    knotify_add(MX_NOTIFY_NET, 0, MX_NOTIFY_ACTION_NET, "", title, text);
  }
}

/* the command thread (dev.c's dev_now) */
void knet_now(void) {
  if (apply_at && kseconds() >= apply_at) {
    apply_at = 0;
    apply(1);
  }
  if (!net_on)
    return;
  volatile struct mxnet_status *v = st();
  u32 gen = v->gen;
  if (gen == net_seen)
    return;
  net_seen = gen;
  if (start_check) {
    start_check = 0;
    apply(0);
  }
  struct mxnet_status now, was;
  const volatile u32 *f = (const volatile u32 *)v;
  for (u32 i = 0; i < sizeof now / 4; i++)
    ((u32 *)&now)[i] = f[i];
  f = (const volatile u32 *)&ar()->told;
  for (u32 i = 0; i < sizeof was / 4; i++)
    ((u32 *)&was)[i] = f[i];
  int same = was.version && now.link == was.link && now.ip == was.ip && now.mask == was.mask &&
             now.gw == was.gw && now.dns == was.dns && now.dhcp == was.dhcp;
  volatile u32 *t = (volatile u32 *)&ar()->told;
  for (u32 i = 0; i < sizeof now / 4; i++)
    t[i] = ((u32 *)&now)[i];
  ar()->told_at = kseconds();
  if (same)
    return;
  tell(&was, &now);
  screen_net();
  struct mx_event e = {MX_EVENT_NET, 0, (s32)gen, 0};
  kmsg_post_all(&e, sizeof e);
}

int knet_status(struct mx_net_status *s) {
  memset(s, 0, sizeof *s);
  if (!net_on)
    return 0;
  volatile struct mxnet_status *v = st();
  u32 link = v->link;
  s->up = 1;
  s->link = link ? 1 : 0;
  s->duplex = link & MXNET_LINK_FDX ? 1 : 0;
  s->speed = !link ? 0 : link & MXNET_LINK_100 ? 100 : 10;
  s->dhcp = (u8)v->dhcp;
  for (int i = 0; i < 6; i++)
    s->mac[i] = v->mac[i];
  s->ip = v->ip, s->mask = v->mask, s->gw = v->gw, s->dns = v->dns;
  u32 lease = v->lease, gone = kseconds() - ar()->told_at;
  s->lease = lease > gone ? lease - gone : 0;
  s->gen = v->gen;
  return 0;
}

/* for the screen: the address now, 0 when the stack is not watched */
int knet_addr(u32 *ip, u32 *mask, u32 *gw, int *dhcp) {
  if (!net_on || !st()->version)
    return 0;
  *ip = st()->ip, *mask = st()->mask, *gw = st()->gw, *dhcp = (int)st()->dhcp;
  return 1;
}
#endif
