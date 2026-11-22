/* The settings store (kernel version 27; <mx/setting.h>).

   A text file in the kernel's directory (mc0:/MXOS/): a first line
   "MXOS settings 1", then "seq=N", then "key=value" lines, then "end". It is
   written as two copies in turn, SETTINGS.DAT and SETTINGS.NEW: a write goes
   to the one that is not the newest, so a power cut while writing leaves the
   other complete; at the start the complete one with the higher seq wins.

   The kernel reads it at its first start and keeps it in .padarea (above its
   code, not cleared by a restart; written through the uncached alias), with
   a magic so that a store a kernel before this one left is not taken. The
   shell's changes are written a few seconds after the last one (the command
   thread, settings_tick).

   Without a store yet, the files it replaces are read once:
   IPCONFIG.DAT (net.ip, net.mask, net.gw), SWAP.CFG (swap.path, swap.size),
   SHELL.CFG (shell.path), SERVICES.CFG (service). */

#include <mx/cpu.h>
#include <mx/proc.h>
#include <mx/setting.h>
#include <mx/sif.h>
#include <mx/string.h>

#include "mxlink.h"

#define NSET 64
#define MAGIC 0x53455431u /* "SET1" */
struct entry {
  char key[MX_SETTING_KEY];
  char value[MX_SETTING_VALUE];
  u32 seq;
};
static struct {
  u32 magic, n, seq, file_seq;
  u32 saved; /* seq when last written or read: a change after it is not on the card */
  char dir[64]; /* the kernel's directory, with its "/" (kept: .bss is cleared by the restart) */
  struct entry e[NSET];
} store __attribute__((section(".padarea"), aligned(64)));

static volatile typeof(store) *st(void) { return MX_UNCACHED(&store); }

/* Settings for this boot only (kernel version 46): what INIT.ELF set with
   mx_setting_set (main.c's init). Read before the store, never written to
   SETTINGS.DAT; the shell's own change of a key drops it here. Kept over
   the kernel's restart, emptied at its first start. */
#define NBOOT 24
static struct {
  u32 magic, n;
  struct {
    char key[MX_SETTING_KEY];
    char value[MX_SETTING_VALUE];
  } e[NBOOT];
} boot __attribute__((section(".padarea"), aligned(64)));
static volatile typeof(boot) *bt(void) { return MX_UNCACHED(&boot); }
static int boot_find(const char *key) {
  volatile typeof(boot) *b = bt();
  if (b->magic != MAGIC)
    return -1;
  for (u32 i = 0; i < b->n && i < NBOOT; i++) {
    char k[MX_SETTING_KEY];
    for (u32 c = 0; c < sizeof k; c++)
      k[c] = b->e[i].key[c];
    k[sizeof k - 1] = 0;
    if (!strcmp(k, key))
      return (int)i;
  }
  return -1;
}
static void boot_drop(const char *key) {
  int i = boot_find(key);
  if (i >= 0)
    bt()->e[i].key[0] = 0, bt()->e[i].value[0] = 0;
}

/* not written yet: kept in the store, as the kernel restarts after its
   first start (.bss cleared) */
static u32 dirty_at; /* seconds of the last change */
static u32 retry_at; /* after a failed write: not before then */
static int dirty(void) { return st()->magic == MAGIC && st()->seq != st()->saved; }

static void copy_out(char *to, const volatile char *from, u32 n) {
  for (u32 i = 0; i < n; i++)
    to[i] = from[i];
}
static void copy_in(volatile char *to, const char *from, u32 n) {
  u32 i = 0;
  for (; i < n - 1 && from[i]; i++)
    to[i] = from[i];
  for (; i < n; i++)
    to[i] = 0;
}

static int find(const char *key) {
  volatile typeof(store) *s = st();
  for (u32 i = 0; i < s->n; i++) {
    char k[MX_SETTING_KEY];
    copy_out(k, s->e[i].key, sizeof k);
    k[sizeof k - 1] = 0;
    if (!strcmp(k, key))
      return (int)i;
  }
  return -1;
}

int ksetting_get(const char *key, char *value, u32 size) {
  int b = boot_find(key);
  if (b >= 0 && bt()->e[b].value[0]) {
    char v[MX_SETTING_VALUE];
    for (u32 c = 0; c < sizeof v; c++)
      v[c] = bt()->e[b].value[c];
    v[sizeof v - 1] = 0;
    u32 l = (u32)strlen(v);
    if (size) {
      u32 n = l < size - 1 ? l : size - 1;
      memcpy(value, v, n);
      value[n] = 0;
    }
    return (int)l;
  }
  int i = find(key);
  if (i < 0 || !size)
    return -1;
  char v[MX_SETTING_VALUE];
  copy_out(v, st()->e[i].value, sizeof v);
  v[sizeof v - 1] = 0;
  if (!v[0])
    return -1; /* removed */
  u32 n = (u32)strlen(v);
  if (n >= size)
    n = size - 1;
  memcpy(value, v, n);
  value[n] = 0;
  return (int)n;
}

/* "on", "1", "yes": 1 */
int ksetting_on(const char *key) {
  char v[8];
  if (ksetting_get(key, v, sizeof v) <= 0)
    return 0;
  return !strcmp(v, "on") || !strcmp(v, "1") || !strcmp(v, "yes");
}

/* the keys the kernel keeps elsewhere too, as they change */
static void apply(const char *key) {
  if (!strcmp(key, "pad.swap_ports"))
    pad_settings.swap_ports = (u8)ksetting_on(key);
  else if (!strcmp(key, "pad.swap_xo"))
    pad_settings.swap_xo = (u8)ksetting_on(key);
  else if (!strncmp(key, "net.", 4))
    knet_setting();
  else if (!strncmp(key, "video.", 6))
    kvideo_setting();
}

/* the kernel's own (no event while loading: quiet) */
static int set(const char *key, const char *value, int quiet) {
  if (!value)
    value = "";
  u32 kl = (u32)strlen(key), vl = (u32)strlen(value);
  if (!kl || kl >= MX_SETTING_KEY || vl >= MX_SETTING_VALUE)
    return -1;
  for (u32 i = 0; i < kl; i++)
    if (key[i] <= ' ' || key[i] == '=')
      return -1;
  for (u32 i = 0; i < vl; i++)
    if (value[i] == '\n' || value[i] == '\r')
      return -1;
  volatile typeof(store) *s = st();
  int i = find(key);
  if (i >= 0) {
    char v[MX_SETTING_VALUE];
    copy_out(v, s->e[i].value, sizeof v);
    v[sizeof v - 1] = 0;
    if (!strcmp(v, value))
      return 0;
  } else {
    if (!vl)
      return 0;
    if (s->n >= NSET) {
      /* a removed one's place */
      for (u32 k = 0; k < s->n && i < 0; k++)
        if (!s->e[k].value[0])
          i = (int)k;
      if (i < 0)
        return -1;
    } else
      i = (int)s->n++;
    copy_in(s->e[i].key, key, MX_SETTING_KEY);
  }
  copy_in(s->e[i].value, value, MX_SETTING_VALUE);
  s->e[i].seq = ++s->seq;
  apply(key);
  if (!quiet) {
    dirty_at = kseconds();
    struct mx_event e = {MX_EVENT_SETTING, 0, (s32)s->seq, 0};
    kmsg_post_all(&e, sizeof e);
  }
  return 0;
}

/* a setting of the kernel's (the home menu's pad settings, mx_swap_set) */
int ksetting_set(const char *key, const char *value) {
  boot_drop(key);
  return set(key, value, 0);
}

/* INIT.ELF's: for this boot only (the boot layer) */
static int boot_set(const char *key, const char *value) {
  if (!value)
    value = "";
  u32 kl = (u32)strlen(key), vl = (u32)strlen(value);
  if (!kl || kl >= MX_SETTING_KEY || vl >= MX_SETTING_VALUE)
    return -1;
  volatile typeof(boot) *b = bt();
  if (b->magic != MAGIC)
    b->magic = MAGIC, b->n = 0;
  int i = boot_find(key);
  for (u32 k = 0; i < 0 && k < b->n; k++)
    if (!b->e[k].key[0])
      i = (int)k;
  if (i < 0) {
    if (b->n >= NBOOT)
      return -1;
    i = (int)b->n++;
  }
  copy_in(b->e[i].key, key, MX_SETTING_KEY);
  copy_in(b->e[i].value, value, MX_SETTING_VALUE);
  apply(key);
  return 0;
}

/* the kernel's own for this boot only (system.root found without the
   setting, kernel version 47) */
int ksetting_set_boot(const char *key, const char *value) { return boot_set(key, value); }

/* <mx/setting.h>: the shell; INIT.ELF for this boot only */
int ksetting_set_shell(const char *key, const char *value) {
  if (kproc_init())
    return boot_set(key, value);
  if (!kproc_shell())
    return -1;
  boot_drop(key);
  return set(key, value, 0);
}

int ksetting_list(u32 after, struct mx_setting *out, int max) {
  volatile typeof(store) *s = st();
  int got = 0;
  /* in the order of their changes */
  for (u32 below = after; got < max;) {
    int bi = -1;
    u32 best = ~0u;
    for (u32 i = 0; i < s->n; i++)
      if (s->e[i].seq > below && s->e[i].seq < best)
        best = s->e[i].seq, bi = (int)i;
    if (bi < 0)
      break;
    struct mx_setting t;
    copy_out(t.key, s->e[bi].key, sizeof t.key);
    copy_out(t.value, s->e[bi].value, sizeof t.value);
    t.key[sizeof t.key - 1] = t.value[sizeof t.value - 1] = 0;
    t.seq = best;
    out[got++] = t;
    below = best;
  }
  return got;
}

u32 ksetting_seq(void) { return st()->seq; }

/* ---- the file ------------------------------------------------------------- */

#ifdef MXLINK_EMU
#define FILE_A "host:settings.dat"
#define FILE_B "host:settings.new"
#else
#define FILE_A "SETTINGS.DAT"
#define FILE_B "SETTINGS.NEW"
#endif


static void path_of(char *to, u32 room, const char *name) {
#ifdef MXLINK_EMU
  (void)room;
  strcpy(to, name);
#else
  char d[sizeof store.dir];
  copy_out(d, st()->dir, sizeof d);
  d[sizeof d - 1] = 0;
  mx_snprintf(to, room, "%s%s", d, name);
#endif
}

/* one copy: its seq when complete, else 0; its lines set when take */
/* above the code with the store (the room below it is the stack's): that
   is read-only through the TLB, so used through the uncached alias, as the
   store */
static char buf_[4096] __attribute__((section(".padarea"), aligned(64)));
#define buf ((char *)MX_UNCACHED(buf_))
static u32 parse(const char *path, int take) {
  int fd = mx_fio_open(path, MX_FIO_RDONLY);
  if (fd < 0)
    return 0;
  int n = mx_fio_read(fd, buf, sizeof buf_ - 1);
  mx_fio_close(fd);
  if (n <= 0)
    return 0;
  buf[n] = 0;
  if (strncmp(buf, "MXOS settings 1\n", 16))
    return 0;
  u32 seq = 0;
  int ended = 0;
  for (char *l = buf + 16; *l && !ended;) {
    char *e = l;
    while (*e && *e != '\n')
      e++;
    char c = *e;
    *e = 0;
    if (!strcmp(l, "end"))
      ended = 1;
    else if (!strncmp(l, "seq=", 4))
      for (const char *p = l + 4; *p >= '0' && *p <= '9'; p++)
        seq = seq * 10 + (u32)(*p - '0');
    else if (take) {
      char *eq = l;
      while (*eq && *eq != '=')
        eq++;
      if (*eq) {
        *eq = 0;
        set(l, eq + 1, 1);
        *eq = '=';
      }
    }
    *e = c;
    l = c ? e + 1 : e;
  }
  return ended ? (seq ? seq : 1) : 0;
}

/* the old files, once: a file's words into keys */
static int words(const char *name, char *w, u32 room) {
  char p[96];
  path_of(p, sizeof p, name);
  int fd = mx_fio_open(p, MX_FIO_RDONLY);
  int n = fd < 0 ? -1 : mx_fio_read(fd, w, room - 1);
  if (fd >= 0)
    mx_fio_close(fd);
  if (n <= 0)
    return 0;
  w[n] = 0;
  /* one line: the first, its spaces kept */
  for (int i = 0; i < n; i++)
    if (w[i] == '\r' || w[i] == '\n')
      w[i] = 0;
  return w[0] != 0;
}
/* PCSX2: a test run's files decide (a file it removed: the key removed) */
#ifdef MXLINK_EMU
#define NONE(key) set(key, "", 1)
#else
#define NONE(key) ((void)0)
#endif
static void import(void) {
  char w[128];
#ifndef MXLINK_EMU
  if (words("IPCONFIG.DAT", w, sizeof w)) {
    const char *keys[3] = {"net.ip", "net.mask", "net.gw"};
    char *s = w;
    for (int k = 0; k < 3; k++) {
      while (*s == ' ' || *s == '\t')
        s++;
      char *e = s;
      while (*e && *e != ' ' && *e != '\t')
        e++;
      char c = *e;
      *e = 0;
      if (*s)
        set(keys[k], s, 1);
      *e = c;
      s = e;
    }
  }
#endif
  if (words(
#ifdef MXLINK_EMU
          "host:swap.txt",
#else
          "SWAP.CFG",
#endif
          w, sizeof w)) {
    char *e = w;
    while (*e && *e != ' ')
      e++;
    if (*e) {
      *e = 0;
      set("swap.size", e + 1, 1);
    } else
      NONE("swap.size");
    set("swap.path", w, 1);
  } else
    NONE("swap.path"), NONE("swap.size");
  if (words(
#ifdef MXLINK_EMU
          "host:shell.txt",
#else
          "SHELL.CFG",
#endif
          w, sizeof w))
    set("shell.path", w, 1);
  else
    NONE("shell.path");
#ifdef MXLINK_EMU
  /* PCSX2's test runs: the system folder (kernel version 47) */
  if (words("host:root.txt", w, sizeof w))
    set("system.root", w, 1);
  else
    NONE("system.root");
#endif
  if (words(
#ifdef MXLINK_EMU
          "host:services.txt",
#else
          "SERVICES.CFG",
#endif
          w, sizeof w))
    set("service", w, 1);
  else
    NONE("service");
}

/* the first start (main.c's read_conf, the card readable): the store read
   from dir (the kernel's directory, with its "/"), the old files if there
   is none; a restart keeps what is there */
void settings_load(const char *kernel_dir) {
  volatile typeof(store) *s = st();
  copy_in(s->dir, strlen(kernel_dir) < sizeof store.dir ? kernel_dir : "", sizeof store.dir);
  s->magic = 0, s->n = 0, s->seq = 0, s->file_seq = 0, s->saved = 0;
  bt()->magic = 0, bt()->n = 0;
  char pa[96], pb[96];
  path_of(pa, sizeof pa, FILE_A);
  path_of(pb, sizeof pb, FILE_B);
  u32 a = parse(pa, 0), b = parse(pb, 0);
  if (a || b) {
    parse(a >= b ? pa : pb, 1);
    s->file_seq = a > b ? a : b;
    s->saved = s->seq;
#ifdef MXLINK_EMU
    import(); /* PCSX2: a run's host:*.txt over the store */
#endif
  } else
    import(); /* written soon (saved 0) */
  dirty_at = 0;
  s->magic = MAGIC;
  con_log(CON_GREY, "settings: %u (%s)", s->n, a || b ? "SETTINGS.DAT" : "first start");
}

/* the kernel's restart: the store as it was (main.c) */
int settings_kept(void) { return st()->magic == MAGIC; }

/* the command thread, twice a second: a change written once 3 s passed */
void settings_tick(void) {
  /* the home menu's toggles (pad_settings, in a program's thread) */
  if (st()->magic == MAGIC) {
    if (pad_settings.swap_ports != ksetting_on("pad.swap_ports"))
      ksetting_set("pad.swap_ports", pad_settings.swap_ports ? "on" : "off");
    if (pad_settings.swap_xo != ksetting_on("pad.swap_xo"))
      ksetting_set("pad.swap_xo", pad_settings.swap_xo ? "on" : "off");
  }
  if (!dirty() || kseconds() - dirty_at < 3 || kseconds() < retry_at)
    return;
  volatile typeof(store) *s = st();
  u32 now = s->seq;
  u32 seq = s->file_seq + 1;
  /* the copy that is not the newest */
  char pa[96], pb[96];
  path_of(pa, sizeof pa, FILE_A);
  path_of(pb, sizeof pb, FILE_B);
  u32 a = parse(pa, 0), b = parse(pb, 0);
  const char *to = a > b ? pb : pa;
  u32 len = (u32)mx_snprintf(buf, sizeof buf_, "MXOS settings 1\nseq=%u\n", seq);
  for (u32 i = 0; i < s->n && len + MX_SETTING_KEY + MX_SETTING_VALUE + 8 < sizeof buf_; i++) {
    struct mx_setting t;
    copy_out(t.key, s->e[i].key, sizeof t.key);
    copy_out(t.value, s->e[i].value, sizeof t.value);
    t.key[sizeof t.key - 1] = t.value[sizeof t.value - 1] = 0;
    if (t.value[0])
      len += (u32)mx_snprintf(buf + len, sizeof buf_ - len, "%s=%s\n", t.key, t.value);
  }
  len += (u32)mx_snprintf(buf + len, sizeof buf_ - len, "end\n");
  int fd = mx_fio_open(to, MX_FIO_WRONLY | MX_FIO_CREAT | MX_FIO_TRUNC);
  /* mx_fio_write: 0 when all was written (not the count); the close
     writes the card's cache back */
  int w = fd < 0 ? -1 : mx_fio_write(fd, buf, len);
  if (fd >= 0 && mx_fio_close(fd) < 0)
    w = -1;
  if (w < 0) {
    con_log(CON_RED, "settings: %s could not be written (%d %d)", to, fd, w);
    /* all of the card driver's files taken (-24, EMFILE): soon again; else the
       card may be out: not every 3 s */
    retry_at = kseconds() + (fd == -24 ? 2 : 30);
    return;
  }
  s->file_seq = seq;
  s->saved = now;
}
