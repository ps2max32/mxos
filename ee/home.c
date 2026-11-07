/* The home menu (mx-kernel 7, libs/maxsdk/docs/mx-os-plan.md step 1): held
   together, Select + Start + L1 + R1 open it over a libmxk program, which
   waits meanwhile; resume, the pad's settings, quit.

   - The display registers are the kernel's: maxsdk writes them through
     gs_display, which writes and keeps them (also from the program's
     interrupt handlers); the menu takes circuit 1 and gives it back.
   - The pad (pad.c) sees the combination and asks for the menu instead of
     handing it to the program. The program stops at its next frame boundary
     (maxsdk's ee/src/gs/home.c) with the GS idle and lends GS memory: then
     home_run runs here, in its thread. A program that does not come within
     2 s (hires mode, no frame pipeline, an older maxsdk) gets the
     combination, which ends it, as before.
   - The menu is drawn by the CPU, 8 lines at a time, and copied into the lent
     memory by IMAGE transfers through the GIF FIFO: only BITBLTBUF, TRXPOS,
     TRXREG and TRXDIR change, which every upload sets anew; no drawing
     register. It is a 320 x 224 CT32 picture over the whole screen on
     circuit 1, blended by its own alpha (MMOD 0) over the program's last
     picture on circuit 2: transparent but for the box. */

#include <mx/bits.h>
#include <mx/cpu.h>
#include <mx/display.h>
#include <mx/gif.h>
#include <mx/gs.h>
#include <mx/hw/regs.h>
#include <mx/irq.h>
#include <mx/kernel.h>
#include <mx/pad.h>
#include <mx/string.h>
#include <mx/thread.h>
#include <mx/timer.h>

#include "mxlink.h"

/* ---- the display ---------------------------------------------------------- */

static const u32 dreg[MX_DISP_NREGS] = {MX_GS_PMODE,   MX_GS_DISPFB1,  MX_GS_DISPLAY1,
                                        MX_GS_DISPFB2, MX_GS_DISPLAY2, MX_GS_BGCOLOR};
/* the program's display registers, as it last wrote them (mask: which) */
static u64 prog_regs[MX_DISP_NREGS];
static u32 prog_mask;
static volatile int menu_up;
static struct mx_video_mode mode;
static int mode_known;

void home_gs_display(const u64 *r, u32 mask) {
  int was = mx_di();
  for (int i = 0; i < MX_DISP_NREGS; i++)
    if (mask >> i & 1) {
      prog_regs[i] = r[i];
      if (!menu_up)
        MX_R64(dreg[i]) = r[i];
    }
  prog_mask |= mask;
  mx_irq_restore(was);
}

void home_gs_mode(const struct mx_video_mode *m) {
  mode = *m;
  mode_known = 1;
}

/* ---- the combination ------------------------------------------------------ */

#define PANEL_W 320
#define PANEL_H 224
#define PANEL_FBW (PANEL_W / 64)
/* CT32 pages are 64 x 32 pixels */
#define PANEL_PAGES (PANEL_FBW * ((PANEL_H + 31) / 32))
#define GIVE_UP (2 * MX_EE_HZ)

static int wanted;      /* the combination seen, the menu asked for */
static u32 wanted_at;   /* since */
static int quit_now;    /* the program gets the combination: it ends */
static int hold_back;   /* after the menu: no buttons until all are released */

u32 home_filter(u32 b) {
  if (hold_back) {
    if (b)
      return 0;
    hold_back = 0;
  }
  if (menu_up)
    return 0; /* the menu's, not the program's */
  if ((b & MX_PAD_QUIT) != MX_PAD_QUIT || quit_now)
    return b;
  /* only for a program whose maxsdk has the menu (it told the mode) */
  if (!mode_known)
    return b;
  if (!wanted) {
    wanted = 1;
    wanted_at = mx_count();
  } else if (mx_count() - wanted_at > GIVE_UP) {
    quit_now = 1; /* it did not come to a frame boundary */
    return b;
  }
  return b & ~MX_PAD_QUIT;
}

u32 home_wanted(void) { return wanted && !menu_up && !quit_now ? PANEL_PAGES : 0; }

/* ---- drawing -------------------------------------------------------------- */

#define STRIP 8
static u32 strip[PANEL_W * STRIP] MX_DMA_LINES;

/* the menu */
enum { I_RESUME, I_PORTS, I_XO, I_QUIT, NITEMS };
static u32 sel;

#define BOX_W 248
#define BOX_H (6 + (NITEMS + 1) * (LINE_H + 6) + 4)
#define BOX_X ((PANEL_W - BOX_W) / 2)
#define BOX_Y ((PANEL_H - BOX_H) / 2)
#define LINE_H 12
#define CHAR_W 6
/* ABGR, alpha 0x80 = 1.0 */
#define C_CLEAR 0x00000000u
#define C_BOX 0x70301810u
#define C_EDGE 0x80806040u
#define C_SEL 0x80704020u
#define C_TEXT 0x80E0E0E0u
#define C_DIM 0x80909090u
#define C_TITLE 0x8060E0E0u

static char lines[NITEMS + 1][44];

static void texts(void) {
  const char *p = status_program(), *name = p;
  for (const char *s = p; *s; s++)
    if (*s == '/' || *s == '\\' || *s == ':')
      name = s + 1;
  mx_snprintf(lines[0], sizeof lines[0], "%.38s", name);
  mx_snprintf(lines[1 + I_RESUME], sizeof lines[0], "Resume");
  mx_snprintf(lines[1 + I_PORTS], sizeof lines[0], "Pads 1 and 2 swapped: %s",
              pad_settings.swap_ports ? "yes" : "no");
  mx_snprintf(lines[1 + I_XO], sizeof lines[0], "Cross and Circle swapped: %s",
              pad_settings.swap_xo ? "yes" : "no");
  mx_snprintf(lines[1 + I_QUIT], sizeof lines[0], "Quit");
}

/* the colour of the menu's pixel (x, y): rows of LINE_H + 6 lines, the
   title and the items; a glyph's 5 x 8 pixels at (1, 3) of its cell */
#define ROW_H (LINE_H + 6)
static u32 pixel(u32 x, u32 y) {
  if (x < BOX_X || x >= BOX_X + BOX_W || y < BOX_Y || y >= BOX_Y + BOX_H)
    return C_CLEAR;
  u32 bx = x - BOX_X, by = y - BOX_Y;
  if (bx < 2 || bx >= BOX_W - 2 || by < 2 || by >= BOX_H - 2)
    return C_EDGE;
  if (by < 6)
    return C_BOX;
  u32 row = (by - 6) / ROW_H, ty = (by - 6) % ROW_H;
  if (row > NITEMS)
    return C_BOX;
  int item = row > 0, chosen = item && row - 1 == sel;
  u32 bg = chosen && bx >= 6 && bx < BOX_W - 6 ? C_SEL : C_BOX;
  if (bx < 10 || ty < 3)
    return bg;
  u32 col = (bx - 10) / CHAR_W, gx = (bx - 10) % CHAR_W, gy = ty - 3;
  const char *s = lines[row];
  if (col >= strlen(s) || gx >= 5 || gy >= 8)
    return bg;
  char c = s[col];
  if (c <= ' ' || c > '~' || !(con_font[c - ' '][gx] >> gy & 1))
    return bg;
  return !item ? C_TITLE : chosen ? C_TEXT : C_DIM;
}

/* room in the GIF FIFO (16 quadwords), or 0 after about 30 ms */
static int fifo_room(void) {
  for (u32 i = 0; i < 1000000; i++)
    if ((MX_R32(MX_GIF_STAT) >> 24 & 31) < 16)
      return 1;
  return 0;
}

static int put(u64 lo, u64 hi) {
  if (!fifo_room())
    return 0;
  mx_w128(MX_GIF_FIFO, mx_qw(lo, hi));
  return 1;
}

/* PATH3 free for the CPU: the GIF channel stopped, PATH3 not masked */
static int path3_free(void) {
  return !(MX_R32(MX_DMA_GIF) & MX_CHCR_STR) && !(MX_R32(MX_GIF_STAT) & 3);
}

/* lines y .. y + STRIP - 1 of the menu into GS memory at page */
static int upload(u32 page, u32 y) {
  for (u32 j = 0; j < STRIP; j++)
    for (u32 x = 0; x < PANEL_W; x++)
      strip[j * PANEL_W + x] = pixel(x, y + j);
  int was = mx_di(), ok = path3_free();
  ok = ok && put(mx_giftag(4, 0, -1, MX_GIF_FLG_PACKED, 1), MX_GIF_REG_AD) &&
       put(mx_gs_bitbltbuf(0, 0, 0, MX_GS_PAGE(page), PANEL_FBW, MX_GS_PSM_CT32),
           MX_GS_REG_BITBLTBUF) &&
       put(mx_gs_trxpos(0, 0, 0, y, 0), MX_GS_REG_TRXPOS) &&
       put(mx_gs_trxreg(PANEL_W, STRIP), MX_GS_REG_TRXREG) && put(0, MX_GS_REG_TRXDIR) &&
       put(mx_giftag(PANEL_W * STRIP / 4, 1, -1, MX_GIF_FLG_IMAGE, 0), 0);
  for (u32 i = 0; ok && i < PANEL_W * STRIP; i += 4)
    ok = put((u64)strip[i + 1] << 32 | strip[i], (u64)strip[i + 3] << 32 | strip[i + 2]);
  mx_irq_restore(was);
  return ok;
}

static int draw(u32 page) {
  texts();
  for (u32 y = 0; y < PANEL_H; y += STRIP)
    if (!upload(page, y))
      return -1;
  return 0;
}

/* ---- the menu ------------------------------------------------------------- */

/* maxsdk's irq.c: one more function at each vblank start */
int mx__irq_vbon_hook(void (*fn)(void));
static int vbl_sema = -1;
static void vblank(void) {
  if (menu_up && vbl_sema >= 0)
    mx_isema_signal(vbl_sema);
}

/* circuit 1: the menu over the whole screen, by its own alpha */
static void show(u32 page) {
  struct mx_display d = {0};
  d.c[0] = (struct mx_circuit){.on = 1, .fbp = page, .fbw = PANEL_FBW, .psm = MX_GS_PSM_CT32,
                               .w = PANEL_W, .h = PANEL_H};
  u64 r[MX_DISP_NREGS];
  mx_display_regs(&mode, &d, r);
  u64 pmode = prog_mask & 1 ? prog_regs[MX_DISP_PMODE] : MX_SET(GS_PMODE_CRTMD, 1);
  pmode &= ~(MX_SET(GS_PMODE_EN1, 1) | MX_SET(GS_PMODE_MMOD, 1) | MX_SET(GS_PMODE_AMOD, 1) |
             MX_SET(GS_PMODE_SLBG, 1) | MX_SET(GS_PMODE_ALP, 0xFF));
  pmode |= MX_SET(GS_PMODE_EN1, 1);
  int was = mx_di();
  MX_R64(MX_GS_DISPFB1) = r[MX_DISP_DISPFB1];
  MX_R64(MX_GS_DISPLAY1) = r[MX_DISP_DISPLAY1];
  MX_R64(MX_GS_PMODE) = pmode;
  mx_irq_restore(was);
}

/* the program's registers again */
static void hide(void) {
  int was = mx_di();
  for (int i = MX_DISP_NREGS - 1; i >= 0; i--)
    if (prog_mask >> i & 1)
      MX_R64(dreg[i]) = prog_regs[i];
  if (!(prog_mask & 1))
    MX_R64(MX_GS_PMODE) = MX_SET(GS_PMODE_CRTMD, 1) | MX_SET(GS_PMODE_EN2, 1);
  mx_irq_restore(was);
}

/* the pad as the menu reads it: both ports, as they are */
static u32 menu_buttons(void) {
  struct mx_pad_state s;
  u32 b = 0;
  for (int p = 0; p < 2; p++)
    if (mx__pad_read(p, &s, 0))
      b |= s.buttons;
  return b;
}

int home_run(u32 page, u32 pages) {
  wanted = 0;
  if (page == ~0u || pages < PANEL_PAGES || !mode_known) {
    quit_now = 1; /* no menu: the combination ends it, as before */
    return 0;
  }
  if (vbl_sema < 0) {
    int sema = mx_sema_create(0, 1);
    if (sema < 0 || mx__irq_vbon_hook(vblank) < 0) {
      quit_now = 1; /* no vblanks for the menu: the combination ends it */
      return 0;
    }
    vbl_sema = sema;
  }
  sel = I_RESUME;
  if (draw(page) < 0) {
    quit_now = 1;
    return 0;
  }
  menu_up = 1;
  show(page);
  /* the combination is still held: wait until it is let go */
  u32 prev = MX_PAD_QUIT, end = 0, quit = 0;
  while (!end) {
    mx_sema_wait(vbl_sema);
    u32 b = menu_buttons(), p = b & ~prev;
    prev = b;
    int redraw = 0;
    if (p & MX_PAD_UP)
      sel = (sel + NITEMS - 1) % NITEMS, redraw = 1;
    if (p & MX_PAD_DOWN)
      sel = (sel + 1) % NITEMS, redraw = 1;
    if (p & (MX_PAD_START | MX_PAD_TRIANGLE))
      end = 1;
    if (p & MX_PAD_CROSS) {
      if (sel == I_RESUME)
        end = 1;
      else if (sel == I_QUIT)
        end = quit = 1;
      else if (sel == I_PORTS)
        pad_settings.swap_ports ^= 1, redraw = 1;
      else if (sel == I_XO)
        pad_settings.swap_xo ^= 1, redraw = 1;
    }
    if (redraw && !end && draw(page) < 0)
      end = 1;
  }
  menu_up = 0;
  hide();
  hold_back = 1;
  return (int)quit;
}
