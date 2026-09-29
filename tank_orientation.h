#pragma once
// Included inside gt, after all tank implementations. Rotation is a between-frame
// operation: the caller must finish DMA before changing layout or panel registers.
static void setOrientation(bool horizontal) {
  if (landscape == horizontal) return;
  const float oldW = (float)W, oldH = (float)H, oldBottom = WATER_BOT;
  configureLayout(horizontal);
  const float sx = W / oldW, sy = (WATER_BOT - SURFACE) / (oldBottom - SURFACE);
  auto mapX = [sx](float x) { return x * sx; };
  auto mapY = [oldH, oldBottom, sy](float y) {
    if (y <= SURFACE) return y;
    return y <= oldBottom ? SURFACE + (y - SURFACE) * sy : H - (oldH - y);
  };
  initTerrain(); initCaustics(); initAnemones(); buildPlants(); updateRayGeometry();
  if (theme == POKEMON) {
    for (auto &f : pk().fish) {
      f.x = clampf(mapX(f.x), 16.f, W - 16.f); f.y = mapY(f.y);
      f.tx = clampf(mapX(f.tx), 16.f, W - 16.f); f.ty = mapY(f.ty);
      bool bottom = f.kind >= PK_WOOPER;
      const float lo = bottom ? layoutY(262.f) : f.kind == PK_TENTACOOL ? (landscape ? 22.f : 28.f) : 29.f;
      const float hi = bottom ? pkGround(f.x) - 10.f : f.kind == PK_TENTACOOL ? layoutY(103.f) : layoutY(269.f);
      f.y = clampf(f.y, lo - 1.f, hi + 2.f);
      pkSync(f);
    }
    pk().pixels = fb;
  } else if (theme == ABYSS) {
    for (auto &f : abyssFish) {
      f.x = clampf(mapX(f.x), 3.f, W - 3.f);
      f.y = clampf(mapY(f.y), SURFACE + 3.f, WATER_BOT - 2.f);
      syncAbyssFish(f);
    }
    for (auto &p : plankton) {
      p.x = (uint16_t)clampi((int)(p.x * sx), 0, W * 128 - 1);
      p.y = (uint16_t)clampi((int)(mapY(p.y / 128.f) * 128.f), 13 * 128, (int)(WATER_BOT - 2.f) * 128);
    }
  } else {
    for (int i = 0; i < nFish; i++) {
      Fish &f = fish[i]; float x0 = f.x, y0 = f.y;
      f.x = clampf(mapX(f.x), 3.f, W - 3.f);
      f.y = clampf(mapY(f.y), SURFACE + 2.f, floorAt(f.x) - 2.f);
      f.ax = mapX(f.ax); f.ay = mapY(f.ay);
      // Translate the flexible spine and trails intact, without stretching them.
      const float dx = f.x - x0, dy = f.y - y0;
      for (int k = 0; k < SPINE; k++) { f.sx[k] += dx; f.sy[k] += dy; }
      for (int k = 0; k < 8; k++) { f.hx[k] += dx; f.hy[k] += dy; }
      syncFish(f);
    }
  }
  for (auto &q : food) if (q.on) {
    q.x = clampf(mapX(q.x), 3.f, W - 3.f); q.y = mapY(q.y);
    const float ground = theme == POKEMON ? pkGround(q.x) - 2.f : floorAt(q.x) - (q.kind == 1 ? 1.5f : 1.f);
    q.y = q.state == 2 ? ground : clampf(q.y, SURFACE, ground);
    syncFood(q);
  }
  for (auto &b : bubbles) if (b.on) { b.x = mapX(b.x); b.y = mapY(b.y); }
  for (auto &s : sparks) if (s.on) { s.x = mapX(s.x); s.y = mapY(s.y); }
  for (auto &m : motes) { m.x = mapX(m.x); m.y = mapY(m.y); }
  for (auto &r : rings) if (r.on) r.x = mapX(r.x);
  bakeM = bakeNight = -1; bakeCursor = 0;
  basePrepared = causReady = false;
}
