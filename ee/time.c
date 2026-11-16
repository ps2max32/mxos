/* The time (kernel version 29, <mx/time.h>): the PS2's clock (in the CD/DVD
   controller, battery backed) read at each start through mxio, Japan's time
   in BCD as every PS2 keeps it, and from then counted with the kernel's
   seconds (dev.c's kseconds). The shell sets it: written to the clock too. */

#include <mx/string.h>

#include "mxlink.h"

#define JST (9 * 3600)

static s64 at_zero; /* UTC at kseconds() 0; known when set */
static int known;

static u32 bcd(u8 v) { return (u32)(v >> 4) * 10 + (v & 15); }
static u8 to_bcd(u32 v) { return (u8)((v / 10) << 4 | v % 10); }

/* days since 1970-01-01 of a date (H. Hinnant's days_from_civil) */
static s64 days_of(s32 y, u32 m, u32 d) {
  y -= m <= 2;
  s32 era = (y >= 0 ? y : y - 399) / 400;
  u32 yoe = (u32)(y - era * 400);
  u32 doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  u32 doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return (s64)era * 146097 + (s64)doe - 719468;
}
static void civil_of(s64 z, s32 *y, u32 *m, u32 *d) {
  z += 719468;
  s32 era = (s32)((z >= 0 ? z : z - 146096) / 146097);
  u32 doe = (u32)(z - (s64)era * 146097);
  u32 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  u32 doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  u32 mp = (5 * doy + 2) / 153;
  *d = doy - (153 * mp + 2) / 5 + 1;
  *m = mp < 10 ? mp + 3 : mp - 9;
  *y = (s32)yoe + era * 400 + (*m <= 2);
}

/* at each start (main.c, after mxio's devices) */
void ktime_boot(void) {
  u8 c[8] = {0};
  known = 0;
  int r = dev_clock(0, c);
  if (r != 1 || c[0]) {
    con_log(CON_YELLOW, "time: the clock could not be read (%d: %02x %02x %02x %02x %02x %02x %02x %02x)", r,
            c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7]);
    return;
  }
  s32 y = 2000 + (s32)bcd(c[7]);
  u32 mo = bcd(c[6] & 0x1F), d = bcd(c[5]);
  s64 jst = days_of(y, mo, d) * 86400 + bcd(c[3]) * 3600 + bcd(c[2]) * 60 + bcd(c[1]);
  at_zero = jst - JST - (s64)kseconds();
  known = 1;
  s64 utc = jst - JST;
  s32 uy;
  u32 um, ud, us = (u32)(utc % 86400);
  civil_of(utc / 86400, &uy, &um, &ud);
  con_log(CON_GREY, "time: %04d-%02u-%02u %02u:%02u:%02u UTC", (int)uy, um, ud, us / 3600, us / 60 % 60, us % 60);
}

/* <mx/time.h>'s mx_time: seconds since 1970 (UTC), 0 when not known */
s64 ktime_now(void) { return known ? at_zero + (s64)kseconds() : 0; }

/* <mx/time.h>'s mx_time_set: the shell only; the clock written too */
int ktime_set(s64 utc) {
  if (!kproc_shell() || utc < 946684800 || utc >= 4102444800LL) /* 2000 .. 2099 */
    return -1;
  s64 t = utc + JST;
  s32 y;
  u32 m, d;
  civil_of(t / 86400, &y, &m, &d);
  u32 sec = (u32)(t % 86400);
  u8 c[8] = {0, to_bcd(sec % 60), to_bcd(sec / 60 % 60), to_bcd(sec / 3600), 0,
             to_bcd(d), to_bcd(m), to_bcd((u32)(y - 2000))};
  if (dev_clock(1, c) != 1)
    return -1;
  at_zero = utc - (s64)kseconds();
  known = 1;
  return 0;
}
