/* maxsdk's event trace (<mx/trace.h>) is never turned on in the kernel: its
   variables here, so that src/irq/trace.c and its 32 KiB ring stay out of
   the kernel's data area (the GS, DMA and frame code it links name them).
   mx__trace_on stays 0, so the ring is never written. */
#include <mx/types.h>

/* <mx/trace.h> declares the ring with its full size; not included here */
int mx__trace_on;
u32 mx__trace_pos;
u32 mx__trace_buf[2];
