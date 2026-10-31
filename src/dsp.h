#pragma once
// Turning audio into a line of the waterfall: a Hann-windowed FFT, scaled so a full-scale sine reads 0dBFS.
#include <math.h>
#include <stdint.h>

#include <complex>
#include <vector>

namespace dsp {
// In-place radix-2 FFT. n is a power of two.
inline void fft(std::complex<float>* x, int n) {
  for (int i = 1, j = 0; i < n; i++) {
    int bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) std::swap(x[i], x[j]);
  }
  for (int len = 2; len <= n; len <<= 1) {
    double ang = -2.0 * M_PI / len;
    std::complex<float> wl((float)cos(ang), (float)sin(ang));
    for (int i = 0; i < n; i += len) {
      std::complex<float> w(1.f, 0.f);
      for (int j = 0; j < len / 2; j++) {
        std::complex<float> u = x[i + j], v = x[i + j + len / 2] * w;
        x[i + j] = u + v;
        x[i + j + len / 2] = u - v;
        w *= wl;
      }
    }
  }
}

class Spectrum {
 public:
  explicit Spectrum(int n) : n_(n), win_((size_t)n), buf_((size_t)n) {
    double sum = 0;
    for (int i = 0; i < n; i++) {
      win_[(size_t)i] = 0.5f - 0.5f * (float)cos(2.0 * M_PI * i / n);
      sum += win_[(size_t)i];
    }
    scale_ = (float)(2.0 / sum);
  }
  int size() const { return n_; }
  int bins() const { return n_ / 2; }
  // in holds size() samples; db gets bins() values in dBFS.
  void compute(const float* in, float* db) {
    for (int i = 0; i < n_; i++) buf_[(size_t)i] = std::complex<float>(in[i] * win_[(size_t)i], 0.f);
    fft(buf_.data(), n_);
    for (int i = 0; i < n_ / 2; i++) {
      float re = buf_[(size_t)i].real() * scale_, im = buf_[(size_t)i].imag() * scale_;
      float p = re * re + im * im;
      db[i] = p > 1e-16f ? 10.f * log10f(p) : -160.f;
    }
  }

 private:
  int n_;
  float scale_ = 1;
  std::vector<float> win_;
  std::vector<std::complex<float>> buf_;
};

// dBFS to and from the 0-255 the waterfall stores: -140 to 0dB, about 0.55dB a step.
inline uint8_t dbToLevel(float db) {
  float v = (db + 140.f) * (255.f / 140.f);
  return v <= 0 ? 0 : v >= 255 ? 255 : (uint8_t)v;
}
inline float levelToDb(float level) { return level * (140.f / 255.f) - 140.f; }
}  // namespace dsp
