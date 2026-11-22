
EE_BIN = spu2_probe.elf
EE_OBJS = main.o freesd_irx.o spu2probe_irx.o
EE_LIBS = -ldebug -lpatches -lkernel

.PHONY: all clean

all: $(EE_BIN)

iop/spu2probe.irx: capture_irx.c iop/imports.lst iop/Makefile
	$(MAKE) -C iop all

freesd_irx.c: $(PS2SDK)/iop/irx/freesd.irx
	$(PS2SDK)/bin/bin2c $< $@ freesd_irx

spu2probe_irx.c: iop/spu2probe.irx
	$(PS2SDK)/bin/bin2c $< $@ spu2probe_irx

clean:
	$(MAKE) -C iop clean
	rm -f $(EE_BIN) *.o freesd_irx.c spu2probe_irx.c

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal
