# mxlink: the EE side of the PS2 loader, on maxsdk (clang), with ps2link's
# IOP modules.
#
#   make            build/tools/mxlink/MXLINK.ELF (at 0x94000, as ps2link: to
#                   boot from the PS2's storage)
#   make high       build/tools/mxlink/mxlink-high.elf (at 0x1E00000: to start
#                   with ps2client execee from a running ps2link, for tests)

# the default goal, before maxsdk.mk's rules
all:

PS2MAX  ?= $(abspath $(CURDIR)/../..)
OUT     ?= $(PS2MAX)/build/tools/mxlink
# the IOP side from ps2dev (envsetup.sh): the IOP gcc, ps2sdk's IOP headers,
# link script and iopfixup, and its prebuilt IOP modules
PS2MAX_PS2DEV  ?= $(abspath $(PS2MAX)/../ps2dev/install)
PS2MAX_IOP_BIN ?= $(PS2MAX_PS2DEV)/iop/bin
PS2MAX_IOP_SDK ?= $(PS2MAX_PS2DEV)/ps2sdk
PS2MAX_IRX     ?= $(PS2MAX_IOP_SDK)/iop/irx
# ps2link's IOP module, built here from iop/
PS2LINK_IRX = $(OUT)/iop/ps2link.irx

# its own libmx, built for size and with a section per function and
# variable, so the link keeps only what mxlink uses (it has to fit below
# 1 MiB); the other programs' libmx stays as it is
MAXSDK_OUT = $(OUT)/maxsdk
export EXTRA_CFLAGS = -Os -ffunction-sections -fdata-sections

include $(PS2MAX)/libs/maxsdk/mk/maxsdk.mk

# SHOT=1: for tests on PCSX2, the screen saved where the start stops
CFLAGS += $(if $(SHOT),-DMXLINK_SHOT)

SRCS = main.c cmd.c exec.c con.c font.c
IRXS = poweroff ps2dev9 netman smap ps2ip udptty ioptrap ps2link
irx_path = $(if $(filter ps2link,$(1)),$(PS2LINK_IRX),$(PS2MAX_IRX)/$(if $(filter ps2ip,$(1)),ps2ip-nm,$(1)).irx)

# resident below the programs' memory, as ps2link: 0x94000 up to the 2 KiB
# where ps2link.irx writes the host's commands (0xFF800)
LOW_LDFLAGS  = --defsym=__mx_base=0x94000 --defsym=__mx_top=0xFF800 --defsym=__mx_stack_size=0x4000
HIGH_LDFLAGS = --defsym=__mx_base=0x1E00000 --defsym=__mx_top=0x1F00000 --defsym=__mx_stack_size=0x4000

OBJS = $(SRCS:%.c=$(OUT)/obj/%.o) $(OUT)/obj/hook.o $(OUT)/obj/irx.o

all: $(OUT)/MXLINK.ELF
high: $(OUT)/mxlink-high.elf

$(OUT)/obj/%.o: ee/%.c ee/mxlink.h $(MX_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT)/obj/hook.o: ee/hook.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT)/obj/irx.S: Makefile $(foreach i,$(IRXS),$(call irx_path,$(i)))
	@mkdir -p $(dir $@)
	{ echo '.section .rodata'; \
	  $(foreach i,$(IRXS),printf '%s\n' '.balign 16' '.globl $(i)_irx' '$(i)_irx:' \
	    '.incbin "$(call irx_path,$(i))"' '.globl $(i)_irx_end' '$(i)_irx_end:';) } > $@

$(OUT)/obj/irx.o: $(OUT)/obj/irx.S
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT)/mxlink.elf: $(OBJS) $(MX_LIB) $(MX_CRT0) $(MX_LD)
	$(call link,$@,$(LOW_LDFLAGS) $(MX_CRT0) $(OBJS) $(MX_LIB))

# the name ps2link has on the PS2's storage
$(OUT)/MXLINK.ELF: $(OUT)/mxlink.elf
	$(LLVM)/llvm-strip -o $@ $<

$(OUT)/mxlink-high.elf: $(OBJS) $(MX_LIB) $(MX_CRT0) $(MX_LD)
	$(call link,$@,$(HIGH_LDFLAGS) $(MX_CRT0) $(OBJS) $(MX_LIB))

# ---- the IOP module (iop/: ps2link's, ps2sdk's IOP build rules) -------------

IOP_CC     = $(PS2MAX_IOP_BIN)/mipsel-none-elf-gcc
IOP_STRIP  = $(PS2MAX_IOP_BIN)/mipsel-none-elf-strip
# ps2sdk's flags (samples/Makefile.iopglobal); with its -gdwarf-2 the module is
# byte for byte the one of ps2link's own build (it changes the code a little)
IOP_CFLAGS = -D_IOP -fno-builtin -G0 -Os -Wall -Werror -gdwarf-2 -gz -msoft-float -mno-explicit-relocs \
             -I$(PS2MAX_IOP_SDK)/iop/include -I$(PS2MAX_IOP_SDK)/common/include -Iiop
IOP_SRCS   = net_fsys.c net_fio.c ps2link.c cmdHandler.c nprintf.c excepHandler.c
IOP_OBJS   = $(IOP_SRCS:%.c=$(OUT)/iop/%.o) $(OUT)/iop/imports.o

$(OUT)/iop/%.o: iop/%.c $(wildcard iop/*.h)
	@mkdir -p $(dir $@)
	$(IOP_CC) $(IOP_CFLAGS) -c $< -o $@

$(OUT)/iop/imports.o: iop/imports.lst iop/irx_imports.h
	@mkdir -p $(dir $@)
	{ echo '#include "irx_imports.h"'; cat $<; } > $(@:.o=.c)
	$(IOP_CC) $(IOP_CFLAGS) -fno-toplevel-reorder -c $(@:.o=.c) -o $@

$(OUT)/iop/ps2link.elf: $(IOP_OBJS)
	$(IOP_CC) -T$(PS2MAX_IOP_SDK)/iop/startup/linkfile -Os -o $@ $^ -nostdlib -dc -r

$(PS2LINK_IRX): $(OUT)/iop/ps2link.elf
	$(IOP_STRIP) --strip-unneeded --remove-section=.pdr --remove-section=.comment \
	  --remove-section=.mdebug.abi32 --remove-section=.gnu.attributes -o $(@:.irx=.stripped.elf) $<
	$(PS2MAX_IOP_SDK)/bin/iopfixup --irx1 -o $@ $(@:.irx=.stripped.elf)

clean:
	rm -rf $(OUT)

.PHONY: all high clean
