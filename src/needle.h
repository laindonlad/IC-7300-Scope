#pragma once
// How a moving-coil meter moves, kept apart from the drawing so it can be tested. The position is a share of
// the scale: 0 is the rest at the left and 1 the end of the printed scale, with a little travel past that
// before the needle hits the stop.
#include <math.h>

namespace needle {
const float kStop = 1.03f;     // where the end stop is
const float kPoFull = 0.85f;   // where 100% power is on the scale, leaving room above it as a real Po scale does

inline float clampShare(float f) { return f < 0 ? 0 : f > 1 ? 1 : f; }
// The bar meter's reading of the raw S value, so that the two agree: S9 is at 120 of 241.
inline float signalShare(int raw) { return raw < 0 ? 0 : clampShare((float)raw / 241.f); }
inline float powerShare(double watts, double fullWatts) {
  return fullWatts > 0 && watts > 0 ? clampShare((float)(kPoFull * watts / fullWatts)) : 0;
}

struct Needle {
  float pos = 0, vel = 0;

  // A spring and a damper, slightly under-damped so it swings a touch past a new reading and settles. It's
  // quicker going up than coming down, the way a receiver's S meter charges fast and leaks away slowly. Fixed
  // small steps keep it the same at any frame rate, and stable through a stall.
  void update(float target, float dt) {
    target = clampShare(target);
    if (dt > 0.1f) dt = 0.1f;
    const float kStep = 1 / 240.f, kDamping = 0.7f;
    for (float left = dt; left > 0; left -= kStep) {
      float h = left < kStep ? left : kStep;
      float w = target > pos ? 20.f : 9.f;
      vel += (w * w * (target - pos) - 2 * kDamping * w * vel) * h;
      pos += vel * h;
      if (pos < 0) pos = 0, vel = -vel * 0.25f;
      if (pos > kStop) pos = kStop, vel = -vel * 0.25f;
    }
  }
};
}  // namespace needle
