#pragma once
// What the digital-mode decoders (src/digi/) share: complex samples, an oscillator, filters, an
// FFT and a text buffer. Plain C++, no Arduino, so it all builds on a PC (tools/digi_test).
#include <math.h>
#include <stdint.h>
#include <string.h>

namespace Dsp {
constexpr float PI_F = 3.14159265f;

struct Cplx {
  float re, im;
};
inline Cplx mul(Cplx a, Cplx b) { return {a.re * b.re - a.im * b.im, a.re * b.im + a.im * b.re}; }
// a times b's conjugate: b's phase taken off a's
inline Cplx mulConj(Cplx a, Cplx b) { return {a.re * b.re + a.im * b.im, a.im * b.re - a.re * b.im}; }
inline float norm(Cplx a) { return a.re * a.re + a.im * a.im; }

// A turning unit vector. next() returns e^(-j w n): multiplying by it moves `hz` down to 0Hz.
struct Nco {
  float c = 1, s = 0, stepC = 1, stepS = 0;
  int n = 0;
  void set(float hz, float rate) {
    stepC = cosf(2 * PI_F * hz / rate);
    stepS = -sinf(2 * PI_F * hz / rate);
  }
  Cplx next() {
    Cplx v = {c, s};
    float nc = c * stepC - s * stepS;
    s = c * stepS + s * stepC;
    c = nc;
    if (++n == 512) {  // rounding would slowly change its length
      n = 0;
      float g = 1.5f - 0.5f * (c * c + s * s);
      c *= g;
      s *= g;
    }
    return v;
  }
};

// A low-pass FIR on complex samples that keeps one output in `decim`. The taps are a windowed
// sinc (or set directly), scaled to unity gain at 0Hz.
template <int MAX>
struct Fir {
  float h[MAX];
  Cplx buf[2 * MAX];
  int len = 1, decim = 1, pos = 0, phase = 0;

  // Hamming-windowed sinc, cut-off (-6dB) at `hz`.
  void lowpass(int taps, float hz, float rate, int keepOne) {
    len = taps;
    decim = keepOne;
    float fc = hz / rate, sum = 0;
    for (int i = 0; i < len; i++) {
      float x = i - (len - 1) / 2.0f;
      float sinc = x == 0 ? 2 * fc : sinf(2 * PI_F * fc * x) / (PI_F * x);
      h[i] = sinc * (0.54f - 0.46f * cosf(2 * PI_F * i / (len - 1)));
      sum += h[i];
    }
    for (int i = 0; i < len; i++) h[i] /= sum;
    clear();
  }
  // A Hann window `taps` long: the matched filter for a raised-cosine pulse that long.
  void hann(int taps, int keepOne) {
    len = taps;
    decim = keepOne;
    float sum = 0;
    for (int i = 0; i < len; i++) sum += h[i] = 0.5f - 0.5f * cosf(2 * PI_F * (i + 0.5f) / len);
    for (int i = 0; i < len; i++) h[i] /= sum;
    clear();
  }
  void clear() {
    memset(buf, 0, sizeof(buf));
    pos = phase = 0;
  }
  // Takes a sample; true, with `out`, when an output is due.
  bool push(Cplx x, Cplx& out) {
    buf[pos] = buf[pos + len] = x;
    if (++pos == len) pos = 0;
    if (++phase < decim) return false;
    phase = 0;
    const Cplx* b = buf + pos;  // oldest first
    float re = 0, im = 0;
    for (int i = 0; i < len; i++) {
      re += h[i] * b[i].re;
      im += h[i] * b[i].im;
    }
    out = {re, im};
    return true;
  }
};

// A 2nd-order Butterworth low-pass on complex samples (transposed direct form II).
struct Biquad {
  float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
  Cplx z1 = {0, 0}, z2 = {0, 0};
  void lowpass(float hz, float rate) {
    float w = 2 * PI_F * hz / rate, cw = cosf(w), al = sinf(w) / (2 * 0.70710678f), a0 = 1 + al;
    b0 = b2 = (1 - cw) / 2 / a0;
    b1 = (1 - cw) / a0;
    a1 = -2 * cw / a0;
    a2 = (1 - al) / a0;
    clear();
  }
  void clear() { z1 = z2 = {0, 0}; }
  Cplx run(Cplx x) {
    Cplx y = {b0 * x.re + z1.re, b0 * x.im + z1.im};
    z1 = {b1 * x.re - a1 * y.re + z2.re, b1 * x.im - a1 * y.im + z2.im};
    z2 = {b2 * x.re - a2 * y.re, b2 * x.im - a2 * y.im};
    return y;
  }
};

// Real audio at 16kHz to complex baseband at 2kHz, `hz` moved to 0. It passes +-540Hz flat
// (an 850Hz RTTY shift and its sidebands) and stops what would fold back into that.
struct Downconverter {
  static constexpr float IN_RATE = 16000, OUT_RATE = 2000;
  Nco nco;
  Fir<64> fir;
  float hz = 0;
  void begin(float centre) {
    hz = centre;
    nco = Nco();
    nco.set(centre, IN_RATE);
    fir.lowpass(64, 950, IN_RATE, (int)(IN_RATE / OUT_RATE));
  }
  bool push(float x, Cplx& out) {
    Cplx v = nco.next();
    return fir.push({x * v.re, x * v.im}, out);
  }
};

// An in-place radix-2 complex FFT of N points.
template <int N>
struct Fft {
  Cplx tw[N / 2];
  uint16_t rev[N];
  Fft() {
    int bits = 0;
    while ((1 << bits) < N) bits++;
    for (int i = 0; i < N; i++) {
      int r = 0;
      for (int b = 0; b < bits; b++)
        if (i >> b & 1) r |= 1 << (bits - 1 - b);
      rev[i] = r;
    }
    for (int i = 0; i < N / 2; i++) tw[i] = {cosf(2 * PI_F * i / N), -sinf(2 * PI_F * i / N)};
  }
  void run(Cplx* x) const {
    for (int i = 0; i < N; i++)
      if (rev[i] > i) {
        Cplx t = x[i];
        x[i] = x[rev[i]];
        x[rev[i]] = t;
      }
    for (int len = 2; len <= N; len <<= 1) {
      int half = len / 2, step = N / len;
      for (int i = 0; i < N; i += len)
        for (int k = 0; k < half; k++) {
          Cplx t = mul(x[i + k + half], tw[k * step]);
          Cplx u = x[i + k];
          x[i + k] = {u.re + t.re, u.im + t.im};
          x[i + k + half] = {u.re - t.re, u.im - t.im};
        }
    }
  }
};

// Decoded text, oldest first, as MorseDecoder keeps it: the oldest quarter goes when it's full,
// and scrolled() counts what's gone. seq() changes with every character. Carriage returns are
// dropped, line feeds kept (at most two in a row), other control characters dropped.
class Text {
 public:
  static constexpr int N = 256;
  void clear() {
    len_ = 0;
    t_[0] = 0;
    scrolled_ = 0;
    seq_++;
  }
  void put(char c) {
    if (c == '\r' || c == 0) return;
    if (c == '\t') c = ' ';
    if (c != '\n' && (c < 32 || c > 126)) return;
    if (c == '\n' && len_ >= 2 && t_[len_ - 1] == '\n' && t_[len_ - 2] == '\n') return;
    if (len_ == N - 1) {
      memmove(t_, t_ + N / 4, len_ - N / 4);
      len_ -= N / 4;
      scrolled_ += N / 4;
    }
    t_[len_++] = c;
    t_[len_] = 0;
    seq_++;
  }
  const char* text() const { return t_; }
  int length() const { return len_; }
  uint32_t scrolled() const { return scrolled_; }
  uint32_t seq() const { return seq_; }

 private:
  char t_[N] = "";
  int len_ = 0;
  uint32_t scrolled_ = 0, seq_ = 0;
};

inline float clamp01(float x) { return x < 0 ? 0 : x > 1 ? 1 : x; }
}  // namespace Dsp
