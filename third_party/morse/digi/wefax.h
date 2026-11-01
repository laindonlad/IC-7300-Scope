#pragma once
// WEFAX (HF weather fax, WMO): 120 lines a minute, FM between 1500Hz (black) and 2300Hz (white).
// A chart is a start tone (5s of black/white at 300Hz, or 675Hz for IOC 288), 30s of phasing
// (each line 25ms white, then black), the picture, then a stop tone
// (5s at 450Hz) and 10s of black. IOC 576 makes pi x 576 = 1809 pixels a line.
//
// The audio is moved to 0Hz around the centre, low-passed and decimated to 8kHz, and the phase
// step between samples is the frequency: the video. A tone (black and white in turn at 300, 450
// or 675Hz) makes lines exactly that far apart in the spectrum, around the signal's centre: they're
// found as the share of the band's power on such a comb, in 0.25s blocks. Not in the video, where
// room echo (a radio heard through the mics) and fading turn the tone into clicks: they change how
// strong the lines are, not where. Phasing lines are folded at the line length and
// the white pulse found in each: their positions give where lines start, and how they drift from
// line to line gives the slant (the two ends' clocks disagreeing), which is then corrected for that
// chart. Without phasing (startNow), lines start wherever, and setSlant is a trim set by hand.
// While idle, phasing heard without its start tone (tuned in late) starts a chart too, at IOC 576:
// WATCH_LINES pulses in a row, each where the last one was.
//
// Lines come out (takeLine) DELAY lines late, so that the stop tone's stripes can be dropped
// before anyone sees them. Plain C++ (tools/digi_test).
#include <algorithm>

#include "dsp.h"

class WefaxDemod {
 public:
  static constexpr float RATE = 16000, VRATE = 8000;  // audio in, video
  static constexpr float CENTRE = 1900, DEV = 400;  // black at -DEV, white at +DEV
  static constexpr float LPM = 120;
  static constexpr int MAX_W = 1809;  // IOC 576
  static constexpr int BINS = 1000;   // a phasing line folded into these
  static constexpr int PULSE = BINS / 20;  // the white pulse: 5% of a line
  static constexpr int TONE_N = 2000;      // a tone block: 0.25s of video
  static constexpr int TONE_FFT = 2048;    // its spectrum: 3.9Hz bins
  static constexpr float TONE_HALF = 800;  // Hz either side of the centre: the band the comb is a share of
  static constexpr float TONE_AT = 0.4f;   // the share on a tone's comb (pictures, noise: under 0.2)
  static constexpr int START_BLOCKS = 8, STOP_BLOCKS = 8;  // 2s of the 5s tones, one block missed at a time
  static constexpr float BIAS_MAX = 300;  // Hz: the most the start tone may move the centre
  static constexpr float PHASE_CONTRAST = 0.5f;  // pulse against the rest of the line, 0-1
  static constexpr int MIN_PHASE_LINES = 5;
  static constexpr int WATCH_LINES = 10;  // phasing with no start tone: pulses in a row to start on
  static constexpr float WATCH_STEP = 5;  // bins: the most such a pulse may move from the last
  static constexpr int FIT_LINES = 20;  // phasing lines to measure the slant from
  static constexpr float FIT_RMS = 3;   // bins: a fit scattered more than this isn't used
  static constexpr float STEP_MIN = 3;  // bins: the least jump of the pulses taken as a step (unstep)
  static constexpr float MAX_PPM = 500;
  static constexpr int MAX_PHASE = 128;
  static constexpr float NO_PHASING_SEC = 20;  // after a start tone, a picture starts anyway
  static constexpr int DELAY = 6;              // lines: 3s, more than a tone takes to find
  static constexpr int HIST = 12288;           // video kept while phasing: the lines after the last pulse
  static constexpr int RING = 16;
  static constexpr int MAX_LINES = 3000;  // 25 minutes: a stop tone missed
  static constexpr int SEGS = 12;         // pieces of a line matched against the last (lineShift)
  static constexpr int MAX_SHIFT = MAX_W / 90;  // px: the most a line may move from the last
  static constexpr float SEG_VAR = 1e-3f;  // a piece flatter than this (variance, 0-1) has nothing to match
  static constexpr float SEG_MATCH = 0.4f;  // the least correlation a piece's match counts with
  static constexpr float SHIFT_MIN = 0.5f;  // px: a line's move under this is left to the slant
  static constexpr int MAX_GUESSED = 6;     // lines in a row moved as the last few, unmeasured
  static constexpr float RATE_LINES = 32;   // the line length follows the moves over about this many lines
  static constexpr float RATE_MIN = 0.1f;   // px a line: an average move under this isn't followed
  static constexpr float TRACK_PPM = 2000;  // the most it may get from them
  enum State : uint8_t { IDLE, STARTING, PHASING, IMAGE };

  WefaxDemod() {
    fir_.lowpass(128, 750, RATE, 2);  // 1150-2650Hz: wider lets in noise that clicks in fades
    reset();
  }

  void reset() {
    state_ = IDLE;
    tune(CENTRE);
    fir_.clear();
    prev_ = {1, 0};
    clearTones();
    startRun_ = stopRun_ = run675_ = 0;
    startMiss_ = stopMiss_ = miss675_ = false;
    ioc_ = 576;
    width_ = MAX_W;
    lines_ = 0;
    w_ = r_ = flushed_ = imageStartW_ = 0;
    linePos_ = 0;
    spl_ = lineSamples(trimPpm_);
    measured_ = false;
    skipNext_ = false;
    watch();
  }

  void feed(const int16_t* s, int n) {
    for (int i = 0; i < n; i++) {
      float x = s[i] * (1.0f / 32768);
      Dsp::Cplx o = nco_.next(), z;
      if (!fir_.push({x * o.re, x * o.im}, z)) continue;
      Dsp::Cplx d = Dsp::mulConj(z, prev_);
      prev_ = z;
      zb_[toneN_] = z;
      video(atan2f(d.im, d.re) * (VRATE / (2 * Dsp::PI_F)));
    }
  }

  // Where the signal's centre (1900Hz from the transmitter) is heard.
  void tune(float hz) {
    centre_ = hz;
    nco_ = Dsp::Nco();
    nco_.set(hz, RATE);
    bias_ = 0;
  }
  // The line length trimmed by `ppm` (longer if positive): used from now, and for charts with no
  // phasing to measure it.
  void setSlant(float ppm) {
    trimPpm_ = ppm;
    spl_ = lineSamples(ppm);
    measured_ = false;
  }
  // A picture from here, at IOC 576, without waiting for a start tone.
  void startNow() {
    if (state_ == IMAGE) return;
    ioc_ = 576;
    spl_ = lineSamples(trimPpm_);
    measured_ = false;
    linePos_ = 0;
    beginImage(false);
  }
  void stopNow() {
    if (state_ != IDLE) endChart(false);
  }
  // The picture's left edge is `frac` of the way across: lines start there from now. The line
  // in progress is dropped.
  void align(float frac) {
    if (state_ != IMAGE) return;
    linePos_ -= frac * spl_;
    if (linePos_ < 0) linePos_ += spl_;
    clearLine();
    skipNext_ = true;
    haveRef_ = false;
    nMoved_ = unmatched_ = 0;
    drift_ = 0;
  }

  // The next line of a picture: its width (0 if none), with its chart's number.
  int takeLine(uint8_t* out, uint32_t& chart) {
    uint32_t end = readyEnd();
    if (w_ - r_ > RING) r_ = w_ - RING;  // the reader fell behind
    if (r_ >= end) return 0;
    const Line& l = ring_[r_++ % RING];
    memcpy(out, l.px, l.w);
    chart = l.chart;
    return l.w;
  }

  State state() const { return state_; }
  int ioc() const { return ioc_; }
  int width() const { return width_; }
  uint32_t chart() const { return chart_; }  // the latest picture's number, from 1
  uint32_t lines() const { return lines_; }  // its lines so far
  int phasingLines() const { return state_ == PHASING ? nPh_ : 0; }
  float ppm() const { return (float)((spl_ / lineSamples(0) - 1) * 1e6); }  // the slant in use
  bool slantMeasured() const { return measured_; }  // from the latest chart's phasing
  float centre() const { return centre_ + bias_; }
  // The last tone block's share of the band's power on the 300, 450 and 675Hz combs (TONE_AT finds one).
  const float* toneShares() const { return toneR_; }

 private:
  static constexpr float TONE_HZ[3] = {300, 450, 675};
  struct Line {
    uint32_t chart;
    uint16_t w;
    uint8_t px[MAX_W];
  };

  static double lineSamples(float ppm) { return VRATE * 60.0 / LPM * (1 + ppm * 1e-6); }

  void video(float hz) {
    float v = hz - bias_;
    v = v < -1000 ? -1000 : v > 1000 ? 1000 : v;
    tones(v);
    if (state_ == PHASING || state_ == IDLE) phasingSample(v);
    else if (state_ == IMAGE) imageSample(v);
  }

  // ---- Tones ----

  void clearTones() {
    sum_ = 0;
    toneN_ = 0;
  }

  void tones(float v) {
    sum_ += v;
    if (++toneN_ < TONE_N) return;
    float mean = sum_ / TONE_N;
    combs();
    clearTones();
    const float* r = toneR_;
    // a tone's comb holds most of the power, and more than the others' (675Hz's carrier line is on
    // the 300Hz comb too)
    int best = r[0] >= r[1] && r[0] >= r[2] ? 0 : r[1] >= r[2] ? 1 : 2;
    bool s300 = best == 0 && r[0] >= TONE_AT, s675 = best == 2 && r[2] >= TONE_AT;
    bool stop = best == 1 && r[1] >= TONE_AT;
    // a block missed (a fade) doesn't end a run, two do
    startRun_ = run(startRun_, startMiss_, s300 || s675);
    run675_ = run(run675_, miss675_, s675);
    stopRun_ = run(stopRun_, stopMiss_, stop);
    block(s300 || s675, mean);
  }

  static int run(int n, bool& missed, bool found) {
    if (found) {
      missed = false;
      return n + 1;
    }
    if (n && !missed) {
      missed = true;
      return n;
    }
    missed = false;
    return 0;
  }

  // toneR_ from the block's baseband (zb_): for each tone, the comb (lines that far apart, at the
  // best offset, each 3 bins wide) holding the most of the power within TONE_HALF of the centre,
  // both less the comb's strongest line: one line is a steady carrier (phasing is mostly black),
  // which any comb fits.
  void combs() {
    static constexpr int N = TONE_FFT, HALF = (int)(TONE_HALF * N / VRATE);
    static constexpr float BIN_HZ = VRATE / N;
    for (int i = 0; i < N; i++) {
      float w = i < TONE_N ? 0.5f - 0.5f * cosf(2 * Dsp::PI_F * i / TONE_N) : 0;
      fftBuf_[i] = i < TONE_N ? Dsp::Cplx{zb_[i].re * w, zb_[i].im * w} : Dsp::Cplx{0, 0};
    }
    fft_.run(fftBuf_);
    float* p = pow_ + HALF;  // p[-HALF..HALF]: bin 0 is the centre
    float total = 0;
    for (int k = -HALF; k <= HALF; k++) total += p[k] = Dsp::norm(fftBuf_[(k + N) % N]);
    for (int t = 0; t < 3; t++) {
      float step = TONE_HZ[t] / BIN_HZ, best = 0;
      for (int o = 0; o < (int)ceilf(step); o++) {
        float s = 0, top = 0;
        for (float f = o - floorf((HALF + o) / step) * step; f <= HALF; f += step) {
          int b = (int)lrintf(f);
          float l = 0;
          for (int j = b - 1; j <= b + 1; j++)
            if (j >= -HALF && j <= HALF) l += p[j];
          s += l;
          if (l > top) top = l;
        }
        float r = total - top > 0 ? (s - top) / (total - top) : 0;
        if (r > best) best = r;
      }
      toneR_[t] = best;
    }
  }

  // After each tone block.
  void block(bool startTone, float mean) {
    if (state_ == STARTING) {
      if (startTone) {  // the tone is black and white in turn: its mean is the centre
        toneSum_ += mean;
        toneBlocks_++;
        return;
      }
      if (startRun_) return;  // a block missed
      if (toneBlocks_ >= 2) {
        float b = toneSum_ / toneBlocks_;
        if (fabsf(bias_ + b) < BIAS_MAX) bias_ += b;
      }
      beginPhasing();
      return;
    }
    if (startRun_ >= START_BLOCKS) {  // a new chart, maybe without the last one's stop tone
      if (state_ != IDLE) endChart(true);
      state_ = STARTING;
      ioc_ = run675_ * 2 > startRun_ ? 288 : 576;
      toneSum_ = mean;
      toneBlocks_ = 1;
      return;
    }
    if (stopRun_ >= STOP_BLOCKS && state_ != IDLE) endChart(true);
  }

  // ---- Phasing ----

  void beginPhasing() {
    state_ = PHASING;
    spl_ = lineSamples(trimPpm_);
    measured_ = false;
    linePos_ = 0;
    watch();
  }

  // Phasing from scratch: after a start tone, or watched for while idle.
  void watch() {
    memset(binSum_, 0, sizeof(binSum_));
    memset(binCnt_, 0, sizeof(binCnt_));
    nPh_ = phMiss_ = 0;
    phLine_ = 0;
    vcount_ = validEnd_ = 0;
  }

  void phasingSample(float v) {
    hist_[vcount_++ % HIST] = v;
    int b = (int)(linePos_ * BINS / spl_);
    if (b >= BINS) b = BINS - 1;
    binSum_[b] += v;
    binCnt_[b]++;
    if (++linePos_ < spl_) return;
    linePos_ -= spl_;
    phasingLine();
    phLine_++;
    if (state_ == IDLE) {
      if (nPh_ == 0) vcount_ = validEnd_ = 0;  // nothing kept is needed
    } else if (nPh_ == 0 && phLine_ * spl_ > NO_PHASING_SEC * VRATE) {
      beginImage(false);
    }
  }

  void phasingLine() {
    float* g = grey_;
    float total = 0, last = 0;
    for (int b = 0; b < BINS; b++) {
      if (binCnt_[b]) last = Dsp::clamp01((binSum_[b] / binCnt_[b] + DEV) / (2 * DEV));
      g[b] = last;
      total += last;
    }
    memset(binSum_, 0, sizeof(binSum_));
    memset(binCnt_, 0, sizeof(binCnt_));
    // The whitest PULSE bins, round the line's end
    float s = 0, best = -1;
    int at = 0;
    for (int b = 0; b < PULSE; b++) s += g[b];
    for (int b = 0; b < BINS; b++) {
      if (s > best) best = s, at = b;
      s += g[(b + PULSE) % BINS] - g[b];
    }
    float pulse = best / PULSE, rest = (total - best) / (BINS - PULSE);
    if (pulse - rest < PHASE_CONTRAST) {
      if (state_ == IDLE || nPh_ < MIN_PHASE_LINES) nPh_ = 0;  // noise, or a picture
      else if (++phMiss_ >= 2) finishPhasing();
      return;
    }
    phMiss_ = 0;
    // Its start, finer: the centroid of the pulse (and its edges) less half a pulse
    float m = 0, w = 0;
    for (int k = -PULSE / 2; k < PULSE + PULSE / 2; k++) {
      float e = g[((at + k) % BINS + BINS) % BINS] - rest;
      if (e <= 0) continue;
      m += e * k;
      w += e;
    }
    float pos = at + (w > 0 ? m / w : PULSE / 2.0f) - PULSE / 2.0f;
    if (nPh_ > 0) {  // unwrapped: it moves a little a line, never most of one
      float p = phPos_[nPh_ - 1];
      while (pos - p > BINS / 2) pos -= BINS;
      while (pos - p < -BINS / 2) pos += BINS;
      if (state_ == IDLE && fabsf(pos - p) > WATCH_STEP) nPh_ = 0;  // out of step: count from this one
    }
    phPos_[nPh_] = pos;
    phAt_[nPh_] = phLine_;
    phEnd_[nPh_] = validEnd_ = vcount_;
    nPh_++;
    if (state_ == IDLE && nPh_ >= WATCH_LINES) {  // phasing, its start tone missed
      nPh_--;  // without the first pulse: it may have been cut short by tuning in
      memmove(phPos_, phPos_ + 1, nPh_ * sizeof(phPos_[0]));
      memmove(phAt_, phAt_ + 1, nPh_ * sizeof(phAt_[0]));
      memmove(phEnd_, phEnd_ + 1, nPh_ * sizeof(phEnd_[0]));
      state_ = PHASING;
      ioc_ = 576;
      measured_ = false;
    }
    if (nPh_ == MAX_PHASE) finishPhasing();
  }

  // Lines start where the pulses do. A good straight fit through them gives the slant too. The
  // picture starts in the line after the last pulse: what's been heard since is decoded again.
  void finishPhasing() {
    int n = nPh_, last = n - 1;
    unstep();
    double pos = phPos_[last] + phStep_[last];
    // Twice: the second time without pulses far off the first line. The last pulse's line can hold
    // the picture's start, and a white top widens that pulse: then the picture starts after the
    // pulse before.
    double mx = 0, my = 0, slope = 0, rms = 0, cut = 1e9;
    int used = n;
    for (int pass = 0; pass < 2; pass++) {
      double sx = 0, sy = 0;
      int k = 0;
      for (int i = 0; i < n; i++)
        if (fabs(phPos_[i] - (my + slope * (phAt_[i] - mx))) <= cut) sx += phAt_[i], sy += phPos_[i], k++;
      double fmx = sx / k, fmy = sy / k, sxx = 0, sxy = 0;
      for (int i = 0; i < n; i++)
        if (fabs(phPos_[i] - (my + slope * (phAt_[i] - mx))) <= cut)
          sxx += (phAt_[i] - fmx) * (phAt_[i] - fmx), sxy += (phAt_[i] - fmx) * (phPos_[i] - fmy);
      double fs = sxx > 0 ? sxy / sxx : 0, e2 = 0;
      for (int i = 0; i < n; i++)
        if (fabs(phPos_[i] - (my + slope * (phAt_[i] - mx))) <= cut) {
          double e = phPos_[i] - (fmy + fs * (phAt_[i] - fmx));
          e2 += e * e;
        }
      mx = fmx, my = fmy, slope = fs, rms = sqrt(e2 / k), used = k;
      cut = 3 * rms + 1;
    }
    double old = spl_;
    if (used >= FIT_LINES && rms < FIT_RMS) {
      while (last > 0 && fabs(phPos_[last] - (my + slope * (phAt_[last] - mx))) > cut) last--;
      pos = my + slope * (phAt_[last] + 1 - mx) + phStep_[last];
      double s = spl_ * (1 + slope / BINS);
      if (fabs(s / lineSamples(0) - 1) * 1e6 < MAX_PPM) {
        spl_ = s;
        measured_ = true;
      }
    } else if (n >= 3) {
      // Too few, or too scattered, to trust a slope (a web SDR's speed swinging makes the pulses
      // zigzag): the median of the last 8. One pulse alone could be anywhere in the zigzag, and the
      // typical move is then one way or the other of it, so they aren't moved to the latest's line.
      int k = 0;
      for (int i = last; i >= 0 && k < 8; i--) phSort_[k++] = phPos_[i];
      std::nth_element(phSort_, phSort_ + k / 2, phSort_ + k);
      pos = phSort_[k / 2] + phStep_[last];
    }
    // The picture's first line starts `off` samples into that line. A line is 25ms of margin (the
    // pulse) and then the picture (BoM's technical page), and stations that fill the margin (Northwood)
    // fill it with the line's right end: so a line starts where the pulse ends.
    double frac = fmod(pos + PULSE, BINS) / BINS;
    if (frac < 0) frac += 1;
    double off = frac * old;
    beginImage(true);
    linePos_ = spl_ - off;
    uint32_t from = vcount_ - phEnd_[last] > HIST ? vcount_ - HIST : phEnd_[last];
    for (uint32_t i = from; i < vcount_; i++) imageSample(hist_[i % HIST]);
  }

  // A web SDR keeps its buffer by now and then dropping or repeating a few ms of audio (a KiwiSDR's
  // steps were ~9px, 5 bins): the pulses jump sideways and stay there, and a straight fit through
  // them is too scattered to trust. A pulse that moves from the last by STEP_MIN bins (or 4 sd of
  // the moves, in noise) more than they typically do, with the next moving typically from it, is a
  // step. They're taken out of phPos_ (the fit's), and phStep_ puts them back for the picture's
  // start. A lone stray pulse moves back again, so it isn't one: the fit drops it.
  void unstep() {
    int n = nPh_;
    for (int i = 0; i < n; i++) phStep_[i] = 0;
    if (n < 3) return;
    float* scratch = phSort_;
    for (int i = 1; i < n; i++) {
      phMove_[i] = (phPos_[i] - phPos_[i - 1]) / (int)(phAt_[i] - phAt_[i - 1]);
      scratch[i - 1] = phMove_[i];
    }
    int m = n - 1;
    std::nth_element(scratch, scratch + m / 2, scratch + m);
    float typ = scratch[m / 2];
    for (int i = 0; i < m; i++) scratch[i] = fabsf(scratch[i] - typ);
    std::nth_element(scratch, scratch + m / 2, scratch + m);
    float at = fmaxf(STEP_MIN, 4 * 1.4826f * scratch[m / 2]);
    // Off the typical move, in bins. A step in mid-line leaves that line's pulse between the two
    // places, so a step may take two lines (moving the same way).
    auto off = [&](int i) { return phPos_[i] - phPos_[i - 1] - typ * (int)(phAt_[i] - phAt_[i - 1]); };
    float sum = 0;
    for (int i = 1; i < n; i++) {
      float e = off(i);
      int j = i + 1;
      if (fabsf(e) >= at / 2 && j < n) {
        float total = e;
        if (j + 1 < n && fabsf(off(j)) >= at / 2 && off(j) * e > 0) total += off(j++);
        if (fabsf(total) >= at && fabsf(off(j)) < at / 2) {
          for (; i < j; i++) phStep_[i] = sum += off(i);
          i--;
          continue;
        }
      }
      phStep_[i] = sum;
    }
    for (int i = 0; i < n; i++) phPos_[i] -= phStep_[i];  }

  // ---- The picture ----

  void beginImage(bool partial) {
    state_ = IMAGE;
    chart_++;
    lines_ = 0;
    width_ = ioc_ == 288 ? 905 : MAX_W;
    imageStartW_ = w_;
    clearLine();
    haveRef_ = false;
    nMoved_ = unmatched_ = 0;
    drift_ = 0;
    skipNext_ = partial;  // what's left of the line in progress isn't a whole one
  }

  void clearLine() {
    memset(pxSum_, 0, sizeof(pxSum_));
    memset(pxCnt_, 0, sizeof(pxCnt_));
  }

  void imageSample(float v) {
    int x = (int)(linePos_ * width_ / spl_);
    if (x >= width_) x = width_ - 1;
    if (x < 0) x = 0;  // lineShift moved the line's start later
    pxSum_[x] += v;
    pxCnt_[x]++;
    if (++linePos_ < spl_) return;
    linePos_ -= spl_;
    emitLine();
  }

  void emitLine() {
    if (skipNext_) {
      skipNext_ = false;
      clearLine();
      return;
    }
    Line& l = ring_[w_ % RING];
    l.chart = chart_;
    l.w = width_;
    float last = -DEV;
    for (int x = 0; x < width_; x++) {
      if (pxCnt_[x]) last = pxSum_[x] / pxCnt_[x];
      cur_[x] = Dsp::clamp01((last + DEV) / (2 * DEV));
    }
    clearLine();
    // A web SDR's audio speeds up and slows down, or skips, to keep its buffer, and the lines move
    // sideways (Twente's WebSDR swung ±0.25% every few lines, a KiwiSDR skipped ~10px every ~25
    // lines). This line goes back by what it moved from the last, and the next starts that much
    // later or earlier. Wrapped round, as the line's end is the margin. A line that can't be
    // measured (a black rule, static) moves as the last few did, for a while. Under SHIFT_MIN is
    // left to the slant: the measures' own error would add up down the chart.
    float d = 0;
    Match how = haveRef_ ? lineShift(d) : FLAT;
    if (how == MATCHED) {
      // The speed: a drift too slow to move a line SHIFT_MIN (a KiwiSDR's, after the phasing) is
      // followed from the moves' average over about RATE_LINES lines, each at most SHIFT_MIN either
      // way (a skip isn't the speed). Only what's over RATE_MIN: the chart's own sloping lines
      // bias the measures by up to ~0.05px a line, and followed that leans the picture.
      drift_ += (fmaxf(-SHIFT_MIN, fminf(SHIFT_MIN, d)) - drift_) / RATE_LINES;
      float over = fabsf(drift_) > RATE_MIN ? drift_ - copysignf(RATE_MIN, drift_) : 0;
      double nominal = lineSamples(0);
      spl_ += over / RATE_LINES * spl_ / width_;
      spl_ = fmax(nominal * (1 - TRACK_PPM * 1e-6), fmin(nominal * (1 + TRACK_PPM * 1e-6), spl_));
      if (fabsf(d) < SHIFT_MIN) d = 0;
      moved_[nMoved_++ % 3] = d;
      unmatched_ = 0;
    } else if (++unmatched_ <= MAX_GUESSED && nMoved_ >= 3) {
      float p = moved_[0], q = moved_[1], r = moved_[2];
      d = fmaxf(fminf(p, q), fminf(fmaxf(p, q), r));  // the median
    }
    linePos_ -= d * spl_ / width_;
    // A guessed line may be out, and matching the next against it would keep that. The next is
    // matched against the last measured, unless that's long gone (the picture changed).
    bool keep = how == MATCHED || !haveRef_ || (how == UNSURE && unmatched_ > MAX_GUESSED);
    for (int x = 0; x < width_; x++) {
      float src = x + d;
      if (src < 0) src += width_;
      if (src >= width_) src -= width_;
      int i = (int)src, j = i + 1 < width_ ? i + 1 : 0;
      float f = src - i, v = cur_[i] + (cur_[j] - cur_[i]) * f;
      if (keep) ref_[x] = v;
      l.px[x] = (uint8_t)(v * 255 + 0.5f);
    }
    haveRef_ = true;
    w_++;
    lines_++;
    if (lines_ >= MAX_LINES) endChart(false);
  }

  // How far this line (cur_) has moved from the last (ref_, as it went out), in px: right if
  // positive. Each of SEGS pieces finds where the last line matches it best, within ±1.1% of a
  // line. A sloping line in the chart moves only the pieces it's in, so the median of the pieces
  // that match well (at least 3) is the timing's. If the speed changed mid-line, the pieces' moves
  // ramp across it, and the median is their middle (a straight fit through them was noisier, and
  // leaned charts in noise). UNSURE: under 3 match (static). FLAT: under 3 have anything to match
  // (a blank stretch, a black rule).
  enum Match { MATCHED, UNSURE, FLAT };
  Match lineShift(float& d) {
    const int W = width_, M = W / 90;
    float got[SEGS];
    int n = 0, busy = 0;
    for (int s = 0; s < SEGS; s++) {
      int x0 = s * W / SEGS, x1 = (s + 1) * W / SEGS, len = x1 - x0;
      float my = 0, yy = 0;
      for (int x = x0; x < x1; x++) my += cur_[x];
      my /= len;
      for (int x = x0; x < x1; x++) yy += (cur_[x] - my) * (cur_[x] - my);
      if (yy < SEG_VAR * len) continue;
      busy++;
      float c[2 * MAX_SHIFT + 1];
      int best = 0;
      for (int k = -M; k <= M; k++) {
        float sp = 0, spp = 0, spy = 0;
        for (int x = x0; x < x1; x++) {
          int i = x - k;
          if (i < 0) i += W;
          else if (i >= W) i -= W;
          float p = ref_[i];
          sp += p, spp += p * p, spy += p * (cur_[x] - my);
        }
        float vp = spp - sp * sp / len;
        c[k + M] = vp > 0 ? spy / sqrtf(vp * yy) : 0;
        if (c[k + M] > c[best]) best = k + M;
      }
      if (c[best] < SEG_MATCH || best == 0 || best == 2 * M) continue;
      float den = c[best - 1] - 2 * c[best] + c[best + 1];
      got[n++] = best - M + (den < 0 ? 0.5f * (c[best - 1] - c[best + 1]) / den : 0);
    }
    if (busy < 3) return FLAT;
    if (n < 3) return UNSURE;
    // The middle half's mean: the median alone (12 pieces have no middle one) leaned the way the
    // chart's sloping lines do, and followed by the speed that leaned the picture
    for (int i = 1; i < n; i++)  // (std::sort here warns of bounds GCC can't see it keeps to)
      for (int j = i; j > 0 && got[j] < got[j - 1]; j--) std::swap(got[j], got[j - 1]);
    int lo = n / 4, hi = n - n / 4;
    d = 0;
    for (int i = lo; i < hi; i++) d += got[i];
    d /= hi - lo;
    return MATCHED;
  }

  // The picture's done. With `tone`, a tone was just found: the lines it's in go.
  void endChart(bool tone) {
    if (state_ == IMAGE && tone) {
      // it started up to half a block before the blocks it was found in
      double since = (STOP_BLOCKS + 0.5) * TONE_N - linePos_;
      uint32_t drop = since > 0 ? (uint32_t)ceil(since / spl_) : 0;
      uint32_t keep = w_ - imageStartW_ > drop ? w_ - drop : imageStartW_;
      if (keep < r_) keep = r_;
      lines_ -= w_ - keep;
      w_ = keep;
    }
    flushed_ = w_;
    state_ = IDLE;
    startRun_ = stopRun_ = 0;
    startMiss_ = stopMiss_ = false;
    watch();
  }

  uint32_t readyEnd() const {
    uint32_t e = state_ == IMAGE && w_ > DELAY ? w_ - DELAY : state_ == IMAGE ? 0 : w_;
    return e > flushed_ ? e : flushed_;
  }

  State state_ = IDLE;
  Dsp::Nco nco_;
  Dsp::Fir<128> fir_;
  Dsp::Cplx prev_ = {1, 0};
  float centre_ = CENTRE, bias_ = 0;
  // tones
  Dsp::Cplx zb_[TONE_N];
  Dsp::Cplx fftBuf_[TONE_FFT];
  Dsp::Fft<TONE_FFT> fft_;
  float pow_[2 * (int)(TONE_HALF * TONE_FFT / VRATE) + 1];
  float sum_ = 0, toneR_[3] = {};
  int toneN_ = 0, startRun_ = 0, stopRun_ = 0, run675_ = 0;
  bool startMiss_ = false, stopMiss_ = false, miss675_ = false;
  float toneSum_ = 0;
  int toneBlocks_ = 0;
  // lines
  int ioc_ = 576, width_ = MAX_W;
  float trimPpm_ = 0;
  double spl_ = 4000, linePos_ = 0;
  bool measured_ = false, skipNext_ = false;
  // phasing
  float binSum_[BINS], grey_[BINS];
  uint16_t binCnt_[BINS];
  float phPos_[MAX_PHASE], phStep_[MAX_PHASE], phMove_[MAX_PHASE], phSort_[MAX_PHASE];  // phStep_: steps taken out by unstep
  uint32_t phAt_[MAX_PHASE], phEnd_[MAX_PHASE];  // each pulse's line, and where it ended (vcount_)
  int nPh_ = 0, phMiss_ = 0;
  uint32_t phLine_ = 0;
  float hist_[HIST];
  uint32_t vcount_ = 0, validEnd_ = 0;
  // the picture
  float pxSum_[MAX_W];
  uint8_t pxCnt_[MAX_W];
  float cur_[MAX_W], ref_[MAX_W];  // the line being finished, 0-1, and the last as it went out
  bool haveRef_ = false;
  float moved_[3] = {};  // the last 3 lines' measured moves (lineShift)
  float drift_ = 0;      // px a line: their average, for the speed
  int nMoved_ = 0, unmatched_ = 0;  // unmatched_: lines in a row not measured
  uint32_t chart_ = 0, lines_ = 0;
  Line ring_[RING];
  uint32_t w_ = 0, r_ = 0, flushed_ = 0, imageStartW_ = 0;
};
