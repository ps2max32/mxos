/* The BIOS's TLB back, at every start. ExecPS2 leaves the TLB as it is, so a
   program killed by a reset would leave its VM entries behind: the BIOS's
   uncached and UCAB ranges gone, pages mapped to old frames. The next
   program's mx_vm_init would take those for the BIOS's and keep them. A
   fixed table and not what was found at the start: that may already be such
   a TLB. */

#include <mx/cpu.h>
#include <mx/kernel.h>

#include "mxlink.h"

#define TLB_N 48
#define TLB_FIRST 2  /* 0, 1: the scratchpad and the kernel, never changed */
#define TLB_USED 39  /* the others: unused, V = 0 */
#define TLB_RESIDENT 13 /* 0x80000-0xFFFFF: mxlink, its code in the upper half */
#define LO_D 4u

static const struct {
  u32 pm, hi, lo0, lo1;
} bios_tlb[TLB_USED] = {
    [TLB_FIRST] = {0x00000000, 0x10000000, 0x00400017, 0x00400053},
    {0x00000000, 0x10002000, 0x00400097, 0x004000d7},
    {0x00000000, 0x10004000, 0x00400117, 0x00400157},
    {0x00000000, 0x10006000, 0x00400197, 0x004001d7},
    {0x00000000, 0x10008000, 0x00400217, 0x00400257},
    {0x00000000, 0x1000a000, 0x00400297, 0x004002d7},
    {0x00000000, 0x1000c000, 0x00400313, 0x00400357},
    {0x00000000, 0x1000e000, 0x00400397, 0x004003d7},
    {0x0001e000, 0x11000000, 0x00440017, 0x00440415},
    {0x0001e000, 0x12000000, 0x00480017, 0x00480415},
    {0x01ffe000, 0x1e000000, 0x00780017, 0x007c0017},
    {0x0007e000, 0x00080000, 0x0000201f, 0x0000301f},
    {0x0007e000, 0x00100000, 0x0000401f, 0x0000501f},
    {0x0007e000, 0x00180000, 0x0000601f, 0x0000701f},
    {0x001fe000, 0x00200000, 0x0000801f, 0x0000c01f},
    {0x001fe000, 0x00400000, 0x0001001f, 0x0001401f},
    {0x001fe000, 0x00600000, 0x0001801f, 0x0001c01f},
    {0x007fe000, 0x00800000, 0x0002001f, 0x0003001f},
    {0x007fe000, 0x01000000, 0x0004001f, 0x0005001f},
    {0x007fe000, 0x01800000, 0x0006001f, 0x0007001f},
    {0x0007e000, 0x20080000, 0x00002017, 0x00003017},
    {0x0007e000, 0x20100000, 0x00004017, 0x00005017},
    {0x0007e000, 0x20180000, 0x00006017, 0x00007017},
    {0x001fe000, 0x20200000, 0x00008017, 0x0000c017},
    {0x001fe000, 0x20400000, 0x00010017, 0x00014017},
    {0x001fe000, 0x20600000, 0x00018017, 0x0001c017},
    {0x007fe000, 0x20800000, 0x00020017, 0x00030017},
    {0x007fe000, 0x21000000, 0x00040017, 0x00050017},
    {0x007fe000, 0x21800000, 0x00060017, 0x00070017},
    {0x0007e000, 0x30100000, 0x0000403f, 0x0000503f},
    {0x0007e000, 0x30180000, 0x0000603f, 0x0000703f},
    {0x001fe000, 0x30200000, 0x0000803f, 0x0000c03f},
    {0x001fe000, 0x30400000, 0x0001003f, 0x0001403f},
    {0x001fe000, 0x30600000, 0x0001803f, 0x0001c03f},
    {0x007fe000, 0x30800000, 0x0002003f, 0x0003003f},
    {0x007fe000, 0x31000000, 0x0004003f, 0x0005003f},
    {0x007fe000, 0x31800000, 0x0006003f, 0x0007003f},
};

static void tlb_read(u32 i, u32 *pm, u32 *hi, u32 *lo0, u32 *lo1) {
  __asm__ volatile("mtc0 %4, $0\n\tsync.p\n\ttlbr\n\tsync.p\n\t"
                   "mfc0 %0, $5\n\tmfc0 %1, $10\n\tmfc0 %2, $2\n\tmfc0 %3, $3\n\t"
                   "mtc0 %5, $10\n\tsync.p"
                   : "=&r"(*pm), "=&r"(*hi), "=&r"(*lo0), "=&r"(*lo1)
                   : "r"(i), "r"(mx__asid));
}

static void tlb_write(u32 i, u32 pm, u32 hi, u32 lo0, u32 lo1) {
  __asm__ volatile("mtc0 %0, $0\n\tmtc0 %1, $5\n\tmtc0 %2, $10\n\tmtc0 %3, $2\n\tmtc0 %4, $3\n\t"
                   "sync.p\n\ttlbwi\n\tsync.p\n\tmtc0 %5, $10\n\tsync.p"
                   :
                   : "r"(i), "r"(pm), "r"(hi), "r"(lo0), "r"(lo1), "r"(mx__asid));
}

/* mxlink's entry as it should be: the upper half without D for the resident
   one, so a store of a program there is a TLB modified exception. Not the
   lower half: the kernel writes 0x80000-0x87FFF through it, and without D the
   PS2 hangs. One tlbwi: the code running here is in it, so it is not cleared
   first. */
static void tlb_resident(u32 lo1) {
  tlb_write(TLB_RESIDENT, bios_tlb[TLB_RESIDENT].pm, bios_tlb[TLB_RESIDENT].hi,
            bios_tlb[TLB_RESIDENT].lo0, lo1);
}

void tlb_writable(void) {
  int was = mx_di();
  tlb_resident(bios_tlb[TLB_RESIDENT].lo1);
  mx_irq_restore(was);
}

/* After ExecPS2, not before it: until then a killed program's vblank handler
   clears the entries it thinks are its own, and with interrupts off through
   ExecPS2 one of its interrupts stays pending until its handler is gone,
   which hangs the kernel. The entries that differ are cleared first (two
   entries matching one address are undefined), parked above the BIOS's
   unused ones, then written. Those that are right stay, so the code running
   here stays mapped. */
#define TLB_CORE 14
#define TLB_CORE_N 8
/* the core slots of a process beside the one in front (step 2e-2, kproc.c):
   kept by tlb_restore, which then leaves out the BIOS's entries over its
   range [0x10_0000, va_end), and tlb_core_map writes after them */
static u32 core_keep, keep_end;
void tlb_core_keep(u32 n, u32 va_end) { core_keep = n, keep_end = va_end; }

/* the BIOS's entry i maps over [0x10_0000, keep_end): the side's range */
static int over_side(u32 i);

void tlb_restore(void) {
  u8 differs[TLB_N];
  int was = mx_di();
  /* the kernel's address space: ASID 0 (tlbr and tlbwi below set it back
     to this), virtual = physical */
  mx__asid_set(0);
  mx__core_off = 0;
  for (u32 i = TLB_FIRST; i < TLB_N; i++) {
    u32 pm, hi, lo0, lo1;
    if (i == TLB_RESIDENT || (i >= TLB_CORE && i < TLB_CORE + core_keep)) {
      differs[i] = 0;
      continue;
    }
    tlb_read(i, &pm, &hi, &lo0, &lo1);
    differs[i] = i >= TLB_USED || pm != bios_tlb[i].pm || hi != bios_tlb[i].hi ||
                 lo0 != bios_tlb[i].lo0 || lo1 != bios_tlb[i].lo1;
    if (differs[i])
      tlb_write(i, 0, 0xE0000000u + ((TLB_N + i) << 13), 0, 0);
  }
  for (u32 i = TLB_FIRST; i < TLB_N; i++)
    if (i >= TLB_CORE && i < TLB_CORE + core_keep)
      continue;
    /* beside a process's core: no global entry of the BIOS over its range
       (two entries matching one address are undefined), the others as
       they are (the kernel's own use of main memory: MODULE_MEM) */
    else if (core_keep && i >= TLB_CORE && i < TLB_CORE + TLB_CORE_N && over_side(i))
      tlb_write(i, 0, 0xE0000000u + (i << 13), 0, 0);
    else if (i >= TLB_USED)
      tlb_write(i, 0, 0xE0000000u + (i << 13), 0, 0);
    else if (differs[i])
      tlb_write(i, bios_tlb[i].pm, bios_tlb[i].hi, bios_tlb[i].lo0, bios_tlb[i].lo1);
  tlb_resident(exec_resident() ? bios_tlb[TLB_RESIDENT].lo1 & ~LO_D : bios_tlb[TLB_RESIDENT].lo1);
  mx_irq_restore(was);
}

/* A program's core [0x10_0000, va_end) at physical 0x10_0000 + off, under
   the ASID now set (mx__asid; docs/mx-os-plan.md, step 2b): the BIOS's
   global entries of cached main memory from 0x10_0000 (14-21) give way to
   entries of that ASID, pages of 4 MiB, 1 MiB or 256 KiB as the alignment
   allows (the main stack is in the core: maxsdk's VM linker scripts). va_end
   and off: multiples of 1 MiB. The entries it took (after those kept for a
   process beside, tlb_core_keep), or -1, the TLB unchanged, if more than
   those 8 are needed. tlb_restore writes the BIOS's back. */
#define LO_CORE 0x1Eu /* cached, D, V, not global */

int tlb_core_map(u32 va_end, u32 off) {
  u32 pm[TLB_CORE_N], hi[TLB_CORE_N], lo0[TLB_CORE_N], lo1[TLB_CORE_N], n = 0;
  u32 first = TLB_CORE + core_keep, max = TLB_CORE_N - core_keep;
  for (u32 v = 0x00100000u; v < va_end; n++) {
    u32 s = 4u << 20;
    while (s > (256u << 10) && (v % (2 * s) || (v + off) % s || v + 2 * s > va_end))
      s >>= 2;
    if (n == max || v % (2 * s) || (v + off) % s)
      return -1;
    pm[n] = ((s >> 12) - 1) << 13;
    hi[n] = v | mx__asid;
    lo0[n] = (v + off) >> 12 << 6 | LO_CORE;
    lo1[n] = (v + s + off) >> 12 << 6 | LO_CORE;
    v += 2 * s;
  }
  int was = mx_di();
  /* all of them parked first: no two entries may match one address */
  for (u32 i = 0; i < max; i++)
    tlb_write(first + i, 0, 0xE0000000u + ((TLB_N + first + i) << 13), 0, 0);
  for (u32 i = 0; i < n; i++)
    tlb_write(first + i, pm[i], hi[i], lo0[i], lo1[i]);
  mx_irq_restore(was);
  return (int)n;
}

static int over_side(u32 i) {
  u32 page = ((bios_tlb[i].pm >> 13) + 1) << 12, lo = bios_tlb[i].hi & ~0x1FFFu;
  return lo < keep_end && lo + 2 * page > 0x00100000u;
}

/* PCSX2 maps an entry when it is written and never looks at its ASID: of two
   entries at one address, the last written is the one used. At a process
   switch (kproc.c), the entries of the process that runs next (its ASID, G =
   0) are written again, so its are the ones used, as on the PS2. Only where
   the TLB is so (tlb_ignores_asid): the emu build, or the PS2 build on PCSX2. */
int tlb_emu;
void tlb_emu_asid(u32 asid) {
  for (u32 i = TLB_FIRST; i < TLB_N; i++) {
    u32 pm, hi, lo0, lo1;
    tlb_read(i, &pm, &hi, &lo0, &lo1);
    if ((lo0 & lo1 & 1) || (hi & 0xFFu) != asid || !((lo0 | lo1) & 2))
      continue;
    tlb_write(i, pm, hi, lo0, lo1);
  }
}

/* Does the TLB look at the ASID? Two entries at one address (in the last two
   slots, unused: the BIOS's kseg3 ones), each under an ASID of its own, onto
   two of the kernel's pages whose first words differ; read under the first
   ASID: the PS2 gives the first page's word (probe/p33_asid.c), PCSX2 the
   second's, the last written. All put back. Interrupts off. 1: it does not. */
int tlb_ignores_asid(void) {
  const u32 va = 0x7F000000u, a = 0x88000u;
  u32 b = a + 0x1000u;
  while (b < 0xC0000u && *(volatile u32 *)(uintptr_t)b == *(volatile u32 *)(uintptr_t)a)
    b += 0x1000u;
  if (b >= 0xC0000u)
    return 0; /* no two pages to tell apart: as the PS2 */
  u32 want_a = *(volatile u32 *)(uintptr_t)a, want_b = *(volatile u32 *)(uintptr_t)b;
  int was = mx_di();
  u32 old = mx__asid, s[2][4];
  for (u32 k = 0; k < 2; k++)
    tlb_read(TLB_N - 2 + k, &s[k][0], &s[k][1], &s[k][2], &s[k][3]);
  tlb_write(TLB_N - 2, 0, va | 0x41u, a >> 12 << 6 | LO_CORE, 0);
  tlb_write(TLB_N - 1, 0, va | 0x42u, b >> 12 << 6 | LO_CORE, 0);
  mx__asid = 0x41u;
  __asm__ volatile("mtc0 %0, $10\n\tsync.p" : : "r"(mx__asid) : "memory");
  u32 got = *(volatile u32 *)(uintptr_t)va;
  mx__asid = old;
  for (u32 k = 0; k < 2; k++)
    tlb_write(TLB_N - 2 + k, s[k][0], s[k][1], s[k][2], s[k][3]);
  mx_irq_restore(was);
  return got == want_b && got != want_a;
}
