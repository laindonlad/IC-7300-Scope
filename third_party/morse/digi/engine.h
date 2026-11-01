#pragma once
// The multi-mode decoder: audio at 16kHz in, a waterfall, the signals found in it with their
// modes, and the text of the one it's locked to. Plain C++, no Arduino (tools/digi_test).
//
// 1. Spectrum: a 2048-point FFT (7.8Hz bins) every 512 samples (32ms). Its rows make the
//    waterfall, and an average of ~1s finds signals: peaks standing MIN_DB over the noise floor
//    (the median bin), and not MASK_DB under a stronger one within MASK_HZ (its keying's
//    sidebands). Peaks closer than MERGE_HZ are one signal (a PSK31 idle's two lines);
//    a run of bins over the floor is one too, at its centre (PSK with data is a hump). Two
//    peaks a standard RTTY shift apart, of similar strength, are a pair: a possible RTTY signal.
// 2. Channels: each candidate gets a channel (up to MAX_CH), which moves it to 0Hz at 2kHz and
//    runs every demodulator that could be it: PSK31/63/125 and CW on a single, RTTY at 45.45,
//    50 and 75 baud (both polarities) on a pair, and NAVTEX (both polarities) on a 170Hz pair. They all decode all the time and each scores
//    itself (psk.h, rtty.h, cw.h): how much what it gets makes sense.
// 3. Deciding, every 8 hops: a channel's best variant is its mode. Where channels overlap, the
//    best-scoring one claims the band, except that an RTTY pair has the singles on and between
//    its tones (one tone alone looks like CW) while it's new or scoring. With nothing locked: a best of LOCK_AT or more, clear by
//    MARGIN of anything else on the same signal, is locked to; otherwise everything over
//    MAYBE_AT is offered as a choice. Locked, its text is what's shown; the lock goes when it
//    has scored under DROP_AT for LOST_SEC.
// With a family chosen (setFamily), only its variants are offered. Pairs are still followed with
// CW or PSK chosen, to keep CW off their tones.
#include <stdio.h>
#include <stdlib.h>

#include <new>

#include "cw.h"
#include "navtex.h"
#include "psk.h"
#include "rtty.h"

namespace Digi {
// Stored (Store::digiFamily): new families go on the end.
// WEFAX, FT8 and WSPR aren't the engine's: with one chosen, the engine only makes the waterfall
// (digital.cpp runs wefax.h or slots.h beside it).
enum Family : uint8_t { AUTO, CW, RTTY, PSK, NAVTEX, WEFAX, FT8, WSPR, FAMILY_COUNT };
inline const char* familyName(Family f) {
  static const char* const N[] = {"Auto", "CW", "RTTY", "PSK", "NAVTEX", "WEFAX", "FT8", "WSPR"};
  return f < FAMILY_COUNT ? N[f] : "?";
}
inline bool waterfallOnly(Family f) { return f == WEFAX || f == FT8 || f == WSPR; }

constexpr float RTTY_BAUDS[3] = {45.45f, 50, 75};
constexpr int PSK_MULTS[3] = {1, 2, 4};

// A signal's mode as found. sub: PSK 0-2 (31/63/125); RTTY the baud (0-2) + 3 x polarity;
// NAVTEX the polarity.
struct Option {
  uint32_t ch = 0;  // the channel it's on
  Family fam = AUTO;
  uint8_t sub = 0;
  float hz = 0, shift = 0;
  float score = 0;
  float level = 0;  // its peak bin, dB over the noise floor (for ranking signals)
  float snr = 0;    // dB in 2500Hz, as WSJT-X gives it (Engine::snrOf)
  bool same(const Option& o) const { return ch == o.ch && fam == o.fam && sub == o.sub; }
};

constexpr float RTTY_SHIFTS[] = {170, 200, 425, 450, 850};

// The nearest standard shift, if `hz` is within 10% of it, else `hz`. Nearest, not first:
// 450 is within 10% of 425.
inline float standardShift(float hz) {
  float best = hz;
  for (float s : RTTY_SHIFTS)
    if (fabsf(hz - s) < s * 0.1f && (best == hz || fabsf(hz - s) < fabsf(hz - best))) best = s;
  return best;
}

// "PSK31", "RTTY 45/170", "CW".
inline void label(const Option& o, char* b, size_t n) {
  switch (o.fam) {
    case PSK: snprintf(b, n, "PSK%d", o.sub == 0 ? 31 : o.sub == 1 ? 63 : 125); break;
    case RTTY: snprintf(b, n, "RTTY %d/%d", (int)RTTY_BAUDS[o.sub % 3], (int)(standardShift(o.shift) + 0.5f)); break;
    case CW: snprintf(b, n, "CW"); break;
    case NAVTEX: snprintf(b, n, "NAVTEX"); break;
    case WEFAX:
    case FT8:
    case WSPR: snprintf(b, n, "%s", familyName(o.fam)); break;
    default: snprintf(b, n, "?"); break;
  }
}

// Text to show: a view of the locked demodulator's (MorseDecoder's for CW).
struct TextView {
  const char* text = "";
  uint32_t scrolled = 0, seq = 0;
};

class Engine {
 public:
  static constexpr float RATE = 16000;
  static constexpr int FFT_N = 2048, HOP = 512;
  static constexpr float BIN_HZ = RATE / FFT_N;
  static constexpr int SPEC_BINS = 448;  // to 3.5kHz
  static constexpr float LO_HZ = 200, HI_HZ = 3200;  // where signals are looked for
  static constexpr int WF_W = 640;  // waterfall pixels across, WF_HZ each from 0Hz
  static constexpr float WF_HZ = 5;
  static constexpr int WF_ROWS = 8;  // waterfall rows queued for the screen
  // A channel is ~15KB, and the board's internal RAM is what runs out: 5. The PC receiver
  // (tools/rx) has room for more, and a busy band needs them (DIGI_MAX_CH).
#ifdef DIGI_MAX_CH
  static constexpr int MAX_CH = DIGI_MAX_CH;
#else
  static constexpr int MAX_CH = 5;
#endif
  static constexpr int MAX_OPTIONS = 3, MAX_SIGNALS = MAX_CH;
  static constexpr float HOPS_PER_SEC = RATE / HOP;
  enum State : uint8_t { SEARCHING, CHOOSING, LOCKED };

  // `fast` gets the channels (the per-sample work), `big` the FFT's buffers.
  explicit Engine(void* (*fast)(size_t) = malloc, void* (*big)(size_t) = malloc) {
    fftWork_ = (Dsp::Cplx*)big(sizeof(Dsp::Cplx) * FFT_N);
    fft_ = new (big(sizeof(Dsp::Fft<FFT_N>))) Dsp::Fft<FFT_N>();
    win_ = (float*)big(sizeof(float) * FFT_N);
    in_ = (float*)big(sizeof(float) * FFT_N);
    for (int i = 0; i < FFT_N; i++) win_[i] = 0.5f - 0.5f * cosf(2 * Dsp::PI_F * i / FFT_N);
    for (Channel*& c : ch_) c = new (fast(sizeof(Channel))) Channel();
    PskDemod::lookup(0);  // builds its table now: with a runner, two cores feed channels
    reset();
  }

  void reset() {
    memset(in_, 0, sizeof(float) * FFT_N);
    inPos_ = 0;
    sinceHop_ = 0;
    hop_ = 0;
    for (float& a : avg_) a = 0;
    floor_ = 1e-12f;
    floorKnown_ = false;
    rowMax_ = 0;
    rowsW_ = rowsR_ = 0;
    for (Channel* c : ch_) c->free();
    for (Tried& t : tried_) t.until = 0;
    nextId_ = 1;
    state_ = SEARCHING;
    lockedId_ = 0;
    lockId_++;
    lowHops_ = 0;
    holdUntil_ = 0;
    pendingUntil_ = 0;
    nOptions_ = nSignals_ = 0;
  }

  // The audio up to each hop goes to every channel, a channel at a time (they're independent,
  // so that's the same as a sample at a time), then the hop's spectrum, which may move them.
  void feed(const int16_t* s, int n) {
    while (n > 0) {
      int take = HOP - sinceHop_ < n ? HOP - sinceHop_ : n;
      for (int i = 0; i < take; i++) {
        float x = s[i] * (1.0f / 32768);
        seg_[i] = x;
        in_[inPos_] = x;
        if (++inPos_ == FFT_N) inPos_ = 0;
      }
      segN_ = take;
      if (runner_) {
        runner_(runnerCtx_);
      } else {
        for (int c = 0; c < MAX_CH; c++) feedChannel(c);
      }
      s += take;
      n -= take;
      sinceHop_ += take;
      if (sinceHop_ == HOP) {
        sinceHop_ = 0;
        spectrum();
      }
    }
  }
  // Instead of feeding the channels itself, feed() calls `run`, which must have feedChannel(c)
  // called once for each c below MAX_CH (on any core, any order) before it returns.
  // An FFT of FFT_N points to use instead of Dsp::Fft (the board's is esp-dsp's), in place,
  // natural order out. The buffer it's given comes from `big` and is 16-byte aligned if that is.
  void setFft(void (*run)(Dsp::Cplx*)) { fftRun_ = run; }
  void setRunner(void (*run)(void*), void* ctx) {
    runner_ = run;
    runnerCtx_ = ctx;
  }
  // True if it was in use.
  bool feedChannel(int c) {
    Channel* ch = ch_[c];
    if (!ch->used) return false;
    for (int i = 0; i < segN_; i++) ch->feed(seg_[i]);
    return true;
  }

  // AUTO, or only one family.
  void setFamily(Family f) {
    if (f == family_) return;
    family_ = f;
    for (Channel* c : ch_) c->free();
    unlockNow();
    pendingUntil_ = 0;
    nOptions_ = nSignals_ = 0;
  }
  Family family() const { return family_; }

  // A tap on the waterfall: decode what's at `hz`.
  void tune(float hz) {
    unlockNow();
    pendingHz_ = hz;
    pendingUntil_ = hop_ + (uint32_t)(PENDING_SEC * HOPS_PER_SEC);
    if (!spanning(hz)) {
      bool pair = family_ == RTTY || family_ == NAVTEX;
      float shift = 170, centre = hz;
      if (pair) nearestPair(hz, centre, shift);
      Channel* c = place(pair, centre, shift, 0);
      if (c) c->pinned = true;
    }
    decide();
  }

  // One of options(): lock to it.
  void choose(const Option& o) {
    Channel* c = byId(o.ch);
    if (!c) return;
    lock(o);
  }

  // Lets go of the lock, and doesn't lock again on its own for a few seconds (time to choose).
  void unlock() {
    unlockNow();
    holdUntil_ = hop_ + (uint32_t)(HOLD_SEC * HOPS_PER_SEC);
    decide();
  }

  State state() const { return state_; }
  // What it's locked to (state() LOCKED), its frequency kept up to date.
  const Option& locked() const { return locked_; }
  // Changes with every lock, so a reader knows the text is a new stream.
  uint32_t lockId() const { return lockId_; }
  int options(const Option*& o) const {
    o = options_;
    return nOptions_;
  }
  // Every signal found (its best guess, fam AUTO if none fits yet), for the waterfall.
  int signals(const Option*& o) const {
    o = signals_;
    return nSignals_;
  }
  TextView text() const {
    TextView v;
    const Channel* c = state_ == LOCKED ? byId(lockedId_) : nullptr;
    if (c) c->text(locked_, v);
    return v;
  }
  bool keyDown() const {
    const Channel* c = state_ == LOCKED ? byId(lockedId_) : nullptr;
    return c && locked_.fam == CW && c->single()->cw.keyDown();
  }
  // The locked CW's speed, else 0.
  float lockedWpm() const {
    const Channel* c = state_ == LOCKED ? byId(lockedId_) : nullptr;
    return c && locked_.fam == CW ? c->single()->cw.wpm() : 0;
  }
  float noiseFloorDb() const { return 10 * log10f(floor_ + 1e-20f); }

  // The next waterfall row (WF_W values, 0 the noise floor to 255 strong), if there's one.
  bool takeRow(uint8_t* row) {
    if (rowsR_ == rowsW_) return false;
    memcpy(row, rows_[rowsR_ % WF_ROWS], WF_W);
    rowsR_++;
    return true;
  }

  float hzForPixel(int px) const { return px * WF_HZ; }

  // Every channel decodes all the time, not only the locked one (streams.h follows them all).
  // `o` (a variant on a channel) as it is now: its tracked frequency, score and level. False
  // once the channel has gone.
  bool follow(Option& o) const {
    const Channel* c = byId(o.ch);
    if (!c) return false;
    o.score = c->score(o.fam, o.sub);
    o.hz = c->trackedHz(o);
    o.level = c->level;
    o.snr = snrOf(o);
    return true;
  }
  // A variant's SNR in 2500Hz, from the averaged spectrum: the power in the bins it takes, less
  // the noise in them, over the noise in 2500Hz. Summing bins needs no correction for the
  // window, which spreads the signal and the noise alike, and an FSK pair's two tones are both
  // counted. It's the average power, so CW (keyed ~45% of the time) reads ~3.5dB under its
  // key-down SNR, and PSK ~1dB under its peak's. The noise is the median of the bins either
  // side of the signal, not the band's: a receiver's filter or a sloping floor makes those
  // differ. Its bins start well clear of it (SNR_GAP): hard-keyed FSK's sidebands are still
  // well over the noise 100Hz from the tones at 20dB, and made it read 4dB low at 20dB, 7dB at 30.
  float snrOf(const Option& o) const {
    const Channel* c = byId(o.ch);
    if (!c || !floorKnown_) return 0;
    float lo[2], hi[2];
    int nb = 1;
    if (c->isPair) {
      float half = (o.fam == RTTY ? RTTY_BAUDS[o.sub % 3] : o.fam == NAVTEX ? 100 : 50) + SNR_EDGE_HZ;
      for (int i = 0; i < 2; i++) {
        float tone = c->hz + (i ? 1 : -1) * c->shift / 2;
        lo[i] = tone - half;
        hi[i] = tone + half;
      }
      nb = 2;
    } else {
      float half = (o.fam == PSK ? 31.25f * PSK_MULTS[o.sub] : 25) + SNR_EDGE_HZ;
      float hz = c->trackedHz(o);
      lo[0] = hz - half;
      hi[0] = hz + half;
    }
    int kLo = (int)ceilf(lo[0] / BIN_HZ), kHi = (int)(hi[nb - 1] / BIN_HZ);
    int bandLo = (int)(LO_HZ / BIN_HZ), bandHi = (int)(HI_HZ / BIN_HZ);
    // Each side's median, and the quieter side's: a neighbour on one side doesn't count
    float below[SNR_RING], above[SNR_RING];
    int nBelow = 0, nAbove = 0;
    for (int k = kLo - SNR_GAP - SNR_RING; k < kLo - SNR_GAP; k++)
      if (k >= bandLo) below[nBelow++] = avg_[k];
    for (int k = kHi + SNR_GAP + 1; k <= kHi + SNR_GAP + SNR_RING; k++)
      if (k <= bandHi) above[nAbove++] = avg_[k];
    float noise = 1e30f;
    if (nBelow >= SNR_RING / 2) noise = median(below, nBelow);
    if (nAbove >= SNR_RING / 2) noise = fminf(noise, median(above, nAbove));
    if (noise > 1e29f) noise = floor_;  // squeezed against both ends of the band
    if (noise < 1e-20f) noise = 1e-20f;
    float sum = 0;
    int n = 0;
    for (int k = kLo < 0 ? 0 : kLo; k <= kHi && k < SPEC_BINS; k++) {
      float f = k * BIN_HZ;
      if ((f >= lo[0] && f <= hi[0]) || (nb == 2 && f >= lo[1] && f <= hi[1])) {
        sum += avg_[k];
        n++;
      }
    }
    float sig = sum - n * noise;
    float db = sig > 0 ? 10 * log10f(sig / (noise * (2500 / BIN_HZ))) : -30;
    return db < -30 ? -30 : db;
  }
  // The best variant on channel `ch` now.
  bool bestOf(uint32_t ch, Option& a) const {
    const Channel* c = byId(ch);
    if (!c) return false;
    Option b;
    c->best(a, b);
    a.hz = c->trackedHz(a);
    return true;
  }
  // A variant's text (empty once its channel has gone).
  TextView text(const Option& o) const {
    TextView v;
    if (const Channel* c = byId(o.ch)) c->text(o, v);
    return v;
  }

  static constexpr float LOCK_AT = 0.6f, MAYBE_AT = 0.3f, MARGIN = 0.25f, DROP_AT = 0.2f;
  static constexpr float LOST_SEC = 6;

 private:
  static constexpr float MIN_DB = 6, MERGE_HZ = 35, PAIR_TOL = 0.1f, PAIR_DB = 10;
  static constexpr float MASK_HZ = 250, MASK_DB = 12;
  static constexpr float MIN_AGE_SEC = 1.5f, UNSEEN_SEC = 4, PINNED_SEC = 15;
  static constexpr float HOLD_SEC = 5, PENDING_SEC = 6;
  static constexpr float PAIR_PROVE_SEC = 6;  // an RTTY pair's time to show it's RTTY
  static constexpr float YOUNG_TONE_HZ = 60;  // a young pair's claim either side of each tone
  static constexpr float TURNS_SEC = 1.5f;    // a pair's age when its tones' overlap means something
  // A channel that's found nothing in TRIAL_SEC (a pair: PAIR_PROVE_SEC) makes way for a
  // candidate without one, and isn't tried again for RETRY_SEC: with more candidates than
  // channels, they take turns, rather than the first five keeping them.
  static constexpr float TRIAL_SEC = 4, RETRY_SEC = 20;
  static constexpr float AVG_K = 1.0f / 32;  // the averaged spectrum: ~1s
  static constexpr int DECIDE_EVERY = 8;     // hops: ~0.25s
  static constexpr int ROW_EVERY = 2;        // hops a waterfall row
  // snrOf: the bins taken past the signal's own band (the window's spread), and the noise's
  // bins either side (312Hz), after a gap (195Hz). digi_test's engine_test tuned them.
  static constexpr float SNR_EDGE_HZ = 2 * BIN_HZ;
  static constexpr int SNR_GAP = 25, SNR_RING = 40;

  struct Single {
    PskDemod psk[3];
    CwDemod cw;
  };
  struct Pair {
    RttyDemod rtty[3];
    NavtexDemod navtex;  // at 170Hz only
    bool hasNavtex;
  };

  struct Channel {
    bool used = false, pinned = false, isPair = false;
    uint32_t id = 0, born = 0, seen = 0;
    uint32_t good = 0;  // hop it last scored MAYBE_AT (0: never)
    mutable bool mixed = false;  // a pair whose tones were found not to take turns (inPair)
    float hz = 0, shift = 0, level = 0;
    Family only = AUTO;
    Dsp::Downconverter dc;
    alignas(8) unsigned char mem[sizeof(Single) > sizeof(Pair) ? sizeof(Single) : sizeof(Pair)];

    Single* single() { return (Single*)mem; }
    const Single* single() const { return (const Single*)mem; }
    Pair* pair() { return (Pair*)mem; }
    const Pair* pair() const { return (const Pair*)mem; }

    void start(uint32_t newId, bool p, float centre, float sh, Family fam, uint32_t now) {
      free();
      used = true;
      pinned = false;
      isPair = p;
      id = newId;
      born = seen = now;
      good = 0;
      mixed = false;
      hz = centre;
      shift = sh;
      only = fam;
      dc.begin(centre);
      if (p) {
        Pair* q = new (mem) Pair();
        for (int i = 0; i < 3; i++) q->rtty[i].begin(sh, RTTY_BAUDS[i]);
        q->hasNavtex = standardShift(sh) == NavtexDemod::SHIFT;
        if (q->hasNavtex) q->navtex.begin(sh);
      } else {
        Single* q = new (mem) Single();
        for (int i = 0; i < 3; i++) q->psk[i].begin(PSK_MULTS[i]);
      }
    }
    void free() {
      if (!used) return;
      if (isPair) pair()->~Pair();
      else single()->~Single();
      used = false;
    }
    void feed(float x) {
      Dsp::Cplx b;
      if (!dc.push(x, b)) return;
      if (isPair) {
        Pair* q = pair();
        if (only != NAVTEX)
          for (RttyDemod& r : q->rtty) r.feed(b);
        if (q->hasNavtex) q->navtex.feed(b);  // always: it overrules RTTY (score())
        return;
      }
      Single* q = single();
      if (only != CW)
        for (PskDemod& p : q->psk) p.feed(b);
      if (only != PSK) q->cw.feed(b);
    }
    float score(Family f, uint8_t sub) const {
      switch (f) {
        case PSK: return isPair ? 0 : single()->psk[sub].score();
        case CW: return isPair ? 0 : single()->cw.score();
        case RTTY: return isPair && !navtexFound() ? pair()->rtty[sub % 3].score(sub / 3) : 0;
        case NAVTEX: return isPair && pair()->hasNavtex ? pair()->navtex.score(sub) : 0;
        default: return 0;
      }
    }
    // NAVTEX's phasing (rep and alpha, over and over) frames as 50 baud RTTY sending one
    // character with 1 stop bit, and some of its text does too. RTTY never makes NAVTEX's
    // characters (4 ones in 7 bits, each repeated 35 bits later), so where NAVTEX scores, RTTY doesn't.
    bool navtexFound() const {
      return pair()->hasNavtex && fmaxf(pair()->navtex.score(0), pair()->navtex.score(1)) >= MAYBE_AT;
    }
    // The best RTTY or NAVTEX variant's score, whatever's being looked for: pairs are kept with
    // CW or PSK chosen too, so they can claim their tones.
    float pairScore() const {
      float s = 0;
      for (uint8_t i = 0; isPair && i < 6; i++) s = fmaxf(s, score(RTTY, i));
      for (uint8_t i = 0; isPair && i < 2; i++) s = fmaxf(s, score(NAVTEX, i));
      return s;
    }
    // Its tones take turns, as an FSK pair's do, at some baud rate. Two unrelated signals a
    // standard shift apart don't (a CW keyed beside a PSK carrier, say).
    bool takingTurns() const {
      for (int i = 0; isPair && i < 3; i++)
        if (pair()->rtty[i].takingTurns()) return true;
      return false;
    }
    // The best two variants (of `only`'s family, if one).
    void best(Option& a, Option& b) const {
      Option base;
      base.ch = id;
      base.hz = hz;
      base.shift = shift;
      base.level = level;
      a = b = base;
      auto consider = [&](Family f, uint8_t sub) {
        if (only != AUTO && only != f) return;
        Option o = base;
        o.fam = f;
        o.sub = sub;
        o.score = score(f, sub);
        if (o.score > a.score) {
          b = a;
          a = o;
        } else if (o.score > b.score) {
          b = o;
        }
      };
      if (isPair) {
        for (uint8_t s = 0; s < 6; s++) consider(RTTY, s);
        for (uint8_t s = 0; s < 2; s++) consider(NAVTEX, s);
      } else {
        for (uint8_t s = 0; s < 3; s++) consider(PSK, s);
        consider(CW, 0);
      }
    }
    // Where the signal is, as its demodulator follows it.
    float trackedHz(const Option& o) const {
      if (o.fam == PSK) return hz + single()->psk[o.sub].offsetHz();
      if (o.fam == CW) return hz + single()->cw.offsetHz();
      return hz;
    }
    // Half the band the signal takes.
    static float halfWidth(const Option& o) {
      if (o.fam == PSK) return 20.0f * PSK_MULTS[o.sub];
      if (o.fam == RTTY || o.fam == NAVTEX) return o.shift / 2 + 60;
      return 30;
    }
    void text(const Option& o, TextView& v) const {
      if (o.fam == CW) {
        const MorseDecoder& d = single()->cw.decoder();
        v.text = d.text();
        v.scrolled = d.scrolled();
        v.seq = d.seq();
        return;
      }
      const Dsp::Text* t = o.fam == PSK      ? &single()->psk[o.sub].text()
                           : o.fam == NAVTEX ? &pair()->navtex.text(o.sub)
                                             : &pair()->rtty[o.sub % 3].text(o.sub / 3);
      v.text = t->text();
      v.scrolled = t->scrolled();
      v.seq = t->seq();
    }
  };

  struct Peak {
    float hz, db;  // dB over the floor
    bool used;
  };
  struct Tried {
    bool pair;
    float hz, shift;
    uint32_t until;  // hop
  };
  static constexpr int MAX_TRIED = 12;

  void spectrum() {
    hop_++;
    for (int i = 0, from = inPos_; i < FFT_N; i++, from = from + 1 == FFT_N ? 0 : from + 1)
      fftWork_[i] = {in_[from] * win_[i], 0};
    if (fftRun_) fftRun_(fftWork_);
    else fft_->run(fftWork_);
    float pw[SPEC_BINS];
    for (int k = 0; k < SPEC_BINS; k++) {
      pw[k] = Dsp::norm(fftWork_[k]);
      avg_[k] += (pw[k] - avg_[k]) * AVG_K;
    }
    waterfallRow(pw);
    if (hop_ % DECIDE_EVERY == 0) {
      findFloor();
      if (waterfallOnly(family_)) return;
      findSignals();
      decide();
    }
  }

  void waterfallRow(const float* pw) {
    if (!floorKnown_) return;  // nothing to scale it by yet
    if (rowMax_ == 0) memset(rowAcc_, 0, sizeof(rowAcc_));
    for (int x = 0; x < WF_W; x++) {
      float b = x * WF_HZ / BIN_HZ;
      int k = (int)b;
      float f = b - k;
      float p = k + 1 < SPEC_BINS ? pw[k] * (1 - f) + pw[k + 1] * f : 0;
      if (p > rowAcc_[x]) rowAcc_[x] = p;  // the stronger of ROW_EVERY hops
    }
    if (++rowMax_ < ROW_EVERY) return;
    rowMax_ = 0;
    uint8_t* row = rows_[rowsW_ % WF_ROWS];
    float fl = 10 * log10f(floor_ + 1e-20f);
    for (int x = 0; x < WF_W; x++) {
      float db = 10 * log10f(rowAcc_[x] + 1e-20f) - fl;  // 0 at the floor
      float v = (db + 4) * (255.0f / 40);
      row[x] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
    }
    rowsW_++;
    if (rowsW_ - rowsR_ > WF_ROWS) rowsR_ = rowsW_ - WF_ROWS;  // the reader fell behind
  }

  void findFloor() {
    int lo = (int)(LO_HZ / BIN_HZ), hi = (int)(HI_HZ / BIN_HZ), n = hi - lo;
    float tmp[SPEC_BINS];
    memcpy(tmp, avg_ + lo, n * sizeof(float));
    float m = median(tmp, n);
    floor_ = m > 1e-20f ? m : 1e-20f;
    floorKnown_ = true;
  }

  // The median of v[0..n), by partial selection (reorders v).
  static float median(float* v, int n) {
    int k = n / 2, l = 0, r = n - 1;
    while (l < r) {
      float piv = v[k];
      int i = l, j = r;
      while (i <= j) {
        while (v[i] < piv) i++;
        while (v[j] > piv) j--;
        if (i <= j) {
          float t = v[i];
          v[i++] = v[j];
          v[j--] = t;
        }
      }
      if (j < k) l = i;
      if (k < i) r = j;
    }
    return v[k];
  }

  void findSignals() {
    int lo = (int)(LO_HZ / BIN_HZ), hi = (int)(HI_HZ / BIN_HZ);
    float thr = floor_ * powf(10, MIN_DB / 10);
    float s[SPEC_BINS];
    for (int k = lo - 1; k <= hi + 1; k++) s[k] = 0.25f * avg_[k - 1] + 0.5f * avg_[k] + 0.25f * avg_[k + 1];
    Peak pk[24];
    int np = 0;
    for (int k = lo; k <= hi && np < 24; k++) {
      if (s[k] < thr || s[k] < s[k - 1] || s[k] <= s[k + 1]) continue;
      float a = log10f(s[k - 1]), b = log10f(s[k]), c = log10f(s[k + 1]);
      float d = a - 2 * b + c, off = d < 0 ? 0.5f * (a - c) / d : 0;
      float hz = (k + off) * BIN_HZ, db = 10 * (b - log10f(floor_));
      if (np && hz - pk[np - 1].hz < MERGE_HZ) {  // one signal: the stronger's level, between them
        Peak& p = pk[np - 1];
        float wa = powf(10, p.db / 10), wb = powf(10, db / 10);
        p.hz = (p.hz * wa + hz * wb) / (wa + wb);
        if (db > p.db) p.db = db;
        continue;
      }
      pk[np++] = {hz, db, false};
    }
    // A strong signal's keying spreads out around it in lesser peaks: those aren't signals
    int kept = 0;
    for (int i = 0; i < np; i++) {
      bool masked = false;
      for (int j = 0; j < np && !masked; j++)
        masked = j != i && fabsf(pk[j].hz - pk[i].hz) < MASK_HZ && pk[j].db > pk[i].db + MASK_DB;
      if (!masked) pk[kept++] = pk[i];
    }
    np = kept;
    struct Cand {
      bool pair;
      float hz, shift, db;
    } cand[24];
    int nc = 0;
    auto add = [&](bool pair, float hz, float shift, float db) {
      if (nc == 24) return;
      for (int i = 0; i < nc; i++)
        if (cand[i].pair == pair && fabsf(cand[i].hz - hz) < 12) {
          if (db > cand[i].db) cand[i].db = db;
          return;
        }
      cand[nc++] = {pair, hz, shift, db};
    };
    // Pairs: RTTY at a standard shift, or the two lines of an idle PSK63/125
    for (int i = 0; i < np; i++)
      for (int j = i + 1; j < np; j++) {
        float sep = pk[j].hz - pk[i].hz, mid = (pk[i].hz + pk[j].hz) / 2, db = fmaxf(pk[i].db, pk[j].db);
        if (fabsf(pk[i].db - pk[j].db) > PAIR_DB) continue;
        for (float sh : RTTY_SHIFTS)
          if (fabsf(sep - sh) < sh * PAIR_TOL) add(true, mid, sep, db);
        if (sep > 50 && sep < 140) add(false, mid, 0, db);
      }
    for (int i = 0; i < np; i++) add(false, pk[i].hz, 0, pk[i].db);
    // Humps: runs of bins over the threshold, at their centre
    for (int k = lo; k <= hi; k++) {
      if (s[k] < thr) continue;
      float sum = 0, mom = 0, top = 0;
      int k0 = k;
      for (; k <= hi && s[k] >= thr; k++) {
        sum += s[k];
        mom += s[k] * k;
        if (s[k] > top) top = s[k];
      }
      if (k - k0 < 3) continue;
      float hz = mom / sum * BIN_HZ, db = 10 * log10f(top / floor_);
      bool masked = false;
      for (int j = 0; j < np && !masked; j++) masked = fabsf(pk[j].hz - hz) < MASK_HZ && pk[j].db > db + MASK_DB;
      if (!masked) add(false, hz, 0, db);
    }
    // Strongest first, pairs before their tones' singles (they're what decides about those)
    auto rank = [](const Cand& c) { return c.db + (c.pair ? 6 : 0); };
    for (int i = 1; i < nc; i++)
      for (int j = i; j > 0 && rank(cand[j]) > rank(cand[j - 1]); j--) {
        Cand t = cand[j];
        cand[j] = cand[j - 1];
        cand[j - 1] = t;
      }
    for (int i = 0; i < nc; i++) {
      const Cand& c = cand[i];
      if ((family_ == RTTY || family_ == NAVTEX) && !c.pair) continue;
      // An existing channel is still seen (the loop below decides about singles in pairs).
      Channel* m = match(c.pair, c.hz, c.shift);
      if (m) {
        m->seen = hop_;
        m->level = c.db;
      } else if (!(!c.pair && inPair(c.hz)) && !failed(c.pair, c.hz, c.shift)) {
        place(c.pair, c.hz, c.shift, c.db);
      }
    }
    // Let go of channels whose signal has gone, and singles a pair has taken over: one that's
    // scoring, or a young one, unless the single is decoding (it may be what made the pair).
    for (Channel* c : ch_) {
      if (!c->used || c->id == lockedId_) continue;
      if (!c->isPair && !c->pinned && inPair(c->hz, !decoding(c))) {
        c->free();
        continue;
      }
      float gone = (hop_ - c->seen) / HOPS_PER_SEC;
      if (gone > (c->pinned ? PINNED_SEC : UNSEEN_SEC)) c->free();
    }
  }

  // `hz` is on or between the tones of an RTTY pair that's scoring, or (with `young`) on the
  // tones of one that's new and still showing what it is: a single there would only see one
  // tone keyed, which looks like CW. A young pair claims only its tones (all between them at
  // the narrow shifts). No pair claims anything once its tones have been seen not to take turns
  // (from TURNS_SEC, when the overlap average has settled): two signals 850Hz apart once kept
  // every CW between them off the air for 6s, the one on a tone for good, and then, scoring
  // now and then as one of them paused, freed the CWs mid-word (found with tools/rx).
  bool inPair(float hz, bool young = true) const {
    for (const Channel* p : ch_) {
      if (!p->used || !p->isPair || fabsf(hz - p->hz) >= p->shift / 2 + 25) continue;
      float age = (hop_ - p->born) / HOPS_PER_SEC;
      if (age >= TURNS_SEC && !p->takingTurns()) p->mixed = true;
      if (p->mixed) continue;  // two signals, not one: its score comes and goes as theirs do
      if (p->pairScore() >= MAYBE_AT) return true;
      if (!young) continue;
      bool onTone = p->shift <= 200 || fabsf(fabsf(hz - p->hz) - p->shift / 2) < YOUNG_TONE_HZ;
      if (age < PAIR_PROVE_SEC && onTone && !p->mixed) return true;
    }
    return false;
  }

  // Scored MAYBE_AT within TRIAL_SEC: CW's score dips between words.
  bool decoding(const Channel* c) const { return c->good && (hop_ - c->good) / HOPS_PER_SEC < TRIAL_SEC; }

  Channel* match(bool pair, float hz, float shift) {
    Channel* best = nullptr;
    float bestD = 1e9f;
    for (Channel* c : ch_) {
      if (!c->used || c->isPair != pair) continue;
      float d = fabsf(c->hz - hz);
      if (d > (pair ? 20 : 12)) continue;
      if (pair && fabsf(c->shift - shift) > c->shift * 0.15f) continue;
      if (d < bestD) {
        bestD = d;
        best = c;
      }
    }
    return best;
  }

  // A candidate whose channel found nothing a short while ago.
  bool failed(bool pair, float hz, float shift) const {
    for (const Tried& t : tried_)
      if (hop_ < t.until && t.pair == pair && fabsf(t.hz - hz) < (pair ? 20 : 12) &&
          (!pair || fabsf(t.shift - shift) < t.shift * 0.15f))
        return true;
    return false;
  }

  // A channel for a new candidate: a free one; or one that's had its chance and found nothing,
  // the weakest of them if this is clearly stronger, else the one that's tried longest once
  // that's past its trial.
  Channel* place(bool pair, float hz, float shift, float db) {
    Channel* slot = nullptr;
    for (Channel* c : ch_)
      if (!c->used) {
        slot = c;
        break;
      }
    if (!slot) {
      Channel *weakest = nullptr, *oldest = nullptr;
      for (Channel* c : ch_) {
        if (c->pinned || c->id == lockedId_) continue;
        float age = (hop_ - c->born) / HOPS_PER_SEC;
        if (age < 3 || decoding(c)) continue;
        if (!weakest || c->level < weakest->level) weakest = c;
        if (age >= (c->isPair ? PAIR_PROVE_SEC : TRIAL_SEC) && (!oldest || c->born < oldest->born)) oldest = c;
      }
      if (weakest && db >= weakest->level + 3) slot = weakest;
      else slot = oldest;
      if (!slot) return nullptr;
      Tried& t = tried_[nTried_++ % MAX_TRIED];
      t = {slot->isPair, slot->hz, slot->shift, hop_ + (uint32_t)(RETRY_SEC * HOPS_PER_SEC)};
    }
    slot->start(nextId_++, pair, hz, shift, family_, hop_);
    slot->level = db;
    return slot;
  }

  Channel* byId(uint32_t id) {
    for (Channel* c : ch_)
      if (c->used && c->id == id) return c;
    return nullptr;
  }
  const Channel* byId(uint32_t id) const {
    for (const Channel* c : ch_)
      if (c->used && c->id == id) return c;
    return nullptr;
  }

  // A channel already covers `hz`.
  bool spanning(float hz) const {
    for (const Channel* c : ch_) {
      if (!c->used) continue;
      float half = c->isPair ? c->shift / 2 + 30 : 25;
      if (fabsf(c->hz - hz) < half) return true;
    }
    return false;
  }

  // For a tap in RTTY or NAVTEX: the two peaks around `hz` a standard shift apart (NAVTEX's),
  // else `hz` as the centre.
  void nearestPair(float hz, float& centre, float& shift) const {
    int lo = (int)(LO_HZ / BIN_HZ), hi = (int)(HI_HZ / BIN_HZ);
    float bestDb = -1;
    for (float sh : RTTY_SHIFTS) {
      if (family_ == NAVTEX && sh != NavtexDemod::SHIFT) continue;
      // tapped on the lower tone, the upper, or between them
      const float centres[3] = {hz + sh / 2, hz - sh / 2, hz};
      for (float c : centres) {
        int a = (int)((c - sh / 2) / BIN_HZ + 0.5f), b = (int)((c + sh / 2) / BIN_HZ + 0.5f);
        if (a < lo || b > hi) continue;
        float pa = fmaxf(avg_[a], fmaxf(avg_[a - 1], avg_[a + 1])), pb = fmaxf(avg_[b], fmaxf(avg_[b - 1], avg_[b + 1]));
        float db = 10 * log10f(fminf(pa, pb) / floor_);
        if (db > MIN_DB && db > bestDb) {
          bestDb = db;
          centre = c;
          shift = sh;
        }
      }
    }
  }

  void lock(const Option& o) {
    locked_ = o;
    locked_.snr = snrOf(o);
    lockedId_ = o.ch;
    state_ = LOCKED;
    lockId_++;
    lowHops_ = 0;
    pendingUntil_ = 0;
    nOptions_ = 0;
  }

  void unlockNow() {
    if (state_ == LOCKED) lockId_++;
    state_ = SEARCHING;
    lockedId_ = 0;
    nOptions_ = 0;
  }

  void decide() {
    // Every channel's best, those old enough to tell.
    struct Best {
      Option a, b;
      bool shown;
    } best[MAX_CH];
    int nb = 0;
    for (Channel* c : ch_) {
      if (!c->used) continue;
      Best& e = best[nb];
      c->best(e.a, e.b);
      if (e.a.score >= MAYBE_AT) c->good = hop_;
      if (e.a.score > 0) e.a.hz = c->trackedHz(e.a);
      bool young = (hop_ - c->born) / HOPS_PER_SEC < MIN_AGE_SEC;
      if (young) e.a.score = e.b.score = 0;
      e.shown = true;
      nb++;
    }
    // A single on one tone of a pair keys on and off like CW, and one between them may look
    // like anything: while the pair is young, or once it's scoring, the pair has them.
    for (int i = 0; i < nb; i++) {
      Channel* c = byId(best[i].a.ch);
      if (c->isPair) continue;
      for (int j = 0; j < nb; j++) {
        Channel* p = byId(best[j].a.ch);
        if (!p->isPair) continue;
        bool onTone = fabsf(c->hz - p->hz) < p->shift / 2 + 25;
        bool young = (hop_ - p->born) / HOPS_PER_SEC < PAIR_PROVE_SEC;
        if (onTone && (young || best[j].a.score >= MAYBE_AT) && c->id != lockedId_) best[i].shown = false;
      }
    }
    // Overlapping signals: the better-scoring (or, with neither scoring, the stronger) claims the band
    for (int i = 0; i < nb; i++)
      for (int j = 0; j < nb; j++) {
        if (i == j || !best[j].shown) continue;
        Option &x = best[i].a, &y = best[j].a;
        float hx = x.score > 0 ? Channel::halfWidth(x) : 25, hy = y.score > 0 ? Channel::halfWidth(y) : 25;
        if (byId(x.ch)->isPair) hx = byId(x.ch)->shift / 2 + 40;
        if (byId(y.ch)->isPair) hy = byId(y.ch)->shift / 2 + 40;
        if (fabsf(x.hz - y.hz) >= hx + hy - 20) continue;
        bool yWins = y.score > x.score + 0.05f || (fabsf(y.score - x.score) <= 0.05f && y.level > x.level) ||
                     (y.ch == lockedId_ && state_ == LOCKED);
        if (yWins && x.ch != lockedId_) best[i].shown = false;
      }
    nSignals_ = 0;
    for (int i = 0; i < nb; i++) {
      if (!best[i].shown) continue;
      Option o = best[i].a;
      if (o.score < MAYBE_AT) o.fam = AUTO;
      if (o.fam == AUTO) o.hz = byId(o.ch)->hz;
      o.snr = snrOf(o);
      signals_[nSignals_++] = o;
    }

    if (state_ == LOCKED) {
      Channel* c = byId(lockedId_);
      float s = c ? c->score(locked_.fam, locked_.sub) : 0;
      if (c) {
        locked_.hz = c->trackedHz(locked_);
        locked_.score = s;
        locked_.level = c->level;
        locked_.snr = snrOf(locked_);
      }
      lowHops_ = s < DROP_AT ? lowHops_ + DECIDE_EVERY : 0;
      if (!c || lowHops_ / HOPS_PER_SEC > LOST_SEC) unlockNow();
      else return;
    }

    // A tap waiting for something to lock to
    if (pendingUntil_ && hop_ < pendingUntil_) {
      const Option* pick = nullptr;
      for (int i = 0; i < nb; i++) {
        const Option& o = best[i].a;
        Channel* c = byId(o.ch);
        float half = c->isPair ? c->shift / 2 + 40 : 40;
        if (fabsf(c->hz - pendingHz_) < half && o.score >= MAYBE_AT && (!pick || o.score > pick->score)) pick = &o;
      }
      if (pick) {
        lock(*pick);
        return;
      }
    } else {
      pendingUntil_ = 0;
    }

    // Unlocked: lock to a clear winner, or offer what might be.
    nOptions_ = 0;
    const Best* top = nullptr;
    for (int i = 0; i < nb; i++)
      if (best[i].shown && (!top || best[i].a.score > top->a.score)) top = &best[i];
    bool held = hop_ < holdUntil_;
    if (top && !held && top->a.score >= LOCK_AT && top->b.score < top->a.score - MARGIN) {
      lock(top->a);
      return;
    }
    Option cand[2 * MAX_CH];
    int n = 0;
    for (int i = 0; i < nb; i++) {
      if (!best[i].shown) continue;
      if (best[i].a.score >= MAYBE_AT) cand[n++] = best[i].a;
      if (best[i].b.score >= MAYBE_AT && best[i].b.score >= best[i].a.score - MARGIN) cand[n++] = best[i].b;
    }
    for (int i = 1; i < n; i++)
      for (int j = i; j > 0 && cand[j].score > cand[j - 1].score; j--) {
        Option t = cand[j];
        cand[j] = cand[j - 1];
        cand[j - 1] = t;
      }
    for (int i = 0; i < n && nOptions_ < MAX_OPTIONS; i++) options_[nOptions_++] = cand[i];
    state_ = nOptions_ ? CHOOSING : SEARCHING;
  }

  float seg_[HOP];  // the samples feed() is handing the channels
  int segN_ = 0;
  void (*runner_)(void*) = nullptr;
  void (*fftRun_)(Dsp::Cplx*) = nullptr;
  void* runnerCtx_ = nullptr;
  Dsp::Cplx* fftWork_;
  Dsp::Fft<FFT_N>* fft_;
  float* win_;
  float* in_;
  int inPos_ = 0, sinceHop_ = 0;
  uint32_t hop_ = 0;
  float avg_[SPEC_BINS];
  float floor_;
  bool floorKnown_;
  float rowAcc_[WF_W];
  int rowMax_ = 0;
  uint8_t rows_[WF_ROWS][WF_W];
  uint32_t rowsW_ = 0, rowsR_ = 0;
  Channel* ch_[MAX_CH];
  Tried tried_[MAX_TRIED] = {};
  uint32_t nTried_ = 0;
  uint32_t nextId_ = 1;
  Family family_ = AUTO;
  State state_ = SEARCHING;
  Option locked_;
  uint32_t lockedId_ = 0, lockId_ = 0;
  uint32_t lowHops_ = 0, holdUntil_ = 0;
  float pendingHz_ = 0;
  uint32_t pendingUntil_ = 0;
  Option options_[MAX_OPTIONS];
  int nOptions_ = 0;
  Option signals_[MAX_SIGNALS];
  int nSignals_ = 0;
};
}  // namespace Digi
