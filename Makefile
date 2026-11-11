# mxos: the kernel of mx OS on the PS2 (MXKERNEL.ELF), on maxsdk (clang), and
# the card's files around it (mc0:/MXOS). The host link is mxlink's app
# (apps/mxlink: MXLINK.ELF, a service); the kernel keeps a fallback for the
# host (ee/cmd.c) and loads ps2link's IOP module from apps/mxlink
# (MXLINK=<its directory>).
#
#   make            build/apps/mxos/MXKERNEL.ELF (at 0x88000: to
#                   boot from the PS2's storage)
#   make high       build/apps/mxos/mxkernel-high.elf (at 0x1E00000: to start
#                   with ps2client execee from a running kernel, for tests)
#   make shell      mxshell (apps/mxshell, MXSHELL=<its directory>) and its
#                   storage drivers into build/apps/mxos/MXOS, for the card;
#                   with mxlink's app (MXLINK.ELF) and SERVICES.CFG
#   make emu        build/apps/mxos-emu/mxkernel.elf:
#                   the kernel for PCSX2: libmxk programs need one, and it has
#                   no network there; it starts what host:mxload.txt says

# the default goal, before maxsdk.mk's rules
all:

PS2MAX  ?= $(abspath $(CURDIR)/../..)
OUT     ?= $(PS2MAX)/build/apps/mxos
# the IOP side from ps2dev (envsetup.sh): ps2sdk's IOP headers and its
# prebuilt IOP modules (the IOP modules here are clang's and lld's)
PS2MAX_PS2DEV  ?= $(abspath $(PS2MAX)/../ps2dev/install)
PS2MAX_IOP_SDK ?= $(PS2MAX_PS2DEV)/ps2sdk
PS2MAX_IRX     ?= $(PS2MAX_IOP_SDK)/iop/irx
# ps2link's IOP module, built by apps/mxlink; the shell, by apps/mxshell
MXLINK      ?= $(PS2MAX)/apps/mxlink
MXSHELL     ?= $(PS2MAX)/apps/mxshell
PS2LINK_IRX = $(OUT)/mxlink/ps2link.irx
CHIME_IRX   = $(OUT)/iop/chime.irx

# its own libmx, built for size and with a section per function and
# variable, so the link keeps only what mxlink uses (it has to fit below
# 1 MiB); the other programs' libmx stays as it is
MAXSDK_OUT = $(OUT)/maxsdk
# its maxsdk too: a small debug hold buffer (the kernel never holds), and
# its software timers (mx_sleep_us, mx_timer_add) on timer 1, timer 0 lent out
# (mx_timer_claim: mx_hires)
export EXTRA_CFLAGS = -Os -ffunction-sections -fdata-sections -DMX_DEBUG_HOLD_BYTES=256 \
                      -DMX_SOFT_TIMER=1

# the kernel and its tools are started by the BIOS: standalone (the kernel links libmxkern itself)
MX_TARGET ?= standalone
include $(PS2MAX)/libs/maxsdk/mk/maxsdk.mk

# SHOT=1: for tests on PCSX2, the screen saved where the start stops
CFLAGS += $(if $(SHOT),-DMXLINK_SHOT)
# maxsdk's protocols with its IOP modules (mxpad.h)
CFLAGS += -I$(PS2MAX)/libs/maxsdk/common/include
# EMU=1 (make emu): the kernel for PCSX2, without network, IOP
# modules or commands: it starts the program in host:mxload.txt
CFLAGS += $(if $(EMU),-DMXLINK_EMU)
# make MODULE_DIR=mc0:/MXOS/NEW/: the IOP modules (and the shell) from there,
# to try new ones with the one-shot boot
CFLAGS += $(if $(MODULE_DIR),-DMODULE_DIR='"$(MODULE_DIR)"')

SRCS = main.c cmd.c exec.c con.c font.c tlb.c pad.c home.c end.c notrace.c kirq.c ksched.c kproc.c kmsg.c klog.c kvideo.c
# the IOP modules, in $(OUT)/MXOS: files of the memory card (mc0:/MXOS)
MODULES = poweroff mxdev9 mxeth mxnet mxfast ioptrap sio2man mcman mcserv padman libsd mxsnd \
          mxio mxsock usbd mxpad

# resident below the programs' memory: 0x88000 up to the 2 KiB
# where ps2link.irx writes the host's commands (0xFF800)
LOW_LDFLAGS  = --defsym=__mx_base=0x88000 --defsym=__mx_text=0xC0000 --defsym=__mx_top=0xFF800 \
               --defsym=__mx_stack_size=0x4000
HIGH_LDFLAGS = --defsym=__mx_base=0x1E00000 --defsym=__mx_text=0x1E40000 --defsym=__mx_top=0x1F00000 \
               --defsym=__mx_stack_size=0x4000

OBJS = $(SRCS:%.c=$(OUT)/obj/%.o) $(OUT)/obj/hook.o $(OUT)/obj/kirq_s.o $(OUT)/obj/ksched_s.o \
       $(OUT)/obj/kvu0.o

all: $(OUT)/MXKERNEL.ELF $(OUT)/iop/mxroot.irx modules
modules: $(MODULES:%=$(OUT)/MXOS/%.irx) $(OUT)/MXOS/ps2link.irx $(OUT)/MXOS/chime.irx
high: $(OUT)/mxkernel-high.elf
emu:
	$(MAKE) EMU=1 OUT=$(OUT)-emu $(OUT)-emu/mxkernel.elf

$(OUT)/obj/%.o: ee/%.c ee/mxlink.h $(MX_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT)/obj/hook.o: ee/hook.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT)/obj/kirq_s.o: ee/kirq.S ee/ksched.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# VU0's registers of a process (kproc.c)
$(OUT)/obj/kvu0.o: ee/kvu0.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -mvu0 -c $< -o $@

$(OUT)/obj/ksched_s.o: ee/ksched.S ee/ksched.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT)/MXOS/%.irx: $(PS2MAX_IRX)/%.irx
	@mkdir -p $(dir $@)
	cp $< $@

# maxsdk's network drivers, stack and fast path, its file and socket
# services, its sound, its USB mass storage driver in place of ps2sdk's and its HDD
# driver: all built by one make of maxsdk's iop/ (one per module would run
# in parallel with -j and build the same files at once), into maxsdk's own
# build dir whatever OUT is
MAXSDK_IRX = $(PS2MAX)/build/libs/maxsdk/iop
$(OUT)/MXOS/mxdev9.irx $(OUT)/MXOS/mxeth.irx $(OUT)/MXOS/mxnet.irx $(OUT)/MXOS/mxfast.irx \
$(OUT)/MXOS/mxio.irx $(OUT)/MXOS/mxsock.irx $(OUT)/MXOS/usbmass_bd.irx $(OUT)/MXOS/mxata.irx \
$(OUT)/MXOS/mxpad.irx $(OUT)/MXOS/mxsnd.irx: maxsdk-iop
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

$(PS2LINK_IRX): mxlink-iop
mxlink-iop:
	$(MAKE) -C $(MXLINK) OUT=$(OUT)/mxlink
.PHONY: mxlink-iop

# the shell: mc0:/MXOS/MXSHELL.ELF, and the storage drivers it loads from
# there; the host link's app, the service the kernel starts at the boot
# (SERVICES.CFG: kernel version 20)
shell: mxlink-iop
	$(MAKE) -C $(MXSHELL) OUT=$(OUT)/mxshell
	@mkdir -p $(OUT)/MXOS
	cp $(OUT)/mxshell/MXOS/* $(OUT)/MXOS/
	cp $(OUT)/mxlink/mxlink.elf $(OUT)/MXOS/MXLINK.ELF
	printf '%sMXLINK.ELF\n' '$(or $(MODULE_DIR),mc0:/MXOS/)' > $(OUT)/MXOS/SERVICES.CFG

mxos-fill: $(OUT)/mxos-fill.elf
$(OUT)/mxos-fill.elf: tools/mxos-fill.c $(MX_LIB) $(MX_CRT0)
	@mkdir -p $(OUT)/obj
	$(CC) $(CFLAGS) -c $< -o $(OUT)/obj/mxos-fill.o
	$(call link,$@,$(MX_CRT0) $(OUT)/obj/mxos-fill.o $(MX_LIB))

# the kernel: maxsdk with the VM
KERNEL_LIB = $(MAXSDK_OUT)/libmxkern.a
$(KERNEL_LIB): $(MX_LIB)
$(OUT)/mxkernel.elf: MX_LD = ee/resident.ld
$(OUT)/mxkernel.elf: $(OBJS) $(KERNEL_LIB) $(MX_CRT0) ee/resident.ld
	$(call link,$@,$(LOW_LDFLAGS) $(MX_CRT0) $(OBJS) $(KERNEL_LIB))

# the name ps2link has on the PS2's storage
$(OUT)/MXKERNEL.ELF: $(OUT)/mxkernel.elf
	$(LLVM)/llvm-strip -o $@ $<

$(OUT)/mxkernel-high.elf: MX_LD = ee/resident.ld
$(OUT)/mxkernel-high.elf: $(OBJS) $(KERNEL_LIB) $(MX_CRT0) ee/resident.ld
	$(call link,$@,$(HIGH_LDFLAGS) $(MX_CRT0) $(OBJS) $(KERNEL_LIB))

# ---- the kernel's IOP modules (iop/: the chime, mxroot), with LLVM ----------------------

# maxsdk's IOP toolchain (libs/maxsdk/iop/iop.mk): clang for mipsel-none-irx,
# which links with ld.lld --irx into the IRX, the import tables from the ILB
# files of maxsdk's and ps2sdk's IOP libraries
PS2MAX_LLVM_BIN ?= $(LLVM)
include $(PS2MAX)/libs/maxsdk/iop/iop.mk
IOP_FLAGS  = $(IOP_CFLAGS) -Os -Werror $(IOP_INCS) -Iiop

$(OUT)/iop/%.o: iop/%.c $(wildcard iop/*.h) $(PS2MAX)/libs/maxsdk/iop/iop.mk
	@mkdir -p $(dir $@)
	$(IOP_CC) $(IOP_FLAGS) -c $< -o $@

$(OUT)/iop/chime.irx: $(OUT)/iop/chime.o
$(OUT)/iop/mxroot.irx: $(OUT)/iop/mxroot.o
$(OUT)/iop/%.irx: $(MAXSDK_ILBS) $(IOP_ILB_RSP)
	$(IOP_CC) -s -o $@ $(filter %.o,$^) $(call iop_link_libs)
# made again when their objects are, not left out as in-between files
.NOTINTERMEDIATE: $(OUT)/iop/chime.irx $(OUT)/iop/mxroot.irx

clean:
	rm -rf $(OUT)

.PHONY: all high emu modules mxos-fill shell clean
