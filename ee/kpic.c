/* The picture of the program paused at the home button (kernel version 32,
   mx OS plan step 5; <mx/screen.h>). The program's frame code (maxsdk's
   src/gs/home.c) reads what it shows back from GS memory into pinned memory
   of its own, before the kernel brings the shell in front, and names it
   here; the shell reads it from there (all of main memory is mapped at
   MX_PHYS_WIN in every address space) while that program is paused. Not in
   resident.ld's state of a program: it is one process's, read by another. */

#include <mx/cpu.h>
#include <mx/kernel.h>
#include <mx/screen.h>
#include <mx/string.h>

#include "mxlink.h"

static struct mx_picture pic;
static u32 pic_phys;

void kpic_set(const struct mx_picture *p, u32 phys) {
  int me = end_proc_front();
  if (!p) {
    if (pic.proc == me)
      pic.bytes = 0;
    return;
  }
  if (me <= 0 || !p->bytes || phys >= MX_KERNEL_LO || p->bytes > MX_KERNEL_LO - phys)
    return;
  pic = *p;
  pic.proc = me;
  pic_phys = phys;
}

int kpic_get(struct mx_picture *p) {
  if (!pic.bytes || kproc_shell() != 1 || !kvm_on() || !exec_proc_parked(pic.proc))
    return -1;
  *p = pic;
  return 0;
}

int kpic_read(void *buf, u32 bytes) {
  struct mx_picture p;
  if (!buf || kpic_get(&p) < 0 || bytes < p.bytes)
    return -1;
  memcpy(buf, (const void *)(uintptr_t)(MX_PHYS_WIN + pic_phys), p.bytes);
  return 0;
}
