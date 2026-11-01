#pragma once
// RTTY (Baudot, 1 start bit, 5 data bits, 1-2 stop bits) at one shift and baud rate, from
// complex baseband at 2kHz centred between the two tones.
//
// 1. Each tone is mixed to 0Hz and summed over one bit (a boxcar: the matched filter for a
//    keyed tone), and their strengths compared. Each is scaled by its own recent peak (ATC),
//    so a fade on one tone doesn't bias the decisions.
// 2. A UART for each polarity (mark the upper tone, or the lower) waits for mark to space,
//    then samples the bits at their centres. The boxcar delays the edge by half a bit, which
//    lands the samples at the ends of the bits, where it has summed the whole of each one.
//    A frame counts if its start bit is space and its stop bit mark.
//    Characters are kept only while one tone stands clearly over the other where the bits are
//    sampled (the squelch).
// 3. Space runs come in whole bits (mark runs take in the stop bits, which may be 1.5), so each
//    one over its nearest whole number of bits measures the bit length. timing() is how far
//    that is from this baud rate's: it tells 45.45 and 50 baud apart, which framing alone
//    doesn't (a slow UART on a fast signal still samples stop bits as mark). score() needs
//    both.
// 4. And the tones taking turns: overlap() is the weaker of the two (each ATC-scaled), averaged.
//    Real FSK stays under ~0.25; two unrelated carriers a standard shift apart (two PSK
//    signals, say) sit at 0.3-0.6, and would otherwise frame now and then.
#include "dsp.h"

class RttyDemod {
 public:
  static constexpr float RATE = 2000;
  static constexpr int MAX_BOX = 64;  // samples: down to 31 baud

  void begin(float shift, float baud) {
    shift_ = shift;
    baud_ = baud;
    bitLen_ = RATE / baud;
    box_ = (int)(bitLen_ + 0.5f);
    if (box_ > MAX_BOX) box_ = MAX_BOX;
    up_.set(shift / 2, RATE);
    down_.set(-shift / 2, RATE);
    reset();
  }

  void reset() {
    memset(ringU_, 0, sizeof(ringU_));
    memset(ringD_, 0, sizeof(ringD_));
    sumU_ = sumD_ = {0, 0};
    pos_ = 0;
    peakU_ = peakD_ = 1e-9f;
    overlap_ = 0.5f;
    lastSign_ = upper_ = rawUpper_ = false;
    run_ = 0;
    for (int p = 0; p < 2; p++) {
      ratio_[p] = 0.8f;
      runs_[p] = 0;
    }
    for (Uart& u : uart_) u.reset();
  }

  void feed(Dsp::Cplx x) {
    Dsp::Cplx u = Dsp::mul(x, up_.next()), d = Dsp::mul(x, down_.next());
    // Sliding sums over one bit
    sumU_.re += u.re - ringU_[pos_].re;
    sumU_.im += u.im - ringU_[pos_].im;
    sumD_.re += d.re - ringD_[pos_].re;
    sumD_.im += d.im - ringD_[pos_].im;
    ringU_[pos_] = u;
    ringD_[pos_] = d;
    if (++pos_ == box_) {
      pos_ = 0;
      resum();  // so rounding doesn't build up in the sliding sums
    }
    float a = sqrtf(Dsp::norm(sumU_)), b = sqrtf(Dsp::norm(sumD_));
    float decay = 1 / (64 * bitLen_);
    peakU_ += (a - peakU_) * (a > peakU_ ? ATTACK : decay);
    peakD_ += (b - peakD_) * (b > peakD_ ? ATTACK : decay);
    float na = a / (peakU_ + 1e-9f), nb = b / (peakD_ + 1e-9f);
    float disc = na - nb;
    overlap_ += (fminf(na, nb) - overlap_) * (1 / (OVERLAP_BITS * bitLen_));
    if (disc > HYST) upper_ = true;
    else if (disc < -HYST) upper_ = false;
    // Run lengths from the plain comparison: ATC leans towards the tone that's been away
    float raw = (a - b) / (a + b + 1e-9f);
    if (raw > HYST) rawUpper_ = true;
    else if (raw < -HYST) rawUpper_ = false;
    measureRun(rawUpper_);
    uart_[0].step(upper_, fabsf(raw), bitLen_);
    uart_[1].step(!upper_, fabsf(raw), bitLen_);
  }

  float shift() const { return shift_; }
  float baud() const { return baud_; }
  // Polarity 0: mark is the upper tone; 1: the lower.
  float framing(int pol) const { return uart_[pol].valid; }
  // How far the measured bit length is from this baud rate's, as a fraction.
  float timing(int pol) const { return fabsf(ratio_[pol] - 1); }
  // The baud rate the signal seems to be sent at.
  float measuredBaud(int pol) const { return baud_ / ratio_[pol]; }
  float overlap() const { return overlap_; }
  // The tones take turns as FSK's do (overlap under what score() allows at all).
  bool takingTurns() const { return overlap_ < O_HI; }
  float score(int pol) const {
    const Uart& u = uart_[pol];
    if (u.frames < MIN_FRAMES || runs_[pol] < MIN_RUNS) return 0;
    return Dsp::clamp01((u.valid - F_LO) / (F_HI - F_LO)) * Dsp::clamp01((T_HI - timing(pol)) / (T_HI - T_LO)) *
           Dsp::clamp01((O_HI - overlap_) / (O_HI - O_LO));
  }
  int bestPolarity() const { return score(1) > score(0) ? 1 : 0; }
  const Dsp::Text& text(int pol) const { return uart_[pol].text; }

  // ITA2 letters and figures (US figures, as fldigi). 0: nothing; FIGS and LTRS shift.
  static constexpr char FIGS = 1, LTRS = 2;
  static constexpr char LETTERS[32] = {0,   'E', '\n', 'A', ' ', 'S', 'I', 'U', '\r', 'D', 'R',
                                       'J', 'N', 'F',  'C', 'K', 'T', 'Z', 'L', 'W',  'H', 'Y',
                                       'P', 'Q', 'O',  'B', 'G', FIGS, 'M', 'X', 'V', LTRS};
  static constexpr char FIGURES[32] = {0,   '3', '\n', '-', ' ', 0,   '8', '7', '\r', '$', '4',
                                       '\'', ',', '!', ':', '(', '5', '"', ')', '2',  '#', '6',
                                       '0', '1', '9',  '?', '&', FIGS, '.', '/', ';', LTRS};

 private:
  static constexpr float ATTACK = 0.05f;
  static constexpr float HYST = 0.15f;
  static constexpr float SQUELCH = 0.5f;  // contrast at the bits' centres: noise alone averages ~0.4
  static constexpr float CONTRAST_K = 0.15f;  // per bit sampled: over about a character  // of the tones' peaks: noise near the crossing doesn't split runs
  static constexpr float FRAME_K = 1.0f / 12;  // framing averaged over ~12 frames (2s at 45 baud)
  static constexpr float RUN_K = 1.0f / 32;
  static constexpr uint32_t MIN_FRAMES = 6, MIN_RUNS = 12;
  static constexpr float OVERLAP_BITS = 32, O_LO = 0.26f, O_HI = 0.32f;
  static constexpr float F_LO = 0.62f, F_HI = 0.9f, T_LO = 0.025f, T_HI = 0.055f;

  struct Uart {
    enum : uint8_t { WAIT_MARK, ARMED, FRAME } state;
    float t, next;
    int bit;
    uint8_t code;
    bool figs;
    float contrast;  // |mark - space| / (mark + space) where the bits are sampled: the squelch
    float valid;
    uint32_t frames;
    Dsp::Text text;
    void reset() {
      state = WAIT_MARK;
      valid = 0.5f;
      frames = 0;
      figs = false;
      contrast = 0;
      text.clear();
    }
    void step(bool mark, float strength, float len) {
      switch (state) {
        case WAIT_MARK:
          if (mark) state = ARMED;
          break;
        case ARMED:
          if (!mark) {
            state = FRAME;
            t = 0;
            next = 0.5f * len;
            bit = 0;
            code = 0;
          }
          break;
        case FRAME:
          if (++t < next) break;
          next += len;
          contrast += (strength - contrast) * CONTRAST_K;
          if (bit == 0) {
            if (mark) state = ARMED;  // too short for a start bit
          } else if (bit <= 5) {
            code |= mark << (bit - 1);
          } else {
            frames++;
            valid += ((mark ? 1.0f : 0.0f) - valid) * (frames < 1 / FRAME_K ? 1.0f / frames : FRAME_K);
            if (mark && contrast > SQUELCH) put();
            state = mark ? ARMED : WAIT_MARK;
          }
          bit++;
          break;
      }
    }
    void put() {
      char c = (figs ? FIGURES : LETTERS)[code];
      if (c == FIGS) figs = true;
      else if (c == LTRS) figs = false;
      else if (c) {
        if (c == ' ') figs = false;  // unshift on space
        text.put(c);
      }
    }
  };

  void resum() {
    sumU_ = sumD_ = {0, 0};
    for (int i = 0; i < box_; i++) {
      sumU_.re += ringU_[i].re;
      sumU_.im += ringU_[i].im;
      sumD_.re += ringD_[i].re;
      sumD_.im += ringD_[i].im;
    }
  }

  // Each run of one tone, in bits, over its nearest whole number. A lower-tone run is a space
  // run if mark is the upper tone (polarity 0), and the other way round.
  void measureRun(bool upper) {
    run_++;
    if (upper == lastSign_) return;
    lastSign_ = upper;
    float bits = run_ / bitLen_;
    run_ = 0;
    if (bits < 0.6f || bits > 4.4f) return;  // past 4 bits, 10% out could round to the wrong count
    float whole = floorf(bits + 0.5f);
    int pol = upper ? 0 : 1;  // the run that just ended was the lower tone if it's upper now
    runs_[pol]++;
    ratio_[pol] += (bits / whole - ratio_[pol]) * (runs_[pol] < 1 / RUN_K ? 1.0f / runs_[pol] : RUN_K);
  }

  float shift_ = 170, baud_ = 45.45f, bitLen_ = 44;
  int box_ = 44;
  Dsp::Nco up_, down_;
  Dsp::Cplx ringU_[MAX_BOX], ringD_[MAX_BOX], sumU_, sumD_;
  int pos_;
  float peakU_, peakD_;
  float overlap_;
  bool lastSign_, upper_, rawUpper_;
  uint32_t run_;
  float ratio_[2];  // measured bit length over this baud rate's
  uint32_t runs_[2];
  Uart uart_[2];
};
