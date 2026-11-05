/* mxlink's boot sound, and the end of a program's sound: the IOP's reset
   leaves the SPU2 as it was, so a program's voices and sound data input go on
   (a buzz). This resets the SPU2 as rom0:CLEARSPU does, without its DMA and
   interrupt handlers, and plays a blip on core 1. The SPU2 plays it alone and
   it ends by the last block's flag, so the module does not stay.

   The start result has NO_RESIDENT_END in its low 2 bits and, above them,
   what it saw (ee/main.c logs it): bits 2-7 the voices with an envelope
   before the reset, 8-9 the cores' auto DMA, 12-27 the blip's envelope
   50 ms in.

   Every wait has a limit: this runs in mxlink's boot. */

#include "irx_imports.h"
#include "chime_blip.h"

#define MODNAME "mxchime"
IRX_ID(MODNAME, 1, 0);

#define R16(a) (*(volatile u16 *)(0xBF900000 + (a)))
#define R32(a) (*(volatile u32 *)(0xBF800000 + (a)))
#define CORE(c) ((c) << 10)
/* registers of a core */
#define VOLL(c, v) R16(CORE(c) + ((v) << 4) + 0x0)
#define VOLR(c, v) R16(CORE(c) + ((v) << 4) + 0x2)
#define PITCH(c, v) R16(CORE(c) + ((v) << 4) + 0x4)
#define ADSR1(c, v) R16(CORE(c) + ((v) << 4) + 0x6)
#define ADSR2(c, v) R16(CORE(c) + ((v) << 4) + 0x8)
#define ENVX(c, v) R16(CORE(c) + ((v) << 4) + 0xA)
#define SSA_HI(c, v) R16(CORE(c) + 0x1C0 + (v) * 12)
#define SSA_LO(c, v) R16(CORE(c) + 0x1C2 + (v) * 12)
#define VMIX(c, r) R16(CORE(c) + 0x188 + (r))
#define MMIX(c) R16(CORE(c) + 0x198)
#define ATTR(c) R16(CORE(c) + 0x19A)
#define KON(c) R16(CORE(c) + 0x1A0)
#define KOFF_HI(c) R16(CORE(c) + 0x1A4)
#define KOFF_LO(c) R16(CORE(c) + 0x1A6)
#define TSA_HI(c) R16(CORE(c) + 0x1A8)
#define TSA_LO(c) R16(CORE(c) + 0x1AA)
#define STD(c) R16(CORE(c) + 0x1AC)
#define ADMA(c) R16(CORE(c) + 0x1B0)
#define STATX(c) R16(CORE(c) + 0x344)
/* the volumes, 40 bytes a core: master, effect, external input, sound data
   input, each left and right */
#define VOL(c, r) R16(0x760 + (c) * 40 + (r))
#define MVOL 0
#define EVOL 4
#define AVOL 8
#define BVOL 12

#define ATTR_ON 0x8000
#define ATTR_DMA 0x0030
#define ATTR_IO 0x0010
#define STATX_IO 0x0400

#define VOICES 24
#define SPU_DATA 0x5010 /* SPU RAM, bytes: after libsd's silent block */
#define VOLUME 0x3FFF   /* the voice's and the master's, as audsrv */
/* the envelope of audsrv's voices: libsd's 0, 0 */
#define BLIP_ADSR1 0
#define BLIP_ADSR2 0
#define BLIP_PITCH 0x0800 /* 24 kHz: PITCH / 4096 * 48 kHz */
#define BLIP_CHECK 50000  /* microseconds */

static void wait(void) {
  for (volatile int i = 0; i < 2000; i++)
    ;
}

static void wait_io(int core) {
  for (int i = 0; i < 0x10000 && (STATX(core) & STATX_IO); i++)
    ;
}

static void ssbus(void) {
  /* the SSBUS, as CLEARSPU: the SPU2's addresses and delays */
  R32(0x1404) = 0xBF900000;
  R32(0x140C) = 0xBF900800;
  R32(0x10F0) |= 0xB0000;
  R32(0x1570) |= 8;
  R32(0x1014) = 0x200B31E1;
  R32(0x1414) = 0x200B31E1;
}

static u32 seen(void) {
  u32 voices = 0, adma = 0;
  for (int c = 0; c < 2; c++) {
    for (int v = 0; v < VOICES; v++)
      voices += ENVX(c, v) != 0;
    adma |= (ADMA(c) & 3) ? 1u << c : 0;
  }
  return voices << 2 | adma << 8;
}

static void silence(void) {
  for (int c = 0; c < 2; c++) {
    VOL(c, MVOL) = VOL(c, MVOL + 2) = 0;
    ATTR(c) &= ~ATTR_DMA;
    ADMA(c) = 0;
    ATTR(c) = 0;
    wait();
    ATTR(c) = ATTR_ON;
    wait();
    for (int i = 0; i < 0xF00 && (STATX(c) & 0x7FF); i++)
      ;
    KOFF_HI(c) = 0xFFFF;
    KOFF_LO(c) = 0xFF;
    for (int v = 0; v < VOICES; v++)
      VOLL(c, v) = VOLR(c, v) = 0;
  }
}

static void setup(void) {
  /* the digital output, as CLEARSPU */
  R16(0x7C6) = 0x900;
  R16(0x7C8) = 0x200;
  R16(0x7CA) = 8;
  R16(0x7C0) = 0xC032;
  /* as libsd and CLEARSPU. Bit 14 (libsd's SD_MUTE) has to be set to hear
     anything: without it the voices run, ENVX and VOLX move, and it is silent */
  ATTR(0) = 0xC000;
  ATTR(1) = 0xC001;
  for (int c = 0; c < 2; c++) {
    for (int r = 0; r < 16; r += 4) { /* every voice to every channel */
      VMIX(c, r) = 0xFFFF;
      VMIX(c, r + 2) = 0xFF;
    }
    VOL(c, EVOL) = VOL(c, EVOL + 2) = 0;
    VOL(c, AVOL) = VOL(c, AVOL + 2) = 0;
    VOL(c, BVOL) = VOL(c, BVOL + 2) = 0; /* the sound data input: off */
  }
  MMIX(0) = 0xFF0;
  MMIX(1) = 0xFFC;
}

/* the blip to SPU RAM: by the data port, 64 bytes at a time, each with its
   own address (with one address for all, the voice ended within 50 ms) */
static void upload(void) {
  for (u32 i = 0; i < sizeof blip / 2; i += 32) {
    u32 at = SPU_DATA + i * 2;
    TSA_HI(0) = at >> 17;
    TSA_LO(0) = (at >> 1) & 0xFFFF;
    for (u32 j = 0; j < 32; j++)
      STD(0) = blip[i + j];
    ATTR(0) = (ATTR(0) & ~ATTR_DMA) | ATTR_IO;
    wait_io(0);
    ATTR(0) &= ~ATTR_DMA;
  }
}

static void play(void) {
  VOLL(1, 0) = VOLR(1, 0) = VOLUME;
  PITCH(1, 0) = BLIP_PITCH;
  ADSR1(1, 0) = BLIP_ADSR1;
  ADSR2(1, 0) = BLIP_ADSR2;
  SSA_HI(1, 0) = SPU_DATA >> 17;
  SSA_LO(1, 0) = (SPU_DATA >> 1) & 0xFFFF;
  wait();
  KON(1) = 1;
}

/* With an argument, for a program that ended with the IOP kept: only its
   voices stop. audsrv goes on using the SPU2's DMA. */
static void voices_off(void) {
  for (int c = 0; c < 2; c++) {
    KOFF_HI(c) = 0xFFFF;
    KOFF_LO(c) = 0xFF;
    for (int v = 0; v < VOICES; v++)
      VOLL(c, v) = VOLR(c, v) = 0;
  }
}

int _start(int argc, char **argv) {
  (void)argv;
  ssbus();
  u32 report = seen();
  if (argc > 1) {
    voices_off();
    return (int)report | MODULE_NO_RESIDENT_END;
  }
  silence();
  setup();
  upload();
  VOL(1, MVOL) = VOL(1, MVOL + 2) = VOLUME;
  play();
  DelayThread(BLIP_CHECK);
  report |= (u32)ENVX(1, 0) << 12;
  return (int)report | MODULE_NO_RESIDENT_END;
}
