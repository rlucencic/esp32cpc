# Amstrad CPC 464/664/6128 and Plus (platform independent; host build in host/)
idf_component_register(SRCS "cpc.c"
                            "z80.c"
                            "video.c"
                            "asic.c"
                            "psg.c"
                            "fdc.c"
                            "input.c"
                            "cart.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS ".")

# Runs in the emulation loop. Jump tables for the Z80's opcode switches (the
# IDF turns them off for code that may run with the flash cache disabled;
# the emulation task never does: flash writes stall it)
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -fjump-tables -ftree-switch-conversion)
