/* The BIOS's TLB back, at every start. ExecPS2 leaves the TLB as it is, so a
   program killed by a reset would leave its VM entries behind: the BIOS's
   uncached and UCAB ranges gone, pages mapped to old frames. The next
   program's mx_vm_init would take those for the BIOS's and keep them. A
   fixed table and not what was found at the start: that may already be such
   a TLB. */

#include <mx/cpu.h>

#include "mxlink.h"

#define TLB_N 48
#define TLB_FIRST 2  /* 0, 1: the scratchpad and the kernel, never changed */
#define TLB_USED 39  /* the others: unused, V = 0 */

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
                   "mfc0 %0, $5\n\tmfc0 %1, $10\n\tmfc0 %2, $2\n\tmfc0 %3, $3"
                   : "=r"(*pm), "=r"(*hi), "=r"(*lo0), "=r"(*lo1)
                   : "r"(i));
}

static void tlb_write(u32 i, u32 pm, u32 hi, u32 lo0, u32 lo1) {
  __asm__ volatile("mtc0 %0, $0\n\tmtc0 %1, $5\n\tmtc0 %2, $10\n\tmtc0 %3, $2\n\tmtc0 %4, $3\n\t"
                   "sync.p\n\ttlbwi\n\tsync.p"
                   :
                   : "r"(i), "r"(pm), "r"(hi), "r"(lo0), "r"(lo1));
}

/* After ExecPS2, not before it: until then a killed program's vblank handler
   clears the entries it thinks are its own, and with interrupts off through
   ExecPS2 one of its interrupts stays pending until its handler is gone,
   which hangs the kernel. The entries that differ are cleared first (two
   entries matching one address are undefined), parked above the BIOS's
   unused ones, then written. Those that are right stay, so the code running
   here stays mapped. */
void tlb_restore(void) {
  u8 differs[TLB_N];
  int was = mx_di();
  for (u32 i = TLB_FIRST; i < TLB_N; i++) {
    u32 pm, hi, lo0, lo1;
    tlb_read(i, &pm, &hi, &lo0, &lo1);
    differs[i] = i >= TLB_USED || pm != bios_tlb[i].pm || hi != bios_tlb[i].hi ||
                 lo0 != bios_tlb[i].lo0 || lo1 != bios_tlb[i].lo1;
    if (differs[i])
      tlb_write(i, 0, 0xE0000000u + ((TLB_N + i) << 13), 0, 0);
  }
  for (u32 i = TLB_FIRST; i < TLB_N; i++)
    if (i >= TLB_USED)
      tlb_write(i, 0, 0xE0000000u + (i << 13), 0, 0);
    else if (differs[i])
      tlb_write(i, bios_tlb[i].pm, bios_tlb[i].hi, bios_tlb[i].lo0, bios_tlb[i].lo1);
  mx_irq_restore(was);
}
