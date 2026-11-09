/* The kernel's log (kernel version 16, <mx/log.h>; docs/mx-os-apps-plan.md
   step 2e-3): what every process writes to its tty (mx_printf: libmxk's
   debug output comes through the kernel's mx_tty_write) and the kernel's
   own output, each write a record with its process, in a ring of LOG_BYTES.
   Any process reads it from where it last was (mx_log_read), in its own
   thread: the copy out is in its address space. A reader that fell behind
   by more than the ring goes on from the oldest record kept.

   A record: its process (1 byte), its length (1 byte), the bytes; writes
   are split into records of up to 64 bytes. Positions count bytes since the start, so they only
   grow. */

#include <mx/bios.h>
#include <mx/cpu.h>
#include <mx/log.h>
#include <mx/sif.h>
#include <mx/string.h>

#include "ksched.h"
#include "mxlink.h"

#define LOG_BYTES 8192 /* a power of two */

static u8 ring[LOG_BYTES];
static u32 head, tail; /* the next write's position, the oldest record's */
static int wait_sema = -1;
static volatile int waiting; /* readers waiting for the next write */

struct sema_param {
  s32 count, max_count, init_count, wait_threads;
  u32 attr, option;
};

/* after ksched_init, in a kernel thread: its semaphore is the kernel's */
void klog_init(void) {
  head = tail = 0;
  struct sema_param p = {0, 64, 0, 0, 0, 0};
  wait_sema = mx_bios_create_sema(&p);
}

static u8 at(u32 pos) { return ring[pos & (LOG_BYTES - 1)]; }

/* interrupts off: one record of n < 256 bytes, the oldest dropped for
   room */
static void put(int proc, const char *s, u32 n) {
  while (head + 2 + n - tail > LOG_BYTES)
    tail += 2u + at(tail + 1);
  ring[head & (LOG_BYTES - 1)] = (u8)proc;
  ring[(head + 1) & (LOG_BYTES - 1)] = (u8)n;
  head += 2;
  for (u32 i = 0; i < n; i++, head++)
    ring[head & (LOG_BYTES - 1)] = (u8)s[i];
}

static int proc_now(void) {
  int p = ksched_active() ? ksched_thread_proc(mx_bios_get_thread_id()) : 0;
  return p > 0 ? p : 0;
}

/* any context: a write of the process running (0: the kernel). The text
   is the caller's, read in its address space. */
void klog_write(const char *s, u32 n) {
  int proc = proc_now();
  char b[64]; /* the caller's text may be in VM: read with interrupts on */
  while (n) {
    u32 k = n > sizeof b ? sizeof b : n;
    memcpy(b, s, k);
    int was = mx_di();
    put(proc, b, k);
    for (; waiting > 0 && wait_sema >= 0; waiting--)
      mx_bios_isignal_sema(wait_sema);
    mx_irq_restore(was);
    s += k, n -= k;
  }
}

/* maxsdk's debug output in the kernel (src/base/debug.c) */
void mx__debug_tap(const char *s, u32 n) { klog_write(s, n); }

/* the service mx_tty_write: a program's output, into the log, then the tty */
int klog_tty_write(const char *s, u32 n) {
  klog_write(s, n);
  return mx_tty_write(s, n);
}

/* <mx/log.h>'s mx_log_read */
int klog_read(u32 *pos, void *buf, u32 max, int *proc, int wait) {
  if (!pos)
    return -1;
  for (;;) {
    int was = mx_di();
    u32 p = *pos;
    if ((s32)(p - tail) < 0 || (s32)(p - head) > 0)
      p = tail; /* overwritten meanwhile, or not a position of this log */
    if (p != head) {
      u32 n = at(p + 1);
      int who = at(p);
      /* copied out with interrupts on: a record that a write meanwhile
         overwrites is read again from the oldest */
      mx_irq_restore(was);
      u32 k = n < max ? n : max;
      u8 *d = buf;
      for (u32 i = 0; i < k; i++)
        d[i] = at(p + 2 + i);
      was = mx_di();
      int lost = (s32)(p - tail) < 0;
      mx_irq_restore(was);
      if (lost) {
        *pos = tail;
        continue;
      }
      *pos = p + 2 + n;
      if (proc)
        *proc = who;
      return (int)k;
    }
    if (!wait || wait_sema < 0) {
      mx_irq_restore(was);
      *pos = p;
      return 0;
    }
    waiting++;
    mx_irq_restore(was);
    mx_bios_wait_sema(wait_sema);
  }
}
