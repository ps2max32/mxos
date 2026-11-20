# INIT.ELF: mx OS's boot script (init.c), an MXOS program (libmxk, VM).
#
#   make    build/apps/mxos/init/INIT.ELF (beside the kernel: its directory)

all:

PS2MAX ?= $(abspath $(CURDIR)/../../..)
OUT    ?= $(PS2MAX)/build/apps/mxos/init

MX_TARGET = mxos
include $(PS2MAX)/libs/maxsdk/mk/maxsdk.mk

all: $(OUT)/INIT.ELF

$(OUT)/obj/init.o: init.c $(MX_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

LIB = $(MAXSDK_OUT)/libmxk.a
$(LIB): $(MX_LIB)
$(OUT)/INIT.ELF: $(OUT)/obj/init.o $(LIB) $(MX_CRT0) $(MX_LD)
	$(call link,$@,$(MX_CRT0) $(OUT)/obj/init.o $(LIB))
