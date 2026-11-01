#pragma once
// Scanning: go through a list of channels, stop on one with a signal, and carry on according to the settings.
// This is only the decisions. It has no GUI or rig code, so test_core can check it directly: the window calls
// update() every frame with what the rig reports, and does the tuning when it's told to.
//
// A signal is an S meter reading at or above the threshold, on every radio. A squelch would do on a receiver,
// but an open one would stop on every channel, and a transceiver's isn't polled.
//
// The dial is watched. If it stops being where the scan put it, somebody has tuned, from here or from the
// rig's own knob, and the scan ends there: it must never fight the operator.
#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace scan {
struct Channel {
  long long hz = 0;
  int mode = -1;  // -1 leaves the rig's mode alone
  bool data = false;
  int filter = 0;
  std::string label;
};

enum Resume { HOLD, AFTER_5S, AFTER_10S, ON_DROP, RESUME_COUNT };

struct Config {
  double dwell = 0.5;     // seconds on each channel, settling included
  double settle = 0.25;   // the meter can't be believed until the rig has moved, and answered a poll or two
  double recover = 1.0;   // after a signal strong enough to stop for, the AGC needs longer to let go
  double moved = 0.6;     // how long the dial must disagree before it counts as somebody tuning
  double dropFor = 2.0;   // ON_DROP: this long below the threshold
  double thresholdDb = -24;  // relative to S9, as civ::smeterDb gives it
  int resume = ON_DROP;
};

enum State { IDLE, LISTENING, STOPPED };
enum Event { NONE, TUNE, HIT, DIAL_MOVED, NOTHING_LEFT };

class Scanner {
 public:
  void start(std::vector<Channel> channels, const Config& c, size_t first = 0) {
    ch_ = std::move(channels);
    locked_.assign(ch_.size(), false);
    cfg_ = c;
    ended_ = NONE;
    state_ = ch_.empty() ? IDLE : LISTENING;
    at_ = first < ch_.size() ? first : 0;
    pendingTune_ = true;
    settleNow_ = cfg_.settle;
    passes_ = 0;
    peak_ = -999;
    mismatchSince_ = -1;
  }
  void stop() { state_ = IDLE; }
  void setConfig(const Config& c) { cfg_ = c; }

  bool running() const { return state_ != IDLE; }
  State state() const { return state_; }
  const Channel& current() const { return ch_[at_]; }
  size_t index() const { return at_; }
  size_t count() const { return ch_.size(); }
  int passes() const { return passes_; }
  double peakDb() const { return peak_; }
  size_t lockedCount() const { return (size_t)std::count(locked_.begin(), locked_.end(), true); }

  // Carry on from a stop. Also what happens to a channel the rig refused, which goes on the locked list first.
  void next() {
    if (running()) {
      if (state_ == STOPPED) settleNow_ = cfg_.recover;
      advance();
    }
  }
  // Leave this channel out for the rest of this scan, and carry on.
  void lockOut() {
    if (!running()) return;
    locked_[at_] = true;
    next();
  }

  // sDb is the S meter relative to S9, or anything very low if it isn't known. Returns what's just happened:
  // on TUNE the caller tunes to current(), and it's on the dial by the next call.
  Event update(double now, long long dialHz, double sDb) {
    if (state_ == IDLE) return ended_ ? take(ended_) : NONE;
    if (pendingTune_) {
      pendingTune_ = false;
      tunedAt_ = now;
      peak_ = -999;
      mismatchSince_ = -1;
      return TUNE;
    }
    if (dialHz != ch_[at_].hz) {
      if (mismatchSince_ < 0) mismatchSince_ = now;
      if (now - mismatchSince_ >= cfg_.moved) {
        state_ = IDLE;
        return DIAL_MOVED;
      }
      return NONE;  // nothing moves while it's in doubt, or the scan would tune over the operator
    }
    if (mismatchSince_ >= 0) {  // a stale reading after all. What's been heard since can't be trusted
      mismatchSince_ = -1;
      if (state_ == LISTENING) tunedAt_ = now;
    }

    if (state_ == LISTENING) {
      double since = now - tunedAt_;
      if (since >= settleNow_) {
        peak_ = std::max(peak_, sDb);
        if (sDb >= cfg_.thresholdDb) {
          state_ = STOPPED;
          stoppedAt_ = lastAbove_ = now;
          return HIT;
        }
      }
      if (since >= settleNow_ + std::max(0.05, cfg_.dwell - cfg_.settle)) {
        settleNow_ = cfg_.settle;
        advance();
        return afterAdvance(now);
      }
      return NONE;
    }

    peak_ = std::max(peak_, sDb);
    if (sDb >= cfg_.thresholdDb) lastAbove_ = now;
    bool go = (cfg_.resume == AFTER_5S && now - stoppedAt_ >= 5) || (cfg_.resume == AFTER_10S && now - stoppedAt_ >= 10) ||
              (cfg_.resume == ON_DROP && now - lastAbove_ >= cfg_.dropFor);
    if (!go) return NONE;
    next();
    return afterAdvance(now);
  }

 private:
  Event take(Event& e) {
    Event got = e;
    e = NONE;
    return got;
  }
  Event afterAdvance(double now) {
    if (state_ == IDLE) return take(ended_);
    pendingTune_ = false;
    tunedAt_ = now;
    peak_ = -999;
    mismatchSince_ = -1;
    return TUNE;
  }
  void advance() {
    for (size_t k = 1; k <= ch_.size(); k++) {
      size_t i = (at_ + k) % ch_.size();
      if (locked_[i]) continue;
      if (i <= at_) passes_++;
      at_ = i;
      state_ = LISTENING;
      pendingTune_ = true;
      return;
    }
    state_ = IDLE;
    ended_ = NOTHING_LEFT;
  }

  std::vector<Channel> ch_;
  std::vector<bool> locked_;
  Config cfg_;
  State state_ = IDLE;
  Event ended_ = NONE;
  size_t at_ = 0;
  bool pendingTune_ = false;
  double tunedAt_ = 0, stoppedAt_ = 0, lastAbove_ = 0, settleNow_ = 0, mismatchSince_ = -1, peak_ = -999;
  int passes_ = 0;
};
}  // namespace scan
