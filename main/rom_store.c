#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_partition.h"
#include "emu/sdcard.h"
#include "rom_store.h"

#define ROM_PARTITION_TYPE  0x40
#define ROM_FOLDER          SDCARD_MOUNT_POINT "/roms"
#define CART_SLOT           10          /* system.cpr from 160 KB on */

static const char *TAG = "rom";

static const char *const names[ROM_COUNT] = {
    "os464.rom", "basic10.rom", "os664.rom", "basic664.rom", "os6128.rom", "basic11.rom",
    "amsdos.rom", "osplus.rom", "basicplus.rom",
};

const char *rom_store_name(rom_id_t id)
{
    return id < ROM_COUNT ? names[id] : "";
}

/* Something there: not erased flash, not zeros */
static bool plausible(const uint8_t *p)
{
    bool ff = true, zero = true;
    for (int i = 0; i < 64; i++) {
        ff &= p[i] == 0xFF;
        zero &= p[i] == 0x00;
    }
    return !ff && !zero;
}

/* Through a mapping, not esp_partition_read(): that turns the caches off
 * while it reads, which stalls the LCD's refill (picture artefacts)
 */
static bool from_partition(uint32_t offset, uint32_t size, uint8_t *buf)
{
    const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ROM_PARTITION_TYPE,
                                                           "cpc_roms");
    if (part == NULL || offset + size > part->size) {
        return false;
    }
    const void *map;
    esp_partition_mmap_handle_t handle;
    if (esp_partition_mmap(part, offset, size, ESP_PARTITION_MMAP_DATA, &map, &handle) != ESP_OK) {
        return false;
    }
    memcpy(buf, map, size);
    esp_partition_munmap(handle);
    return true;
}

/* roms/<name> on the SD card (any case) */
static long from_sd(const char *name, uint8_t *buf, uint32_t max)
{
    char path[96];
    snprintf(path, sizeof(path), "%s/%s", ROM_FOLDER, name);
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        for (char *p = path + strlen(ROM_FOLDER) + 1; *p; p++) {
            *p = (*p >= 'a' && *p <= 'z') ? *p - 32 : *p;
        }
        f = fopen(path, "rb");
    }
    if (f == NULL) {
        return -1;
    }
    long n = fread(buf, 1, max, f);
    fclose(f);
    if (n > 0) {
        ESP_LOGI(TAG, "%s from the SD card", name);
    }
    return n;
}

bool rom_store_load(rom_id_t id, uint8_t *buf)
{
    if (id >= ROM_COUNT) {
        return false;
    }
    if (from_partition(id * CPC_ROM_SIZE, CPC_ROM_SIZE, buf) && plausible(buf)) {
        return true;
    }
    return from_sd(names[id], buf, CPC_ROM_SIZE) == CPC_ROM_SIZE && plausible(buf);
}

uint32_t rom_store_system_cart(uint8_t *pages, const char **name)
{
    uint8_t head[8];
    uint32_t size = 0;
    if (from_partition(CART_SLOT * CPC_ROM_SIZE, 8, head) && memcmp(head, "RIFF", 4) == 0) {
        uint32_t len = 8 + (head[4] | (head[5] << 8) | (head[6] << 16) | ((uint32_t)head[7] << 24));
        uint8_t *file = len <= CPC_CART_MAX + 4096 ? malloc(len) : NULL;
        if (file != NULL && from_partition(CART_SLOT * CPC_ROM_SIZE, len, file)) {
            size = cpc_cpr_pages(file, len, pages);
        }
        free(file);
    }
    if (size == 0) {
        uint8_t *file = malloc(CPC_CART_MAX + 4096);
        long n = file ? from_sd("system.cpr", file, CPC_CART_MAX + 4096) : -1;
        if (n > 0) {
            size = cpc_cpr_pages(file, n, pages);
        }
        free(file);
    }
    if (size) {
        *name = "system cartridge";
        return size;
    }
    memset(pages, 0xFF, 4 * CPC_ROM_SIZE);
    if (rom_store_load(ROM_OSPLUS, pages) && rom_store_load(ROM_BASICPLUS, pages + CPC_ROM_SIZE)) {
        rom_store_load(ROM_AMSDOS, pages + 3 * CPC_ROM_SIZE);
        *name = "OS and BASIC (Plus)";
        return 4 * CPC_ROM_SIZE;
    }
    return 0;
}
