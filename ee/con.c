/* The console: text on a 640 x 448 screen (NTSC) or 640 x 512 (PAL, by
   the ROM's region), interlaced, drawn as the debug screen of maxsdk draws
   (src/base/dbgscreen.c): GS registers and primitives written to the GIF
   FIFO by the CPU, no DMA.

   The 5 x 8 font (font.c) is a texture, put in GS memory at the start: a
   glyph in a cell of 8 x 10 texels, its pixels white and opaque, the rest
   white and transparent. A character is one sprite, the cell stretched
   1.25 times in width (a glyph pixel 1.25 pixels wide, a character 7.5:
   84 columns) and 2 times in height (a glyph pixel 2 lines, one in each
   field: no flicker; rows of 18 lines, 24 rows on NTSC, 28 on PAL),
   filtered bilinearly and blended over what is there: soft edges.

   Tried before: 640 x 448 interlaced with one-pixel glyphs (unreadable on
   the TV), 640 x 224 with glyph pixels 2 wide (53 columns, too few), 512
   wide stretched by the display (84 columns, hard edges), 640 x 224 with
   this texture (soft edges in width only). */

#include <mx/cpu.h>
#include <mx/debug.h>
#include <mx/gif.h>
#include <mx/gs.h>
#include <mx/irq.h>
#include <mx/string.h>
#include <mx/video.h>
#include <stdarg.h>

#include "mxlink.h"

#define W 640
#define CW16 120 /* a column: 7.5 pixels, in 1/16 */
#define CH 18
#define X0 2
#define Y0 8

static u32 H;  /* 448 or 512 */
u32 con_rows; /* 24 or 28 */

/* the font texture: 16 cells of 8 x 10 texels a row, 6 rows */
#define TW 128
#define TH 64
#define CELL_W 8
#define CELL_H 10

static int on;
static u32 font_tbp;

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

/* A+D: n registers */
static int ad(const u32 *regs, const u64 *val, u32 n) {
  if (!put(mx_giftag(n, 1, -1, MX_GIF_FLG_PACKED, 1), MX_GIF_REG_AD))
    return 0;
  for (u32 i = 0; i < n; i++)
    if (!put(val[i], regs[i]))
      return 0;
  return 1;
}

/* the font texel at (x, y): white, opaque on a glyph pixel */
static u32 texel(u32 x, u32 y) {
  u32 g = (y / CELL_H) * (TW / CELL_W) + x / CELL_W;
  int gx = (int)(x % CELL_W) - 1, gy = (int)(y % CELL_H) - 1;
  int on_px = g < 95 && gx >= 0 && gx < 5 && gy >= 0 && gy < 8 && (con_font[g][gx] >> gy & 1);
  return on_px ? 0x80808080u : 0x00808080u;
}

/* the font into GS memory: a host to local transfer through the FIFO */
static void font_upload(void) {
  static const u32 regs[] = {MX_GS_REG_BITBLTBUF, MX_GS_REG_TRXPOS, MX_GS_REG_TRXREG,
                             MX_GS_REG_TRXDIR};
  const u64 val[] = {mx_gs_bitbltbuf(0, 0, 0, font_tbp, TW / 64, MX_GS_PSM_CT32),
                     mx_gs_trxpos(0, 0, 0, 0, 0), mx_gs_trxreg(TW, TH), 0};
  if (!ad(regs, val, 4))
    return;
  if (!put(mx_giftag(TW * TH / 4, 1, -1, MX_GIF_FLG_IMAGE, 0), 0))
    return;
  for (u32 y = 0; y < TH; y++)
    for (u32 x = 0; x < TW; x += 4)
      if (!put((u64)texel(x + 1, y) << 32 | texel(x, y),
               (u64)texel(x + 3, y) << 32 | texel(x + 2, y)))
        return;
}

/* context 1 drawing into the shown buffer in colour c: flat, or the font
   (textured, filtered, blended) */
static int setup(u32 c, int text) {
  static const u32 regs[] = {MX_GS_REG_FRAME_1,    MX_GS_REG_ZBUF_1,    MX_GS_REG_TEST_1,
                             MX_GS_REG_XYOFFSET_1, MX_GS_REG_SCISSOR_1, MX_GS_REG_ALPHA_1,
                             MX_GS_REG_FBA_1,      MX_GS_REG_PRMODECONT, MX_GS_REG_DTHE,
                             MX_GS_REG_COLCLAMP,   MX_GS_REG_TEXFLUSH,  MX_GS_REG_TEX0_1,
                             MX_GS_REG_TEX1_1,     MX_GS_REG_CLAMP_1,   MX_GS_REG_RGBAQ,
                             MX_GS_REG_PRIM};
  const u64 val[] = {mx_gs_frame(mx_gs.fbp[0], mx_gs.fbw, MX_GS_PSM_CT32, 0),
                     mx_gs_zbuf(0, 0, 1),
                     mx_gs_test(0, 0, 0, 0, 0, 0, 1, 1),
                     0,
                     mx_gs_scissor(0, W - 1, 0, H - 1),
                     /* (Cs - Cd) * As + Cd */
                     mx_gs_alpha(0, 1, 0, 1, 0),
                     0,
                     1,
                     0,
                     1,
                     0,
                     /* RGBA, modulated by the colour */
                     mx_gs_tex0(font_tbp, TW / 64, MX_GS_PSM_CT32, 7, 6, 1, 0),
                     /* bilinear */
                     mx_gs_tex1(0, 0, 1, 1, 0, 0, 0),
                     mx_gs_clamp(1, 1, 0, 0, 0, 0),
                     mx_gs_rgbaq(c & 0xFF, c >> 8 & 0xFF, c >> 16 & 0xFF, 0x80, 1.0f),
                     /* REGLIST ignores the GIF tag's PRIM */
                     mx_gs_prim(MX_GS_PRIM_SPRITE, 0, text, 0, text, 0, text, 0, 0)};
  return ad(regs, val, sizeof regs / sizeof regs[0]);
}

/* PATH3 free for the CPU: the GIF channel stopped and PATH3 not masked */
static int path3_free(void) {
  for (u32 i = 0; MX_R32(MX_DMA_GIF) & MX_CHCR_STR; i++)
    if (i > 1000000u)
      return 0;
  return !(MX_R32(MX_GIF_STAT) & 3);
}

static u64 xy16(u32 x, u32 y) { return mx_gs_xyz(x, y, 0); }

static void box(u32 x, u32 y, u32 w, u32 h, u32 rgb) {
  if (!setup(rgb, 0) || !put(mx_giftag(1, 1, -1, MX_GIF_FLG_REGLIST, 2),
                              mx_gif_regs2(MX_GS_REG_XYZ2, MX_GS_REG_XYZ2)))
    return;
  put(xy16(x << 4, y << 4), xy16((x + w) << 4, (y + h) << 4));
}

void con_text(u32 col, u32 row, u32 rgb, const char *s) {
  if (!on || row >= con_rows)
    return;
  u32 n = 0;
  for (u32 c = col; s[c - col] && c < CON_COLS; c++)
    if (s[c - col] > ' ' && s[c - col] <= '~')
      n++;
  if (!n)
    return;
  int was = mx_di();
  if (path3_free() && setup(rgb, 1) &&
      put(mx_giftag(n, 1, -1, MX_GIF_FLG_REGLIST, 4),
          mx_gif_regs4(MX_GS_REG_UV, MX_GS_REG_XYZ2, MX_GS_REG_UV, MX_GS_REG_XYZ2))) {
    /* the cell's texels (1, 1) on at the character's place: the sprite
       starts a texel (1.25 pixels, 2 lines) before it */
    u32 y = (Y0 + row * CH - 2) << 4;
    for (; *s && col < CON_COLS; s++, col++) {
      if (*s <= ' ' || *s > '~')
        continue;
      u32 g = (u32)(*s - ' ');
      u32 u = (g % (TW / CELL_W)) * CELL_W << 4, v = (g / (TW / CELL_W)) * CELL_H << 4;
      u32 x = (X0 << 4) + col * CW16 - 20;
      /* REGLIST: two registers a quadword; the cell, 8 x 10 texels, on
         10 x 20 pixels */
      if (!put(mx_gs_uv(u, v), xy16(x, y)) ||
          !put(mx_gs_uv(u + (CELL_W << 4), v + (CELL_H << 4)), xy16(x + 160, y + 320)))
        break;
    }
  }
  mx_irq_restore(was);
}

static void clear_row(u32 row) {
  int was = mx_di();
  if (path3_free())
    box(0, Y0 + row * CH, W, CH, 0);
  mx_irq_restore(was);
}

void con_line(u32 row, u32 rgb, const char *fmt, ...) {
  char buf[CON_COLS + 1];
  va_list ap;
  va_start(ap, fmt);
  mx_vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  if (!on)
    return;
  clear_row(row);
  con_text(0, row, rgb, buf);
}

/* the log: rows LOG0 .. con_rows - 1, kept as text to redraw on a scroll */
#define LOG0 12
#define NLOG (CON_ROWS - LOG0) /* room for PAL's */
static char log_text[NLOG][CON_COLS + 1];
static u32 log_rgb[NLOG];
static u32 log_n;

void con_log(u32 rgb, const char *fmt, ...) {
  char buf[160];
  va_list ap;
  va_start(ap, fmt);
  mx_vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  /* the debug output goes through the IOP's file server, which a running
     program has taken over (its SIF receive buffer): only without one */
  if (!exec_running())
    mx_printf("mxlink: %s\n", buf);
  u32 nlog = con_rows - LOG0;
  if (log_n == nlog) {
    for (u32 i = 1; i < nlog; i++) {
      memcpy(log_text[i - 1], log_text[i], sizeof log_text[0]);
      log_rgb[i - 1] = log_rgb[i];
    }
    log_n--;
  }
  memcpy(log_text[log_n], buf, CON_COLS);
  log_text[log_n][CON_COLS] = 0;
  log_rgb[log_n++] = rgb;
  if (!on || exec_running())
    return;
  for (u32 i = 0; i < log_n; i++) {
    clear_row(LOG0 + i);
    con_text(0, LOG0 + i, log_rgb[i], log_text[i]);
  }
}

void con_init(void) {
  int pal = mx_video_default() == &mx_video_pal;
  H = pal ? 512 : 448;
  con_rows = (H - 2 * Y0) / CH;
  mx_gs_init_mode(W, H, pal ? MX_GS_PAL : 0);
  font_tbp = MX_GS_PAGE(mx_vram_alloc(4)); /* 128 x 64, 32-bit: 4 pages */
  int was = mx_di();
  if (path3_free()) {
    font_upload();
    box(0, 0, W, H, 0);
  }
  mx_irq_restore(was);
  on = 1;
  /* a separator above the log */
  was = mx_di();
  if (path3_free())
    box(0, Y0 + LOG0 * CH - 6, W, 2, 0x404040);
  mx_irq_restore(was);
  /* the log so far (a program's end without the restart keeps it) */
  for (u32 i = 0; i < log_n; i++)
    con_text(0, LOG0 + i, log_rgb[i], log_text[i]);
}
