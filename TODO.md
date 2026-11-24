# esp32cpc roadmap

Proposed work in priority order. See [Development](README.md#development) for the current project.

1. **Run the Plus workflow on the Tab5.** Boot the system cartridge, drive
   Burnin’ Rubber with a physical keyboard, and capture a raster split and
   sprite checkpoint against the host. Check the speaker, then use a scratch
   6128 disk to verify SAVE, eject, remount and LOAD.

2. **Compare CRTC behavior with a small diagnostic.** Use the CRTC path in
   `components/cpc/video.c` to compare register readback, display width, sync
   and split timing for types 0, 1 and 2. Record which differences affect
   actual software before choosing the first selectable variant.

3. **Sketch a CDT pulse player for the 464.** Start with standard data, pauses
   and a turbo block. Verify a 464 ROM load and a custom-loader pilot against a
   reference pulse trace, and test a truncated block without disrupting the
   current disk and cartridge paths.
