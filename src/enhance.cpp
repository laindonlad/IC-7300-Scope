#include "enhance.h"

#include <math.h>
#include <string.h>

#include <algorithm>
#include <complex>
#include <vector>

#include "audio.h"
#include "dsp.h"
#include "rnnoise.h"

namespace {
constexpr float kRate = (float)AudioEngine::kRate;

// The exponential integral E1, for the log-spectral gain. Abramowitz and Stegun 5.1.53 and 5.1.56.
float expint(float v) {
  if (v < 1e-6f) v = 1e-6f;
  if (v < 1.f)
    return -0.57721566f - logf(v) + v * (0.99999193f + v * (-0.24991055f + v * (0.05519968f + v * (-0.00976004f + v * 0.00107857f))));
  if (v > 30.f) return 0.f;
  return expf(-v) / v * ((v * v + 2.334733f * v + 0.250621f) / (v * v + 3.330657f * v + 1.681534f));
}

// Ephraim and Malah's MMSE log-spectral amplitude estimator, with the a priori SNR decision-directed and
// the noise from minimum statistics: the floor is the quietest each bin has been over the last second
// or so, which speech and keying never hold for long.
class SpectralNr {
 public:
  static constexpr int kN = 1024, kHop = kN / 2, kBins = kN / 2 + 1;
  SpectralNr() : win_(kN), frame_(kN), tail_(kHop), buf_(kN), smooth_(kBins), subMin_(kBins), noise_(kBins),
                 hist_(kHist * kBins), prevPow_(kBins), gain_(kBins), g2_(kBins) {
    for (int i = 0; i < kN; i++) win_[i] = sqrtf(0.5f - 0.5f * cosf(2.f * (float)M_PI * i / kN));  // squares sum to 1 at half overlap
    reset();
  }
  void reset() {
    std::fill(frame_.begin(), frame_.end(), 0.f);
    std::fill(tail_.begin(), tail_.end(), 0.f);
    std::fill(prevPow_.begin(), prevPow_.end(), 0.f);
    std::fill(gain_.begin(), gain_.end(), 1.f);
    frames_ = 0;
  }
  // kHop samples in, kHop out, kHop later.
  void block(const float* in, float* out, float strength) {
    memmove(frame_.data(), frame_.data() + kHop, sizeof(float) * kHop);
    memcpy(frame_.data() + kHop, in, sizeof(float) * kHop);
    for (int i = 0; i < kN; i++) buf_[i] = std::complex<float>(frame_[i] * win_[i], 0.f);
    dsp::fft(buf_.data(), kN);

    const float eps = 1e-14f;
    bool first = frames_ == 0;
    for (int k = 0; k < kBins; k++) {
      float p = std::norm(buf_[k]);
      smooth_[k] = first ? p : 0.7f * smooth_[k] + 0.3f * p;
      if (first) {
        subMin_[k] = smooth_[k];
        for (int h = 0; h < kHist; h++) hist_[h * kBins + k] = smooth_[k];
      } else {
        subMin_[k] = std::min(subMin_[k], smooth_[k]);
      }
    }
    // A new sub-window every kSub frames; the floor is the least of the last kHist.
    if (++frames_ % kSub == 0) {
      int slot = (frames_ / kSub) % kHist;
      memcpy(&hist_[slot * kBins], subMin_.data(), sizeof(float) * kBins);
      memcpy(subMin_.data(), smooth_.data(), sizeof(float) * kBins);
    }
    // Stronger means a higher estimate of the noise and a deeper floor for the gain.
    const float over = 1.f + 1.5f * strength;
    const float floorGain = powf(10.f, -(6.f + 24.f * strength) / 20.f);
    const float xiMin = floorGain * floorGain;
    for (int k = 0; k < kBins; k++) {
      float m = subMin_[k];
      for (int h = 0; h < kHist; h++) m = std::min(m, hist_[h * kBins + k]);
      noise_[k] = 1.6f * m * over + eps;  // the least of a smoothed spectrum sits below its mean
      float p = std::norm(buf_[k]);
      float gamma = std::min(p / noise_[k], 1e4f);
      float xi = first ? std::max(gamma - 1.f, 0.f) : 0.98f * prevPow_[k] / noise_[k] + 0.02f * std::max(gamma - 1.f, 0.f);
      xi = std::max(xi, xiMin);
      float r = xi / (1.f + xi);
      float g = std::min(1.f, r * expf(0.5f * expint(r * gamma)));
      g2_[k] = std::max(g, floorGain);
      prevPow_[k] = g2_[k] * g2_[k] * p;
    }
    // A little smoothing across frequency, against "musical noise": lone bins left standing.
    for (int k = 0; k < kBins; k++) {
      float a = g2_[k > 0 ? k - 1 : k], b = g2_[k], c = g2_[k < kBins - 1 ? k + 1 : k];
      gain_[k] = 0.25f * a + 0.5f * b + 0.25f * c;
    }
    for (int k = 0; k < kBins; k++) buf_[k] *= gain_[k];
    for (int k = 1; k < kN / 2; k++) buf_[kN - k] = std::conj(buf_[k]);
    // The inverse, by way of the forward transform.
    for (auto& z : buf_) z = std::conj(z);
    dsp::fft(buf_.data(), kN);
    const float scale = 1.f / kN;
    for (int i = 0; i < kHop; i++) out[i] = tail_[i] + buf_[i].real() * scale * win_[i];
    for (int i = 0; i < kHop; i++) tail_[i] = buf_[kHop + i].real() * scale * win_[kHop + i];
  }

 private:
  static constexpr int kSub = 12, kHist = 8;  // 12 frames is 128ms; eight of them about a second
  std::vector<float> win_, frame_, tail_;
  std::vector<std::complex<float>> buf_;
  std::vector<float> smooth_, subMin_, noise_, hist_, prevPow_, gain_, g2_;
  int frames_ = 0;
};

// Brings the peaks to about -6dBFS: fast down, held, then slowly back up, at most 18dB.
class Leveller {
 public:
  void reset() { env_ = 0.f, gain_ = 1.f, hold_ = 0; }
  void run(float* s, int n) {
    const float att = 1.f - expf(-1.f / (0.001f * kRate));
    const float rel = powf(10.f, -6.f / 20.f / kRate);  // 6dB a second
    const float down = 1.f - expf(-1.f / (0.002f * kRate)), up = 1.f - expf(-1.f / (0.08f * kRate));
    const float target = 0.5f, maxGain = 8.f;
    const int holdN = (int)(0.3f * kRate);
    for (int i = 0; i < n; i++) {
      float a = fabsf(s[i]);
      if (a > env_) env_ += (a - env_) * att, hold_ = holdN;
      else if (hold_ > 0) hold_--;
      else env_ *= rel;
      float want = target / std::max(env_, target / maxGain);
      gain_ += (want - gain_) * (want < gain_ ? down : up);
      float y = s[i] * gain_;
      // What gets past the attack is rounded off rather than clipped.
      float ay = fabsf(y);
      if (ay > 0.9f) y = copysignf(0.9f + 0.1f * tanhf((ay - 0.9f) / 0.1f), y);
      s[i] = y;
    }
  }

 private:
  float env_ = 0.f, gain_ = 1.f;
  int hold_ = 0;
};
// One biquad, from Robert Bristow-Johnson's cookbook. In doubles, which a low cut at 48kHz needs.
struct Biquad {
  double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
  enum Kind { HIGH_PASS, LOW_PASS, LOW_SHELF, PEAK, HIGH_SHELF };
  void set(Kind kind, double hz, double q, double db) {
    double A = pow(10.0, db / 40.0), w = 2.0 * M_PI * hz / kRate, c = cos(w), sn = sin(w), al = sn / (2.0 * q);
    double B0, B1, B2, A0, A1, A2;
    switch (kind) {
      case HIGH_PASS: B0 = (1 + c) / 2, B1 = -(1 + c), B2 = (1 + c) / 2, A0 = 1 + al, A1 = -2 * c, A2 = 1 - al; break;
      case LOW_PASS: B0 = (1 - c) / 2, B1 = 1 - c, B2 = (1 - c) / 2, A0 = 1 + al, A1 = -2 * c, A2 = 1 - al; break;
      case PEAK: B0 = 1 + al * A, B1 = -2 * c, B2 = 1 - al * A, A0 = 1 + al / A, A1 = -2 * c, A2 = 1 - al / A; break;
      default: {
        double sq = 2 * sqrt(A) * al, s = kind == LOW_SHELF ? 1 : -1;
        B0 = A * ((A + 1) - s * (A - 1) * c + sq);
        B1 = s * 2 * A * ((A - 1) - s * (A + 1) * c);
        B2 = A * ((A + 1) - s * (A - 1) * c - sq);
        A0 = (A + 1) + s * (A - 1) * c + sq;
        A1 = -s * 2 * ((A - 1) + s * (A + 1) * c);
        A2 = (A + 1) + s * (A - 1) * c - sq;
      }
    }
    b0 = B0 / A0, b1 = B1 / A0, b2 = B2 / A0, a1 = A1 / A0, a2 = A2 / A0;
  }
  void bypass() { b0 = 1, b1 = b2 = a1 = a2 = 0; }
  float run(float x) {  // transposed direct form II
    double y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return (float)y;
  }
  void clear() { z1 = z2 = 0; }
};

// The cuts are fourth-order Butterworth, two biquads each, so they're steep enough to hear.
class Equaliser {
 public:
  void configure(float low, float bass, float mid, float treble, float high) {
    if (low == low_ && bass == bass_ && mid == mid_ && treble == treble_ && high == high_) return;
    low_ = low, bass_ = bass, mid_ = mid, treble_ = treble, high_ = high;
    const double q1 = 0.5412, q2 = 1.3066;
    if (low > 0) f_[0].set(Biquad::HIGH_PASS, low, q1, 0), f_[1].set(Biquad::HIGH_PASS, low, q2, 0);
    else f_[0].bypass(), f_[1].bypass();
    f_[2].set(Biquad::LOW_SHELF, 250, 0.707, bass);
    f_[3].set(Biquad::PEAK, 1200, 0.8, mid);
    f_[4].set(Biquad::HIGH_SHELF, 2200, 0.707, treble);
    if (high > 0) f_[5].set(Biquad::LOW_PASS, high, q1, 0), f_[6].set(Biquad::LOW_PASS, high, q2, 0);
    else f_[5].bypass(), f_[6].bypass();
  }
  void reset() {
    for (Biquad& b : f_) b.clear();
  }
  void run(float* s, int n) {
    for (int i = 0; i < n; i++) {
      float x = s[i];
      for (Biquad& b : f_) x = b.run(x);
      s[i] = x;
    }
  }

 private:
  Biquad f_[7];
  float low_ = -1, bass_ = 0, mid_ = 0, treble_ = 0, high_ = 0;
};
}  // namespace

struct Enhancer::Impl {
  DenoiseState* rnn = nullptr;
  SpectralNr spectral;
  Leveller leveller;
  Equaliser eq;
  bool equalising = false;
  // A block in, the last block's result out, so every call gives back what it was given.
  static constexpr int kMaxBlock = SpectralNr::kHop;
  float in[kMaxBlock] = {}, out[kMaxBlock] = {}, dry[kMaxBlock] = {}, tmp[kMaxBlock] = {};
  int pos = 0, blockLen = 0, running = -1;
  bool levelling = false;

  void start(int method) {
    running = method;
    pos = 0;
    memset(in, 0, sizeof in), memset(out, 0, sizeof out), memset(dry, 0, sizeof dry);
    if (method == VOICE) {
      if (rnn) rnnoise_init(rnn, nullptr);  // the built-in model: no allocation
      blockLen = rnnoise_get_frame_size();
    } else if (method == SPECTRAL) {
      spectral.reset();
      blockLen = SpectralNr::kHop;
    } else {
      blockLen = 0;
    }
  }
  void block(float strength) {
    if (running == VOICE && rnn) {
      // RNNoise works in 16-bit units, and gives back the frame before: the dry mix has to wait as well.
      float x[kMaxBlock];
      for (int i = 0; i < blockLen; i++) x[i] = in[i] * 32768.f;
      rnnoise_process_frame(rnn, tmp, x);
      float d = (1.f - strength) * (1.f - strength);
      for (int i = 0; i < blockLen; i++) {
        float wet = tmp[i] / 32768.f;
        out[i] = wet + d * (dry[i] - wet);
      }
      memcpy(dry, in, sizeof(float) * blockLen);
    } else if (running == SPECTRAL) {
      spectral.block(in, out, strength);
    }
  }
};

Enhancer::Enhancer() : impl_(new Impl) {
  static_assert(Impl::kMaxBlock >= 480, "a block must hold RNNoise's frame");
  impl_->rnn = rnnoise_create(nullptr);
}

Enhancer::~Enhancer() {
  if (impl_->rnn) rnnoise_destroy(impl_->rnn);
}

void Enhancer::process(float* s, int n) {
  Impl& m = *impl_;
  bool eq = eqOn_.load(std::memory_order_relaxed);
  if (eq != m.equalising) {
    m.equalising = eq;
    m.eq.reset();
  }
  if (eq) {
    m.eq.configure(eqLow_.load(std::memory_order_relaxed), eqBass_.load(std::memory_order_relaxed), eqMid_.load(std::memory_order_relaxed),
                   eqTreble_.load(std::memory_order_relaxed), eqHigh_.load(std::memory_order_relaxed));
    m.eq.run(s, n);
  }
  int method = method_.load(std::memory_order_relaxed);
  if (method == VOICE && !m.rnn) method = SPECTRAL;
  if (method != m.running) m.start(method);
  bool lev = leveller_.load(std::memory_order_relaxed);
  if (lev != m.levelling) {
    m.levelling = lev;
    m.leveller.reset();
  }
  if (m.blockLen > 0) {
    float strength = std::max(0.f, std::min(1.f, strength_.load(std::memory_order_relaxed)));
    for (int i = 0; i < n; i++) {
      float y = m.out[m.pos];
      m.in[m.pos] = s[i];
      s[i] = y;
      if (++m.pos == m.blockLen) {
        m.block(strength);
        m.pos = 0;
      }
    }
  }
  if (lev) m.leveller.run(s, n);
}
