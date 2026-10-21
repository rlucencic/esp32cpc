/*
 * Amstrad CPC 464, 664, 6128 and the Plus range (464 Plus, 6128 Plus).
 *
 * Z80 with the Gate Array's wait states (every instruction a whole number
 * of microseconds), Gate Array (27 colours, modes 0-3, interrupts every 52
 * lines), 6845 CRTC (counters, so split screens and overscan work), 8255
 * PPI, AY-3-8912 with the keyboard matrix, uPD765 floppy controller with
 * DSK and extended DSK images, RAM expansions up to 576 KB, expansion ROMs
 * in upper ROM slots. The Plus models add the ASIC: 4096 colours, 16
 * hardware sprites, split screen, soft scroll, programmable raster
 * interrupt, three DMA sound channels, and cartridges (.cpr).
 *
 * Extras for SymbOS and friends: the Symbiface II real-time clock (the
 * host's local time) and PS/2 mouse, and an AMX mouse on the joystick port.
 *
 * Platform independent: the caller provides the ROMs and the big buffers
 * and drives the emulation one video frame (19968 us) at a time.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "upd765.h"

typedef enum {
    CPC_MODEL_464,
    CPC_MODEL_664,
    CPC_MODEL_6128,
    CPC_MODEL_464PLUS,
    CPC_MODEL_6128PLUS,
    CPC_MODEL_COUNT
} cpc_model_t;

static inline bool cpc_model_is_plus(cpc_model_t m)
{
    return m == CPC_MODEL_464PLUS || m == CPC_MODEL_6128PLUS;
}

/* The picture: the monitor's visible area at mode 2 resolution (one CRTC
 * character = 16 pixels) and two lines per scanline would be 768 x 600;
 * this is one line per scanline, the board doubles it
 */
#define CPC_SCREEN_WIDTH        768
#define CPC_SCREEN_HEIGHT       300
#define CPC_SCREEN_HZ           50
#define CPC_FRAME_US            19968   /* 312 lines of 64 us */

#define CPC_ROM_SIZE            16384
#define CPC_CART_MAX            (512 * 1024)
#define CPC_DRIVES              UPD765_DRIVES   /* A: and B: */
#define CPC_ROM_SLOTS           16      /* upper ROM numbers 0-15 */

typedef struct {
    cpc_model_t model;
    uint8_t *ram;               /* ram_size bytes */
    uint32_t ram_size;          /* 64, 128 or 576 KB */

    /* 16 KB ROM images (dumps as they are), for the classic models: */
    const uint8_t *os;          /* lower ROM */
    const uint8_t *basic;       /* upper ROM 0 */
    const uint8_t *amsdos;      /* upper ROM 7: the disc drive; NULL: none */

    /* Plus models: the system cartridge (OS, BASIC, AMSDOS, ...), up to
     * CPC_CART_MAX bytes of 16 KB pages, used when no cartridge is in
     */
    const uint8_t *system_cart;
    uint32_t system_cart_size;

    /* CPC_SCREEN_WIDTH x CPC_SCREEN_HEIGHT RGB565 pixels */
    uint16_t *frame;
} cpc_config_t;

typedef struct {
    uint32_t frames;
    uint16_t pc;
    bool plus;
    bool asic_unlocked;
    uint8_t mode;               /* screen mode 0-3 */
    uint8_t crtc[18];
} cpc_status_t;

/* Set up the machine and cold-reset it. False if something is missing. */
bool cpc_init(const cpc_config_t *config);

/* Cold reset, as the power switch */
void cpc_reset(void);

/* Emulate one video frame (19968 us, 50.08 Hz) */
void cpc_run_frame(void);

void cpc_get_status(cpc_status_t *status);
cpc_model_t cpc_model(void);

/* Read CPC memory as the CPU sees it now (no side effects) */
uint8_t cpc_peek(uint16_t address);

/* ---- Screen ---- */

/* Line y (0 .. CPC_SCREEN_HEIGHT - 1) as RGB565, CPC_SCREEN_WIDTH pixels */
void cpc_render_line_rgb565(int y, uint16_t *dst);

/* Changes when the picture does */
uint32_t cpc_screen_signature(void);

/* Only every other picture is drawn (the board shows 25 a second); true:
 * all of them (screenshots of a given frame)
 */
void cpc_draw_every_frame(bool all);

/* ---- Sound: 16-bit stereo (A left, B both, C right) ---- */

#define CPC_AUDIO_HZ            31250

void cpc_audio_enable(bool enable);
int cpc_audio_render(int16_t *dst, int frames);
int cpc_audio_pending(void);

/* ---- Floppy disks: drives 0 (A:) and 1 (B:) ----
 *
 * The uPD765 and its DSK and extended DSK images are esp32emu's
 * (upd765.h); these are its disk calls. Not while cpc_run_frame() runs.
 */
bool cpc_disk_insert(int drive, const char *path);
void cpc_disk_eject(int drive);
static inline const char *cpc_disk_path(int drive) { return upd765_disk_path(drive); }
static inline bool cpc_disk_write_protected(int drive) { return upd765_disk_write_protected(drive); }
static inline bool cpc_disk_dirty(void) { return upd765_disk_dirty(); }
void cpc_disk_flush(void);

/* A disk's free space and size in bytes as AMSDOS counts them (data,
 * system and IBM formats), worked out at insert, eject and flush; false
 * when not known. Any task.
 */
bool cpc_disk_space(int drive, uint64_t *free_bytes, uint64_t *size);

/* A formatted blank disk: `bytes` 184320 = data format (40 tracks of 9
 * sectors $C1-$C9), 173056 = system (vendor) format ($41-$49)
 */
static inline bool cpc_disk_create(const char *path, uint32_t bytes)
{
    return upd765_disk_create(path, 40, 9, bytes == 173056 ? 0x41 : 0xC1, NULL);
}

typedef upd765_status_t cpc_disk_status_t;
static inline void cpc_disk_status(cpc_disk_status_t *status) { upd765_disk_status(status); }

/* ---- Cartridge slot ----
 *
 * A Plus cartridge (.cpr), or expansion ROMs (.rom, 16 KB, with or
 * without a 128-byte AMSDOS header) for the upper ROM slots: NAME_1.ROM
 * brings NAME_2.ROM ... along into the next slots. Takes effect at the
 * next reset (cpc_cart_insert asks for one). Not while cpc_run_frame()
 * runs.
 */
bool cpc_cart_insert(const char *path);
void cpc_cart_eject(void);
const char *cpc_cart_path(void);            /* NULL when empty */
/* What's in: "Plus cartridge, 128 KB", "ROMs in slots 1-2", ... */
const char *cpc_cart_info(void);
bool cpc_cart_is_plus(void);                /* a .cpr is in */

/* A .cpr file's pages into `pages` (CPC_CART_MAX bytes, $FF where none):
 * their size in bytes, 0 if it isn't a cartridge
 */
uint32_t cpc_cpr_pages(const uint8_t *file, uint32_t size, uint8_t *pages);

/* ---- Keyboard, joystick, mice: safe to call from another task ---- */

/* CPC key number: matrix line * 8 + bit (0 .. 79, as in BASIC's INKEY) */
void cpc_key(uint8_t key, bool down);

/* Joystick 0: CPC_JOY_* bits */
enum {
    CPC_JOY_UP = 0x01, CPC_JOY_DOWN = 0x02, CPC_JOY_LEFT = 0x04, CPC_JOY_RIGHT = 0x08,
    CPC_JOY_FIRE2 = 0x10,       /* the usual fire button (BASIC's JOY(0) AND 16) */
    CPC_JOY_FIRE1 = 0x20,
};
void cpc_joystick(uint8_t bits);

/* Relative mouse motion (screen pixels, y down) and buttons (bit 0 left,
 * 1 right, 2 middle): the AMX mouse on the joystick port and the
 * Symbiface II PS/2 mouse both see it
 */
void cpc_mouse(int dx, int dy, uint8_t buttons);

/* Release all keys */
void cpc_keys_up(void);

/* The CPC key for an ASCII character (UK layout), -1 if none; *shift if it
 * needs SHIFT. '\n' is RETURN.
 */
int cpc_ascii_key(char c, bool *shift);

/* Type text: one key every few frames, from the emulation (the firmware
 * scans the keyboard 50 times a second). Replaces what's still queued.
 */
void cpc_type(const char *text);
