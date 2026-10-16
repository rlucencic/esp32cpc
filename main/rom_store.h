#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "cpc.h"

/*
 * The CPC's ROMs: from the cpc_roms flash partition (roms/<name> in the
 * project, written by idf.py flash into 16 KB slots), else from the SD
 * card's "roms" folder under the same names.
 */
typedef enum {
    ROM_OS464, ROM_BASIC10, ROM_OS664, ROM_BASIC664, ROM_OS6128, ROM_BASIC11,
    ROM_AMSDOS, ROM_OSPLUS, ROM_BASICPLUS, ROM_COUNT
} rom_id_t;

const char *rom_store_name(rom_id_t id);   /* "os6128.rom" */

/* 16 KB into buf; false if there's none */
bool rom_store_load(rom_id_t id, uint8_t *buf);

/* The Plus system cartridge into pages (CPC_CART_MAX bytes): system.cpr,
 * else built from osplus.rom, basicplus.rom and amsdos.rom (pages 0, 1, 3).
 * Its size in bytes, 0 if there's none; *name says which.
 */
uint32_t rom_store_system_cart(uint8_t *pages, const char **name);
