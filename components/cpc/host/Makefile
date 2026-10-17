# Host build of the CPC core: make -C components/cpc/host
#   build/cpc_host <model> [frames] [out.ppm] [ram_kb]   (see host_main.c)
# The Z80 core and the uPD765 are esp32emu's (the Z80's own test:
# make -C ../esp32emu/components/z80/test)
CPC       := ..
Z80       := ../../../../esp32emu/components/z80
FDC       := ../../../../esp32emu/components/upd765
BUILD     := build
CFLAGS    := -O2 -g -Wall -Wextra -Wno-unused-parameter -DZ80_HOST_HOOK -DCONFIG_Z80_TIMING_CPC=1 \
             -I$(CPC)/include -I$(CPC) -I$(Z80)/include -I$(FDC)/include

CPC_SRCS  := $(wildcard $(CPC)/*.c)
CPC_OBJS  := $(patsubst $(CPC)/%.c,$(BUILD)/cpc/%.o,$(CPC_SRCS)) $(BUILD)/z80.o $(BUILD)/upd765.o

all: $(BUILD)/cpc_host

$(BUILD)/cpc_host: host_main.c $(CPC_OBJS)
	$(CC) $(CFLAGS) $^ -lm -o $@

$(BUILD)/cpc/%.o: $(CPC)/%.c $(wildcard $(CPC)/*.h $(CPC)/include/*.h) $(Z80)/include/z80.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/z80.o: $(Z80)/z80.c $(Z80)/include/z80.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/upd765.o: $(FDC)/upd765.c $(FDC)/include/upd765.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD)

.PHONY: all clean
