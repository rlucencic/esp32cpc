/*
 * Z80 core for the Amstrad CPC.
 *
 * Instruction-stepped, with all documented and undocumented behaviour that
 * matters (flags bits 3 and 5, MEMPTR for BIT n,(HL), IXH/IXL, SLL, the
 * DDCB forms that also copy to a register, block I/O flags). Times are in
 * CPC microseconds ("NOPs"): the Gate Array stretches every memory and I/O
 * access to a 1 us grid, so each instruction takes a whole number of them.
 *
 * Memory and I/O go through the machine (cpc_internal.h): 16 KB banks for
 * reading and writing, and cpc_in()/cpc_out() with the microsecond inside
 * the instruction at which the access happens.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef union {
    uint16_t w;
    struct {
        uint8_t l, h;       /* little-endian hosts (Xtensa, x86, ARM) */
    } b;
} z80_pair_t;

typedef struct {
    z80_pair_t af, bc, de, hl, ix, iy, sp, pc;
    z80_pair_t wz;                      /* MEMPTR */
    uint16_t af2, bc2, de2, hl2;
    uint8_t i, r;                       /* r: bit 7 as written, 0-6 counting */
    uint8_t iff1, iff2, im;
    uint8_t halted;
    uint8_t ei_delay;                   /* EI just executed: no interrupt yet */
    volatile uint8_t int_line;          /* /INT asserted */
} z80_t;

extern z80_t z80;

void z80_reset(void);

/* Execute one instruction (or accept an interrupt); its time in us */
int z80_step(void);

/* Disassemble the instruction at pc into buf; its length */
int z80_disassemble(uint16_t pc, char *buf, int len);
