/* Linker script of the kernel (lld): the last MiB of main memory
   (<mx/cpu.h>: MX_KERNEL_LO), virtual = physical. Its code and read-only
   data from __mx_text, its data from __mx_data to __mx_top, each in a segment
   of its own and under a TLB entry of its own (ee/tlb.c): the code's without
   the right to write, so a wild store of a program into it is an exception.
   What a loader writes (mxboot, which runs in the last 256 KiB) ends below
   __mx_top - 256 KiB: the code, .data and .kept. .bss (the kernel clears it
   itself), its heap, the main stack and the VM's tables (__mx_tables, at the
   top: ee/exec.c's kvm_boot) are in no segment. __self_start and __self_end:
   what the kernel takes (ee/exec.c). */

OUTPUT_ARCH(mips)
ENTRY(mxlink_start)

MEMORY {
  text : ORIGIN = __mx_text, LENGTH = __mx_data - __mx_text
  ram : ORIGIN = __mx_data, LENGTH = __mx_top - __mx_data
  spr : ORIGIN = 0x70000000, LENGTH = 16K
}

PHDRS {
  text PT_LOAD;
  data PT_LOAD;
}

SECTIONS {
  __self_start = __mx_text;
  __self_end = __mx_top;

  .text ORIGIN(text) : {
    __text_start = .;
    KEEP(*(.text.start))
    *(.text .text.*)
    __text_end = .;
  } >text :text

  /* VU microcode, kept 16-byte aligned so it can be sent with MPG. */
  .vutext : ALIGN(16) { *(.vutext .vutext.*) } >text

  .rodata : ALIGN(128) { *(.rodata .rodata.*) *(.rdata) } >text

  .init_array : ALIGN(16) {
    __init_array_start = .;
    KEEP(*(SORT(.init_array.*)))
    KEEP(*(.init_array))
    __init_array_end = .;
  } >text

  /* Written by the IOP's DMA (ee/pad.c: the USB pads' area), not loaded, not
     cleared: with the code, written through the uncached alias only. */
  .padarea (NOLOAD) : ALIGN(64) { *(.padarea) . = ALIGN(64); } >text


  /* The kernel's state of a program (ee/end.c): its .data here, its .bss
     below; set back as linked when a program ends without the restart. Of
     it, __proc_*: what a process beside the one in front has of its own,
     exchanged at a switch between the two (kproc.c, step 2e-2); the rest
     (the VM, the image, the home menu) is the one in front's. */
  .data ORIGIN(ram) : {
    __data_start = .;
    __prog_data_start = .;
    __proc_data_start = .;
    *libmxkern.a:heap.c.o(.data .data.*)
    *libmxkern.a:mem.c.o(.data .data.*)
    *libmxkern.a:exception.c.o(.data .data.*)
    *libmxkern.a:thread.c.o(.data .data.*)
    *libmxkern.a:pad.c.o(.data .data.*)
    . = ALIGN(4);
    __proc_data_end = .;
    /* the VM's (vm.c, image.c): kept, not set back, while a program that
       waits or runs beside has VM (its address space, mx OS plan 3a) */
    __vm_data_start = .;
    *libmxkern.a:vm.c.o(.data .data.*)
    *libmxkern.a:image.c.o(.data .data.*)
    . = ALIGN(4);
    __vm_data_end = .;
    *home.o(.data .data.*)
    __prog_data_end = .;
    *(.data .data.*)
  } >ram :data

  _gp = ALIGN(128) + 0x7ff0;
  .sdata : ALIGN(128) { *(.lit4) *(.lit8) *(.sdata .sdata.*) } >ram

  /* Packet data: aligned to a cache line, so a write back never touches a
     neighbour. */
  .mx.packets : ALIGN(64) { *(.mx.packets .mx.packets.*) . = ALIGN(64); } >ram
  __data_end = .;

  /* What survives a restart (ExecPS2); all before __data_end is set back by
     mxlink_start, from __data_copy. */
  .kept : ALIGN(16) { KEEP(*(.kept)) } >ram
  __loaded_end = .;

  .bss (NOLOAD) : ALIGN(128) {
    __bss_start = .;
    /* MX_DMA_LINES (<mx/cpu.h>): each variable on lines of its own, the
       last one too; the program's first (ee/end.c) */
    __prog_bss_start = .;
    /* DMA lines (the pad's buffers for SIF DMA): never exchanged, the IOP
       may be reading or writing them; the one in front's */
    *libmxkern.a:heap.c.o(.bss.mx.lines)
    *libmxkern.a:mem.c.o(.bss.mx.lines)
    *libmxkern.a:exception.c.o(.bss.mx.lines)
    *libmxkern.a:thread.c.o(.bss.mx.lines)
    *libmxkern.a:pad.c.o(.bss.mx.lines)
    . = ALIGN(64);
    __proc_bss_start = .;
    *libmxkern.a:heap.c.o(.bss .bss.* COMMON)
    *libmxkern.a:mem.c.o(.bss .bss.* COMMON)
    /* exception.c's but its stack (in use only within an exception), by
       name: a new variable there is the one in front's until added here */
    *libmxkern.a:exception.c.o(.bss.mx__exception_report.* .bss.mx__exception_state
                               .bss.mx__exception_note .bss.mx__text_lo .bss.mx__text_hi)
    *libmxkern.a:thread.c.o(.bss .bss.* COMMON)
    *libmxkern.a:pad.c.o(.bss .bss.* COMMON)
    . = ALIGN(64);
    __proc_bss_end = .;
    *libmxkern.a:exception.c.o(.bss .bss.*)
    . = ALIGN(64);
    __vm_bss_start = .; /* as __vm_data_start */
    *libmxkern.a:vm.c.o(.bss.mx.lines)
    *libmxkern.a:image.c.o(.bss.mx.lines)
    . = ALIGN(64);
    *libmxkern.a:vm.c.o(.bss .bss.* COMMON)
    *libmxkern.a:image.c.o(.bss .bss.* COMMON)
    . = ALIGN(64);
    __vm_bss_end = .;
    *home.o(.bss.mx.lines)
    . = ALIGN(64);
    *home.o(.bss .bss.* COMMON)
    . = ALIGN(64);
    __prog_bss_end = .;
    *(.bss.mx.lines)
    . = ALIGN(64);
    *(.sbss .sbss.*) *(.scommon)
    *(.bss .bss.*) *(COMMON)
    . = ALIGN(16);
    __bss_end = .;
  } >ram :NONE
  .data_copy (NOLOAD) : { __data_copy = .; . += __data_end - __data_start; } >ram
  _end = .;


  /* Scratchpad: 16 KiB at 0x7000_0000, not loaded, not cleared. */
  .spr (NOLOAD) : { *(.spr .spr.*) } >spr

  /DISCARD/ : { *(.MIPS.abiflags) *(.reginfo) *(.gnu.attributes) *(.comment) *(.pdr) }

  /* the main stack below the VM's tables; the kernel's heap is from _end up
     to it (maxsdk's start.c) */
  PROVIDE(__stack_size = __mx_stack_size);
  PROVIDE(__stack = __mx_top - __mx_tables - __stack_size);
}

ASSERT(_end <= __stack, "the kernel's data leaves no room for its stack")
ASSERT(__loaded_end <= __mx_top - 0x40000, "the kernel's loaded data reaches where mxboot runs")
