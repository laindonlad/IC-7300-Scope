#pragma once
// PSK31, PSK63 and PSK125 (BPSK with Varicode), from complex baseband at 2kHz centred on the
// signal (within the AFC's reach).
//
// 1. The transmitted pulse is a raised cosine two symbols long, so the matched filter is a Hann
//    window that long. It keeps 16 samples a symbol.
// 2. Symbol timing: the filter's output power peaks mid-symbol and dips at phase reversals. Its
//    average at each of the 16 sample phases says where the middle is, and each symbol is taken
//    one symbol after the last, nudged towards it.
// 3. Each symbol's phase against the last: no change is a 1, a reversal a 0. The phase step
//    doubled (which takes the data off) is what the mixer is off by: AFC.
// 4. Varicode: a character is its code, then 00. Idle is a string of 0s.
// Characters are kept only while the steps have lately been close to 0 or 180 degrees (the
// squelch). quality() is how close to 0 or 180 degrees the steps fall (2/pi for noise), and valid() the
// share of bits that make known characters (or idle). A PSK63 signal read as PSK31, or the other
// way round, gives runs that never reach 00, so valid() tells the speeds apart.
#include "dsp.h"

class PskDemod {
 public:
  static constexpr float RATE = 2000;  // baseband in
  static constexpr int SPS = 16;       // samples a symbol after the matched filter

  // G3PLX's Varicode for ASCII 0-127.
  static constexpr const char* VARICODE[128] = {
    "1010101011", "1011011011", "1011101101", "1101110111", "1011101011", "1101011111", "1011101111", "1011111101",
    "1011111111", "11101111",   "11101",      "1101101111", "1011011101", "11111",      "1101110101", "1110101011",
    "1011110111", "1011110101", "1110101101", "1110101111", "1101011011", "1101101011", "1101101101", "1101010111",
    "1101111011", "1101111101", "1110110111", "1101010101", "1101011101", "1110111011", "1011111011", "1101111111",
    "1",          "111111111",  "101011111",  "111110101",  "111011011",  "1011010101", "1010111011", "101111111",
    "11111011",   "11110111",   "101101111",  "111011111",  "1110101",    "110101",     "1010111",    "110101111",
    "10110111",   "10111101",   "11101101",   "11111111",   "101110111",  "101011011",  "101101011",  "110101101",
    "110101011",  "110110111",  "11110101",   "110111101",  "111101101",  "1010101",    "111010111",  "1010101111",
    "1010111101", "1111101",    "11101011",   "10101101",   "10110101",   "1110111",    "11011011",   "11111101",
    "101010101",  "1111111",    "111111101",  "101111101",  "11010111",   "10111011",   "11011101",   "10101011",
    "11010101",   "111011101",  "10101111",   "1101111",    "1101101",    "101010111",  "110110101",  "101011101",
    "101110101",  "101111011",  "1010101101", "111110111",  "111101111",  "111111011",  "1010111111", "101101101",
    "1011011111", "1011",       "1011111",    "101111",     "101101",     "11",         "111101",     "1011011",
    "101011",     "1101",       "111101011",  "10111111",   "11011",      "111011",     "1111",       "111",
    "111111",     "110111111",  "10101",      "10111",      "101",        "110111",     "1111011",    "1101011",
    "11011111",   "1011101",    "111010101",  "1010110111", "110111011",  "1010110101", "1011010111", "1110110101",
  };

  PskDemod() {
    for (int i = 0; i < SPS; i++) {
      cosT_[i] = cosf(2 * Dsp::PI_F * i / SPS);
      sinT_[i] = sinf(2 * Dsp::PI_F * i / SPS);
    }
    begin(1);
  }

  // 1: PSK31, 2: PSK63, 4: PSK125.
  void begin(int mult) {
    mult_ = mult;
    baud_ = 31.25f * mult;
    int per = (int)(RATE / baud_);  // baseband samples a symbol: 64, 32, 16
    fir_.hann(2 * per, per / SPS);
    reset();
  }

  void reset() {
    fir_.clear();
    rot_ = Dsp::Nco();
    off_ = 0;
    for (float& e : energy_) e = 0;
    ph_ = 0;
    target_ = 0;
    wait_ = SPS;
    prev_ = {0, 0};
    sh_ = 0;
    nb_ = 0;
    q_ = recent_ = 2 / Dsp::PI_F;
    valid_ = 0.5f;
    symbols_ = 0;
    text_.clear();
  }

  void feed(Dsp::Cplx x) {
    x = Dsp::mul(x, rot_.next());
    Dsp::Cplx y;
    if (!fir_.push(x, y)) return;
    energy_[ph_] += (Dsp::norm(y) - energy_[ph_]) * ENERGY_K;
    if (--wait_ <= 0) {
      symbol(y);
      // The next one a symbol on, nudged towards the middle.
      float err = target_ - ph_;
      if (err > SPS / 2) err -= SPS;
      if (err < -SPS / 2) err += SPS;
      err *= TIMING_K;
      wait_ += SPS + (err > 1 ? 1 : err < -1 ? -1 : err);
    }
    if (++ph_ == SPS) {
      ph_ = 0;
      findMiddle();
    }
  }

  int mult() const { return mult_; }
  float baud() const { return baud_; }
  float offsetHz() const { return off_; }  // how far the signal is from the channel's centre
  float quality() const { return q_; }
  float valid() const { return valid_; }
  // 0 (noise, or another speed) to 1 (a clean signal at this speed).
  float score() const {
    if (symbols_ < baud_) return 0;  // a second's worth first
    return Dsp::clamp01((q_ - Q_LO) / (Q_HI - Q_LO)) * Dsp::clamp01((valid_ - V_LO) / (V_HI - V_LO));
  }
  const Dsp::Text& text() const { return text_; }

  // Code to character: -1 if not a code.
  static int lookup(uint32_t code) {
    static int8_t rev[2048];
    static bool built = false;
    if (!built) {
      memset(rev, -1, sizeof(rev));
      for (int c = 0; c < 128; c++) {
        uint32_t v = 0;
        for (const char* p = VARICODE[c]; *p; p++) v = v << 1 | (*p == '1');
        rev[v] = c;
      }
      built = true;
    }
    return code < 2048 ? rev[code] : -1;
  }

 private:
  static constexpr float ENERGY_K = 1.0f / 24;  // per symbol: timing follows over ~24 symbols
  static constexpr float AFC_K = 0.05f;
  static constexpr float TIMING_K = 0.25f;
  static constexpr float RECENT_K = 1.0f / 16, SQUELCH = 0.8f;  // noise alone averages 2/pi
  static constexpr float Q_LO = 0.74f, Q_HI = 0.9f, V_LO = 0.6f, V_HI = 0.85f;
  static constexpr int MAX_BITS = 14;  // a run this long without 00 isn't a character

  void findMiddle() {
    float x = 0, y = 0;
    for (int i = 0; i < SPS; i++) {
      x += energy_[i] * cosT_[i];
      y += energy_[i] * sinT_[i];
    }
    float a = atan2f(y, x) * SPS / (2 * Dsp::PI_F);
    target_ = a < 0 ? a + SPS : a;
  }

  void symbol(Dsp::Cplx y) {
    Dsp::Cplx d = Dsp::mulConj(y, prev_);
    prev_ = y;
    float m = sqrtf(Dsp::norm(d));
    if (m <= 0) return;
    symbols_++;
    // ~1.5s averages whatever the speed (a plain average until there's that much)
    float k = 1 / (1.5f * baud_);
    if (symbols_ < 1 / k) k = 1.0f / symbols_;
    q_ += (fabsf(d.re) / m - q_) * k;
    recent_ += (fabsf(d.re) / m - recent_) * RECENT_K;
    Dsp::Cplx d2 = Dsp::mul(d, d);
    float err = atan2f(d2.im, d2.re) / 2;  // radians a symbol
    off_ += AFC_K * err * baud_ / (2 * Dsp::PI_F);
    float lim = 10.0f * mult_;
    if (off_ > lim) off_ = lim;
    if (off_ < -lim) off_ = -lim;
    rot_.set(off_, RATE);
    varicode(d.re > 0);
  }

  void varicode(bool one) {
    sh_ = sh_ << 1 | one;
    nb_++;
    float k = 1 / (2.0f * baud_);  // ~2s of bits
    if ((sh_ & 3) == 0) {
      uint32_t code = sh_ >> 2;
      bool ok = true;
      if (code) {
        int c = lookup(code);
        ok = c >= 32 || c == '\n' || c == '\r';
        if (ok && recent_ > SQUELCH) text_.put((char)c);
      }
      float w = nb_ * k;
      valid_ += ((ok ? 1.0f : 0.0f) - valid_) * (w > 1 ? 1 : w);
      sh_ = 0;
      nb_ = 0;
    } else if (nb_ > MAX_BITS) {
      float w = nb_ * k;
      valid_ -= valid_ * (w > 1 ? 1 : w);
      sh_ = 0;
      nb_ = 0;
    }
  }

  int mult_ = 1;
  float baud_ = 31.25f;
  Dsp::Fir<128> fir_;
  Dsp::Nco rot_;
  float off_;
  float energy_[SPS], cosT_[SPS], sinT_[SPS];
  int ph_;
  float target_;  // the middle of a symbol, in samples into the 16
  float wait_;    // samples to the next symbol
  Dsp::Cplx prev_;
  uint32_t sh_;
  int nb_;
  float q_, valid_;
  float recent_;  // quality over the last ~16 symbols: the squelch
  uint32_t symbols_;
  Dsp::Text text_;
};
