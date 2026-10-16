# Amstrad CPC ROM images

Put the ROM images here (not part of this repository). `idf.py flash` writes
them into 16 KB slots of the `cpc_roms` flash partition; only the ones for the
models you want are needed.

| File | What | Models |
|---|---|---|
| `os464.rom` | OS (lower ROM) of the 464 | 464 |
| `basic10.rom` | BASIC 1.0 | 464 |
| `os664.rom` | OS of the 664 | 664 |
| `basic664.rom` | BASIC 1.1 of the 664 | 664 |
| `os6128.rom` | OS of the 6128 | 6128 |
| `basic11.rom` | BASIC 1.1 | 6128 |
| `amsdos.rom` | AMSDOS, the disc ROM (upper ROM 7) | 464, 664, 6128 (a 464 gets it like a DD1 drive) |
| `system.cpr` | The Plus system cartridge: OS, BASIC 1.4, AMSDOS, Burnin' Rubber | 464 Plus, 6128 Plus |
| `osplus.rom`, `basicplus.rom` | Plus OS and BASIC, used (with `amsdos.rom`) when there's no `system.cpr` | 464 Plus, 6128 Plus |

All are 16 KB, except `system.cpr` (a RIFF cartridge file, 128 KB of pages).

Without flashing, the emulator also finds them in a `roms` folder on the SD
card under the same names.

Amstrad allowed its CPC ROMs to be distributed with emulators, as long as the
copyright messages stay intact; `amsdos.rom` and `system.cpr` are the ones
that come with Caprice32.
