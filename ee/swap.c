/* MXOS's swap file (kernel version 26, mx OS plan 4e; <mx/dev.h>).

   One swap file for every program, where mc0:/MXOS/SWAP.CFG says (on PCSX2
   host:swap.txt), or none. It is attached when its device is there (at the
   boot, or when the device comes: nothing waits for it), and the user is
   told through notifications: the file was made, it is in use (do not
   remove the device: urgent while it is), its device went (the pages that
   were only in it are lost), or there is none (MXOS soon runs out of
   memory). mx_swap_set (the shell) chooses another place or none. */

#include <mx/dev.h>
#include <mx/setting.h>
#include <mx/sif.h>
#include <mx/notify.h>
#include <mx/string.h>

#include "mxlink.h"

static u32 state = MX_SWAP_NONE;
static int warn_id;  /* the urgent notification while it is: in use, or none */
static char device[12];
static char attached_path[MX_SETTING_VALUE]; /* the file attach() opened */

static void device_of(const char *path, char *to, u32 room) {
  u32 n = 0;
  while (path[n] && path[n] != ':' && n + 2 < room)
    to[n] = path[n], n++;
  to[n] = path[n] == ':' ? ':' : 0;
  to[n + 1] = 0;
}

static void resolve(void) {
  if (warn_id)
    knotify_resolve(warn_id);
  warn_id = 0;
}

static void none_warning(void) {
  resolve();
  warn_id = knotify_add(MX_NOTIFY_SWAP, 1, MX_NOTIFY_ACTION_SWAP, "", "No swap file",
                        "MXOS runs without a swap file: programs will soon run out of memory. "
                        "Choose a place for it.");
}

/* the setting swap.iop (kernel version 49): the swap file through mxswap.irx
   on the IOP, compressed (maxsdk's vm.c), from its next attach */
static void iop_mode(void) {
  extern int mx__vm_swap_iop;
#ifdef SWAP_IOP_TEST /* a test build (make emu SWAP_IOP_TEST=1): on unless set off */
  char v[8];
  mx__vm_swap_iop = !(ksetting_get("swap.iop", v, sizeof v) > 0 && !strcmp(v, "off"));
#ifdef MXLINK_EMU
  mx__vm_swap_iop *= 2; /* PCSX2's host: files work through the IOP */
#endif
#else
  mx__vm_swap_iop = ksetting_on("swap.iop");
#endif
}

/* the file attached to the VM: 0, or -1 */
static int attach(void) {
  iop_mode();
  u32 bytes;
  const char *path = kswap_path(&bytes);
  char text[MX_NOTIFY_TEXT];
  if (!path || !kvm_on())
    return -1;
  int fd = mx_fio_open(path, MX_FIO_RDONLY);
  int made = fd < 0;
  if (fd >= 0)
    mx_fio_close(fd);
  state = MX_SWAP_CREATING;
  mx__vm_swap_size(bytes);
  extern const char *mx__loader_swap;
  mx__loader_swap = path;
  if (mx__vm_swap_open(path) < 0 || !mx__vm_has_swap()) {
    state = MX_SWAP_FAILED;
    mx_snprintf(text, sizeof text, "%s could not be %s. MXOS runs without one.", path,
                made ? "made" : "opened");
    resolve();
    warn_id = knotify_add(MX_NOTIFY_SWAP, 1, MX_NOTIFY_ACTION_SWAP, "", "Swap file failed", text);
    return -1;
  }
  state = MX_SWAP_ATTACHED;
  mx_snprintf(attached_path, sizeof attached_path, "%s", path);
  if (made) {
    mx_snprintf(text, sizeof text, "%s (%u MB)", path, bytes >> 20);
    knotify_add(MX_NOTIFY_SWAP, 0, MX_NOTIFY_ACTION_SWAP, "", "Swap file created", text);
  }
  mx_snprintf(text, sizeof text, "The swap file is on %s: do not remove %s.", device, device);
  resolve();
  warn_id = knotify_add(MX_NOTIFY_SWAP, 1, MX_NOTIFY_ACTION_SWAP, "", "Swap file in use", text);
  return 0;
}

/* after the boot's VM and device watch: the setting read, attached if its
   device is there (else when it comes, kswap_device) */
void kswap_boot(void) {
  iop_mode();
  extern int mx__vm_swap_iop;
  extern void mx__vm_swap_iop_buf(void);
  if (mx__vm_swap_iop)
    mx__vm_swap_iop_buf(); /* its buffer while the pool is empty (vm.c) */
  const char *path = kswap_path(0);
  if (!path || !path[0]) {
    state = MX_SWAP_NONE, device[0] = 0;
    none_warning();
    return;
  }
  device_of(path, device, sizeof device);
  state = MX_SWAP_WAITING;
  /* host: and a device mxio does not watch: tried now */
  if (!strncmp(device, "host:", 5) || dev_present(device))
    attach();
}

/* after INIT.ELF (main.c): its swap.path and swap.size, when they differ
   from the file in use */
void kswap_reload(void) {
  u32 mib = 0;
  const char *path = kswap_path(&mib);
  (void)mib;
  if (state == MX_SWAP_ATTACHED && path && !strcmp(path, attached_path))
    return;
  if (state == MX_SWAP_ATTACHED && mx__vm_swap_detach() < 0)
    return; /* it holds pages already: the next start */
  kswap_boot();
}

/* the command thread: a device came or went */
void kswap_device(const char *name, int came) {
  if (!device[0] || strcmp(name, device))
    return;
  if (came && (state == MX_SWAP_WAITING || state == MX_SWAP_FAILED)) {
    attach();
  } else if (!came && state == MX_SWAP_ATTACHED) {
    mx__vm_swap_gone();
    state = MX_SWAP_WAITING;
    resolve();
    char text[MX_NOTIFY_TEXT];
    mx_snprintf(text, sizeof text,
                "%s was removed while it held the swap file: a program whose memory was in it "
                "ends. Put it back to use it again.",
                device);
    warn_id = knotify_add(MX_NOTIFY_SWAP, 1, MX_NOTIFY_ACTION_SWAP, "", "Swap device removed", text);
  }
}

/* the swap file is on device name (MX_DEV_SWAP) */
int kswap_on(const char *name) { return state == MX_SWAP_ATTACHED && !strcmp(name, device); }

static int has_colon(const char *p) {
  while (*p && *p != ':')
    p++;
  return *p == ':';
}

/* <mx/dev.h>: only the shell chooses */
int kswap_set(const char *path, u32 mib) {
  if (!kproc_shell())
    return -1;
  if (!path)
    path = "";
  if (strlen(path) >= sizeof(((struct mx_swap_info *)0)->path) || (path[0] && !has_colon(path)))
    return -1;
  if (!mib)
    mib = 64;
  u32 now_mib = 0;
  const char *now = kswap_path(&now_mib);
  if (now && !strcmp(now, path) && now_mib >> 20 == mib && state == MX_SWAP_ATTACHED)
    return 0;
  /* the old file: closed if it holds no page; else the new place counts
     from the next start */
  if (state == MX_SWAP_ATTACHED && mx__vm_swap_detach() < 0) {
    if (kswap_path_set(path, mib) < 0)
      return -1;
    knotify_add(MX_NOTIFY_SWAP, 0, MX_NOTIFY_ACTION_SWAP, "", "Swap file",
                "The swap file holds memory of running programs: the new place is used after "
                "the next start of the PS2.");
    return 0;
  }
  if (kswap_path_set(path, mib) < 0)
    return -1;
  resolve();
  if (!path[0]) {
    state = MX_SWAP_NONE, device[0] = 0;
    none_warning();
    return 0;
  }
  device_of(path, device, sizeof device);
  state = MX_SWAP_WAITING;
  if (!strncmp(device, "host:", 5) || dev_present(device))
    attach();
  return 0;
}

int kswap_info(struct mx_swap_info *s) {
  memset(s, 0, sizeof *s);
  u32 bytes = 0;
  const char *path = kswap_path(&bytes);
  if (path)
    memcpy(s->path, path, strlen(path) < sizeof s->path ? strlen(path) + 1 : sizeof s->path - 1);
  s->mib = bytes >> 20;
  s->state = state;
  memcpy(s->device, device, sizeof s->device);
  return 0;
}

/* ---- a host's run on its own swap file (host:) ----------------------------
   A program the host link (the service) starts in front may bring a swap
   file on host: ("host:<path> [MiB]", mxlink's app from host:mxswap.txt):
   for that run it is the VM's swap file in place of MXOS's (the shell is
   ended for a host's run, so MXOS's holds little; if it holds pages, it
   stays). At the run's end MXOS's comes back. No notifications: the host's. */
static int host_run;      /* the host's file attached for the run in front */
static char host_path[64];

int kswap_host(const char *spec) {
  if (!spec || strncmp(spec, "host:", 5) || !kvm_on())
    return -1;
  u32 n = 0, mib = 0;
  while (spec[n] && spec[n] != ' ' && n + 1 < sizeof host_path)
    host_path[n] = spec[n], n++;
  host_path[n] = 0;
  for (const char *p = spec + n; *p; p++)
    if (*p >= '0' && *p <= '9')
      mib = mib * 10 + (u32)(*p - '0');
  if (!mib)
    mib = 128;
  if (state == MX_SWAP_ATTACHED && mx__vm_swap_detach() < 0) {
    con_log(CON_YELLOW, "swap: MXOS's file holds pages: the run keeps it, not %s", host_path);
    return -1;
  }
  if (state == MX_SWAP_ATTACHED)
    state = MX_SWAP_WAITING;
  mx__vm_swap_size(mib << 20);
  iop_mode();
  extern const char *mx__loader_swap;
  mx__loader_swap = host_path;
  if (mx__vm_swap_open(host_path) < 0 || !mx__vm_has_swap()) {
    con_log(CON_RED, "swap: %s could not be opened: the run has no swap file", host_path);
    mx__loader_swap = 0;
    kswap_host_end();
    return -1;
  }
  host_run = 1;
  con_log(CON_GREY, "swap: %s, %u MiB, for the host's run", host_path, mib);
  return 0;
}

/* the run in front ended (main.c): MXOS's file again, where its device is */
void kswap_host_end(void) {
  if (host_run) {
    host_run = 0;
    /* the pages of the processes that stay (the service's) that went out
       into it: back in frames, as the file goes with the run */
    if (mx__vm_swap_drain() < 0)
      con_log(CON_RED, "swap: no frames to take %s's pages back", host_path);
    if (mx__vm_swap_detach() < 0) {
      /* never left attached: its host side ends with the run */
      con_log(CON_RED, "swap: %s still holds pages: they are lost", host_path);
      mx__vm_swap_gone();
    }
  }
  if (mx__vm_has_swap())
    return;
  u32 bytes = 0;
  const char *path = kswap_path(&bytes);
  if (!path || !path[0])
    return;
  device_of(path, device, sizeof device);
  if (state != MX_SWAP_FAILED)
    state = MX_SWAP_WAITING;
  if (!strncmp(device, "host:", 5) || dev_present(device)) {
    /* quietly: it was attached before the run (its notification stands) */
    mx__vm_swap_size(bytes);
    extern const char *mx__loader_swap;
    mx__loader_swap = path;
    if (mx__vm_swap_open(path) == 0 && mx__vm_has_swap())
      state = MX_SWAP_ATTACHED;
  }
}
