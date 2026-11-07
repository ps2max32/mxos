/* The pad is the kernel's (<mx/kernel.h>, version 6): libmxk's mx_pad_*
   read it here, with the pad's settings applied. maxsdk's own reading
   (mx__pad_init, mx__pad_read) with the area in mxlink's memory: the ports
   are opened when a program first asks and closed when it ends (main.c's
   cleanup), so a program with pad code of its own finds them free. */

#include <mx/kernel.h>
#include <mx/pad.h>

#include "mxlink.h"

/* kept over the restarts: the settings last until a reset */
struct pad_settings pad_settings __attribute__((section(".kept"))) = {0};

static u32 swap_bits(u32 b, u32 x, u32 y) {
  u32 bx = b & x, by = b & y;
  return (b & ~(x | y)) | (bx ? y : 0) | (by ? x : 0);
}

int pad_read(int port, struct mx_pad_state *s, int sticks) {
  if (pad_settings.swap_ports && (unsigned)port <= 1)
    port ^= 1;
  int r = mx__pad_read(port, s, sticks);
  if (pad_settings.swap_xo)
    s->buttons = swap_bits(s->buttons, MX_PAD_CROSS, MX_PAD_CIRCLE);
  return r;
}
