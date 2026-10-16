/*
 * Keyboard, joystick and mice, from other tasks (Bluetooth, console).
 *
 * Events go through a small queue that the emulation drains once a frame
 * into the keyboard matrix: 10 lines of 8 keys, active low, read through
 * the AY's port A. Line 9 is also joystick 0, and the AMX mouse: its
 * direction lines pulse once per movement step, a step for each time the
 * firmware selects line 9 again. The Symbiface II PS/2 mouse hands out
 * X, Y and button records from a FIFO at &FD10.
 */
#include <stdio.h>
#include <string.h>
#include "cpc_internal.h"

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
#define LOCK()      portENTER_CRITICAL(&s_lock)
#define UNLOCK()    portEXIT_CRITICAL(&s_lock)
#else
#define LOCK()
#define UNLOCK()
#endif

#define QUEUE_SIZE  64          /* power of two */
#define SF2_FIFO    64

enum { EV_KEY, EV_JOY, EV_MOUSE, EV_KEYS_UP };

typedef struct {
    uint8_t type;
    uint8_t code;
    uint8_t value;
    int16_t dx, dy;
} input_event_t;

static input_event_t s_queue[QUEUE_SIZE];
static volatile uint32_t s_head, s_tail;

static struct {
    uint8_t matrix[16];         /* active low */
    uint8_t joy;
    int amx_x, amx_y;           /* steps not delivered yet */
    uint8_t amx_buttons;
    int last_line;
    uint8_t sf2[SF2_FIFO];
    int sf2_head, sf2_tail;
    uint8_t sf2_buttons;
} in;

static void push(const input_event_t *ev)
{
    LOCK();
    if (s_head - s_tail < QUEUE_SIZE) {
        s_queue[s_head % QUEUE_SIZE] = *ev;
        __atomic_store_n(&s_head, s_head + 1, __ATOMIC_RELEASE);
    }
    UNLOCK();
}

void cpc_key(uint8_t key, bool down)
{
    input_event_t ev = { .type = EV_KEY, .code = key, .value = down };
    push(&ev);
}

void cpc_joystick(uint8_t bits)
{
    input_event_t ev = { .type = EV_JOY, .value = bits };
    push(&ev);
}

void cpc_mouse(int dx, int dy, uint8_t buttons)
{
    input_event_t ev = { .type = EV_MOUSE, .value = buttons, .dx = dx, .dy = dy };
    push(&ev);
}

void cpc_keys_up(void)
{
    input_event_t ev = { .type = EV_KEYS_UP };
    push(&ev);
}

static void sf2_push(uint8_t v)
{
    int next = (in.sf2_head + 1) % SF2_FIFO;
    if (next != in.sf2_tail) {
        in.sf2[in.sf2_head] = v;
        in.sf2_head = next;
    }
}

/* Offsets of -32 .. 31 per record; X right and Y up are positive */
static void sf2_motion(uint8_t mode, int d)
{
    while (d) {
        int step = d > 31 ? 31 : d < -32 ? -32 : d;
        sf2_push(mode | (step & 0x3F));
        d -= step;
    }
}

uint8_t input_sf2_mouse(void)
{
    if (in.sf2_tail == in.sf2_head) {
        return 0;
    }
    uint8_t v = in.sf2[in.sf2_tail];
    in.sf2_tail = (in.sf2_tail + 1) % SF2_FIFO;
    return v;
}

static void mouse(int dx, int dy, uint8_t buttons)
{
    in.amx_x += dx;
    in.amx_y += dy;
    in.amx_buttons = buttons;
    sf2_motion(0x40, dx);
    sf2_motion(0x80, -dy);
    if (buttons != in.sf2_buttons) {
        in.sf2_buttons = buttons;
        sf2_push(0xC0 | (buttons & 0x1F));
    }
}

/* ---- Typing ---- */

#define TYPE_MAX    256
#define KEY_SHIFT   21

static char s_type[TYPE_MAX];
static volatile int s_type_len;
static int s_type_pos, s_type_frame;

int cpc_ascii_key(char c, bool *shift)
{
    /* Unshifted and shifted characters per key number */
    static const char plain[80] = {
        0, 0, 0, 0, 0, 0, 0, 0,         0, 0, 0, 0, 0, 0, 0, 0,
        0, '[', '\n', ']', 0, 0, '\\', 0,   '^', '-', '@', 'p', ';', ':', '/', '.',
        '0', '9', 'o', 'i', 'l', 'k', 'm', ',',   '8', '7', 'u', 'y', 'h', 'j', 'n', ' ',
        '6', '5', 'r', 't', 'g', 'f', 'b', 'v',   '4', '3', 'e', 'w', 's', 'd', 'c', 'x',
        '1', '2', 27, 'q', '\t', 'a', 0, 'z',     0, 0, 0, 0, 0, 0, 0, 8,
    };
    static const char shifted[80] = {
        [17] = '{', [19] = '}', [22] = '`', [24] = 0, [25] = '=', [26] = '|', [28] = '+',
        [29] = '*', [30] = '?', [31] = '>', [32] = '_', [33] = ')', [39] = '<', [40] = '(',
        [41] = '\'', [48] = '&', [49] = '%', [56] = '$', [57] = '#', [64] = '!', [65] = '"',
    };
    *shift = false;
    if (c >= 'A' && c <= 'Z') {
        *shift = true;
        c = c - 'A' + 'a';
    }
    if (c == '\r') {
        c = '\n';
    }
    for (int k = 0; k < 80; k++) {
        if (c && plain[k] == c) {
            return k;
        }
    }
    for (int k = 0; k < 80; k++) {
        if (c && shifted[k] == c) {
            *shift = true;
            return k;
        }
    }
    return -1;
}

void cpc_type(const char *text)
{
    s_type_len = 0;
    s_type_pos = 0;
    snprintf(s_type, sizeof(s_type), "%s", text);
    __atomic_store_n(&s_type_len, (int)strlen(s_type), __ATOMIC_RELEASE);
}

/* Each character: keys down for 2 frames, up for 2 */
static void type_frame(void)
{
    int len = __atomic_load_n(&s_type_len, __ATOMIC_ACQUIRE);
    if (s_type_pos >= len) {
        return;
    }
    bool shift;
    int key = cpc_ascii_key(s_type[s_type_pos], &shift);
    int phase = s_type_frame++;
    if (key < 0) {
        s_type_pos++;
        s_type_frame = 0;
        return;
    }
    if (phase == 0) {
        if (shift) {
            in.matrix[KEY_SHIFT >> 3] &= ~(1u << (KEY_SHIFT & 7));
        }
        in.matrix[key >> 3] &= ~(1u << (key & 7));
    } else if (phase == 2) {
        in.matrix[key >> 3] |= 1u << (key & 7);
        in.matrix[KEY_SHIFT >> 3] |= 1u << (KEY_SHIFT & 7);
    } else if (phase >= 3) {
        s_type_pos++;
        s_type_frame = 0;
    }
}

void input_frame(void)
{
    type_frame();
    uint32_t head = __atomic_load_n(&s_head, __ATOMIC_ACQUIRE);
    while (s_tail != head) {
        input_event_t ev = s_queue[s_tail % QUEUE_SIZE];
        __atomic_store_n(&s_tail, s_tail + 1, __ATOMIC_RELEASE);
        switch (ev.type) {
        case EV_KEY:
            if (ev.code < 80) {
                uint8_t bit = 1u << (ev.code & 7);
                in.matrix[ev.code >> 3] = ev.value ? in.matrix[ev.code >> 3] & ~bit
                                                   : in.matrix[ev.code >> 3] | bit;
            }
            break;
        case EV_JOY:
            in.joy = ev.value;
            break;
        case EV_MOUSE:
            mouse(ev.dx, ev.dy, ev.value);
            break;
        case EV_KEYS_UP:
            memset(in.matrix, 0xFF, sizeof(in.matrix));
            in.joy = 0;
            break;
        }
    }
}

void input_row_selected(int line)
{
    if (line == 9 && in.last_line != 9) {
        /* the AMX interface hands out one step per axis per selection */
        in.amx_x += in.amx_x > 0 ? -1 : in.amx_x < 0 ? 1 : 0;
        in.amx_y += in.amx_y > 0 ? -1 : in.amx_y < 0 ? 1 : 0;
    }
    in.last_line = line;
}

uint8_t input_row(int line)
{
    uint8_t v = in.matrix[line & 15];
    if (line == 9) {
        uint8_t low = in.joy & 0x3F;
        low |= in.amx_y < 0 ? 0x01 : in.amx_y > 0 ? 0x02 : 0;
        low |= in.amx_x < 0 ? 0x04 : in.amx_x > 0 ? 0x08 : 0;
        low |= (in.amx_buttons & 1) ? 0x10 : 0;     /* left: fire 2 */
        low |= (in.amx_buttons & 2) ? 0x20 : 0;     /* right: fire 1 */
        low |= (in.amx_buttons & 4) ? 0x40 : 0;
        v &= ~low;
    }
    return v;
}

void input_reset(void)
{
    memset(in.matrix, 0xFF, sizeof(in.matrix));
    in.amx_x = in.amx_y = 0;
    in.sf2_head = in.sf2_tail = 0;
    in.last_line = -1;
}
