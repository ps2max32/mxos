/* Linker script of the resident mxlink (lld): what is written from __mx_base,
   its stack just below __mx_text, then the code and read-only data from
   __mx_text to __mx_top, each in a segment of its own. The BIOS's TLB entry 13
   maps 0x80000-0xFFFFF in halves of 256 KiB: ee/tlb.c takes the right to write
   from the upper one (the lower one holds 0x80000-0x87FFF, which the kernel
   writes through it), so a wild store of a program into mxlink's code is an
   exception. __self_start and __self_end: what mxlink takes (ee/exec.c). */

OUTPUT_ARCH(mips)
ENTRY(mxlink_start)

MEMORY {
  ram : ORIGIN = __mx_base, LENGTH = __mx_top - __mx_base
  spr : ORIGIN = 0x70000000, LENGTH = 16K
}

PHDRS {
  data PT_LOAD;
  text PT_LOAD;
}

SECTIONS {
  __self_start = __mx_base;
  __self_end = __mx_top;

  /* The kernel's state of a program (ee/end.c): its .data here, its .bss
     below; set back as linked when a program ends without the restart. */
  .data ORIGIN(ram) : {
    __data_start = .;
    __prog_data_start = .;
    *libmxkern.a:vm.c.o(.data .data.*)
    *libmxkern.a:image.c.o(.data .data.*)
    *libmxkern.a:heap.c.o(.data .data.*)
    *libmxkern.a:mem.c.o(.data .data.*)
    *libmxkern.a:exception.c.o(.data .data.*)
    *libmxkern.a:thread.c.o(.data .data.*)
    *libmxkern.a:pad.c.o(.data .data.*)
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

  .bss (NOLOAD) : ALIGN(128) {
    __bss_start = .;
    /* MX_DMA_LINES (<mx/cpu.h>): each variable on lines of its own, the
       last one too; the program's first (ee/end.c) */
    __prog_bss_start = .;
    *libmxkern.a:vm.c.o(.bss.mx.lines)
    *libmxkern.a:image.c.o(.bss.mx.lines)
    *libmxkern.a:heap.c.o(.bss.mx.lines)
    *libmxkern.a:mem.c.o(.bss.mx.lines)
    *libmxkern.a:exception.c.o(.bss.mx.lines)
    *libmxkern.a:thread.c.o(.bss.mx.lines)
    *libmxkern.a:pad.c.o(.bss.mx.lines)
    *home.o(.bss.mx.lines)
    . = ALIGN(64);
    *libmxkern.a:vm.c.o(.bss .bss.* COMMON)
    *libmxkern.a:image.c.o(.bss .bss.* COMMON)
    *libmxkern.a:heap.c.o(.bss .bss.* COMMON)
    *libmxkern.a:mem.c.o(.bss .bss.* COMMON)
    *libmxkern.a:exception.c.o(.bss .bss.* COMMON)
    *libmxkern.a:thread.c.o(.bss .bss.* COMMON)
    *libmxkern.a:pad.c.o(.bss .bss.* COMMON)
    *home.o(.bss .bss.* COMMON)
    . = ALIGN(64);
    __prog_bss_end = .;
    *(.bss.mx.lines)
    . = ALIGN(64);
    *(.sbss .sbss.*) *(.scommon)
    *(.bss .bss.*) *(COMMON)
    . = ALIGN(16);
    __bss_end = .;
  } >ram
  .data_copy (NOLOAD) : { __data_copy = .; . += __data_end - __data_start; } >ram
  _end = .;

  .text __mx_text : {
    __text_start = .;
    KEEP(*(.text.start))
    *(.text .text.*)
    __text_end = .;
  } >ram :text

  /* VU microcode, kept 16-byte aligned so it can be sent with MPG. */
  .vutext : ALIGN(16) { *(.vutext .vutext.*) } >ram

  .rodata : ALIGN(128) { *(.rodata .rodata.*) *(.rdata) } >ram

  .init_array : ALIGN(16) {
    __init_array_start = .;
    KEEP(*(SORT(.init_array.*)))
    KEEP(*(.init_array))
    __init_array_end = .;
  } >ram

  /* Scratchpad: 16 KiB at 0x7000_0000, not loaded, not cleared. */
  .spr (NOLOAD) : { *(.spr .spr.*) } >spr

  /DISCARD/ : { *(.MIPS.abiflags) *(.reginfo) *(.gnu.attributes) *(.comment) *(.pdr) }

  PROVIDE(__stack_size = __mx_stack_size);
  PROVIDE(__stack = __mx_text - __stack_size);
}

ASSERT(_end <= __stack, "mxlink's data leaves no room for the stack below its code")
