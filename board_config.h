// Waveshare ESP32-C6-LCD-1.47: non-touch ST7789 board only.
#pragma once
#define BOARD_NAME "ESP32-C6-LCD-1.47"
#define LCD_DC 15
#define LCD_CS 14
#define LCD_SCK 7
#define LCD_MOSI 6
#define LCD_MISO GFX_NOT_DEFINED
#define LCD_RST 21
#define LCD_BL 22
#define LCD_IPS true
#define LCD_ROTATION 0
#define LCD_SWAP_RB 0
#define PIN_BOOT 9
#define PIN_AUX_HIGH 4
#define HAS_RGB_LED 1
#define PIN_RGB_LED 8
// Physical panel dimensions, even when its logical orientation is landscape.
#define LCD_W 172
#define LCD_H 320
#define LCD_COL_OFF 34
#define LCD_ROW_OFF 0
#ifndef BACKLIGHT_LEVEL
#define BACKLIGHT_LEVEL 50
#endif
#ifndef LCD_SPI_HZ
#define LCD_SPI_HZ 80000000UL
#endif
// 1 = 7 Pokemon (default). Optional 2 = 14, 3 = 21; denser populations.
#ifndef GT_POKEMON_COPIES
#define GT_POKEMON_COPIES 1
#endif
