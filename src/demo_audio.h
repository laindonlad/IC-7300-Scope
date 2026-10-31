#pragma once
// What the simulated rig "hears", for --demo: noise plus the same stations its scope shows, each at the
// audio tone the dial and mode would put it on, so the two waterfalls agree and clicking one tunes the other.
#include <math.h>
#include <stdint.h>

#include <atomic>
#include <map>
#include <vector>

#include "civ.h"
#include "sim_rig.h"
#include "tuning.h"

class DemoAudio {
 public:
  std::atomic<long long> dialHz{14074000};
  std::atomic<int> mode{civ::USB};
  std::atomic<int> pitch{600};

  void generate(float* out, int n) {
    const float dt = 1.f / 48000.f;
    int m = mode;
    long long dial = dialHz;
    if (--refresh_ <= 0) {
      refresh_ = 5;
      sigs_ = simSignals(dial - 4000, dial + 4000);
    }
    for (int i = 0; i < n; i++) {
      seed_ = seed_ * 1664525u + 1013904223u;
      float w = (float)(int32_t)seed_ / 2147483648.f;
      lp_ += 0.35f * (w - lp_);
      out[i] = lp_ * 0.012f;
    }
    if (m == civ::AM || m == civ::FM) return;
    for (const SimSignal& s : sigs_) {
      double f = tuning::audioOffset(m, pitch) + (tuning::isLowerSide(m) ? (double)(dial - s.hz) : (double)(s.hz - dial));
      if (f < 100 || f > 3400) continue;
      float amp = (float)pow(10.0, (s.db - 62.0) / 20.0);
      Tone& t = tones_[s.hz];
      for (int i = 0; i < n; i++) {
        double sec = t_ + i * (double)dt;
        float gate = 1.f;
        double freq = f;
        uint32_t id = (uint32_t)s.hz;
        if (s.kind == 0) {
          gate = (hash(id + (uint32_t)(sec / 0.075)) & 2) ? 1.f : 0.f;  // keying
        } else if (s.kind == 2) {
          freq = f + (hash(id + (uint32_t)(sec / 0.16)) % 8) * 6.25;    // FT8-like tones
          gate = fmod(sec + id % 15, 15.0) < 12.6 ? 1.f : 0.f;
        } else {
          // Speech-like: a wandering tone, broken into syllables.
          freq = f * (0.3 + (hash(id + (uint32_t)(sec / 0.09)) % 100) / 80.0);
          gate = (hash(id + (uint32_t)(sec / 0.31)) & 7) > 1 ? 0.8f : 0.f;
          if (freq > 3000) freq = 3000;
        }
        t.env += 0.004f * (gate - t.env);
        t.phase += 2.0 * M_PI * freq * dt;
        out[i] += amp * t.env * (float)sin(t.phase);
      }
      t.phase = fmod(t.phase, 2.0 * M_PI);
    }
    t_ += n * (double)dt;
  }

 private:
  struct Tone {
    double phase = 0;
    float env = 0;
  };
  static uint32_t hash(uint32_t v) {
    v ^= v >> 16;
    v *= 0x7feb352dU;
    v ^= v >> 15;
    v *= 0x846ca68bU;
    v ^= v >> 16;
    return v;
  }
  std::vector<SimSignal> sigs_;
  std::map<long long, Tone> tones_;
  int refresh_ = 0;
  double t_ = 0;
  uint32_t seed_ = 12345;
  float lp_ = 0;
};
