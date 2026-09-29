#pragma once
// Included inside gt. Native panel size stays 172x320; logical dimensions rotate.
// Keep creature geometry in native pixels. Only habitats and scenery are laid out.
static constexpr int FRAME_PIXELS = 172 * 320, MAX_SIDE = 320;
static constexpr int BLOOM_CELLS = 44 * 80, CAUSTIC_CELLS = 160 * 23;
static bool landscape = false;
static int W = 172, H = 320, BW = 44, BH = 80;
static int VCX = 86, VCY = 150, CAUS_TOP = 236, CGW = 86, CGH = 42;
static constexpr float SURFACE = 9.f;
static float WATER_BOT = 278.f, RAY_BOT = 258.f, depthScale = 1.f;
static inline float layoutX(float x) { return landscape ? x * (320.f / 172.f) : x; }
static inline int layoutXi(int x) { return landscape ? x * 320 / 172 : x; }
static inline float referenceX(float x) { return landscape ? x * (172.f / 320.f) : x; }
static inline float layoutY(float y) {
  if (!landscape || y <= SURFACE) return y;
  return y <= 278.f ? SURFACE + (y - SURFACE) * depthScale : H - (320.f - y);
}
static inline int layoutYi(int y) {
  if (!landscape || y <= 9) return y;
  return y <= 278 ? 9 + (y - 9) * 121 / 269 : H - (320 - y);
}
static void configureLayout(bool horizontal) {
  landscape = horizontal; W = horizontal ? 320 : 172; H = horizontal ? 172 : 320;
  WATER_BOT = H - 42.f; depthScale = (WATER_BOT - SURFACE) / 269.f;
  RAY_BOT = layoutY(258.f); VCX = W / 2; VCY = H * 150 / 320;
  BW = W / 4 + 1; BH = H / 4;
  CAUS_TOP = H * 59 / 80; CGW = W / 2; CGH = (H - CAUS_TOP + 1) / 2;
}
