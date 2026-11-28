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
#define TLB_USED 45  /* the others: unused, V = 0 */
/* The kernel's own two entries (MX_KERNEL_LO, the last MiB: pages of 256
   KiB), in place of the end of the BIOS's entry 21 (0x1800000-0x1FFFFFF):
   its code without D, so a store of a program there is a TLB modified
   exception, and its data. Never cleared: the code running here is in them. */
#define TLB_KCODE 21
#define TLB_KDATA 40
#define LO_D 4u
#define PM256K 0x0007e000u
#define KCODE_HI MX_KERNEL_LO
#define KCODE_LO0 (MX_KERNEL_LO >> 12 << 6 | 0x1Bu)
#define KCODE_LO1 ((MX_KERNEL_LO + 0x40000u) >> 12 << 6 | 0x1Bu)
#define KDATA_HI (MX_KERNEL_LO + 0x80000u)
#define KDATA_LO0 ((MX_KERNEL_LO + 0x80000u) >> 12 << 6 | 0x1Fu)
#define KDATA_LO1 ((MX_KERNEL_LO + 0xC0000u) >> 12 << 6 | 0x1Fu)

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
    /* cached main memory from 1 MiB (13-20: TLB_CORE, a program's core in
       their place); the BIOS has these one slot up, and one entry for
       0x1800000-0x1FFFFFF, which ends in the kernel here: 20 and 41-44 */
    {0x0007e000, 0x00100000, 0x0000401f, 0x0000501f},
    {0x0007e000, 0x00180000, 0x0000601f, 0x0000701f},
    {0x001fe000, 0x00200000, 0x0000801f, 0x0000c01f},
    {0x001fe000, 0x00400000, 0x0001001f, 0x0001401f},
    {0x001fe000, 0x00600000, 0x0001801f, 0x0001c01f},
    {0x007fe000, 0x00800000, 0x0002001f, 0x0003001f},
    {0x007fe000, 0x01000000, 0x0004001f, 0x0005001f},
    {0x001fe000, 0x01800000, 0x0006001f, 0x0006401f},
    [TLB_KCODE] = {PM256K, KCODE_HI, KCODE_LO0, KCODE_LO1},
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
    {0x0007e000, 0x00080000, 0x0000201f, 0x0000301f}, /* the BIOS's 13 */
    [TLB_KDATA] = {PM256K, KDATA_HI, KDATA_LO0, KDATA_LO1},
    {0x001fe000, 0x01a00000, 0x0006801f, 0x0006c01f},
    {0x001fe000, 0x01c00000, 0x0007001f, 0x0007401f},
    {0x0007e000, 0x01e00000, 0x0007801f, 0x0007901f},
    {0x0007e000, 0x01e80000, 0x0007a01f, 0x0007b01f},
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

/* The kernel's code entry with or without the right to write: one tlbwi (the
   code running here is in it, so it is not cleared first). */
static void tlb_kcode(u32 d) {
  tlb_write(TLB_KCODE, PM256K, KCODE_HI, KCODE_LO0 | d, KCODE_LO1 | d);
}

void tlb_writable(void) {
  int was = mx_di();
  tlb_kcode(LO_D);
  mx_irq_restore(was);
}

/* The first start (mxboot's or PCSX2's ExecPS2): the kernel runs under the
   BIOS's entry 21. Its own two in its place, with nothing read or written
   between the two tlbwi (all in registers): after the first one only its
   code is mapped. Interrupts off. */
static void tlb_kernel_first(void) {
  u32 pm, hi, lo0, lo1;
  tlb_read(TLB_KCODE, &pm, &hi, &lo0, &lo1);
  if (pm == PM256K && (hi & ~0xFFu) == KCODE_HI)
    return; /* a restart, or a kernel that replaced another: its own already */
  __asm__ volatile("mtc0 %0, $0\n\tmtc0 %2, $5\n\tmtc0 %3, $10\n\tmtc0 %4, $2\n\tmtc0 %5, $3\n\t"
                   "sync.p\n\ttlbwi\n\tsync.p\n\t"
                   "mtc0 %1, $0\n\tmtc0 %6, $10\n\tmtc0 %7, $2\n\tmtc0 %8, $3\n\t"
                   "sync.p\n\ttlbwi\n\tsync.p\n\tmtc0 %9, $10\n\tsync.p"
                   :
                   : "r"(TLB_KCODE), "r"(TLB_KDATA), "r"(PM256K), "r"(KCODE_HI), "r"(KCODE_LO0),
                     "r"(KCODE_LO1), "r"(KDATA_HI), "r"(KDATA_LO0), "r"(KDATA_LO1), "r"(mx__asid));
}

/* After ExecPS2, not before it: until then a killed program's vblank handler
   clears the entries it thinks are its own, and with interrupts off through
   ExecPS2 one of its interrupts stays pending until its handler is gone,
   which hangs the kernel. The entries that differ are cleared first (two
   entries matching one address are undefined), parked above the BIOS's
   unused ones, then written. Those that are right stay, so the code running
   here stays mapped. */
#define TLB_CORE 13
#define TLB_CORE_N 8
/* the processes beside the one in front (step 2e-2, kproc.c): their cores
   reach to va_end, and tlb_restore leaves out the BIOS's global entries over
   [0x10_0000, va_end), which would match their addresses under any ASID.
   Their cores have no entries of their own (mx OS plan step 6, part 5,
   2026-11-27; before, each kept its slots and the one in front had those
   left: a shell and a service took 4 of the 8), so the one in front has all
   8. */
static u32 keep_end;
void tlb_core_side(u32 va_end) { keep_end = va_end; }

/* the BIOS's entry i maps over [0x10_0000, keep_end): the side's range */
static int over_side(u32 i);

void tlb_restore(void) {
  u8 differs[TLB_N];
  int was = mx_di();
  /* the kernel's address space: ASID 0 (tlbr and tlbwi below set it back
     to this), virtual = physical */
  mx__asid_set(0);
  mx__core_off = 0;
  tlb_kernel_first();
  for (u32 i = TLB_FIRST; i < TLB_N; i++) {
    u32 pm, hi, lo0, lo1;
    if (i == TLB_KCODE || i == TLB_KDATA) {
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
    /* beside a process's core: no global entry of the BIOS over its range
       (two entries matching one address are undefined), the others as
       they are (the kernel's own use of main memory: MODULE_MEM) */
    if (keep_end && i >= TLB_CORE && i < TLB_CORE + TLB_CORE_N && over_side(i))
      tlb_write(i, 0, 0xE0000000u + (i << 13), 0, 0);
    else if (i >= TLB_USED)
      tlb_write(i, 0, 0xE0000000u + (i << 13), 0, 0);
    else if (differs[i])
      tlb_write(i, bios_tlb[i].pm, bios_tlb[i].hi, bios_tlb[i].lo0, bios_tlb[i].lo1);
  tlb_kcode(0);
  mx_irq_restore(was);
}

/* A program's core [0x10_0000, va_end) at physical 0x10_0000 + off, under
   the ASID now set (mx__asid; docs/mx-os-plan.md, step 2b): the BIOS's
   global entries of cached main memory from 0x10_0000 (13-20) give way to
   entries of that ASID (the main stack is in the core: maxsdk's VM linker
   scripts). The entries it took, or -1, the TLB unchanged, if more than
   those 8 are needed
   or it reaches the entries that stay (41-44). tlb_restore writes the
   BIOS's back.

   The cover (mx OS plan step 6, 2026-11-17): pairs of the largest pages
   that fit, 4 MiB down to 16 KiB, each pair at a multiple of twice its page
   and each page at a multiple of its size in physical memory too; and for
   the end, an entry with only its even half (the odd one invalid: an
   access there is a TLB exception and the kernel's report,
   tests/ps2/k70_tlbhalf.c). Nothing else may be mapped where an invalid
   half is (two entries matching one address), so only the last entry is
   such a one. Before, the core was whole MiBs (pairs of 256 KiB at least):
   the host link's service took 1 MiB for 350 KiB, the shell 2 for 1.25. */
#define LO_CORE 0x1Eu /* cached, D, V, not global */
#define CORE_VA_MAX 0x01A00000u
static const u32 core_page[5] = {4u << 20, 1u << 20, 256u << 10, 64u << 10, 16u << 10};

/* the entries of [0x10_0000, va_end) at + off: how many (-1: more than max,
   or it cannot be covered), into pm, hi, lo0, lo1 if not 0; *largest: the
   largest page used */
static int core_cover(u32 va_end, u32 off, u32 max, u32 *pm, u32 *hi, u32 *lo0, u32 *lo1, u32 *largest) {
  u32 n = 0, big = 0;
  for (u32 v = 0x00100000u; v < va_end; n++) {
    u32 s = 0;
    int half = 0;
    for (u32 k = 0; k < 5 && !s; k++) {
      u32 t = core_page[k];
      if (v % (2 * t) || (v + off) % t)
        continue;
      if (v + 2 * t <= va_end)
        s = t;
      else if (v + t == va_end)
        s = t, half = 1;
    }
    if (!s || n == max)
      return -1;
    if (pm) {
      pm[n] = ((s >> 12) - 1) << 13;
      hi[n] = v | mx__asid;
      lo0[n] = (v + off) >> 12 << 6 | LO_CORE;
      lo1[n] = half ? 0 : (v + s + off) >> 12 << 6 | LO_CORE;
    }
    if (s > big)
      big = s;
    v += half ? s : 2 * s;
  }
  if (largest)
    *largest = big;
  return (int)n;
}

/* The size a core of bytes gets: its end (from 0x10_0000), and what its
   physical run must be a multiple of. Rounded up to 32 KiB, 64 KiB, 256 KiB
   or 1 MiB, whichever costs least: what is rounded up is memory no one
   uses, an entry is one the VM's pages do not have (counted as 96 KiB). */
u32 tlb_core_plan(u32 bytes, u32 *align) {
  static const u32 gran[4] = {32u << 10, 64u << 10, 256u << 10, 1u << 20};
  u32 best = 0, best_cost = ~0u, best_big = 1u << 20;
  for (u32 g = 0; g < 4; g++) {
    u32 size = (bytes + gran[g] - 1) & ~(gran[g] - 1), big = 0;
    /* off as a multiple of 1 MiB that is none of 4 MiB: no 4 MiB pages
       planned (the run is not asked to be at a multiple of 4 MiB) */
    int n = core_cover(0x00100000u + size, 1u << 20, TLB_CORE_N, 0, 0, 0, 0, &big);
    if (n < 0)
      continue;
    u32 cost = size - bytes + (u32)n * (96u << 10);
    if (cost < best_cost)
      best = size, best_cost = cost, best_big = big;
  }
  if (!best) /* none fits in the 8 entries: tlb_core_map refuses it */
    best = (bytes + (1u << 20) - 1) & ~((1u << 20) - 1);
  *align = best_big > (1u << 20) ? 1u << 20 : best_big < (32u << 10) ? 32u << 10 : best_big;
  return 0x00100000u + best;
}

int tlb_core_map(u32 va_end, u32 off) {
  u32 pm[TLB_CORE_N], hi[TLB_CORE_N], lo0[TLB_CORE_N], lo1[TLB_CORE_N];
  u32 first = TLB_CORE, max = TLB_CORE_N;
  if (va_end > CORE_VA_MAX)
    return -1;
  int n = core_cover(va_end, off, max, pm, hi, lo0, lo1, 0);
  if (n < 0)
    return -1;
  int was = mx_di();
  /* all of them parked first: no two entries may match one address */
  for (u32 i = 0; i < max; i++)
    tlb_write(first + i, 0, 0xE0000000u + ((TLB_N + first + i) << 13), 0, 0);
  for (int i = 0; i < n; i++)
    tlb_write(first + (u32)i, pm[i], hi[i], lo0[i], lo1[i]);
  mx_irq_restore(was);
  return n;
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

/* PCSX2 again: a side's core has no entries of its own (kproc.c), so its
   addresses would be the front's entries' there, and its threads would run
   in the front's core. While a side's space is live, the front's core
   entries (slots 13-20, which the VM's ring never takes) are out, and back
   at the switch to the front's. All valid ones there, whatever their ASID:
   while the kernel starts a program they are already the new one's, under
   an ASID that is not yet the live one. Only where tlb_emu is set. */
static u32 hidden[TLB_CORE_N][4];
static u8 nhidden, hidden_at[TLB_CORE_N];
void tlb_emu_hide(void) {
  nhidden = 0;
  for (u32 i = TLB_CORE; i < TLB_CORE + TLB_CORE_N; i++) {
    u32 *e = hidden[nhidden];
    tlb_read(i, &e[0], &e[1], &e[2], &e[3]);
    if ((e[2] & e[3] & 1) || !((e[2] | e[3]) & 2))
      continue;
    tlb_write(i, 0, 0xE0000000u + ((TLB_N + i) << 13), 0, 0);
    hidden_at[nhidden++] = (u8)i;
  }
}
void tlb_emu_unhide(void) {
  for (u32 k = 0; k < nhidden; k++)
    tlb_write(hidden_at[k], hidden[k][0], hidden[k][1], hidden[k][2], hidden[k][3]);
  nhidden = 0;
}

/* Does the TLB look at the ASID? Two entries at one address (in the last two
   slots, unused: the BIOS's kseg3 ones), each under an ASID of its own, onto
   two of the kernel's pages whose first words differ; read under the first
   ASID: the PS2 gives the first page's word (probe/p33_asid.c), PCSX2 the
   second's, the last written. All put back. Interrupts off. 1: it does not. */
int tlb_ignores_asid(void) {
  const u32 va = 0x7F000000u, a = MX_KERNEL_LO, end = MX_KERNEL_LO + 0x40000u;
  u32 b = a + 0x1000u;
  while (b < end && *(volatile u32 *)(uintptr_t)b == *(volatile u32 *)(uintptr_t)a)
    b += 0x1000u;
  if (b >= end)
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

/* For the report of a hang (main.c): TLB entry i as it is. */
void tlb_dump_entry(u32 i, u32 o[4]) {
  int was = mx_di();
  tlb_read(i, &o[0], &o[1], &o[2], &o[3]);
  mx_irq_restore(was);
}
