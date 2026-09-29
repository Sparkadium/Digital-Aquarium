#pragma once
#include <stdint.h>
// One gesture per press. Mode selection waits for release, leaving a longer
// hold free to rotate without also switching tanks. Unsigned times handle wrap.
class BootGesture {
public:
  enum Event { NONE, FEED, WAFER, MODE_READY, NEXT_MODE, ROTATE };
  static constexpr uint32_t DEBOUNCE_MS = 30, WAFER_MS = 550, MODE_MS = 2000, ROTATE_MS = 5000;
  void begin(bool pressed, uint32_t now) {
    raw = stable = pressed; armed = !pressed; rotated = hinted = false;
    changedAt = downAt = now;
  }
  Event update(bool pressed, uint32_t now) {
    if (pressed != raw) { raw = pressed; changedAt = now; }
    if (raw != stable && (uint32_t)(now - changedAt) >= DEBOUNCE_MS) {
      stable = raw;
      if (stable) { downAt = changedAt; rotated = hinted = false; }
      else {
        if (!armed) { armed = true; return NONE; }
        if (rotated) return NONE;
        uint32_t held = changedAt - downAt;
        if (held >= ROTATE_MS) return ROTATE;
        if (held >= MODE_MS) return NEXT_MODE;
        if (held >= WAFER_MS) return WAFER;
        return held >= DEBOUNCE_MS ? FEED : NONE;
      }
    }
    if (armed && stable && raw && !rotated) {
      uint32_t held = now - downAt;
      if (held >= ROTATE_MS) { rotated = true; return ROTATE; }
      if (held >= MODE_MS && !hinted) { hinted = true; return MODE_READY; }
    }
    return NONE;
  }
private:
  bool raw = false, stable = false, armed = true, rotated = false, hinted = false;
  uint32_t changedAt = 0, downAt = 0;
};
