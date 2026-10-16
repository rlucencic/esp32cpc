/*
 * Z80 core test: runs CP/M test programs (prelim.com, zexdoc.cim,
 * zexall.cim) on a flat 64 KB with the two BDOS calls they use.
 *
 *   z80_test <program.com>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "z80.h"
#include "z80_bus.h"

static uint8_t s_mem[65536];
uint8_t *cpc_rd[4] = { s_mem, s_mem + 0x4000, s_mem + 0x8000, s_mem + 0xC000 };
uint8_t *cpc_wr[4] = { s_mem, s_mem + 0x4000, s_mem + 0x8000, s_mem + 0xC000 };

void cpc_write_special(uint16_t addr, uint8_t v) { (void)addr; (void)v; }
uint8_t cpc_in(uint16_t port, int at) { (void)port; (void)at; return 0xFF; }
void cpc_out(uint16_t port, uint8_t v, int at) { (void)port; (void)v; (void)at; }
uint8_t cpc_int_ack(void) { return 0xFF; }

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <program.com>\n", argv[0]);
        return 1;
    }
    FILE *f = fopen(argv[1], "rb");
    if (f == NULL) {
        perror(argv[1]);
        return 1;
    }
    fread(s_mem + 0x100, 1, 0xFE00, f);
    fclose(f);
    z80_reset();
    z80.pc.w = 0x100;
    z80.sp.w = 0xF000;
    s_mem[5] = 0xC9;        /* BDOS: handled below, then RET */
    uint64_t us = 0, instructions = 0;
    for (;;) {
        uint16_t pc = z80.pc.w;
        if (pc == 0) {
            break;
        }
        if (pc == 5) {
            if (z80.bc.b.l == 2) {
                putchar(z80.de.b.l);
            } else if (z80.bc.b.l == 9) {
                for (uint16_t a = z80.de.w; s_mem[a] != '$'; a++) {
                    putchar(s_mem[a]);
                }
            }
            fflush(stdout);
        }
        us += z80_step();
        instructions++;
    }
    printf("\n%llu instructions, %llu us of CPC time\n", (unsigned long long)instructions,
           (unsigned long long)us);
    return 0;
}
