/* The CRTC timing without the BIOS (kernel version 22, plan step 2g): maxsdk
   computes the registers from a description (<mx/crtc.h>), this writes them the
   way the BIOS's SetGsCrt does, with what else that does on the console:

   - the HD clock switch, 0xBF80146C bit 3 (with bit 0), for the 74.25 MHz modes
     (720p, 1080i): set or cleared, about 20 ms for the clock, then a GS reset;
   - the video encoder on an I2C bus at 0xBA000000 (address 0x42): reset, then
     a few registers a kind of mode.

   Both were read from the BIOS's code of one console (ROM 2.00, a
   SCPH-70004). The BIOS decides on its own variables, set at its boot: the
   kernel only takes this path where they are the ones of a console it
   knows; elsewhere (PCSX2) it calls the BIOS's SetGsCrt itself for a mode
   the BIOS has, so that programs never do (mx OS plan 2h-3).

   A second console (ROM 2.20, a SCPH-90006; read the same way, 2026-11-13):
   the same GS registers and the same encoder registers, but no clock switch
   and no GS reset for the HD modes, the encoder on another bus (0xBF801900),
   and 576p with the 480p registers and 0x38 = 0x10. */

#include <mx/bios.h>
#include <mx/bits.h>
#include <mx/crtc.h>
#include <mx/hw/regs.h>
#include <mx/kernel.h>
#include <mx/thread.h>

#include "mxlink.h"

/* the BIOS's variables (its boot fills them) */
#define BIOS_GS_VARIANT 0x80022618u /* 0 on the console the path is known for */
#define BIOS_BOARD 0x80022580u      /* 0x32 there; >= 0x31: the clock switch */
#define BIOS_HD_CLOCK 0x80022584u   /* 1 while the switch is on HD */
#define BIOS_ENCODER 0x80022588u    /* the encoder's I2C register 0x7E: 0x1C */

#define CLOCK 0xBF80146Cu
#define CLOCK_HD 0x8
#define CLOCK_ON 0x1

#define ENC 0xBA000000u
#define ENC_GO (ENC + 0x00)     /* 0x82: write */
#define ENC_ADDR (ENC + 0x02)   /* the device: 0x42 (write) */
#define ENC_STAT (ENC + 0x06)   /* write 3 first; bit 0: not acknowledged, bit 1: done */
#define ENC_RESET (ENC + 0x0A)  /* 0, then 1 */
#define ENC_REG (ENC + 0x10)
#define ENC_DATA (ENC + 0x12)
#define ENC_DEV 0x42

/* the console: 0 not known (the BIOS's SetGsCrt), BOARD_200 ROM 2.00's (the
   clock switch, the encoder at 0xBA000000), BOARD_220 ROM 2.20's (no clock
   switch, the encoder at 0xBF801900) */
enum { BOARD_NONE, BOARD_200, BOARD_220 };
static int known;
static u32 seen[3]; /* what kvideo_init read, for kvideo_report */

/* ROM 2.20's variables (its kernel's data is 0x5C0 higher, and it has no
   board variable: the encoder's is 4 lower than that), and two instructions
   of its SetGsCrt and encoder reset that load them: only this kernel */
#define BIOS220_GS_VARIANT 0x80022BD8u
#define BIOS220_ENCODER 0x80022B44u
#define BIOS220_CODE1 0x8000C45Cu /* lw v0, 0x2bd8(v0) */
#define BIOS220_CODE2 0x80007640u /* lw v1, 0x2b44(v1) */

/* ROM 2.20's encoder bus: the device, the register and its value into a
   queue, then go; status bit 0 busy, bit 2 not acknowledged */
#define E2 0xBF801900u
#define E2_DATA (E2 + 0x00)
#define E2_ADDR (E2 + 0x04)
#define E2_ADDR_HI (E2 + 0x05)
#define E2_GO (E2 + 0x06)    /* 0x11 */
#define E2_STAT (E2 + 0x08)  /* write 8 first */
#define E2_RESET (E2 + 0x18) /* 0, then 1 */
/* the BIOS has GetGsDxDyOffset: a ROM after 2001-06-08 (ROMVER's date) */
static int has_dxdy;
/* the BIOS's GetGsDxDyOffset of every mode code it knows (2 to 0x73), asked
   once at the kernel's start and kept (mx OS plan 2h-5: the BIOS's memory,
   which the call reads, becomes the kernel's; its table depends on the GS's
   revision and the BIOS's boot): dx, dy */
#define DXDY_FIRST 2
#define DXDY_N 0x72
static s16 dxdy[DXDY_N][2] __attribute__((section(".kept")));
static int dxdy_known __attribute__((section(".kept")));
#define ROMVER_DATE 0xBFC7FF54u /* YYYYMMDD, in ROMVER (VVVVRTYYYYMMDD) */

void kvideo_init(void) {
  if (!kirq_is_on())
    return;
  /* another ROM than 2.00 (its kernel's data elsewhere, kirq.c): the
     addresses below are not its: nothing read, the BIOS's SetGsCrt. Such a
     ROM is a later one (an earlier kernel's vectors are not known to
     kirq.c), so it has GetGsDxDyOffset */
  u32 kirq_bios_delta(void);
  int rom200 = kirq_bios_delta() == 0;
  int rom220 = kirq_bios_delta() == 0x5C0 && kpeek32(BIOS220_CODE1) == 0x8C422BD8u &&
               kpeek32(BIOS220_CODE2) == 0x8C632B44u;
  u32 variant = rom200 ? kpeek32(BIOS_GS_VARIANT) : rom220 ? kpeek32(BIOS220_GS_VARIANT) : ~0u;
  u32 board = rom200 ? kpeek32(BIOS_BOARD) : 0;
  u32 enc = rom200 ? kpeek32(BIOS_ENCODER) & 0xFF : rom220 ? kpeek32(BIOS220_ENCODER) & 0xFF : 0;
  known = rom200 && variant == 0 && board == 0x32 && enc == 0x1C   ? BOARD_200
          : rom220 && variant == 0 && enc == 0x1C                  ? BOARD_220
                                                                    : BOARD_NONE;
  mx__crtc_set = kvideo_crtc_set;
  u32 date = 0;
  for (u32 i = 0; i < 8 && rom200; i++)
    date = date * 10 + (((kpeek32((ROMVER_DATE + i) & ~3u) >> ((ROMVER_DATE + i) & 3) * 8) & 0xFF) - '0');
  has_dxdy = !rom200 || date > 20010608;
  if (has_dxdy && !dxdy_known) {
    for (int i = 0; i < DXDY_N; i++) {
      int x = 0, y = 0, w, h;
      mx_bios_get_gs_dxdy_offset(DXDY_FIRST + i, &x, &y, &w, &h);
      dxdy[i][0] = (s16)x, dxdy[i][1] = (s16)y;
    }
    dxdy_known = 1;
  }
  seen[0] = variant, seen[1] = board, seen[2] = enc;
}
/* said once the screen is there (main), not at the kernel's first steps
   (kirq.c's kirq_report) */
void kvideo_report(void) {
  if (!known && kirq_is_on())
    con_log(CON_YELLOW, "video: a console not known (%x %x %x): the BIOS's SetGsCrt", seen[0],
            seen[1], seen[2]);
}

static int enc_write(u32 reg, u32 val) {
  if (known == BOARD_220) {
    for (int tries = 0; tries < 50; tries++) {
      kpoke8(E2_STAT, 8);
      kpoke8(E2_ADDR, ENC_DEV);
      kpoke8(E2_ADDR_HI, 0);
      kpoke8(E2_DATA, reg);
      kpoke8(E2_DATA, val);
      kpoke8(E2_GO, 0x11);
      for (int i = 0; kpeek8(E2_STAT) & 1; i++) {
        if (i > 200)
          return -1;
        mx_sleep_us(10);
      }
      if (!(kpeek8(E2_STAT) & 4))
        return 0;
    }
    return -1;
  }
  for (int tries = 0; tries < 50; tries++) {
    kpoke16(ENC_STAT, 3);
    kpoke16(ENC_ADDR, ENC_DEV);
    kpoke16(ENC_REG, reg);
    kpoke16(ENC_DATA, val);
    kpoke16(ENC_GO, 0x82);
    if (kpeek16(ENC_STAT) & 1)
      continue;
    for (int i = 0; !(kpeek16(ENC_STAT) & 2); i++) {
      if (i > 200)
        return -1;
      mx_sleep_us(10);
    }
    return 0;
  }
  return -1;
}

/* the encoder's registers for a kind of mode (as the BIOS writes them; for
   modes it does not have, those of the nearest one) */
static int encoder(const struct mx_crtc *c, int hd_clock) {
  static const u8 sdtv[] = {0x32, 1, 0x33, 2};
  static const u8 p480[] = {0x77, 0x11, 0x93, 1, 0x91, 2, 0x30, 0, 0x31, 7, 0x32, 1, 0x33, 0x14};
  /* 576p as ROM 2.20's BIOS writes it (ROM 2.00's has no such mode: there
     the same registers, not seen on a screen) */
  static const u8 p576[] = {0x77, 0x11, 0x93, 1, 0x91, 2,    0x30, 0,
                            0x31, 7,    0x32, 1, 0x33, 0x14, 0x38, 0x10};
  static const u8 vesa[] = {0x77, 0x11, 0x93, 1,    0x91, 1, 0x52, 0x27,
                            0x30, 0,    0x31, 7, 0x32, 1, 0x33, 0x3C};
  u8 hd[] = {0x77, 0x11, 0x93, 1, 0x90, 4,    0x91, 3, 0x52, 0x2B,
             0x30, 0,    0x31, 7, 0x32, 1, 0x33, 0x28, 0x34, 9};
  const u8 *t;
  u32 n;
  if (c->kind == MX_CRTC_HD && !hd_clock && c->vdp == 576)
    t = p576, n = sizeof p576;
  else if (c->kind == MX_CRTC_NTSC || c->kind == MX_CRTC_PAL)
    t = sdtv, n = sizeof sdtv;
  else if (c->kind == MX_CRTC_VESA)
    t = vesa, n = sizeof vesa;
  else if (!hd_clock)
    t = p480, n = sizeof p480;
  else {
    /* 720p: 0x90 = 6, 0x30 = 0x20; 1080i: 4, 0 */
    hd[5] = c->interlace ? 4 : 6;
    hd[11] = c->interlace ? 0 : 0x20;
    t = hd, n = sizeof hd;
  }
  if (known == BOARD_220) {
    kpoke8(E2_RESET, 0);
    mx_sleep_us(20);
    kpoke16(E2_RESET, 1);
  } else {
    kpoke16(ENC_RESET, 0);
    mx_sleep_us(20);
    kpoke16(ENC_RESET, 1);
  }
  int err = enc_write(0xB1, 0);
  for (u32 i = 0; i < n; i += 2)
    err |= enc_write(t[i], t[i + 1]);
  return err;
}

/* The BIOS's code of a mode it has (SetGsCrt's), found by its description
   (<mx/crtc.h>'s, ffmd aside); 0 if it has none */
static int bios_code(const struct mx_crtc *c) {
  static const u8 codes[] = {0x02, 0x03, 0x50, 0x51, 0x52, 0x53, 0x1A, 0x1B, 0x1C, 0x2A,
                             0x2B, 0x2C, 0x2D, 0x3B, 0x3C, 0x3D, 0x3E, 0x4A, 0x4B};
  for (u32 i = 0; i < sizeof codes; i++) {
    const struct mx_crtc *d = mx_crtc_for_code(codes[i], c->interlace);
    if (d && d->vck_hz == c->vck_hz && d->kind == c->kind && d->spml == c->spml &&
        d->interlace == c->interlace && d->hs == c->hs && d->hbp == c->hbp &&
        d->hact == c->hact && d->hfp == c->hfp && d->hseq == c->hseq && d->vs == c->vs &&
        d->vbpe == c->vbpe && d->vbp == c->vbp && d->vdp == c->vdp && d->vfpe == c->vfpe &&
        d->vfp == c->vfp)
      return codes[i];
  }
  return 0;
}

/* kernel version 23: the board's offsets of a mode's active area for a program
   (mx_video_board_offset), which makes no BIOS calls: the BIOS's
   GetGsDxDyOffset as asked at the kernel's start (dxdy); 0, 0 for a code it
   does not know, or on a ROM without the call */
void kvideo_board_offset(int code, int *dx, int *dy) {
  u32 i = (u32)(code - DXDY_FIRST);
  int ok = dxdy_known && i < DXDY_N;
  *dx = ok ? dxdy[i][0] : 0;
  *dy = ok ? dxdy[i][1] : 0;
}

int kvideo_crtc_set(const struct mx_crtc *c) {
  struct mx_crtc_regs r;
  if (!known) {
    int code = bios_code(c);
    if (!code)
      return -1;
    mx_bios_set_gs_crt(c->interlace, code, c->ffmd);
    return 0;
  }
  if (!kirq_is_on() || mx_crtc_regs(c, &r))
    return -1;
  u64 prst = MX_SET(GS_SMODE1_PRST, 1), sint = MX_SET(GS_SMODE1_SINT, 1);
  u64 s1 = r.smode1 & ~(prst | sint);
  int tv54 = MX_GET(GS_SMODE1_VCKSEL, r.smode1) != 0; /* NTSC, PAL, 480p */
  int hd_clock = c->kind == MX_CRTC_HD && !tv54;

  /* the clock: switched only when it changes (ROM 2.20's console has no
     switch: its BIOS writes none, and no GS reset) */
  u32 v = known == BOARD_200 ? kpeek16(CLOCK) : hd_clock ? CLOCK_HD : 0;
  if (!!(v & CLOCK_HD) != hd_clock) {
    kpoke16(CLOCK, hd_clock ? v | CLOCK_HD | CLOCK_ON : (v & ~CLOCK_HD) | CLOCK_ON);
    kpoke32(BIOS_HD_CLOCK, hd_clock); /* the BIOS's SetGsCrt knows it then */
    mx_sleep_us(25000);
    MX_R64(MX_GS_CSR) = MX_BIT(GS_CSR_RESET);
  }

  int err = 0;
  if (c->kind == MX_CRTC_VESA) {
    /* the BIOS: NTSC's clock first, the encoder, then the timing */
    struct mx_crtc_regs n;
    mx_crtc_regs(&mx_crtc_ntsc, &n);
    MX_R64(MX_GS_SMODE1) = n.smode1;
    err = encoder(c, 0);
  } else
    MX_R64(MX_GS_SMODE1) = s1 | prst | sint;
  MX_R64(MX_GS_SYNCH1) = r.synch1;
  MX_R64(MX_GS_SYNCH2) = r.synch2;
  MX_R64(MX_GS_SYNCV) = r.syncv;
  MX_R64(MX_GS_SMODE2) = r.smode2;
  MX_R64(MX_GS_SRFSH) = r.srfsh;
  if (tv54)
    MX_R64(MX_GS_SMODE1) = s1 | prst;
  else {
    MX_R64(MX_GS_SMODE1) = s1 | sint;
    mx_sleep_us(15000);
    MX_R64(MX_GS_SMODE1) = s1;
  }
  if (c->kind != MX_CRTC_VESA)
    err = encoder(c, hd_clock);
  if (err)
    con_log(CON_RED, "video: the encoder did not answer");
  return 0;
}
