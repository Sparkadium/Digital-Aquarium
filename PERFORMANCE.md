# Verification — dual orientation, seven Pokémon

## Target and memory

Built successfully for `esp32:esp32:esp32c6:CDCOnBoot=cdc`, Arduino ESP32 **3.3.0**, GFX Library for Arduino **1.5.9**, 160 MHz, 4 MB flash, default partitions.

| Quantity | This release |
|---|---:|
| Program storage reported by compiler | 496,411 bytes |
| Global/static RAM reported by compiler | 217,568 bytes |
| Global/static RAM increase over previous release | 2,952 bytes |
| Framebuffer, reused in both orientations | 110,080 bytes |
| Optional existing backdrop cache, on heap | 110,080 bytes |
| Full flash image | 4,194,304 bytes |
| Application-only image | 496,560 bytes |

The extra static storage accommodates 320-wide terrain/raster tables, caustic capacity and orientation/button state. Creature storage remains a union shared by the mutually exclusive modes. There is no additional full framebuffer and no per-frame heap allocation. Rotation rebuilds procedural scenery once, outside DMA.

The compiler reports 110,112 bytes remaining in its dynamic-memory summary. That is **not measured free runtime heap**. The startup message reports whether the optional backdrop cache was allocated; serial statistics report actual device free heap. Both cache and no-cache paths were tested.

## Frame-rate implications

Both views are 55,040 RGB565 pixels. The ideal payload time at the configured **80 MHz SPI** remains **11.008 ms**, excluding driver overhead. ST7789 address traversal performs rotation; the CPU does not transpose the image. Normal background refresh updates eight portrait rows or four landscape rows per frame, keeping its pixel work similar. Simulation and background preparation still overlap the four DMA transactions.

No physical board was available for measuring this release. A 30 FPS guarantee cannot be inferred from compilation or native host tests. The previous 14/21-Pokémon versions ran well according to the user's hardware feedback; seven is now the default to reduce crowding. Serial reports average FPS, per-stage timing, orientation, creature count, longest measured frame work, work frames over 33.3 ms and runtime heap. Rotation/mode transitions may produce a longer frame because the backdrop is rebuilt once.

## Checks completed

- **Ten mode/orientation combinations:** 8,000 variable-duration simulation steps plus 500 warm-up frames each; feeding and rendering throughout. All positions remained finite and bounded; no geometry command overflow. Every Pokémon species ate in both views.
- **Live rotation:** 20 round-trip exercises (40 orientation changes) during those runs. The selected mode, population, hunger/cooldowns, phases, Pokémon meals, food identities/lifetimes and total consumption persist. Flexible community-fish spines retain their shape. The backdrop and caustic caches are invalidated correctly.
- **7,250 portrait comparison frames:** all five modes matched the previous version's pixels and consumption counts exactly at seven Pokémon. Includes variable frame intervals, feeding, lighting changes, plant reseeding, cache and no-cache paths. Reference headers and a reproduction script are included.
- **BOOT input:** tap/wafer/mode thresholds, switch bounce, five-second one-shot rotation, release suppression, startup-held suppression, missed-frame handling and 32-bit millisecond wrap. Two-second mode changes occur on release; a five-second hold cannot also change modes.
- **Memory safety:** AddressSanitizer and UndefinedBehaviorSanitizer passed the orientation suite. A separate sanitizer smoke check covers both orientations with optional bloom and trails enabled. LeakSanitizer was disabled because this host does not support it; these tests do not measure C6 stack or heap use.
- **DMA:** sentinel checks show scene rotation and cache preparation leave the active framebuffer unchanged. The bus harness passed ownership, alignment, transfer sizes, pixel order, queue failure and cleanup checks.
- **Display geometry:** inspected the pinned GFX driver: rotation 1 swaps logical width/height and uses offsets (0,34); portrait uses (34,0). Both frame-submission paths use the logical width/height while the constructor retains physical 172×320 dimensions. Hardware appearance remains to be checked on the device.
- **Extended-coordinate check:** creatures at x=290 still excite Abyss plankton correctly; unsigned Q7 positions support the entire landscape width.
- **Transitions/fallback:** repeated mode changes in both views with the backdrop cache disabled; identical stable cached/uncached Abyss and Pokémon renders.
- **Labels:** the existing pixel-mask test for J and the centered COMB JELLIES label passes.
- **Firmware:** esptool identifies ESP32-C6 and validates the application checksum/hash. The complete image is 4 MB and contains the identical application at 0x10000. Build log, image details and SHA-256 hashes are included.
- **Visual review:** the PNG previews are rendered by the actual C++ firmware, at native resolution, rather than a separate mockup renderer.

## Reproduce

From this package directory, with g++ and Python 3:

```sh
bash extras/run_checks.sh
```

For the sanitizer checks:

```sh
g++ -O1 -g -std=c++11 -fsanitize=address,undefined -fno-omit-frame-pointer extras/test_orientation.cpp -o orientation_asan
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 ./orientation_asan
g++ -O0 -g -std=c++11 -DGT_BLOOM=1 -DGT_TRAILS=1 -fsanitize=address,undefined -fno-omit-frame-pointer extras/test_optional.cpp -o optional_asan
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 ./optional_asan
```

With the target dependencies installed:

```sh
arduino-cli compile --fqbn esp32:esp32:esp32c6:CDCOnBoot=cdc Glowtank
```

Actual screen orientation, colors, sustained FPS, cache allocation and button feel still require a physical-device check. This package has not been flashed onto the user's board here.
