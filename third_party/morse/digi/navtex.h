#pragma once
// NAVTEX (SITOR-B: CCIR 476 characters, sent twice) at 100 baud and 170Hz shift, from complex
// baseband at 2kHz centred between the two tones.
//
// 1. The tones are compared as in rtty.h: each mixed to 0Hz, summed over one bit (a boxcar)
//    and scaled by its own recent peak (ATC). Their difference is a soft bit.
// 2. The bits run back to back, with no start or stop bits, so a clock is kept. The boxcar holds
//    a whole bit half a bit after the difference crosses zero: that's where it samples, and each
//    crossing nudges it there.
// 3. A character is 7 bits, first bit lowest, with exactly four ones: 35 of the 128 patterns.
//    Each is sent twice, the second copy 5 character slots (35 bits) after the first, the copies
//    of neighbouring characters interleaved. Before a message, the phasing signal sends rep in
//    the first-copy slots and alpha in the second-copy ones. Which bit a character starts on
//    (7 choices), and which slots hold second copies (2), are found by averaging, for each, how
//    often the 7 bits make a character (valid) and how often a second copy matches its first or
//    is phasing (agree). The best of the 14 is followed.
// 4. A character comes out at its second copy: that copy if it's valid, else the first, else
//    the two added bit by bit if that makes one, else '_'.
// score() needs both averages high. On noise a character is valid 27% of the time and almost
// never repeats 35 bits later. A polarity's bits are the other's inverted, which turns every
// character (4 ones) into a non-character (3), so the wrong one never scores.
#include "dsp.h"

class NavtexDemod {
 public:
  static constexpr float RATE = 2000, BAUD = 100, SHIFT = 170;
  static constexpr int BIT = 20;  // samples

  void begin(float shift = SHIFT) {
    up_.set(shift / 2, RATE);
    down_.set(-shift / 2, RATE);
    reset();
  }

  void reset() {
    memset(ringU_, 0, sizeof(ringU_));
    memset(ringD_, 0, sizeof(ringD_));
    memset(bits_, 0, sizeof(bits_));
    sumU_ = sumD_ = {0, 0};
    pos_ = 0;
    peakU_ = peakD_ = 1e-9f;
    phase_ = 0;
    lastRaw_ = 0;
    side_ = 0;
    nBits_ = 0;
    framer_[0].reset();
    framer_[1].reset();
  }

  void feed(Dsp::Cplx x) {
    Dsp::Cplx u = Dsp::mul(x, up_.next()), d = Dsp::mul(x, down_.next());
    sumU_.re += u.re - ringU_[pos_].re;
    sumU_.im += u.im - ringU_[pos_].im;
    sumD_.re += d.re - ringD_[pos_].re;
    sumD_.im += d.im - ringD_[pos_].im;
    ringU_[pos_] = u;
    ringD_[pos_] = d;
    if (++pos_ == BIT) {
      pos_ = 0;
      resum();
    }
    float a = sqrtf(Dsp::norm(sumU_)), b = sqrtf(Dsp::norm(sumD_));
    float decay = 1.0f / (64 * BIT);
    peakU_ += (a - peakU_) * (a > peakU_ ? ATTACK : decay);
    peakD_ += (b - peakD_) * (b > peakD_ ? ATTACK : decay);
    float soft = a / (peakU_ + 1e-9f) - b / (peakD_ + 1e-9f);
    clock((a - b) / (a + b + 1e-9f));
    if (++phase_ >= BIT) {
      phase_ -= BIT;
      takeBit(soft);
    }
  }

  // Polarity 0: a one (B) is the upper tone; 1: the lower.
  float score(int pol) const {
    const Framer& f = framer_[pol];
    if (nBits_ < MIN_BITS || f.at < 0) return 0;
    return Dsp::clamp01((f.valid[f.at] - V_LO) / (V_HI - V_LO)) *
           Dsp::clamp01((f.agree[f.at][f.par] - A_LO) / (A_HI - A_LO));
  }
  int bestPolarity() const { return score(1) > score(0) ? 1 : 0; }
  const Dsp::Text& text(int pol) const { return framer_[pol].text; }
  // How often characters are valid, and second copies agree, where it's following (diagnostics).
  float valid(int pol) const { return framer_[pol].at < 0 ? 0 : framer_[pol].valid[framer_[pol].at]; }
  float agree(int pol) const {
    const Framer& f = framer_[pol];
    return f.at < 0 ? 0 : f.agree[f.at][f.par];
  }

  // CCIR 476 (ITU-R M.476), as fldigi's navtex.cxx: letters and figures for each 7-bit code,
  // '_' for none. The figures' bell (0x4B) is shown as '\''.
  static constexpr char LETTERS[128 + 1] =
      "_______________________J___F_CK_"
      "_______W___Y_PQ______G___MX_V___"
      "_______A___S_IU____D_RE__N__ ___"
      "___Z_L___H__\n____OB_T___\r_______";
  static constexpr char FIGURES[128 + 1] =
      "_______________________'___!_:(_"
      "_______2___6_01______&___./_;___"
      "_______-___'_87____$_43__,__ ___"
      "___\"_)___#__\n____9?_5___\r_______";
  static_assert(sizeof(LETTERS) == 129 && sizeof(FIGURES) == 129, "a character for each code");
  static constexpr uint8_t ALPHA = 0x0F, BETA = 0x33, CHAR32 = 0x6A, REP = 0x66, LTRS = 0x5A, FIGS = 0x36;
  static bool isChar(uint8_t c) {  // four bits set (a loop, as Visual Studio has no __builtin_popcount)
    int ones = 0;
    for (; c; c &= (uint8_t)(c - 1)) ones++;
    return ones == 4;
  }

 private:
  static constexpr float ATTACK = 0.05f;
  static constexpr float HYST = 0.15f;
  static constexpr float CLOCK_K = 0.08f;  // of a crossing's timing error, per crossing
  static constexpr int RING = 64;          // bits kept: a character and its first copy 35 back
  static constexpr float AVG_K = 1.0f / 16;  // valid and agree: over ~16 characters
  static constexpr float SWITCH = 0.15f;  // another start bit/parity must agree this much more
  // 0.84s: some characters' worth, before the engine's first decision (1.5s), as NAVTEX has to
  // overrule RTTY there (engine.h). Noise can't score this soon: it almost never repeats a character.
  static constexpr uint32_t MIN_BITS = 7 * 12;
  static constexpr float V_LO = 0.5f, V_HI = 0.85f, A_LO = 0.2f, A_HI = 0.6f;
  // Printed only while valid where it's following is over SQUELCH, and the copies have agreed
  // lately (over ~4 characters): the slow averages take half a second to fall when a signal ends.
  static constexpr float SQUELCH = 0.5f, RECENT_K = 0.25f, RECENT_MIN = 0.3f;

  struct Framer {
    float valid[7];
    float agree[7][2];
    uint32_t n[7];  // characters averaged at each start bit
    int at, par;    // the start bit (bit count mod 7) and second-copy parity followed; at < 0: none
    float recent;   // agree where it's following, fast
    bool figs;
    Dsp::Text text;
    void reset() {
      for (int a = 0; a < 7; a++) {
        valid[a] = agree[a][0] = agree[a][1] = 0;
        n[a] = 0;
      }
      at = -1;
      par = 0;
      recent = 0;
      figs = false;
      text.clear();
    }
    void put(uint8_t c) {
      if (c == LTRS) figs = false;
      else if (c == FIGS) figs = true;
      else if (c == ALPHA || c == BETA || c == CHAR32 || c == REP) return;
      else text.put((figs ? FIGURES : LETTERS)[c]);
    }
  };

  // Nudges the bit clock toward half a bit after each crossing of the tones' difference. Only
  // a crossing from one clear side to the other counts, so noise near zero doesn't.
  // The bit is taken at phase BIT - 1, so a crossing should be at BIT / 2 - 1.
  void clock(float raw) {
    if ((raw > 0) != (lastRaw_ > 0)) cross_ = phase_ - fabsf(raw) / (fabsf(raw) + fabsf(lastRaw_) + 1e-9f);
    lastRaw_ = raw;
    int side = raw > HYST ? 1 : raw < -HYST ? -1 : 0;
    if (!side || side == side_) return;
    if (side_) {  // clearly from one side to the other: the last crossing was a real one
      float e = cross_ - (BIT / 2.0f - 1);
      if (e > BIT / 2.0f) e -= BIT;
      if (e < -BIT / 2.0f) e += BIT;
      phase_ -= CLOCK_K * e;
    }
    side_ = side;
  }

  float softBit(uint32_t i) const { return bits_[i % RING]; }

  // The 7 bits ending `back` bits before the newest, as a code (with `sign` -1: inverted).
  uint8_t code(uint32_t back, float sign) const {
    uint32_t end = nBits_ - 1 - back;
    uint8_t c = 0;
    for (int i = 0; i < 7; i++) c |= (softBit(end - 6 + i) * sign > 0) << i;
    return c;
  }

  void takeBit(float soft) {
    bits_[nBits_ % RING] = soft;
    nBits_++;
    if (nBits_ < 42) return;
    for (int pol = 0; pol < 2; pol++) frame(framer_[pol], pol ? -1.0f : 1.0f);
  }

  void frame(Framer& f, float sign) {
    uint32_t t = nBits_ - 1;
    int a = t % 7, p = (t / 7) % 2;
    uint8_t c = code(0, sign), first = code(35, sign), prev = code(7, sign);
    bool ok = isChar(c);
    float k = ++f.n[a] < 1 / AVG_K ? 1.0f / f.n[a] : AVG_K;
    f.valid[a] += ((ok ? 1.0f : 0.0f) - f.valid[a]) * k;
    bool agrees = (ok && c == first) || (c == ALPHA && prev == REP);
    // Each parity is updated every other character: the same step as valid's, for half as many
    float kp = f.n[a] < 2 / AVG_K ? 2.0f / (f.n[a] + 1) : AVG_K;
    f.agree[a][p] += ((agrees ? 1.0f : 0.0f) - f.agree[a][p]) * fminf(1, kp);

    // Follow the start bit and parity that agree most, sticking with it unless another is clearly better.
    float best = f.at < 0 ? -1 : f.agree[f.at][f.par] + SWITCH;
    if (f.agree[a][p] > best && f.valid[a] > V_LO) {
      if (f.at != a) f.figs = false;
      f.at = a;
      f.par = p;
      f.recent = f.agree[a][p];
    }
    if (a != f.at || p != f.par) return;
    f.recent += ((agrees ? 1.0f : 0.0f) - f.recent) * RECENT_K;
    if (f.valid[a] < SQUELCH || f.recent < RECENT_MIN) return;
    if (ok) return f.put(c);
    if (isChar(first)) return f.put(first);
    uint32_t end = nBits_ - 1;
    uint8_t sum = 0;
    for (int i = 0; i < 7; i++) sum |= ((softBit(end - 6 + i) + softBit(end - 41 + i)) * sign > 0) << i;
    if (isChar(sum)) return f.put(sum);
    f.text.put('_');
  }

  void resum() {
    sumU_ = sumD_ = {0, 0};
    for (int i = 0; i < BIT; i++) {
      sumU_.re += ringU_[i].re;
      sumU_.im += ringU_[i].im;
      sumD_.re += ringD_[i].re;
      sumD_.im += ringD_[i].im;
    }
  }

  Dsp::Nco up_, down_;
  Dsp::Cplx ringU_[BIT], ringD_[BIT], sumU_, sumD_;
  int pos_;
  float peakU_, peakD_;
  float phase_;  // samples since the last bit was taken
  float lastRaw_, cross_ = 0;
  int side_;  // 1/-1: the difference was last clearly upper/lower
  float bits_[RING];
  uint32_t nBits_;
  Framer framer_[2];
};
