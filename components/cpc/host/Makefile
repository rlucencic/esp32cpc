# Host build of the CPC core: make -C components/cpc/host
#   build/cpc_host <model> [frames] [out.ppm] [ram_kb]   (see host_main.c)
#   build/z80_test <prelim.com | zexdoc.cim | zexall.cim>
CPC       := ..
BUILD     := build
CFLAGS    := -O2 -g -Wall -Wextra -Wno-unused-parameter -DZ80_HOST_HOOK -I$(CPC)/include -I$(CPC)

CPC_SRCS  := $(wildcard $(CPC)/*.c)
CPC_OBJS  := $(patsubst $(CPC)/%.c,$(BUILD)/cpc/%.o,$(CPC_SRCS))

all: $(BUILD)/cpc_host $(BUILD)/z80_test

$(BUILD)/cpc_host: host_main.c $(CPC_OBJS)
	$(CC) $(CFLAGS) $^ -lm -o $@

$(BUILD)/z80_test: z80_test.c $(CPC)/z80.c $(CPC)/z80.h $(CPC)/z80_bus.h
	$(CC) $(filter-out -DZ80_HOST_HOOK,$(CFLAGS)) z80_test.c $(CPC)/z80.c -o $@

$(BUILD)/cpc/%.o: $(CPC)/%.c $(wildcard $(CPC)/*.h $(CPC)/include/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD)

.PHONY: all clean
