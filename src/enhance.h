#pragma once
// Cleaning up what you hear through the speakers, when reception is bad. Only the monitor gets it:
// the waterfall, the decoders and the recorder keep the rig's audio untouched.
//
//  Voice     RNNoise, a small neural network trained on speech. Best for SSB, AM and FM, but it
//            takes CW and data tones for noise.
//  Spectral  An MMSE log-spectral estimator over a tracked noise floor (the "NR2" of SDR programs).
//            Works on anything, CW and data included.
//  Leveller  An AGC after either, so fading and quiet stations come up evenly.
//  EQ        Low and high cuts with bass, middle and treble, ahead of the rest. Separate from the
//            noise reduction: it can be on while that's off.
//
// process() runs on the audio thread. The settings are atomics, so the window changes them as it likes.
#include <atomic>
#include <memory>

class Enhancer {
 public:
  enum Method { OFF, VOICE, SPECTRAL };
  Enhancer();
  ~Enhancer();

  void setMethod(int m) { method_ = m; }
  int method() const { return method_; }
  void setStrength(float s) { strength_ = s; }  // 0-1
  void setLeveller(bool on) { leveller_ = on; }
  // Cuts in Hz, 0 for none; the rest in dB.
  void setEq(bool on, float lowCut, float bassDb, float midDb, float trebleDb, float highCut) {
    eqLow_ = lowCut, eqBass_ = bassDb, eqMid_ = midDb, eqTreble_ = trebleDb, eqHigh_ = highCut;
    eqOn_ = on;
  }

  // In place. Never allocates or waits. Delays by a block (about 10ms) when on.
  void process(float* s, int n);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  std::atomic<int> method_{OFF};
  std::atomic<float> strength_{0.7f};
  std::atomic<bool> leveller_{false};
  std::atomic<bool> eqOn_{false};
  std::atomic<float> eqLow_{0}, eqBass_{0}, eqMid_{0}, eqTreble_{0}, eqHigh_{0};
};
