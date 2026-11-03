/* Programs: the ELF read through the IOP's file server and put in place by
   the EE (maxsdk's loader/mxload.c, which this replaces), where ps2link has
   the IOP do it (SifLoadElf), then started in a thread of its own, as
   ps2link starts one.

   Segments in maxsdk's VM (a program linked with ld/maxsdk-vm.ld) are left
   to the program's start-up, which reads them from argv[0]; such a program
   gets MX_LOADER_MAGIC in place of ps2link's pid (<mx/loader.h>), so it
   knows it may. Every other program gets ps2link's argument block as it
   is: ps2sdk programs run as under ps2link. */

#include <mx/bios.h>
#include <mx/debug.h>
#include <mx/loader.h>
#include <mx/sif.h>
#include <mx/string.h>
#include <mx/vm.h>

#include "mxlink.h"

struct ehdr {
  u8 ident[16];
  u16 type, machine;
  u32 version, entry, phoff, shoff, flags;
  u16 ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};

struct phdr {
  u32 type, offset, vaddr, paddr, filesz, memsz, flags, align;
};

/* EE kernel CreateThread parameters */
struct thread_param {
  int status;
  void *func;
  void *stack;
  int stack_size;
  void *gp;
  int initial_priority;
  int current_priority;
  u32 attr, option;
};

#define PT_LOAD 1
#define MAX_PHDRS 16
#define LOW 0x00100000u
#define TOP 0x02000000u
/* below LOW: loaders (ps2link, mxlink itself at 0x94000), above the EE
   kernel; started by ExecPS2 */
#define LOADER_LOW 0x00090000u
#define USER_PRIO 64

static int user_tid; /* 0: no program */
static struct mx_loader_args largs;
static u8 user_stack[16 * 1024] __attribute__((aligned(16)));

int exec_running(void) { return user_tid != 0; }

void exec_kill(void) {
  if (!user_tid)
    return;
  mx_bios_terminate_thread(user_tid);
  mx_bios_delete_thread(user_tid);
  user_tid = 0;
}

static int fail(int fd, const char *path, const char *what) {
  if (fd >= 0)
    mx_fio_close(fd);
  con_log(CON_RED, "%s: %s", path, what);
  return -1;
}

/* argv from ps2link's packet: argc strings one after the other */
static void set_args(int argc, const char *args, u32 len) {
  struct mx_args *a = &largs.args;
  if (len > sizeof a->payload)
    len = sizeof a->payload;
  memcpy(a->payload, args, len);
  a->payload[sizeof a->payload - 1] = 0;
  if (argc > MX_MAX_ARGS)
    argc = MX_MAX_ARGS;
  u32 t = 0;
  int i = 0;
  while (i < argc && t < sizeof a->payload) {
    a->argv[i++] = &a->payload[t];
    t += strlen(&a->payload[t]) + 1;
  }
  a->argc = i;
}

int exec_elf(int argc, const char *args, u32 args_len) {
  if (user_tid) {
    con_log(CON_RED, "a program is running already");
    return -1;
  }
  set_args(argc < 1 ? 1 : argc, args, args_len);
  const char *path = largs.args.argv[0];
  if (exec_returns())
    con_log(CON_WHITE, "exec %s (%u returned before, the last with status %d)", path,
            exec_returns(), (int)exec_last_status());
  else
    con_log(CON_WHITE, "exec %s", path);

  static struct ehdr eh;
  static struct phdr ph[MAX_PHDRS];
  int fd = mx_fio_open(path, MX_FIO_RDONLY);
  if (fd < 0)
    return fail(-1, path, "cannot open");
  if (mx_fio_read(fd, &eh, sizeof eh) != sizeof eh || eh.ident[0] != 0x7f ||
      eh.ident[1] != 'E' || eh.ident[2] != 'L' || eh.ident[3] != 'F' ||
      eh.machine != 8 /* MIPS */)
    return fail(fd, path, "not a MIPS ELF");
  if (eh.phentsize != sizeof(struct phdr) || eh.phnum == 0 || eh.phnum > MAX_PHDRS)
    return fail(fd, path, "unexpected program headers");
  u32 phbytes = eh.phnum * sizeof(struct phdr);
  if (mx_fio_lseek(fd, (s32)eh.phoff, MX_FIO_SEEK_SET) != (int)eh.phoff ||
      mx_fio_read(fd, ph, phbytes) != (int)phbytes)
    return fail(fd, path, "cannot read the program headers");

  /* check all before loading any: a bad one leaves memory as it was */
  extern char __text_start[], __stack[], __stack_size[];
  u32 self_start = (u32)(uintptr_t)__text_start;
  u32 self_end = (u32)(uintptr_t)__stack + (u32)(uintptr_t)__stack_size;
  int vm = 0, loader = 0;
  for (u32 i = 0; i < eh.phnum; i++) {
    const struct phdr *p = &ph[i];
    if (p->type != PT_LOAD || p->memsz == 0)
      continue;
    if (p->vaddr >= MX_VM_BASE && p->vaddr < MX_VM_LIMIT) {
      vm++;
      continue;
    }
    if (p->vaddr >= LOADER_LOW && p->vaddr < LOW)
      loader = 1;
    if (p->filesz > p->memsz || p->vaddr < LOADER_LOW || p->vaddr + p->memsz > TOP ||
        p->vaddr + p->memsz < p->vaddr) {
      con_log(CON_RED, "segment %u at %08x, %u bytes: not in %08x-%08x", i, p->vaddr, p->memsz,
              LOADER_LOW, TOP);
      return fail(fd, path, "a segment is not where mxlink can load it");
    }
    /* not over mxlink itself (make high: at 0x1E00000, in the programs'
       memory), its stack included: starting mxlink-high.elf from it hung
       the PS2 */
    if (p->vaddr < self_end && p->vaddr + p->memsz > self_start) {
      con_log(CON_RED, "segment %u at %08x, %u bytes: over mxlink at %08x-%08x", i, p->vaddr,
              p->memsz, self_start, self_end);
      return fail(fd, path, "it would overwrite mxlink");
    }
  }
  if (loader && vm)
    return fail(fd, path, "a loader (below 1 MiB) with VM segments");
  u32 bytes = 0;
  for (u32 i = 0; i < eh.phnum; i++) {
    const struct phdr *p = &ph[i];
    if (p->type != PT_LOAD || p->memsz == 0 || (p->vaddr >= MX_VM_BASE && p->vaddr < MX_VM_LIMIT))
      continue;
    u8 *d = (u8 *)(uintptr_t)p->vaddr;
    if (p->filesz && (mx_fio_lseek(fd, (s32)p->offset, MX_FIO_SEEK_SET) != (int)p->offset ||
                      mx_fio_read(fd, d, p->filesz) != (int)p->filesz))
      return fail(fd, path, "cannot read a segment");
    memset(d + p->filesz, 0, p->memsz - p->filesz);
    bytes += p->memsz;
  }
  mx_fio_close(fd);
  if (vm)
    con_log(CON_GREY, "  %u KiB loaded, %d VM segment%s left to the program", bytes >> 10, vm,
            vm == 1 ? "" : "s");
  else
    con_log(CON_GREY, "  %u KiB loaded, entry %08x", bytes >> 10, eh.entry);

  mx_bios_flush_cache(0);
  mx_bios_flush_cache(2);

  if (loader) {
    /* a loader (another mxlink, ps2link, ...) gets the machine, as from a
       boot loader: ExecPS2 resets the EE kernel (our threads and handlers
       go; memory stays) and starts it with the arguments. mxlink-high
       starts MXLINK.ELF so, for a test before it is installed. */
    con_log(CON_YELLOW, "  a loader: started by ExecPS2, mxlink ends");
    mx_debug_flush();
    mx_bios_exec_ps2((void *)(uintptr_t)eh.entry, 0, largs.args.argc, largs.args.argv);
    return fail(-1, path, "ExecPS2 returned");
  }

  struct thread_param tp = {0};
  tp.func = (void *)(uintptr_t)eh.entry;
  tp.stack = user_stack;
  tp.stack_size = sizeof user_stack;
  tp.initial_priority = USER_PRIO;
  int tid = mx_bios_create_thread(&tp);
  if (tid < 0)
    return fail(-1, path, "cannot create its thread");
  /* ps2link: the address of the thread id; programs with VM segments:
     MX_LOADER_MAGIC, so maxsdk's start-up reads them */
  largs.pid = vm ? MX_LOADER_MAGIC : (u32)(uintptr_t)&user_tid;
  /* the way back for maxsdk programs: only from the resident mxlink (below
     the programs; mxlink-high is in their memory) */
  largs.ret_magic = self_end <= LOW ? MX_LOADER_RET : 0;
  largs.ret = mxlink_return;
  /* and for the others (ps2sdk's, older maxsdk ones): their Exit */
  if (self_end <= LOW)
    mx_bios_set_syscall(4, mxlink_exit_hook);
  exec_started(path);

  status_state(ST_RUNNING, path);
  /* from here the program owns the IOP's SIF replies and the screen: the
     debug output to the serial port only (con_log) */
  mx_debug_flush();
  mx_debug_sinks(MX_SINK_SIO);
  user_tid = tid;
  if (mx_bios_start_thread(tid, &largs) < 0) {
    user_tid = 0;
    mx_bios_delete_thread(tid);
    return fail(-1, path, "cannot start its thread");
  }
  return 0;
}
