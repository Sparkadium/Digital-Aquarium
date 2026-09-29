// Glowtank — simulation and renderer.
//
// Plain C++ with no Arduino dependencies, so the same code runs on the
// ESP32-C6 and in a desktop preview harness. The C6 has no floating-point
// unit, so everything that touches pixels is integer: glow sprites are
// precomputed kernels, trig is a table, plants walk in fixed point. Floats
// are kept to the fish logic (a few thousand operations per frame).
//
// Frame pipeline (the sketch calls these in order and times each one):
//   step(dt)          simulation
//   renderBase()      static backdrop (sliced re-bake) copied into fb
//   renderMid()       caustics, back plants, wafers
//   renderGlow()      every additive glow: fish, food, buds, anemones, sparks
//   renderBloom()     quarter-resolution threshold + blur, added back
//   renderFront()     marine snow, bubbles, front plants, surface, label
#pragma once
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <new>
#include <initializer_list>

// ---- look switches (turn off to buy frame time) ----
#ifndef GT_TRAILS
#define GT_TRAILS 0          // glow ghosts behind moving fish (about 0.3M instructions a frame)
#endif
#ifndef GT_BLOOM
#define GT_BLOOM 0           // halo around bright glows: ~3.9M instructions a frame on the C6, a third of the frame
#endif
#ifndef GT_SNOW
#define GT_SNOW 1
#endif
#ifndef GT_CAUSTICS
#define GT_CAUSTICS 1
#endif
#ifndef GT_RAYS
#define GT_RAYS 1
#endif
#ifndef GT_AUTOFEED_S
#define GT_AUTOFEED_S 0          // seconds between automatic feeds; 0 = off
#endif
#ifndef GT_FLAKES_PER_PINCH
#define GT_FLAKES_PER_PINCH 12
#endif

namespace gt {

#include "tank_layout.h"
enum TankTheme { REALISTIC, FANTASY, COMB_JELLIES, ABYSS, POKEMON, THEME_COUNT };
static int theme = REALISTIC;
static bool basePrepared = false, causReady = false;
static void abyssBakeRow(int y, uint16_t *dst);
static void pokemonBakeRow(int y, uint16_t *dst);

static const float PIF = 3.14159265f, TAUF = 6.2831853f;
static const float GLOW = 1.0f, FLOW = 0.35f, SINK = 1.0f, TRAIL = 0.62f;

alignas(4) uint16_t fb[FRAME_PIXELS];   // what gets sent to the panel
static uint16_t *bgS = 0;    // baked backdrop (heap, 110 KB): water, gravel, rocks, rays, moon, vignette

// ======================================================================
// small helpers
// ======================================================================
static uint32_t rs = 0x2545F491u;
static inline uint32_t xr() { uint32_t x = rs; x ^= x << 13; x ^= x >> 17; x ^= x << 5; rs = x; return x; }
static inline float frand() { return (float)(xr() >> 8) * (1.0f / 16777216.0f); }
static inline float rnd(float a, float b) { return a + (b - a) * frand(); }
static inline float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
static inline int clampi(int v, int a, int b) { return v < a ? a : (v > b ? b : v); }
static inline float smooth(float e0, float e1, float x) { float u = clampf((x - e0) / (e1 - e0), 0.f, 1.f); return u * u * (3.f - 2.f * u); }
static inline int ifloor(float v) { int i = (int)v; return (v < (float)i) ? i - 1 : i; }

// ---- trig: 1024-entry Q14 sine, angles in BAM (65536 = full turn) ----
static int16_t SIN14[1025];
static const float RAD2BAM = 10430.378f;
static inline int isin(uint16_t a) { int i = a >> 6, f = a & 63; int s0 = SIN14[i], s1 = SIN14[i + 1]; return s0 + (((s1 - s0) * f) >> 6); }
static inline int icos(uint16_t a) { return isin((uint16_t)(a + 16384)); }
static inline uint16_t bam(float rad) { return (uint16_t)(int32_t)(rad * RAD2BAM); }
static inline float fsin(float r) { return (float)isin(bam(r)) * (1.f / 16384.f); }
static inline float fcos(float r) { return (float)icos(bam(r)) * (1.f / 16384.f); }
static inline void fsincos(float r, float &sn, float &cs) { uint16_t b = bam(r); sn = (float)isin(b) * (1.f / 16384.f); cs = (float)icos(b) * (1.f / 16384.f); }
static float fatan2(float y, float x) {
  float ax = fabsf(x), ay = fabsf(y);
  if (ax < 1e-9f && ay < 1e-9f) return 0.f;
  float a = (ax > ay) ? ay / ax : ax / ay, s = a * a;
  float r = ((-0.0464964749f * s + 0.15931422f) * s - 0.327622764f) * s * a + a;
  if (ay > ax) r = 1.57079637f - r;
  if (x < 0.f) r = 3.14159274f - r;
  if (y < 0.f) r = -r;
  return r;
}
static inline float wrapA(float a) { while (a > PIF) a -= TAUF; while (a < -PIF) a += TAUF; return a; }

// ---- time: integer ms; phases derived without float drift ----
static uint32_t tms = 0;
static constexpr uint32_t K16(double radPerMs) { return (uint32_t)(radPerMs * 10430.378 * 65536.0 + 0.5); }
static inline uint16_t tph(uint32_t k16) { return (uint16_t)(((uint64_t)tms * k16) >> 16); }
// phase rates, fixed at compile time (the C6 has no FPU, so no runtime doubles)
static constexpr uint32_t KT_FLOWX = K16(0.0006), KT_FLOWY = K16(0.0005), KT_SWAY = K16(0.00085);
static constexpr uint32_t KT_C1 = K16(0.0011), KT_C2 = K16(0.0009), KT_C3 = K16(0.00135);
static constexpr uint32_t KT_ANEM = K16(0.0017), KT_S1 = K16(0.0022), KT_S2 = K16(0.0031);
static constexpr uint32_t KT_OTO = K16(0.012), KT_FEEL = K16(0.004);
static constexpr uint32_t KT_RAY = K16(0.0001), KT_RAY2 = K16(0.00017), KT_MOON = K16(0.00007), KT_CYCLE = K16(6.2831853 / 120000.0);

// ---- colour ----
struct RGB8 { uint8_t r, g, b; };
static RGB8 hsl(float h, float s, float l) {
  h = fmodf(h, 360.f); if (h < 0) h += 360.f;
  float c = (1.f - fabsf(2.f * l - 1.f)) * s, hp = h / 60.f;
  float x = c * (1.f - fabsf(fmodf(hp, 2.f) - 1.f)), m = l - c * 0.5f;
  float r = 0, g = 0, b = 0;
  if (hp < 1) { r = c; g = x; } else if (hp < 2) { r = x; g = c; } else if (hp < 3) { g = c; b = x; }
  else if (hp < 4) { g = x; b = c; } else if (hp < 5) { r = x; b = c; } else { r = c; b = x; }
  RGB8 o = { (uint8_t)clampi((int)((r + m) * 255.f + 0.5f), 0, 255), (uint8_t)clampi((int)((g + m) * 255.f + 0.5f), 0, 255), (uint8_t)clampi((int)((b + m) * 255.f + 0.5f), 0, 255) };
  return o;
}
// hue colours at 50% lightness, cached by 6-degree step and saturation bucket
static const uint8_t SATB[8] = { 18, 25, 30, 35, 60, 90, 96, 100 };
static RGB8 hueTab[60][8];
static uint8_t hueHave[60];
static uint8_t satBucket[101];
static RGB8 hueCol(int hue, int sat) {
  int hi = ((hue + 3) / 6) % 60; if (hi < 0) hi += 60;
  int sb = satBucket[clampi(sat, 0, 100)];
  if (!(hueHave[hi] & (1 << sb))) { hueTab[hi][sb] = hsl(hi * 6.f, SATB[sb] / 100.f, 0.5f); hueHave[hi] |= (1 << sb); }
  return hueTab[hi][sb];
}

static const uint8_t BAYER4[16] = { 0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5 };
static inline uint16_t pack565d(int r, int g, int b, int x, int y) {
  int d = BAYER4[((y & 3) << 2) | (x & 3)];
  r += d >> 1; g += d >> 2; b += d >> 1;
  if (r > 255) r = 255; if (g > 255) g = 255; if (b > 255) b = 255;
  if (r < 0) r = 0; if (g < 0) g = 0; if (b < 0) b = 0;
  return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}
// additive add of an 8-bit colour scaled by q (Q8), with dithered rounding
static inline void addPx(int x, int y, int r8, int g8, int b8, int q) {
  if ((unsigned)x >= (unsigned)W || (unsigned)y >= (unsigned)H || q <= 0) return;
  uint16_t &p = fb[y * W + x];
  int d = BAYER4[((y & 3) << 2) | (x & 3)];
  int r = (p >> 11) + ((r8 * q + (d << 7)) >> 11);
  int g = ((p >> 5) & 63) + ((g8 * q + (d << 6)) >> 10);
  int b = (p & 31) + ((b8 * q + (d << 7)) >> 11);
  if (r > 31) r = 31; if (g > 63) g = 63; if (b > 31) b = 31;
  p = (uint16_t)((r << 11) | (g << 5) | b);
}
// tiny glow: one point split bilinearly over 4 pixels (position in 1/16 px)
static inline void addDot(int xq, int yq, RGB8 c, int q) {
  int x = xq >> 4, y = yq >> 4, fx = xq & 15, fy = yq & 15;
  addPx(x, y, c.r, c.g, c.b, (q * (16 - fx) * (16 - fy)) >> 8); addPx(x + 1, y, c.r, c.g, c.b, (q * fx * (16 - fy)) >> 8);
  addPx(x, y + 1, c.r, c.g, c.b, (q * (16 - fx) * fy) >> 8); addPx(x + 1, y + 1, c.r, c.g, c.b, (q * fx * fy) >> 8);
}
// alpha blend toward an 8-bit colour, a in Q8
static inline void blendPx(int x, int y, RGB8 c, int a) {
  if ((unsigned)x >= (unsigned)W || (unsigned)y >= (unsigned)H || a <= 0) return;
  uint16_t &p = fb[y * W + x];
  int r = (p >> 11) << 3, g = ((p >> 5) & 63) << 2, b = (p & 31) << 3;
  r += ((c.r - r) * a) >> 8; g += ((c.g - g) * a) >> 8; b += ((c.b - b) * a) >> 8;
  p = pack565d(r, g, b, x, y);
}

// ======================================================================
// glow kernels: the browser's radial-gradient sprite, precomputed
// A = hue-coloured part, C = white core, both 0..255
// ======================================================================
struct Kern { uint8_t S, K0; uint16_t off; uint16_t spanOff; };
static Kern kern[18][4];
static uint8_t kspan[1300];   // per kernel row: first and one-past-last non-zero column
static uint8_t kpool[20100];
static float profA(float u) {
  static const float U[5] = { 0.f, 0.12f, 0.34f, 0.66f, 1.f }, A[5] = { 0.10f, 0.39f, 0.235f, 0.07f, 0.f };
  if (u >= 1.f) return 0.f;
  for (int i = 0; i < 4; i++) if (u < U[i + 1]) { float f = (u - U[i]) / (U[i + 1] - U[i]); return A[i] + (A[i + 1] - A[i]) * f; }
  return 0.f;
}
static float profC(float u) {
  static const float U[5] = { 0.f, 0.12f, 0.34f, 0.66f, 1.f }, C[5] = { 0.90f, 0.43f, 0.045f, 0.f, 0.f };
  if (u >= 1.f) return 0.f;
  for (int i = 0; i < 4; i++) if (u < U[i + 1]) { float f = (u - U[i]) / (U[i + 1] - U[i]); return C[i] + (C[i + 1] - C[i]) * f; }
  return 0.f;
}
static void buildKernels() {
  uint32_t off = 0;
  for (int k = 0; k < 18; k++) {
    float R = k < 12 ? 0.5f * (k + 1) : 7.f + 2.f * (k - 12);
    int K0 = (int)ceilf(R) + 1, S = 2 * K0 + 1;
    int phases = k < 12 ? 4 : 1;
    for (int ph = 0; ph < phases; ph++) {
      float cx = k < 12 ? K0 + 0.25f + 0.5f * (ph & 1) : K0 + 0.5f;
      float cy = k < 12 ? K0 + 0.25f + 0.5f * (ph >> 1) : K0 + 0.5f;
      kern[k][ph].S = (uint8_t)S; kern[k][ph].K0 = (uint8_t)K0; kern[k][ph].off = (uint16_t)off;
      for (int j = 0; j < S; j++) for (int i = 0; i < S; i++) {
        float a = 0, c = 0;
        for (int sy = 0; sy < 2; sy++) for (int sx = 0; sx < 2; sx++) {
          float dx = i + 0.25f + 0.5f * sx - cx, dy = j + 0.25f + 0.5f * sy - cy;
          float u = sqrtf(dx * dx + dy * dy) / R;
          a += profA(u); c += profC(u);
        }
        kpool[off + j * S + i] = (uint8_t)clampi((int)(a * 0.25f * 255.f + 0.5f), 0, 255);
        kpool[off + S * S + j * S + i] = (uint8_t)clampi((int)(c * 0.25f * 255.f + 0.5f), 0, 255);
      }
      off += 2 * S * S;
    }
  }
  // non-zero column span of every kernel row, so blits skip the empty corners
  uint32_t so = 0;
  for (int k = 0; k < 18; k++) for (int ph = 0; ph < (k < 12 ? 4 : 1); ph++) {
    Kern &K = kern[k][ph];
    K.spanOff = (uint16_t)so;
    const uint8_t *A = kpool + K.off, *C = A + K.S * K.S;
    for (int j = 0; j < K.S; j++) {
      int a = K.S, b = 0;
      for (int i = 0; i < K.S; i++) if (A[j * K.S + i] | C[j * K.S + i]) { if (i < a) a = i; b = i + 1; }
      if (b == 0) a = 0;
      kspan[so++] = (uint8_t)a; kspan[so++] = (uint8_t)b;
    }
  }
}
static uint32_t blitCount = 0;
// Bloom has fixed capacity for either logical orientation.
#if GT_BLOOM
static uint8_t glowMask[BLOOM_CELLS];              // blocks touched by a glow sprite this frame
#endif
// additive glow sprite of radius R (px) at (x,y)
static void blitCore(int ix, int iy, int ph, int k, int a8, RGB8 c);
static inline int kernFor(float R) { return R <= 6.25f ? clampi((int)(R * 2.f + 0.5f) - 1, 0, 11) : 12 + clampi((int)((R - 7.f) * 0.5f + 0.5f), 0, 5); }
static void blit(float x, float y, float R, float alpha, RGB8 c) {
  if (alpha <= 0.008f || R < 0.3f) return;
  int a8 = (int)(alpha * 255.f); if (a8 > 255) a8 = 255; if (a8 < 2) return;
  int k = kernFor(R), ix = ifloor(x), iy = ifloor(y), ph = 0;
  if (k < 12) ph = ((x - (float)ix) >= 0.5f ? 1 : 0) | ((y - (float)iy) >= 0.5f ? 2 : 0);
  blitCore(ix, iy, ph, k, a8, c);
}
// integer entry: position in 1/16 px, kernel index from kernFor(), alpha 0..255
static inline void blitQ(int xq, int yq, int k, int a8, RGB8 c) {
  if (a8 < 2) return;
  int ph = k < 12 ? (((xq & 15) >= 8) ? 1 : 0) | (((yq & 15) >= 8) ? 2 : 0) : 0;
  blitCore(xq >> 4, yq >> 4, ph, k, a8 > 255 ? 255 : a8, c);
}
static void blitCore(int ix, int iy, int ph, int k, int a8, RGB8 c) {
  const Kern &K = kern[k][ph];
  int S = K.S, x0 = ix - K.K0, y0 = iy - K.K0;
  if (x0 >= W || y0 >= H || x0 + S <= 0 || y0 + S <= 0) return;
  const uint8_t *A = kpool + K.off, *C = A + S * S;
  int i0 = x0 < 0 ? -x0 : 0, i1 = (x0 + S > W) ? W - x0 : S;
  int j0 = y0 < 0 ? -y0 : 0, j1 = (y0 + S > H) ? H - y0 : S;
  int cr = c.r * a8, cg = c.g * a8, cb = c.b * a8, cw = 255 * a8;
  blitCount++;
  const uint8_t *sp = kspan + K.spanOff;
#if GT_BLOOM
  for (int by = (y0 + j0) >> 2; by <= (y0 + j1 - 1) >> 2; by++)
    memset(glowMask + by * BW + ((x0 + i0) >> 2), 1, (size_t)(((x0 + i1 - 1) >> 2) - ((x0 + i0) >> 2) + 1));
#endif
  for (int j = j0; j < j1; j++) {
    int py = y0 + j;
    uint16_t *row = fb + py * W + x0 + i0;
    const uint8_t *ar = A + j * S, *cc = C + j * S;
    const uint8_t *by = BAYER4 + ((py & 3) << 2);
    int ia = sp[2 * j] > i0 ? sp[2 * j] : i0, ib = sp[2 * j + 1] < i1 ? sp[2 * j + 1] : i1;
    for (int i = ia; i < ib; i++) {
      int ka = ar[i], kc = cc[i];
      if (!(ka | kc)) continue;
      int white = cw * kc;
      int d = by[(x0 + i) & 3];
      uint16_t p = row[i - i0];
      int r = (p >> 11) + ((cr * ka + white + (d << 15)) >> 19);
      int g = ((p >> 5) & 63) + ((cg * ka + white + (d << 14)) >> 18);
      int b = (p & 31) + ((cb * ka + white + (d << 15)) >> 19);
      if (r > 31) r = 31; if (g > 63) g = 63; if (b > 31) b = 31;
      row[i - i0] = (uint16_t)((r << 11) | (g << 5) | b);
    }
  }
}

// ======================================================================
// terrain + backdrop
// ======================================================================
static float floorYf[MAX_SIDE + 1];
static int16_t floorQ4[MAX_SIDE + 1];
static inline float floorAt(float x) { return floorYf[clampi((int)x, 0, W)]; }
static uint8_t gradR[MAX_SIDE], gradG[MAX_SIDE], gradB[MAX_SIDE];
static uint8_t vigLUT[1224];
static int32_t dx2[MAX_SIDE];


struct Rock { int x, y, rx, ry; RGB8 inner, outer, rim1, rim2; int32_t A, B, A1, B1, A2, B2; int32_t hx, hy; };
static Rock rocks[3];
static float rockSkipY[3];   // feeler y below which a rock can't be reached
static const int ROCKDEF[3][5] = { { 30, 296, 26, 13, 196 }, { 132, 300, 31, 15, 206 }, { 88, 308, 20, 9, 188 } };

static inline uint32_t hh(int x, int y) {
  uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u;
  h = (h ^ (h >> 13)) * 1274126177u;
  return h ^ (h >> 16);
}

static void initTerrain() {
  for (int x = 0; x <= W; x++) {
    float fx = referenceX((float)x);
    floorYf[x] = (H - 28.f) + sinf(fx * 0.055f) * 7.f + sinf(fx * 0.017f + 1.7f) * 5.f + sinf(fx * 0.13f + 0.4f) * 2.2f;
    floorQ4[x] = (int16_t)(floorYf[x] * 16.f);
  }
  // water gradient #0b2433 → #081c2a (0.18) → #05131e (0.55) → #030a12 (1)
  static const float GS[4] = { 0.f, 0.18f, 0.55f, 1.f };
  static const uint8_t GC[4][3] = { { 0x0b, 0x24, 0x33 }, { 0x08, 0x1c, 0x2a }, { 0x05, 0x13, 0x1e }, { 0x03, 0x0a, 0x12 } };
  for (int y = 0; y < H; y++) {
    float f = (y + 0.5f) / H;
    int s = f < GS[1] ? 0 : (f < GS[2] ? 1 : 2);
    float u = (f - GS[s]) / (GS[s + 1] - GS[s]);
    gradR[y] = (uint8_t)(GC[s][0] + (GC[s + 1][0] - GC[s][0]) * u);
    gradG[y] = (uint8_t)(GC[s][1] + (GC[s + 1][1] - GC[s][1]) * u);
    gradB[y] = (uint8_t)(GC[s][2] + (GC[s + 1][2] - GC[s][2]) * u);
    if (y < (int)SURFACE - 2) { gradR[y] = 3; gradG[y] = 7; gradB[y] = 11; }
  }
  // vignette: 0 at r0=64px, 0.55 at r1=237px from centre
  for (int i = 0; i < 1224; i++) {
    float d = sqrtf((float)(i << 5));
    float a = 0.55f * clampf((d - 64.f) / (237.f - 64.f), 0.f, 1.f);
    vigLUT[i] = (uint8_t)clampi((int)((1.f - a) * 255.f + 0.5f), 0, 255);
  }
  for (int x = 0; x < W; x++) dx2[x] = (x - VCX) * (x - VCX);
  for (int i = 0; i < 3; i++) {
    Rock &r = rocks[i];
    r.x = layoutXi(ROCKDEF[i][0]); r.y = layoutYi(ROCKDEF[i][1]); r.rx = ROCKDEF[i][2]; r.ry = ROCKDEF[i][3];
    float h = (float)ROCKDEF[i][4];
    r.inner = hsl(h, 0.20f, 0.12f); r.outer = hsl(h, 0.24f, 0.05f);
    r.rim1 = hsl(h, 0.55f, 0.62f); r.rim2 = hsl(h, 0.60f, 0.70f);
    r.A = (int32_t)(65536.f / (r.rx * r.rx)); r.B = (int32_t)(65536.f / (r.ry * r.ry));
    float rx1 = r.rx * 0.92f, ry1 = r.ry * 0.9f, rx2 = r.rx * 0.6f, ry2 = r.ry * 0.55f;
    r.A1 = (int32_t)(65536.f / (rx1 * rx1)); r.B1 = (int32_t)(65536.f / (ry1 * ry1));
    r.A2 = (int32_t)(65536.f / (rx2 * rx2)); r.B2 = (int32_t)(65536.f / (ry2 * ry2));
    r.hx = r.x - r.rx / 4; r.hy = r.y - (r.ry * 6) / 10;
    rockSkipY[i] = (float)r.y - ((float)r.rx + 8.f) / 1.8f - 0.5f;
  }
}

// ---- light state ----
static int lightMode = 2;               // 0 day, 1 dusk, 2 night, 3 cycle
static float dayLight = 0.2f;
static float introT = 0.f;              // seconds since power-on sequence began
static float lightsF = 0.f, ambF = 0.f, nightF = 0.8f, mF = 0.f;

// ---- ray geometry, refreshed each frame, baked a slice at a time ----
static const float BEAMX[3] = { 34.f, 86.f, 139.f }, BEAMW[3] = { 14.f, 22.f, 12.f }, BEAML[3] = { 26.f, 38.f, -18.f }, BEAMA[3] = { 0.20f, 0.15f, 0.17f }, BEAMP[3] = { 0.f, 1.9f, 3.4f };
static float rTop[3], rBot[3], rAl[3], moonTop, moonBot;
static int16_t rbCx[MAX_SIDE][3], rbHw[MAX_SIDE][3];   // Q4, for marine snow lighting
static uint16_t rbS[MAX_SIDE][3];                // Q12
static int16_t accR[MAX_SIDE], accG[MAX_SIDE], accB[MAX_SIDE];
static int bakeM = -1, bakeNight = -1;   // Q8 values the backdrop was baked with
static int bakeCursor = 0;

static void updateRayGeometry() {
  uint16_t rp = tph(KT_RAY), rp2 = tph(KT_RAY2);
  for (int b = 0; b < 3; b++) {
    uint16_t pb = bam(BEAMP[b]);
    float wob = (float)isin((uint16_t)(rp + pb)) * (7.f / 16384.f);
    rTop[b] = layoutX(BEAMX[b]) + wob; rBot[b] = layoutX(BEAMX[b] + BEAML[b]) + wob * 2.2f;
    rAl[b] = BEAMA[b] * (0.72f + 0.28f * (float)isin((uint16_t)(rp2 + pb)) * (1.f / 16384.f));
  }
  float ms = (float)isin(tph(KT_MOON)) * (1.f / 16384.f);
  moonTop = layoutX(120.f) + ms * 6.f; moonBot = layoutX(96.f) + ms * 12.f;
}

static inline void spanAdd(float cx, float hw, int r, int g, int b, int q) {
  int x0 = (int)ceilf(cx - hw), x1 = ifloor(cx + hw);
  if (x0 < 0) x0 = 0; if (x1 > W - 1) x1 = W - 1;
  int ar = (r * q) >> 8, ag = (g * q) >> 8, ab = (b * q) >> 8;
  for (int x = x0; x <= x1; x++) { accR[x] += ar; accG[x] += ag; accB[x] += ab; }
}

static void bakeRow(int y, int mQ8, int nightQ8, uint16_t *dst) {
  if (theme == ABYSS) { abyssBakeRow(y, dst); return; }
  if (theme == POKEMON) { pokemonBakeRow(y, dst); return; }
  memset(accR, 0, W * sizeof(accR[0])); memset(accG, 0, W * sizeof(accG[0])); memset(accB, 0, W * sizeof(accB[0]));
  float fy = (float)y + 0.5f;
#if GT_RAYS
  if (fy >= SURFACE && fy <= RAY_BOT) {
    float f = (fy - SURFACE) / (RAY_BOT - SURFACE);
    float gr = f < 0.4f ? 1.f - (f / 0.4f) * 0.66f : 0.34f * (1.f - (f - 0.4f) / 0.6f);
    for (int b = 0; b < 3; b++) {
      float cx = rTop[b] + (rBot[b] - rTop[b]) * f;
      for (int s = 0; s < 3; s++) {
        float k = 1.f - 0.32f * s;
        float hw = BEAMW[b] * (0.5f * k * (1.f - f) + 1.5f * k * f);
        int q = (int)(rAl[b] * (0.30f + 0.30f * s) * gr * mQ8);
        if (q > 0) spanAdd(cx, hw, 150, 222, 250, q);
      }
      rbCx[y][b] = (int16_t)(cx * 16.f);
      rbHw[y][b] = (int16_t)(BEAMW[b] * (0.5f + f) * 16.f);
      rbS[y][b] = (uint16_t)(rAl[b] * (1.f - 0.8f * f) * mQ8 * 16.f);
    }
  } else { for (int b = 0; b < 3; b++) rbS[y][b] = 0; }
#else
  for (int b = 0; b < 3; b++) rbS[y][b] = 0;
#endif
  if (nightQ8 > 2 && fy >= SURFACE && fy <= layoutY(280.f)) {
    float f = (fy - SURFACE) / (layoutY(280.f) - SURFACE);
    float ga = f < 0.5f ? 0.11f - 0.12f * f : 0.05f * (1.f - (f - 0.5f) * 2.f);
    float cx = moonTop + (moonBot - moonTop) * f;
    for (int s = 0; s < 3; s++) {
      float k = 1.f - 0.3f * s;
      float hw = 16.f * k * (1.f - f) + 44.f * k * f;
      int q = (int)(ga * (0.3f + 0.25f * s) * nightQ8);
      if (q > 0) spanAdd(cx, hw, 130, 150, 255, q);
    }
  }

  uint16_t *out = dst + y * W;
  int dy2 = (y - VCY) * (y - VCY);
  int r0 = gradR[y], g0 = gradG[y], b0 = gradB[y];
  int yq = y * 16 + 8;
  int rowRocks[3], nRowRocks = 0;
  for (int i = 0; i < 3; i++) if (y - rocks[i].y >= -rocks[i].ry && y - rocks[i].y <= rocks[i].ry) rowRocks[nRowRocks++] = i;
  for (int x = 0; x < W; x++) {
    int r = r0, g = g0, b = b0;
    int fq = floorQ4[x];
    if (yq >= fq) {
      // gravel: pebble noise, darker with depth
      int dq = ((yq - fq) * 158) >> 8; if (dq > 256) dq = 256;
      uint32_t h = hh(x, y);
      int nf = (((h & 255) * 205) + (((h >> 8) & 255) * 64)) >> 8;
      int vq = 17 * 256 - 11 * dq + ((nf * 15 * (256 - ((dq * 154) >> 8))) >> 8);
      r = ((vq * 236) >> 16) + 3; g = ((vq * 251) >> 16) + 6; b = ((vq * 264) >> 16) + 10;
      if (yq - fq < 16) { r += 16; g += 27; b += 31; }   // lit crest line
    }
    // rocks, later ones on top
    for (int i = 0; i < nRowRocks; i++) {
      const Rock &rk = rocks[rowRocks[i]];
      int dx = x - rk.x, dy = y - rk.y;
      if (dx < -rk.rx || dx > rk.rx || dy < -rk.ry || dy > rk.ry) continue;
      int32_t e = dx * dx * rk.A + dy * dy * rk.B;
      if (e > 65536) continue;
      int hx = x - rk.hx, hy = y - rk.hy;
      int32_t t2 = (hx * hx + hy * hy) * rk.A; if (t2 > 65536) t2 = 65536;
      int t8 = t2 >> 8;
      r = rk.inner.r + (((rk.outer.r - rk.inner.r) * t8) >> 8);
      g = rk.inner.g + (((rk.outer.g - rk.inner.g) * t8) >> 8);
      b = rk.inner.b + (((rk.outer.b - rk.inner.b) * t8) >> 8);
      // rim highlights along the upper arcs
      int dy1 = 2 * dy + 3;                         // (dy + 1.5) * 2
      int32_t e1 = dx * dx * rk.A1 + ((dy1 * dy1 * rk.B1) >> 2);
      int32_t th1 = (65536 * 11) / (rk.ry * 9);
      if (dy1 < -(rk.ry * 36) / 100 && e1 > 65536 - th1 && e1 < 65536 + th1) {
        r += ((rk.rim1.r - r) * 107) >> 8; g += ((rk.rim1.g - g) * 107) >> 8; b += ((rk.rim1.b - b) * 107) >> 8;
      }
      int dy2r = dy + 3;
      int32_t e2 = dx * dx * rk.A2 + dy2r * dy2r * rk.B2;
      int32_t th2 = (65536 * 7) / (rk.ry * 6);
      if (dy2r < -(rk.ry * 25) / 100 && e2 > 65536 - th2 && e2 < 65536 + th2) {
        r += ((rk.rim2.r - r) * 41) >> 8; g += ((rk.rim2.g - g) * 41) >> 8; b += ((rk.rim2.b - b) * 41) >> 8;
      }
    }
    r = (r * mQ8) >> 8; g = (g * mQ8) >> 8; b = (b * mQ8) >> 8;
    r += accR[x]; g += accG[x]; b += accB[x];
    int v = vigLUT[(dx2[x] + dy2) >> 5];
    r = (r * v) >> 8; g = (g * v) >> 8; b = (b * v) >> 8;
    out[x] = pack565d(r, g, b, x, y);
  }
}

// ======================================================================
// plants: bracketed L-systems, expanded once, walked in fixed point
// ======================================================================
enum { OP_F = 0, OP_L = 1, OP_R = 2, OP_PUSH = 3, OP_POP = 4 };
struct PlantDef { const char *axiom, *ruleX, *ruleF; float delta; int n, x, h, hue; bool front; };
static const PlantDef PLANTDEF[6] = {
  { "X", "F-[[X]+X]+F[+X]-X", "FF", 22.5f, 4, 15, 128, 150, false },           // fern
  { "F", 0, "F[+F]F[-F]F", 25.7f, 3, 55, 64, 170, false },                        // fanweed
  { "X", "F[+X]F[-X]+X", "FF", 20.f, 4, 104, 150, 138, false },                   // hornwort
  { "F", 0, "FF-[-F+F+F]+[+F-F-F]", 22.5f, 2, 158, 74, 160, false },             // cushion
  { "F", 0, "F[+F]F[-F]F", 25.7f, 2, 76, 34, 178, true },                         // fanweed, front
  { "X", "F[+X]F[-X]+X", "FF", 20.f, 3, 147, 50, 128, true },                     // hornwort, front
};
struct Plant {
  uint16_t op0, nOps, segs, tips;
  int32_t x0, y0, len, delta, lean, bend;   // Q8 / Q8 / Q8 / BAM / BAM / BAM
  uint16_t phase; int hue; bool front;
  RGB8 col[8];
};
static Plant plants[6];
static const int OPMAX = 4200;
static uint8_t opPool[OPMAX];
static int8_t jitPool[OPMAX];            // branch-angle jitter, BAM / 8
static int opUsed = 0, segTotal = 0, tipTotal = 0;
static uint32_t plantSeed = 1;

static char *expandStr(const char *axiom, const char *rx, const char *rf, int n) {
  size_t len = strlen(axiom);
  char *s = (char *)malloc(len + 1); if (!s) return 0;
  memcpy(s, axiom, len + 1);
  size_t lx = rx ? strlen(rx) : 1, lf = rf ? strlen(rf) : 1;
  for (int it = 0; it < n; it++) {
    size_t out = 0;
    for (size_t i = 0; i < len; i++) out += (s[i] == 'X') ? lx : (s[i] == 'F' ? lf : 1);
    char *o = (char *)malloc(out + 1); if (!o) { free(s); return 0; }
    size_t p = 0;
    for (size_t i = 0; i < len; i++) {
      const char *rep = (s[i] == 'X' && rx) ? rx : ((s[i] == 'F' && rf) ? rf : 0);
      if (rep) { memcpy(o + p, rep, strlen(rep)); p += strlen(rep); } else o[p++] = s[i];
    }
    o[p] = 0; free(s); s = o; len = p;
  }
  return s;
}

static int32_t pstX[64], pstY[64], pstA[64], pstD[64];
static int16_t tipBuf[2 * 200];
static int tipN = 0;

// walk one plant; draw = 0 only measures
template <bool DRAW>
static void walkPlant(const Plant &p, int swayOn, int32_t *minY, int mQ8, int frontA) {
  int32_t x = p.x0, y = p.y0, a = -16384 + p.lean;
  int d = 0, sp = 0; bool lastF = false;
  uint16_t swT = tph(KT_SWAY);
  for (int i = 0; i < p.nOps; i++) {
    uint8_t op = opPool[p.op0 + i];
    if (op == OP_F) {
      if (swayOn) {
        int s = isin((uint16_t)(swT + p.phase + (((p.y0 - y) * 521) >> 8)));
        a += (int32_t)(((int64_t)s * p.bend * (12 + 7 * d)) / (16384 * 20));
      }
      int32_t nx = x + ((icos((uint16_t)a) * p.len) >> 14);
      int32_t ny = y + ((isin((uint16_t)a) * p.len) >> 14);
      if (DRAW) {
        int dd = d > 7 ? 7 : d;
        RGB8 c = p.col[dd];
        int alpha = dd <= 2 ? 256 : (dd == 3 ? 220 : 145);
        if (p.front) alpha = (alpha * frontA) >> 8;
        RGB8 cc = c;
        if (!p.front) { cc.r = (uint8_t)((c.r * mQ8) >> 8); cc.g = (uint8_t)((c.g * mQ8) >> 8); cc.b = (uint8_t)((c.b * mQ8) >> 8); }
        // Bresenham, 2px for trunk depths
        int x0 = (x + 128) >> 8, y0 = (y + 128) >> 8, x1 = (nx + 128) >> 8, y1 = (ny + 128) >> 8;
        int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1, dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1, err = dx + dy;
        bool thick = dd <= 1, steep = -dy > dx;
        for (int guard = 0; guard < 64; guard++) {
          if (alpha >= 256 && !p.front) { if ((unsigned)x0 < (unsigned)W && (unsigned)y0 < (unsigned)H) fb[y0 * W + x0] = pack565d(cc.r, cc.g, cc.b, x0, y0); }
          else blendPx(x0, y0, cc, alpha > 255 ? 255 : alpha);
          if (thick) blendPx(steep ? x0 + 1 : x0, steep ? y0 : y0 + 1, cc, (alpha * 150) >> 8);
          if (x0 == x1 && y0 == y1) break;
          int e2 = 2 * err;
          if (e2 >= dy) { err += dy; x0 += sx; }
          if (e2 <= dx) { err += dx; y0 += sy; }
        }
      } else if (minY && ny < *minY) *minY = ny;
      x = nx; y = ny; lastF = true;
    } else if (op == OP_L) { a -= p.delta + jitPool[p.op0 + i] * 8; }
    else if (op == OP_R) { a += p.delta + jitPool[p.op0 + i] * 8; }
    else if (op == OP_PUSH) {
      if (sp < 64) { pstX[sp] = x; pstY[sp] = y; pstA[sp] = a; pstD[sp] = d; sp++; }
      d++; lastF = false;
    } else if (op == OP_POP) {
      if (lastF && tipN < 200) { tipBuf[2 * tipN] = (int16_t)(x >> 4); tipBuf[2 * tipN + 1] = (int16_t)(y >> 4); tipN++; }
      if (sp > 0) { sp--; x = pstX[sp]; y = pstY[sp]; a = pstA[sp]; d = pstD[sp]; }
      lastF = false;
    }
  }
  if (lastF && tipN < 200) { tipBuf[2 * tipN] = (int16_t)(x >> 4); tipBuf[2 * tipN + 1] = (int16_t)(y >> 4); tipN++; }
}

static void buildPlants() {
  uint32_t save = rs; rs = 0x9E3779B9u ^ (plantSeed * 9973u); if (!rs) rs = 1;
  opUsed = 0; segTotal = 0; tipTotal = 0;
  for (int i = 0; i < 6; i++) {
    const PlantDef &d = PLANTDEF[i];
    Plant &p = plants[i];
    char *s = expandStr(d.axiom, d.ruleX, d.ruleF, d.n);
    p.op0 = (uint16_t)opUsed; p.nOps = 0; p.segs = 0;
    if (s) {
      for (const char *c = s; *c; c++) {
        int op = -1;
        if (*c == 'F') op = OP_F; else if (*c == '+') op = OP_R; else if (*c == '-') op = OP_L;
        else if (*c == '[') op = OP_PUSH; else if (*c == ']') op = OP_POP;
        if (op < 0 || opUsed >= OPMAX) continue;
        opPool[opUsed] = (uint8_t)op;
        jitPool[opUsed] = (int8_t)((frand() - 0.5f) * 2.f * 113.f);   // ±5 degrees in BAM/8
        if (op == OP_F) p.segs++;
        opUsed++; p.nOps++;
      }
      free(s);
    }
    p.x0 = (int32_t)(layoutX(d.x) * 256.f); p.y0 = (int32_t)((floorAt(layoutX(d.x)) + 2.f) * 256.f);
    p.delta = (int32_t)(d.delta / 360.f * 65536.f);
    p.lean = (int32_t)((frand() - 0.5f) * 0.22f * RAD2BAM);
    p.phase = (uint16_t)(frand() * 65535.f);
    p.hue = d.hue; p.front = d.front;
    for (int k = 0; k < 8; k++) p.col[k] = hsl((float)(d.hue + k * 4), (40.f + 6.f * k) / 100.f, (17.f + 6.5f * k) / 100.f);
    // measure at rest with unit segments, then scale to the target height
    p.len = 256; p.bend = 0;
    int32_t minY = p.y0; tipN = 0;
    walkPlant<false>(p, 0, &minY, 256, 256);
    p.tips = (uint16_t)tipN;
    float tall = (float)(p.y0 - minY) / 256.f; if (tall < 1.f) tall = 1.f;
    p.len = (int32_t)((float)d.h * depthScale / tall * 256.f);
    float bendRad = 0.42f / (tall * 0.6f > 4.f ? tall * 0.6f : 4.f);
    p.bend = (int32_t)(bendRad * RAD2BAM);
    segTotal += p.segs; tipTotal += p.tips;
  }
  rs = save;
}

// ======================================================================
// species + fish
// ======================================================================
enum { B_SCHOOL, B_HOVER, B_BOTTOM, B_CLING, B_JELLY, B_ORB, B_SEAHORSE, B_COMB };
enum { DS_FISH, DS_JELLY, DS_SEAHORSE, DS_RIBBON, DS_ORB, DS_COMB };
enum { MK_NONE, MK_NEON, MK_HONEY, MK_CORY, MK_OTO, MK_CLOWN, MK_TANG, MK_PRISM, MK_ROSE, MK_SHARK, MK_GOOSE };
struct Species {
  const char *name; uint8_t beh, style, mark;
  float L, depth;                   // body length in px, body depth as a fraction of length
  RGB8 body, fin, tailc;            // lit colours (tail fin can differ)
  int glowHue, glowSat; float glowA;
  uint8_t tail; float tailL, tailS; // tail: 0 forked / 1 rounded; length and half-spread as fractions of L
  float dorsal0, dorsal1, dorsalH;  // dorsal fin: start/end along the body (0 = snout) and height (fraction of L)
  float spd, dash, turn, band0, band1, coh, ali, rad;
  bool eatsFlake, eatsWafer; float range, full0, full1;
};
enum { SP_NEON, SP_EMBER, SP_HONEY, SP_CORY, SP_OTO, SP_CHROMIS, SP_CLOWN, SP_TANG, SP_SEAHORSE, SP_MOONJELLY,
       SP_PRISM, SP_RIBBON, SP_ORB, SP_ROSEJELLY, SP_SHARK, SP_BEROE, SP_GOOSE, NSP };
static const Species SP[NSP] = {
  // ---- realistic: 29 US gal / 110 L planted community, drawn ~2x scale so they read on a 1.47" screen
  { "neon",  B_SCHOOL, DS_FISH, MK_NEON, 13.f, 0.27f, { 100, 115, 140 }, { 150, 170, 200 }, { 150, 170, 200 }, 192, 96, 0.18f, 0, 0.30f, 0.20f, 0.44f, 0.56f, 0.10f, 0.69f, 1.6f, 0.08f, 0.28f, 0.60f, 0.020f, 0.9f, 2000.f, true, false, 95.f, 4000.f, 9000.f },
  { "ember", B_SCHOOL, DS_FISH, MK_NONE, 9.f, 0.33f, { 255, 118, 58 }, { 255, 92, 52 }, { 255, 92, 52 }, 14, 100, 0.26f, 0, 0.30f, 0.20f, 0.42f, 0.56f, 0.12f, 0.78f, 1.6f, 0.10f, 0.16f, 0.50f, 0.012f, 0.45f, 1400.f, true, false, 75.f, 3500.f, 8000.f },
  { "honey", B_HOVER, DS_FISH, MK_HONEY, 18.f, 0.44f, { 232, 160, 60 }, { 215, 132, 48 }, { 215, 132, 48 }, 36, 100, 0.16f, 1, 0.30f, 0.20f, 0.26f, 0.74f, 0.12f, 0.38f, 1.35f, 0.045f, 0.05f, 0.36f, 0, 0, 0, true, false, 85.f, 6000.f, 12000.f },
  { "cory",  B_BOTTOM, DS_FISH, MK_CORY, 14.f, 0.36f, { 225, 215, 196 }, { 205, 199, 186 }, { 205, 199, 186 }, 40, 20, 0.18f, 0, 0.30f, 0.20f, 0.30f, 0.44f, 0.24f, 0.33f, 1.7f, 0.08f, 0.9f, 1.0f, 0, 0, 0, true, true, 105.f, 3000.f, 6000.f },
  { "oto",   B_CLING, DS_FISH, MK_OTO, 12.f, 0.22f, { 168, 158, 115 }, { 152, 142, 102 }, { 152, 142, 102 }, 54, 35, 0.16f, 0, 0.30f, 0.20f, 0.36f, 0.48f, 0.10f, 0.52f, 1.f, 0.10f, 0.1f, 0.95f, 0, 0, 0, false, true, 150.f, 0, 0 },
  // ---- fantasy: real animals you'd never keep together in a freshwater tank
  { "chromis", B_SCHOOL, DS_FISH, MK_NONE, 11.f, 0.36f, { 70, 215, 190 }, { 95, 230, 210 }, { 95, 230, 210 }, 172, 90, 0.24f, 0, 0.32f, 0.22f, 0.35f, 0.60f, 0.12f, 0.72f, 1.6f, 0.09f, 0.20f, 0.55f, 0.018f, 0.8f, 1800.f, true, false, 90.f, 3500.f, 8000.f },
  { "clown", B_SCHOOL, DS_FISH, MK_CLOWN, 14.f, 0.42f, { 255, 118, 20 }, { 255, 105, 25 }, { 255, 105, 25 }, 28, 100, 0.22f, 1, 0.26f, 0.20f, 0.30f, 0.62f, 0.13f, 0.45f, 1.5f, 0.08f, 0.55f, 0.88f, 0.010f, 0.3f, 1600.f, true, false, 80.f, 4000.f, 9000.f },
  { "tang",  B_SCHOOL, DS_FISH, MK_TANG, 18.f, 0.50f, { 40, 90, 230 }, { 50, 100, 235 }, { 255, 215, 40 }, 222, 96, 0.24f, 0, 0.30f, 0.24f, 0.20f, 0.80f, 0.10f, 0.50f, 1.5f, 0.06f, 0.20f, 0.70f, 0.004f, 0.3f, 3000.f, true, false, 90.f, 5000.f, 10000.f },
  { "seahorse", B_SEAHORSE, DS_SEAHORSE, MK_NONE, 24.f, 0.f, { 250, 185, 50 }, { 255, 215, 130 }, { 255, 215, 130 }, 44, 100, 0.20f, 0, 0, 0, 0, 0, 0, 0.12f, 1.f, 0.f, 0.30f, 0.85f, 0, 0, 0, true, false, 45.f, 6000.f, 12000.f },
  { "moon jelly", B_JELLY, DS_JELLY, MK_NONE, 18.f, 0.f, { 225, 205, 255 }, { 255, 170, 230 }, { 255, 170, 230 }, 290, 60, 0.30f, 0, 0, 0, 0, 0, 0, 0.f, 1.f, 0.f, 0.08f, 0.75f, 0, 0, 0, false, false, 0, 0, 0 },
  // ---- ultra fantasy: invented creatures, all circles, triangles and curves
  { "prism fish", B_SCHOOL, DS_FISH, MK_PRISM, 14.f, 0.36f, { 230, 230, 245 }, { 200, 200, 255 }, { 200, 200, 255 }, 0, 96, 0.22f, 1, 0.60f, 0.36f, 0.30f, 0.62f, 0.22f, 0.70f, 1.6f, 0.08f, 0.15f, 0.65f, 0.020f, 0.8f, 2000.f, true, false, 95.f, 4000.f, 9000.f },
  { "ribbon eel", B_SCHOOL, DS_RIBBON, MK_NONE, 40.f, 0.09f, { 200, 200, 200 }, { 200, 200, 200 }, { 200, 200, 200 }, 0, 96, 0.20f, 0, 0, 0, 0, 0, 0, 0.60f, 1.4f, 0.06f, 0.15f, 0.80f, 0, 0, 0, true, false, 70.f, 5000.f, 10000.f },
  { "orb drifter", B_ORB, DS_ORB, MK_NONE, 16.f, 0.f, { 200, 200, 200 }, { 200, 200, 200 }, { 200, 200, 200 }, 0, 96, 0.35f, 0, 0, 0, 0, 0, 0, 0.f, 1.f, 0.f, 0.15f, 0.80f, 0, 0, 0, false, false, 0, 0, 0 },
  { "rose jelly", B_JELLY, DS_JELLY, MK_ROSE, 20.f, 0.f, { 255, 220, 250 }, { 255, 180, 240 }, { 255, 180, 240 }, 320, 96, 0.34f, 0, 0, 0, 0, 0, 0, 0.f, 1.f, 0.f, 0.08f, 0.70f, 0, 0, 0, false, false, 0, 0, 0 },
  // ---- baby shark: one grey cruiser. tail 2 = shark tail, long upper lobe
  { "baby shark", B_SCHOOL, DS_FISH, MK_SHARK, 36.f, 0.24f, { 118, 132, 148 }, { 104, 118, 134 }, { 104, 118, 134 }, 205, 30, 0.10f, 2, 0.30f, 0.20f, 0.36f, 0.50f, 0.20f, 0.46f, 1.5f, 0.035f, 0.20f, 0.72f, 0, 0, 0, true, false, 120.f, 5000.f, 10000.f },
  // ---- deep sea comb jellies (ctenophores): clear bodies, eight comb rows of travelling rainbow light.
  //      depth = body width / length. They glide mouth-first and catch flakes themselves (updateComb).
  { "beroe", B_COMB, DS_COMB, MK_NONE, 22.f, 0.60f, { 255, 196, 214 }, { 255, 120, 165 }, { 255, 120, 165 }, 330, 60, 0.16f, 0, 0, 0, 0, 0, 0, 0.13f, 1.f, 0.f, 0.10f, 0.70f, 0, 0, 0, false, false, 40.f, 5000.f, 10000.f },
  { "sea gooseberry", B_COMB, DS_COMB, MK_GOOSE, 11.f, 0.84f, { 210, 232, 255 }, { 255, 214, 170 }, { 255, 214, 170 }, 190, 60, 0.16f, 0, 0, 0, 0, 0, 0, 0.10f, 1.f, 0.f, 0.08f, 0.80f, 0, 0, 0, false, false, 34.f, 3000.f, 7000.f },
};
// tank themes: which creatures, how many
struct Stock { uint8_t sp, n; };
static const Stock THEME[THEME_COUNT][6] = {
  { { SP_NEON, 12 }, { SP_EMBER, 10 }, { SP_HONEY, 2 }, { SP_CORY, 6 }, { SP_OTO, 4 }, { 0, 0 } },
  { { SP_CHROMIS, 9 }, { SP_CLOWN, 4 }, { SP_TANG, 3 }, { SP_SEAHORSE, 2 }, { SP_MOONJELLY, 3 }, { 0, 0 } },
  { { SP_BEROE, 3 }, { SP_GOOSE, 5 }, { 0, 0 }, { 0, 0 }, { 0, 0 }, { 0, 0 } },
  {},  // Abyss uses its own compact creature model in abyss.h.
  {},  // Pokemon uses runtime projected geometry in pokemon.h.
};
static const char *themeName(int t) { static const char *N[THEME_COUNT] = { "REALISTIC", "FANTASY", "COMB JELLIES", "ABYSS", "POKEMON" }; return N[((t % THEME_COUNT) + THEME_COUNT) % THEME_COUNT]; }
static int themeLight(int t, int dayMode) { return t == FANTASY ? 1 : (t == COMB_JELLIES || t == ABYSS) ? 2 : dayMode; }
static RGB8 glowCol[NSP];
static float sepTab[NSP][NSP];     // squared personal-space radius for each species pair
static int16_t sepQ[NSP][NSP];     // same radius + 1 px, in 1/16 px, for the integer prefilter
static const int SPINE = 6;     // spine points, snout first
static const int MAXFISH = 40;
struct Fish {
  uint8_t sp, state; int8_t side, up;
  float x, y, h, v, wander, timer, full, flash, ignite, tail;
  float ax, ay, aface; int8_t afood; uint16_t aserial; bool hasAnchor, bubbled;
  float sx[6], sy[6];       // spine, snout first, fixed spacing (follow-the-leader)
  float hx[8], hy[8]; uint8_t hpos;
  float hc, hs;             // cos/sin of h, refreshed whenever h changes
  float vx, vy; int16_t hue;   // free-drifting creatures; per-individual hue for fantasy species
  int16_t qx, qy;           // x,y in 1/16 px, refreshed whenever x,y change (integer prefilters)
};
enum AbyssSpecies { A_LANTERN, A_HATCHET, A_LOOSEJAW, A_ANGLER };
static const int ABYSS_FISH_COUNT = 11, ABYSS_PLANKTON_COUNT = 450;
struct AbyssFish {
  float x,y,h,v,wander,phase,speedScale,focus,hc,hs;
  uint16_t qx,qy;  // Q7; unsigned so the bottom 64 pixels fit too
  uint8_t sp;
};
struct Plankton { uint16_t x,y,phase,energy; }; // Q7 position, Q16 energy; 8 bytes
static_assert(sizeof(Plankton) == 8, "Plankton state should stay compact");

// These tanks are mutually exclusive. Reuse the community-fish storage for
// Abyss's 450 plankton points and 11 creatures; preserve backdrop heap space.
struct CommunityState { Fish fish[MAXFISH]; };
struct AbyssState { AbyssFish inhabitants[ABYSS_FISH_COUNT]; Plankton plankton[ABYSS_PLANKTON_COUNT]; };
static_assert(sizeof(AbyssState) <= sizeof(CommunityState), "Abyss must fit existing creature RAM");
#include "pokemon_types.h"
static_assert(sizeof(PokemonState) <= sizeof(CommunityState), "Pokemon must fit existing creature RAM");
union CreatureStorage { CommunityState community; AbyssState abyss; PokemonState pokemon; };
static CreatureStorage creatures;
static Fish (&fish)[MAXFISH] = creatures.community.fish;
static AbyssFish (&abyssFish)[ABYSS_FISH_COUNT] = creatures.abyss.inhabitants;
static Plankton (&plankton)[ABYSS_PLANKTON_COUNT] = creatures.abyss.plankton;

static inline void syncFish(Fish &f) { fsincos(f.h, f.hs, f.hc); f.qx = (int16_t)(f.x * 16.f); f.qy = (int16_t)(f.y * 16.f); }
static int nFish = 0;

struct Food { uint8_t kind, state; bool on; uint16_t serial; float x, y, vx, vy, r, mass, life, floatT, term, phase, hue; int16_t qx, qy; };
static inline void syncFood(Food &f) { f.qx = (int16_t)(f.x * 16.f); f.qy = (int16_t)(f.y * 16.f); }
static const int MAXFOOD = 72;
static Food food[MAXFOOD];
static uint16_t foodSerial = 1;
struct Bubble { bool on; float x, y, r, vy, phase; };
struct Spark { bool on; float x, y, vx, vy, life, r; RGB8 c; };
struct Ring { bool on; float x, r, life; };
struct Mote { float x, y, z, ph; };
static Bubble bubbles[48];
static Spark sparks[64];
static Ring rings[12];
static Mote motes[110];
static uint32_t eaten = 0;

static inline float bandY(float fr) { return SURFACE + fr * (WATER_BOT - SURFACE); }
static inline float flowX(float x, float y) { (void)x; return (float)isin((uint16_t)(bam(y * 0.021f) + tph(KT_FLOWX))) * (FLOW * 0.5f / 16384.f); }
static inline float flowY(float x, float y) { (void)y; return (float)icos((uint16_t)(bam(x * 0.027f) - tph(KT_FLOWY))) * (FLOW * 0.22f / 16384.f); }

static void addBubble(float x, float y, float r, float vy) {
  for (int i = 0; i < 48; i++) if (!bubbles[i].on) { bubbles[i] = { true, x, y, r, vy, rnd(0, TAUF) }; return; }
}
static void addSpark(float x, float y, float vx, float vy, float r, RGB8 c) {
  for (int i = 0; i < 64; i++) if (!sparks[i].on) { sparks[i] = { true, x, y, vx, vy, 1.f, r, c }; return; }
}
static void addRing(float x, float r, float life) {
  for (int i = 0; i < 12; i++) if (!rings[i].on) { rings[i] = { true, x, r, life }; return; }
}

static void initFish(Fish &f, int sp) {
  const Species &s = SP[sp];
  memset(&f, 0, sizeof(f));
  f.sp = (uint8_t)sp;
  f.x = rnd(30.f, W - 30.f);
  f.y = bandY(rnd(s.band0, s.band1));
  if (s.beh == B_BOTTOM) f.y = floorAt(f.x) - 4.f;
  if (s.beh == B_CLING) { f.x = frand() < 0.5f ? 4.f : W - 4.f; f.y = bandY(rnd(0.2f, 0.8f)); }
  if (s.beh == B_SEAHORSE) { static const float PX[3] = { 55.f, 104.f, 150.f }; f.x = layoutX(PX[xr() % 3]) + rnd(-8.f, 8.f); f.ax = f.x; f.ay = f.y; }
  f.hue = (int16_t)((s.mark == MK_PRISM || s.style == DS_RIBBON || s.style == DS_ORB || s.mark == MK_ROSE || s.style == DS_COMB) ? (int)rnd(0.f, 359.f) : s.glowHue);
  f.h = rnd(-0.5f, 0.5f) + (frand() < 0.5f ? 0.f : PIF);
  f.v = s.spd * 0.6f;
  f.side = frand() < 0.5f ? -1 : 1;
  f.timer = rnd(1000.f, 6000.f);
  f.ignite = introT + rnd(0.05f, 0.85f);
  float c = fcos(f.h), sn = fsin(f.h), sl = s.L / (SPINE - 1);
  for (int i = 0; i < SPINE; i++) { f.sx[i] = f.x - c * sl * i; f.sy[i] = f.y - sn * sl * i; }
  f.tail = rnd(0, TAUF); f.up = 1;
  for (int i = 0; i < 8; i++) { f.hx[i] = f.x; f.hy[i] = f.y; }
  syncFish(f);
}

static void initMotes() { for (int i = 0; i < 110; i++) motes[i] = { rnd(0, (float)W), rnd(SURFACE + 2.f, layoutY(290.f)), rnd(0.15f, 1.f), rnd(0, TAUF) }; }

// Included inside gt, after the shared food, colour and pixel helpers.
#include "abyss.h"
#include "pokemon.h"
static int population() { return theme == POKEMON ? PK_COUNT : theme == ABYSS ? ABYSS_FISH_COUNT : nFish; }

// ---- feeding ----
static void feed(float x) {
  if (theme == ABYSS) { abyssFeed(x); return; }
  if (theme == POKEMON) { pokemonFeed(x); return; }
  if (x < 0) x = rnd(W * 0.25f, W * 0.75f);
  for (int i = 0; i < GT_FLAKES_PER_PINCH; i++) {
    for (int k = 0; k < MAXFOOD; k++) if (!food[k].on) {
      Food &f = food[k];
      f = Food();
      f.on = true; f.kind = 0; f.state = 0; f.serial = foodSerial++;
      f.x = clampf(x + rnd(-11.f, 11.f), 4.f, W - 4.f); f.y = SURFACE + 1.5f;
      f.vx = rnd(-0.12f, 0.12f); f.r = rnd(1.5f, 2.5f); f.life = 1.f;
      f.floatT = rnd(40.f, 170.f); f.term = rnd(0.10f, 0.24f); f.phase = rnd(0, TAUF); f.hue = 42.f + rnd(-8.f, 10.f);
      syncFood(f);
      break;
    }
  }
  RGB8 sc = hueCol(50, 96);
  for (int s = 0; s < 6; s++) addSpark(x + rnd(-10.f, 10.f), SURFACE + rnd(-1.f, 1.5f), rnd(-0.4f, 0.4f), rnd(-0.35f, -0.05f), 1.5f, sc);
  addRing(x, 1.f, 1.f); addRing(x + rnd(-6.f, 6.f), 0.f, 0.8f);
}
static const float WAFER_SPOTS[5] = { 60, 70, 104, 110, 166 };
static void dropWafer(float x) {
  if (theme == ABYSS) { abyssFeed(x); return; }
  if (theme == POKEMON) { pokemonFeed(x); return; }
  if (x < 0) x = layoutX(WAFER_SPOTS[xr() % 5]) + rnd(-3.f, 3.f);
  for (int k = 0; k < MAXFOOD; k++) if (!food[k].on) {
    Food &f = food[k];
    f = Food();
    f.on = true; f.kind = 1; f.state = 1; f.serial = foodSerial++;
    f.x = x; f.y = SURFACE + 2.f; f.r = 4.2f; f.mass = 1.f; f.life = 1.f; f.term = 0.55f; f.phase = rnd(0, TAUF); f.hue = 92.f;
    syncFood(f);
    break;
  }
  addRing(x, 1.f, 1.f);
}

// ---- power-on ----
static float autoFeedAt = 2.6f;
static void restart() {
  memset(food, 0, sizeof(food)); memset(bubbles, 0, sizeof(bubbles));
  memset(sparks, 0, sizeof(sparks)); memset(rings, 0, sizeof(rings));
  eaten = 0; introT = 0.f; autoFeedAt = 2.6f;
  nFish = 0;
  if (theme != ABYSS && theme != POKEMON) new (&creatures.community) CommunityState();
  for (int i = 0; i < 6 && THEME[theme][i].n; i++) for (int c = 0; c < THEME[theme][i].n && nFish < MAXFISH; c++) initFish(fish[nFish++], THEME[theme][i].sp);
  initMotes();
  if (theme == ABYSS) initAbyss();
  if (theme == POKEMON) initPokemon();
  bakeM = -1;
  basePrepared = false; causReady = false;
}

static void setLight(int m) { lightMode = ((m % 4) + 4) % 4; }
// Swap inhabitants and invalidate cached lighting when entering/leaving Abyss.
static void setTheme(int t) {
  theme = ((t % THEME_COUNT) + THEME_COUNT) % THEME_COUNT;
  memset(food, 0, sizeof(food));
  memset(bubbles, 0, sizeof(bubbles)); memset(sparks, 0, sizeof(sparks)); memset(rings, 0, sizeof(rings));
  nFish = 0;
  if (theme != ABYSS && theme != POKEMON) new (&creatures.community) CommunityState();
  for (int i = 0; i < 6 && THEME[theme][i].n; i++) for (int c = 0; c < THEME[theme][i].n && nFish < MAXFISH; c++) initFish(fish[nFish++], THEME[theme][i].sp);
  if (theme == ABYSS) initAbyss();
  if (theme == POKEMON) initPokemon();
  bakeM = -1;
  basePrepared = false; causReady = false;
}
static const char *lightName(int m) {
  static const char *N[4] = { "DAY LIGHTS", "DUSK", "NIGHT GLOW", "DAY-NIGHT CYCLE" };
  return N[((m % 4) + 4) % 4];
}

// ---- label ----
static const char *labelText = 0;
static float labelT = 0.f;
static void showLabel(const char *s) { labelText = s; labelT = 2.2f; }

// ======================================================================
// simulation
// ======================================================================
static inline bool wants(const Fish &f, const Food &fd) {
  const Species &s = SP[f.sp];
  if (fd.kind == 1) return s.eatsWafer && fd.state == 2;
  if (!s.eatsFlake) return false;
  if (s.beh == B_BOTTOM) return fd.state == 2 || fd.y > bandY(0.62f);
  if (s.beh == B_HOVER) return fd.y < bandY(0.5f);
  return fd.state != 2;
}
static void nibble(Food &fd, float k) {
  fd.mass -= 0.0016f * k;
  fd.r = 1.4f + 2.8f * (fd.mass > 0 ? fd.mass : 0.f);
  if (fd.mass <= 0.f && fd.on) { fd.on = false; eaten++; }
}
static void chain(Fish &f) {
  float L = SP[f.sp].L / (SPINE - 1);
  f.sx[0] = f.x; f.sy[0] = f.y;
  for (int i = 1; i < SPINE; i++) {
    float px = f.sx[i], py = f.sy[i];
    if (i >= 2) {   // lean toward continuing the previous segment so the body doesn't kink
      float ex = f.sx[i - 1] - f.sx[i - 2], ey = f.sy[i - 1] - f.sy[i - 2];
      px += (f.sx[i - 1] + ex - px) * 0.18f; py += (f.sy[i - 1] + ey - py) * 0.18f;
    }
    float dx = px - f.sx[i - 1], dy = py - f.sy[i - 1], d2 = dx * dx + dy * dy;
    if (d2 < 1e-6f) { dx = -f.hc; dy = -f.hs; d2 = 1.f; }
    float s = L / sqrtf(d2);
    f.sx[i] = f.sx[i - 1] + dx * s; f.sy[i] = f.sy[i - 1] + dy * s;
  }
  f.hpos = (uint8_t)((f.hpos + 1) & 7); f.hx[f.hpos] = f.x; f.hy[f.hpos] = f.y;
}
static void pickAnchor(Fish &f) {
  if (frand() < 0.62f) {
    bool left = frand() < 0.5f;
    f.ax = left ? 3.5f : W - 3.5f; f.ay = bandY(rnd(0.12f, 0.82f)); f.aface = -PIF * 0.5f;
  } else {
    const Rock &rk = rocks[xr() & 1];
    float dx = rnd(-0.55f, 0.55f) * rk.rx, u = clampf(dx / rk.rx, -0.95f, 0.95f);
    f.ax = rk.x + dx; f.ay = rk.y - rk.ry * sqrtf(1.f - u * u) - 1.5f; f.aface = frand() < 0.5f ? 0.f : PIF;
  }
  f.afood = -1; f.hasAnchor = true; f.state = 0;
}
static void updateOto(Fish &f, float k, float dt) {
  const Species &s = SP[f.sp];
  if (!f.hasAnchor || f.afood < 0) {
    for (int q = 0; q < MAXFOOD; q++) {
      Food &fd = food[q];
      if (fd.on && fd.kind == 1 && fd.state == 2 && frand() < 0.006f * k) {
        f.ax = fd.x + rnd(-2.5f, 2.5f); f.ay = fd.y - 1.5f; f.aface = frand() < 0.5f ? 0.f : PIF;
        f.afood = (int8_t)q; f.aserial = fd.serial; f.hasAnchor = true; f.state = 0; break;
      }
    }
  }
  if (!f.hasAnchor) pickAnchor(f);
  if (f.afood >= 0 && (!food[f.afood].on || food[f.afood].serial != f.aserial)) pickAnchor(f);
  float dx = f.ax - f.x, dy = f.ay - f.y, d = sqrtf(dx * dx + dy * dy);
  if (f.state == 0) {
    float diff = wrapA(fatan2(dy, dx) - f.h);
    f.h = wrapA(f.h + clampf(diff, -s.turn * k, s.turn * k));
    float cruise = s.spd < 0.06f + d * 0.04f ? s.spd : 0.06f + d * 0.04f;
    if (fabsf(diff) > 1.2f) cruise *= 0.6f;
    f.v += (cruise - f.v) * 0.05f * k;
    float oc, os; fsincos(f.h, os, oc);
    f.x += oc * f.v * k; f.y += os * f.v * k;
    if (d < 1.6f) { f.state = 1; f.timer = rnd(6000.f, 22000.f); }
  } else {
    f.x += (f.ax - f.x) * 0.08f * k + (float)isin((uint16_t)(tph(KT_OTO) + f.side * 41722)) * (0.03f / 16384.f);
    f.y += (f.ay - f.y) * 0.08f * k;
    f.h = wrapA(f.h + wrapA(f.aface - f.h) * 0.05f * k);
    f.v = 0.f;
    f.timer -= dt;
    if (f.afood >= 0) nibble(food[f.afood], k * 0.6f);
    if (f.timer <= 0.f) f.hasAnchor = false;
  }
  f.x = clampf(f.x, 2.5f, W - 2.5f);
  f.y = clampf(f.y, SURFACE + 3.f, floorAt(f.x) - 1.f);
  syncFish(f);
}

// jellyfish: pulse to rise, drift, sink slowly between pulses
static void updateJelly(Fish &f, float k) {
  const Species &s = SP[f.sp];
  float prev = fsin(f.tail);
  f.tail += 0.075f * k; if (f.tail > TAUF) f.tail -= TAUF;
  float c = fsin(f.tail);
  if (c > 0.f && c > prev) { f.vy -= 0.018f * c * k; f.vx += rnd(-0.01f, 0.01f) * k; }   // bell contracting: thrust
  f.vy += 0.0035f * k;                                                                     // negative buoyancy
  f.vx *= 1.f - 0.015f * k; f.vy *= 1.f - 0.02f * k;
  float top = bandY(s.band0), bot = bandY(s.band1);
  if (f.y < top) f.vy += 0.01f * k;
  if (f.y > bot) f.vy -= 0.012f * k;
  if (f.x < 20.f) f.vx += 0.006f * k;
  if (f.x > W - 20.f) f.vx -= 0.006f * k;
  f.vx += flowX(f.x, f.y) * 0.004f * k;
  f.x += f.vx * k; f.y += f.vy * k;
  f.x = clampf(f.x, 6.f, W - 6.f); f.y = clampf(f.y, SURFACE + 8.f, layoutY(270.f));
}
// orb drifter: slow random walk, spinning
static void updateOrb(Fish &f, float k) {
  const Species &s = SP[f.sp];
  f.tail += 0.02f * k; if (f.tail > TAUF) f.tail -= TAUF;
  f.vx += rnd(-0.008f, 0.008f) * k; f.vy += rnd(-0.006f, 0.006f) * k;
  float sp2 = f.vx * f.vx + f.vy * f.vy;
  if (sp2 > 0.09f) { float sc = 0.3f / sqrtf(sp2); f.vx *= sc; f.vy *= sc; }
  float top = bandY(s.band0), bot = bandY(s.band1);
  if (f.y < top) f.vy += 0.006f * k;
  if (f.y > bot) f.vy -= 0.006f * k;
  if (f.x < 22.f) f.vx += 0.006f * k;
  if (f.x > W - 22.f) f.vx -= 0.006f * k;
  f.x += f.vx * k; f.y += f.vy * k;
  f.x = clampf(f.x, 8.f, W - 8.f); f.y = clampf(f.y, SURFACE + 10.f, layoutY(268.f));
}
// comb jelly: glides mouth-first on its comb rows, turning slowly; catches flakes that come close
static void updateComb(Fish &f, float k, float dt) {
  const Species &s = SP[f.sp];
  f.tail += 0.10f * k; if (f.tail > TAUF) f.tail -= TAUF;                 // comb-row light wave
  f.aface += 0.006f * k; if (f.aface > TAUF) f.aface -= TAUF;             // slow roll about the long axis
  f.wander = clampf(f.wander + rnd(-0.0015f, 0.0015f) * k, -0.008f, 0.008f);
  float turn = f.wander, spd = s.spd * (0.85f + 0.15f * fsin(f.tail * 0.25f));
  if (f.full > 0.f) f.full -= dt;
  else {
    int target = -1; float best = s.range * s.range;
    for (int q = 0; q < MAXFOOD; q++) {
      const Food &fd = food[q];
      if (!fd.on || fd.kind != 0 || fd.state == 2) continue;
      float dx = fd.x - f.x, dy = fd.y - f.y, d2 = dx * dx + dy * dy;
      if (d2 < best) { best = d2; target = q; }
    }
    if (target >= 0) {
      Food &fd = food[target];
      turn += clampf(wrapA(fatan2(fd.y - f.y, fd.x - f.x) - f.h) * 0.03f, -0.02f, 0.02f);
      spd *= 1.4f;
      float reach = s.L * 0.55f;
      if (best < reach * reach) { fd.on = false; f.full = rnd(s.full0, s.full1); eaten++; addSpark(fd.x, fd.y, 0.f, -0.15f, 1.2f, glowCol[f.sp]); }
    }
  }
  float top = bandY(s.band0), bot = bandY(s.band1), m = s.L * 0.8f + 8.f;
  if (f.x < m || f.x > W - m || f.y < top || f.y > bot) {
    float des = fatan2((top + bot) * 0.5f - f.y, W * 0.5f - f.x);
    turn += clampf(wrapA(des - f.h) * 0.02f, -0.015f, 0.015f);
  }
  f.h = wrapA(f.h + turn * k);
  float sn, cs; fsincos(f.h, sn, cs);
  f.vx = cs * spd; f.vy = sn * spd;
  f.x += f.vx * k; f.y += f.vy * k;
  f.x = clampf(f.x, 6.f, W - 6.f); f.y = clampf(f.y, SURFACE + 8.f, layoutY(268.f));
}
// seahorse: upright, slow, drifts between plants, bobs; picks off flakes that come close
static void updateSeahorse(Fish &f, float k, float dt) {
  const Species &s = SP[f.sp];
  f.tail += 0.09f * k; if (f.tail > TAUF) f.tail -= TAUF;
  int target = -1; float best = s.range * s.range;
  if (f.full <= 0.f) for (int q = 0; q < MAXFOOD; q++) {
    const Food &fd = food[q];
    if (!fd.on || fd.kind != 0 || fd.state == 2) continue;
    float dx = fd.x - f.x, dy = fd.y - (f.y - s.L * 0.4f), d2 = dx * dx + dy * dy;
    if (d2 < best) { best = d2; target = q; }
  }
  float tx = f.ax, ty = f.ay, spd = 0.12f;
  if (target >= 0) {
    Food &fd = food[target];
    tx = fd.x - f.up * s.L * 0.3f; ty = fd.y + s.L * 0.4f; spd = 0.22f;
    if (best < 30.f) { fd.on = false; f.full = rnd(s.full0, s.full1); eaten++; addSpark(fd.x, fd.y, 0.f, -0.2f, 1.2f, glowCol[f.sp]); }
  } else {
    f.timer -= dt;
    if (f.timer <= 0.f) {
      static const float PX[3] = { 55.f, 104.f, 150.f };
      f.ax = clampf(layoutX(PX[xr() % 3]) + rnd(-14.f, 14.f), 16.f, W - 16.f); f.ay = bandY(rnd(s.band0, s.band1));
      f.timer = rnd(4000.f, 11000.f);
    }
  }
  if (f.full > 0.f) f.full -= dt;
  float dx = tx - f.x, dy = ty - f.y, d = sqrtf(dx * dx + dy * dy) + 0.001f;
  float want = d > 2.f ? spd : 0.f;
  f.vx += (dx / d * want - f.vx) * 0.03f * k;
  f.vy += (dy / d * want - f.vy) * 0.03f * k;
  f.x += f.vx * k; f.y += (f.vy + fsin(f.tail * 0.5f) * 0.05f) * k;
  if (f.vx > 0.03f) f.up = 1; else if (f.vx < -0.03f) f.up = -1;
  f.x = clampf(f.x, 12.f, W - 12.f); f.y = clampf(f.y, SURFACE + s.L * 0.6f, layoutY(262.f));
}

static void step(uint32_t dtMs) {
  if (dtMs > 48) dtMs = 48;
  tms += dtMs;
  float dt = (float)dtMs, k = dt / 16.667f;
  introT += dt * 0.001f;
  if (labelT > 0.f) labelT -= dt * 0.001f;

  float tgt = lightMode == 0 ? 1.f : lightMode == 1 ? 0.55f : lightMode == 2 ? 0.2f : 0.6f + 0.4f * (float)icos(tph(KT_CYCLE)) * (1.f / 16384.f);
  dayLight = tgt;

#if GT_AUTOFEED_S > 0
  if (introT >= autoFeedAt) { feed(rnd(34.f, 138.f)); autoFeedAt = introT + GT_AUTOFEED_S * rnd(0.85f, 1.15f); }
#endif

  if (theme == ABYSS) {
    dayLight = 0.2f; lightsF = 1.f; ambF = 0.2f; nightF = 0.8f; mF = 0.28f;
    stepAbyss(dtMs);
    return;
  }

  if (theme == POKEMON) {
    dayLight=0.55f; lightsF=1.f; ambF=0.55f; nightF=0.45f; mF=0.62f;
    stepPokemon(dtMs); return;
  }

  for (int i = 0; i < nFish; i++) {
    Fish &f = fish[i];
    const Species &s = SP[f.sp];
    if (s.beh == B_CLING) { updateOto(f, k, dt); f.tail += (0.04f + f.v * 0.55f) * k; if (f.tail > TAUF) f.tail -= TAUF; chain(f); continue; }
    if (s.beh == B_COMB) { updateComb(f, k, dt); syncFish(f); chain(f); continue; }
    if (s.beh == B_JELLY || s.beh == B_ORB || s.beh == B_SEAHORSE) {
      if (s.beh == B_JELLY) updateJelly(f, k); else if (s.beh == B_ORB) updateOrb(f, k); else updateSeahorse(f, k, dt);
      f.h = fatan2(f.vy, f.vx + 1e-4f); syncFish(f); chain(f); continue;
    }

    float hx = f.hc, hy = f.hs;
    f.wander = clampf(f.wander + rnd(-0.06f, 0.06f) * k, -0.7f, 0.7f);
    float wx, wy; fsincos(f.h + f.wander, wy, wx);
    float cruise = s.spd, topM = SURFACE + 14.f, level = 0.55f;
    bool floorFeel = true;

    int target = -1; float best = s.range * s.range;
    if (f.full <= 0.f && !(s.beh == B_BOTTOM && f.state >= 2)) {
      int qr = (int)(s.range * 16.f) + 32;
      for (int q = 0; q < MAXFOOD; q++) {
        const Food &fd = food[q];
        if (!fd.on) continue;
        int qdx = fd.qx - f.qx, qdy = fd.qy - f.qy;
        if (qdx > qr || qdx < -qr || qdy > qr || qdy < -qr) continue;   // integer prefilter: far outside range
        if (!wants(f, fd)) continue;
        float dx = fd.x - f.x, dy = fd.y - f.y, d2 = dx * dx + dy * dy;
        if (d2 < best) { best = d2; target = q; }
      }
    }
    if (target >= 0) {
      Food &tf = food[target];
      float dd = sqrtf(best); if (dd < 0.001f) dd = 0.001f;
      wx += (tf.x - f.x) / dd * 3.2f; wy += (tf.y - f.y) / dd * 3.2f;
      cruise = s.spd * s.dash; level = 1.f; topM = SURFACE + 3.f;
      if (tf.kind == 1) {
        if (dd < s.L * 0.15f + tf.r + 1.f) { cruise = 0.02f; nibble(tf, k); }
      } else if (dd < s.L * 0.15f + tf.r + 1.2f) {
        tf.on = false;
        f.full = rnd(s.full0, s.full1); f.flash = 1.f; eaten++;
        for (int q = 0; q < 5; q++) addSpark(tf.x, tf.y, rnd(-0.6f, 0.6f), rnd(-0.6f, 0.6f), 1.2f, glowCol[f.sp]);
      }
    }

    if (s.beh == B_SCHOOL) {
      float bt = bandY(s.band0), bb = bandY(s.band1);
      if (target < 0) { if (f.y < bt) wy += (bt - f.y) * 0.03f; if (f.y > bb) wy -= (f.y - bb) * 0.03f; }
      float cx = 0, cy = 0, axs = 0, ays = 0; int cnt = 0;
      const int qrad = 46 * 16;   // rad is at most 45 px squared-distance; prefilter in 1/16 px
      for (int j = 0; j < nFish; j++) {
        if (j == i || fish[j].sp != f.sp) continue;
        int qdx = fish[j].qx - f.qx, qdy = fish[j].qy - f.qy;
        if (qdx > qrad || qdx < -qrad || qdy > qrad || qdy < -qrad) continue;
        float ox = fish[j].x - f.x, oy = fish[j].y - f.y;
        if (ox * ox + oy * oy > s.rad) continue;
        cnt++; cx += fish[j].x; cy += fish[j].y; axs += fish[j].hc; ays += fish[j].hs;
      }
      if (cnt && target < 0) {
        float ic = 1.f / cnt;
        wx += (cx * ic - f.x) * s.coh + axs * ic * s.ali;
        wy += (cy * ic - f.y) * s.coh + ays * ic * s.ali;
      }
    } else if (s.beh == B_HOVER) {
      topM = SURFACE + (target >= 0 ? 3.f : 8.f);
      level = target >= 0 ? 1.f : 0.4f;
      float ht = bandY(s.band0), hb = bandY(s.band1);
      if (target < 0) {
        if (f.y < ht) wy += (ht - f.y) * 0.02f;
        if (f.y > hb) wy -= (f.y - hb) * 0.03f;
        f.timer -= dt;
        if (f.timer <= 0.f) { f.state = f.state ? 0 : 1; f.timer = f.state ? rnd(1500.f, 4500.f) : rnd(2500.f, 6000.f); }
        if (f.state == 1) cruise = 0.03f;
        for (int m = 0; m < nFish; m++) {
          if (m == i || fish[m].sp != f.sp) continue;
          float gx = fish[m].x - f.x, gy = fish[m].y - f.y, gd2 = gx * gx + gy * gy;
          if (gd2 > 1800.f) { float gd = sqrtf(gd2); wx += gx / gd * 0.5f; wy += gy / gd * 0.3f; }
        }
      } else f.state = 0;
    } else if (s.beh == B_BOTTOM) {
      floorFeel = false; level = 1.f;
      float fl = floorAt(f.x) - 4.f;
      if (f.state == 2) {                      // dart up for a gulp of air
        wx = hx * 0.25f; wy = -4.f; cruise = 0.85f; topM = SURFACE + 1.f;
        if (f.y < SURFACE + 5.f) { f.state = 3; f.bubbled = false; }
      } else if (f.state == 3) {               // glide back down
        wx = hx; wy = 3.f; cruise = 0.65f;
        if (!f.bubbled && f.y > bandY(0.45f)) { addBubble(f.x, f.y - 2.f, 0.9f, -0.45f); f.bubbled = true; }
        if (f.y > fl - 4.f) { f.state = 0; f.timer = rnd(3000.f, 8000.f); }
      } else {
        wy += (fl - f.y) * 0.08f;
        if (target < 0) {
          f.wander = clampf(f.wander + rnd(-0.1f, 0.1f) * k, -1.1f, 1.1f);
          f.timer -= dt;
          if (f.timer <= 0.f) {
            if (f.state == 0 && frand() < 0.35f) { f.state = 1; f.timer = rnd(1500.f, 5000.f); }
            else { f.state = 0; f.timer = rnd(3000.f, 8000.f); }
          }
          if (f.state == 1) cruise = 0.015f;
          if (frand() < 0.00022f * k) f.state = 2;
          for (int c = 0; c < nFish; c++) {
            if (c == i || fish[c].sp != f.sp) continue;
            float cxd = fish[c].x - f.x;
            if (cxd > 40.f) wx += 0.12f; else if (cxd < -40.f) wx -= 0.12f;
          }
        } else if (f.state == 1) f.state = 0;
      }
    }

    if (level < 1.f) wy *= level;

    float sx = 0, sy = 0;
    for (int n = 0; n < nFish; n++) {
      if (n == i) continue;
      int qdx = fish[n].qx - f.qx, qdy = fish[n].qy - f.qy, qs = sepQ[f.sp][fish[n].sp];
      if (qdx >= qs || qdx <= -qs || qdy >= qs || qdy <= -qs) continue;   // integer prefilter
      float px2 = fish[n].x - f.x, py2 = fish[n].y - f.y, pd2 = px2 * px2 + py2 * py2;
      if (pd2 < sepTab[f.sp][fish[n].sp] && pd2 > 0.01f) { float ip = 1.f / pd2; sx -= px2 * ip; sy -= py2 * ip; }
    }
    wx += sx * 4.f; wy += sy * 4.f;

    // feeler: steer away from glass, surface, gravel and rocks before reaching them
    float look = 22.f + s.L * 0.6f + f.v * 14.f, il = 1.f / look;
    float px = f.x + hx * look, py = f.y + hy * look;
    float left = 10.f + s.L * 0.3f, right = W - left, bot = floorAt(px) - (target >= 0 ? 4.f : 14.f);
    float ix = 0, iy = 0; bool press = false;
    if (px < left) ix += left - px;
    if (px > right) ix -= px - right;
    if (py < topM) iy += topM - py;
    if (floorFeel && py > bot) iy -= py - bot;
    if (f.x < left) ix += (left - f.x) * 2.f;
    if (f.x > right) ix -= (f.x - right) * 2.f;
    if (f.y < topM) iy += (topM - f.y) * 2.f;
    if (floorFeel && f.y > floorAt(f.x) - 8.f) iy -= (f.y - (floorAt(f.x) - 8.f)) * 2.f;
    if (ix != 0.f || iy != 0.f) { press = true; wx += ix * il * 6.f; wy += iy * il * 6.f; }
    if (s.beh != B_BOTTOM || f.state < 2) {
      for (int r = 0; r < 3; r++) {
        const Rock &rk = rocks[r];
        if (py < rockSkipY[r]) continue;                 // feeler well above this rock: can't touch it
        float rdx = px - rk.x, rdy = (py - rk.y) * 1.8f, rd2 = rdx * rdx + rdy * rdy, lim = rk.rx + 8.f;
        if (rd2 < lim * lim && rd2 > 0.0001f) {
          float rd = sqrtf(rd2), push = (lim - rd) / lim;
          wx += rdx / rd * push * 5.f; wy += rdy / rd * push * 5.f; press = true;
        }
      }
    }

    float diff = wrapA(fatan2(wy, wx) - f.h);
    if (fabsf(diff) > 2.7f) diff = f.side * fabsf(diff);
    else if (frand() < 0.004f) f.side = (int8_t)-f.side;
    float maxTurn = s.turn * k * (press ? 1.25f : 1.f);
    f.h = wrapA(f.h + clampf(diff, -maxTurn, maxTurn));
    if (fabsf(diff) > 1.2f && cruise > 0.1f) cruise *= 0.8f;
    f.v += (cruise - f.v) * 0.04f * k;
    f.x += (fcos(f.h) * f.v + flowX(f.x, f.y) * 0.12f) * k;
    f.y += (fsin(f.h) * f.v + flowY(f.x, f.y) * 0.12f) * k;
    f.x = clampf(f.x, 3.f, W - 3.f);
    f.y = clampf(f.y, SURFACE + 2.f, floorAt(f.x) - 2.f);
    syncFish(f);
    f.tail += (0.10f + f.v * 0.55f) * k; if (f.tail > TAUF) f.tail -= TAUF;
    if (f.full > 0.f) f.full -= dt;
    if (f.flash > 0.f) f.flash -= dt * 0.004f;
    chain(f);
  }

  for (int a = 0; a < MAXFOOD; a++) {
    Food &fo = food[a];
    if (!fo.on) continue;
    fo.phase += 0.045f * k;
    if (fo.state == 0) {
      fo.vx *= 1.f - 0.03f * k;
      fo.x += (fo.vx + flowX(fo.x, SURFACE) * 0.35f) * k;
      fo.y = SURFACE + 1.5f + fsin(fo.phase * 1.3f) * 0.4f;
      fo.floatT -= k;
      if (fo.floatT <= 0.f) fo.state = 1;
    } else if (fo.state == 1) {
      float term = fo.kind == 1 ? fo.term : fo.term * SINK;
      fo.vy += (term - fo.vy) * (fo.kind == 1 ? 0.05f : 0.02f) * k;
      fo.x += (fo.kind == 1 ? fsin(fo.phase) * 0.05f : fsin(fo.phase) * 0.16f + flowX(fo.x, fo.y) * 0.25f) * k;
      fo.y += fo.vy * k * depthScale;
      float gy = floorAt(fo.x) - (fo.kind == 1 ? 1.5f : 1.f);
      if (fo.y >= gy) { fo.y = gy; fo.state = 2; }
    } else if (fo.kind == 0) {
      fo.life -= 0.0004f * k;
      if (fo.life <= 0.f) fo.on = false;
    }
    fo.x = clampf(fo.x, 3.f, W - 3.f);
    syncFood(fo);
  }

  if (frand() < 0.055f * k) addBubble(layoutX(133.f) + rnd(-3.f, 3.f), layoutY(288.f), rnd(0.7f, 1.9f), -rnd(0.25f, 0.55f));
  for (int b = 0; b < 48; b++) {
    Bubble &bu = bubbles[b];
    if (!bu.on) continue;
    bu.phase += 0.05f * k; bu.y += bu.vy * k; bu.x += fsin(bu.phase) * 0.24f * k;
    if (bu.y < SURFACE + 1.f) { bu.on = false; if (bu.r > 1.f) addRing(bu.x, 0.5f, 0.5f); }
  }
  for (int s = 0; s < 64; s++) {
    Spark &sk = sparks[s];
    if (!sk.on) continue;
    sk.x += sk.vx * k; sk.y += sk.vy * k; sk.vy += 0.004f * k;
    sk.life -= dt * 0.0022f;
    if (sk.life <= 0.f) sk.on = false;
  }
  for (int r = 0; r < 12; r++) {
    Ring &rg = rings[r];
    if (!rg.on) continue;
    rg.r += 0.32f * k; rg.life -= 0.012f * k;
    if (rg.life <= 0.f) rg.on = false;
  }
#if GT_SNOW
  for (int m = 0; m < 110; m++) {
    Mote &mo = motes[m];
    mo.ph += 0.012f * k; if (mo.ph > TAUF) mo.ph -= TAUF;
    mo.y += (0.010f + 0.022f * mo.z) * k;
    mo.x += (fsin(mo.ph) * 0.025f + flowX(mo.x, mo.y) * 0.18f * mo.z) * k;
    if (mo.x < 0) mo.x += W; if (mo.x >= W) mo.x -= W;
    if (mo.y > floorAt(mo.x)) { mo.y = SURFACE + 2.f; mo.x = rnd(0, (float)W - 0.01f); }
  }
#endif

  // lighting factors used by the renderer
  lightsF = smooth(0.9f, 2.5f, introT);
  ambF = lightsF * dayLight;
  nightF = 1.f - dayLight;
  mF = 1.f - ((1.f - ambF) * 0.9f + (1.f - lightsF) * 0.1f);
  updateRayGeometry();
}

// ======================================================================
// rendering
// ======================================================================
// prepBase touches only the backdrop cache, never fb, so it can run while the panel is still receiving fb
static void prepBase() {
  if (theme == POKEMON) { pokemonPrepareBase(); return; }
  if (!bgS) return;
  int mQ8 = clampi((int)(mF * 256.f), 0, 256);
  int nQ8 = clampi((int)(nightF * lightsF * 256.f), 0, 256);
  if (bakeM < 0 || abs(mQ8 - bakeM) >= 2 || abs(nQ8 - bakeNight) >= 2) {
    for (int y = 0; y < H; y++) bakeRow(y, mQ8, nQ8, bgS);  // light level changed: full re-bake
    bakeM = mQ8; bakeNight = nQ8; bakeCursor = 0;
  } else if (theme != ABYSS && theme != POKEMON) {
    for (int n = 0; n < H / 40; n++) { bakeRow(bakeCursor, bakeM, bakeNight, bgS); bakeCursor = (bakeCursor + 1) % H; }   // ~40 frames per refresh in either orientation; keep the pixel budget steady
  }
}
static void renderBase() {
  if (theme == POKEMON) { pokemonRenderBase(); return; }
  if (!bgS) {                                              // no room for the cache: draw it fresh every frame
    int mQ8 = clampi((int)(mF * 256.f), 0, 256), nQ8 = clampi((int)(nightF * lightsF * 256.f), 0, 256);
    for (int y = 0; y < H; y++) bakeRow(y, mQ8, nQ8, fb);
    return;
  }
  if (!basePrepared) prepBase();
  basePrepared = false;
  memcpy(fb, bgS, sizeof(fb));
}

// caustics: sum of three sines on a 2px grid, lit where the sum crosses zero
// Caustic cache is shared by both layouts.
static uint16_t cColA1[160], cColA2[160], cColA3[160], cRowA2[42], cRowA3[42];
static uint8_t cFade[42];
static void initCaustics() {
  for (int x = 0; x < CGW; x++) { float u = x * 2.f + 1.f; cColA1[x] = bam(u * 0.23f); cColA2[x] = bam(u * 0.08f); cColA3[x] = bam(u * 0.11f); }
  for (int y = 0; y < CGH; y++) {
    float wy = CAUS_TOP + y * 2.f + 1.f;
    cRowA2[y] = bam(wy * 0.31f); cRowA3[y] = (uint16_t)(-(int)bam(wy * 0.19f));
    cFade[y] = (uint8_t)clampi((int)(smooth((float)CAUS_TOP, layoutY(284.f), wy) * 255.f), 0, 255);
  }
}
static uint8_t causQ[CAUSTIC_CELLS];
// the sine sums, into a small grid; touches no fb, so it can run during the panel push
static void prepCaustics() {
  if (theme == ABYSS || theme == POKEMON) { causReady = false; return; }
#if GT_CAUSTICS
  int kq = (int)(0.32f * mF * 256.f);
  uint16_t T1 = tph(KT_C1), T2 = tph(KT_C2), T3 = tph(KT_C3);
  int16_t colSine[160];
  for (int cx = 0; cx < CGW; cx++) colSine[cx] = (int16_t)isin((uint16_t)(cColA1[cx] + T1));
  for (int cy = 0; cy < CGH; cy++) {
    int fade = cFade[cy];
    uint8_t *row = causQ + cy * CGW;
    if (!fade || kq <= 2) { memset(row, 0, CGW); continue; }
    for (int cx = 0; cx < CGW; cx++) {
      int s = colSine[cx] + isin((uint16_t)(cRowA2[cy] + cColA2[cx] - T2)) + isin((uint16_t)(cColA3[cx] + cRowA3[cy] + T3));
      int c = 16384 - ((abs(s) * 14746) >> 14);
      int q = 0;
      if (c > 0) { int c8 = c >> 6; c8 = (((c8 * c8) >> 8) * c8) >> 8; q = (((c8 * fade) >> 8) * kq) >> 8; }
      row[cx] = (uint8_t)(q > 255 ? 255 : q);
    }
  }
  causReady = true;
#endif
}
static void renderCaustics() {
  if (theme == ABYSS) return;
#if GT_CAUSTICS
  if (!causReady) prepCaustics();
  causReady = false;
  for (int cy = 0; cy < CGH; cy++) {
    const uint8_t *row = causQ + cy * CGW;
    for (int cx = 0; cx < CGW; cx++) {
      int q = row[cx];
      if (q < 2) continue;
      int x = cx * 2, y = CAUS_TOP + cy * 2;
      addPx(x, y, 150, 226, 255, q); addPx(x + 1, y, 150, 226, 255, q);
      addPx(x, y + 1, 150, 226, 255, q); addPx(x + 1, y + 1, 150, 226, 255, q);
    }
  }
#endif
}

static int plantTipStart[6], plantTipCount[6];
static void renderPlants(bool front) {
  int mQ8 = clampi((int)((0.3f + 0.7f * mF) * 256.f), 0, 256);   // plants stay a little visible in the dark
  int frontA = (int)(0.6f * (0.3f + 0.7f * ambF) * 256.f);
  for (int i = 0; i < 6; i++) {
    if (plants[i].front != front) continue;
    plantTipStart[i] = tipN;
    walkPlant<true>(plants[i], 1, 0, mQ8, frontA);
    plantTipCount[i] = tipN - plantTipStart[i];
  }
}

static void renderMid() {
  if (theme == POKEMON) return;
  if (theme == ABYSS) return;
  renderCaustics();
  tipN = 0;
  renderPlants(false);
  int mQ8 = clampi((int)(mF * 256.f), 0, 256);
  RGB8 wc = { (uint8_t)((0x2d * mQ8) >> 8), (uint8_t)((0x3a * mQ8) >> 8), (uint8_t)((0x17 * mQ8) >> 8) };
  for (int w = 0; w < MAXFOOD; w++) {
    const Food &fd = food[w];
    if (!fd.on || fd.kind != 1) continue;
    int rx = (int)(fd.r + 0.5f), ry = (int)(fd.r * 0.45f + 0.5f); if (ry < 1) ry = 1;
    for (int y = -ry; y <= ry; y++) for (int x = -rx; x <= rx; x++)
      if (x * x * ry * ry + y * y * rx * rx <= rx * rx * ry * ry) blendPx((int)fd.x + x, (int)fd.y + y, wc, 256);
  }
}

// ---- fish bodies: simple geometry along the spine (discs + triangles), no sprites ----
// anti-aliased solid disc, alpha-blended (a in Q8)
static void disc(float cx, float cy, float r, RGB8 c, int a) {
  if (r < 0.35f || a <= 0) return;
  int x0 = ifloor(cx - r - 1.f), x1 = ifloor(cx + r + 1.f), y0 = ifloor(cy - r - 1.f), y1 = ifloor(cy + r + 1.f);
  if (x1 < 0 || y1 < 0 || x0 >= W || y0 >= H) return;
  if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0; if (x1 > W - 1) x1 = W - 1; if (y1 > H - 1) y1 = H - 1;
  int cxq = (int)(cx * 16.f), cyq = (int)(cy * 16.f), rq = (int)(r * 16.f);
  int r2 = rq * rq, m = (8 << 16) / rq;
  for (int y = y0; y <= y1; y++) {
    int dy = y * 16 + 8 - cyq, dy2 = dy * dy;
    for (int x = x0; x <= x1; x++) {
      int dx = x * 16 + 8 - cxq;
      int cov = (((r2 - dx * dx - dy2) * m) >> 16) + 128;   // ~ (r - d + 0.5) in Q8
      if (cov <= 0) continue;
      if (cov > 256) cov = 256;
      blendPx(x, y, c, (cov * a) >> 8);
    }
  }
}
// anti-aliased triangle (2x2 supersampled edge functions), alpha-blended
static void tri(float ax, float ay, float bx, float by, float cx, float cy, RGB8 c, int a) {
  if (a <= 0) return;
  int X0 = (int)(ax * 16.f), Y0 = (int)(ay * 16.f), X1 = (int)(bx * 16.f), Y1 = (int)(by * 16.f), X2 = (int)(cx * 16.f), Y2 = (int)(cy * 16.f);
  int area = (X1 - X0) * (Y2 - Y0) - (Y1 - Y0) * (X2 - X0);
  if (area == 0) return;
  if (area < 0) { int t = X1; X1 = X2; X2 = t; t = Y1; Y1 = Y2; Y2 = t; }
  int minx = (X0 < X1 ? (X0 < X2 ? X0 : X2) : (X1 < X2 ? X1 : X2)) >> 4, maxx = (X0 > X1 ? (X0 > X2 ? X0 : X2) : (X1 > X2 ? X1 : X2)) >> 4;
  int miny = (Y0 < Y1 ? (Y0 < Y2 ? Y0 : Y2) : (Y1 < Y2 ? Y1 : Y2)) >> 4, maxy = (Y0 > Y1 ? (Y0 > Y2 ? Y0 : Y2) : (Y1 > Y2 ? Y1 : Y2)) >> 4;
  if (minx < 0) minx = 0; if (miny < 0) miny = 0; if (maxx > W - 1) maxx = W - 1; if (maxy > H - 1) maxy = H - 1;
  const int ex0 = -(Y1 - Y0) * 8, ex1 = -(Y2 - Y1) * 8, ex2 = -(Y0 - Y2) * 8;
  const int ey0 = (X1 - X0) * 8, ey1 = (X2 - X1) * 8, ey2 = (X0 - X2) * 8;
  for (int y = miny; y <= maxy; y++) {
    const int px = minx * 16 + 4, py = y * 16 + 4;
    int e0 = (X1 - X0) * (py - Y0) - (Y1 - Y0) * (px - X0);
    int e1 = (X2 - X1) * (py - Y1) - (Y2 - Y1) * (px - X1);
    int e2 = (X0 - X2) * (py - Y2) - (Y0 - Y2) * (px - X2);
    for (int x = minx; x <= maxx; x++, e0 += 2 * ex0, e1 += 2 * ex1, e2 += 2 * ex2) {
      int hits = ((e0 | e1 | e2) >= 0)
               + (((e0 + ex0) | (e1 + ex1) | (e2 + ex2)) >= 0)
               + (((e0 + ey0) | (e1 + ey1) | (e2 + ey2)) >= 0)
               + (((e0 + ex0 + ey0) | (e1 + ex1 + ey1) | (e2 + ex2 + ey2)) >= 0);
    if (hits) blendPx(x, y, c, (hits * a) >> 2);
    }
  }
}
static inline RGB8 shade(RGB8 c, float k) {
  RGB8 o = { (uint8_t)clampi((int)(c.r * k), 0, 255), (uint8_t)clampi((int)(c.g * k), 0, 255), (uint8_t)clampi((int)(c.b * k), 0, 255) };
  return o;
}
// body half-depth profile along t (0 = snout, 1 = tail root): blunt head, deepest ~0.25, slim peduncle
static inline float bodyProfile(float t) {
  if (t < 0.22f) return 0.55f + 0.45f * fsin(t * (1.5708f / 0.22f));
  if (t < 0.86f) return 0.22f + 0.78f * fcos((t - 0.22f) * (1.5708f / 0.64f));
  return 0.22f;
}
// point and "up" normal on the spine at t (0..1)
static void spineAt(const Fish &f, float t, float &x, float &y, float &nx, float &ny) {
  float u = t * (SPINE - 1); int i = (int)u; if (i > SPINE - 2) i = SPINE - 2;
  float fr = u - i;
  x = f.sx[i] + (f.sx[i + 1] - f.sx[i]) * fr; y = f.sy[i] + (f.sy[i + 1] - f.sy[i]) * fr;
  float dx = f.sx[i] - f.sx[i + 1], dy = f.sy[i] - f.sy[i + 1];   // points toward the snout
  float il = 1.f / (fabsf(dx) + fabsf(dy) + 1e-4f);                 // cheap normalise; exactness doesn't matter here
  dx *= il; dy *= il;
  nx = dy * f.up; ny = -dx * f.up;
}

// Each fish is rasterised into two small coverage buffers (fins, body) using max(), then
// blended onto the frame once. Overlapping discs cost a byte compare instead of a blend.
static const int CB = 80;
static uint8_t cbBody[CB * CB], cbFin[CB * CB];
static int cbX0, cbY0, cbW, cbH;
// Conservative written spans shared by both coverage buffers. Empty padding never blends.
static uint8_t cbLo[CB], cbHi[CB];
static inline void cbSpan(int y, int lo, int hi) {
  if (lo > hi) return;
  if (lo < cbLo[y]) cbLo[y] = (uint8_t)lo;
  if (hi + 1 > cbHi[y]) cbHi[y] = (uint8_t)(hi + 1);
}
static void cbDisc(uint8_t *buf, float cx, float cy, float r) {
  if (r < 0.35f) return;
  int x0 = ifloor(cx - r - 1.f) - cbX0, x1 = ifloor(cx + r + 1.f) - cbX0, y0 = ifloor(cy - r - 1.f) - cbY0, y1 = ifloor(cy + r + 1.f) - cbY0;
  if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0; if (x1 > cbW - 1) x1 = cbW - 1; if (y1 > cbH - 1) y1 = cbH - 1;
  int cxq = (int)((cx - cbX0) * 16.f), cyq = (int)((cy - cbY0) * 16.f), rq = (int)(r * 16.f);
  int r2 = rq * rq, m = (8 << 16) / rq;
  for (int y = y0; y <= y1; y++) {
    int dy = y * 16 + 8 - cyq, dy2 = dy * dy;
    cbSpan(y, x0, x1);
    uint8_t *row = buf + y * CB;
    for (int x = x0; x <= x1; x++) {
      int dx = x * 16 + 8 - cxq;
      int cov = (((r2 - dx * dx - dy2) * m) >> 16) + 128;
      if (cov <= 0) continue;
      if (cov > 255) cov = 255;
      if (cov > row[x]) row[x] = (uint8_t)cov;
    }
  }
}
static void cbTri(uint8_t *buf, float ax, float ay, float bx, float by, float cx, float cy) {
  int X0 = (int)((ax - cbX0) * 16.f), Y0 = (int)((ay - cbY0) * 16.f), X1 = (int)((bx - cbX0) * 16.f), Y1 = (int)((by - cbY0) * 16.f);
  int X2 = (int)((cx - cbX0) * 16.f), Y2 = (int)((cy - cbY0) * 16.f);
  int area = (X1 - X0) * (Y2 - Y0) - (Y1 - Y0) * (X2 - X0);
  if (area == 0) return;
  if (area < 0) { int t = X1; X1 = X2; X2 = t; t = Y1; Y1 = Y2; Y2 = t; }
  int minx = (X0 < X1 ? (X0 < X2 ? X0 : X2) : (X1 < X2 ? X1 : X2)) >> 4, maxx = (X0 > X1 ? (X0 > X2 ? X0 : X2) : (X1 > X2 ? X1 : X2)) >> 4;
  int miny = (Y0 < Y1 ? (Y0 < Y2 ? Y0 : Y2) : (Y1 < Y2 ? Y1 : Y2)) >> 4, maxy = (Y0 > Y1 ? (Y0 > Y2 ? Y0 : Y2) : (Y1 > Y2 ? Y1 : Y2)) >> 4;
  if (minx < 0) minx = 0; if (miny < 0) miny = 0; if (maxx > cbW - 1) maxx = cbW - 1; if (maxy > cbH - 1) maxy = cbH - 1;
  for (int y = miny; y <= maxy; y++) cbSpan(y, minx, maxx);
  const int ex0 = -(Y1 - Y0) * 8, ex1 = -(Y2 - Y1) * 8, ex2 = -(Y0 - Y2) * 8;
  const int ey0 = (X1 - X0) * 8, ey1 = (X2 - X1) * 8, ey2 = (X0 - X2) * 8;
  for (int y = miny; y <= maxy; y++) {
    const int px = minx * 16 + 4, py = y * 16 + 4;
    int e0 = (X1 - X0) * (py - Y0) - (Y1 - Y0) * (px - X0);
    int e1 = (X2 - X1) * (py - Y1) - (Y2 - Y1) * (px - X1);
    int e2 = (X0 - X2) * (py - Y2) - (Y0 - Y2) * (px - X2);
    for (int x = minx; x <= maxx; x++, e0 += 2 * ex0, e1 += 2 * ex1, e2 += 2 * ex2) {
      int hits = ((e0 | e1 | e2) >= 0)
               + (((e0 + ex0) | (e1 + ex1) | (e2 + ex2)) >= 0)
               + (((e0 + ey0) | (e1 + ey1) | (e2 + ey2)) >= 0)
               + (((e0 + ex0 + ey0) | (e1 + ex1 + ey1) | (e2 + ex2 + ey2)) >= 0);
    if (hits) { int v = hits * 64 - 1; uint8_t &c = buf[y * CB + x]; if (v > c) c = (uint8_t)v; }
    }
  }
}
// ---- integer spine: points in 1/16 px, per-segment "up" normals in Q8, profile LUT ----
struct SpineQ { int32_t x[6], y[6]; int32_t nx[5], ny[5]; };
static uint16_t PROFQ[65], PROFS[65];   // bodyProfile(i/64) in Q8; PROFS = shark (pointed snout)
static void buildProfile() {
  for (int i = 0; i <= 64; i++) {
    float t = i / 64.f;
    PROFQ[i] = (uint16_t)(bodyProfile(t) * 256.f + 0.5f);
    float sp = t < 0.3f ? 0.18f + 0.82f * sinf(t / 0.3f * 1.5708f) : (t < 0.88f ? 0.2f + 0.8f * cosf((t - 0.3f) / 0.58f * 1.5708f) : 0.2f);
    PROFS[i] = (uint16_t)(sp * 256.f + 0.5f);
  }
}
static void spineQ(const Fish &f, SpineQ &q) {
  for (int i = 0; i < SPINE; i++) { q.x[i] = (int32_t)(f.sx[i] * 16.f); q.y[i] = (int32_t)(f.sy[i] * 16.f); }
  for (int i = 0; i < SPINE - 1; i++) {
    int dx = q.x[i] - q.x[i + 1], dy = q.y[i] - q.y[i + 1];   // toward the snout
    int len = (int)sqrtf((float)(dx * dx + dy * dy)); if (len < 1) len = 1;
    q.nx[i] = (dy * 256 / len) * f.up; q.ny[i] = (-dx * 256 / len) * f.up;
  }
}
// t in Q10 (0..1024); returns point (Q4) and normal (Q8)
static inline void spineAtQ(const SpineQ &q, int t10, int &x, int &y, int &nx, int &ny) {
  int u = t10 * (SPINE - 1), i = u >> 10; if (i > SPINE - 2) i = SPINE - 2;
  int fr = u - (i << 10);
  x = q.x[i] + (((q.x[i + 1] - q.x[i]) * fr) >> 10); y = q.y[i] + (((q.y[i + 1] - q.y[i]) * fr) >> 10);
  nx = q.nx[i]; ny = q.ny[i];
}
static inline int profQ(int t10) { int i = t10 >> 4; return PROFQ[i > 64 ? 64 : i]; }
static inline int profS(int t10) { int i = t10 >> 4; return PROFS[i > 64 ? 64 : i]; }
// integer AA disc into a coverage buffer (centre and radius in 1/16 px, screen space)
static void cbDiscQ(uint8_t *buf, int cxq, int cyq, int rq) {
  if (rq < 6) return;
  cxq -= cbX0 * 16; cyq -= cbY0 * 16;
  int x0 = ((cxq - rq) >> 4) - 1, x1 = ((cxq + rq) >> 4) + 1, y0 = ((cyq - rq) >> 4) - 1, y1 = ((cyq + rq) >> 4) + 1;
  if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0; if (x1 > cbW - 1) x1 = cbW - 1; if (y1 > cbH - 1) y1 = cbH - 1;
  int r2 = rq * rq, m = (8 << 16) / rq;
  for (int y = y0; y <= y1; y++) {
    int dy = y * 16 + 8 - cyq, dy2 = dy * dy;
    cbSpan(y, x0, x1);
    uint8_t *row = buf + y * CB;
    for (int x = x0; x <= x1; x++) {
      int dx = x * 16 + 8 - cxq;
      int cov = (((r2 - dx * dx - dy2) * m) >> 16) + 128;
      if (cov <= 0) continue;
      if (cov > 255) cov = 255;
      if (cov > row[x]) row[x] = (uint8_t)cov;
    }
  }
}
// integer AA disc blended straight onto the frame
static void discQ(int cxq, int cyq, int rq, RGB8 c, int a) {
  if (rq < 6 || a <= 0) return;
  int x0 = ((cxq - rq) >> 4) - 1, x1 = ((cxq + rq) >> 4) + 1, y0 = ((cyq - rq) >> 4) - 1, y1 = ((cyq + rq) >> 4) + 1;
  if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0; if (x1 > W - 1) x1 = W - 1; if (y1 > H - 1) y1 = H - 1;
  int r2 = rq * rq, m = (8 << 16) / rq;
  for (int y = y0; y <= y1; y++) {
    int dy = y * 16 + 8 - cyq, dy2 = dy * dy;
    for (int x = x0; x <= x1; x++) {
      int dx = x * 16 + 8 - cxq;
      int cov = (((r2 - dx * dx - dy2) * m) >> 16) + 128;
      if (cov <= 0) continue;
      if (cov > 256) cov = 256;
      blendPx(x, y, c, (cov * a) >> 8);
    }
  }
}

static void drawFishBody(Fish &f, float bright, SpineQ &sq) {
  const Species &s = SP[f.sp];
  float ig = clampf((introT - f.ignite) / 0.35f, 0.f, 1.f);
  if (ig <= 0.f) return;
  // keep the dorsal side pointing up the screen, with hysteresis so it doesn't flicker when vertical
  float upY = -f.hc * f.up;
  if (upY > 0.25f) f.up = (int8_t)-f.up;
  spineQ(f, sq);
  int a = (int)(256.f * ig);
  RGB8 body = shade(s.body, bright), fin = shade(s.fin, bright), tailc = shade(s.tailc, bright);
  if (s.mark == MK_PRISM) {   // pearly body tinted by this fish's hue, saturated fins
    RGB8 h = hueCol(f.hue, 96), h2 = hueCol(f.hue + 60, 96);
    body = shade(RGB8{ (uint8_t)((h.r + 510) / 3), (uint8_t)((h.g + 510) / 3), (uint8_t)((h.b + 510) / 3) }, bright);
    fin = shade(h, bright); tailc = shade(h2, bright);
  }
  bool ownTail = tailc.r != fin.r || tailc.g != fin.g || tailc.b != fin.b;
  RGB8 dark = shade(body, 0.62f);
  float L = s.L, R = 0.5f * s.depth * L;

  // coverage buffer around the fish
  float mnx = f.sx[0], mxx = f.sx[0], mny = f.sy[0], mxy = f.sy[0];
  for (int i = 1; i < SPINE; i++) {
    if (f.sx[i] < mnx) mnx = f.sx[i]; if (f.sx[i] > mxx) mxx = f.sx[i];
    if (f.sy[i] < mny) mny = f.sy[i]; if (f.sy[i] > mxy) mxy = f.sy[i];
  }
  float pad = R + (s.tailL + 0.06f) * L + s.dorsalH * L + 2.f;
  cbX0 = ifloor(mnx - pad); cbY0 = ifloor(mny - pad);
  cbW = ifloor(mxx + pad) - cbX0 + 1; cbH = ifloor(mxy + pad) - cbY0 + 1;
  if (cbW > CB) cbW = CB; if (cbH > CB) cbH = CB;
  if (cbX0 >= W || cbY0 >= H || cbX0 + cbW <= 0 || cbY0 + cbH <= 0) return;
  for (int y = 0; y < CB; y++) {
    if (cbHi[y] > cbLo[y]) {
      memset(cbBody + y * CB + cbLo[y], 0, cbHi[y] - cbLo[y]);
      memset(cbFin + y * CB + cbLo[y], 0, cbHi[y] - cbLo[y]);
    }
    cbLo[y] = CB; cbHi[y] = 0;
  }

  // tail fin, flicking with the tail beat
  float bxp, byp, nx, ny;
  spineAt(f, 1.f, bxp, byp, nx, ny);
  float tx = f.sx[SPINE - 1] - f.sx[SPINE - 2], ty = f.sy[SPINE - 1] - f.sy[SPINE - 2];
  float il = 1.f / (sqrtf(tx * tx + ty * ty) + 1e-4f); tx *= il; ty *= il;
  float wag = fsin(f.tail), spread = L * s.tailS * (0.78f + 0.22f * wag), tl = L * s.tailL, lift = wag * L * 0.05f;
  float ox = bxp - tx * 0.8f, oy = byp - ty * 0.8f;
  float T[4][2][3];   // two tail triangles: [tri][x/y][vertex]
  if (s.tail == 0) {
    float nxp = bxp + tx * tl * 0.5f, nyp = byp + ty * tl * 0.5f;
    float ux = bxp + tx * tl + nx * (spread + lift), uy = byp + ty * tl + ny * (spread + lift);
    float lx = bxp + tx * tl - nx * (spread - lift), ly = byp + ty * tl - ny * (spread - lift);
    T[0][0][0] = ox; T[0][1][0] = oy; T[0][0][1] = ux; T[0][1][1] = uy; T[0][0][2] = nxp; T[0][1][2] = nyp;
    T[1][0][0] = ox; T[1][1][0] = oy; T[1][0][1] = nxp; T[1][1][1] = nyp; T[1][0][2] = lx; T[1][1][2] = ly;
  } else if (s.tail == 2) {   // shark: tall swept upper lobe, small lower lobe
    float nxp = bxp + tx * tl * 0.35f, nyp = byp + ty * tl * 0.35f;
    float ux = bxp + tx * tl * 1.1f + nx * (spread * 1.5f + lift), uy = byp + ty * tl * 1.1f + ny * (spread * 1.5f + lift);
    float lx = bxp + tx * tl * 0.55f - nx * (spread * 0.8f - lift), ly = byp + ty * tl * 0.55f - ny * (spread * 0.8f - lift);
    T[0][0][0] = ox; T[0][1][0] = oy; T[0][0][1] = ux; T[0][1][1] = uy; T[0][0][2] = nxp; T[0][1][2] = nyp;
    T[1][0][0] = ox; T[1][1][0] = oy; T[1][0][1] = nxp; T[1][1][1] = nyp; T[1][0][2] = lx; T[1][1][2] = ly;
  } else {
    float ex = bxp + tx * tl, ey = byp + ty * tl;
    float ux = bxp + tx * tl * 0.8f + nx * (spread + lift), uy = byp + ty * tl * 0.8f + ny * (spread + lift);
    float lx = bxp + tx * tl * 0.8f - nx * (spread - lift), ly = byp + ty * tl * 0.8f - ny * (spread - lift);
    T[0][0][0] = ox; T[0][1][0] = oy; T[0][0][1] = ux; T[0][1][1] = uy; T[0][0][2] = ex; T[0][1][2] = ey;
    T[1][0][0] = ox; T[1][1][0] = oy; T[1][0][1] = ex; T[1][1][1] = ey; T[1][0][2] = lx; T[1][1][2] = ly;
  }
  if (!ownTail) for (int q = 0; q < 2; q++) cbTri(cbFin, T[q][0][0], T[q][1][0], T[q][0][1], T[q][1][1], T[q][0][2], T[q][1][2]);
  // dorsal fin on the upper side (the panda cory's is black, drawn separately below)
  float dx0, dy0, dx1, dy1, n0x, n0y, n1x, n1y;
  spineAt(f, s.dorsal0, dx0, dy0, n0x, n0y); spineAt(f, s.dorsal1, dx1, dy1, n1x, n1y);
  float r0 = R * bodyProfile(s.dorsal0), r1 = R * bodyProfile(s.dorsal1), dh = s.dorsalH * L;
  float dpx = dx0 + n0x * r0 * 0.8f, dpy = dy0 + n0y * r0 * 0.8f, dqx = dx1 + n1x * r1 * 0.8f, dqy = dy1 + n1y * r1 * 0.8f;
  float dax = dqx + n1x * dh + (dx1 - dx0) * 0.35f, day = dqy + n1y * dh + (dy1 - dy0) * 0.35f;
  if (s.mark != MK_CORY) cbTri(cbFin, dpx, dpy, dqx, dqy, dax, day);
  // body: discs along the spine (all integer)
  int Rq = (int)(R * 16.f);
  int n = (int)(L * 0.8f) + 3;
  for (int i = 0; i <= n; i++) {
    int t10 = (i << 10) / n, x, y, qx, qy;
    spineAtQ(sq, t10, x, y, qx, qy);
    cbDiscQ(cbBody, x, y, (Rq * (s.mark == MK_SHARK ? profS(t10) : profQ(t10))) >> 8);
  }
  // composite: fins (translucent) then body
  int fa = (a * 150) >> 8;
  if (ownTail) for (int q = 0; q < 2; q++) tri(T[q][0][0], T[q][1][0], T[q][0][1], T[q][1][1], T[q][0][2], T[q][1][2], tailc, (a * 200) >> 8);
  for (int y = 0; y < cbH; y++) {
    int py = cbY0 + y; if ((unsigned)py >= (unsigned)H) continue;
    const uint8_t *rb = cbBody + y * CB, *rf = cbFin + y * CB;
    int lo = cbLo[y] > -cbX0 ? cbLo[y] : -cbX0;
    int hi = cbHi[y] < W - cbX0 ? cbHi[y] : W - cbX0;
    for (int x = lo; x < hi; x++) {
      int px = cbX0 + x; if ((unsigned)px >= (unsigned)W) continue;
      int cb = rb[x], cf = rf[x];
      if (cf && cb < 250) blendPx(px, py, fin, (fa * cf) >> 8);
      if (cb) blendPx(px, py, body, (a * cb) >> 8);
    }
  }
  // darker back for a little shape
  for (int i = 1; i < n; i += 3) {
    int t10 = (i << 10) / n, x, y, qx, qy;
    spineAtQ(sq, t10, x, y, qx, qy);
    int r = (Rq * profQ(t10)) >> 8;
    discQ(x + ((qx * r * 115) >> 16), y + ((qy * r * 115) >> 16), r >> 1, dark, (a * 110) >> 8);
  }
  // species markings
  if (s.mark == MK_CLOWN) {   // three white bands
    static const int TB[3] = { 205, 532, 860 };
    RGB8 wht = shade(RGB8{ 245, 245, 250 }, bright);
    for (int b = 0; b < 3; b++) {
      int x, y, qx, qy; spineAtQ(sq, TB[b], x, y, qx, qy);
      int r = (Rq * profQ(TB[b])) >> 8;
      for (int o = -1; o <= 1; o++) discQ(x + ((qx * r * o * 140) >> 16), y + ((qy * r * o * 140) >> 16), (r * (b == 2 ? 80 : 110)) >> 8, wht, (a * 235) >> 8);
    }
  } else if (s.mark == MK_TANG) {   // dark "palette" sweep along the upper body
    RGB8 nav = { 12, 18, 48 };
    for (int i = 0; i <= 6; i++) {
      int t10 = 140 + (620 * i) / 6, x, y, qx, qy; spineAtQ(sq, t10, x, y, qx, qy);
      int r = (Rq * profQ(t10)) >> 8;
      discQ(x + ((qx * r * 90) >> 16), y + ((qy * r * 90) >> 16), (r * 85) >> 8, nav, (a * 220) >> 8);
    }
  } else if (s.mark == MK_CORY) {   // panda: black mask over the eye, black dorsal, black tail root
    RGB8 blk = { 22, 22, 26 };
    tri(dpx, dpy, dqx, dqy, dax, day, blk, (a * 200) >> 8);
    int x, y, qx, qy;
    spineAtQ(sq, 123, x, y, qx, qy); discQ(x, y, (Rq * 205) >> 8, blk, (a * 230) >> 8);
    spineAtQ(sq, 952, x, y, qx, qy); discQ(x, y, (Rq * 108) >> 8, blk, (a * 200) >> 8);
  } else if (s.mark == MK_SHARK) {
    RGB8 belly = shade(RGB8{ 222, 228, 234 }, bright), gill = shade(RGB8{ 60, 70, 82 }, bright);
    for (int i = 0; i <= 9; i++) {   // countershaded belly
      int t10 = 100 + (620 * i) / 9, x, y, qx, qy; spineAtQ(sq, t10, x, y, qx, qy);
      int r = (Rq * profS(t10)) >> 8;
      discQ(x - ((qx * r * 110) >> 16), y - ((qy * r * 110) >> 16), (r * 120) >> 8, belly, (a * 230) >> 8);
    }
    {   // pectoral fin, swept back and down
      int x, y, qx, qy; spineAtQ(sq, 300, x, y, qx, qy);
      int r = (Rq * profS(300)) >> 8, x2, y2, q2x, q2y; spineAtQ(sq, 450, x2, y2, q2x, q2y);
      float ax = (x - ((qx * r * 200) >> 16)) / 16.f, ay = (y - ((qy * r * 200) >> 16)) / 16.f;
      float bx = (x2 - ((q2x * r * 200) >> 16)) / 16.f, by = (y2 - ((q2y * r * 200) >> 16)) / 16.f;
      float cx = (x2 - ((q2x * r * 700) >> 16)) / 16.f + (bx - ax) * 0.6f, cy = (y2 - ((q2y * r * 700) >> 16)) / 16.f + (by - ay) * 0.6f;
      tri(ax, ay, bx, by, cx, cy, fin, (a * 230) >> 8);
    }
    for (int g = 0; g < 3; g++) {    // gill slits
      int x, y, qx, qy; spineAtQ(sq, 205 + g * 26, x, y, qx, qy);
      int r = (Rq * profS(205 + g * 26)) >> 8;
      for (int o = -3; o <= 2; o++) blendPx((x + ((qx * r * o * 38) >> 16) + 8) >> 4, (y + ((qy * r * o * 38) >> 16) + 8) >> 4, gill, (a * 150) >> 8);
    }
  } else if (s.mark == MK_OTO) {   // oto: dark lateral stripe
    RGB8 st = shade(RGB8{ 70, 62, 44 }, bright);
    for (int i = 0; i <= 6; i++) {
      int t10 = 41 + (922 * i) / 6, x, y, qx, qy;
      spineAtQ(sq, t10, x, y, qx, qy); discQ(x, y, ((Rq * 72) >> 8) + 5, st, (a * 200) >> 8);
    }
  }
  // eye
  {
    int x, y, qx, qy;
    spineAtQ(sq, 102, x, y, qx, qy);
    int ex = x + ((qx * Rq * 51) >> 16), ey = y + ((qy * Rq * 51) >> 16);
    int px = (ex + 8) >> 4, py = (ey + 8) >> 4;
    if (s.mark == MK_CORY) blendPx(px, py, RGB8{ 200, 200, 200 }, (a * 200) >> 8);
    else if (L >= 16.f) discQ(ex, ey, 14, RGB8{ 12, 12, 16 }, (a * 230) >> 8);
    else blendPx(px, py, RGB8{ 12, 12, 16 }, (a * 220) >> 8);
  }
}

// the glowing part: body glow, neon stripes, flashes, trails (additive)
static void drawFishGlow(const Fish &f, float gm, bool trails, const SpineQ &sq) {
  (void)trails;
  const Species &s = SP[f.sp];
  float ig = clampf((introT - f.ignite) / 0.35f, 0.f, 1.f);
  if (ig <= 0.f) return;
  float am = gm * ig, L = s.L, R = 0.5f * s.depth * L;
  RGB8 gc = s.mark == MK_PRISM ? hueCol(f.hue, 96) : glowCol[f.sp];
#if GT_TRAILS
  if (trails && s.beh != B_CLING) {
    int h6 = (f.hpos + 2) & 7;
    float mdx = f.x - f.hx[h6], mdy = f.y - f.hy[h6];
    if (mdx * mdx + mdy * mdy > 2.f) {
      static const float TA[3] = { 0.22f, 0.12f, 0.06f };
      for (int g = 0; g < 3; g++) {
        int hi = (f.hpos + 8 - 2 * (g + 1)) & 7;
        blit(f.hx[hi], f.hy[hi], R * 1.6f, s.glowA * am * TA[g] * 2.f * TRAIL / 0.62f, gc);
      }
    }
  }
#endif
  int mxq, myq, qx, qy;
  spineAtQ(sq, 410, mxq, myq, qx, qy);
  float mx = mxq * (1.f / 16.f), my = myq * (1.f / 16.f);
  blit(mx, my, L * 0.42f * GLOW, s.glowA * 1.3f * am, gc);      // soft body glow
  if (s.mark == MK_PRISM) {   // iridescent shimmer running along the body
    int Rq = (int)(R * 16.f), q = (int)(0.9f * am * 255.f), sh = (int)(tms / 12);
    for (int i = 0; i <= 7; i++) {
      int t10 = 60 + (800 * i) / 7, x, y;
      spineAtQ(sq, t10, x, y, qx, qy);
      int r = (Rq * profQ(t10)) >> 8;
      addDot(x + ((qx * r * 40) >> 16), y + ((qy * r * 40) >> 16), hueCol(f.hue + i * 28 + sh, 96), q);
    }
  }
  if (s.mark == MK_NEON) {
    // neon: the electric stripe along the upper body (shifts blue-green with angle), red on the lower rear
    RGB8 cyan = hueCol(190 + (isin((uint16_t)(bam(f.h) + f.side * 10430)) * 14 >> 14), 96), red = hueCol(356, 92);
    int Rq = (int)(R * 16.f);
    int ca = (int)(0.65f * am * 255.f), ra = (int)(0.85f * am * 255.f);
    for (int i = 0; i <= 6; i++) {
      int t10 = 61 + (573 * i) / 6, x, y;
      spineAtQ(sq, t10, x, y, qx, qy);
      int r = (Rq * profQ(t10)) >> 8;
      addDot(x + ((qx * r * 77) >> 16), y + ((qy * r * 77) >> 16), cyan, ca + (ca >> 1));
    }
    for (int i = 0; i <= 4; i++) {
      int t10 = 492 + (410 * i) / 4, x, y;
      spineAtQ(sq, t10, x, y, qx, qy);
      int r = (Rq * profQ(t10)) >> 8;
      addDot(x - ((qx * r * 90) >> 16), y - ((qy * r * 90) >> 16), red, ra + (ra >> 1));
    }
  }
  if (f.flash > 0.f) blit(f.x, f.y, L * 0.6f, f.flash * 0.5f * am, gc);
  if (ig < 1.f) blit(mx, my, L * 0.9f, (1.f - ig) * ig * 1.6f, gc);   // ignition flare
}
// ---- jellyfish: translucent bell, 4-fold inner pattern, trailing tentacles ----
static void drawJelly(const Fish &f, float gm) {
  const Species &s = SP[f.sp];
  float ig = clampf((introT - f.ignite) / 0.35f, 0.f, 1.f);
  if (ig <= 0.f) return;
  bool rose = s.mark == MK_ROSE;
  float c = 0.5f + 0.5f * fsin(f.tail), R0 = s.L * 0.5f;
  float Rb = R0 * (1.f - 0.18f * c), Rh = R0 * 0.8f * (1.f + 0.14f * c);
  RGB8 bell = s.body, pat = s.fin;
  if (rose) { RGB8 h = hueCol(f.hue + (int)(tms / 40), 96); bell = RGB8{ (uint8_t)((h.r + 255) / 2), (uint8_t)((h.g + 255) / 2), (uint8_t)((h.b + 255) / 2) }; pat = hueCol(f.hue + 120 + (int)(tms / 40), 96); }
  int a = (int)(256.f * ig);
  blit(f.x, f.y - Rh * 0.3f, R0 * 1.3f, s.glowA * gm * ig, rose ? pat : glowCol[f.sp]);
  // bell: half-ellipse, brighter toward the rim
  int cxq = (int)(f.x * 16.f), y0 = ifloor(f.y - Rh), y1 = ifloor(f.y);
  for (int y = y0; y <= y1; y++) {
    float dy = ((float)y + 0.5f - f.y) / Rh;
    float w = Rb * sqrtf(clampf(1.f - dy * dy, 0.f, 1.f));
    int wq = (int)(w * 16.f); if (wq < 8) continue;
    int x0 = (cxq - wq) >> 4, x1 = (cxq + wq) >> 4;
    for (int x = x0; x <= x1; x++) {
      int dx = x * 16 + 8 - cxq;
      int e = (dx * dx * 256) / (wq * wq); if (e > 256) continue;   // 0 centre .. 256 edge
      blendPx(x, y, bell, (a * (70 + ((e * 130) >> 8))) >> 8);
    }
  }
  // rim
  for (int x = (int)(f.x - Rb); x <= (int)(f.x + Rb); x++) addPx(x, (int)f.y, bell.r, bell.g, bell.b, (a * 120) >> 8);
  // inner pattern: moon jelly's four horseshoes / rose jelly's three-petal rose curve r = |cos(k*theta)|
  int petals = rose ? 3 : 2, n = 32;
  for (int i = 0; i < n; i++) {
    uint16_t th = (uint16_t)(i * 65536 / n + (rose ? tph(KT_ANEM) : 0));
    int rr = abs(icos((uint16_t)(th * petals)));   // Q14
    float r = Rb * 0.5f * rr * (1.f / 16384.f);
    int xq = (int)((f.x + r * icos(th) * (1.f / 16384.f)) * 16.f), yq = (int)((f.y - Rh * 0.42f + r * 0.55f * isin(th) * (1.f / 16384.f)) * 16.f);
    addDot(xq, yq, rose ? hueCol(f.hue + i * 11 + (int)(tms / 30), 96) : pat, (a * 150) >> 8);
  }
  // tentacles: fine, trailing, waving
  uint16_t ph = (uint16_t)(f.tail * RAD2BAM);
  int nt = 9, len = (int)(R0 * 2.4f);
  for (int j = 0; j < nt; j++) {
    float x0 = f.x - Rb + 2.f * Rb * (j + 0.5f) / nt;
    RGB8 tc = rose ? hueCol(f.hue + j * 40 + (int)(tms / 30), 96) : bell;
    for (int m = 1; m < len; m++) {
      int sw = isin((uint16_t)(ph - m * 2600 + j * 7000));
      int xq = (int)(x0 * 16.f) + ((sw * m / len * 26) >> 14), yq = (int)(f.y * 16.f) + m * 16;
      addPx(xq >> 4, yq >> 4, tc.r, tc.g, tc.b, (a * (110 - 80 * m / len)) >> 9);
    }
  }
  // oral arms: four frilly ribbons from the centre
  for (int j = 0; j < 4; j++) {
    for (int m = 1; m < (int)(R0 * 1.4f); m++) {
      int sw = isin((uint16_t)(ph - m * 3000 + j * 16384));
      int xq = (int)((f.x + (j - 1.5f) * 1.4f) * 16.f) + ((sw * 22) >> 14), yq = (int)((f.y + m) * 16.f);
      addDot(xq, yq, pat, (a * 90) >> 8);
    }
  }
}

// ---- seahorse: an S-curve of discs with a curled tail, tube snout, coronet and fluttering fin ----
static float shX[20], shY[20], shR[20];   // unit-height shape, facing right, y down
static void initSeahorse() {
  for (int i = 0; i < 20; i++) {
    float t = i / 19.f, x, y, r;
    if (t <= 0.72f) {
      float u = t / 0.72f;
      x = sinf(u * PIF) * 0.14f - 0.02f; y = -0.45f + u * 0.70f;
      r = u < 0.12f ? 0.10f : (u < 0.22f ? 0.075f : 0.075f + 0.055f * sinf((u - 0.22f) / 0.78f * PIF));
    } else {
      float u = (t - 0.72f) / 0.28f, ex = sinf(PIF) * 0.14f - 0.02f, ey = 0.25f, rs = 0.08f;
      float th = u * 1.7f * PIF, rad = rs * (1.f - 0.55f * u);
      x = ex - rs + rad * cosf(th); y = ey + rad * sinf(th);
      r = 0.045f * (1.f - 0.6f * u) + 0.008f;
    }
    shX[i] = x; shY[i] = y; shR[i] = r;
  }
}
static void drawSeahorse(const Fish &f, float bright, float gm) {
  const Species &s = SP[f.sp];
  float ig = clampf((introT - f.ignite) / 0.35f, 0.f, 1.f);
  if (ig <= 0.f) return;
  int a = (int)(256.f * ig);
  float L = s.L, sg = (float)f.up, tilt = fsin(f.tail * 0.5f) * 0.12f, ct = fcos(tilt), st = fsin(tilt);
  RGB8 hc = hueCol(f.hue, 96);
  RGB8 body = shade(RGB8{ (uint8_t)((hc.r + s.body.r) / 2), (uint8_t)((hc.g + s.body.g) / 2), (uint8_t)((hc.b + s.body.b) / 2) }, bright);
  RGB8 dark = shade(body, 0.6f), fin = shade(s.fin, bright);
  #define SHP(lx, ly, X, Y) { float _x = (lx) * L, _y = (ly) * L; X = f.x + sg * (_x * ct - _y * st); Y = f.y + (_x * st + _y * ct); }
  blit(f.x, f.y, L * 0.45f, s.glowA * gm * ig, glowCol[f.sp]);
  // dorsal fin, fluttering fast
  float fx0, fy0, fx1, fy1, fx2, fy2;
  SHP(-0.10f, -0.10f, fx0, fy0); SHP(-0.10f, 0.10f, fx1, fy1); SHP(-0.24f + 0.03f * fsin(f.tail * 4.f), 0.0f, fx2, fy2);
  tri(fx0, fy0, fx1, fy1, fx2, fy2, fin, (a * 120) >> 8);
  // snout and coronet
  float sx0, sy0, sx1, sy1, sx2, sy2;
  SHP(0.03f, -0.44f, sx0, sy0); SHP(0.34f, -0.405f, sx1, sy1); SHP(0.03f, -0.37f, sx2, sy2);
  tri(sx0, sy0, sx1, sy1, sx2, sy2, body, a);
  SHP(-0.04f, -0.53f, sx0, sy0); SHP(-0.01f, -0.62f, sx1, sy1); SHP(0.04f, -0.52f, sx2, sy2);
  tri(sx0, sy0, sx1, sy1, sx2, sy2, body, a);
  // body and tail
  for (int i = 0; i < 20; i++) {
    float X, Y; SHP(shX[i], shY[i], X, Y);
    discQ((int)(X * 16.f), (int)(Y * 16.f), (int)(shR[i] * L * 16.f), body, a);
  }
  // ridged rings on the back
  for (int i = 3; i < 16; i += 2) {
    float X, Y; SHP(shX[i] - shR[i] * 0.6f, shY[i], X, Y);
    discQ((int)(X * 16.f), (int)(Y * 16.f), (int)(shR[i] * L * 7.f), dark, (a * 150) >> 8);
  }
  float ex, ey; SHP(0.04f, -0.46f, ex, ey);
  blendPx((int)(ex + 0.5f), (int)(ey + 0.5f), RGB8{ 12, 12, 16 }, a > 255 ? 255 : a);
  #undef SHP
}

// ---- ribbon eel: a long rainbow ribbon that swims in travelling waves ----
static void drawRibbon(const Fish &f, float bright) {
  const Species &s = SP[f.sp];
  float ig = clampf((introT - f.ignite) / 0.35f, 0.f, 1.f);
  if (ig <= 0.f) return;
  int a = (int)(256.f * ig);
  SpineQ sq; spineQ(f, sq);
  uint16_t ph = (uint16_t)(f.tail * RAD2BAM);
  int n = (int)(s.L * 0.8f), R16 = (int)(s.depth * s.L * 16.f), sh = (int)(tms / 25);
  int eyex = 0, eyey = 0;
  for (int i = n; i >= 0; i--) {
    int t10 = (i << 10) / n, x, y, qx, qy;
    spineAtQ(sq, t10, x, y, qx, qy);
    int off = ((40 * isin((uint16_t)(ph - t10 * 90))) >> 14) * t10 >> 10;     // lateral wave, growing toward the tail
    x += (qx * off) >> 8; y += (qy * off) >> 8;
    int r = (R16 * (1024 - (t10 * 650 >> 10))) >> 10;
    RGB8 c = hueCol(f.hue + (t10 * 300 >> 10) + sh, 96);
    discQ(x, y, r, shade(c, bright * 1.05f), a);
    addDot(x + ((qx * (r + 22)) >> 8), y + ((qy * (r + 22)) >> 8), c, (a * 70) >> 8);   // glowing fin edge
    if ((i & 3) == 0) addDot(x, y, c, (a * 110) >> 8);
    if (i == 1) { eyex = x + ((qx * r) >> 9); eyey = y + ((qy * r) >> 9); }
  }
  blendPx((eyex + 8) >> 4, (eyey + 8) >> 4, RGB8{ 10, 10, 14 }, a > 255 ? 255 : a);
}

// ---- orb drifter: a golden-angle rosette of lights that turns and breathes, with trailing tendrils ----
static void drawOrb(const Fish &f, float gm) {
  const Species &s = SP[f.sp];
  float ig = clampf((introT - f.ignite) / 0.35f, 0.f, 1.f);
  if (ig <= 0.f) return;
  int a = (int)(256.f * ig);
  uint16_t spin = (uint16_t)(f.tail * RAD2BAM), br = (uint16_t)(f.tail * 3.f * RAD2BAM);
  const uint16_t GOLD = 25033;   // 137.508 degrees in BAM
  int cxq = (int)(f.x * 16.f), cyq = (int)(f.y * 16.f), sh = (int)(tms / 30);
  blit(f.x, f.y, s.L * 0.55f, s.glowA * gm * ig, hueCol(f.hue + sh, 96));
  for (int k = 1; k <= 34; k++) {
    uint16_t th = (uint16_t)(k * GOLD + spin);
    int r = (int)(16.f * 1.25f * sqrtf((float)k));           // Q4
    r += (r * isin((uint16_t)(br - k * 1900)) >> 17);          // +-12% breathing
    addDot(cxq + ((r * icos(th)) >> 14), cyq + ((r * isin(th)) >> 14), hueCol(f.hue + k * 9 + sh, 96), (a * (230 - k * 4)) >> 8);
  }
  // tendrils
  for (int j = 0; j < 5; j++) {
    for (int m = 3; m < 20; m++) {
      int sw = isin((uint16_t)(br / 3 - m * 2800 + j * 13000));
      int xq = cxq + (j - 2) * 40 + ((sw * m * 12) >> 14), yq = cyq + (int)(s.L * 0.3f * 16.f) + m * 16;
      addDot(xq, yq, hueCol(f.hue + j * 50 + sh, 96), (a * (90 - m * 4)) >> 8);
    }
  }
}

// ---- comb jelly: a clear oval, eight comb rows with rainbow light running along them, a glowing
//      balance organ at the top end; sea gooseberries trail two fine tentacles with side filaments ----
static const int CR_N = 9;
static const int16_t CR_T[CR_N] = { -218, -170, -118, -64, -10, 44, 98, 150, 198 };   // position along the axis, Q8 of the half-length
static int16_t CR_W[CR_N];                                                         // half-width there, Q8
static void initComb() { for (int i = 0; i < CR_N; i++) { float t = CR_T[i] / 256.f; CR_W[i] = (int16_t)(sqrtf(1.f - t * t) * 0.9f * 256.f); } }
static void drawComb(const Fish &f, float gm) {
  const Species &s = SP[f.sp];
  float ig = clampf((introT - f.ignite) / 0.35f, 0.f, 1.f);
  if (ig <= 0.f) return;
  int a = (int)(256.f * ig);
  bool goose = s.mark == MK_GOOSE;
  uint16_t hb = bam(f.h);
  int ux = icos(hb), uy = isin(hb);                 // axis toward the mouth, Q14
  int px = -uy, py = ux;                            // across the body
  int A = (int)(s.L * 8.f), B = (int)(s.L * 8.f * s.depth);   // semi-axes, Q4
  int cxq = (int)(f.x * 16.f), cyq = (int)(f.y * 16.f);
  RGB8 body = s.body, lip = s.fin;
  blit(f.x, f.y, s.L * 0.75f, s.glowA * gm * ig, hueCol(f.hue + (int)(tms / 40), 60));
  // clear body: faint in the middle, brighter toward the edge
  int R = (A >> 4) + 2, x0 = (cxq >> 4) - R, y0 = (cyq >> 4) - R;
  for (int y = y0; y <= y0 + 2 * R; y++) {
    int dy = y * 16 + 8 - cyq;
    for (int x = x0; x <= x0 + 2 * R; x++) {
      int dx = x * 16 + 8 - cxq;
      int u = (dx * ux + dy * uy) >> 14, v = (dx * px + dy * py) >> 14;
      int Bu = goose ? B : (B * (256 + (u * 36) / A)) >> 8;     // beroe widens toward the mouth
      int e = ((u * u) << 8) / (A * A) + ((v * v) << 8) / (Bu * Bu);
      if (e > 256) continue;
      blendPx(x, y, body, (a * (22 + ((((e * e) >> 8) * 96) >> 8))) >> 8);
    }
  }
  // comb rows: eight meridians turning slowly with the body; the near ones sparkle
  uint16_t spin = (uint16_t)(f.aface * RAD2BAM), wph = (uint16_t)(f.tail * RAD2BAM);
  int sh = (int)(tms / 14);
  for (int r = 0; r < 8; r++) {
    uint16_t phi = (uint16_t)(r * 8192 + spin);
    int near = icos(phi), side = isin(phi);         // Q14
    int far = near < 0;
    for (int i = 0; i < CR_N; i++) {
      int u = (A * CR_T[i]) >> 8;
      int Bu = goose ? B : (B * (256 + (u * 36) / A)) >> 8;
      int v = (((Bu * CR_W[i]) >> 8) * side) >> 14;
      int xq = cxq + ((u * ux + v * px) >> 14), yq = cyq + ((u * uy + v * py) >> 14);
      int w = isin((uint16_t)(wph * 2 + i * 6500 + r * 2400));   // light running from the top end to the mouth
      if (far) { if (w > 8000) addDot(xq, yq, hueCol(f.hue + i * 30 + sh, 96), (a * ((w - 8000) >> 7)) >> 9); continue; }
      addPx((xq + 8) >> 4, (yq + 8) >> 4, 230, 240, 255, (a * 26) >> 8);              // the row itself, faint
      if (w > 0) addDot(xq, yq, hueCol(f.hue + i * 30 + r * 12 + sh, 96), (a * (w >> 6) * (8 + (near >> 11)) * 3) >> 13);
    }
  }
  // balance organ at the top end
  addDot(cxq - ((A * 15 / 16 * ux) >> 14), cyq - ((A * 15 / 16 * uy) >> 14), RGB8{ 255, 250, 235 }, (a * 170) >> 8);
  // lips at the mouth end
  if (!goose) for (int j = -2; j <= 2; j++) {
    int u = A - 10 - abs(j) * 4, v = j * B * 7 / 16;
    addDot(cxq + ((u * ux + v * px) >> 14), cyq + ((u * uy + v * py) >> 14), lip, (a * 90) >> 8);
  }
  // sea gooseberry tentacles: trail behind, curving out, with fine side filaments
  if (goose) {
    uint16_t ph = (uint16_t)(f.tail * 0.5f * RAD2BAM);
    int len = (int)(s.L * 2.6f);
    for (int sd = -1; sd <= 1; sd += 2) {
      int bxq = cxq - ((A * 5 / 16 * ux) >> 14) + sd * ((B * 3 / 4 * px) >> 14);
      int byq = cyq - ((A * 5 / 16 * uy) >> 14) + sd * ((B * 3 / 4 * py) >> 14);
      for (int m = 1; m < len; m++) {
        int sw = isin((uint16_t)(ph - m * 2200 + sd * 9000));
        int off = sd * (m * 5 + ((sw * m) >> 13));          // Q4 sideways
        int bk = m * 16;                                    // Q4 backwards
        int xq = bxq - ((bk * ux) >> 14) + ((off * px) >> 14), yq = byq - ((bk * uy) >> 14) + ((off * py) >> 14);
        int q = (a * (120 - 90 * m / len)) >> 8;
        addPx((xq + 8) >> 4, (yq + 8) >> 4, lip.r, lip.g, lip.b, q);
        if (m % 3 == 0 && m > 3) {                          // filament: a 2-px tick outward
          int fx = xq + sd * ((24 * px) >> 14) - ((10 * ux) >> 14), fy = yq + sd * ((24 * py) >> 14) - ((10 * uy) >> 14);
          addPx((fx + 8) >> 4, (fy + 8) >> 4, lip.r, lip.g, lip.b, q >> 1);
        }
      }
    }
  }
}

static void drawFeelers(const Fish &f) {
  float ig = clampf((introT - f.ignite) / 0.35f, 0.f, 1.f);
  if (ig <= 0.f) return;
  const Species &s = SP[f.sp];
  float bx = f.hc, by = f.hs, L = s.L, R = 0.5f * s.depth * L;
  float px, py, qx, qy;
  spineAt(f, 0.3f, px, py, qx, qy);
  int q = (int)(0.4f * ig * 256.f);
  RGB8 tipc = hueCol(36, 96);
  for (int k = -1; k <= 1; k += 2) {
    float sway = (float)isin((uint16_t)(tph(KT_FEEL) + (k + f.side) * 10430)) * (1.5f / 16384.f);
    float x0 = px - qx * R * 0.8f, y0 = py - qy * R * 0.8f;              // from the belly
    float xc = x0 - bx * L * 0.2f - qx * L * 0.25f, yc = y0 - by * L * 0.2f - qy * L * 0.25f;
    float ex = x0 - bx * L * 0.55f - qx * L * 0.45f + k * 1.2f + sway, ey = y0 - by * L * 0.55f - qy * L * 0.45f;
    for (int i = 1; i <= 12; i++) {
      float t = i / 12.f, u = 1.f - t;
      float x = u * u * x0 + 2 * u * t * xc + t * t * ex, y = u * u * y0 + 2 * u * t * yc + t * t * ey;
      addPx((int)(x + 0.5f), (int)(y + 0.5f), 255, 200, 120, q);
    }
    blit(ex, ey, 1.3f * GLOW, 0.5f * ig, tipc);
  }
}

static const int AN_N0 = 64, AN_N1 = 44, AN_T = AN_N0 + AN_N1;
// golden-angle rosettes, precomputed in integers: per point r0 (Q4), cos/sin*0.42 (Q14), kernel, base alpha
static int16_t anR0q[AN_T], anCq[AN_T], anSq[AN_T]; static uint8_t anK[AN_T], anA[AN_T];
static int AX16[2], AY16[2];
static const int AN[2] = { AN_N0, AN_N1 };
static int anRmaxq[2];
static void initAnemones() {
  AX16[0] = layoutXi(30)*16; AX16[1] = layoutXi(132)*16;
  AY16[0] = layoutYi(283)*16; AY16[1] = layoutYi(286)*16;
  const float GOLDEN = PIF * (3.f - sqrtf(5.f));
  static const float AC[2] = { 1.55f, 1.45f };
  for (int a = 0, base = 0; a < 2; base += AN[a], a++) {
    anRmaxq[a] = (int)(AC[a] * sqrtf((float)AN[a]) * 16.f);
    for (int k = 1; k <= AN[a]; k++) {
      int i = base + k - 1; float f = 1.f - (float)k / AN[a];
      anR0q[i] = (int16_t)(AC[a] * sqrtf((float)k) * 16.f);
      anCq[i] = (int16_t)(cosf(k * GOLDEN) * 16384.f); anSq[i] = (int16_t)(sinf(k * GOLDEN) * 0.42f * 16384.f);
      anK[i] = (uint8_t)kernFor((1.f + f) * GLOW); anA[i] = (uint8_t)((0.28f + 0.42f * f) * 255.f);
    }
  }
}
static void drawAnemones(float boost) {
  static const int AHUE[2] = { 330, 24 };
  int bq = (int)(boost * 256.f);
  uint16_t tb = tph(KT_ANEM);
  const uint16_t kstep = bam(0.09f);
  for (int a = 0, base = 0; a < 2; base += AN[a], a++) {
    RGB8 c = hueCol(AHUE[a], 96);
    for (int k = 1; k <= AN[a]; k++) {
      int i = base + k - 1;
      int r = anR0q[i] + ((anR0q[i] * 1147 / 16384) * isin((uint16_t)(tb - k * kstep)) >> 14);   // 1 + 0.07 sin
      int x = AX16[a] + ((r * anCq[i]) >> 14), y = AY16[a] + ((r * anSq[i]) >> 14) - (((anRmaxq[a] - r) * 97) >> 8);
      addDot(x, y, c, (anA[i] * bq * 3) >> 9);
    }
  }
}

static SpineQ frameSpine[MAXFISH];
static void renderGlow() {
  blitCount = 0;
  if (theme == ABYSS) { renderAbyss(); return; }
  if (theme == POKEMON) { renderPokemon(); return; }
#if GT_BLOOM
  memset(glowMask, 0, sizeof(glowMask));
#endif
  float night = nightF, fishFade = 1.f;
  float gm = 1.f + night * 0.45f;
  // things that barely move
  float budBoost = (0.5f + 0.5f * lightsF) * (1.f + night * 1.4f) * fishFade;
  for (int i = 0; i < 6; i++) {
    RGB8 c = hueCol(plants[i].hue + 24, 96);
    float ba = (plants[i].front ? 0.16f : 0.26f) * budBoost;
    int bq = (int)(ba * 1.6f * 256.f), bq2 = bq / 3;
    for (int t = 0; t < plantTipCount[i]; t++) {
      int j = plantTipStart[i] + t, xq = tipBuf[2 * j], yq = tipBuf[2 * j + 1];
      addDot(xq, yq, c, bq);
      addDot(xq - 16, yq, c, bq2); addDot(xq + 16, yq, c, bq2); addDot(xq, yq - 16, c, bq2); addDot(xq, yq + 16, c, bq2);
    }
  }
  drawAnemones((0.55f + 0.45f * lightsF) * (1.f + night * 0.6f));
  for (int i = 0; i < MAXFOOD; i++) {
    const Food &fd = food[i];
    if (!fd.on) continue;
    if (fd.kind == 0 && fd.state == 2) blit(fd.x, fd.y, fd.r * 2.6f * GLOW, 0.5f * fd.life, hueCol((int)fd.hue, 96));
    if (fd.kind == 1) blit(fd.x, fd.y, fd.r * 2.f * GLOW, 0.45f, hueCol(92, 60));
  }
  // fish: solid bodies (lit by the room, never fully dark), then their glow on top
  float bright = 0.55f + 0.45f * ambF;
  for (int i = 0; i < nFish; i++) {
    const Species &sp = SP[fish[i].sp];
    if (sp.style == DS_FISH) drawFishBody(fish[i], bright, frameSpine[i]);
    else if (sp.style == DS_SEAHORSE) drawSeahorse(fish[i], bright, gm);
    else if (sp.style == DS_RIBBON) drawRibbon(fish[i], bright);
  }
  for (int i = 0; i < nFish; i++) {
    const Species &sp = SP[fish[i].sp];
    if (sp.style == DS_FISH) drawFishGlow(fish[i], gm, sp.beh != B_CLING, frameSpine[i]);
    else if (sp.style == DS_JELLY) drawJelly(fish[i], gm);
    else if (sp.style == DS_ORB) drawOrb(fish[i], gm);
    else if (sp.style == DS_COMB) drawComb(fish[i], gm);
    if (sp.mark == MK_HONEY) drawFeelers(fish[i]);
  }
  for (int i = 0; i < MAXFOOD; i++) {
    const Food &fd = food[i];
    if (!fd.on || fd.kind != 0 || fd.state == 2) continue;
    RGB8 c = hueCol((int)fd.hue, 96);
    blit(fd.x, fd.y, fd.r * 2.4f * GLOW, 0.85f, c);
    blit(fd.x, fd.y, fd.r * 0.7f * GLOW, 0.95f, c);
  }
  for (int i = 0; i < 64; i++) if (sparks[i].on) blit(sparks[i].x, sparks[i].y, sparks[i].r * 2.2f * GLOW, sparks[i].life * 0.7f, sparks[i].c);
}

#if GT_BLOOM
// bloom: 4x4 downsample above a threshold, [1 2 1] blur twice, bilinear add back
static uint16_t blm[3][BLOOM_CELLS], blt[BLOOM_CELLS];
static uint8_t blmNZ[BLOOM_CELLS];
// blur restricted to [x0,x1]x[y0,y1]; reads neighbours outside it exactly as a full pass would
static void blurPass(uint16_t *c, int x0, int x1, int y0, int y1) {
  int hy0 = y0 > 0 ? y0 - 1 : 0, hy1 = y1 < BH - 1 ? y1 + 1 : BH - 1;
  for (int y = hy0; y <= hy1; y++) {
    const uint16_t *row = c + y * BW; uint16_t *out = blt + y * BW;
    for (int x = x0; x <= x1; x++) {
      int l = row[x > 0 ? x - 1 : x], m = row[x], r = row[x < BW - 1 ? x + 1 : x];
      out[x] = (uint16_t)((l + 2 * m + r) >> 2);
    }
  }
  for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) {
    int u = blt[(y > 0 ? y - 1 : y) * BW + x], m = blt[y * BW + x], d = blt[(y < BH - 1 ? y + 1 : y) * BW + x];
    c[y * BW + x] = (uint16_t)((u + 2 * m + d) >> 2);
  }
}
#endif
static void renderBloom() {
  if (theme == ABYSS || theme == POKEMON) return;
#if GT_BLOOM
  const int T = 16 * 60;   // per-channel threshold on a 16-pixel sum (8-bit units)
  int bx0 = BW, bx1 = -1, by0 = BH, by1 = -1;
  for (int by = 0; by < BH; by++) {
    uint16_t *o0 = blm[0] + by * BW, *o1 = blm[1] + by * BW, *o2 = blm[2] + by * BW;
    for (int bx = 0; bx < BW; bx++) {
      int sr = 0, sg = 0, sb = 0;
      if (bx < W / 4 && glowMask[by * BW + bx]) {   // only glow blooms, as in the mockup
        // SWAR: spread each RGB565 pixel into three fields of one 32-bit word, add 16 at once
        uint32_t acc = 0;
        for (int j = 0; j < 4; j++) {
          const uint16_t *row = fb + (by * 4 + j) * W + bx * 4;
          for (int i = 0; i < 4; i++) { uint32_t p = row[i]; acc += ((p & 0xF800u) << 10) | ((p & 0x07E0u) << 5) | (p & 0x1Fu); }
        }
        sr = (int)((acc >> 21) & 0x3FF) << 3; sg = (int)((acc >> 10) & 0x7FF) << 2; sb = (int)(acc & 0x3FF) << 3;
      }
      sr -= T; sg -= T; sb -= T;
      o0[bx] = (uint16_t)(sr > 0 ? sr : 0); o1[bx] = (uint16_t)(sg > 0 ? sg : 0); o2[bx] = (uint16_t)(sb > 0 ? sb : 0);
      if (sr > 0 || sg > 0 || sb > 0) {
        if (bx < bx0) bx0 = bx; if (bx > bx1) bx1 = bx;
        if (by < by0) by0 = by; if (by > by1) by1 = by;
      }
    }
  }
  if (bx1 < 0) return;
  // two blur passes spread light at most 2 cells; everything outside stays zero
  int rx0 = clampi(bx0 - 2, 0, BW - 1), rx1 = clampi(bx1 + 2, 0, BW - 1), ry0 = clampi(by0 - 2, 0, BH - 1), ry1 = clampi(by1 + 2, 0, BH - 1);
  for (int c = 0; c < 3; c++) { blurPass(blm[c], rx0, rx1, ry0, ry1); blurPass(blm[c], rx0, rx1, ry0, ry1); }
  for (int y = 0; y < BH; y++) for (int x = 0; x < BW; x++) {
    int o = y * BW + x;
    blmNZ[o] = (x >= rx0 && x <= rx1 && y >= ry0 && y <= ry1) ? ((blm[0][o] | blm[1][o] | blm[2][o]) > 12) : 0;
  }
  int strength = (int)((0.9f + 0.35f * nightF) * 256.f);   // Q8
  int ux1 = rx1 + 1 < W / 4 ? rx1 + 1 : W / 4 - 1, uy1 = ry1 + 1 < BH ? ry1 + 1 : BH - 1;
  for (int by = (ry0 > 0 ? ry0 - 1 : 0); by <= uy1; by++) for (int bx = (rx0 > 0 ? rx0 - 1 : 0); bx <= ux1; bx++) {
    bool lit = false;
    for (int j = -1; j <= 1 && !lit; j++) for (int i = -1; i <= 1; i++) {
      int xx = bx + i, yy = by + j;
      if (xx >= 0 && xx < BW && yy >= 0 && yy < BH && blmNZ[yy * BW + xx]) { lit = true; break; }
    }
    if (!lit) continue;
    for (int j = 0; j < 4; j++) {
      int y = by * 4 + j, gy = y * 2 - 3, y0 = gy >> 3, fy = gy & 7;
      int ya = clampi(y0, 0, BH - 1), yb = clampi(y0 + 1, 0, BH - 1);
      for (int i = 0; i < 4; i++) {
        int x = bx * 4 + i, gx = x * 2 - 3, x0 = gx >> 3, fx = gx & 7;
        int xa = clampi(x0, 0, BW - 1), xb2 = clampi(x0 + 1, 0, BW - 1);
        int v[3];
        for (int c = 0; c < 3; c++) {
          const uint16_t *m = blm[c];
          int top = m[ya * BW + xa] * (8 - fx) + m[ya * BW + xb2] * fx;
          int bot = m[yb * BW + xa] * (8 - fx) + m[yb * BW + xb2] * fx;
          v[c] = (top * (8 - fy) + bot * fy) >> 6;
        }
        if ((v[0] | v[1] | v[2]) == 0) continue;
        addPx(x, y, clampi(v[0] >> 4, 0, 255), clampi(v[1] >> 4, 0, 255), clampi(v[2] >> 4, 0, 255), strength);
      }
    }
  }
#endif
}

// tiny 5x7 font for the light-mode label
static const char FONT_CH[] = "ACDEGHIKLNOSTUWY-FPMBR0123456789.J";
static const uint8_t FONT[][7] = {
  { 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 }, { 0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E }, { 0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E },
  { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F }, { 0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F }, { 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 },
  { 0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E }, { 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 }, { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F },
  { 0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11 }, { 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E }, { 0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E },
  { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 }, { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E }, { 0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A },
  { 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04 }, { 0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00 },
  { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10 }, { 0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10 }, { 0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11 },
  { 0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E }, { 0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11 },
  { 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E }, { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E }, { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F },
  { 0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E }, { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 }, { 0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E },
  { 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E }, { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 }, { 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E },
  { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C }, { 0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C },
  { 0x07, 0x02, 0x02, 0x02, 0x12, 0x12, 0x0C },  // J (COMB JELLIES)
};
static_assert(sizeof(FONT_CH) - 1 == sizeof(FONT) / sizeof(FONT[0]), "Font characters must match glyphs");
static void drawText(int x, int y, const char *t, int q) {
  for (; *t; t++, x += 6) {
    const char *p = strchr(FONT_CH, *t);
    if (!p || *t == ' ') continue;
    const uint8_t *g = FONT[p - FONT_CH];
    for (int r = 0; r < 7; r++) for (int b = 0; b < 5; b++) if (g[r] & (0x10 >> b)) addPx(x + b, y + r, 210, 245, 255, q);
  }
}
// optional on-screen stats (the sketch fills these in); drawn over a dimmed strip
static char overlay[3][32];
static void setOverlay(int line, const char *t) { if (line >= 0 && line < 3) { strncpy(overlay[line], t ? t : "", 31); overlay[line][31] = 0; } }
static void drawOverlay() {
  for (int l = 0; l < 3; l++) {
    if (!overlay[l][0]) continue;
    int y = 14 + l * 9, w = (int)strlen(overlay[l]) * 6 + 3;
    for (int yy = y - 1; yy < y + 8; yy++) for (int xx = 1; xx < 1 + w && xx < W; xx++) blendPx(xx, yy, RGB8{ 0, 0, 0 }, 150);
    drawText(3, y, overlay[l], 256);
  }
}
static void drawLabel() {
  if (!labelText || labelT <= 0.f) return;
  float a = clampf(labelT / 0.4f, 0.f, 1.f) * clampf((2.2f - labelT) / 0.25f, 0.f, 1.f);
  int q = (int)(a * 200.f);
  int n = (int)strlen(labelText), x = (W - n * 6 + 1) / 2, y = 24;
  for (int c = 0; c < n; c++, x += 6) {
    const char *p = strchr(FONT_CH, labelText[c]);
    if (!p || labelText[c] == 0) continue;
    const uint8_t *g = FONT[p - FONT_CH];
    for (int r = 0; r < 7; r++) for (int b = 0; b < 5; b++) if (g[r] & (0x10 >> b)) addPx(x + b, y + r, 200, 240, 255, q);
  }
}

static void renderFront() {
  if (theme == POKEMON) { pokemonFront(); drawLabel(); drawOverlay(); return; }
  if (theme == ABYSS) { drawLabel(); drawOverlay(); return; }
  float fishFade = 1.f;
#if GT_SNOW
  float snowA = (0.35f + 0.65f * lightsF) * fishFade;
  for (int m = 0; m < 110; m++) {
    const Mote &mo = motes[m];
    int y = (int)mo.y, x16 = (int)(mo.x * 16.f);
    if ((unsigned)y >= (unsigned)H) continue;
    float lit = 0.f;
    for (int b = 0; b < 3; b++) {
      if (!rbS[y][b]) continue;
      int dx = abs(x16 - rbCx[y][b]), hw = rbHw[y][b];
      if (dx < hw) lit += rbS[y][b] * (1.f / 4096.f) * (1.f - (float)dx / hw);
    }
    lit *= 9.f;
    float a = ((0.05f + 0.13f * mo.z) * (0.5f + 0.5f * ambF) + lit * mo.z) * snowA;
    int q = (int)(a * 256.f); if (q > 230) q = 230;
    if (q < 3) continue;
    addPx((int)mo.x, y, 205, 236, 255, q);
    if (mo.z > 0.8f) addPx((int)mo.x + 1, y, 205, 236, 255, q >> 1);
  }
#endif
  // bubbles: small outlined circles
  int bq = (int)(0.3f * (0.4f + 0.6f * lightsF) * 256.f);
  for (int b = 0; b < 48; b++) {
    const Bubble &bu = bubbles[b];
    if (!bu.on) continue;
    int nPts = bu.r < 1.2f ? 6 : 10;
    for (int i = 0; i < nPts; i++) {
      uint16_t a = (uint16_t)(i * 65536 / nPts);
      addPx((int)(bu.x + bu.r * icos(a) * (1.f / 16384.f) + 0.5f), (int)(bu.y + bu.r * isin(a) * (1.f / 16384.f) + 0.5f), 190, 235, 255, bq);
    }
  }
  renderPlants(true);
  // surface line + ripples
  int sq = (int)((0.25f + 0.75f * lightsF) * 0.32f * 256.f);
  uint16_t p1 = tph(KT_S1), p2 = tph(KT_S2);
  for (int x = 0; x < W; x++) {
    float y = SURFACE + isin((uint16_t)(x * 1537 + p1)) * (1.2f / 16384.f) + isin((uint16_t)(x * 3582 - p2)) * (0.6f / 16384.f);
    int iy = ifloor(y); int f = (int)((y - iy) * 256.f);
    addPx(x, iy, 140, 220, 255, (sq * (256 - f)) >> 8);
    addPx(x, iy + 1, 140, 220, 255, (sq * f) >> 8);
  }
  for (int r = 0; r < 12; r++) {
    const Ring &rg = rings[r];
    if (!rg.on) continue;
    int q = (int)(rg.life * 0.55f * (0.25f + 0.75f * lightsF) * 256.f);
    int nPts = 8 + (int)(rg.r * 3.f); if (nPts > 64) nPts = 64;
    for (int i = 0; i < nPts; i++) {
      uint16_t a = (uint16_t)(i * 65536 / nPts);
      addPx((int)(rg.x + rg.r * icos(a) * (1.f / 16384.f) + 0.5f), (int)(SURFACE + 1.5f + rg.r * 0.2f * isin(a) * (1.f / 16384.f) + 0.5f), 180, 235, 255, q);
    }
  }
  drawLabel();
  drawOverlay();
}

// ======================================================================
// setup
// ======================================================================
static void init(uint32_t seed, bool horizontal = false) {
  configureLayout(horizontal);
  rs = seed ? seed : 0x2545F491u;
  for (int i = 0; i <= 1024; i++) SIN14[i] = (int16_t)lrintf(16384.f * sinf(i * TAUF / 1024.f));
  memset(hueHave, 0, sizeof(hueHave));
  for (int sat = 0; sat <= 100; sat++) {
    int sb = 0, bd = 999;
    for (int i = 0; i < 8; i++) { int d = abs(SATB[i] - sat); if (d < bd) { bd = d; sb = i; } }
    satBucket[sat] = (uint8_t)sb;
  }
  if (!bgS) bgS = (uint16_t *)malloc(sizeof(uint16_t) * FRAME_PIXELS);   // optional: falls back to per-frame drawing
  buildKernels();
  buildProfile();
  initSeahorse(); initComb();
  initTerrain();
  initCaustics();
  initAnemones();
  buildPlants();
  for (int s = 0; s < NSP; s++) {
    glowCol[s] = hueCol(SP[s].glowHue, SP[s].glowSat);
    for (int o = 0; o < NSP; o++) {
      float r = 0.4f * ((SP[s].L < 18.f ? SP[s].L : 18.f) + (SP[o].L < 18.f ? SP[o].L : 18.f));
      sepTab[s][o] = r * r; sepQ[s][o] = (int16_t)((r + 1.f) * 16.f);
    }
  }
  restart();
  step(1);
}

#include "tank_orientation.h"

static void reseedPlants() { plantSeed++; buildPlants(); }

// Swap fb into the panel's big-endian byte order, two pixels per word. fb is rebuilt
// from scratch every frame, so the sketch can send it as raw bytes in one call.
static void swapForPanel(bool bytes, bool swapRB) {
  for (int i = 0; i < W * H / 2; i++) {
    uint32_t v; memcpy(&v, fb + 2 * i, sizeof(v));
    if (swapRB) v = ((v & 0x001F001Fu) << 11) | ((v >> 11) & 0x001F001Fu) | (v & 0x07E007E0u);   // panel wants BGR
    if (bytes) v = ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);
    memcpy(fb + 2 * i, &v, sizeof(v));
  }
}

}  // namespace gt
