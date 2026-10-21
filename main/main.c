/*
 * Amstrad CPC 464 / 664 / 6128 / 464 Plus / 6128 Plus emulator on the
 * Makerfabs MaTouch ESP32-S3 7".
 *
 * The board side (tasks, LCD, touch, WiFi, Bluetooth, setup screen, web
 * page, console) is the shared runtime in ../esp32emu; this file plugs the
 * CPC into it: memory, ROMs, settings, keyboard map, disks and the
 * cartridge slot, status.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "emu/emu.h"
#include "cpc.h"
#include "rom_store.h"
#include "settings.h"

static const char *TAG = "main";

static const char *const model_names[] = { "464", "664", "6128", "464+", "6128+", "" };
static const char *const model_full[] = {
    "Amstrad CPC 464", "Amstrad CPC 664", "Amstrad CPC 6128", "Amstrad 464 Plus", "Amstrad 6128 Plus",
};

#define RAM_MAX     (128 * 1024)

static cpc_config_t s_cfg;
static uint8_t *s_rom[3];               /* OS, BASIC, AMSDOS */
static uint8_t *s_cart;                 /* the Plus system cartridge */
static char s_rom_text[96];

/* ---- Memory ----
 *
 * Everything at power-on, before the screen, WiFi and Bluetooth fragment
 * PSRAM: RAM for the largest model (128 KB, so changing the model never
 * needs a restart), the ROMs, the system cartridge and the picture.
 */
static void reserve_memory(void)
{
    settings_load();
    s_cfg.ram = heap_caps_malloc(RAM_MAX, MALLOC_CAP_SPIRAM);
    for (int i = 0; i < 3; i++) {
        s_rom[i] = heap_caps_malloc(CPC_ROM_SIZE, MALLOC_CAP_SPIRAM);
    }
    s_cart = heap_caps_malloc(CPC_CART_MAX, MALLOC_CAP_SPIRAM);
    s_cfg.frame = heap_caps_calloc(CPC_SCREEN_WIDTH * CPC_SCREEN_HEIGHT, sizeof(uint16_t), MALLOC_CAP_SPIRAM);
}

static void storage(bool sd_card)
{
    emu_set_drive_info(sd_card ? "Disks and cartridges on the SD card" : "No SD card: no disks");
}

/* ---- Starting ---- */

static bool missing(const char *what)
{
    static char message[200];
    snprintf(message, sizeof(message),
             "No %s ROM: put %s into roms/ and flash with idf.py flash, or copy it into the "
             "\"roms\" folder on the SD card.", model_full[settings.model], what);
    emu_set_message(message);
    return false;
}

static bool start(void)
{
    if (s_cfg.ram == NULL || s_cfg.frame == NULL || s_cart == NULL) {
        emu_set_message("Not enough memory: see the log.");
        return false;
    }
    cpc_model_t model = settings.model;
    s_cfg.model = model;
    s_cfg.ram_size = settings_ram_kb() * 1024;
    s_cfg.os = s_cfg.basic = s_cfg.amsdos = NULL;
    s_cfg.system_cart = NULL;
    s_cfg.system_cart_size = 0;

    if (cpc_model_is_plus(model)) {
        const char *name = "";
        uint32_t size = rom_store_system_cart(s_cart, &name);
        if (size == 0) {
            return missing("system.cpr (or osplus.rom and basicplus.rom)");
        }
        s_cfg.system_cart = s_cart;
        s_cfg.system_cart_size = size;
        snprintf(s_rom_text, sizeof(s_rom_text), "%s, %u KB", name, (unsigned)(size / 1024));
    } else {
        static const rom_id_t os[] = { ROM_OS464, ROM_OS664, ROM_OS6128 };
        static const rom_id_t basic[] = { ROM_BASIC10, ROM_BASIC664, ROM_BASIC11 };
        if (!rom_store_load(os[model], s_rom[0])) {
            return missing(rom_store_name(os[model]));
        }
        if (!rom_store_load(basic[model], s_rom[1])) {
            return missing(rom_store_name(basic[model]));
        }
        s_cfg.os = s_rom[0];
        s_cfg.basic = s_rom[1];
        s_cfg.amsdos = rom_store_load(ROM_AMSDOS, s_rom[2]) ? s_rom[2] : NULL;
        snprintf(s_rom_text, sizeof(s_rom_text), "%s, %s%s", rom_store_name(os[model]),
                 rom_store_name(basic[model]), s_cfg.amsdos ? ", amsdos.rom" : " (no disc ROM)");
    }
    if (!cpc_init(&s_cfg)) {
        emu_set_message("The CPC could not be set up: see the log.");
        return false;
    }
    ESP_LOGI(TAG, "%s, %u KB: %s", model_full[model], settings_ram_kb(), s_rom_text);
    return true;
}

static void screen_mode(emu_screen_t *mode)
{
    mode->width = CPC_SCREEN_WIDTH;
    mode->height = CPC_SCREEN_HEIGHT;
    mode->hz = CPC_SCREEN_HZ;
}

/* ---- Keyboard, joystick, mouse ----
 *
 * Keys by position (a UK/US keyboard onto the CPC's): F1-F10 are the
 * CPC's f1-f0 keypad keys, the numeric keypad is joystick 0 (8 4 6 2 and
 * the diagonals, 0 or 5 fire, Enter fire 1 ... see below), F12 makes the
 * cursor keys the joystick too (Alt: fire).
 */
#define K(n)        ((n) | 0x80)
static const uint8_t usage_to_cpc[0x68] = {
    [0x04] = K(69), [0x05] = K(54), [0x06] = K(62), [0x07] = K(61),     /* a b c d */
    [0x08] = K(58), [0x09] = K(53), [0x0A] = K(52), [0x0B] = K(44),     /* e f g h */
    [0x0C] = K(35), [0x0D] = K(45), [0x0E] = K(37), [0x0F] = K(36),     /* i j k l */
    [0x10] = K(38), [0x11] = K(46), [0x12] = K(34), [0x13] = K(27),     /* m n o p */
    [0x14] = K(67), [0x15] = K(50), [0x16] = K(60), [0x17] = K(51),     /* q r s t */
    [0x18] = K(42), [0x19] = K(55), [0x1A] = K(59), [0x1B] = K(63),     /* u v w x */
    [0x1C] = K(43), [0x1D] = K(71),                                     /* y z */
    [0x1E] = K(64), [0x1F] = K(65), [0x20] = K(57), [0x21] = K(56),     /* 1 2 3 4 */
    [0x22] = K(49), [0x23] = K(48), [0x24] = K(41), [0x25] = K(40),     /* 5 6 7 8 */
    [0x26] = K(33), [0x27] = K(32),                                     /* 9 0 */
    [0x28] = K(18),         /* Return */
    [0x29] = K(66),         /* Esc */
    [0x2A] = K(79),         /* Backspace -> DEL */
    [0x2B] = K(68),         /* Tab */
    [0x2C] = K(47),         /* Space */
    [0x2D] = K(25),         /* - = */
    [0x2E] = K(24),         /* = -> ^ */
    [0x2F] = K(26),         /* [ -> @ */
    [0x30] = K(17),         /* ] -> [ */
    [0x31] = K(19),         /* \ -> ] */
    [0x32] = K(19),         /* non-US # -> ] */
    [0x33] = K(29),         /* ; -> : */
    [0x34] = K(28),         /* ' -> ; */
    [0x35] = K(22),         /* ` -> \ */
    [0x36] = K(39),         /* , */
    [0x37] = K(31),         /* . */
    [0x38] = K(30),         /* / */
    [0x39] = K(70),         /* Caps Lock */
    [0x3A] = K(13), [0x3B] = K(14), [0x3C] = K(5), [0x3D] = K(20),      /* F1-F4 -> f1-f4 */
    [0x3E] = K(12), [0x3F] = K(4), [0x40] = K(10), [0x41] = K(11),      /* F5-F8 */
    [0x42] = K(3), [0x43] = K(15), [0x44] = K(7),                       /* F9 F10 F11 -> f9 f0 f. */
    [0x49] = K(9),          /* Insert -> COPY */
    [0x4C] = K(16),         /* Delete -> CLR */
    [0x4F] = K(1),          /* Right */
    [0x50] = K(8),          /* Left */
    [0x51] = K(2),          /* Down */
    [0x52] = K(0),          /* Up */
    [0x58] = K(6),          /* keypad Enter -> ENTER */
    [0x64] = K(22),         /* non-US \ (ISO key left of Z) */
};

/* Modifiers $E0-$E7: LCtrl LShift LAlt LGUI RCtrl RShift RAlt RGUI */
static const uint8_t modifier_to_cpc[8] = { 23, 21, 9, 0xFF, 23, 21, 9, 0xFF };

static uint8_t s_modifiers;
static uint8_t s_joy_pad, s_joy_arrows;
static bool s_arrows_joystick;

static void joystick_update(void)
{
    cpc_joystick(s_joy_pad | s_joy_arrows);
}

/* Numeric keypad: directions and fire */
static bool keypad_joystick(uint8_t usage, bool pressed)
{
    static const uint8_t bits[] = {
        [0x59 - 0x59] = CPC_JOY_DOWN | CPC_JOY_LEFT, CPC_JOY_DOWN, CPC_JOY_DOWN | CPC_JOY_RIGHT,
        CPC_JOY_LEFT, CPC_JOY_FIRE2, CPC_JOY_RIGHT,
        CPC_JOY_UP | CPC_JOY_LEFT, CPC_JOY_UP, CPC_JOY_UP | CPC_JOY_RIGHT,
        CPC_JOY_FIRE2,                          /* 0 */
        CPC_JOY_FIRE1,                          /* . */
    };
    if (usage < 0x59 || usage > 0x63) {
        return false;
    }
    uint8_t b = bits[usage - 0x59];
    s_joy_pad = pressed ? s_joy_pad | b : s_joy_pad & ~b;
    joystick_update();
    return true;
}

static void hid_key(uint8_t usage, bool pressed)
{
    if (usage >= 0xE0 && usage <= 0xE7) {
        int bit = usage - 0xE0;
        if (s_arrows_joystick && (bit == 2 || bit == 6)) {
            /* Alt is fire while the cursor keys are the joystick */
            s_joy_arrows = pressed ? s_joy_arrows | CPC_JOY_FIRE2 : s_joy_arrows & ~CPC_JOY_FIRE2;
            joystick_update();
            return;
        }
        uint8_t key = modifier_to_cpc[bit];
        if (key == 0xFF) {
            return;
        }
        uint8_t mask = 0;
        for (int i = 0; i < 8; i++) {
            mask |= modifier_to_cpc[i] == key ? 1u << i : 0;
        }
        bool was = (s_modifiers & mask) != 0;
        s_modifiers = pressed ? s_modifiers | (1u << bit) : s_modifiers & ~(1u << bit);
        bool now = (s_modifiers & mask) != 0;
        if (was != now) {
            cpc_key(key, now);
        }
        return;
    }
    if (usage == 0x45) {            /* F12: cursor keys <-> joystick */
        if (pressed) {
            s_arrows_joystick = !s_arrows_joystick;
            s_joy_arrows = 0;
            joystick_update();
            ESP_LOGI(TAG, "cursor keys: %s", s_arrows_joystick ? "joystick" : "cursor keys");
        }
        return;
    }
    if (s_arrows_joystick && usage >= 0x4F && usage <= 0x52) {
        static const uint8_t dir[4] = { CPC_JOY_RIGHT, CPC_JOY_LEFT, CPC_JOY_DOWN, CPC_JOY_UP };
        uint8_t b = dir[usage - 0x4F];
        s_joy_arrows = pressed ? s_joy_arrows | b : s_joy_arrows & ~b;
        joystick_update();
        return;
    }
    if (keypad_joystick(usage, pressed)) {
        return;
    }
    if (usage < sizeof(usage_to_cpc) && usage_to_cpc[usage]) {
        cpc_key(usage_to_cpc[usage] & 0x7F, pressed);
    }
}

static void hid_mouse(int dx, int dy, uint8_t buttons)
{
    cpc_mouse(dx, dy, buttons);
}

/* ---- Setup screen choices ---- */

static int model_get(void)
{
    return settings.model;
}

static void model_set(int i)
{
    if (i >= 0 && i < CPC_MODEL_COUNT) {
        settings.model = i;
        settings_save();
    }
}

static const emu_choice_t choices[] = {
    { "Model", model_names, model_get, model_set, false },
};

/* ---- Disks and the cartridge slot ---- */

enum { DRIVE_A, DRIVE_B, DRIVE_CART, DRIVES };

static const char *const extensions[] = { ".dsk", ".cpr", ".rom", NULL };
static const char *const drive_names[] = { "A:", "B:", "Cartridge" };
static const disks_size_t sizes[] = {
    { 184320, "178 KB data format" },
    { 173056, "169 KB system format" },
};

static bool has_ext(const char *name, const char *ext)
{
    size_t n = strlen(name), e = strlen(ext);
    return n > e && strcasecmp(name + n - e, ext) == 0;
}

static bool accepts(int drive, const char *name)
{
    bool disk = has_ext(name, ".dsk");
    return drive == DRIVE_CART ? !disk : disk;
}

static bool takes_blank(int drive)
{
    return drive != DRIVE_CART;
}

static bool insert(int drive, const char *path)
{
    if (!accepts(drive, path)) {
        return false;
    }
    if (drive == DRIVE_CART) {
        return cpc_cart_insert(path);
    }
    return cpc_disk_insert(drive, path);
}

static void eject(int drive)
{
    if (drive == DRIVE_CART) {
        cpc_cart_eject();
    } else {
        cpc_disk_eject(drive);
    }
}

static const char *path(int drive)
{
    return drive == DRIVE_CART ? cpc_cart_path() : cpc_disk_path(drive);
}

static bool write_protected(int drive)
{
    return drive == DRIVE_CART || cpc_disk_write_protected(drive);
}

static const disks_config_t disks = {
    .folder = "cpc",
    .extensions = extensions,
    .description = "Disks (.DSK), cartridges (.CPR), ROMs (.ROM)",
    .title = "Disks and cartridge",
    .button = "Disks",
    .nvs_namespace = "cpcdisks",
    .drives = DRIVES,
    .drive_names = drive_names,
    .new_prefix = "BLANK",
    .new_extension = ".dsk",
    .sizes = sizes,
    .size_count = sizeof(sizes) / sizeof(sizes[0]),
    .max_upload = 8 * 1024 * 1024,
    .insert = insert,
    .eject = eject,
    .path = path,
    .write_protected = write_protected,
    .create = cpc_disk_create,
    .accepts = accepts,
    .takes_blank = takes_blank,
};

/* Disk writes go to the SD card from here (core 0), between frames, once
 * nothing has been written for a second
 */
static void flush(void *arg)
{
    cpc_disk_flush();
}

static void every_second(void)
{
    static int dirty_for;
    if (cpc_disk_dirty()) {
        if (++dirty_for >= 2) {
            emu_paused(flush, NULL);
            dirty_for = 0;
        }
    } else {
        dirty_for = 0;
    }
}

/* ---- Status ---- */

static void status(emu_json_t *j, int section)
{
    char text[160];
    if (section == EMU_STATUS_ROWS) {
        snprintf(text, sizeof(text), "%s, %u KB", model_full[cpc_model()], (unsigned)(s_cfg.ram_size / 1024));
        emu_json_row(j, "System", text);
        emu_json_row(j, "ROMs", s_rom_text);
        cpc_status_t s;
        cpc_get_status(&s);
        /* CRTC characters are 2 bytes: 4, 8 or 16 pixels in modes 0, 1, 2 */
        static const uint8_t pixels_per_char[4] = { 4, 8, 16, 4 };
        snprintf(text, sizeof(text), "mode %d, %d × %d pixels, 50 Hz%s", s.mode,
                 s.crtc[1] * pixels_per_char[s.mode & 3], s.crtc[6] * ((s.crtc[9] & 31) + 1),
                 s.plus ? (s.asic_unlocked ? ", ASIC unlocked" : ", ASIC locked") : "");
        emu_json_row(j, "Screen", text);
        emu_json_row(j, "Joystick", s_arrows_joystick ? "numeric keypad and cursor keys (F12)"
                                                       : "numeric keypad (F12: cursor keys too)");
        return;
    }
    cpc_disk_status_t ds;
    cpc_disk_status(&ds);
    for (int d = 0; d < CPC_DRIVES; d++) {
        const char *name = disks_name(d);
        snprintf(text, sizeof(text), "track %u", ds.track[d]);
        uint64_t free_bytes = 0, size = 0;
        cpc_disk_space(d, &free_bytes, &size);
        emu_json_drive_space(j, drive_names[d], name, cpc_disk_write_protected(d), ds.light == d,
                             ds.reads[d], ds.writes[d], name ? text : NULL, free_bytes, size);
    }
    const char *cart = disks_name(DRIVE_CART);
    snprintf(text, sizeof(text), "%s%s", cpc_cart_info(),
             cpc_cart_is_plus() && !cpc_model_is_plus(cpc_model()) ? " (needs a Plus)" : "");
    emu_json_drive(j, drive_names[DRIVE_CART], cart, true, false, 0, 0, cart ? text : NULL);
}

/* ---- Console ---- */

static void console_info(char *buf, size_t len)
{
    cpc_status_t s;
    cpc_get_status(&s);
    snprintf(buf, len, "frames %lu PC &%04X mode %d%s", (unsigned long)s.frames, s.pc, s.mode,
             s.plus ? (s.asic_unlocked ? " ASIC" : " plus") : "");
}

/*   k <n>              tap CPC key number n (line * 8 + bit, e.g. 18 RETURN)
 *   K <n> <0|1>        key n down (1) or up (0)
 *   T <text>           type text (\n is RETURN)
 *   j <bits>           joystick 0 (1 up 2 down 4 left 8 right 16 fire 32 fire 1)
 *   m <dx> <dy> <btn>  mouse
 *   R <model>          restart as model 0 464, 1 664, 2 6128, 3 464 Plus, 4 6128 Plus
 */
static bool console_command(const char *line)
{
    switch (line[0]) {
    case 'k': {
        unsigned key = strtoul(line + 1, NULL, 10);
        cpc_key(key, true);
        vTaskDelay(pdMS_TO_TICKS(80));
        cpc_key(key, false);
        return true;
    }
    case 'K': {
        unsigned key = 0, down = 0;
        if (sscanf(line + 1, "%u %u", &key, &down) == 2) {
            cpc_key(key, down);
        }
        return true;
    }
    case 'T': {
        char text[200];
        const char *s = line + 1;
        while (*s == ' ') {
            s++;
        }
        size_t n = 0;
        for (; *s && n < sizeof(text) - 1; s++) {
            if (s[0] == '\\' && s[1] == 'n') {
                text[n++] = '\n';
                s++;
            } else {
                text[n++] = *s;
            }
        }
        text[n] = 0;
        cpc_type(text);
        return true;
    }
    case 'j':
        cpc_joystick(strtoul(line + 1, NULL, 0));
        return true;
    case 'm': {
        int dx = 0, dy = 0, button = 0;
        sscanf(line + 1, "%d %d %d", &dx, &dy, &button);
        cpc_mouse(dx, dy, button);
        return true;
    }
    case 'R': {
        int model = 0;
        if (sscanf(line + 1, "%d", &model) == 1 && model >= 0 && model < CPC_MODEL_COUNT) {
            settings.model = model;
            settings_save();
            emu_restart();
        }
        return true;
    }
    default:
        return false;
    }
}

/* ---- The machine ---- */

static const emu_machine_t machine = {
    .name = "Amstrad CPC",
    .short_name = "CPC",
    .view_width = CPC_SCREEN_WIDTH,     /* 768 x 300, every line twice: 768 x 600 */
    .scale = EMU_SCALE_NEAREST,
    .reserve_memory = reserve_memory,
    .storage = storage,
    .start = start,
    .reset = cpc_reset,
    .run_frame = cpc_run_frame,
    .screen_mode = screen_mode,
    .screen_signature = cpc_screen_signature,
    .render_line = cpc_render_line_rgb565,
    .audio_hz = CPC_AUDIO_HZ,
    .audio_enable = cpc_audio_enable,
    .audio_render = cpc_audio_render,
    .audio_pending = cpc_audio_pending,
    .hid_key = hid_key,
    .hid_mouse = hid_mouse,
    .choices = choices,
    .choice_count = 1,
    .disks = &disks,
    .status = status,
    .console_info = console_info,
    .console_command = console_command,
    .every_second = every_second,
};

void app_main(void)
{
    emu_run(&machine);
}
