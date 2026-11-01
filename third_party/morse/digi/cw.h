#pragma once
// CW for the multi-mode decoder: the existing MorseDecoder, fed one signal. Complex baseband at
// 2kHz centred on the tone is narrowed to +-BW, brought back up to 16kHz (linear interpolation:
// its images land well clear of MorseDecoder's 400-1200Hz) and moved to 700Hz, so neighbours
// in the pass band don't reach the decoder.
//
// score() asks for MorseDecoder locked most of the time at a believable speed, with the key up
// for a fair share of it (a steady carrier, PSK, doesn't pass), and for quiet around the tone
// while it's up: one tone of an RTTY pair keys on and off like CW, but the other tone fills the
// gaps. That's measured as the power within BW but away from the tone (+-NARROW) with the key up,
// over the tone's power with it down.
#include "../morse_decoder.h"
#include "dsp.h"

class CwDemod {
 public:
  static constexpr float RATE = 2000;
  static constexpr int UP = MorseDecoder::RATE / 2000;
  static constexpr float TONE = 700, BW = 200;

  CwDemod() {
    dec_.setToneRange(TONE - RANGE, TONE + RANGE);
    reset();
  }

  void reset() {
    for (Dsp::Biquad& b : lp_) b.lowpass(BW, RATE);
    for (Dsp::Biquad& b : narrow_) b.lowpass(NARROW, RATE);
    toneMix_ = Dsp::Nco();
    toneOff_ = 0;
    sumWide_ = sumTone_ = 0;
    upAround_ = 0;
    downTone_ = 1e-9f;
    up_ = Dsp::Nco();
    up_.set(-TONE, MorseDecoder::RATE);
    prev_ = {0, 0};
    n_ = 0;
    dec_.reset();
    lock_ = down_ = 0;
    off_ = 0;
    blocks_ = lockedBlocks_ = 0;
  }

  void feed(Dsp::Cplx x) {
    x = lp_[1].run(lp_[0].run(x));
    Dsp::Cplx t = narrow_[1].run(narrow_[0].run(Dsp::mul(x, toneMix_.next())));
    sumWide_ += Dsp::norm(x);
    sumTone_ += Dsp::norm(t);
    for (int i = 1; i <= UP; i++) {
      float f = (float)i / UP;
      Dsp::Cplx v = {prev_.re + (x.re - prev_.re) * f, prev_.im + (x.im - prev_.im) * f};
      float s = Dsp::mul(v, up_.next()).re * 2 * 32767;
      buf_[n_++] = (int16_t)(s > 32767 ? 32767 : s < -32767 ? -32767 : s);
    }
    prev_ = x;
    if (n_ < BLOCK) return;
    dec_.feed(buf_, BLOCK);
    n_ = 0;
    blocks_++;
    // ~2s averages (plain ones until there's that much)
    float k = blocks_ < 1 / K ? 1.0f / blocks_ : K;
    bool l = dec_.locked();
    if (l && fabsf(dec_.toneHz() - TONE) <= MAX_OFF) off_ = dec_.toneHz() - TONE;
    lock_ += ((l ? 1.0f : 0.0f) - lock_) * k;
    if (l) {
      lockedBlocks_++;
      float kl = lockedBlocks_ < 1 / K ? 1.0f / lockedBlocks_ : K;
      down_ += ((dec_.keyDown() ? 1.0f : 0.0f) - down_) * kl;
      float around = sumWide_ > sumTone_ ? sumWide_ - sumTone_ : 0;
      if (dec_.keyDown()) downTone_ += (sumTone_ - downTone_) * kl;
      else upAround_ += (around - upAround_) * kl;
    }
    sumWide_ = sumTone_ = 0;
    float off = offsetHz();
    if (fabsf(off - toneOff_) > 4) {  // keep the narrow filter on the tone
      toneOff_ = off;
      toneMix_.set(off, RATE);
    }
  }

  float score() const {
    if (blocks_ < WARM) return 0;
    float w = dec_.wpm();
    if (w < 5 || w > 45) return 0;
    // it's hearing a neighbour: that has its own channel
    if (fabsf(offsetHz()) > MAX_OFF || (dec_.locked() && fabsf(dec_.toneHz() - TONE) > MAX_OFF)) return 0;
    float s = Dsp::clamp01((lock_ - 0.3f) / 0.4f);
    if (down_ > 0.8f) s *= Dsp::clamp01((0.92f - down_) / 0.12f);
    if (down_ < 0.15f) s *= Dsp::clamp01(down_ / 0.15f);
    s *= Dsp::clamp01((0.8f - around()) / 0.4f);
    return s;
  }
  float wpm() const { return dec_.wpm(); }
  // Power around the tone with the key up, over the tone's with it down.
  float around() const { return upAround_ / downTone_; }
  float snrDb() const { return dec_.snrDb(); }
  bool keyDown() const { return dec_.keyDown(); }
  // The tone's offset from the channel's centre, as last heard locked.
  float offsetHz() const { return off_; }
  const MorseDecoder& decoder() const { return dec_; }

 private:
  static constexpr int BLOCK = 64;
  static constexpr float K = 1.0f / 500;  // per 4ms block: ~2s
  static constexpr uint32_t WARM = 375;   // 1.5s
  static constexpr float NARROW = 40;
  static constexpr float MAX_OFF = 60;
  static constexpr float RANGE = 100;  // Hz either side of TONE the decoder may take as its tone

  Dsp::Biquad lp_[2], narrow_[2];
  Dsp::Nco toneMix_;
  float toneOff_;
  float sumWide_, sumTone_, upAround_, downTone_;
  Dsp::Nco up_;
  Dsp::Cplx prev_;
  int16_t buf_[BLOCK];
  int n_;
  MorseDecoder dec_;
  float lock_, down_, off_;
  uint32_t blocks_, lockedBlocks_;
};
