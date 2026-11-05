/* mxlink's start: the network configuration, the IOP reset and ps2link's
   IOP modules (the network, the host: file system, the tty), then the
   command thread; the status screen meanwhile.

   As ps2link, mxlink starts twice: the first time (booted from a memory
   card, or started by ps2link or another loader) it reads IPCONFIG.DAT
   next to its ELF with the IOP as the loader left it, keeps it in .data
   (not cleared at a start) and starts itself again by ExecPS2, which
   resets the EE kernel: the loader's threads and interrupt handlers are
   gone. A reset command from the host (mxlink_restart) does the same
   ExecPS2. */

#include <mx/bios.h>
#include <mx/cpu.h>
#include <mx/debug.h>
#include <mx/gs.h>
#include <mx/hw/regs.h>
#include <mx/iop.h>
#include <mx/sif.h>
#include <mx/string.h>

#include "mxlink.h"

/* kept across ExecPS2 (.data is not cleared, .bss is): the configuration
   for ps2ip ("ip\0mask\0gw\0"), and the path mxlink was booted from */
#define CONF_MAX 64
static char if_conf[CONF_MAX] __attribute__((section(".data")));
static u32 if_conf_len __attribute__((section(".data"))) = 0;
static char boot_path[256] __attribute__((section(".data")));
static u32 restarts __attribute__((section(".data"))) = 0;
/* the program's return (mxlink_return): its path and exit status, and the
   IOP's highest module id after mxlink's modules (a higher one after the
   program: it loaded modules, the IOP is reset) */
static char last_path[256] __attribute__((section(".data")));
static u32 returned __attribute__((section(".data"))) = 0;
static s32 last_status __attribute__((section(".data"))) = 0;
static s32 iop_last __attribute__((section(".data"))) = 0;
static u32 returns __attribute__((section(".data"))) = 0; /* programs returned so far */
/* mx_loader_launch: the program to start after the restart */
static s32 launch_argc __attribute__((section(".data"))) = 0;
static u32 launch_len __attribute__((section(".data"))) = 0;
static char launch_args[256] __attribute__((section(".data"))) = {0};
static char launch_swap[64] __attribute__((section(".data"))) = {0};

#define DEFAULT_IP "192.168.1.10"
#define DEFAULT_NETMASK "255.255.255.0"
#define DEFAULT_GW "192.168.1.0"

/* the IOP modules (irx.S, made by the Makefile) */
#define IRX(n) extern const u8 n##_irx[], n##_irx_end[];
IRX(chime) IRX(poweroff) IRX(ps2dev9) IRX(netman) IRX(smap) IRX(ps2ip) IRX(udptty) IRX(ioptrap) IRX(ps2link)
#undef IRX

/* the screen's rows */
#define ROW_TITLE 0
#define ROW_NET 2
#define ROW_HOST 3
#define ROW_MEM 4
#define ROW_STATE 6
#define ROW_LAST 7
#define ROW_HELP 9

static int state = ST_BOOT;
static int chime; /* iop/chime.c's start result, 0: it did not run */

int status_get(void) { return state; }

/* the IOP's status record (iop/cmdHandler.c), which answers ps2client's
   status: over its RPC server, so not while a program runs (it has the SIF
   replies): set before it starts and when mxlink is ready again */
#define NPM_SID 0x014d704eu
#define NPM_STATUS 2
static struct {
  u32 state, returns;
  s32 last_status;
  char path[256];
  u32 pad; /* a multiple of 16 bytes */
} iop_status MX_DMA_LINES;

static void iop_status_send(u32 st) {
  static struct mx_sif_client npm;
  static u32 answer[4] MX_DMA_LINES;
  if (!mx_sif_bound(&npm) && mx_sif_bind_tries(&npm, NPM_SID, 3) < 0)
    return;
  iop_status.state = st;
  iop_status.returns = returns;
  iop_status.last_status = last_status;
  memcpy(iop_status.path, last_path, sizeof iop_status.path);
  mx_sif_call(&npm, NPM_STATUS, &iop_status, sizeof iop_status, (void *)MX_UNCACHED(answer), 0);
}

void status_state(int st, const char *what) {
  state = st;
  if (st == ST_READY)
    iop_status_send(1);
  else if (st == ST_RUNNING)
    iop_status_send(2);
  switch (st) {
  case ST_BOOT:
    con_line(ROW_STATE, CON_YELLOW, "State:  starting - %s", what);
    break;
  case ST_READY:
    con_line(ROW_STATE, CON_GREEN, "State:  ready");
    break;
  case ST_RUNNING:
    con_line(ROW_STATE, CON_CYAN, "State:  running %s", what);
    break;
  }
}

/* "ip mask gw" (spaces, CR, LF between) into if_conf */
static void parse_conf(char *buf) {
  u32 n = 0;
  if_conf_len = 0;
  for (char *s = buf; *s && n < 3;) {
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')
      s++;
    if (!*s)
      break;
    char *w = s;
    while (*s && *s != ' ' && *s != '\t' && *s != '\r' && *s != '\n')
      s++;
    u32 len = (u32)(s - w);
    if (len > 15 || if_conf_len + len + 1 > CONF_MAX)
      break;
    memcpy(if_conf + if_conf_len, w, len);
    if_conf[if_conf_len + len] = 0;
    if_conf_len += len + 1;
    n++;
  }
  if (n == 3)
    return;
  /* incomplete: the defaults */
  if_conf_len = 0;
  const char *def[3] = {DEFAULT_IP, DEFAULT_NETMASK, DEFAULT_GW};
  for (int i = 0; i < 3; i++) {
    u32 len = strlen(def[i]) + 1;
    memcpy(if_conf + if_conf_len, def[i], len);
    if_conf_len += len;
  }
}

/* IPCONFIG.DAT next to the ELF mxlink was booted from */
static void read_conf(void) {
  char path[300], buf[CONF_MAX];
  u32 dir = 0;
  for (u32 i = 0; boot_path[i]; i++)
    if (boot_path[i] == '/' || boot_path[i] == '\\' || boot_path[i] == ':')
      dir = i + 1;
  memcpy(path, boot_path, dir);
  strcpy(path + dir, "IPCONFIG.DAT");
  int n = -1;
  if (dir) {
    int fd = mx_fio_open(path, MX_FIO_RDONLY);
    if (fd >= 0) {
      n = mx_fio_read(fd, buf, sizeof buf - 1);
      mx_fio_close(fd);
    }
  }
  buf[n > 0 ? n : 0] = 0;
  parse_conf(buf);
}

static void restart(void) __attribute__((noreturn));
static void restart(void) {
  extern char __start[], _gp[];
  char *argv[1] = {boot_path};
  restarts++;
  mx_debug_flush();
  mx_bios_flush_cache(0);
  /* ExecPS2 drops the handlers but leaves the channel on: a command from the
     host now would end in the kernel's "DMAC(5) Handler does not exist" and
     its break. cmd_init turns it on again. */
  mx_bios_disable_dmac(MX_DMAC_SIF0);
  mx_bios_exec_ps2(__start, _gp, 1, argv);
  for (;;)
    ;
}

void mxlink_restart(void) {
  exec_kill();
  iop_last = 0; /* ps2link.irx gave up host: for this reset: never kept */
  restart();
}

u32 exec_returns(void) { return returns; }
s32 exec_last_status(void) { return last_status; }

void exec_started(const char *path) {
  u32 n = strlen(path);
  if (n >= sizeof last_path)
    n = sizeof last_path - 1;
  memcpy(last_path, path, n);
  last_path[n] = 0;
}

/* A program's way back (<mx/loader.h>: maxsdk's mx_exit calls it): runs in
   the program, so only this file's .data and a BIOS call. The restart's
   ExecPS2 drops the program's threads, semaphores and handlers. */
void mxlink_return(int status) {
  returned = 1;
  returns++;
  last_status = status;
  restart();
}

void mxlink_launch(int argc, const char *args, u32 args_len, const char *swap) {
  u32 n = swap ? strlen(swap) : 0;
  if (n >= sizeof launch_swap)
    n = 0;
  memcpy(launch_swap, swap ? swap : "", n);
  launch_swap[n] = 0;
  if (args_len > sizeof launch_args)
    args_len = sizeof launch_args;
  memcpy(launch_args, args, args_len);
  launch_len = args_len;
  launch_argc = argc;
  restart();
}

static int load(const char *name, const u8 *start, const u8 *end, const char *args, u32 args_len) {
  int res = 0;
  int id = mx_iop_load_module_buffer(start, (u32)(end - start), args, args_len, &res);
  if (id < 0 || (res & 3) == 1 /* NO_RESIDENT_END: it did not stay */) {
    con_log(CON_RED, "IOP module %s: %d (%d)", name, id, res);
    return -1;
  }
  return 0;
}

/* After a program: the IOP as mxlink left it (ps2link.irx there, no module
   loaded since)? Then it stays, and the network with it: no reset. */
static int iop_kept(void) {
  if (!returned || !iop_last || mx_sif_init() < 0)
    return 0;
  struct mx_iop_module m;
  return mx_iop_last_module() == iop_last && mx_iop_find_module("ps2link", &m) > 0;
}

static int boot_iop(void) {
  /* the modules the program loaded are there for the next one; its return
     then resets the IOP, as they are not mxlink's */
  if (launch_argc)
    return mx_sif_init();
  if (iop_kept())
    return 0;
  if (returned)
    con_log(CON_GREY, "the program changed the IOP: reset");
  status_state(ST_BOOT, "IOP reset");
  if (mx_iop_reset("") < 0) {
    con_log(CON_RED, "the IOP did not come back from its reset");
    return -1;
  }
  mx_iop_patch_lmb();
  mx_iop_patch_prefix_check();
  /* every program that made sound loaded a module, so it gets here */
  if (mx_iop_load_module_buffer(chime_irx, (u32)(chime_irx_end - chime_irx), 0, 0, &chime) < 0 ||
      (chime & 3) != 1)
    con_log(CON_RED, "the chime did not run: a program's sound may go on");
  status_state(ST_BOOT, "IOP modules");
#define LOAD(n, a, l)                                                                              \
  if (load(#n, n##_irx, n##_irx_end, a, l) < 0)                                                    \
    return -1;
  LOAD(poweroff, 0, 0)
  LOAD(ps2dev9, 0, 0)
  LOAD(netman, 0, 0)
  LOAD(smap, 0, 0)
  LOAD(ps2ip, if_conf, if_conf_len)
  LOAD(udptty, 0, 0)
  LOAD(ioptrap, 0, 0)
  LOAD(ps2link, 0, 0)
#undef LOAD
  iop_last = mx_iop_last_module();
  return 0;
}

/* make SHOT=1, for tests on PCSX2 (no network there): where the start
   stops, the screen to host:mxlink.tga and the test end marker */
static void stopped(void) {
#ifdef MXLINK_SHOT
  mx_gs_save_tga(0, "host:mxlink.tga");
  mx_printf("@@PS2TEST_END@@ status=0\n");
#endif
  for (;;)
    mx_bios_sleep_thread();
}

static void screen(void) {
  con_line(ROW_TITLE, CON_WHITE, "mxlink %s", MXLINK_VERSION);
  con_text(CON_COLS - 16, ROW_TITLE, CON_GREY, "maxsdk  ps2link");
  const char *ip = if_conf, *mask = ip + strlen(ip) + 1, *gw = mask + strlen(mask) + 1;
  con_line(ROW_NET, CON_WHITE, "Net:    %s  mask %s  gw %s", ip, mask, gw);
  con_line(ROW_HOST, CON_WHITE, "Boot:   %s%s", boot_path, restarts > 1 ? "  (restarted)" : "");
  extern char __text_start[], _end[];
  con_line(ROW_MEM, CON_WHITE, "Memory: mxlink %05x-%05x, programs 00100000-%08x",
           (u32)(uintptr_t)__text_start, (u32)(uintptr_t)_end, mx_bios_get_memory_size());
  con_line(ROW_HELP, CON_GREY, "ps2client -h %s execee host:<elf> [args]", ip);
  if (last_path[0])
    con_line(ROW_LAST, last_status ? CON_RED : CON_WHITE, "Last:   %s, exit status %d", last_path,
             (int)last_status);
}

int main(int argc, char **argv) {
  if (!restarts) {
    /* the first start: the loader's IOP, its threads and handlers */
    u32 n = argc > 0 && argv[0] ? strlen(argv[0]) : 0;
    if (n >= sizeof boot_path)
      n = 0;
    memcpy(boot_path, n ? argv[0] : "", n);
    boot_path[n] = 0;
    read_conf();
    restart();
  }

  con_init();
  screen();
  status_state(ST_BOOT, "IOP");
  mx_debug_sinks(MX_SINK_SIO);
  if (boot_iop() < 0) {
    status_state(ST_BOOT, "failed");
    stopped();
  }
  /* the tty is up (udptty): the debug output to ps2client too */
  mx_debug_sinks(MX_SINK_SIO | MX_SINK_TTY);
  if (cmd_init() < 0) {
    con_log(CON_RED, "the command thread did not start");
    stopped();
  }
  if (chime)
    con_log(CON_GREY, "sound reset: %d voices were on, auto DMA %d; blip envelope %04x",
            chime >> 2 & 63, chime >> 8 & 3, chime >> 12 & 0xFFFF);
  if (returned)
    con_log(last_status ? CON_RED : CON_GREEN, "%s returned, exit status %d", last_path,
            (int)last_status);
  returned = 0;
  con_log(CON_GREEN, "mxlink %s ready", MXLINK_VERSION);
  if (launch_argc) {
    int argc = launch_argc;
    launch_argc = 0;
    exec_swap(launch_swap[0] ? launch_swap : 0);
    cmd_launch(argc, launch_args, launch_len);
  } else {
    status_state(ST_READY, 0);
  }
  /* the main thread ends; the command thread serves */
  mx_bios_exit_delete_thread();
  return 0;
}
