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
#include <mx/sifdma.h>
#include <mx/debug.h>
#include <mx/gs.h>
#include <mx/hw/regs.h>
#include <mx/iop.h>
#include <mx/kernel.h>
#include <mx/sif.h>
#include <mx/string.h>
#include <mx/timer.h>

#include "ksched.h"
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
/* the service started, not yet one (mx_proc_service); given up when it
   ended before that, until the next reset: not started again and again */
static s32 service_try KEPT = 0, service_off KEPT = 0;
/* the BIOS's threads for the next program: one not built for kernel version
   10, which calls the BIOS's thread and semaphore syscalls itself (exec.c
   restarts mxlink so, and back after it) */
/* the relaunched program is the shell (exec_shell_next again after the restart) */
static s32 launch_shell KEPT = 0;
/* a program has ended and waits for the command thread (program_ended) */
static volatile int ending;

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
   free whenever the IOP is set up. Another directory (make MODULE_DIR=...)
   tries new modules: the one-shot boot of an mxlink built so reads them
   there, and a reset boots the default again. */
#ifndef MODULE_DIR
#define MODULE_DIR "mc0:/MXOS/"
#endif
#define SHELL_PATH MODULE_DIR "MXSHELL.ELF"
#define MODULE_MEM ((u8 *)0x01000000)
enum {
  M_CHIME, M_MXNET = 4, M_MXIO = 14, M_MXSOCK = 15, M_USBD = 16, M_MXPAD = 17
};
static const char *const module_names[] = {
    "chime", "poweroff",
    /* the network: maxsdk's drivers and stack (libs/maxsdk/docs/iop-drivers.md),
       and mxfast, the fast path for host: files and the console (the tty) */
    "mxdev9", "mxeth", "mxnet", "mxfast",
    "ioptrap", "ps2link", "sio2man", "mcman", "mcserv", "padman",
    /* maxsdk's sound (<mx/snd.h>; iop/sound/mxsnd), on libsd: programs bind
       to it and load nothing */
    "libsd", "mxsnd",
    /* not on an older card: files then go through FILEIO, and there are no sockets */
    "mxio", "mxsock",
    /* not on an older card: no USB pads (mxpad, on usbd: libs/maxsdk/docs/pad-bt-plan.md) */
    "usbd", "mxpad",
};
#define OPTIONAL(i) ((i) >= M_MXIO)
_Static_assert(sizeof module_names / sizeof module_names[0] == M_MXPAD + 1, "M_* and module_names");
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

static struct mx_sif_client npm;
static void iop_status_send(u32 st) {
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

/* The BIOS syscalls the program that ended made from its own code (mx OS
   plan step 2h-3; kirq.c, ksys.S), by number (an i-call's negative), with
   their counts, a few to a line: on the screen's log, or (log 0: before the
   screen, the emu build) the debug output. */
static void counts_report(const u32 *n, const char *what, int log) {
  char line[84];
  u32 len = 0, total = 0;
  for (int i = 0; i < 256; i++)
    total += n[i];
  if (log)
    con_log(CON_GREY, "  %s: %u%s", what, total, total ? ", by number:" : "");
  else
    mx_printf("mxkernel:   %s: %u%s\n", what, total, total ? ", by number:" : "");
  for (int i = 0; i < 256; i++) {
    if (!n[i])
      continue;
    if (len > 60) {
      if (log)
        con_log(CON_GREY, "  %s", line);
      else
        mx_printf("mxkernel:   %s\n", line);
      len = 0;
    }
    len += (u32)mx_snprintf(line + len, sizeof line - len, "%s%s%02x x%u", len ? ", " : "",
                            i > 0x80 ? "-" : "", i > 0x80 ? 256 - i : i, n[i]); /* 0x80: GetGsDxDyOffset */
  }
  if (len) {
    if (log)
      con_log(CON_GREY, "  %s", line);
    else
      mx_printf("mxkernel:   %s\n", line);
  }
}

static void bios_calls_report(int log) {
  counts_report(ksys_watch_end(), "BIOS syscalls", log);
  /* the kernel's own (step 2h-4): the debug output only */
  counts_report(ksys_kernel_counts(), "the kernel's own BIOS syscalls since its boot", 0);
}

static void restart(void) __attribute__((noreturn));
static void restart(void) {
  extern char mxlink_start[], _gp[];
  char *argv[1] = {boot_path};
  restarts++;
  mx_debug_flush();
  mx_dcache_flush_all();
  /* ExecPS2 drops the handlers but leaves the channel on: a command from the
     host now would end in the kernel's "DMAC(5) Handler does not exist" and
     its break. cmd_init turns it on again. */
  kirq_sif0_off();
  /* the first start's ExecPS2: the BIOS's vectors again (until main turns
     the kernel's on again, an interrupt must not find the kernel's entry
     with its state half reset); the kernel's own restart keeps its vectors
     and tables (2h-4e-1), interrupts off until its start-up turns them on,
     and its interrupt entry sends them to the BIOS's handlers until
     ksched_init */
  mx_di();
  if (restarts > 1)
    kirq_keep();
  else
    kirq_off();
  mx__sif_dma_give(); /* the BIOS's SIF1 ring again, for whoever runs next */
  if (restarts > 1) {
    /* mx OS plan 2h-4c: after the first start (which sheds the loader's
       threads and handlers by ExecPS2) the kernel starts itself again:
       mxlink_start puts .data back, crt0 clears .bss and sets the stack, on
       the BIOS thread it runs on (its threads and semaphores are its own:
       none of the BIOS's to drop). In user mode, interrupts off, as
       ExecPS2 starts it; .kept stays. */
    static char *argv_kept[1] KEPT;
    argv_kept[0] = boot_path;
    mx_dcache_flush_all();
    mx_icache_invalidate_all();
    register u32 a0 __asm__("$4") = 1;
    register char **a1 __asm__("$5") = argv_kept;
    __asm__ volatile(".set push\n.set noreorder\njr %2\nnop\n.set pop"
                     :
                     : "r"(a0), "r"(a1), "r"(mxlink_start)
                     : "memory");
    __builtin_unreachable();
  }
  mx_bios_exec_ps2(mxlink_start, _gp, 1, argv);
  for (;;)
    ;
}

void mxlink_restart(void) {
  exec_kill();
  iop_last = 0; /* ps2link.irx gave up host: for this reset: never kept */
  no_shell = 0;
  service_off = 0;
  crash_return = 0;
  restart();
}

u32 exec_returns(void) { return returns; }
const char *status_program(void) { return last_path; }
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
  /* a libmxk program from the resident mxlink: the command thread ends it,
     without the restart (end.c); this thread waits for that. Only one that
     showed the kernel its interrupt handlers (table version 8): an older
     libmxk's would stay with the BIOS, in the next program's memory. */
  if (exec_on_libmxk() && exec_resident() && end_possible() && end_tables_known() &&
      mx_bios_get_thread_id() != cmd_thread_id()) {
    ending = 1;
    cmd_wake();
    for (;;)
      mx_bios_sleep_thread();
  }
  restart();
}

int end_pending(void) { return ending; }

/* start.c: no BIOS thread or heap set-up after the kernel's own restart
   (2h-4e-1) */
int mx__kernel_kept(void) { return kirq_kept(); }

/* exception.c, after the report of a crash: one of a program the kernel
   started ends that program as its own mx_exit(255) would (the kernel's
   mx_exit has no way back: the BIOS's Exit, a reset, when PCSX2 started the
   kernel) */
void mx__crash_end(int status) {
  if (exec_running())
    mxlink_return(status);
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
  /* on the screen at once: the report after this goes to the host, through
     the SIF, which may be what is broken */
  con_line(ROW_LAST + 1, CON_RED, "Crash:  %s at %08x, address %08x, ra %08x", c->what, c->epc,
           c->badvaddr, c->ra);
  mx__fio_crash();
}

/* sif.c: a SIF wait of over 3 s (1 the send, 2 the answer). On the screen
   only: the log's host copy would go through the SIF that is stuck. */
void mx__sif_stuck(int where) {
  u32 d[8];
  mx__sif_dma_state(d);
  con_line(ROW_LAST + 1, CON_YELLOW, "SIF stuck (%s): own %u want %u written %u cur %u",
           where == 1 ? "send" : "answer", d[0], d[1], d[2], d[3]);
  con_line(ROW_LAST + 2, CON_YELLOW, "SIF1 CHCR %08x TADR %08x QWC %x MADR %08x D_STAT %08x", d[4],
           d[5], d[6], d[7], MX_R32(MX_D_STAT));
#ifdef MXLINK_EMU
  mx_printf("mxkernel: SIF stuck (%s): own %u written %u cur %u CHCR %08x QWC %x\n",
            where == 1 ? "send" : "answer", d[0], d[2], d[3], d[4], d[6]);
#endif
}

int mxlink_crash(struct mx_crash *c) {
  if (!crash_return || crash_return != returns)
    return 0;
  *c = crash;
  return 1;
}

/* the paths of the programs that wait for one they started (step 2c) or
   run beside it, by their level (exec_parked() - 1 the last) */
static char parked_path[2][sizeof last_path];
/* one's path (kproc_list) */
const char *parked_program(int level) { return parked_path[level & 1]; }
static char *parked_top(void) { return parked_path[(exec_parked() - 1) & 1]; }

/* in a service's thread: the program in launch_args started in front by the
   command thread, waited for: its process, -1 if it did not start */
static int service_start(int argc) {
  exec_park_wait_set();
  cmd_launch(argc, launch_args, launch_len);
  int child;
  while (!(child = exec_park_child()))
    mx_bios_sleep_thread();
  exec_park_wait_clear();
  return child;
}

/* the program in front when a service has none of its own: the shell, or on
   PCSX2 host:mxload.txt's; 0: none */
static u32 default_args(char *to, u32 max, int *argc);

static int launch(int argc, const char *args, u32 args_len, const char *swap, int beside) {
  u32 n = swap ? strlen(swap) : 0;
  if (n >= sizeof launch_swap)
    n = 0;
  memcpy(launch_swap, swap ? swap : "", n);
  launch_swap[n] = 0;
  if (args_len > sizeof launch_args)
    args_len = sizeof launch_args;
  memcpy(launch_args, args, args_len);
  launch_len = args_len;
  /* a service beside (kernel version 18): its spawn starts the program in
     front when none runs, and it stays beside */
  if (exec_service_caller()) {
    if (beside != 2 || exec_running())
      return -1;
    no_shell = 1; /* kernel version 20: after it the text screen, as a host's */
    return service_start(argc);
  }
  /* a program with its core at a run of its own and without VM waits for
     the other, in its own thread (not the command thread's takeover) */
  if (mx_bios_get_thread_id() != cmd_thread_id() && exec_park(beside != 0) == 0) {
    memcpy(parked_top(), last_path, sizeof last_path);
    pads_close();
    exec_swap(launch_swap[0] ? launch_swap : 0);
    exec_park_wait_set();
    cmd_launch(argc, launch_args, launch_len);
    if (beside == 2) {
      /* spawn: back once it runs, with its process; -1 if it did not start
         (exec_unpark(-1): this one is in front again) */
      int child;
      while (!(child = exec_park_child()) && !exec_park_done())
        mx_bios_sleep_thread();
      exec_park_wait_clear(); /* its end wakes only whoever waits for it */
      return child && !exec_park_done() ? child : -1;
    }
    while (!exec_park_done())
      mx_bios_sleep_thread();
    return exec_park_status();
  }
  if (beside)
    return -1;
  /* in its place: its VM ends here (libmxk leaves it to the kernel, which
     may keep it for a program that waits, phase 3a) */
  mx__vm_shutdown();
  mx__vm_swap_close();
  launch_argc = argc;
  launch_cleanup = 1;
  restart();
}

int mxlink_launch(int argc, const char *args, u32 args_len, const char *swap) {
  return launch(argc, args, args_len, swap, 0);
}
/* step 2e-2: the caller's other threads go on beside the other (kproc.c) */
int mxlink_launch_beside(int argc, const char *args, u32 args_len, const char *swap) {
  return launch(argc, args, args_len, swap, 1);
}
/* step 2e-3 (kernel version 14): as launch_beside, the caller back at once
   with the other's process */
int kproc_spawn(int argc, const char *args, u32 args_len, const char *swap) {
  return launch(argc, args, args_len, swap, 2);
}

/* proc_service (kernel version 18): the caller, a program with its core at a
   run of its own and without VM, stays beside every program in front from
   now on; the program in front that would have come next (the shell) starts
   now. 0 (its process when one started), -1 when it cannot. */
int kproc_service(void) {
  if (exec_parked() || mx_bios_get_thread_id() == cmd_thread_id() || exec_park(1) < 0)
    return -1;
  exec_park_service_set();
  service_try = 0;
  memcpy(parked_top(), last_path, sizeof last_path);
  pads_close();
  int argc = 0;
  launch_len = default_args(launch_args, sizeof launch_args, &argc);
  if (!argc) {
    status_state(ST_READY, 0); /* none in front: ready for the host */
    return 0;
  }
  exec_swap(0);
  int child = service_start(argc);
  return child > 0 ? child : 0;
}

/* proc_kill: the program in front ended by another process as if it had
   called mx_exit(status), without the restart (which would end the caller
   too): -1 when that cannot be */
/* the shell ended by the service (the host link, for the host): the
   restart, its IOP kept and cleaned up, as for the takeover of old, not
   reset with the modules read again: the card did not answer after it (the
   shell's drivers, in the middle of a call) */
static int shell_stopped;
/* its end prints no test end marker: the host's run is the next program */
static s32 quiet_end KEPT = 0;

int kproc_kill(int id, int status) {
  if (id <= 0 || id != end_proc_front() || id == ksched_thread_proc(mx_bios_get_thread_id()) ||
      !exec_running() || !exec_on_libmxk() || !exec_resident() || !end_possible() ||
      !end_tables_known() || ending)
    return -1;
  returned = 1;
  returns++;
  last_status = (u32)status & 0xFF;
  /* kernel version 20: a service (the host link) ended it: the text screen
     after it, not the shell, until the next reset */
  if (exec_service_caller()) {
    no_shell = 1;
    shell_stopped = exec_in_shell();
  }
  ending = 1;
  cmd_wake();
  return 0;
}

void mxlink_takeover(int argc, const char *args, u32 args_len) {
  exec_kill();
  no_shell = 1;
  mxlink_launch(argc, args, args_len, 0); /* from the command thread: the restart */
  __builtin_unreachable();
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

/* the IOP reset, with the ROM's card drivers: 0 when it reads the card */
static int rom_card(void) {
  return mx_iop_reset("") < 0 || mx_iop_load_module("rom0:SIO2MAN") < 0 ||
                 mx_iop_load_module("rom0:MCMAN") < 0 || mx_iop_load_module("rom0:MCSERV") < 0 ||
                 !card_ready()
             ? -1
             : 0;
}

static int read_all(void) {
  u8 *to = MODULE_MEM;
  for (u32 i = 0; i < NMODULES; i++) {
    int fd = module_open(i), size = fd < 0 ? -1 : mx_fio_lseek(fd, 0, MX_FIO_SEEK_END);
    if (OPTIONAL(i) && fd < 0) {
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

/* All modules to memory: with the IOP as it is if that reads the card (the
   boot loader's, mxlink's), else, or when that fails (a program ended in the
   middle of a card call: the shell, ended by the host link), after a reset
   with the ROM's drivers. */
static int read_modules(void) {
  status_state(ST_BOOT, "IOP modules: reading");
  int kept = mx_sif_init() >= 0 && card_ready();
  if (kept && read_all() == 0)
    return 0;
  if (kept)
    con_log(CON_GREY, "the IOP as it was: reset with the ROM's card drivers");
  if (rom_card() < 0) {
    con_log(CON_RED, "no IOP modules: cannot read %s", MODULE_DIR);
    return -1;
  }
  return read_all();
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
#ifdef MXLINK_EMU
  (void)m; /* PCSX2's IOP: no ps2link */
  return mx_iop_last_module() == iop_last;
#else
  return mx_iop_last_module() == iop_last && mx_iop_find_module("ps2link", &m) > 0;
#endif
}

/* What a program leaves on the IOP when it ends, so the next finds the IOP as
   it was. A program that crashed or was stopped did none of this itself. */
#define PAD_SID 0x8000010Fu
#define PAD_CLOSE 0x8000010D
#define MXSND_SID 0x4D58534Eu /* libs/maxsdk/common/include/mxsnd.h */
#define MXSND_STOP 3
static struct mx_sif_client pad_rpc, snd_rpc;
static s32 rpc_cmd[32] MX_DMA_LINES, rpc_res[32] MX_DMA_LINES;

/* mxsnd stays for the next program: its status writes into the program's
   memory stop, and its voices and stream are silenced, when its status area
   is in the ending program's physical memory (exec_end_range: not the one
   beside it, whose sound goes on). Without mxsnd, the chime's quiet mode
   stops the voices. */
static void sound_off(void) {
  if (mx_sif_bind_tries(&snd_rpc, MXSND_SID, 2) == 0 && snd_rpc.server) {
    u32 lo, hi;
    exec_end_range(&lo, &hi);
    volatile s32 *a = MX_UNCACHED(rpc_cmd);
    a[0] = (s32)lo, a[1] = (s32)hi, a[2] = a[3] = 0;
    mx_sif_call(&snd_rpc, MXSND_STOP, (void *)a, 16, (void *)MX_UNCACHED(rpc_res), 4);
    return;
  }
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
  pads_close();
  if (!iop_silent())
    sound_off();
}

/* an open port goes on writing the pad's state to EE memory; the USB and
   Bluetooth pads' motors stop */
void pads_close(void) {
  pad_rumble_off();
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
  for (u32 i = M_CHIME + 1; i < NMODULES; i++) {
    if (!modules[i].size)
      continue;
    int stack = i == M_MXNET;
    if (load(i, stack ? if_conf : 0, stack ? if_conf_len : 0) < 0) {
      if (i >= M_USBD)
        continue; /* no USB pads, nothing more: the boot goes on */
      return -1;
    }
  }
  iop_last = mx_iop_last_module();
  return 0;
}

/* make SHOT=1, for tests on PCSX2 (no network there): where the start
   stops, the screen to host:mxlink.tga and the test end marker */
static void stopped(void) {
#ifdef MXLINK_SHOT
  mx_gs_save_tga(0, "host:mxkernel.tga");
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
  con_line(ROW_TITLE, CON_WHITE, "mx kernel %s", MXLINK_VERSION);
  const char *ip = if_conf, *mask = ip + strlen(ip) + 1, *gw = mask + strlen(mask) + 1;
  con_line(ROW_NET, CON_WHITE, "Net:    %s  mask %s  gw %s", ip, mask, gw);
  con_line(ROW_HOST, CON_WHITE, "Boot:   %s%s", boot_path, restarts > 1 ? "  (restarted)" : "");
  u32 self_start, self_end;
  exec_self(&self_start, &self_end);
  con_line(ROW_MEM, CON_WHITE, "Memory: kernel %05x-%05x, programs 00100000-%08x",
           self_start, self_end, MX_MAIN_BYTES);
  con_line(ROW_HELP, CON_GREY, "ps2client -h %s execee host:<elf> [args]", ip);
  if (last_path[0])
    con_line(ROW_LAST, last_status ? CON_RED : CON_WHITE, "Last:   %s, exit status %d", last_path,
             (int)last_status);
  if (crash_return && crash_return == returns)
    con_line(ROW_LAST + 1, CON_RED, "Crash:  %s at %08x, address %08x, ra %08x", crash.what,
             crash.epc, crash.badvaddr, crash.ra);
}

/* a file of words (a path and its arguments): its words one after the
   other, each with its 0, in to; their number (0: none, or no file) */
static int read_words(const char *path, char *to, u32 max, u32 *len) {
  int fd = mx_fio_open(path, MX_FIO_RDONLY);
  int n = fd < 0 ? -1 : mx_fio_read(fd, to, max - 1);
  if (fd >= 0)
    mx_fio_close(fd);
  int words = 0;
  u32 l = 0;
  for (int i = 0; i < n;) {
    while (i < n && to[i] <= ' ')
      i++;
    if (i == n)
      break;
    while (i < n && to[i] > ' ')
      to[l++] = to[i++];
    to[l++] = 0;
    words++;
  }
  *len = l;
  return words;
}

/* mx OS plan 3c: the one swap file of MXOS programs, mc0:/MXOS/SWAP.CFG (on
   PCSX2 host:swap.txt): its path and, optionally, its size in MiB (64 by
   default). A program without a swap file of its launch's gets it (in place
   of the one it asks for); none without the file. Read once. */
#ifdef MXLINK_EMU
#define SWAP_CFG "host:swap.txt"
#else
#define SWAP_CFG MODULE_DIR "SWAP.CFG"
#endif
static char swap_path[64];
static u32 swap_mib = 64;
static int swap_read;
const char *kswap_path(u32 *bytes) {
  if (!swap_read) {
#ifndef MXLINK_EMU
    if (!card_ready())
      return 0; /* again at the next program */
#endif
    swap_read = 1;
    char w[96];
    u32 len;
    int n = read_words(SWAP_CFG, w, sizeof w, &len);
    u32 l = n >= 1 ? strlen(w) : 0;
    if (l && l < sizeof swap_path) {
      memcpy(swap_path, w, l + 1);
      u32 mib = 0;
      for (const char *p = w + l + 1; n >= 2 && *p >= '0' && *p <= '9'; p++)
        mib = mib * 10 + (u32)(*p - '0');
      if (mib)
        swap_mib = mib;
      con_log(CON_GREY, "swap: %s, %u MiB", swap_path, swap_mib);
    }
  }
  if (bytes)
    *bytes = swap_mib << 20;
  return swap_path[0] ? swap_path : 0;
}

/* the service started at the boot (kernel version 18): mc0:/MXOS/SERVICES.CFG
   (on PCSX2 host:services.txt), a path and its arguments; it calls
   mx_proc_service, and the program in front that comes next starts */
#ifdef MXLINK_EMU
#define SERVICES_PATH "host:services.txt"
#else
#define SERVICES_PATH MODULE_DIR "SERVICES.CFG"
#endif
static char service_args[256];
static u32 service_len;
static int service_argc = -1; /* -1: not read yet */
static int service_there(void) {
  if (service_argc < 0) {
    service_argc = 0;
#ifndef MXLINK_EMU
    if (!exec_resident() || !card_ready())
      return 0;
#endif
    service_argc = read_words(SERVICES_PATH, service_args, sizeof service_args, &service_len);
  }
  return service_argc > 0;
}

#ifdef MXLINK_EMU
static char emu_args[256];
static u32 emu_len;
static int emu_argc;
#endif

static u32 default_args(char *to, u32 max, int *argc) {
#ifdef MXLINK_EMU
  if (emu_len <= max && emu_argc) {
    memcpy(to, emu_args, emu_len);
    *argc = emu_argc;
    return emu_len;
  }
#else
  if (shell_there() && sizeof SHELL_PATH <= max) {
    memcpy(to, SHELL_PATH, sizeof SHELL_PATH);
    exec_shell_next();
    *argc = 1;
    return sizeof SHELL_PATH;
  }
#endif
  *argc = 0;
  return 0;
}

#ifdef MXLINK_EMU
/* The IOP as PCSX2 starts it, and the program from host:mxload.txt: its path
   and arguments, in words. The program's end marker ends the run. */
static void emu_main(void) {
  static char args[256];
  if (returned) {
    mx_printf("mxkernel: %s returned, exit status %d\n", last_path, (int)last_status);
    bios_calls_report(0);
    mx_printf("@@PS2TEST_END@@ status=%d\n", (int)last_status);
    stopped();
  }
  restarts = 1;
  /* the kernel's threads, as main gives them */
  kirq_on();
  mx__sif_dma_take(); /* SIF1 through maxsdk's ring, not the BIOS's */
  kvideo_init();      /* the video modes without SetGsCrt */
  ksched_init();
  kmsg_init();
  klog_init();
  mx__timer_init();
  mx__fio_lock_init(); /* every process's threads use the file server */
  /* after the kernel's threads, as in main: a vblank handler the screen
     adds before them is the BIOS's, which the kernel's interrupt entry
     never calls (its hooks, the SIF waits' timeouts, never ran) */
  con_init();
  u32 len = 0;
  int words = mx_sif_init() < 0 ? 0 : read_words("host:mxload.txt", args, sizeof args, &len);
  /* a program's mx_loader_launch: that one next, after the restart, as main's
     after_program does */
  if (launch_argc && mx_sif_init() == 0) {
    words = launch_argc, len = launch_len;
    memcpy(args, launch_args, len < sizeof args ? len : sizeof args);
    launch_argc = 0, launch_cleanup = 0;
  }
  if (!words) {
    mx_printf("mxkernel: no host:mxload.txt\n");
    stopped();
  }
  iop_last = mx_iop_last_module(); /* iop_kept: the IOP as PCSX2 started it */
  /* started by the command thread, as on the PS2: the end without a
     restart, and programs that start others (launch, launch_beside) */
  if (cmd_init() < 0) {
    mx_printf("mxkernel: the command thread did not start\n");
    stopped();
  }
  /* with a service (host:services.txt), host:mxload.txt's program comes
     after it, in front (mx_proc_service) */
  if (service_there()) {
    memcpy(emu_args, args, len);
    emu_len = len, emu_argc = words;
    cmd_launch(service_argc, service_args, service_len);
    mx_bios_exit_delete_thread();
  }
  cmd_launch(words, args, len);
  mx_bios_exit_delete_thread();
}
#endif

static void after_program(void);

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

  /* the kernel's interrupt entry and threads (mx OS plan, step 2d): every
     program is an MXOS one (2h-2) */
  kirq_on();
  mx__sif_dma_take(); /* SIF1 through maxsdk's ring, not the BIOS's */
  kvideo_init();      /* the video modes without SetGsCrt */
  ksched_init();
  kmsg_init();
  klog_init();
  mx__timer_init();
  mx__fio_lock_init(); /* every process's threads use the file server */
  con_init();
  screen();
  status_state(ST_BOOT, "IOP");
  mx_debug_sinks(MX_SINK_SIO);
  if (boot_iop() < 0) {
    status_state(ST_BOOT, "failed");
    stopped();
  }
  pad_usb_start();
  /* the tty is up (mxfast): the debug output to ps2client too */
  mx_debug_sinks(MX_SINK_SIO | MX_SINK_TTY);
  if (cmd_init() < 0) {
    con_log(CON_RED, "the command thread did not start");
    stopped();
  }
  after_program();
  /* the main thread ends; the command thread serves */
  mx_bios_exit_delete_thread();
  return 0;
}

/* What follows a program's end, after the restart (main) or without it
   (program_ended): the screen's log, the end marker, the next program or the
   shell. */
static void after_program(void) {
  if (chime)
    con_log(CON_GREY, "sound reset: %d voices were on, auto DMA %d; blip envelope %04x",
            chime >> 2 & 63, chime >> 8 & 3, chime >> 12 & 0xFFFF);
  if (returned) {
    con_log(last_status ? CON_RED : CON_GREEN, "%s returned, exit status %d", last_path,
            (int)last_status);
    bios_calls_report(1);
  }
  if (returned && crash_return == returns && crash.note)
    con_log(CON_RED, "%s", crash.note);
  /* A run ends here, however the program ended: not every way out prints the
     marker. Before mxlink says it is ready, so that it cannot end the next run. */
  if (returned && !launch_argc && !exec_park_service() && !quiet_end)
    mx_printf("@@PS2TEST_END@@ status=%d\n", (int)last_status);
  quiet_end = 0;
  int came_back = (int)returned;
  returned = 0;
  if (service_try && came_back) {
    con_log(CON_RED, "the service %s ended before it became one: not started again", last_path);
    service_off = 1;
  }
  service_try = 0;
  /* the shell crashed, also before its first line (in its start): the text
     screen until the next reset, not the shell again and again */
  if (came_back && crash_return == returns && !strcmp(last_path, SHELL_PATH) && !no_shell) {
    con_log(CON_RED, "the shell %s crashed: not started again until a reset", SHELL_PATH);
    no_shell = 1;
  }
  con_log(CON_GREEN, "mx kernel %s ready", MXLINK_VERSION);
  if (launch_argc) {
    int argc = launch_argc;
    launch_argc = 0;
    exec_swap(launch_swap[0] ? launch_swap : 0);
    if (launch_shell)
      exec_shell_next();
    launch_shell = 0;
    cmd_launch(argc, launch_args, launch_len);
  } else if (!exec_parked() && !service_off && service_there()) {
    /* the service, which starts the shell (mx_proc_service) */
    service_try = 1;
    cmd_launch(service_argc, service_args, service_len);
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
}

/* The end of a libmxk program, in the command thread: what the restart's
   ExecPS2 and main's start did, the IOP kept. A program that changed the IOP,
   or an IOP that does not answer, gets the restart after all. */
/* The kernel's own clients whose semaphore went with the program's (made
   while it ran: npm's, when its first status went out with the program's
   start): made again at their next call. Right after semas_off, before a
   new semaphore can take the same id. */
void end_kernel_clients(void) {
  for (int i = 0; i < 4; i++)
    mx__sif_client_check(i == 0 ? &npm : i == 1 ? &pad_rpc : i == 2 ? &snd_rpc : &sock_rpc);
}

void program_ended(void) {
  ending = 0;
  end_cleanup();
  /* the VM of a program that waits or runs beside goes on (phase 3a): its
     entries again over the BIOS's, before an interrupt (the VM's vblank)
     reaches its tables */
  int was = mx_di();
  tlb_restore();
  if (exec_vm_kept())
    mx__vm_tlb_again();
  mx_irq_restore(was);
  kirq_on();
  /* the shell ended by the service: always the restart, as the takeover
     did (without it the next program crashed at once, before its first
     line: something of the shell's went on) */
  if (shell_stopped || !iop_kept() || iop_cleanup_checked() < 0) {
    if (shell_stopped)
      launch_cleanup = 1, quiet_end = 1;
    shell_stopped = 0;
    restart();
  }
  u32 threads_left, semas_left, calls_left;
  end_counts(&threads_left, &semas_left, &calls_left);
  end_reset_state();
  exec_ended();
  con_init();
  screen();
  u32 threads, semas;
  end_ended(&threads, &semas);
  con_log(CON_GREY, "ended without a restart: %u threads, %u semaphores", threads, semas);
  u32 n_intc, n_dmac;
  kirq_counts(&n_intc, &n_dmac);
  con_log(CON_GREY, "the kernel first at %u INTC and %u DMAC interrupts (%u others to the BIOS)",
          n_intc, n_dmac, kirq_bios_count());
  if (threads_left || semas_left || calls_left)
    con_log(CON_RED, "the end left %u threads, %u semaphores, %u IOP calls, %u RPC packets",
            threads_left, semas_left, calls_left & 0xFF, calls_left >> 8);
  if (exec_park_service()) {
    /* a service beside: it stays, and is told (kernel version 18) */
    exec_front_ended((int)last_status);
    after_program();
    mx_debug_flush();
    mx_debug_sinks(MX_SINK_SIO | MX_SINK_TTY);
    return;
  }
  if (exec_parked()) {
    /* the program that started it goes on (step 2c) */
    con_log(last_status ? CON_RED : CON_GREEN, "%s returned, exit status %d: back to %s", last_path,
            (int)last_status, parked_top());
    memcpy(last_path, parked_top(), sizeof last_path);
    exec_unpark((int)last_status);
    status_state(exec_in_shell() ? ST_MENU : ST_RUNNING, last_path);
    mx_debug_flush();
    mx_debug_sinks(MX_SINK_SIO | MX_SINK_TTY); /* a libmxk program's */
    return;
  }
  after_program();
}
