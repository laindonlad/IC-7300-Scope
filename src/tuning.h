#pragma once
// What a typed frequency means, how the bands are laid out, and where an audio tone is on the air.
// Plain functions, so the tests can pin them down.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include <string>

#include "civ.h"

namespace tuning {
// "14.074" 14.074MHz; "14074" 14074kHz; "14074.5" 14074.5kHz; "7" 7MHz; "198" 198kHz; "14074000" Hz.
// A k or m suffix says so outright ("3573k", "14.074m"). "+5" and "-1.5" are kHz from the dial.
// Without a suffix it depends on where the radio stops. On a 7300 (74.8 MHz), 75 or under is MHz, 76 up to
// 75000 is kHz, and above that Hz. On a 9700 (1300 MHz), "145.5" and "1296" are MHz and "145500" is kHz, and
// the same on an R8600 (3 GHz), where 198 is 198 MHz and long wave wants "198k".
// Returns the frequency, or -1 if it isn't one the rig can tune.
inline long long parseFreq(const std::string& raw, long long dialHz, const civ::Radio& radio = civ::kRadio7300) {
  std::string s;
  for (char c : raw)
    if (c != ' ' && c != ',' && c != '_') s += c;
  if (s.empty()) return -1;
  bool relative = s[0] == '+' || s[0] == '-';
  char unit = 0;
  char last = s.back();
  if (last == 'k' || last == 'K') unit = 'k', s.pop_back();
  else if (last == 'm' || last == 'M') unit = 'm', s.pop_back();
  else if (last == 'h' || last == 'H') unit = 'h', s.pop_back();
  if (s.empty()) return -1;
  // A dotted form like 14.074.000 reads as written on the rig's display.
  int dots = 0;
  for (char c : s) dots += c == '.';
  if (dots >= 2) {
    std::string joined;
    for (char c : s)
      if (c != '.') joined += c;
    s = joined;
    unit = 'h';
  }
  char* end = nullptr;
  double v = strtod(s.c_str(), &end);
  if (end == s.c_str() || *end) return -1;
  long long hz;
  if (relative) {
    double scale = unit == 'm' ? 1e6 : unit == 'h' ? 1.0 : 1e3;
    hz = dialHz + llround(v * scale);
  } else if (unit == 'm') {
    hz = llround(v * 1e6);
  } else if (unit == 'k') {
    hz = llround(v * 1e3);
  } else if (unit == 'h') {
    hz = llround(v);
  } else if (v <= ceil((double)radio.topHz() / 1e6)) {
    hz = llround(v * 1e6);
  } else if (v <= ceil((double)radio.topHz() / 1e6) * 1000) {
    hz = llround(v * 1e3);
  } else {
    hz = llround(v);
  }
  return radio.canTune(hz) ? hz : -1;
}

// "14.074.000": grouped the way the rig's display does it.
inline std::string formatHz(long long hz) {
  if (hz < 0) return "--.---.---";
  char b[32];
  snprintf(b, sizeof b, "%lld.%03lld.%03lld", hz / 1000000, hz / 1000 % 1000, hz % 1000);
  return b;
}

// Stops at the edge of the range it's in, so a spun dial can't run off the end of 2 m and into nothing.
inline long long stepFreq(long long hz, long long step, int clicks, const civ::Radio& radio = civ::kRadio7300) {
  // Landing on a whole step first, as a rig with tuning steps does: 14.074.123 up 1kHz goes to 14.075.000.
  long long snapped = clicks > 0 ? hz - hz % step + step * clicks
                                 : (hz % step ? hz - hz % step + step * (clicks + 1) : hz + step * clicks);
  const civ::HzRange& r = radio.nearest(hz);
  return snapped < r.lo ? r.lo : snapped > r.hi ? r.hi : snapped;
}

inline bool isLowerSide(int mode) { return mode == civ::LSB || mode == civ::CW_R || mode == civ::RTTY; }

// The tone an RF frequency makes in the USB audio, and back. For CW the dial is where a station sounds
// at the pitch, and for RTTY where the mark tone (2125Hz) is. SSB's dial is the suppressed carrier.
// AM, FM, WFM and the digital modes don't map a tone to one RF frequency: returns -1.
inline double audioOffset(int mode, int cwPitchHz) {
  switch (mode) {
    case civ::CW:
    case civ::CW_R: return cwPitchHz;
    case civ::RTTY:
    case civ::RTTY_R: return 2125;
  }
  return 0;
}
inline long long rfFromAudio(long long dialHz, int mode, double audioHz, int cwPitchHz) {
  if (civ::isAmLike(mode) || mode == civ::FM || civ::hasNoFilters(mode) || mode < 0) return -1;
  double d = audioHz - audioOffset(mode, cwPitchHz);
  return dialHz + llround(isLowerSide(mode) ? -d : d);
}
// Where the dial must go for a signal now at audioHz to come out at targetAudioHz.
inline long long dialToCentre(long long dialHz, int mode, double audioHz, double targetAudioHz) {
  double d = audioHz - targetAudioHz;
  return dialHz + llround(isLowerSide(mode) ? -d : d);
}
// The rig's CW pitch knob: 0-255 runs 300-900Hz.
inline int cwPitchHz(int raw) { return raw < 0 ? 600 : 300 + (int)llround(raw * 600.0 / 255.0); }
// Key speed: 0-255 runs 6-48 WPM.
inline int keySpeedWpm(int raw) { return raw < 0 ? 0 : 6 + (int)llround(raw * 42.0 / 255.0); }
inline int keySpeedRaw(int wpm) { return (int)llround((wpm - 6) * 255.0 / 42.0); }

// The tuning dial, behaving as a rig's weighted VFO knob does. It turns where the pointer is, seen from the
// dial's centre, into clicks. Screen coordinates, so y runs down and clockwise is positive, which tunes up.
// It only follows how far the pointer has moved round, so grabbing it anywhere doesn't make it jump.
//
// Turned slowly, every click is the same size. Spun fast, the clicks are multiplied, as a rig's automatic
// tuning step does. Let go while it's spinning fast and it runs on for a moment, like a flywheel. A slow
// turn stops dead, so fine tuning never drifts.
class Dial {
 public:
  static constexpr int kClicksPerTurn = 100;
  static constexpr int kClicksPerStep = 10;  // so one turn is ten of the tuning step
  static constexpr double kClick = 2.0 * M_PI / kClicksPerTurn;
  static constexpr double kTurn = 2.0 * M_PI;

  // How far the dial moves for a click, given the tuning step.
  static long long clickHz(long long stepHz) { return stepHz >= kClicksPerStep ? stepHz / kClicksPerStep : 1; }

  // Call every frame while the dial is held. Near the centre the angle swings wildly for a tiny movement,
  // so inside deadRadius it lets go and picks up again from wherever the pointer comes back out.
  int spin(double dx, double dy, double deadRadius, double dt) {
    held_ = true;
    if (dx * dx + dy * dy < deadRadius * deadRadius) {
      grabbed_ = false;
      speed_ = 0;
      return 0;
    }
    double a = atan2(dy, dx);
    if (!grabbed_) {
      grabbed_ = true;
      last_ = a;
      speed_ = 0;  // catching it stops any coasting
      pending_ = 0;
      return 0;
    }
    double d = a - last_;
    last_ = a;
    if (d > M_PI) d -= kTurn;
    else if (d <= -M_PI) d += kTurn;
    if (dt > 0) speed_ += (d / dt - speed_) * (1.0 - exp(-dt / 0.08));  // smoothed, radians a second
    return advance(d);
  }

  // Call every frame while it isn't held. Returns what the flywheel turned it by.
  int idle(double dt) {
    if (held_) {
      held_ = grabbed_ = false;
      if (fabs(speed_) < 0.75 * kTurn) stop();
    }
    if (speed_ == 0 || dt <= 0) return 0;
    speed_ *= exp(-dt / 0.35);
    if (fabs(speed_) < 0.25 * kTurn) {
      stop();
      return 0;
    }
    return advance(speed_ * dt);
  }

  void stop() {
    speed_ = 0;
    pending_ = 0;
  }
  // Clicks from elsewhere, as the mouse wheel: the dial is drawn turning with them.
  void nudge(int clicks) { angle_ += clicks * kClick; }
  double angle() const { return angle_; }  // radians turned so far, for drawing

 private:
  int advance(double rad) {
    angle_ += rad;
    // Up to 1.5 turns a second is plain tuning, then it builds to 8 times at 4 turns a second.
    double t = (fabs(speed_) / kTurn - 1.5) / 2.5;
    double rate = t <= 0 ? 1.0 : t >= 1 ? 8.0 : 1.0 + 7.0 * t;
    pending_ += rad * rate;
    int clicks = (int)(pending_ / kClick);
    pending_ -= clicks * kClick;
    return clicks;
  }
  bool held_ = false, grabbed_ = false;
  double last_ = 0, pending_ = 0, angle_ = 0, speed_ = 0;
};

// The filter's passband in the audio, for shading the waterfall. Approximate: the rig's own edges move
// with Twin PBT and the SSB filter's low cut.
inline void audioPassband(int mode, int widthHz, int cwPitch, double& lo, double& hi) {
  if (mode == civ::CW || mode == civ::CW_R) {
    lo = cwPitch - widthHz / 2.0, hi = cwPitch + widthHz / 2.0;
  } else if (mode == civ::RTTY || mode == civ::RTTY_R) {
    lo = 2125 + 85 - widthHz / 2.0, hi = 2125 + 85 + widthHz / 2.0;
  } else {
    lo = 100, hi = 100.0 + widthHz;
  }
}
// The same on the air, for shading the wideband scope.
inline void rfPassband(long long dial, int mode, int widthHz, double& lo, double& hi) {
  if (mode == civ::LSB) lo = (double)dial - 100 - widthHz, hi = (double)dial - 100;
  else if (mode == civ::USB) lo = (double)dial + 100, hi = (double)dial + 100 + widthHz;
  else lo = (double)dial - widthHz / 2.0, hi = (double)dial + widthHz / 2.0;
}
}  // namespace tuning
