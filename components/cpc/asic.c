/*
 * The Plus ASIC: unlocking, the register page at &4000-&7FFF (sprites,
 * 12-bit palette, raster interrupt, split screen, soft scroll, interrupt
 * vector, DMA sound) and its interrupts.
 *
 *   &4000-&4FFF  sprite pixels: sprite n at &4000 + n * 256, 16 x 16, 4 bits
 *   &6000-&607F  sprite n at &6000 + n * 8: X (2 bytes), Y (2), magnification
 *   &6400-&643F  palette: pens 0-15, border, sprite colours 1-15;
 *                %RRRRBBBB, %0000GGGG
 *   &6800 PRI    raster interrupt scanline (0: the Gate Array's interrupts)
 *   &6801 SSSL   split screen after this scanline, &6802-3 SSA its address
 *   &6804 SSCR   soft scroll: bit 7 wider left border, 6-4 lines, 3-0 pixels
 *   &6805 IVR    IM 2 vector (bits 7-3); bit 0 = 0: DMA interrupts clear
 *                themselves when taken
 *   &6C00-&6C0B  DMA channels 0-2: address (2 bytes), prescaler
 *   &6C0F DCSR   DMA enables (bits 0-2) / interrupt flags (6-4, 7 raster)
 */
#include <string.h>
#include "cpc_internal.h"

asic_t asic;

static const uint8_t unlock_seq[] = {
    0x00, 0x00, 0xFF, 0x77, 0xB3, 0x51, 0xA8, 0xD4, 0x62, 0x39, 0x9C, 0x46, 0x2B, 0x15, 0x8A, 0xCD,
};

void asic_reset(void)
{
    uint8_t *page = asic.page;
    memset(&asic, 0, sizeof(asic));
    asic.page = page;
    asic.ivr = 1;
    if (page != NULL) {
        memset(page, 0, CPC_ROM_SIZE);
        memset(page + 0x2808, 0x3F, 8);     /* analogue joysticks: centred */
    }
}

/* Written to the CRTC's register select port: after a non-zero byte, the
 * 15 bytes 00 FF 77 ... CD; then any byte unlocks. A different 16th byte
 * locks it again.
 */
void asic_lock_byte(uint8_t val)
{
    const int len = sizeof(unlock_seq);
    if (asic.lock_pos == 0) {
        if (val) {
            asic.lock_pos = 1;
        }
    } else if (asic.lock_pos < len) {
        if (val == unlock_seq[asic.lock_pos]) {
            asic.lock_pos++;
        } else {
            if (asic.lock_pos == len - 1) {
                asic.unlocked = false;
            }
            asic.lock_pos = val ? 1 : 2;
        }
    } else {
        asic.unlocked = true;
        asic.lock_pos = val ? 1 : 0;
    }
}

static void update_dcsr(void)
{
    uint8_t d = 0;
    for (int c = 0; c < 3; c++) {
        d |= asic.dma[c].enabled ? 1u << c : 0;
        d |= asic.dma[c].int_pending ? 0x40u >> c : 0;
    }
    d |= asic.raster_int ? 0x80 : 0;
    asic.page[0x2C0F] = d;
}

static inline uint16_t rgb12_to_565(uint8_t rb, uint8_t g)
{
    uint8_t r = rb >> 4, b = rb & 15;
    g &= 15;
    return (r << 12) | ((r >> 3) << 11) | (g << 7) | ((g >> 2) << 5) | (b << 1) | (b >> 3);
}

void asic_write(uint16_t addr, uint8_t val)
{
    uint16_t a = addr & 0x3FFF;
    if (a < 0x1000) {
        int s = a >> 8;
        asic.sprite[s][a & 0xFF] = val & 15;
        asic.page[a] = val & 15;
        return;
    }
    if (a >= 0x2000 && a < 0x2080) {
        int s = (a >> 3) & 15;
        switch (a & 7) {
        case 0: case 1: {
            asic.page[a] = asic.page[a + 4] = val;
            uint16_t x = (asic.page[0x2000 + s * 8] | (asic.page[0x2001 + s * 8] << 8)) & 0x3FF;
            asic.spr_x[s] = x > 767 ? (int16_t)(x | 0xFC00) : (int16_t)x;
            break;
        }
        case 2: case 3: {
            asic.page[a] = asic.page[a + 4] = val;
            uint16_t y = (asic.page[0x2002 + s * 8] | (asic.page[0x2003 + s * 8] << 8)) & 0x1FF;
            asic.spr_y[s] = y > 255 ? (int16_t)(y | 0xFE00) : (int16_t)y;
            break;
        }
        case 4: {
            static const uint8_t mag[4] = { 0, 1, 2, 4 };
            asic.spr_mx[s] = mag[(val >> 2) & 3];
            asic.spr_my[s] = mag[val & 3];
            break;
        }
        default:
            break;
        }
        return;
    }
    if (a >= 0x2400 && a < 0x2440) {
        asic.page[a] = (a & 1) ? val & 15 : val;
        int colour = (a & 0x3F) >> 1;
        uint16_t base = 0x2400 + colour * 2;
        video_set_pen_rgb(colour, rgb12_to_565(asic.page[base], asic.page[base + 1]));
        return;
    }
    switch (a) {
    case 0x2800:
        asic.pri = val;
        break;
    case 0x2801:
        asic.sssl = val;
        break;
    case 0x2802:
        asic.ssa = (asic.ssa & 0x00FF) | ((val & 0x3F) << 8);
        break;
    case 0x2803:
        asic.ssa = (asic.ssa & 0x3F00) | val;
        break;
    case 0x2804:
        asic.hscroll = val & 15;
        asic.vscroll = (val >> 4) & 7;
        asic.extend_border = val & 0x80;
        break;
    case 0x2805:
        asic.ivr = val & 0xF9;
        break;
    case 0x2C0F:
        for (int c = 0; c < 3; c++) {
            bool on = val & (1u << c);
            if (on && !asic.dma[c].enabled) {
                asic.dma[c].pause = 0;
                asic.dma[c].tick = 0;
            }
            asic.dma[c].enabled = on;
            if (val & (0x40u >> c)) {
                asic.dma[c].int_pending = false;
            }
        }
        if (val & 0x80) {
            asic.raster_int = false;
        }
        update_dcsr();
        cpc_update_int();
        return;
    default:
        if (a >= 0x2C00 && a < 0x2C0C) {
            int c = (a >> 2) & 3;
            switch (a & 3) {
            case 0: asic.dma[c].addr = (asic.dma[c].addr & 0xFF00) | (val & 0xFE); break;
            case 1: asic.dma[c].addr = (asic.dma[c].addr & 0x00FF) | (val << 8); break;
            case 2: asic.dma[c].prescaler = val; break;
            default: break;
            }
        }
        break;
    }
    asic.page[a] = val;
}

/* One instruction per enabled channel at each HSYNC; RAM reads see the
 * base 64 KB
 */
void asic_dma(void)
{
    bool changed = false;
    const uint8_t *ram = cpc.cfg.ram;
    for (int c = 0; c < 3; c++) {
        typeof(asic.dma[0]) *ch = &asic.dma[c];
        if (!ch->enabled) {
            continue;
        }
        if (ch->pause) {
            if (ch->tick < ch->prescaler) {
                ch->tick++;
                continue;
            }
            ch->tick = 0;
            ch->pause--;
            continue;
        }
        uint16_t op = ram[ch->addr] | (ram[(uint16_t)(ch->addr + 1)] << 8);
        ch->addr += 2;
        switch ((op >> 12) & 7) {
        case 0:
            psg_write_reg((op >> 8) & 15, op & 0xFF);
            break;
        default:
            if (op & 0x1000) {                  /* PAUSE n */
                ch->pause = op & 0x0FFF;
                ch->tick = 0;
            }
            if (op & 0x2000) {                  /* REPEAT n */
                ch->loops = op & 0x0FFF;
                ch->loop_addr = ch->addr;
            }
            if (op & 0x4000) {
                if ((op & 0x0001) && ch->loops > 0) {  /* LOOP */
                    ch->loops--;
                    ch->addr = ch->loop_addr;
                }
                if (op & 0x0010) {              /* INT */
                    ch->int_pending = true;
                    changed = true;
                }
                if (op & 0x0020) {              /* STOP */
                    ch->enabled = false;
                    changed = true;
                }
            }
            break;
        }
        asic.page[0x2C00 + c * 4] = ch->addr;
        asic.page[0x2C01 + c * 4] = ch->addr >> 8;
    }
    if (changed) {
        update_dcsr();
        cpc_update_int();
    }
}

void asic_raster_int(void)
{
    asic.raster_int = true;
    update_dcsr();
    cpc_update_int();
}

bool asic_int_pending(void)
{
    return asic.raster_int || asic.dma[0].int_pending || asic.dma[1].int_pending ||
           asic.dma[2].int_pending;
}

/* Priority: raster, DMA 2, DMA 1, DMA 0 (source ids 6, 0, 2, 4) */
uint8_t asic_int_ack(bool im2)
{
    uint8_t vector;
    if (asic.raster_int) {
        asic.raster_int = false;
        vector = (asic.ivr & 0xF8) | 6;
    } else {
        int c = asic.dma[2].int_pending ? 2 : asic.dma[1].int_pending ? 1 : 0;
        vector = (asic.ivr & 0xF8) | ((2 - c) * 2);
        if (im2 && !(asic.ivr & 1)) {
            asic.dma[c].int_pending = false;
        }
    }
    update_dcsr();
    return vector;
}
