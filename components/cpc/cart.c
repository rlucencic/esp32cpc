/*
 * The cartridge slot: a Plus cartridge (.cpr: RIFF "AMS!" with chunks
 * "cb00".."cb31" of up to 16 KB, one per page) or expansion ROMs for the
 * upper ROM slots, like a ROM board. Games converted to ROMs come in parts
 * (NAME_1.ROM, NAME_2.ROM): inserting one brings the others along.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "cpc_internal.h"

#define MAX_ROMS    8

static struct {
    char path[256];
    bool cpr;
    uint8_t *data;
    uint32_t size;              /* bytes (16 KB pages) */
    int count;                  /* expansion ROMs */
    char info[64];
} s_cart;

static uint8_t *read_file(const char *path, uint32_t *size, uint32_t max)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = NULL;
    if (n > 0 && (uint32_t)n <= max) {
        buf = cpc_malloc(n);
        if (buf != NULL && fread(buf, 1, n, f) != (size_t)n) {
            free(buf);
            buf = NULL;
        }
    }
    fclose(f);
    *size = n;
    return buf;
}

static uint32_t le32(const uint8_t *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

uint32_t cpc_cpr_pages(const uint8_t *file, uint32_t size, uint8_t *pages)
{
    if (size < 12 || memcmp(file, "RIFF", 4) != 0 || memcmp(file + 8, "AMS!", 4) != 0) {
        return 0;
    }
    memset(pages, 0xFF, CPC_CART_MAX);
    int highest = -1;
    uint32_t p = 12;
    while (p + 8 <= size) {
        uint32_t len = le32(file + p + 4);
        const uint8_t *id = file + p;
        if (p + 8 + len > size) {
            break;
        }
        if (id[0] == 'c' && id[1] == 'b' && isdigit(id[2]) && isdigit(id[3])) {
            int page = (id[2] - '0') * 10 + (id[3] - '0');
            if (page < 32) {
                memcpy(pages + page * CPC_ROM_SIZE, file + p + 8, len > CPC_ROM_SIZE ? CPC_ROM_SIZE : len);
                highest = page > highest ? page : highest;
            }
        }
        p += 8 + len + (len & 1);
    }
    return (highest + 1) * CPC_ROM_SIZE;
}

/* RIFF AMS! -> pages */
static bool load_cpr(const uint8_t *file, uint32_t size)
{
    if (size < 12 || memcmp(file, "RIFF", 4) != 0) {
        return false;
    }
    uint8_t *pages = cpc_malloc(CPC_CART_MAX);
    if (pages == NULL) {
        return false;
    }
    uint32_t bytes = cpc_cpr_pages(file, size, pages);
    if (bytes == 0) {
        free(pages);
        return false;
    }
    s_cart.cpr = true;
    s_cart.data = pages;
    s_cart.size = bytes;
    snprintf(s_cart.info, sizeof(s_cart.info), "Plus cartridge, %u KB", (unsigned)(s_cart.size / 1024));
    return true;
}

/* A 128-byte AMSDOS header: its checksum over bytes 0-66 is at 67 */
static bool amsdos_header(const uint8_t *p)
{
    unsigned sum = 0;
    for (int i = 0; i < 67; i++) {
        sum += p[i];
    }
    return (sum & 0xFFFF) == (unsigned)(p[67] | (p[68] << 8));
}

/* One ROM file into a 16 KB page */
static bool load_rom(const char *path, uint8_t *page)
{
    uint32_t size;
    uint8_t *file = read_file(path, &size, 64 * 1024);
    if (file == NULL) {
        return false;
    }
    const uint8_t *data = file;
    if (size >= 128 + 256 && amsdos_header(file)) {
        data += 128;
        size -= 128;
    }
    bool ok = size >= 256 && size <= CPC_ROM_SIZE && data[0] <= 2;   /* ROM type: fore/back/extension */
    if (ok) {
        memset(page, 0xFF, CPC_ROM_SIZE);
        memcpy(page, data, size);
    }
    free(file);
    return ok;
}

/* "path/NAME_1.ROM" -> prefix "path/NAME_", the digit's position */
static int part_position(const char *path)
{
    size_t len = strlen(path);
    if (len < 7 || strcasecmp(path + len - 4, ".rom") != 0) {
        return -1;
    }
    if (path[len - 6] == '_' && isdigit((unsigned char)path[len - 5])) {
        return len - 5;
    }
    return -1;
}

static bool load_roms(const char *path)
{
    uint8_t *pages = cpc_malloc(MAX_ROMS * CPC_ROM_SIZE);
    if (pages == NULL) {
        return false;
    }
    int count = 0;
    int at = part_position(path);
    if (at >= 0) {
        char name[256];
        if (strlen(path) >= sizeof(name)) {
            free(pages);
            return false;
        }
        strcpy(name, path);
        for (char d = '1'; d <= '9' && count < MAX_ROMS; d++) {
            name[at] = d;
            if (load_rom(name, pages + count * CPC_ROM_SIZE)) {
                count++;
            } else if (d > path[at]) {
                break;
            }
        }
    } else if (load_rom(path, pages)) {
        count = 1;
    }
    if (count == 0) {
        free(pages);
        return false;
    }
    s_cart.cpr = false;
    s_cart.data = pages;
    s_cart.size = count * CPC_ROM_SIZE;
    s_cart.count = count;
    return true;
}

bool cpc_cart_insert(const char *path)
{
    uint32_t size;
    uint8_t *file = read_file(path, &size, CPC_CART_MAX + 64 * 1024);
    if (file == NULL) {
        return false;
    }
    cpc_cart_eject();
    bool ok = load_cpr(file, size);
    free(file);
    if (!ok) {
        ok = load_roms(path);
    }
    if (!ok) {
        return false;
    }
    snprintf(s_cart.path, sizeof(s_cart.path), "%s", path);
    cpc.reset_pending = true;
    return true;
}

void cpc_cart_eject(void)
{
    if (s_cart.data == NULL) {
        return;
    }
    free(s_cart.data);
    memset(&s_cart, 0, sizeof(s_cart));
    memset(cpc.slots, 0, sizeof(cpc.slots));
    cpc.cart = NULL;
    cpc.reset_pending = true;
}

const char *cpc_cart_path(void)
{
    return s_cart.data ? s_cart.path : NULL;
}

const char *cpc_cart_info(void)
{
    return s_cart.data ? s_cart.info : "";
}

bool cpc_cart_is_plus(void)
{
    return s_cart.data != NULL && s_cart.cpr;
}

void cart_apply(void)
{
    memset(cpc.slots, 0, sizeof(cpc.slots));
    if (s_cart.data != NULL && !s_cart.cpr) {
        /* Slots 1-6 first (the 464 only starts ROMs 0-7), then 8-15 */
        static const uint8_t order[] = { 1, 2, 3, 4, 5, 6, 8, 9, 10, 11, 12, 13, 14, 15 };
        int first = order[0], last = first;
        for (int i = 0; i < s_cart.count && i < (int)sizeof(order); i++) {
            cpc.slots[order[i]] = s_cart.data + i * CPC_ROM_SIZE;
            last = order[i];
        }
        snprintf(s_cart.info, sizeof(s_cart.info), s_cart.count > 1 ? "ROMs in slots %d-%d" : "ROM in slot %d",
                 first, last);
    }
    if (cpc.plus) {
        if (s_cart.data != NULL && s_cart.cpr) {
            cpc.cart = s_cart.data;
            cpc.cart_pages = s_cart.size / CPC_ROM_SIZE;
        } else {
            cpc.cart = cpc.cfg.system_cart;
            cpc.cart_pages = cpc.cfg.system_cart_size / CPC_ROM_SIZE;
        }
    } else {
        cpc.cart = NULL;
        cpc.cart_pages = 0;
    }
}
