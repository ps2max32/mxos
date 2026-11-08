# mxlink: the EE side of the PS2 loader, on maxsdk (clang), with ps2link's
# IOP modules.
#
#   make            build/tools/mxlink/MXLINK.ELF (at 0x88000: to
#                   boot from the PS2's storage)
#   make high       build/tools/mxlink/mxlink-high.elf (at 0x1E00000: to start
#                   with ps2client execee from a running ps2link, for tests)
#   make emu        build/tools/mxlink-emu/mxlink.elf:
#                   the kernel for PCSX2: libmxk programs need one, and it has
#                   no network there; it starts what host:mxload.txt says

# the default goal, before maxsdk.mk's rules
all:

PS2MAX  ?= $(abspath $(CURDIR)/../..)
OUT     ?= $(PS2MAX)/build/tools/mxlink
# the IOP side from ps2dev (envsetup.sh): ps2sdk's IOP headers and iopfixup,
# and its prebuilt IOP modules (the IOP modules here are clang's and lld's)
PS2MAX_PS2DEV  ?= $(abspath $(PS2MAX)/../ps2dev/install)
PS2MAX_IOP_SDK ?= $(PS2MAX_PS2DEV)/ps2sdk
PS2MAX_IRX     ?= $(PS2MAX_IOP_SDK)/iop/irx
# ps2link's IOP module, built here from iop/
PS2LINK_IRX = $(OUT)/iop/ps2link.irx
CHIME_IRX   = $(OUT)/iop/chime.irx

# its own libmx, built for size and with a section per function and
# variable, so the link keeps only what mxlink uses (it has to fit below
# 1 MiB); the other programs' libmx stays as it is
MAXSDK_OUT = $(OUT)/maxsdk
export EXTRA_CFLAGS = -Os -ffunction-sections -fdata-sections

include $(PS2MAX)/libs/maxsdk/mk/maxsdk.mk

# SHOT=1: for tests on PCSX2, the screen saved where the start stops
CFLAGS += $(if $(SHOT),-DMXLINK_SHOT)
# EMU=1 (make emu): the kernel for PCSX2, without network, IOP
# modules or commands: it starts the program in host:mxload.txt
CFLAGS += $(if $(EMU),-DMXLINK_EMU)

SRCS = main.c cmd.c exec.c con.c font.c tlb.c pad.c home.c end.c
# the IOP modules, in $(OUT)/MXOS: files of the memory card (mc0:/MXOS)
MODULES = poweroff mxdev9 mxeth mxnet mxfast ioptrap sio2man mcman mcserv padman libsd audsrv \
          mxio mxsock

# resident below the programs' memory: 0x88000 up to the 2 KiB
# where ps2link.irx writes the host's commands (0xFF800)
LOW_LDFLAGS  = --defsym=__mx_base=0x88000 --defsym=__mx_text=0xC0000 --defsym=__mx_top=0xFF800 \
               --defsym=__mx_stack_size=0x4000
HIGH_LDFLAGS = --defsym=__mx_base=0x1E00000 --defsym=__mx_text=0x1E40000 --defsym=__mx_top=0x1F00000 \
               --defsym=__mx_stack_size=0x4000

OBJS = $(SRCS:%.c=$(OUT)/obj/%.o) $(OUT)/obj/hook.o

all: $(OUT)/MXLINK.ELF $(OUT)/iop/mxroot.irx modules
modules: $(MODULES:%=$(OUT)/MXOS/%.irx) $(OUT)/MXOS/ps2link.irx $(OUT)/MXOS/chime.irx
high: $(OUT)/mxlink-high.elf
emu:
	$(MAKE) EMU=1 OUT=$(OUT)-emu $(OUT)-emu/mxlink.elf

$(OUT)/obj/%.o: ee/%.c ee/mxlink.h $(MX_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT)/obj/hook.o: ee/hook.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT)/MXOS/%.irx: $(PS2MAX_IRX)/%.irx
	@mkdir -p $(dir $@)
	cp $< $@

# maxsdk's network drivers, stack and fast path, its file and socket
# services, its USB mass storage driver in place of ps2sdk's and its HDD
# driver: all built by one make of maxsdk's iop/ (one per module would run
# in parallel with -j and build the same files at once), into maxsdk's own
# build dir whatever OUT is
MAXSDK_IRX = $(PS2MAX)/build/libs/maxsdk/iop
$(OUT)/MXOS/mxdev9.irx $(OUT)/MXOS/mxeth.irx $(OUT)/MXOS/mxnet.irx $(OUT)/MXOS/mxfast.irx \
$(OUT)/MXOS/mxio.irx $(OUT)/MXOS/mxsock.irx $(OUT)/MXOS/usbmass_bd.irx $(OUT)/MXOS/mxata.irx: maxsdk-iop
	@mkdir -p $(dir $@)
	cp $(MAXSDK_IRX)/$(notdir $@) $@
maxsdk-iop:
	$(MAKE) -C $(PS2MAX)/libs/maxsdk/iop EXTRA_CFLAGS= OUT=$(MAXSDK_IRX)
.PHONY: maxsdk-iop

$(OUT)/MXOS/ps2link.irx: $(PS2LINK_IRX)
	@mkdir -p $(dir $@)
	cp $< $@

$(OUT)/MXOS/chime.irx: $(CHIME_IRX)
	@mkdir -p $(dir $@)
	cp $< $@

# the shell: mc0:/MXOS/MXSHELL.ELF, and the storage drivers it loads from there
SHELL_SRCS = main.c draw.c cover.c games.c
SHELL_OBJS = $(SHELL_SRCS:%.c=$(OUT)/shell/%.o) $(OUT)/shell/font.o
STORAGE_MODULES = bdm bdmfs_fatfs usbd usbmass_bd mx4sio_bd mxata

shell: $(OUT)/MXOS/MXSHELL.ELF $(STORAGE_MODULES:%=$(OUT)/MXOS/%.irx)

$(OUT)/shell/%.o: shell/%.c shell/shell.h shell/font.h $(MX_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT)/shell/font.o: shell/font.S shell/font.bin
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# libmxk in core: the kernel's services, without VM
SHELL_LIB = $(MAXSDK_OUT)/libmxk.a
$(SHELL_LIB): $(MX_LIB)
$(OUT)/shell/shell.elf: $(SHELL_OBJS) $(SHELL_LIB) $(MX_CRT0) $(MX_LD)
	$(call link,$@,$(MX_CRT0) $(SHELL_OBJS) $(SHELL_LIB))

$(OUT)/MXOS/MXSHELL.ELF: $(OUT)/shell/shell.elf
	@mkdir -p $(dir $@)
	$(LLVM)/llvm-strip -o $@ $<

mxos-fill: $(OUT)/mxos-fill.elf
$(OUT)/mxos-fill.elf: tools/mxos-fill.c $(MX_LIB) $(MX_CRT0)
	@mkdir -p $(OUT)/obj
	$(CC) $(CFLAGS) -c $< -o $(OUT)/obj/mxos-fill.o
	$(call link,$@,$(MX_CRT0) $(OUT)/obj/mxos-fill.o $(MX_LIB))

# the kernel: maxsdk with the VM
KERNEL_LIB = $(MAXSDK_OUT)/libmxkern.a
$(KERNEL_LIB): $(MX_LIB)
$(OUT)/mxlink.elf: MX_LD = ee/resident.ld
$(OUT)/mxlink.elf: $(OBJS) $(KERNEL_LIB) $(MX_CRT0) ee/resident.ld
	$(call link,$@,$(LOW_LDFLAGS) $(MX_CRT0) $(OBJS) $(KERNEL_LIB))

# the name ps2link has on the PS2's storage
$(OUT)/MXLINK.ELF: $(OUT)/mxlink.elf
	$(LLVM)/llvm-strip -o $@ $<

$(OUT)/mxlink-high.elf: MX_LD = ee/resident.ld
$(OUT)/mxlink-high.elf: $(OBJS) $(KERNEL_LIB) $(MX_CRT0) ee/resident.ld
	$(call link,$@,$(HIGH_LDFLAGS) $(MX_CRT0) $(OBJS) $(KERNEL_LIB))

# ---- the IOP modules (iop/: ps2link's, the chime, mxroot), with LLVM ----------------------

IOP_CC     = $(LLVM)/clang --target=mipsel-none-elf -mcpu=r3000a
IOP_CFLAGS = -std=gnu23 -D_IOP -fno-builtin -ffreestanding -G0 -Os -Wall -Werror -Wno-unused-but-set-global \
             -msoft-float -mno-abicalls -fno-pic -fno-unwind-tables -fno-asynchronous-unwind-tables \
             -I$(PS2MAX_IOP_SDK)/iop/include -I$(PS2MAX_IOP_SDK)/common/include -Iiop
# the import tables through maxsdk's irx_clang.h (clang puts file-scope asm first)
IOP_IMPORT_CFLAGS = -include $(PS2MAX)/libs/maxsdk/iop/irx_clang.h
# ld.lld -r with maxsdk's IOP link script, as maxsdk's iop/Rules.mk
IOP_LD     = $(LLVM)/ld.lld -m elf32ltsmip -r -T $(PS2MAX)/libs/maxsdk/iop/irx.ld
IOP_SRCS   = net_fsys.c net_fio.c ps2link.c cmdHandler.c nprintf.c excepHandler.c
IOP_OBJS   = $(IOP_SRCS:%.c=$(OUT)/iop/%.o) $(OUT)/iop/imports.o

$(OUT)/iop/%.o: iop/%.c $(wildcard iop/*.h)
	@mkdir -p $(dir $@)
	$(IOP_CC) $(IOP_CFLAGS) -c $< -o $@

$(OUT)/iop/imports.o: iop/imports.lst iop/irx_imports.h $(PS2MAX)/libs/maxsdk/iop/irx_clang.h
	@mkdir -p $(dir $@)
	{ echo '#include "irx_imports.h"'; cat $<; } > $(@:.o=.c)
	$(IOP_CC) $(IOP_CFLAGS) $(IOP_IMPORT_CFLAGS) -c $(@:.o=.c) -o $@

$(OUT)/iop/ps2link.elf: $(IOP_OBJS)
	$(IOP_LD) -o $@ $^

$(OUT)/iop/chime_imports.o: iop/chime_imports.lst iop/irx_imports.h $(PS2MAX)/libs/maxsdk/iop/irx_clang.h
	@mkdir -p $(dir $@)
	{ echo '#include "irx_imports.h"'; cat $<; } > $(@:.o=.c)
	$(IOP_CC) $(IOP_CFLAGS) $(IOP_IMPORT_CFLAGS) -c $(@:.o=.c) -o $@

$(OUT)/iop/chime.elf: $(OUT)/iop/chime.o $(OUT)/iop/chime_imports.o
	$(IOP_LD) -o $@ $^

$(OUT)/iop/mxroot_imports.o: iop/mxroot_imports.lst iop/irx_imports.h $(PS2MAX)/libs/maxsdk/iop/irx_clang.h
	@mkdir -p $(dir $@)
	{ echo '#include "irx_imports.h"'; cat $<; } > $(@:.o=.c)
	$(IOP_CC) $(IOP_CFLAGS) $(IOP_IMPORT_CFLAGS) -c $(@:.o=.c) -o $@

$(OUT)/iop/mxroot.elf: $(OUT)/iop/mxroot.o $(OUT)/iop/mxroot_imports.o
	$(IOP_LD) -o $@ $^

$(OUT)/iop/%.irx: $(OUT)/iop/%.elf
	$(LLVM)/llvm-objcopy --strip-unneeded $< $(@:.irx=.stripped.elf)
	$(PS2MAX_IOP_SDK)/bin/iopfixup --irx1 -o $@ $(@:.irx=.stripped.elf)

clean:
	rm -rf $(OUT)

.PHONY: all high emu modules mxos-fill shell clean
