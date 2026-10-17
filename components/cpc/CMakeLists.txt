# Amstrad CPC 464/664/6128 and Plus (platform independent; host build in host/)
idf_component_register(SRCS "cpc.c"
                            "video.c"
                            "asic.c"
                            "psg.c"
                            "input.c"
                            "cart.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "."
                       REQUIRES z80 upd765)

# Runs in the emulation loop. Jump tables for the switches (the IDF turns
# them off for code that may run with the flash cache disabled; the
# emulation task never does: flash writes stall it)
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -fjump-tables -ftree-switch-conversion)
