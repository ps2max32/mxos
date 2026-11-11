/* The CRTC timing without the BIOS (kernel version 22, plan step 2g): maxsdk
   computes the registers from a description (<mx/crtc.h>), this writes them the
   way the BIOS's SetGsCrt does, with what else that does on the console:

   - the HD clock switch, 0xBF80146C bit 3 (with bit 0), for the 74.25 MHz modes
     (720p, 1080i): set or cleared, about 20 ms for the clock, then a GS reset;
   - the video encoder on an I2C bus at 0xBA000000 (address 0x42): reset, then
     a few registers a kind of mode.

   Both were read from the BIOS's code of one console. The BIOS decides on its
   own variables, set at its boot: the kernel only takes this path where they
   are the ones of that console, else mx_video_set uses SetGsCrt as before. */

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

static int known;

void kvideo_init(void) {
  if (!kirq_is_on())
    return;
  u32 variant = kpeek32(BIOS_GS_VARIANT), board = kpeek32(BIOS_BOARD);
  u32 enc = kpeek32(BIOS_ENCODER) & 0xFF;
  known = variant == 0 && board == 0x32 && enc == 0x1C;
  mx__crtc_set = kvideo_crtc_set;
  if (!known)
    con_log(CON_YELLOW, "video: a console not known (%x %x %x): the BIOS's SetGsCrt", variant,
            board, enc);
}

static int enc_write(u32 reg, u32 val) {
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
  static const u8 vesa[] = {0x77, 0x11, 0x93, 1,    0x91, 1, 0x52, 0x27,
                            0x30, 0,    0x31, 7, 0x32, 1, 0x33, 0x3C};
  u8 hd[] = {0x77, 0x11, 0x93, 1, 0x90, 4,    0x91, 3, 0x52, 0x2B,
             0x30, 0,    0x31, 7, 0x32, 1, 0x33, 0x28, 0x34, 9};
  const u8 *t;
  u32 n;
  if (c->kind == MX_CRTC_NTSC || c->kind == MX_CRTC_PAL)
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
  kpoke16(ENC_RESET, 0);
  mx_sleep_us(20);
  kpoke16(ENC_RESET, 1);
  int err = enc_write(0xB1, 0);
  for (u32 i = 0; i < n; i += 2)
    err |= enc_write(t[i], t[i + 1]);
  return err;
}

int kvideo_crtc_set(const struct mx_crtc *c) {
  struct mx_crtc_regs r;
  if (!known || !kirq_is_on() || mx_crtc_regs(c, &r))
    return -1;
  u64 prst = MX_SET(GS_SMODE1_PRST, 1), sint = MX_SET(GS_SMODE1_SINT, 1);
  u64 s1 = r.smode1 & ~(prst | sint);
  int tv54 = MX_GET(GS_SMODE1_VCKSEL, r.smode1) != 0; /* NTSC, PAL, 480p */
  int hd_clock = c->kind == MX_CRTC_HD && !tv54;

  /* the clock: switched only when it changes */
  u32 v = kpeek16(CLOCK);
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
