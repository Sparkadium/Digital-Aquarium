# Glowtank — portrait + landscape

For the **non-touch Waveshare ESP32-C6-LCD-1.47**. One firmware runs **Realistic, Fantasy, Comb Jellies, Abyss and Pokémon** in both **172 × 320 portrait** and **320 × 172 landscape**.

**Seven Pokémon by default**, one each of Tentacool, Chinchou, Magikarp, Horsea, Goldeen, Wooper and Staryu. Their rounded geometry, depth sorting and smooth turns are retained. Plants, creatures and scenery are generated with mathematics on the device. No image assets or generated-art textures are used.

## BOOT controls

| Gesture | Action |
|---|---|
| Tap; release before 0.55 seconds | Feed |
| Hold 0.55–2 seconds, then release | Drop a wafer; ordinary sinking food in Abyss and Pokémon |
| Hold 2–5 seconds, then release | Change tank and remember it |
| Hold for 5 seconds | Toggle portrait/landscape immediately and remember it |

A brief purple LED flash at two seconds means releasing will change tanks. Keep holding to five seconds for rotation; a cyan flash confirms it. **Rotation never also changes the tank or feeds.** You can keep holding after rotation; the next gesture begins only after releasing.

Mode order: **Realistic → Fantasy → Comb Jellies → Abyss → Pokémon**.

Orientation applies to every mode and survives power-off. A fresh installation starts in portrait Pokémon mode. Ordinary Arduino uploads preserve stored settings. The BOOT press already held when the sketch starts is reserved for the existing SPI fallback and is ignored by the tank controls until released.

## What rotates

The display controller changes its address direction in hardware. The scene is laid out for the new width and height: habitats, feeding positions, floor, plants, rocks, rays and particles adjust. Animal shapes stay in native pixels, so they retain their proportions. Food sinks more slowly through the shallower landscape tank, keeping it available for the chase.

Rotating preserves the selected tank, its current inhabitants, animation phases, hunger/cooldowns, Pokémon meal counts, feeding totals and remaining food. Procedural scenery is rebuilt from the same seed. A mode change still loads that mode normally.

## Upload using Arduino IDE

1. Extract the ZIP and replace your previous **Glowtank** sketch folder with the included **Glowtank** folder. Keep the `.ino` and **all ten `.h` files** together.
2. Install **esp32 by Espressif Systems 3.3.0** and **GFX Library for Arduino 1.5.9**, the versions used for this build.
3. Select **ESP32C6 Dev Module**, **USB CDC On Boot: Enabled**, **160 MHz**, **4 MB flash**, default partitions.
4. Open `Glowtank/Glowtank.ino`, select the device port and upload.

To compile with Arduino CLI:

```sh
arduino-cli compile --fqbn esp32:esp32:esp32c6:CDCOnBoot=cdc Glowtank
```

## Precompiled firmware

`firmware/Glowtank-7-dual-orientation-full.bin` is the complete **4 MB ESP32-C6 image**: bootloader, partitions and application. Flash it at **0x0** with an ESP32-C6-compatible flasher. A full-image flash clears saved settings; a normal Arduino sketch upload preserves them. Do not put the full image at an application-only offset.

For an existing installation with the same default partition layout, `firmware/Glowtank-7-dual-orientation-app.bin` contains only the application and goes at **0x10000**. Use the Arduino upload route if unsure of your partition layout. SHA-256 hashes are in `firmware/SHA256SUMS.txt`.

No touch-board build or 14/21-population binaries are included. The optional source setting `GT_POKEMON_COPIES` remains available; its default is **1**.

## Performance and checking the device

The frame rate remains uncapped. Both orientations send the same **55,040 pixels / 110,080 bytes** per frame. The existing framebuffer and optional backdrop cache are reused. There is no software image rotation, second framebuffer, or per-frame heap allocation. Background refresh work remains roughly constant between orientations; simulation continues to overlap SPI DMA.

Serial Monitor at **115200** reports orientation, mode, population, average FPS, stage timings, longest frame work, frames over 33.3 ms and free heap every two seconds. The display has no performance overlay by default. A rotation or mode switch rebuilds the backdrop once, so that transition can take longer than normal frames.

This release compiles for the C6 and passed the included native tests, including feeding, rotation, button thresholds, rendering bounds and DMA-buffer safety. It has **not been flashed or FPS-measured on physical hardware here**. Check a minute of each orientation, including feeding. The previous 14/21 populations ran well on your device; this release returns to seven for a less crowded tank.

See `PERFORMANCE.md` for exact build and verification results. `previews/` contains actual firmware renders of all ten combinations. The original pins, color order, brightness setting and 80 MHz SPI are retained. No Wi-Fi, PSRAM, SD card or touch input is required.
