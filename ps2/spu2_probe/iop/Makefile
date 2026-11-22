
IOP_BIN = spu2probe.irx

IOP_OBJS = capture_irx.o imports.o

IOP_IMPORT_INCS += \
    sound/libsd \
    system/sifcmd \
    system/threadman \
    system/sysclib \
    system/stdio

.PHONY: all
.DEFAULT_GOAL := all

all: $(IOP_BIN)

vpath %.c ..
vpath %.lst .

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.iopglobal
