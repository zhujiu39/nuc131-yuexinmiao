# yuexinmiao: NUC131 + GD25Q64 Animation Player

An open-source bare-metal animation player validated on real hardware. A NUC131SD2AE reads
delta-compressed RGB565 frames from a GD25Q64 SPI NOR Flash and streams them to a 128x128 LCD
at 30 fps through a GPIO-based serial interface.

## Highlights

- NUC131SD2AE Cortex-M0 at 48 MHz.
- GD25Q64 over SPI0 Mode 0 at 10 MHz.
- 128x128 RGB565 LCD.
- Custom YXMV format: per-frame changed rectangle plus PackBits RLE.
- Runtime checks for JEDEC ID, header CRC32, index CRC32, frame bounds, and decode bounds.
- Python pack generator and full-image validator.
- Keil MDK 5 project with a minimal vendored Nuvoton BSP subset.
- Ready-to-flash MCU and 8 MiB GD25Q64 images.
- Hardware playback verified with 1210 frames at 30 fps.

## Quick start

1. Program `release/GD25Q64/release_yuexinmiao_gd25q64_8MiB.bin` at external Flash address
   `0x000000` and verify all 8,388,608 bytes.
2. Program `release/MCU/release_yuexinmiao.hex` into NUC131 APROM.
3. Power-cycle the board.

Open `firmware/KEIL/yuexinmiao.uvprojx` to rebuild the MCU firmware.

The original cat MP4 is intentionally excluded. The generated Flash image is included as requested
by the repository owner. See [third-party notices](THIRD_PARTY_NOTICES.md) before redistribution.

Original project code is MIT-licensed. Nuvoton BSP and ARM CMSIS files retain their upstream terms.

