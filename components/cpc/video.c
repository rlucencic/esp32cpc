/*
 * Video: the 6845 CRTC's counters, the Gate Array (palette, modes,
 * interrupts), a monitor that locks onto HSYNC and VSYNC, and the Plus
 * ASIC's additions (sprites, split screen, soft scroll, raster interrupt).
 *
 * Time advances one CRTC character (1 us) at a time. Each character inside
 * the monitor's visible window is drawn into a line buffer with the
 * palette and mode of that moment, so mid-line colour changes, split
 * screens and overscan come out as on the real thing. At the next HSYNC
 * the line goes into the frame (only when it changed).
 */
#include <limits.h>
#include <string.h>
#include "cpc_internal.h"

#define WIN_CHARS       (CPC_SCREEN_WIDTH / 16)
#define WIN_LINES       CPC_SCREEN_HEIGHT
#define WIN_X           14      /* chars from HSYNC start to the window's left edge */
#define WIN_Y           20      /* lines from VSYNC start to the window's top */
#define FREE_RUN_X      52      /* no HSYNC by then: the monitor starts a line itself */
#define FREE_RUN_Y      340

static struct {
    /* CRTC */
    uint8_t reg[18];
    uint8_t sel;
    uint8_t hcc, vcc, ra, adj;
    bool in_adj;
    uint16_t ma_row, ma_next;
    bool hdisp, vdisp;
    bool hsync, vsync;
    uint8_t hsync_cnt, vsync_cnt;

    /* Gate Array */
    uint8_t pen;
    uint8_t ink[17];
    uint8_t mode, mode_req;
    uint8_t int_cnt;
    bool ga_int;
    uint8_t vs_delay;

    /* Monitor */
    int mon_x, mon_y;
    int mon_limit;              /* lines drawn: WIN_LINES, or 0 in a skipped picture */
    bool draw;
    int disp_x0;              /* pixel x of CRTC char 0 on this line, INT_MIN: none */
    uint16_t sl;                /* scanlines since the frame started (ASIC) */
    uint8_t prev[16];           /* pens of the previous char (soft scroll) */
    uint64_t clock;
    uint32_t signature;
} v;

/* Colours: 0-15 pens, 16 border, 17-31 Plus sprite colours 1-15 */
static uint16_t s_pal[32];
static uint32_t s_pal2[17];     /* pens and border, each colour twice */
static uint16_t s_line[CPC_SCREEN_WIDTH] __attribute__((aligned(4)));

static void set_pal(int i, uint16_t c)
{
    s_pal[i] = c;
    if (i < 17) {
        s_pal2[i] = c | ((uint32_t)c << 16);
    }
}
static uint32_t s_hash[WIN_LINES];

/* byte -> 8 pens at mode 2 resolution, 4 bits each, first pixel on top */
static uint32_t s_pens[4][256];

/* Hardware colours 0-31: R, G, B levels 0 (off), 1 (half), 2 (full) */
static const uint8_t hw_rgb[32][3] = {
    { 1, 1, 1 }, { 1, 1, 1 }, { 0, 2, 1 }, { 2, 2, 1 }, { 0, 0, 1 }, { 2, 0, 1 }, { 0, 1, 1 }, { 2, 1, 1 },
    { 2, 0, 1 }, { 2, 2, 1 }, { 2, 2, 0 }, { 2, 2, 2 }, { 2, 0, 0 }, { 2, 0, 2 }, { 2, 1, 0 }, { 2, 1, 2 },
    { 0, 0, 1 }, { 0, 2, 1 }, { 0, 2, 0 }, { 0, 2, 2 }, { 0, 0, 0 }, { 0, 0, 2 }, { 0, 1, 0 }, { 0, 1, 2 },
    { 1, 0, 1 }, { 1, 2, 1 }, { 1, 2, 0 }, { 1, 2, 2 }, { 1, 0, 0 }, { 1, 0, 2 }, { 1, 1, 0 }, { 1, 1, 2 },
};

static inline uint16_t rgb565(int r, int g, int b)
{
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

/* A Gate Array colour on the classic monitor */
static uint16_t hw_colour(uint8_t hw)
{
    static const uint8_t level[3] = { 0x00, 0x80, 0xFF };
    const uint8_t *c = hw_rgb[hw & 31];
    return rgb565(level[c[0]], level[c[1]], level[c[2]]);
}

/* ... and as the Plus ASIC stores it: 12-bit, page word %0000GGGG RRRRBBBB */
uint16_t asic_ga_colour_rgb565(uint8_t hw, uint16_t *page_word)
{
    static const uint8_t level4[3] = { 0x0, 0x6, 0xF };
    const uint8_t *c = hw_rgb[hw & 31];
    uint8_t r = level4[c[0]], g = level4[c[1]], b = level4[c[2]];
    *page_word = (g << 8) | (r << 4) | b;
    return rgb565(r * 17, g * 17, b * 17);
}

static void make_pen_tables(void)
{
    for (int b = 0; b < 256; b++) {
        uint32_t m0 = 0, m1 = 0, m2 = 0, m3 = 0;
        int p0[2] = {
            ((b >> 7) & 1) | ((b >> 2) & 2) | ((b >> 3) & 4) | ((b << 2) & 8),
            ((b >> 6) & 1) | ((b >> 1) & 2) | ((b >> 2) & 4) | ((b << 3) & 8),
        };
        for (int i = 0; i < 8; i++) {
            int shift = 28 - 4 * i;
            m2 |= (uint32_t)((b >> (7 - i)) & 1) << shift;
            int j = i / 2;
            m1 |= (uint32_t)(((b >> (7 - j)) & 1) | (((b >> (3 - j)) & 1) << 1)) << shift;
            m0 |= (uint32_t)p0[i / 4] << shift;
            m3 |= (uint32_t)(p0[i / 4] & 3) << shift;
        }
        s_pens[0][b] = m0;
        s_pens[1][b] = m1;
        s_pens[2][b] = m2;
        s_pens[3][b] = m3;
    }
}

#define R(n)    v.reg[n]

/* ---- Lines into the frame ---- */

/* FNV-1a over 32-bit words, two lanes. (Not rotate-xor: over a line of
 * equal words that cancels out, and a blue line looked like a black one.)
 */
static uint32_t hash_line(const uint16_t *line)
{
    const uint32_t *p = (const uint32_t *)line;
    uint32_t a = 2166136261u, b = 0x9E3779B9u;
    for (int i = 0; i < CPC_SCREEN_WIDTH / 2; i += 2) {
        a = (a ^ p[i]) * 16777619u;
        b = (b ^ p[i + 1]) * 2246822519u;
    }
    return a ^ (b >> 1);
}

static void commit(int y, const uint16_t *line)
{
    uint32_t h = hash_line(line);
    if (h != s_hash[y]) {
        s_hash[y] = h;
        memcpy(cpc.cfg.frame + y * CPC_SCREEN_WIDTH, line, sizeof(s_line));
        v.signature++;
    }
}

static void commit_black(int y)
{
    static const uint16_t black[CPC_SCREEN_WIDTH];
    commit(y, black);
}

/* Plus sprites over this line's screen area */
static void draw_sprites(void)
{
    int x0 = v.disp_x0;
    int xmin = x0 + (asic.extend_border ? 16 : 0), xmax = x0 + R(1) * 16;
    xmin = xmin < 0 ? 0 : xmin;
    xmax = xmax > CPC_SCREEN_WIDTH ? CPC_SCREEN_WIDTH : xmax;
    int line = v.sl;
    for (int s = 15; s >= 0; s--) {
        int mx = asic.spr_mx[s], my = asic.spr_my[s];
        if (mx == 0 || my == 0) {
            continue;
        }
        int row = line - asic.spr_y[s];
        if (row < 0 || row >= 16 * my) {
            continue;
        }
        const uint8_t *src = asic.sprite[s] + (row / my) * 16;
        int x = x0 + asic.spr_x[s];
        if (x >= xmax || x + 16 * mx <= xmin) {
            continue;
        }
        for (int c = 0; c < 16; c++, x += mx) {
            uint8_t p = src[c];
            if (p == 0) {
                continue;
            }
            uint16_t colour = s_pal[16 + p];
            for (int k = 0; k < mx; k++) {
                if (x + k >= xmin && x + k < xmax) {
                    s_line[x + k] = colour;
                }
            }
        }
    }
}

/* Every other picture is drawn: the board shows 25 a second at most, and
 * drawing is most of the video's work
 */
static void monitor_vsync(void)
{
    if (v.draw) {
        for (int y = v.mon_y < 0 ? 0 : v.mon_y; y < WIN_LINES; y++) {
            commit_black(y);    /* not reached this frame: the monitor's retrace */
        }
    }
    v.mon_y = -WIN_Y - 1;
    v.draw = !v.draw || cpc.draw_all;
    v.mon_limit = v.draw ? WIN_LINES : 0;
}

/* A new line on the monitor (HSYNC, or its own timing without one) */
static void monitor_line(void)
{
    if (v.mon_y >= 0 && v.mon_y < v.mon_limit) {
        if (cpc.plus && v.disp_x0 != INT_MIN) {
            draw_sprites();
        }
        commit(v.mon_y, s_line);
    }
    v.mon_y++;
    v.mon_x = -WIN_X;
    v.disp_x0 = INT_MIN;
    if (v.mon_y > FREE_RUN_Y) {
        monitor_vsync();
    }
}

/* ---- One character ---- */

static inline void fill16(uint16_t *dst, uint32_t c2)
{
    uint32_t *d = (uint32_t *)dst;
    d[0] = d[1] = d[2] = d[3] = d[4] = d[5] = d[6] = d[7] = c2;
}

/* One byte of screen memory as 8 pixels, two per 32-bit store */
static inline void pixels8(uint32_t *d, uint8_t mode, uint8_t byte)
{
    uint32_t w = s_pens[mode][byte];
    switch (mode) {
    case 1:
        d[0] = s_pal2[w >> 28];
        d[1] = s_pal2[(w >> 20) & 15];
        d[2] = s_pal2[(w >> 12) & 15];
        d[3] = s_pal2[(w >> 4) & 15];
        break;
    case 2:
        d[0] = s_pal[w >> 28] | (s_pal[(w >> 24) & 15] << 16);
        d[1] = s_pal[(w >> 20) & 15] | (s_pal[(w >> 16) & 15] << 16);
        d[2] = s_pal[(w >> 12) & 15] | (s_pal[(w >> 8) & 15] << 16);
        d[3] = s_pal[(w >> 4) & 15] | (s_pal[w & 15] << 16);
        break;
    default:    /* 0 and 3: two wide pixels */
        d[0] = d[1] = s_pal2[w >> 28];
        d[2] = d[3] = s_pal2[(w >> 12) & 15];
        break;
    }
}

static void CPC_HOT render_char(void)
{
    uint16_t *dst = &s_line[v.mon_x * 16];
    if (v.hsync || v.vsync) {
        fill16(dst, 0);
        return;
    }
    if (!v.hdisp || !v.vdisp) {
        fill16(dst, s_pal2[16]);
        if (cpc.plus) {
            memset(v.prev, 16, sizeof(v.prev));
        }
        return;
    }
    if (v.disp_x0 == INT_MIN) {
        v.disp_x0 = (v.mon_x - v.hcc) * 16;
    }
    unsigned ra = v.ra;
    uint16_t ma = v.ma_row + v.hcc;
    if (cpc.plus && asic.vscroll) {
        ra += asic.vscroll;
        if (ra > R(9)) {
            ra -= R(9) + 1;
            ma += R(1);
        }
    }
    uint32_t addr = ((ma & 0x3000) << 2) | ((ra & 7) << 11) | ((ma & 0x3FF) << 1);
    const uint8_t *ram = cpc.cfg.ram;
    if (!cpc.plus || (asic.hscroll == 0 && !asic.extend_border)) {
        pixels8((uint32_t *)dst, v.mode, ram[addr]);
        pixels8((uint32_t *)dst + 4, v.mode, ram[addr + 1]);
        return;
    }
    /* Plus soft scroll: the pixels come late by hscroll */
    uint32_t w0 = s_pens[v.mode][ram[addr]];
    uint32_t w1 = s_pens[v.mode][ram[addr + 1]];
    uint8_t pens[32];
    memcpy(pens, v.prev, 16);
    for (int i = 0; i < 8; i++) {
        pens[16 + i] = (w0 >> (28 - 4 * i)) & 15;
        pens[24 + i] = (w1 >> (28 - 4 * i)) & 15;
    }
    memcpy(v.prev, pens + 16, 16);
    if (asic.extend_border && v.hcc == 0) {
        fill16(dst, s_pal2[16]);
        return;
    }
    const uint8_t *p = pens + 16 - asic.hscroll;
    for (int i = 0; i < 16; i++) {
        dst[i] = s_pal[p[i]];
    }
}

/* ---- CRTC events ---- */

static void raise_ga_int(void)
{
    if (cpc.plus && asic.pri) {
        return;         /* the programmable raster interrupt replaces it */
    }
    v.ga_int = true;
    cpc_update_int();
}

static void hsync_start(void)
{
    v.hsync = true;
    v.hsync_cnt = 0;
    monitor_line();
    if (cpc.plus) {
        asic_dma();
    }
}

static void hsync_end(void)
{
    v.hsync = false;
    v.mode = v.mode_req;
    if (cpc.plus && asic.pri && v.sl == asic.pri) {
        asic_raster_int();
    }
    if (++v.int_cnt >= 52) {
        v.int_cnt = 0;
        raise_ga_int();
    }
    if (v.vs_delay && --v.vs_delay == 0) {
        /* two HSYNCs into VSYNC the counter restarts, with an interrupt
         * unless one came less than 32 lines ago
         */
        if (v.int_cnt >= 32) {
            raise_ga_int();
        }
        v.int_cnt = 0;
    }
}

static void vsync_start(void)
{
    v.vsync = true;
    v.vsync_cnt = 0;
    v.vs_delay = 2;
    monitor_vsync();
}

/* A character row starts (or the frame) */
static void row_start(void)
{
    if (v.vcc == R(6)) {
        v.vdisp = false;
    }
    if (v.vcc == R(7) && !v.vsync) {
        vsync_start();
    }
}

static void new_frame(void)
{
    v.vcc = 0;
    v.ra = 0;
    v.in_adj = false;
    v.adj = 0;
    v.ma_row = v.ma_next = ((R(12) << 8) | R(13)) & 0x3FFF;
    v.vdisp = true;
    v.sl = 0;
    row_start();
}

/* hcc wrapped to 0: the next scanline */
static void line_end(void)
{
    bool split = cpc.plus && asic.sssl && v.sl == asic.sssl;
    v.sl++;
    if (v.vsync) {
        int width = R(3) >> 4;
        if (++v.vsync_cnt >= (width ? width : 16)) {
            v.vsync = false;
        }
    }
    if (v.in_adj) {
        v.ra = (v.ra + 1) & 31;
        if (++v.adj >= R(5)) {
            new_frame();
        }
    } else if (v.ra == R(9)) {
        v.ra = 0;
        v.ma_row = v.ma_next;
        if (v.vcc == R(4)) {
            if (R(5)) {
                v.in_adj = true;
                v.adj = 0;
                v.vcc = (v.vcc + 1) & 127;
            } else {
                new_frame();
            }
        } else {
            v.vcc = (v.vcc + 1) & 127;
            row_start();
        }
    } else {
        v.ra = (v.ra + 1) & 31;
    }
    if (split) {
        v.ma_row = v.ma_next = asic.ssa;
    }
    v.hdisp = R(1) != 0;
}

static inline void step_char(void)
{
    v.mon_x++;
    if (v.hcc == R(0)) {
        v.hcc = 0;
        line_end();
    } else {
        v.hcc++;
    }
    if (v.hcc == R(1)) {
        v.hdisp = false;
        if (v.ra == R(9)) {
            v.ma_next = v.ma_row + R(1);
        }
    }
    if (v.hsync) {
        int width = R(3) & 15;
        if (++v.hsync_cnt >= (width ? width : 16)) {
            hsync_end();
        }
    } else if (v.hcc == R(2)) {
        hsync_start();
    }
    if (v.mon_x >= FREE_RUN_X) {
        monitor_line();
    }
}

/* How many characters from now on only move the counters: no line end, no
 * R1, no HSYNC start or end, no monitor line
 */
static inline int quiet_chars(void)
{
    if (v.hcc >= R(0)) {
        return 0;
    }
    int q = FREE_RUN_X - 1 - v.mon_x;
    int d = R(0) - v.hcc;
    q = d < q ? d : q;
    if (R(1) > v.hcc) {
        d = R(1) - v.hcc - 1;
        q = d < q ? d : q;
    }
    if (v.hsync) {
        int width = R(3) & 15;
        d = (width ? width : 16) - v.hsync_cnt - 1;
        q = d < q ? d : q;
    } else if (R(2) > v.hcc) {
        d = R(2) - v.hcc - 1;
        q = d < q ? d : q;
    }
    return q < 0 ? 0 : q;
}

/* Draw n characters from the current position (the visible ones) */
static void render_span(int n)
{
    uint8_t hcc = v.hcc;
    int mon_x = v.mon_x;
    int from = mon_x < 0 ? -mon_x : 0;
    int to = mon_x + n > WIN_CHARS ? WIN_CHARS - mon_x : n;
    for (int i = from; i < to; i++) {
        v.hcc = hcc + i;
        v.mon_x = mon_x + i;
        render_char();
    }
    v.hcc = hcc;
    v.mon_x = mon_x;
}

#ifdef CPC_PROFILE
#include "esp_cpu.h"
uint32_t cpc_prof_video, cpc_prof_render;
#endif

void CPC_HOT video_run(int us)
{
#ifdef CPC_PROFILE
    uint32_t t0 = esp_cpu_get_cycle_count();
#endif
    while (us > 0) {
        int q = quiet_chars();
        if (q > 0) {
            q = q < us ? q : us;
            if ((unsigned)v.mon_y < (unsigned)v.mon_limit) {
                render_span(q);
            }
            v.hcc += q;
            v.mon_x += q;
            if (v.hsync) {
                v.hsync_cnt += q;
            }
            v.clock += q;
            us -= q;
            if (us == 0) {
                break;
            }
        }
        /* a character where something happens */
        if ((unsigned)v.mon_y < (unsigned)v.mon_limit && (unsigned)v.mon_x < WIN_CHARS) {
            render_char();
        }
        step_char();
        v.clock++;
        us--;
    }
#ifdef CPC_PROFILE
    cpc_prof_video += esp_cpu_get_cycle_count() - t0;
#endif
}

uint64_t video_clock(void)
{
    return v.clock;
}

/* ---- Registers ---- */

void ga_write(uint8_t val)
{
    switch (val >> 6) {
    case 0:
        v.pen = (val & 0x10) ? 16 : (val & 0x0F);
        break;
    case 1: {
        uint8_t hw = val & 0x1F;
        v.ink[v.pen] = hw;
        if (cpc.plus) {
            uint16_t word;
            set_pal(v.pen, asic_ga_colour_rgb565(hw, &word));
            asic.page[0x2400 + v.pen * 2] = word;
            asic.page[0x2400 + v.pen * 2 + 1] = word >> 8;
        } else {
            set_pal(v.pen, hw_colour(hw));
        }
        break;
    }
    case 2:
        v.mode_req = val & 3;
        if (val & 0x10) {
            v.int_cnt = 0;
            v.ga_int = false;
            cpc_update_int();
        }
        break;
    default:
        break;
    }
}

void video_set_pen_rgb(int pen, uint16_t rgb)
{
    set_pal(pen & 31, rgb);
}

void crtc_select(uint8_t val)
{
    v.sel = val & 31;
}

void crtc_write(uint8_t val)
{
    static const uint8_t mask[16] = {
        0xFF, 0xFF, 0xFF, 0xFF, 0x7F, 0x1F, 0x7F, 0x7F, 0xFF, 0x1F, 0x7F, 0x1F, 0x3F, 0xFF, 0x3F, 0xFF,
    };
    if (v.sel < 16) {
        v.reg[v.sel] = val & mask[v.sel];
    }
}

uint8_t crtc_read(int port)
{
    if (port == 3 && v.sel >= 12 && v.sel <= 17) {
        return v.reg[v.sel];
    }
    return port == 3 ? 0 : 0xFF;
}

bool video_vsync(void)
{
    return v.vsync;
}

bool video_ga_int(void)
{
    return v.ga_int;
}

void video_int_ack(void)
{
    v.ga_int = false;
    v.int_cnt &= 0x1F;
}

uint8_t video_mode(void)
{
    return v.mode;
}

void video_crtc_regs(uint8_t *regs)
{
    memcpy(regs, v.reg, 18);
}

uint32_t video_signature(void)
{
    return v.signature;
}

void video_reset(void)
{
    static bool tables;
    if (!tables) {
        make_pen_tables();
        tables = true;
    }
    uint64_t clock = v.clock;
    uint32_t signature = v.signature;
    memset(&v, 0, sizeof(v));
    v.clock = clock;
    v.signature = signature + 1;
    /* Until the firmware programs it: a usual 64 x 312 frame */
    static const uint8_t init[16] = { 63, 40, 46, 0x8E, 38, 0, 25, 30, 0, 7, 0, 0, 0x30, 0, 0, 0 };
    memcpy(v.reg, init, sizeof(init));
    v.hdisp = v.vdisp = true;
    v.mon_y = -WIN_Y;
    v.draw = true;
    v.mon_limit = WIN_LINES;
    v.disp_x0 = INT_MIN;
    memset(s_pal, 0, sizeof(s_pal));
    memset(s_pal2, 0, sizeof(s_pal2));
    memset(s_hash, 0xA5, sizeof(s_hash));
    new_frame();
}

/* ---- Public ---- */

void cpc_render_line_rgb565(int y, uint16_t *dst)
{
    if (y >= 0 && y < CPC_SCREEN_HEIGHT && cpc.cfg.frame != NULL) {
        memcpy(dst, cpc.cfg.frame + y * CPC_SCREEN_WIDTH, CPC_SCREEN_WIDTH * sizeof(uint16_t));
    }
}

uint32_t cpc_screen_signature(void)
{
    return v.signature;
}

void cpc_draw_every_frame(bool all)
{
    cpc.draw_all = all;
}
