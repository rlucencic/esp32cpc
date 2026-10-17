/*
 * Between the CPC core's modules.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "cpc.h"
#include "z80.h"

/* Code on the emulation's hot path */
#define CPC_HOT     Z80_HOT

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "esp_log.h"
#define CPC_LOG(fmt, ...)   ESP_LOGI("cpc", fmt, ##__VA_ARGS__)
/* All of the core's buffers in PSRAM: plain malloc() serves small blocks
 * (a disk track is ~4.6 KB) from internal RAM, and a disk image used it up
 */
#define cpc_malloc(n)       heap_caps_malloc((n), MALLOC_CAP_SPIRAM)
#define cpc_calloc(n, s)    heap_caps_calloc((n), (s), MALLOC_CAP_SPIRAM)
#else
#include <stdio.h>
#include <stdlib.h>
#define CPC_LOG(fmt, ...)   printf("cpc: " fmt "\n", ##__VA_ARGS__)
#define cpc_malloc(n)       malloc(n)
#define cpc_calloc(n, s)    calloc((n), (s))
#endif

/* ---- cpc.c: the machine ---- */

typedef struct {
    cpc_config_t cfg;
    bool plus;

    /* Memory map */
    uint8_t ram_config;         /* last RAM banking byte (%11bbbccc) */
    uint8_t rom_config;         /* Gate Array mode/ROM register (%100irumm) */
    uint8_t upper_rom;          /* selected upper ROM number */
    uint8_t rmr2;               /* Plus: lower ROM page and position, ASIC page */
    const uint8_t *slots[CPC_ROM_SLOTS];    /* expansion ROMs */
    const uint8_t *cart;        /* Plus: the cartridge in use (16 KB pages) */
    uint32_t cart_pages;

    /* Time: us of the current instruction already handed to the video */
    int io_at;
    uint32_t frames;

    /* 8255 PPI */
    uint8_t ppi_a, ppi_c, ppi_control;

    uint8_t rtc_reg;            /* Symbiface II RTC register select */
    bool reset_pending;
    bool draw_all;              /* draw every picture, not every other */
} cpc_t;

extern cpc_t cpc;

/* Recompute the CPU's view of memory after a banking change */
void mem_update(void);

/* Interrupt line from the Gate Array and the ASIC */
void cpc_update_int(void);

/* ---- video.c: CRTC, Gate Array, monitor, Plus video ---- */

void video_reset(void);
void video_run(int us);                 /* advance the video by us */
uint64_t video_clock(void);             /* us since power-on */
void ga_write(uint8_t v);               /* port &7Fxx, bits 7-6 = 00, 01, 10 */
void crtc_select(uint8_t v);
void crtc_write(uint8_t v);
uint8_t crtc_read(int port);            /* &BExx (2) or &BFxx (3) */
bool video_vsync(void);                 /* PPI port B bit 0 */
bool video_ga_int(void);
void video_int_ack(void);               /* the Z80 took the interrupt */
void video_set_pen_rgb(int pen, uint16_t rgb565);   /* Plus palette */
uint8_t video_mode(void);
void video_crtc_regs(uint8_t *regs);    /* 18 */
uint32_t video_signature(void);

/* ---- asic.c: the Plus ASIC ---- */

typedef struct {
    bool unlocked;
    uint8_t lock_pos;
    uint8_t *page;              /* 16 KB register page as the CPU reads it */
    uint8_t sprite[16][256];    /* pixels, 4 bits */
    int16_t spr_x[16], spr_y[16];
    uint8_t spr_mx[16], spr_my[16];     /* magnification 0 (off), 1, 2, 4 */
    uint8_t pri, sssl;
    uint16_t ssa;
    uint8_t hscroll, vscroll;
    bool extend_border;
    uint8_t ivr;
    bool raster_int;
    struct {
        uint16_t addr, loop_addr;
        uint8_t prescaler;
        bool enabled, int_pending;
        uint16_t pause, tick, loops;
    } dma[3];
} asic_t;

extern asic_t asic;

void asic_reset(void);
void asic_lock_byte(uint8_t v);         /* CRTC register select writes */
void asic_write(uint16_t addr, uint8_t v);
void asic_dma(void);                    /* once per scanline (HSYNC) */
void asic_raster_int(void);
bool asic_int_pending(void);
uint8_t asic_int_ack(bool im2);         /* vector for IM 2 */
uint16_t asic_ga_colour_rgb565(uint8_t hw, uint16_t *page_word);  /* GA colour on a Plus */

/* ---- psg.c: AY-3-8912 and the sound output ---- */

void psg_reset(void);
void psg_bus(uint8_t control, uint8_t data);     /* BDIR/BC1 from PPI port C */
uint8_t psg_read(void);                 /* PPI port A in */
void psg_write_reg(uint8_t reg, uint8_t v);      /* also the Plus DMA */
void audio_init(void);
void audio_frame_end(void);

/* ---- input.c ---- */

void input_reset(void);
void input_frame(void);                 /* drain the queue, once a frame */
uint8_t input_row(int line);            /* matrix line, active low */
void input_row_selected(int line);      /* PPI port C keyboard line write */
uint8_t input_sf2_mouse(void);          /* Symbiface II &FD10 */

/* ---- cart.c ---- */

void cart_apply(void);                  /* expansion ROMs and the cartridge, at reset */
