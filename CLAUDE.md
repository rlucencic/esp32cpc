# esp32cpc: Amstrad CPC 464 / 664 / 6128 / 464 Plus / 6128 Plus on the Makerfabs MaTouch ESP32-S3 7"

An Amstrad CPC (Z80, Gate Array, CRTC, AY, uPD765; the Plus ASIC) on an
ESP32-S3 board with a 1024x600 touch LCD. Bluetooth keyboard and optional
mouse, disk images in A: and B:, a cartridge slot for Plus cartridges and
expansion ROMs, an LVGL setup screen, a web page for images and live status.
ESP-IDF v5.5.4, C.

The board side (tasks, LCD, touch, SD, WiFi, BLE, setup screen, web page,
console) is the shared library in `../esp32emu`, also used by `../esp32ste`
(Atari STE) and `../esp32mac` (Macintosh). See its CLAUDE.md for the runtime
and the board-level lessons; changes there need all three projects built.
The CPC has its own Z80 core here (`components/cpc/z80.c`); esp32emu's
Musashi isn't used.

## Hardware

Makerfabs MaTouch ESP32-S3 Parallel TFT with Touch 7", ESP32-S3-WROOM-1-N16R8
(16 MB flash, 8 MB octal PSRAM), 1024x600 RGB565 panel, GT911 touch, microSD,
MAX98357A amplifier. Pins: `../esp32emu/components/emu/include/emu/board.h`.
The native USB port is the console, `/dev/cu.usbmodem1101` on the user's Mac.

## Build, flash, debug

Build like the others (the shell's default Python environment doesn't match):

```sh
bash -c '. $IDF_PATH/export.sh >/dev/null 2>&1; \
  export IDF_PYTHON_ENV_PATH=/Users/robertlucencic/.espressif/tools/python/v5.5.4/venv; \
  export PATH=$IDF_PYTHON_ENV_PATH/bin:$PATH; \
  idf.py build && idf.py -p /dev/cu.usbmodem1101 app-flash'
```

- `app-flash` writes only the app; `idf.py flash` also writes the ROMs in
  `roms/` (not in git) into 16 KB slots of the `cpc_roms` partition (slot
  order in the top `CMakeLists.txt` and `main/rom_store.c`; `system.cpr`
  from slot 10). That partition overlaps the Mac's and the ST's ROM
  partitions: switching the board back to one of them needs a full
  `idf.py flash` there. NVS (WiFi, BLE pairings) is shared.
- `../esp32emu/tools/emu_console.py PORT log N | screenshot F.png | lcd F.png | send "cmd" ... | info | reset`
  with the IDF venv's Python (`-I`). One touch per `send` call.
- `curl http://amstrad.local/api/status`; disks: see `../esp32emu/README.md`.
- Flashing restarts the board and ends the user's session: say so first.

Console commands: the shared ones (`s` `L` `r` `F` `t` `i`, see
`emu/console.h`) and the CPC's (`main/main.c`): `k <n>` tap CPC key number
n (line * 8 + bit, 18 = RETURN), `K <n> 0|1`, `T <text>` type (`\n` =
RETURN), `j <bits>` joystick, `m dx dy btn` mouse, `R <model>` restart as
0 464 .. 4 6128 Plus.

## Layout

| Path | What |
|---|---|
| `main/main.c` | The CPC as an `emu_machine_t`: memory, start, key map, joystick, drives, status, console |
| `main/rom_store.c` | ROMs from the `cpc_roms` partition slots, else the SD card's `roms/` |
| `main/settings.c` | NVS `cpc`: `model` (the RAM follows the model) |
| `components/cpc/` | The CPC, platform independent (`include/cpc.h`) |
| `components/cpc/host/` | Host build: `cpc_host` and `z80_test` (see Testing) |
| `roms/` | `os464.rom` ... `system.cpr` (see `roms/README.md`; not in git) |
| `partitions.csv` | nvs, phy, factory 8 MB, cpc_roms 512K (type 0x40), storage |

## The core (`components/cpc`)

- `z80.c`: own Z80, passes zexdoc and zexall (all 67). One switch per page;
  DD/FD reuse the unprefixed code with IX/IY in place of HL. Times are CPC
  microseconds (tables from Caprice32's cycle tables / 4: every M-cycle
  waits for the Gate Array's 1 us grid). `cpc_in/out(port, v, at)` get the
  microsecond inside the instruction where the access happens (OUT (C),r at
  3 of 4, OUTI at 4 of 5, IN at the end); the machine advances the video to
  that point first. IM 1 interrupt 5 us, IM 2 7 us; IM 0 reads $FF (RST 38).
- `cpc.c`: memory map (16 KB read/write bank pointers, 6128 RAM configs and
  512 KB expansion banks; a NULL write bank = the ASIC page), I/O decoding
  by address bits (GA A15=0/A14=1, RAM config A15=0 with %11xxxxxx, CRTC
  A14=0, ROM select A13=0, PPI A11=0, FDC A10=0/A7=0, Symbiface II &FD1x),
  the PPI, the interrupt line, the frame loop (19968 us), Symbiface II RTC
  (host local time, BCD or binary per register B).
- `video.c`: CRTC counters (hcc, vcc, ra, adjust), Gate Array (palette,
  modes switched at HSYNC end, the 52-line interrupt counter and its VSYNC
  resync), a monitor that locks onto HSYNC (window starts 14 chars after it)
  and VSYNC (20 lines after it), window 48 chars x 300 lines. Characters in
  the window are drawn into a line buffer; at the next HSYNC the line goes
  into the PSRAM frame if its hash changed. Stretches without CRTC events are
  done in one loop (`quiet_chars`); only every other picture is drawn.
  Plus: sprites over the screen area at line commit, split screen (SSSL:
  the line after it starts at SSA), soft scroll, PRI at HSYNC end.
- `asic.c`: Plus unlock sequence (on CRTC register select writes), register
  page &4000-&7FFF (RMR2 %10111xxx maps it), sprites, 12-bit palette (GA
  colour writes store their 12-bit equivalents there too), DMA (one
  instruction per channel per HSYNC; LOAD writes the AY), interrupt vectors
  (raster 6, DMA2 0, DMA1 2, DMA0 4).
- `psg.c`: AY through the PPI (port C bits 7-6 = BDIR/BC1), register 14 =
  keyboard line; event queue to the render side like the ST's YM (32 us per
  sample, 31250 Hz).
- `fdc.c`: uPD765, non-DMA, no rotation timing; reads end with "end of
  cylinder" like on a CPC (no terminal count). DSK/EDSK parsed into tracks
  in memory; `cpc_disk_flush()` writes changed tracks in place, or the whole
  file as EDSK after a format.
- `cart.c`: `.cpr` (RIFF AMS!, cbNN chunks) or expansion ROMs into slots
  1-6, 8-15 (NAME_1.ROM brings NAME_2.ROM ...). Inserting resets.
- `input.c`: input queue from other tasks, matrix, joystick, AMX mouse (one
  step per row 9 selection), Symbiface II mouse FIFO, `cpc_type()`.

Plus memory: cartridge page 0 is the lower ROM (RMR2 moves it to &4000 or
&8000), upper ROM 7 = page 3, %1xxxxxxx = page n & 31, anything else page
1; expansion ROMs in slots 1-15 (not 7) override.

## Persistent state

- NVS `cpc`: `model`. NVS `cpcdisks`: `disk_a`..`disk_c` (A:, B:, cartridge).
- SD card: `cpc/` (images, cartridges, ROMs; made if missing), `roms/`
  (optional system ROMs).

## Memory budget (measured)

- PSRAM: RAM 128 KB, three ROM buffers, the 512 KB system cartridge, the
  768x300 frame (450 KB), all at power-on. ~4 MB PSRAM free while running.
- Internal RAM: ~65 KB free while running, with a disk in A: too (min ~22
  KB during web uploads, largest block 30 KB).
- Core 1: ~60% busy at the BASIC prompt at 100% speed (video ~1.65M of
  4.8M cycles per frame, the rest mostly the Z80).

## Lessons

- **The IDF compiles with `-fno-jump-tables`**: the Z80's switches became
  compare chains. `components/cpc/CMakeLists.txt` turns jump tables back on
  for the core (the emulation task is stalled during flash writes anyway).
- **Stepping the CRTC per microsecond cost ~78 cycles each** (20,000 a
  frame): the first board build ran at 94%. Batching the quiet stretches
  and drawing every other picture got it to 100% with room to spare.
- **A rotate-xor line hash cancels out on uniform lines** (rotating by 5
  over 192 words visits every rotation an even number of times): blank blue
  lines hashed like black ones and never reached the frame. FNV it is.
- **Plain `malloc()` takes small blocks from internal RAM**: each disk
  track (~4.6 KB) did, a CP/M disk left 19 KB free with a 7 KB largest
  block, and web uploads failed ("Can't write to the SD card": no 8 KB DMA
  buffer). The core allocates through `cpc_malloc()` (PSRAM) now.
- On the CPC, typing while BASIC prints (LIST) pauses the output and eats
  the key: host scripts must wait for the output to end.
- The 6128 Plus system cartridge's page 3 is AMSDOS 0.7 with the start-up
  menu (f1 BASIC, f2 Burnin' Rubber); built from osplus/basicplus/amsdos
  instead, the Plus starts in BASIC.

## Testing

- **Host build**: `make -C components/cpc/host`, then
  `build/cpc_host <464|664|6128|464plus|6128plus> [frames] [out.ppm]`
  (picture doubled to 768x600). Environment: `CPC_ROMS=<dir>` (default
  `../../../roms`), `CPC_DISKA/B=<dsk>`, `CPC_CART=<cpr|rom>`,
  `CPC_SCRIPT=<file>` (lines `<frame> type <text> | key <n> 0|1 | joy <bits>
  | mouse dx dy b | shot <ppm> | insert <d> <path> | eject <d> | cart <path>
  | peek <hex> | reset`), `CPC_STATUS=<n>`, `CPC_TRACE=<pc>[:<n>]`,
  `CPC_WAV=<file>`, `CPC_SKIP` (draw every other picture, as on the board),
  `CPC_QUIET`. `build/z80_test <prelim.com|zexdoc.cim|zexall.cim>` (from
  github.com/superzazu/z80 roms/; ~30 s each).
- Verified on the host: all five models boot (464 BASIC 1.0, 664/6128 BASIC
  1.1, Plus menu and BASIC); 6128 SAVE/CAT/LOAD on a blank disk made by
  `cpc_disk_create`; Burnin' Rubber from the system cartridge (title,
  race: sprites, 12-bit colours, raster splits); VincentGR expansion-ROM
  games `|BOUL` (Boulder Dash) and the two-part `|ARKA` (Arkanoid).
- On the board: 6128 boots to BASIC at 100% speed (50 fps); setup screen,
  WiFi, clock, web status.
- Not verified yet: sound on the speaker, BLE keyboard/mouse on the CPC,
  disks/cartridges through the web page and the setup screen on the board,
  the Plus on the board, SymbOS (SF2 mouse/RTC), AMX mouse programs.

## Open items

- No tape (`.cdt`), printer, CRTC type differences, FDC timing.
- Touch screen unused (only the setup corner).

## README and GitHub

Public at `github.com/rlucencic/esp32cpc` (pushed to `main`), with topics
and an About text like esp32ste's and esp32mac's.

## README images

In `docs/images/` (how they're made: `../esp32emu/CLAUDE.md`, README
screenshots; `screenshot` reports 768x300 and saves it as 768x600):

| Image | What |
|---|---|
| `basic-6128.png` | The 6128 starting up in BASIC 1.1 (board) |
| `cpm22.png` | CP/M 2.2 after `\|CPM`, `DIR` (board) |
| `cpm-plus.png` | CP/M Plus v1.0, 61K TPA, `DIR` (board) |
| `web.png` | Web page with example data, after dropping a ZIP of two `.dsk` (local node server, as in esp32emu's notes; Chrome's upload tool only takes files under esp32mac/esp32ste/esp32emu, so stage them in `../esp32mac/build/`) |
| `basic.png`, `plus-menu.png`, `burnin-rubber-title.png`, `burnin-rubber.png`, `boulder-dash-rom.png` | Host build renders; not in the README |

## Working with the user

- Expects changes built, flashed and checked on the board, then a short
  report of what was verified and what wasn't.
- Keep their private data (WiFi name, IP, SD card contents) out of anything
  meant for publishing. Screenshots: a new file each time, never over an
  existing one.
- Model choice decides the RAM (64 or 128 KB): no separate memory setting
  (the user asked for none).
