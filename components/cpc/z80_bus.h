/*
 * What the Z80 core needs from the machine: the memory banks and the I/O
 * ports. The CPC provides them (cpc.c); the host build's CPU test has its
 * own flat 64 KB.
 */
#pragma once

#include <stdint.h>

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#define CPC_HOT     IRAM_ATTR
#else
#define CPC_HOT
#endif

/* 16 KB banks as the CPU sees them; a NULL write bank goes to
 * cpc_write_special() (the Plus ASIC's register page)
 */
extern uint8_t *cpc_rd[4];
extern uint8_t *cpc_wr[4];

void cpc_write_special(uint16_t addr, uint8_t v);

static inline uint8_t mem_rd(uint16_t a)
{
    return cpc_rd[a >> 14][a & 0x3FFF];
}

static inline void mem_wr(uint16_t a, uint8_t v)
{
    uint8_t *p = cpc_wr[a >> 14];
    if (p) {
        p[a & 0x3FFF] = v;
    } else {
        cpc_write_special(a, v);
    }
}

/* I/O at microsecond `at` of the current instruction (the machine brings
 * its video up to that moment first)
 */
uint8_t cpc_in(uint16_t port, int at);
void cpc_out(uint16_t port, uint8_t v, int at);

/* Interrupt acknowledge: the byte on the data bus (the IM 2 vector) */
uint8_t cpc_int_ack(void);
