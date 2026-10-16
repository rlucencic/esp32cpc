/*
 * Desktop build of the CPC core, for testing without the board:
 *
 *   cpc_host <model> [frames] [out.ppm] [ram_kb]
 *
 * model: 464, 664, 6128, 464plus, 6128plus. ROMs from CPC_ROMS (default
 * ../../../roms): os464.rom basic10.rom os664.rom basic664.rom os6128.rom
 * basic11.rom amsdos.rom, and system.cpr (or osplus.rom + basicplus.rom).
 *
 * Environment:
 *   CPC_DISKA, CPC_DISKB    disk images in A: and B:
 *   CPC_CART=<file>         cartridge (.cpr) or expansion ROM(s)
 *   CPC_SCRIPT=<file>       lines "<frame> <command>":
 *                             type <text>          type it (\n for RETURN)
 *                             key <n> 0|1          CPC key number down / up
 *                             joy <bits>           joystick 0
 *                             mouse <dx> <dy> <b>  mouse motion, buttons
 *                             shot <file.ppm>      screenshot
 *                             insert <d> <path>    disk into drive d (0 A:, 1 B:)
 *                             eject <d>
 *                             cart <path>          cartridge / ROMs (resets)
 *                             peek <hex addr>      print the byte there
 *                             reset
 *   CPC_STATUS=<n>          print PC and the CRTC every n frames
 *   CPC_TRACE=<pc>[:<n>]    disassemble n instructions from when PC is reached
 *   CPC_WAV=<file>          record the sound (stereo WAV)
 *   CPC_QUIET               no summary
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "cpc.h"
#include "z80.h"

static uint16_t s_frame[CPC_SCREEN_WIDTH * CPC_SCREEN_HEIGHT];

static void write_ppm(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        return;
    }
    fprintf(f, "P6\n%d %d\n255\n", CPC_SCREEN_WIDTH, CPC_SCREEN_HEIGHT * 2);
    uint16_t line[CPC_SCREEN_WIDTH];
    uint8_t rgb[CPC_SCREEN_WIDTH * 3];
    for (int y = 0; y < CPC_SCREEN_HEIGHT; y++) {
        cpc_render_line_rgb565(y, line);
        for (int x = 0; x < CPC_SCREEN_WIDTH; x++) {
            uint16_t c = line[x];
            rgb[x * 3] = ((c >> 11) & 31) * 255 / 31;
            rgb[x * 3 + 1] = ((c >> 5) & 63) * 255 / 63;
            rgb[x * 3 + 2] = (c & 31) * 255 / 31;
        }
        fwrite(rgb, 1, sizeof(rgb), f);
        fwrite(rgb, 1, sizeof(rgb), f);
    }
    fclose(f);
    printf("host: screenshot %s\n", path);
}

static uint8_t *read_file(const char *dir, const char *name, uint32_t *size)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(n);
    if (fread(buf, 1, n, f) != (size_t)n) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    if (size) {
        *size = n;
    }
    return buf;
}

/* ---- Tracing ---- */

static uint16_t s_trace_pc;
static int s_trace_left;
static bool s_tracing, s_trace_on;

void z80_host_hook(void)
{
    if (!s_trace_on) {
        return;
    }
    if (!s_tracing && z80.pc.w == s_trace_pc && s_trace_left > 0) {
        s_tracing = true;
    }
    if (s_tracing) {
        char buf[64];
        z80_disassemble(z80.pc.w, buf, sizeof(buf));
        printf("  %04X  %-20s AF=%04X BC=%04X DE=%04X HL=%04X IX=%04X SP=%04X\n", z80.pc.w, buf,
               z80.af.w, z80.bc.w, z80.de.w, z80.hl.w, z80.ix.w, z80.sp.w);
        if (--s_trace_left <= 0) {
            s_tracing = false;
        }
    }
}

/* ---- Script ---- */

typedef struct {
    uint32_t frame;
    char cmd[16];
    char arg[512];
} line_t;

static line_t *s_script;
static int s_lines, s_next;

static void load_script(const char *path)
{
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        fprintf(stderr, "no script %s\n", path);
        exit(1);
    }
    s_script = calloc(4096, sizeof(line_t));
    char buf[600];
    while (s_lines < 4096 && fgets(buf, sizeof(buf), f)) {
        line_t *l = &s_script[s_lines];
        int at = 0;
        if (buf[0] == '#' || sscanf(buf, "%u %15s %n", &l->frame, l->cmd, &at) < 2) {
            continue;
        }
        snprintf(l->arg, sizeof(l->arg), "%s", buf + at);
        l->arg[strcspn(l->arg, "\r\n")] = 0;
        s_lines++;
    }
    fclose(f);
}

/* "\n" in the text is RETURN */
static void unescape(char *s)
{
    char *d = s;
    for (; *s; s++) {
        if (s[0] == '\\' && s[1] == 'n') {
            *d++ = '\n';
            s++;
        } else {
            *d++ = *s;
        }
    }
    *d = 0;
}

static void run_script(uint32_t frame)
{
    while (s_next < s_lines && s_script[s_next].frame <= frame) {
        line_t *l = &s_script[s_next++];
        if (strcmp(l->cmd, "type") == 0) {
            unescape(l->arg);
            cpc_type(l->arg);
        } else if (strcmp(l->cmd, "key") == 0) {
            unsigned key = 0, down = 0;
            sscanf(l->arg, "%u %u", &key, &down);
            cpc_key(key, down);
        } else if (strcmp(l->cmd, "joy") == 0) {
            cpc_joystick(strtoul(l->arg, NULL, 0));
        } else if (strcmp(l->cmd, "mouse") == 0) {
            int dx = 0, dy = 0, b = 0;
            sscanf(l->arg, "%d %d %d", &dx, &dy, &b);
            cpc_mouse(dx, dy, b);
        } else if (strcmp(l->cmd, "shot") == 0) {
            write_ppm(l->arg);
        } else if (strcmp(l->cmd, "insert") == 0) {
            int d = 0, at = 0;
            sscanf(l->arg, "%d %n", &d, &at);
            if (!cpc_disk_insert(d, l->arg + at)) {
                printf("host: can't insert %s\n", l->arg + at);
            }
        } else if (strcmp(l->cmd, "eject") == 0) {
            cpc_disk_eject(atoi(l->arg));
        } else if (strcmp(l->cmd, "cart") == 0) {
            if (!cpc_cart_insert(l->arg)) {
                printf("host: can't use %s\n", l->arg);
            }
        } else if (strcmp(l->cmd, "peek") == 0) {
            unsigned a = 0;
            sscanf(l->arg, "%x", &a);
            printf("host: frame %u: &%04X = &%02X\n", frame, a, cpc_peek(a));
        } else if (strcmp(l->cmd, "reset") == 0) {
            cpc_reset();
        } else {
            printf("host: unknown script command %s\n", l->cmd);
        }
    }
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <464|664|6128|464plus|6128plus> [frames] [out.ppm] [ram_kb]\n", argv[0]);
        return 1;
    }
    static const char *const names[] = { "464", "664", "6128", "464plus", "6128plus" };
    int model = -1;
    for (int i = 0; i < CPC_MODEL_COUNT; i++) {
        if (strcmp(argv[1], names[i]) == 0) {
            model = i;
        }
    }
    if (model < 0) {
        fprintf(stderr, "unknown model %s\n", argv[1]);
        return 1;
    }
    int frames = argc > 2 ? atoi(argv[2]) : 250;
    const char *out = argc > 3 ? argv[3] : NULL;
    uint32_t ram_kb = argc > 4 ? atoi(argv[4]) : (model == CPC_MODEL_464 || model == CPC_MODEL_664 ? 64 : 128);
    const char *dir = getenv("CPC_ROMS") ? getenv("CPC_ROMS") : "../../../roms";

    static const char *const os[] = { "os464.rom", "os664.rom", "os6128.rom" };
    static const char *const basic[] = { "basic10.rom", "basic664.rom", "basic11.rom" };
    cpc_config_t cfg = {
        .model = model,
        .ram = malloc(ram_kb * 1024),
        .ram_size = ram_kb * 1024,
        .frame = s_frame,
    };
    if (cpc_model_is_plus(model)) {
        uint32_t size;
        uint8_t *cpr = read_file(dir, "system.cpr", &size);
        uint8_t *pages = malloc(CPC_CART_MAX);
        if (cpr != NULL) {
            cfg.system_cart_size = cpc_cpr_pages(cpr, size, pages);
        } else {
            /* OS, BASIC and AMSDOS as pages 0, 1, 3 */
            uint8_t *o = read_file(dir, "osplus.rom", NULL), *b = read_file(dir, "basicplus.rom", NULL);
            uint8_t *a = read_file(dir, "amsdos.rom", NULL);
            memset(pages, 0xFF, 4 * CPC_ROM_SIZE);
            if (o && b) {
                memcpy(pages, o, CPC_ROM_SIZE);
                memcpy(pages + CPC_ROM_SIZE, b, CPC_ROM_SIZE);
                if (a) {
                    memcpy(pages + 3 * CPC_ROM_SIZE, a, CPC_ROM_SIZE);
                }
                cfg.system_cart_size = 4 * CPC_ROM_SIZE;
            }
        }
        cfg.system_cart = pages;
    } else {
        cfg.os = read_file(dir, os[model], NULL);
        cfg.basic = read_file(dir, basic[model], NULL);
        cfg.amsdos = read_file(dir, "amsdos.rom", NULL);
    }
    if (!cpc_init(&cfg)) {
        fprintf(stderr, "cpc_init failed (ROMs in %s?)\n", dir);
        return 1;
    }
    cpc_draw_every_frame(!getenv("CPC_SKIP"));
    for (int d = 0; d < CPC_DRIVES; d++) {
        const char *path = getenv(d ? "CPC_DISKB" : "CPC_DISKA");
        if (path && !cpc_disk_insert(d, path)) {
            fprintf(stderr, "can't insert %s\n", path);
        }
    }
    if (getenv("CPC_CART") && !cpc_cart_insert(getenv("CPC_CART"))) {
        fprintf(stderr, "can't use %s\n", getenv("CPC_CART"));
    }
    if (getenv("CPC_SCRIPT")) {
        load_script(getenv("CPC_SCRIPT"));
    }
    const char *tr = getenv("CPC_TRACE");
    if (tr) {
        unsigned pc = 0;
        s_trace_left = 200;
        sscanf(tr, "%x:%d", &pc, &s_trace_left);
        s_trace_pc = pc;
        s_trace_on = true;
    }
    int status_every = getenv("CPC_STATUS") ? atoi(getenv("CPC_STATUS")) : 0;
    FILE *wav = getenv("CPC_WAV") ? fopen(getenv("CPC_WAV"), "wb") : NULL;
    uint32_t wav_frames = 0;
    if (wav) {
        cpc_audio_enable(true);
        cpc_init(&cfg);
        cpc_draw_every_frame(!getenv("CPC_SKIP"));
        fseek(wav, 44, SEEK_SET);
    }

    clock_t start = clock();
    for (int f = 0; f < frames; f++) {
        run_script(f);
        cpc_run_frame();
        if (wav) {
            int16_t buf[1024 * 2];
            int n;
            while ((n = cpc_audio_render(buf, 1024)) > 0) {
                fwrite(buf, 4, n, wav);
                wav_frames += n;
            }
        }
        if (status_every && (f + 1) % status_every == 0) {
            cpc_status_t s;
            cpc_get_status(&s);
            printf("frame %d: PC &%04X mode %d R1 %d R6 %d R12/13 %02X%02X%s\n", f + 1, s.pc, s.mode,
                   s.crtc[1], s.crtc[6], s.crtc[12], s.crtc[13], s.asic_unlocked ? " ASIC" : "");
        }
    }
    double secs = (double)(clock() - start) / CLOCKS_PER_SEC;
    if (cpc_disk_dirty()) {
        cpc_disk_flush();
    }
    if (!getenv("CPC_QUIET")) {
        cpc_status_t s;
        cpc_get_status(&s);
        printf("%d frames in %.2f s (%.0f%% of real time), PC &%04X\n", frames, secs,
               frames / 50.08 / secs * 100, s.pc);
        cpc_disk_status_t ds;
        cpc_disk_status(&ds);
        for (int d = 0; d < CPC_DRIVES; d++) {
            if (cpc_disk_path(d)) {
                printf("drive %c: %u sectors read, %u written\n", 'A' + d, ds.reads[d], ds.writes[d]);
            }
        }
    }
    if (out) {
        write_ppm(out);
    }
    if (wav) {
        uint32_t data = wav_frames * 4, rate = CPC_AUDIO_HZ, bytes = rate * 4;
        uint8_t h[44] = "RIFF....WAVEfmt \x10\0\0\0\x01\0\x02\0........\x04\0\x10\0data....";
        uint32_t riff = 36 + data;
        memcpy(h + 4, &riff, 4);
        memcpy(h + 24, &rate, 4);
        memcpy(h + 28, &bytes, 4);
        memcpy(h + 40, &data, 4);
        fseek(wav, 0, SEEK_SET);
        fwrite(h, 1, 44, wav);
        fclose(wav);
    }
    return 0;
}
