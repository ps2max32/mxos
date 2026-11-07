/* Commands from the host, as ps2link takes them: ps2link.irx on the IOP
   gets ps2client's UDP packets and sends each by SIF DMA to EE address
   0xFF800 (CMD_AREA): word 0 the PKO_RPC_* number (not 0, so it tells a
   new packet), the network packet from word 1 (fields big-endian). The
   SIF0 DMA end interrupt copies it and wakes the command thread. */

#include <mx/bios.h>
#include <mx/cpu.h>
#include <mx/debug.h>
#include <mx/hw/formats.h>
#include <mx/hw/regs.h>
#include <mx/sif.h>
#include <mx/string.h>

#include "mxlink.h"

#define CMD_AREA 0x000FF800u

/* ps2link's commands (include/hostlink.h) */
#define PKO_RESET_CMD 0xbabe0201u
#define PKO_EXECEE_CMD 0xbabe0203u
#define PKO_SCRDUMP_CMD 0xbabe0205u
#define PKO_NETDUMP_CMD 0xbabe0206u
#define PKO_DUMP_MEM 0xbabe0207u
#define PKO_START_VU 0xbabe0208u
#define PKO_STOP_VU 0xbabe0209u
#define PKO_DUMP_REG 0xbabe020au
#define PKO_GSEXEC_CMD 0xbabe020bu
#define PKO_WRITE_MEM 0xbabe020cu
#define PKO_IOPEXCEP_CMD 0xbabe020du
#define PKO_MAX_PATH 256

/* the packets after the header (cmd: 4 bytes, len: 2), packed */
#define HDR 6

static u32 be32(const u8 *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }
static u16 be16(const u8 *p) { return (u16)(p[0] << 8 | p[1]); }

static int cmd_tid;
static int sif0_id;
/* the packet, copied out of CMD_AREA by the interrupt */
static u8 pkt[1024] __attribute__((aligned(16)));
static volatile int pkt_new;
/* mx_loader_launch: as an EXECEE, in the command thread */
static volatile int launch_argc;
static const char *launch_args;
static u32 launch_len;
static u8 data_buf[16 * 1024] __attribute__((aligned(64)));

static int sif0_handler(int ch, void *arg, void *addr) {
  (void)ch, (void)arg, (void)addr;
  volatile u32 *a = MX_UNCACHED((u32 *)(uintptr_t)CMD_AREA);
  if (a[0]) {
    u32 *d = (u32 *)pkt;
    for (u32 i = 0; i < sizeof pkt / 4; i++)
      d[i] = a[i];
    a[0] = 0;
    mx_sync();
    pkt_new = 1;
    mx_bios_iwakeup_thread(cmd_tid);
  }
  mx_bios_isif_set_dchain();
  return 0;
}

/* a path of the packet; without a device it is ps2client's (host:), as
   ps2client's ping (a dumpreg to "ps2client.ping") expects */
static int has_device(const char *s) {
  for (; *s; s++)
    if (*s == ':')
      return 1;
  return 0;
}

static void path_of(char *d, const u8 *s) {
  char t[PKO_MAX_PATH];
  memcpy(t, s, PKO_MAX_PATH);
  t[PKO_MAX_PATH - 1] = 0;
  if (has_device(t))
    memcpy(d, t, PKO_MAX_PATH);
  else
    mx_snprintf(d, PKO_MAX_PATH, "host:%s", t);
}

/* the commands that use the IOP's file server: not while a program runs
   (it has the SIF replies) */
static int files_ok(const char *what) {
  /* a host that wants its files stops the shell; ps2client asks again */
  if (exec_in_shell())
    mxlink_takeover(0, 0, 0);
  if (!exec_running())
    return 1;
  con_log(CON_RED, "%s: not while a program runs", what);
  return 0;
}

/* dumpmem: offset, size, path */
static void dump_mem(const u8 *p) {
  u32 off = be32(p + HDR), size = be32(p + HDR + 4);
  char path[PKO_MAX_PATH];
  path_of(path, p + HDR + 8);
  if (!files_ok("dumpmem"))
    return;
  con_log(CON_WHITE, "dumpmem %08x, %u bytes -> %s", off, size, path);
  int fd = mx_fio_open(path, MX_FIO_WRONLY | MX_FIO_CREAT | MX_FIO_TRUNC);
  if (fd < 0) {
    con_log(CON_RED, "  cannot open %s", path);
    return;
  }
  for (u32 done = 0; done < size;) {
    u32 n = size - done < sizeof data_buf ? size - done : sizeof data_buf;
    memcpy(data_buf, (const void *)(uintptr_t)(off + done), n);
    if (mx_fio_write(fd, data_buf, n) < 0) {
      con_log(CON_RED, "  write failed");
      break;
    }
    done += n;
  }
  mx_fio_close(fd);
}

/* writemem: offset, size, path */
static void write_mem(const u8 *p) {
  u32 off = be32(p + HDR), size = be32(p + HDR + 4);
  char path[PKO_MAX_PATH];
  path_of(path, p + HDR + 8);
  if (!files_ok("writemem"))
    return;
  con_log(CON_WHITE, "writemem %s -> %08x, %u bytes", path, off, size);
  int fd = mx_fio_open(path, MX_FIO_RDONLY);
  if (fd < 0) {
    con_log(CON_RED, "  cannot open %s", path);
    return;
  }
  for (u32 done = 0; done < size;) {
    u32 n = size - done < sizeof data_buf ? size - done : sizeof data_buf;
    int r = mx_fio_read(fd, data_buf, n);
    if (r <= 0) {
      con_log(CON_RED, "  read failed");
      break;
    }
    memcpy((void *)(uintptr_t)(off + done), data_buf, (u32)r);
    done += (u32)r;
  }
  mx_fio_close(fd);
  mx_bios_flush_cache(0);
  mx_bios_flush_cache(2);
}

/* dumpreg: the register sets of ps2link */
static const u32 dma_regs[] = {
    0x1000e000, 0x1000e010, 0x1000e020, 0x1000e030, 0x1000e040, 0x1000e050, 0x1000e060,
    0x1000f520, 0x10008000, 0x10008010, 0x10008020, 0x10008030, 0x10008040, 0x10008050,
    0x10009000, 0x10009010, 0x10009020, 0x10009030, 0x10009040, 0x10009050, 0x1000a000,
    0x1000a010, 0x1000a020, 0x1000a030, 0x1000a040, 0x1000a050, 0x1000b000, 0x1000b010,
    0x1000b020, 0x1000b400, 0x1000b410, 0x1000b420, 0x1000b430, 0x1000c000, 0x1000c010,
    0x1000c020, 0x1000c400, 0x1000c410, 0x1000c420, 0x1000c430, 0x1000c800, 0x1000c810,
    0x1000c820, 0x1000d000, 0x1000d010, 0x1000d020, 0x1000d080, 0x1000d400, 0x1000d410,
    0x1000d420, 0x1000d430, 0x1000d480};
static const u32 intc_regs[] = {0x1000F000, 0x1000F010};
static const u32 timer_regs[] = {0x10000000, 0x10000010, 0x10000020, 0x10000030, 0x10000800,
                                 0x10000810, 0x10000820, 0x10000830, 0x10001000, 0x10001010,
                                 0x10001020, 0x10001800, 0x10001810, 0x10001820};
static const u32 gs_regs[] = {0x12001000, 0x12001080};
static const u32 sif_regs[] = {0x1000F230};
static const u32 fifo_regs[] = {0x10005000, 0x10007000};
static const u32 gif_regs[] = {0x10003020, 0x10003040, 0x10003050, 0x10003060,
                               0x10003070, 0x10003080, 0x10003090, 0x100030a0};
static const u32 vif0_regs[] = {0x10003800, 0x10003810, 0x10003820, 0x10003830, 0x10003840,
                                0x10003850, 0x10003860, 0x10003870, 0x10003880, 0x10003890,
                                0x100038d0, 0x10003900, 0x10003910, 0x10003920, 0x10003930,
                                0x10003940, 0x10003950, 0x10003960, 0x10003970};
static const u32 vif1_regs[] = {0x10003c00, 0x10003c10, 0x10003c20, 0x10003c30, 0x10003c40,
                                0x10003c50, 0x10003c60, 0x10003c70, 0x10003c80, 0x10003c90,
                                0x10003ca0, 0x10003cb0, 0x10003cc0, 0x10003cd0, 0x10003ce0,
                                0x10003d00, 0x10003d10, 0x10003d20, 0x10003d30, 0x10003d40,
                                0x10003d50, 0x10003d60, 0x10003d70};
static const u32 ipu_regs[] = {0x10002000, 0x10002010, 0x10002020, 0x10002030};

#define SET(a) {a, sizeof a / sizeof a[0]}
static const struct {
  const u32 *r;
  u32 n;
} reg_sets[] = {/* in ps2link's order: REGDMA .. REGIPU, REGALL (10) is all of them */
                SET(dma_regs),  SET(intc_regs), SET(timer_regs), SET(gs_regs),  SET(sif_regs),
                SET(fifo_regs), SET(gif_regs),  SET(vif0_regs),  SET(vif1_regs), SET(ipu_regs)};

static void dump_reg(const u8 *p) {
  u32 which = be32(p + HDR);
  char path[PKO_MAX_PATH];
  path_of(path, p + HDR + 4);
  if (!files_ok("dumpreg"))
    return;
  u32 *w = (u32 *)data_buf, n = 0;
  for (u32 s = 0; s < 10; s++)
    if (which == s || which == 10)
      for (u32 i = 0; i < reg_sets[s].n; i++)
        w[n++] = MX_R32(reg_sets[s].r[i]);
  if (!n) {
    /* REGVU0, REGVU1: not yet */
    con_log(CON_RED, "dumpreg %u: not supported", which);
    return;
  }
  con_log(CON_WHITE, "dumpreg %u -> %s", which, path);
  int fd = mx_fio_open(path, MX_FIO_WRONLY | MX_FIO_CREAT | MX_FIO_TRUNC);
  if (fd < 0) {
    con_log(CON_RED, "  cannot open %s", path);
    return;
  }
  mx_fio_write(fd, w, n * 4);
  mx_fio_close(fd);
}

/* gsexec: size, path: up to 128 bytes of GIF packet sent by PATH3 */
static void gs_exec(const u8 *p) {
  u32 size = be16(p + HDR);
  char path[PKO_MAX_PATH];
  path_of(path, p + HDR + 2);
  if (!files_ok("gsexec"))
    return;
  int fd = mx_fio_open(path, MX_FIO_RDONLY);
  if (fd < 0) {
    con_log(CON_RED, "gsexec: cannot open %s", path);
    return;
  }
  int n = mx_fio_read(fd, data_buf, 128);
  mx_fio_close(fd);
  if (n < 0)
    return;
  con_log(CON_WHITE, "gsexec %s, %u bytes", path, size);
  mx_dcache_writeback(data_buf, 128);
  MX_R32(MX_DMA_GIF_QWC) = size / 16;
  MX_R32(MX_DMA_GIF_MADR) = (u32)(uintptr_t)data_buf;
  MX_R32(MX_DMA_GIF_CHCR) = 0x101;
  while (MX_R32(MX_DMA_GIF_CHCR) & 0x100)
    ;
}

/* ctc2 $8, vi28 (FBRST) as a word: the assembler takes it only with -mvu0 */
static void fbrst(u32 v) {
  __asm__ volatile("move $8, %0\n\t.word 0x48c8e000\n\tsync.p" : : "r"(v) : "$8", "memory");
}

static void vu(const u8 *p, int start) {
  u32 vpu = be32(p + HDR);
  if (vpu > 1)
    return;
  con_log(CON_WHITE, "%s VU%u", start ? "startvu" : "stopvu", vpu);
  /* stop: force break, VIF force break; start: reset, VIF STC */
  fbrst((start ? 2u : 1u) << (vpu * 8));
  MX_R32(vpu ? 0x10003C10u : 0x10003810u) = start ? 8 : 2;
}

static const char *const iop_codes[16] = {
    "interrupt", "TLB modified", "TLB load", "TLB store", "address load", "address store",
    "bus fetch", "bus data",  "syscall",      "break",    "reserved instr", "coprocessor",
    "overflow",  "trap",      "?",            "?"};

/* the IOP's exception (ps2link.irx's packet: epc, cause, badvaddr, status,
   32 registers, ..., the module's relative EPC (word 40), the name length,
   the name) */
static void iop_exception(const u32 *w) {
  exec_kill();
  u32 epc = w[1], cause = w[2], bad = w[3], sr = w[4];
  const u32 *regs = &w[5];
  char name[64];
  memcpy(name, &w[42], sizeof name - 1);
  name[sizeof name - 1] = 0;
  con_log(CON_RED, "IOP exception: %s", iop_codes[(cause >> 2) & 15]);
  con_log(CON_RED, "  module %s, relative EPC %08x", name, w[40]);
  con_log(CON_RED, "  EPC %08x cause %08x bad %08x sr %08x", epc, cause, bad, sr);
  con_log(CON_RED, "  ra %08x sp %08x", regs[31], regs[29]);
}

static void cmd_thread(void *arg) {
  (void)arg;
  for (;;) {
    while (!pkt_new && !launch_argc && !end_pending())
      mx_bios_sleep_thread();
    if (end_pending()) {
      program_ended();
      continue;
    }
    if (launch_argc) {
      int argc = launch_argc;
      launch_argc = 0;
      if (exec_elf(argc, launch_args, launch_len) < 0)
        status_state(ST_READY, 0);
      continue;
    }
    pkt_new = 0;
    const u8 *p = pkt + 4;
    u32 cmd = be32(p);
    switch (cmd) {
    case PKO_RESET_CMD:
      con_log(CON_YELLOW, "reset");
      mxlink_restart();
      break;
    case PKO_EXECEE_CMD:
      if (exec_in_shell())
        mxlink_takeover((int)be32(p + HDR), (const char *)p + HDR + 4, PKO_MAX_PATH);
      if (!exec_running())
        mxlink_host_run();
      exec_elf((int)be32(p + HDR), (const char *)p + HDR + 4, PKO_MAX_PATH);
      break;
    case PKO_SCRDUMP_CMD:
    case PKO_NETDUMP_CMD:
      break; /* where exceptions are reported: the screen and the host, both */
    case PKO_DUMP_MEM:
      dump_mem(p);
      break;
    case PKO_WRITE_MEM:
      write_mem(p);
      break;
    case PKO_DUMP_REG:
      dump_reg(p);
      break;
    case PKO_GSEXEC_CMD:
      gs_exec(p);
      break;
    case PKO_START_VU:
    case PKO_STOP_VU:
      vu(p, cmd == PKO_START_VU);
      break;
    case PKO_IOPEXCEP_CMD:
      iop_exception((const u32 *)p);
      break;
    default:
      con_log(CON_RED, "unknown command %08x", cmd);
      break;
    }
  }
}

static u8 cmd_stack[16 * 1024] __attribute__((aligned(16)));

void cmd_launch(int argc, const char *args, u32 args_len) {
  launch_args = args;
  launch_len = args_len;
  launch_argc = argc;
  mx_bios_wakeup_thread(cmd_tid);
}

int cmd_thread_id(void) { return cmd_tid; }
void cmd_wake(void) { mx_bios_wakeup_thread(cmd_tid); }

int cmd_init(void) {
  /* ps2link's command thread: above the programs (64) */
  extern char _gp[];
  struct {
    int status;
    void *func, *stack;
    int stack_size;
    void *gp;
    int initial_priority, current_priority;
    u32 attr, option;
  } tp = {0, (void *)cmd_thread, cmd_stack, sizeof cmd_stack, _gp, 20, 0, 0, 0};
  cmd_tid = mx_bios_create_thread(&tp);
  if (cmd_tid < 0 || mx_bios_start_thread(cmd_tid, 0) < 0)
    return -1;

  MX_UNCACHED((u32 *)(uintptr_t)CMD_AREA)[0] = 0;
  if (MX_R32(MX_D_STAT) & (1u << MX_DMAC_SIF0))
    MX_R32(MX_D_STAT) = 1u << MX_DMAC_SIF0;
  if (!(MX_R32(MX_DMA_SIF0_CHCR) & 0x100))
    mx_bios_sif_set_dchain();
  sif0_id = mx_bios_add_dmac_handler(MX_DMAC_SIF0, sif0_handler, 0, 0);
  mx_bios_enable_dmac(MX_DMAC_SIF0);
  return sif0_id < 0 ? -1 : 0;
}
