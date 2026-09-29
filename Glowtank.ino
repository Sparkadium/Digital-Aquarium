/* Glowtank — a glowing-particle community aquarium.
   Waveshare ESP32-C6-LCD-1.47 (non-touch), selected in board_config.h.
   Arduino ESP32 core 3.x, GFX Library for Arduino 1.5.9. No PSRAM, LVGL, SD or Wi-Fi.

   BOOT tap ............ pinch of flakes
   BOOT hold 0.55 s .... sinking algae wafer; normal food in Abyss / Pokemon
   BOOT release 2–5 s .. Realistic -> Fantasy -> Comb Jellies -> Abyss -> Pokemon (remembered)

   BOOT hold 5 s ...... portrait / landscape (remembered; no mode change)

   Serial (115200) prints frame rate and a per-stage timing breakdown every 2 s.
*/
#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <Preferences.h>
#include <SPI.h>
#include "driver/spi_master.h"
#include "board_config.h"
#include "tank.h"
#include "boot_gesture.h"

#include "lcd_dma.h"
Arduino_DataBus *bus = nullptr;
Arduino_TFT *gfx = nullptr;
static GlowtankDmaBus *dmaBus = nullptr;
Preferences prefs;

#ifndef FRAME_MIN_MS
#define FRAME_MIN_MS 0          // no frame cap; set e.g. 33 to hold ~30 fps
#endif
#ifndef STATS_ON_SCREEN_S
#define STATS_ON_SCREEN_S 0     // optional startup timing overlay; serial timing remains enabled
#endif
#ifndef LCD_DMA
#define LCD_DMA 1               // IDF owns SPI from startup; 0 = original Arduino_HWSPI path
#endif
#ifndef LCD_FAST_PUSH
#define LCD_FAST_PUSH 1         // 0 = the Chaos Glass writePixels path (32-pixel chunks)
#endif


// ---------------------------------------------------------------- RGB LED (touchless board)
#if HAS_RGB_LED
static uint32_t ledOffAt = 0;
static bool ledOn = false;
static void ledFlash(uint8_t r, uint8_t g, uint8_t b, uint32_t ms) {
  rgbLedWrite(PIN_RGB_LED, r, g, b);
  ledOn = true; ledOffAt = millis() + ms;
}
static void ledService(uint32_t now) {
  if (ledOn && (int32_t)(now - ledOffAt) >= 0) { rgbLedWrite(PIN_RGB_LED, 0, 0, 0); ledOn = false; }
}
#else
static inline void ledFlash(uint8_t, uint8_t, uint8_t, uint32_t) {}
static inline void ledService(uint32_t) {}
#endif

static void doFeed(float x) { gt::feed(x); ledFlash(40, 22, 0, 220); }
static void doWafer(float x) { gt::dropWafer(x); ledFlash(6, 30, 4, 350); }
#ifndef TANK_LIGHT
#define TANK_LIGHT 0            // 0 day, 1 dusk, 2 night glow, 3 day-night cycle
#endif
static void nextTank() {
  gt::setTheme(gt::theme + 1);
  gt::setLight(gt::themeLight(gt::theme, TANK_LIGHT));
  gt::step(0);  // refresh lighting before the first frame in the new tank
  gt::showLabel(gt::themeName(gt::theme));
  prefs.putUChar("theme", (uint8_t)gt::theme);
  Serial.printf("tank: %s\n", gt::themeName(gt::theme));
}

// DMA setup failure or BOOT held during startup selects the original display driver.
static bool useDma = false;
static Arduino_TFT *makePanel() {
  return new Arduino_ST7789(bus, LCD_RST, LCD_ROTATION, LCD_IPS, LCD_W, LCD_H,
                             LCD_COL_OFF, LCD_ROW_OFF, LCD_COL_OFF, LCD_ROW_OFF);
}
static bool beginPanel() {
#if LCD_DMA
  if (digitalRead(PIN_BOOT) != LOW) {
    dmaBus = new GlowtankDmaBus(); bus = dmaBus;
    gfx = makePanel();
    if (gfx->begin(LCD_SPI_HZ) && dmaBus->ok()) { useDma = true; return true; }
    Serial.printf("DMA init failed (%d); using Arduino SPI\n", (int)dmaBus->error());
    delete static_cast<Arduino_ST7789 *>(gfx); gfx = nullptr;
    delete dmaBus; dmaBus = nullptr;
  }
#endif
  bus = new Arduino_HWSPI(LCD_DC, LCD_CS, LCD_SCK, LCD_MOSI, LCD_MISO);
  gfx = makePanel();
  return gfx->begin(LCD_SPI_HZ);
}
static void displayFault() {
  Serial.printf("Display transfer error %d; restart with BOOT held for Arduino SPI\n", (int)dmaBus->error());
  while (true) delay(100); // Do not overwrite a buffer whose DMA completion is uncertain.
}

// ---------------------------------------------------------------- BOOT button
static BootGesture boot;
static void rotateTank() {
  if (useDma && !dmaBus->waitFrame()) displayFault();
  gt::setOrientation(!gt::landscape);
  // ST7789 changes its address traversal in hardware; no framebuffer transpose.
  gfx->setRotation(gt::landscape ? 1 : LCD_ROTATION);
  gt::step(0);
  gt::showLabel(gt::landscape ? "LANDSCAPE" : "PORTRAIT");
  prefs.putBool("landscape", gt::landscape);
  ledFlash(0, 24, 30, 350);
  Serial.printf("orientation: %s (%dx%d)\n", gt::landscape ? "landscape" : "portrait", gt::W, gt::H);
}
static void serviceBoot(uint32_t now) {
  switch (boot.update(digitalRead(PIN_BOOT) == LOW, now)) {
    case BootGesture::FEED: doFeed(-1.f); break;
    case BootGesture::WAFER: doWafer(-1.f); break;
    case BootGesture::MODE_READY: ledFlash(24, 0, 24, 180); break;
    case BootGesture::NEXT_MODE: nextTank(); break;
    case BootGesture::ROTATE: rotateTank(); break;
    default: break;
  }
}

// ---------------------------------------------------------------- setup / loop
void setup() {
  Serial.begin(115200);
  // With "USB CDC On Boot: Enabled" this is the USB port; give the PC a moment to open it.
  for (uint32_t t = millis(); !Serial && millis() - t < 1500;) delay(10);
  Serial.println("\nGlowtank starting");
  pinMode(PIN_AUX_HIGH, OUTPUT); digitalWrite(PIN_AUX_HIGH, HIGH);
  pinMode(PIN_BOOT, INPUT_PULLUP);
  boot.begin(digitalRead(PIN_BOOT) == LOW, millis());
  pinMode(LCD_BL, OUTPUT); digitalWrite(LCD_BL, LOW);

  if (!beginPanel()) { Serial.println("Display init failed"); while (true) delay(100); }
  gfx->setRotation(LCD_ROTATION);
  gfx->fillScreen(0);

  Serial.printf("panel push: %s, %lu MHz\n", useDma ? "DMA" : "Arduino SPI", (unsigned long)(LCD_SPI_HZ / 1000000));
  ledcAttach(LCD_BL, 20000, 8);
  ledcWrite(LCD_BL, BACKLIGHT_LEVEL);


#if HAS_RGB_LED
  rgbLedWrite(PIN_RGB_LED, 0, 0, 0);
#endif

  prefs.begin("glowtank", false);
  uint32_t t0 = millis();
  gt::init(esp_random(), prefs.getBool("landscape", false));
  gfx->setRotation(gt::landscape ? 1 : LCD_ROTATION);
  gt::dayLight = gt::lightMode == 0 ? 1.f : gt::lightMode == 1 ? 0.55f : gt::lightMode == 2 ? 0.2f : 0.8f;
  gt::theme = prefs.getUChar("theme", gt::POKEMON) % gt::THEME_COUNT;
  gt::setLight(gt::themeLight(gt::theme, TANK_LIGHT));
  gt::restart();
  if (useDma) { gt::step(33); gt::prepBase(); gt::basePrepared = true; gt::prepCaustics(); }
  Serial.printf("\nGlowtank on %s — init %lu ms, free heap %lu bytes, %d fish, %d plant segments, backdrop %s\n",
                BOARD_NAME, (unsigned long)(millis() - t0), (unsigned long)ESP.getFreeHeap(), gt::population(), gt::segTotal,
                gt::bgS ? "cached" : "drawn per frame (no heap for the cache)");
  Serial.printf("tank: %s | BOOT on GPIO%d reads %s\n", gt::themeName(gt::theme), PIN_BOOT,
                digitalRead(PIN_BOOT) == LOW ? "LOW (held, or wrong pin)" : "HIGH (released)");
}

static uint32_t lastMs = 0, statAt = 0;
static uint32_t stageUs[7];
static uint32_t frames = 0, blitSum = 0;
static uint32_t worstFrameUs = 0, overBudgetFrames = 0;

void loop() {
  uint32_t now = millis();
  if (!statAt) statAt = now;
  if (FRAME_MIN_MS && now - lastMs < FRAME_MIN_MS) { delay(1); return; }
  uint32_t dt = lastMs ? now - lastMs : 33;
  lastMs = now;

  const uint32_t frameStartUs = micros();
  serviceBoot(now);
  ledService(now);

  uint32_t a = micros();
  // (the simulation step for this frame already ran while the previous frame was being sent)
  if (!useDma) { gt::step(dt); gt::prepBase(); gt::basePrepared = true; gt::prepCaustics(); }
  uint32_t b = micros();
  gt::renderBase();        uint32_t c = micros();
  gt::renderMid();         uint32_t d = micros();
  gt::renderGlow();        uint32_t e = micros();
  gt::renderBloom();       uint32_t f = micros();
  gt::renderFront();       uint32_t g = micros();
  static uint32_t simUs = 0, waitUs = 0;
#if LCD_DMA
  if (useDma) {
    gt::swapForPanel(true, LCD_SWAP_RB);
    gfx->startWrite();
    gfx->writeAddrWindow(0, 0, gt::W, gt::H);
    if (!dmaBus->queueFrame(gt::fb)) displayFault();
    uint32_t s0 = micros();
    gt::step(dt);                                  // runs while the pixels go out
    gt::prepBase(); gt::basePrepared = true;
    gt::prepCaustics();
    uint32_t s1 = micros();
    if (!dmaBus->waitFrame()) displayFault();
    gfx->endWrite();
    simUs = s1 - s0; waitUs = (s0 - g) + (micros() - s1);
  } else
#endif
  {
    gfx->startWrite();
    gfx->writeAddrWindow(0, 0, gt::W, gt::H);
#if LCD_FAST_PUSH
    gt::swapForPanel(true, LCD_SWAP_RB);
    bus->writeBytes((uint8_t *)gt::fb, (uint32_t)LCD_W * LCD_H * 2);
#else
    if (LCD_SWAP_RB) gt::swapForPanel(false, true);
    bus->writePixels(gt::fb, (uint32_t)LCD_W * LCD_H);
#endif
    gfx->endWrite();
    simUs = b - a; waitUs = micros() - g;
  }

  // stage 0 = sim (+backdrop/caustic prep), 6 = time spent only waiting on the panel
  stageUs[0] += simUs; stageUs[1] += c - b; stageUs[2] += d - c; stageUs[3] += e - d;
  stageUs[4] += f - e; stageUs[5] += g - f; stageUs[6] += waitUs;
  const uint32_t frameUs = micros() - frameStartUs;
  if (frameUs > worstFrameUs) worstFrameUs = frameUs;
  if (frameUs > 33333) overBudgetFrames++;
  frames++; blitSum += gt::blitCount;

  uint32_t statsNow = millis();
  if (statsNow - statAt >= 2000) {
    float n = frames ? (float)frames : 1.f;
    float fps = frames * 1000.f / (statsNow - statAt);
    float ms[7]; for (int k = 0; k < 7; k++) ms[k] = stageUs[k] / n / 1000.f;
    Serial.printf("fps %.1f | ms: sim %.1f  base %.1f  mid %.1f  glow %.1f  bloom %.1f  front %.1f  spi-wait %.1f | %u sprites\n",
                  (double)fps, (double)ms[0], (double)ms[1], (double)ms[2], (double)ms[3], (double)ms[4], (double)ms[5], (double)ms[6],
                  (unsigned)(blitSum / (frames ? frames : 1)));
    Serial.printf("%s | %s %dx%d | %d creatures | worst %.1f ms | >33.3ms %lu/%lu | heap %lu\n",
                  gt::themeName(gt::theme), gt::landscape ? "landscape" : "portrait", gt::W, gt::H, gt::population(), (double)(worstFrameUs*.001f),
                  (unsigned long)overBudgetFrames, (unsigned long)frames, (unsigned long)ESP.getFreeHeap());
    if (STATS_ON_SCREEN_S && now < (uint32_t)STATS_ON_SCREEN_S * 1000u) {
      char b[32];
      snprintf(b, sizeof b, "FPS %.1f  SPI %.1f", (double)fps, (double)ms[6]); gt::setOverlay(0, b);
      snprintf(b, sizeof b, "SIM %.1f GLOW %.1f", (double)ms[0], (double)ms[3]); gt::setOverlay(1, b);
      snprintf(b, sizeof b, "BG %.1f MID %.1f FRT %.1f", (double)(ms[1]), (double)ms[2], (double)(ms[4] + ms[5])); gt::setOverlay(2, b);
    } else { gt::setOverlay(0, ""); gt::setOverlay(1, ""); gt::setOverlay(2, ""); }
    memset(stageUs, 0, sizeof(stageUs)); frames = 0; blitSum = 0; statAt = statsNow;
    worstFrameUs = 0; overBudgetFrames = 0;
  }
}
