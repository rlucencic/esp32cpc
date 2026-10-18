/*
 * The machine: memory map, I/O decoding, the PPI, interrupts, the frame
 * loop, and the Symbiface II clock.
 */
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "cpc_internal.h"
#include "emudbg.h"
#ifdef CPC_PROFILE
#include "esp_cpu.h"
#endif

cpc_t cpc;

static int s_carry;             /* us the last instruction ran into this frame */
static uint8_t s_cmos[64];      /* Symbiface II RTC RAM */

/* ---- Memory ---- */

/* 6128 RAM configurations: which 16 KB page sits at 0000, 4000, 8000, C000
 * (4-7: the selected 64 KB expansion bank)
 */
static const uint8_t ram_map[8][4] = {
    { 0, 1, 2, 3 }, { 0, 1, 2, 7 }, { 4, 5, 6, 7 }, { 0, 3, 2, 7 },
    { 0, 4, 2, 3 }, { 0, 5, 2, 3 }, { 0, 6, 2, 3 }, { 0, 7, 2, 3 },
};

static const uint8_t *cart_page(unsigned page)
{
    if (cpc.cart == NULL || cpc.cart_pages == 0) {
        return cpc.cfg.basic;
    }
    return cpc.cart + (page % cpc.cart_pages) * CPC_ROM_SIZE;
}

static const uint8_t *upper_rom(void)
{
    uint8_t n = cpc.upper_rom;
    if (n < CPC_ROM_SLOTS && n != 7 && cpc.slots[n] != NULL && (n != 0 || !cpc.plus)) {
        return cpc.slots[n];
    }
    if (cpc.plus) {
        /* ROM 7 is the cartridge's page 3 (AMSDOS), %1xxxxxxx a page of its
         * own, anything else page 1 (BASIC)
         */
        return cart_page((n & 0x80) ? (n & 31) : n == 7 ? 3 : 1);
    }
    if (n == 7 && cpc.cfg.amsdos != NULL) {
        return cpc.cfg.amsdos;
    }
    return cpc.cfg.basic;
}

void mem_update(void)
{
    uint8_t *ram = cpc.cfg.ram;
    uint32_t size = cpc.cfg.ram_size;
    uint8_t config = size > 0x10000 ? cpc.ram_config & 7 : 0;
    uint32_t bank = (cpc.ram_config >> 3) & 7;
    if (0x10000 + (bank + 1) * 0x10000 > size) {
        bank = 0;
    }
    for (int i = 0; i < 4; i++) {
        uint8_t page = ram_map[config][i];
        uint32_t offset = page < 4 ? page * 0x4000 : 0x10000 + bank * 0x10000 + (page - 4) * 0x4000;
        z80_rd[i] = z80_wr[i] = ram + offset;
    }
    if (!(cpc.rom_config & 0x04)) {
        if (cpc.plus) {
            int pos = (cpc.rmr2 >> 3) & 3;
            z80_rd[pos == 3 ? 0 : pos] = (uint8_t *)cart_page(cpc.rmr2 & 7);
        } else {
            z80_rd[0] = (uint8_t *)cpc.cfg.os;
        }
    }
    if (!(cpc.rom_config & 0x08)) {
        z80_rd[3] = (uint8_t *)upper_rom();
    }
    if (cpc.plus && (cpc.rmr2 & 0x18) == 0x18) {
        z80_rd[1] = asic.page;
        z80_wr[1] = NULL;       /* write_special() */
    }
}

static void write_special(uint16_t addr, uint8_t v)
{
    asic_write(addr, v);
}

uint8_t cpc_peek(uint16_t address)
{
    return z80_peek(address);
}

/* ---- Interrupts ---- */

void cpc_update_int(void)
{
    z80.int_line = video_ga_int() || (cpc.plus && asic_int_pending());
}

static uint8_t int_ack(void)
{
    uint8_t vector = 0xFF;
    if (cpc.plus && !video_ga_int() && asic_int_pending()) {
        vector = asic_int_ack(z80.im == 2);
    } else if (cpc.plus) {
        vector = (asic.ivr & 0xF8) | 6;
    }
    video_int_ack();
    cpc_update_int();
    return vector;
}

/* ---- Symbiface II real-time clock (DS1287 at &FD14/&FD15) ---- */

static uint8_t rtc_value(int v)
{
    bool binary = s_cmos[11] & 0x04;
    return binary ? v : ((v / 10) << 4) | (v % 10);
}

static uint8_t rtc_read(void)
{
    uint8_t reg = cpc.rtc_reg & 0x3F;
    if (reg >= 10) {
        return reg == 10 ? (s_cmos[10] & 0x7F) : reg == 12 ? 0 : reg == 13 ? 0x80 : s_cmos[reg];
    }
    time_t now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);
    switch (reg) {
    case 0: return rtc_value(t.tm_sec);
    case 2: return rtc_value(t.tm_min);
    case 4: {
        int h = t.tm_hour;
        if (!(s_cmos[11] & 0x02)) {         /* 12-hour clock, bit 7 = PM */
            bool pm = h >= 12;
            h = h % 12 ? h % 12 : 12;
            return rtc_value(h) | (pm ? 0x80 : 0);
        }
        return rtc_value(h);
    }
    case 6: return t.tm_wday + 1;
    case 7: return rtc_value(t.tm_mday);
    case 8: return rtc_value(t.tm_mon + 1);
    case 9: return rtc_value(t.tm_year % 100);
    default: return s_cmos[reg];            /* alarms */
    }
}

static void rtc_write(uint8_t v)
{
    uint8_t reg = cpc.rtc_reg & 0x3F;
    if (reg == 0 || reg == 2 || reg == 4 || (reg >= 6 && reg <= 9) || reg == 12 || reg == 13) {
        return;     /* the time comes from the host (the internet on the board) */
    }
    s_cmos[reg] = v;
}

/* ---- PPI and the AY behind it ---- */

static void ppi_port_c_changed(void)
{
    input_row_selected(cpc.ppi_c & 0x0F);
    psg_bus(cpc.ppi_c >> 6, cpc.ppi_a);
}

static void ppi_write(int port, uint8_t v)
{
    switch (port) {
    case 0:
        cpc.ppi_a = v;
        if (!(cpc.ppi_control & 0x10)) {
            psg_bus(cpc.ppi_c >> 6, v);
        }
        break;
    case 1:
        break;      /* port B is an input */
    case 2:
        cpc.ppi_c = v;
        ppi_port_c_changed();
        break;
    default:
        if (v & 0x80) {
            cpc.ppi_control = v;        /* mode set: outputs go to 0 */
            cpc.ppi_a = 0;
            cpc.ppi_c = 0;
        } else {
            int bit = (v >> 1) & 7;
            cpc.ppi_c = (v & 1) ? cpc.ppi_c | (1u << bit) : cpc.ppi_c & ~(1u << bit);
        }
        ppi_port_c_changed();
        break;
    }
}

static uint8_t ppi_read(int port)
{
    switch (port) {
    case 0:
        return (cpc.ppi_control & 0x10) ? psg_read() : cpc.ppi_a;
    case 1:
        /* tape in 0, printer busy, no /EXP, 50 Hz, Amstrad, VSYNC */
        return 0x5E | (video_vsync() ? 1 : 0);
    case 2:
        return cpc.ppi_c;
    default:
        return 0xFF;
    }
}

/* ---- I/O ---- */

static inline void io_sync(int at)
{
    if (at > cpc.io_at) {
        video_run(at - cpc.io_at);
        cpc.io_at = at;
    }
}

static void select_upper_rom(uint8_t v)
{
    cpc.upper_rom = v;
    mem_update();
}

static void CPC_HOT io_out(uint16_t port, uint8_t v, int at)
{
    io_sync(at);
    uint8_t hi = port >> 8;

    if ((port & 0xC000) == 0x4000 && (v & 0xC0) != 0xC0) {
        if (cpc.plus && asic.unlocked && (v & 0xE0) == 0xA0) {
            cpc.rmr2 = v;           /* Plus: lower ROM page/position, ASIC page */
            mem_update();
        } else {
            if ((v & 0xC0) == 0x80) {
                cpc.rom_config = v;
                mem_update();
            }
            ga_write(v);
        }
    }
    if (!(port & 0x8000) && (v & 0xC0) == 0xC0) {
        cpc.ram_config = v & 0x3F;
        mem_update();
    }
    if (!(port & 0x4000)) {
        switch (hi & 3) {
        case 0:
            if (cpc.plus) {
                asic_lock_byte(v);
            }
            crtc_select(v);
            break;
        case 1:
            crtc_write(v);
            break;
        default:
            break;
        }
    }
    if (!(port & 0x2000)) {
        select_upper_rom(v);
    }
    if (!(port & 0x0800)) {
        ppi_write(hi & 3, v);
    }
    if (!(port & 0x0480)) {
        if (!(port & 0x0100)) {
            upd765_motor(v & 1);
        } else if (port & 1) {
            upd765_write(v);
        }
    }
    if (hi == 0xFD) {
        if ((port & 0xFF) == 0x15) {
            cpc.rtc_reg = v;
        } else if ((port & 0xFF) == 0x14) {
            rtc_write(v);
        }
    }
}

static uint8_t CPC_HOT io_in(uint16_t port, int at)
{
    io_sync(at);
    uint8_t hi = port >> 8;
    uint8_t v = 0xFF;

    if (!(port & 0x4000) && (hi & 2)) {
        v &= crtc_read(hi & 3);
    }
    if (!(port & 0x0800)) {
        v &= ppi_read(hi & 3);
    }
    if (!(port & 0x0480) && (port & 0x0100)) {
        v &= (port & 1) ? upd765_read() : upd765_status();
    }
    if (hi == 0xFD) {
        uint8_t lo = port & 0xFF;
        if (lo == 0x10 || lo == 0x18) {
            v = input_sf2_mouse();
        } else if (lo == 0x14) {
            v = rtc_read();
        }
    }
    return v;
}

/* ---- Running ---- */

void cpc_reset(void)
{
    z80_reset();
    cpc.rom_config = 0;
    cpc.ram_config = 0;
    cpc.upper_rom = 0;
    cpc.rmr2 = 0;
    cpc.ppi_a = cpc.ppi_c = 0;
    cpc.ppi_control = 0x82;
    cpc.io_at = 0;
    s_carry = 0;
    cart_apply();
    asic_reset();
    video_reset();
    psg_reset();
    upd765_reset();
    input_reset();
    mem_update();
    cpc_update_int();
}

/* ---- The debugger (emudbg.h) ---- */

static emudbg_cpu_t s_dbg_cpus[] = {
    { .name = "Z80", .arch = &emudbg_arch_z80 },
};

static void dbg_where(char *buf, size_t len)
{
    snprintf(buf, len, "frame %lu", (unsigned long)cpc.frames);
}

static void dbg_screen(int *width, int *height)
{
    *width = CPC_SCREEN_WIDTH;
    *height = CPC_SCREEN_HEIGHT;
}

static emudbg_machine_t s_dbg = {
    .cpus = s_dbg_cpus,
    .cpu_count = 1,
    .where = dbg_where,
    .reset = cpc_reset,
    .screen = dbg_screen,
    .render_line = cpc_render_line_rgb565,
    .type = cpc_type,
};

bool cpc_init(const cpc_config_t *config)
{
    cpc_t old = cpc;
    memset(&cpc, 0, sizeof(cpc));
    cpc.cfg = *config;
    cpc.plus = cpc_model_is_plus(config->model);
    memcpy(cpc.slots, old.slots, sizeof(cpc.slots));
    if (config->ram == NULL || config->ram_size < 0x10000 || config->frame == NULL) {
        return false;
    }
    if (cpc.plus) {
        if (config->system_cart == NULL || config->system_cart_size < CPC_ROM_SIZE) {
            return false;
        }
        if (asic.page == NULL) {
            asic.page = cpc_malloc(CPC_ROM_SIZE);
            if (asic.page == NULL) {
                return false;
            }
        }
    } else if (config->os == NULL || config->basic == NULL) {
        return false;
    }
    memset(config->ram, 0, config->ram_size);
    s_cmos[10] = 0x26;
    s_cmos[11] = 0x02;
    static const z80_bus_t bus = {
        .in = io_in, .out = io_out, .int_ack = int_ack, .write_special = write_special,
    };
    z80_set_bus(&bus);
    upd765_set_creator("esp32cpc");
    audio_init();
    cpc_reset();
    s_dbg.name = cpc.plus ? "Amstrad CPC Plus" : "Amstrad CPC";
    emudbg_set_machine(&s_dbg);
    return true;
}

void CPC_HOT cpc_run_frame(void)
{
    if (cpc.reset_pending) {
        cpc.reset_pending = false;
        cpc_reset();
    }
    input_frame();
    int budget = CPC_FRAME_US - s_carry;
    while (budget > 0) {
        cpc.io_at = 0;
        int n = z80_step();
        if (n > cpc.io_at) {
            video_run(n - cpc.io_at);
        }
        budget -= n;
    }
    s_carry = -budget;
#ifdef CPC_PROFILE
    {
        extern uint32_t cpc_prof_video, cpc_prof_render;
        static uint32_t start, n;
        uint32_t now = esp_cpu_get_cycle_count();
        if (++n == 250) {
            CPC_LOG("per frame: %lu cycles, video %lu (render %lu)", (unsigned long)((now - start) / 250),
                    (unsigned long)(cpc_prof_video / 250), (unsigned long)(cpc_prof_render / 250));
            cpc_prof_video = cpc_prof_render = 0;
            n = 0;
            start = esp_cpu_get_cycle_count();
        }
    }
#endif
    audio_frame_end();
    upd765_frame();
    cpc.frames++;
}

void cpc_get_status(cpc_status_t *s)
{
    s->frames = cpc.frames;
    s->pc = z80.pc.w;
    s->plus = cpc.plus;
    s->asic_unlocked = asic.unlocked;
    s->mode = video_mode();
    video_crtc_regs(s->crtc);
}

cpc_model_t cpc_model(void)
{
    return cpc.cfg.model;
}
