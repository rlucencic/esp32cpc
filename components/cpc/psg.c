/*
 * AY-3-8912 and the sound output.
 *
 * The CPU reaches the AY through the PPI: port A carries the data, port C
 * bits 7-6 the bus control (BDIR, BC1). Register 14 is the AY's I/O port,
 * which reads the keyboard line the PPI selects.
 *
 * Like the Atari's YM2149 (../esp32ste), sound register writes become
 * events stamped with their time in output samples; the render side (the
 * speaker task) replays them into its own copy of the chip. The Plus DMA
 * channels write the AY the same way.
 *
 * The chip runs from 1 MHz; everything inside advances in ticks of 8 us
 * (125 kHz), four per output sample. The envelope's 16 steps take two
 * ticks of the period each; it runs as 32 half steps here.
 */
#include <stdlib.h>
#include <string.h>
#include "cpc_internal.h"

#define TICKS_PER_SAMPLE    4           /* 125 kHz / CPC_AUDIO_HZ */
#define US_PER_SAMPLE       32
#define EVENT_QUEUE         8192        /* power of two */

static const uint8_t reg_mask[16] = {
    0xFF, 0x0F, 0xFF, 0x0F, 0xFF, 0x0F, 0x1F, 0xFF,
    0x1F, 0x1F, 0x1F, 0xFF, 0xFF, 0x0F, 0xFF, 0xFF,
};

/* ---- Emulation side ---- */

typedef struct {
    uint64_t time;                      /* output sample */
    uint8_t reg, value;
} audio_event_t;

static struct {
    uint8_t reg[16];
    uint8_t sel;
    uint8_t control;
    bool enabled;
    audio_event_t *queue;
    volatile uint32_t head, tail;
    volatile uint64_t now;              /* samples the render side may produce */
    uint32_t dropped;
} ay;

static void push_event(uint8_t reg, uint8_t value)
{
    uint64_t t = video_clock() / US_PER_SAMPLE;
    if (t > ay.now) {
        __atomic_store_n(&ay.now, t, __ATOMIC_RELEASE);
    }
    if (!ay.enabled || ay.queue == NULL) {
        return;
    }
    uint32_t head = ay.head;
    if (head - __atomic_load_n(&ay.tail, __ATOMIC_ACQUIRE) >= EVENT_QUEUE) {
        ay.dropped++;
        return;
    }
    audio_event_t *e = &ay.queue[head % EVENT_QUEUE];
    e->time = t;
    e->reg = reg;
    e->value = value;
    __atomic_store_n(&ay.head, head + 1, __ATOMIC_RELEASE);
}

void psg_write_reg(uint8_t reg, uint8_t v)
{
    reg &= 15;
    ay.reg[reg] = v & reg_mask[reg];
    if (reg < 14) {
        push_event(reg, ay.reg[reg]);
    }
}

void psg_bus(uint8_t control, uint8_t data)
{
    ay.control = control & 3;
    switch (ay.control) {
    case 2:
        psg_write_reg(ay.sel, data);
        break;
    case 3:
        if (data < 16) {
            ay.sel = data;
        }
        break;
    default:
        break;
    }
}

uint8_t psg_read(void)
{
    if (ay.control != 1) {
        return 0xFF;
    }
    if (ay.sel == 14) {
        uint8_t keys = input_row(cpc.ppi_c & 0x0F);
        return (ay.reg[7] & 0x40) ? ay.reg[14] & keys : keys;
    }
    if (ay.sel == 15) {
        return (ay.reg[7] & 0x80) ? ay.reg[15] : 0xFF;
    }
    return ay.reg[ay.sel];
}

void psg_reset(void)
{
    for (int r = 0; r < 14; r++) {
        psg_write_reg(r, r == 7 ? 0x3F : 0);
    }
    ay.reg[14] = ay.reg[15] = 0xFF;
    ay.sel = 0;
    ay.control = 0;
}

void audio_frame_end(void)
{
    uint64_t t = video_clock() / US_PER_SAMPLE;
    if (t > ay.now) {
        __atomic_store_n(&ay.now, t, __ATOMIC_RELEASE);
    }
}

/* ---- Render side ---- */

static struct {
    uint8_t reg[14];
    uint16_t tone_count[3];
    uint8_t tone_out[3];
    uint16_t noise_count;
    uint32_t lfsr;
    uint8_t noise_out;
    uint32_t env_count;
    uint8_t env_pos;            /* 0..31 half steps */
    uint8_t env_attack, env_holding, env_hold_value;
    int32_t dc_l, dc_r;
    uint64_t pos;               /* samples rendered */
} ren = { .lfsr = 1 };

/* AY 4-bit DAC, full scale 8191 per channel */
static const uint16_t ay_level[16] = {
    0, 104, 151, 222, 327, 484, 675, 1103, 1299, 2088, 2917, 3661, 4621, 5802, 6899, 8191,
};

static void synth_write(uint8_t reg, uint8_t value)
{
    ren.reg[reg] = value;
    if (reg == 13) {
        ren.env_count = 0;
        ren.env_pos = 0;
        ren.env_attack = (value & 0x04) != 0;
        ren.env_holding = 0;
    }
}

static inline uint8_t env_level(void)
{
    uint8_t p = ren.env_holding ? ren.env_hold_value : ren.env_attack ? ren.env_pos : 31 - ren.env_pos;
    return p >> 1;
}

static inline void env_advance(uint32_t ticks)
{
    if (ren.env_holding) {
        return;
    }
    uint32_t ep = ren.reg[11] | (ren.reg[12] << 8);
    ep = ep ? ep : 1;
    ren.env_count += ticks;
    while (ren.env_count >= ep && !ren.env_holding) {
        ren.env_count -= ep;
        if (++ren.env_pos > 31) {
            uint8_t shape = ren.reg[13];
            bool cont = shape & 0x08, att = shape & 0x04, alt = shape & 0x02, hold = shape & 0x01;
            ren.env_pos = 0;
            if (!cont) {
                ren.env_holding = 1;
                ren.env_hold_value = 0;
            } else if (hold) {
                ren.env_holding = 1;
                ren.env_hold_value = (alt ^ att) ? 31 : 0;
            } else if (alt) {
                ren.env_attack ^= 1;
            }
        }
    }
}

static inline void noise_advance(uint32_t ticks)
{
    uint32_t np = ren.reg[6] ? ren.reg[6] * 2 : 2;
    ren.noise_count += ticks;
    while (ren.noise_count >= np) {
        ren.noise_count -= np;
        uint32_t bit = (ren.lfsr ^ (ren.lfsr >> 3)) & 1;
        ren.lfsr = (ren.lfsr >> 1) | (bit << 16);
    }
    ren.noise_out = ren.lfsr & 1;
}

/* Ticks the tone output is high during the next `ticks` */
static inline uint32_t tone_advance(int ch, uint32_t ticks)
{
    uint32_t period = ren.reg[ch * 2] | (ren.reg[ch * 2 + 1] << 8);
    period = period ? period : 1;
    uint32_t count = ren.tone_count[ch];
    if (count >= period) {
        count = period - 1;
    }
    uint8_t out = ren.tone_out[ch];
    uint32_t high = 0;
    while (ticks) {
        uint32_t to_toggle = period - count;
        if (to_toggle > ticks) {
            high += out ? ticks : 0;
            count += ticks;
            break;
        }
        high += out ? to_toggle : 0;
        ticks -= to_toggle;
        count = 0;
        out ^= 1;
    }
    ren.tone_count[ch] = count;
    ren.tone_out[ch] = out;
    return high;
}

static inline int16_t clip16(int32_t v)
{
    return v < -32768 ? -32768 : v > 32767 ? 32767 : v;
}

static void CPC_HOT render_span(int16_t *dst, int n)
{
    uint8_t mixer = ren.reg[7];
    for (int i = 0; i < n; i++) {
        env_advance(TICKS_PER_SAMPLE);
        noise_advance(TICKS_PER_SAMPLE);
        uint8_t env = env_level();
        int32_t ch[3];
        for (int c = 0; c < 3; c++) {
            bool tone_off = (mixer >> c) & 1;
            bool noise_off = (mixer >> (c + 3)) & 1;
            uint32_t high = tone_off ? TICKS_PER_SAMPLE : tone_advance(c, TICKS_PER_SAMPLE);
            ch[c] = 0;
            if ((noise_off || ren.noise_out) && high) {
                uint8_t vol = ren.reg[8 + c];
                ch[c] = ay_level[(vol & 0x10) ? env : vol & 15] * high / TICKS_PER_SAMPLE;
            }
        }
        /* A left, C right, B in the middle */
        int32_t l = (ch[0] + ch[1] / 2) * 2;
        int32_t r = (ch[2] + ch[1] / 2) * 2;
        ren.dc_l += ((l << 8) - ren.dc_l) >> 10;
        ren.dc_r += ((r << 8) - ren.dc_r) >> 10;
        *dst++ = clip16(l - (ren.dc_l >> 8));
        *dst++ = clip16(r - (ren.dc_r >> 8));
    }
}

int CPC_HOT cpc_audio_render(int16_t *dst, int frames)
{
    uint64_t limit = __atomic_load_n(&ay.now, __ATOMIC_ACQUIRE);
    if (ren.pos + frames > limit) {
        frames = limit > ren.pos ? limit - ren.pos : 0;
    }
    int done = 0;
    while (done < frames) {
        uint32_t tail = ay.tail;
        uint32_t head = __atomic_load_n(&ay.head, __ATOMIC_ACQUIRE);
        while (tail != head && ay.queue[tail % EVENT_QUEUE].time <= ren.pos) {
            synth_write(ay.queue[tail % EVENT_QUEUE].reg, ay.queue[tail % EVENT_QUEUE].value);
            tail++;
        }
        __atomic_store_n(&ay.tail, tail, __ATOMIC_RELEASE);
        int n = frames - done;
        if (tail != head) {
            uint64_t next = ay.queue[tail % EVENT_QUEUE].time;
            if (next - ren.pos < (uint64_t)n) {
                n = next - ren.pos;
            }
        }
        render_span(dst + done * 2, n);
        ren.pos += n;
        done += n;
    }
    return done;
}

int cpc_audio_pending(void)
{
    return __atomic_load_n(&ay.now, __ATOMIC_ACQUIRE) - ren.pos;
}

void cpc_audio_enable(bool enable)
{
    ay.enabled = enable;
}

void audio_init(void)
{
    if (ay.enabled && ay.queue == NULL) {
        ay.queue = cpc_malloc(EVENT_QUEUE * sizeof(audio_event_t));
    }
    ay.head = ay.tail = 0;
    uint64_t now = video_clock() / US_PER_SAMPLE;
    ay.now = now;
    memset(&ren, 0, sizeof(ren));
    ren.lfsr = 1;
    ren.pos = now;
}
