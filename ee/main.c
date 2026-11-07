/* mxlink's start: the network configuration, the IOP reset and ps2link's
   IOP modules (the network, the host: file system, the tty), then the
   command thread; the status screen meanwhile.

   As ps2link, mxlink starts twice: the first time (booted from a memory
   card, or started by ps2link or another loader) it reads IPCONFIG.DAT
   next to its ELF with the IOP as the loader left it, keeps it (KEPT) and
   starts itself again by ExecPS2, which
   resets the EE kernel: the loader's threads and interrupt handlers are
   gone. A reset command from the host (mxlink_restart) does the same
   ExecPS2. */

#include <mx/bios.h>
#include <mx/cpu.h>
#include <mx/debug.h>
#include <mx/gs.h>
#include <mx/hw/regs.h>
#include <mx/iop.h>
#include <mx/kernel.h>
#include <mx/sif.h>
#include <mx/string.h>
#include <mx/timer.h>

#include "mxlink.h"

/* What survives ExecPS2: .bss is cleared and .data set back (hook.S), so a
   start finds every variable as linked, the libraries' too. */
#define KEPT __attribute__((section(".kept")))

/* the configuration for ps2ip ("ip\0mask\0gw\0"), and the path mxlink was
   booted from */
#define CONF_MAX 64
static char if_conf[CONF_MAX] KEPT;
static u32 if_conf_len KEPT = 0;
static char boot_path[256] KEPT;
static u32 restarts KEPT = 0;
/* the program's return (mxlink_return): its path and exit status, and the
   IOP's highest module id after mxlink's modules (a higher one after the
   program: it loaded modules, the IOP is reset) */
static char last_path[256] KEPT;
static u32 returned KEPT = 0;
static s32 last_status KEPT = 0;
static s32 iop_last KEPT = 0;
static u32 returns KEPT = 0; /* programs returned so far */
/* mx_loader_launch: the program to start after the restart */
static s32 launch_argc KEPT = 0;
static u32 launch_len KEPT = 0;
static char launch_args[256] KEPT = {0};
static char launch_swap[64] KEPT = {0};
static s32 launch_cleanup KEPT = 0; /* the program before it ended */
/* the next return goes to the text screen, not to the shell: after a host's
   program, and after the shell ended itself (it may crash at each start) */
static s32 no_shell KEPT = 0;

/* The crash of the program that ended as return number crash_return: kept
   over the restart, for the screen and the shell (mx_crash_last). */
static struct mx_crash crash KEPT = {0};
static char crash_note[MX_CRASH_NOTE] KEPT = {0};
static u32 crash_return KEPT = 0;

#define DEFAULT_IP "192.168.1.10"
#define DEFAULT_NETMASK "255.255.255.0"
#define DEFAULT_GW "192.168.1.0"

/* The IOP modules are files of the memory card (make: $(OUT)/MXOS), in the
   order they load. They are read to the programs' memory above mxlink's,
   free whenever the IOP is set up. */
#define MODULE_DIR "mc0:/MXOS/"
#define SHELL_PATH MODULE_DIR "MXSHELL.ELF"
#define MODULE_MEM ((u8 *)0x01000000)
enum { M_CHIME, M_PS2IP = 5, M_MXIO = 15, M_MXSOCK = 16 };
static const char *const module_names[] = {
    "chime",   "poweroff", "ps2dev9", "netman", "smap",   "ps2ip-nm", "udptty",
    "ioptrap", "ps2link",  "sio2man", "mcman",  "mcserv", "padman", "libsd",   "audsrv",
    /* not on an older card: files then go through FILEIO, and there are no sockets */
    "mxio",    "mxsock",
};
#define NMODULES (sizeof module_names / sizeof module_names[0])
static struct {
  const u8 *start;
  u32 size;
} modules[NMODULES];

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
  else if (st == ST_MENU)
    iop_status_send(3);
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
  case ST_MENU:
    con_line(ROW_STATE, CON_CYAN, "State:  menu %s", what);
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

/* The start's output: the serial port only, as the IOP a program left may not
   answer, and the screen comes first (maxsdk's debug.c) */
const int mx__debug_start_sinks = MX_SINK_SIO;
/* maxsdk's start.c: the kernel's own fatal end (the VM's, in a program's
   thread) prints no end marker, mxlink does when the program is back */
const int mx__kernel_self = 1;

static void restart(void) __attribute__((noreturn));
static void restart(void) {
  extern char mxlink_start[], _gp[];
  char *argv[1] = {boot_path};
  restarts++;
  mx_debug_flush();
  mx_bios_flush_cache(0);
  /* ExecPS2 drops the handlers but leaves the channel on: a command from the
     host now would end in the kernel's "DMAC(5) Handler does not exist" and
     its break. cmd_init turns it on again. */
  mx_bios_disable_dmac(MX_DMAC_SIF0);
  mx_bios_exec_ps2(mxlink_start, _gp, 1, argv);
  for (;;)
    ;
}

void mxlink_restart(void) {
  exec_kill();
  iop_last = 0; /* ps2link.irx gave up host: for this reset: never kept */
  no_shell = 0;
  crash_return = 0;
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
  if (exec_in_shell())
    no_shell = 1;
  returned = 1;
  returns++;
  last_status = status;
  restart();
}

/* In the report of exception.c, on the kernel's stack. */
void mx__crash_kept(struct mx_crash *c) {
  const char *const *np = exec_note();
  const char *note = np && *np ? *np : c->note;
  if (note && ((u32)(uintptr_t)note < 0x00080000u || (u32)(uintptr_t)note >= 0x02000000u))
    note = 0; /* not in core */
  u32 n = 0;
  for (; note && note[n] && n < sizeof crash_note - 1; n++)
    crash_note[n] = note[n];
  crash_note[n] = 0;
  c->note = n ? crash_note : 0;
  crash = *c;
  crash_return = returns + 1;
  mx__fio_crash();
}

int mxlink_crash(struct mx_crash *c) {
  if (!crash_return || crash_return != returns)
    return 0;
  *c = crash;
  return 1;
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
  launch_cleanup = 1;
  restart();
}

void mxlink_takeover(int argc, const char *args, u32 args_len) {
  exec_kill();
  no_shell = 1;
  mxlink_launch(argc, args, args_len, 0);
}

void mxlink_host_run(void) { no_shell = 1; }

static int module_open(u32 i) {
  char path[48];
  u32 n = sizeof MODULE_DIR - 1, l = strlen(module_names[i]);
  memcpy(path, MODULE_DIR, n);
  memcpy(path + n, module_names[i], l);
  memcpy(path + n + l, ".irx", 5);
  return mx_fio_open(path, MX_FIO_RDONLY);
}

/* Can the card be read now? The first access takes the card a while. */
static int card_ready(void) {
  for (int i = 0; i < 20; i++) {
    int fd = module_open(0);
    if (fd >= 0) {
      mx_fio_close(fd);
      return 1;
    }
    mx_delay_us(100000);
  }
  return 0;
}

/* All modules to memory: with the IOP as it is if that reads the card (the
   boot loader's, mxlink's), else after a reset with the ROM's drivers. */
static int read_modules(void) {
  status_state(ST_BOOT, "IOP modules: reading");
  if (mx_sif_init() < 0 || !card_ready()) {
    if (mx_iop_reset("") < 0 || mx_iop_load_module("rom0:SIO2MAN") < 0 ||
        mx_iop_load_module("rom0:MCMAN") < 0 || mx_iop_load_module("rom0:MCSERV") < 0 ||
        !card_ready()) {
      con_log(CON_RED, "no IOP modules: cannot read %s", MODULE_DIR);
      return -1;
    }
  }
  u8 *to = MODULE_MEM;
  for (u32 i = 0; i < NMODULES; i++) {
    int fd = module_open(i), size = fd < 0 ? -1 : mx_fio_lseek(fd, 0, MX_FIO_SEEK_END);
    if (i >= M_MXIO && fd < 0) {
      modules[i].size = 0;
      continue;
    }
    if (size <= 0 || mx_fio_lseek(fd, 0, MX_FIO_SEEK_SET) != 0 ||
        mx_fio_read(fd, to, (u32)size) != size) {
      con_log(CON_RED, "IOP module %s%s.irx: cannot read it", MODULE_DIR, module_names[i]);
      if (fd >= 0)
        mx_fio_close(fd);
      return -1;
    }
    mx_fio_close(fd);
    modules[i].start = to;
    modules[i].size = (u32)size;
    to += ((u32)size + 63) & ~63u;
  }
  return 0;
}

static int load(u32 i, const char *args, u32 args_len) {
  int res = 0;
  int id = mx_iop_load_module_buffer(modules[i].start, modules[i].size, args, args_len, &res);
  if (id < 0 || (res & 3) == 1 /* NO_RESIDENT_END: it did not stay */) {
    con_log(CON_RED, "IOP module %s: %d (%d)", module_names[i], id, res);
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

/* What a program leaves on the IOP when it ends, so the next finds the IOP as
   it was. A program that crashed or was stopped did none of this itself. */
#define PAD_SID 0x8000010Fu
#define PAD_CLOSE 0x8000010D
#define AUDSRV_SID 0x870884eu
#define AUDSRV_STOP_AUDIO 0x0006
#define AUDSRV_INIT_ADPCM 0x0016
static struct mx_sif_client pad_rpc, snd_rpc;
static s32 rpc_cmd[32] MX_DMA_LINES, rpc_res[32] MX_DMA_LINES;

/* audsrv stays: its play thread runs on for the next program, with the stream
   muted. (AUDSRV_QUIT would end that thread for good: a
   later AUDSRV_INIT does nothing.) */
static void sound_off(void) {
  if (mx_sif_bind_tries(&snd_rpc, AUDSRV_SID, 2) < 0 || !snd_rpc.server)
    return;
  static const int calls[] = {AUDSRV_STOP_AUDIO, AUDSRV_INIT_ADPCM};
  for (u32 i = 0; i < 2; i++) {
    volatile s32 *a = MX_UNCACHED(rpc_cmd);
    a[0] = a[1] = a[2] = a[3] = 0;
    mx_sif_call(&snd_rpc, calls[i], (void *)a, 16, (void *)MX_UNCACHED(rpc_res), 4);
  }
  /* neither stops a voice that is playing: the chime's quiet mode does */
  int fd = module_open(M_CHIME), size = fd < 0 ? -1 : mx_fio_lseek(fd, 0, MX_FIO_SEEK_END);
  if (size > 0 && mx_fio_lseek(fd, 0, MX_FIO_SEEK_SET) == 0 &&
      mx_fio_read(fd, MODULE_MEM, (u32)size) == size)
    mx_iop_load_module_buffer(MODULE_MEM, (u32)size, "-q", 3, &chime);
  if (fd >= 0)
    mx_fio_close(fd);
}

extern void mx__mxio_cleanup(void);
/* the sockets a program left open: common/include/mxsock.h's MXSOCK_CLOSE_ALL.
   Not through <mx/net.h>, whose buffers do not fit here */
#define MXSOCK_SID 0x4D58534B
#define MXSOCK_CLOSE_ALL 11
static struct mx_sif_client sock_rpc;
static s32 sock_io[16] __attribute__((aligned(64)));
static void close_sockets(void) {
  if (mx_sif_bind_tries(&sock_rpc, MXSOCK_SID, 2) == 0 && sock_rpc.server)
    mx_sif_call(&sock_rpc, MXSOCK_CLOSE_ALL, MX_UNCACHED(sock_io), 32, MX_UNCACHED(sock_io + 8), 16);
}
/* A server of the IOP that does not answer while this cleans up: the calls
   give up after a second (mx__sif_wait_limit), and the IOP is reset */
static u32 timeouts_before;
static int iop_silent(void) { return mx__sif_timeouts != timeouts_before; }

static void iop_cleanup(void) {
  /* descriptors 0-2 are the tty's */
  for (int fd = 3; fd < 32 && !iop_silent(); fd++)
    mx_fio_close(fd);
  if (iop_silent())
    return;
  mx__mxio_cleanup();
  close_sockets();
  if (iop_silent())
    return;
  /* an open port goes on writing the pad's state to EE memory */
  if (mx_sif_bind_tries(&pad_rpc, PAD_SID, 2) == 0 && pad_rpc.server) {
    for (int port = 0; port < 2; port++) {
      volatile s32 *c = MX_UNCACHED(rpc_cmd);
      c[0] = PAD_CLOSE;
      c[1] = port;
      c[2] = 0;
      c[3] = 1;
      mx_sif_call(&pad_rpc, 1, (void *)c, sizeof rpc_cmd, (void *)MX_UNCACHED(rpc_res),
                  sizeof rpc_res);
    }
  }
  if (!iop_silent())
    sound_off();
}

/* iop_cleanup with the limit: 0, or -1 when the IOP did not answer */
static int iop_cleanup_checked(void) {
  timeouts_before = mx__sif_timeouts;
  mx__sif_wait_limit = MX_EE_HZ;
  iop_cleanup();
  mx__sif_wait_limit = 0;
  if (!iop_silent())
    return 0;
  con_log(CON_RED, "the IOP did not answer: reset");
  return -1;
}

static int boot_iop(void) {
  /* what the program loaded is there for the next one, and from now on */
  if (launch_argc || launch_cleanup) {
    if (mx_sif_init() < 0)
      return -1;
    if (launch_cleanup && iop_cleanup_checked() < 0) {
      launch_cleanup = 0;
      goto reset;
    }
    launch_cleanup = 0;
    iop_last = mx_iop_last_module();
    return 0;
  }
  if (iop_kept() && iop_cleanup_checked() == 0)
    return 0;
reset:
  if (returned)
    con_log(CON_GREY, "the program changed the IOP: reset");
  if (read_modules() < 0)
    return -1;
  status_state(ST_BOOT, "IOP reset");
  if (mx_iop_reset("") < 0) {
    con_log(CON_RED, "the IOP did not come back from its reset");
    return -1;
  }
  mx_iop_patch_lmb();
  mx_iop_patch_prefix_check();
  /* every program that made sound loaded a module, so it gets here */
  if (mx_iop_load_module_buffer(modules[M_CHIME].start, modules[M_CHIME].size, 0, 0, &chime) < 0 ||
      (chime & 3) != 1)
    con_log(CON_RED, "the chime did not run: a program's sound may go on");
  status_state(ST_BOOT, "IOP modules");
  for (u32 i = M_CHIME + 1; i < NMODULES; i++)
    if (modules[i].size && load(i, i == M_PS2IP ? if_conf : 0, i == M_PS2IP ? if_conf_len : 0) < 0)
      return -1;
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

/* Is there a shell to start? Not after a host's program (no_shell), nor on
   mxlink-high, which offers programs no way back. */
static int shell_there(void) {
  if (no_shell || !exec_resident() || !card_ready())
    return 0;
  int fd = mx_fio_open(SHELL_PATH, MX_FIO_RDONLY);
  if (fd < 0)
    return 0;
  mx_fio_close(fd);
  return 1;
}

static void screen(void) {
  con_line(ROW_TITLE, CON_WHITE, "mxlink %s", MXLINK_VERSION);
  con_text(CON_COLS - 16, ROW_TITLE, CON_GREY, "maxsdk  ps2link");
  const char *ip = if_conf, *mask = ip + strlen(ip) + 1, *gw = mask + strlen(mask) + 1;
  con_line(ROW_NET, CON_WHITE, "Net:    %s  mask %s  gw %s", ip, mask, gw);
  con_line(ROW_HOST, CON_WHITE, "Boot:   %s%s", boot_path, restarts > 1 ? "  (restarted)" : "");
  u32 self_start, self_end;
  exec_self(&self_start, &self_end);
  con_line(ROW_MEM, CON_WHITE, "Memory: mxlink %05x-%05x, programs 00100000-%08x",
           self_start, self_end, mx_bios_get_memory_size());
  con_line(ROW_HELP, CON_GREY, "ps2client -h %s execee host:<elf> [args]", ip);
  if (last_path[0])
    con_line(ROW_LAST, last_status ? CON_RED : CON_WHITE, "Last:   %s, exit status %d", last_path,
             (int)last_status);
  if (crash_return && crash_return == returns)
    con_line(ROW_LAST + 1, CON_RED, "Crash:  %s at %08x, address %08x, ra %08x", crash.what,
             crash.epc, crash.badvaddr, crash.ra);
}

#ifdef MXLINK_EMU
/* The IOP as PCSX2 starts it, and the program from host:mxload.txt: its path
   and arguments, in words. The program's end marker ends the run. */
static void emu_main(void) {
  static char args[256];
  con_init();
  if (returned) {
    mx_printf("mxlink: %s returned, exit status %d\n", last_path, (int)last_status);
    mx_printf("@@PS2TEST_END@@ status=%d\n", (int)last_status);
    stopped();
  }
  restarts = 1;
  int fd = mx_sif_init() < 0 ? -1 : mx_fio_open("host:mxload.txt", MX_FIO_RDONLY);
  int n = fd < 0 ? -1 : mx_fio_read(fd, args, sizeof args - 1);
  if (fd >= 0)
    mx_fio_close(fd);
  if (n <= 0) {
    mx_printf("mxlink: no host:mxload.txt\n");
    stopped();
  }
  int words = 0;
  u32 len = 0;
  for (int i = 0; i < n;) {
    while (i < n && args[i] <= ' ')
      i++;
    if (i == n)
      break;
    while (i < n && args[i] > ' ')
      args[len++] = args[i++];
    args[len++] = 0;
    words++;
  }
  if (exec_elf(words, args, len) < 0)
    stopped();
  mx_bios_exit_delete_thread();
}
#endif

int main(int argc, char **argv) {
  tlb_restore();
#ifdef MXLINK_EMU
  emu_main();
#endif
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
  if (returned && crash_return == returns && crash.note)
    con_log(CON_RED, "%s", crash.note);
  /* A run ends here, however the program ended: not every way out prints the
     marker. Before mxlink says it is ready, so that it cannot end the next run. */
  if (returned && !launch_argc)
    mx_printf("@@PS2TEST_END@@ status=%d\n", (int)last_status);
  int came_back = (int)returned;
  returned = 0;
  con_log(CON_GREEN, "mxlink %s ready", MXLINK_VERSION);
  if (launch_argc) {
    int argc = launch_argc;
    launch_argc = 0;
    exec_swap(launch_swap[0] ? launch_swap : 0);
    cmd_launch(argc, launch_args, launch_len);
  } else if (shell_there()) {
    /* it is told what came back: argv[1] the program, argv[2] its status */
    static char args[256];
    u32 n = sizeof SHELL_PATH;
    memcpy(args, SHELL_PATH, n);
    int m = 0;
    if (came_back && n + strlen(last_path) + 16 < sizeof args) {
      memcpy(args + n, last_path, strlen(last_path) + 1);
      n += strlen(last_path) + 1;
      m = mx_snprintf(args + n, 16, "%d", (int)last_status);
      n += (u32)m + 1;
    }
    exec_shell_next();
    cmd_launch(m ? 3 : 1, args, n);
  } else {
    status_state(ST_READY, 0);
  }
  /* the main thread ends; the command thread serves */
  mx_bios_exit_delete_thread();
  return 0;
}
