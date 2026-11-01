#pragma once
// Every signal's text as a conversation of its own, for a terminal (the telnet server): lines
// tagged with the signal they're from,
//   [2 PSK31 1102] CQ CQ DE G4ABC G4ABC PSE K
//   [3 CW 1500] TNX FER CALL UR RST 599
// with a line when a stream starts and ends. Plain C++ (tools/digi_test).
//
// A signal gets a stream once a variant on its channel has scored LOCK_AT for START_MS, or the
// engine is locked to it. The stream follows that variant until it scores under DROP_AT for
// LOST_SEC, its channel goes, or another variant on the channel clearly beats it for SWITCH_MS
// (which starts a new stream). A channel whose stream has just ended needs SWITCH_MS too: as a
// signal ends, the averages behind CW's score pass through what keying looks like, so for a few
// seconds the leftovers of a PSK carrier score as CW. A stream's text is written from the start
// of what its variant decoded, so the words that found it are in.
//
// Text goes out once it's final: CW LAG characters behind (MorseDecoder can rewrite recent
// text) or after QUIET_MS of nothing new; PSK and RTTY only ever add, so at once. One stream
// owns the open line and its text goes out a word at a time as it comes. The others hold theirs
// until they have a whole line (a line end, or WRAP characters), or pause for PAUSE_MS, then
// take the line over. So a lone signal reads live, several at once come out in whole lines, and
// a line taken over ends between words.
//
// A Sink (setSink) also gets each stream's start, its final text as soon as it's final (not
// held back for the shared line), and its end: the PC receiver (tools/rx) shows every stream as
// a conversation of its own.
#include <stdio.h>
#include <string.h>

#include "engine.h"

namespace Digi {
class Streams {
 public:
  using Out = void (*)(void* ctx, const char* s);
  using Clock = void (*)(char* b, size_t n);  // "14:03 ", or "" with no clock

  struct Sink {
    virtual void started(int num, const Option& o) = 0;
    virtual void text(int num, const char* s, int n) = 0;
    virtual void ended(int num) = 0;
  };

  explicit Streams(Out out, void* ctx = nullptr, Clock clock = nullptr, bool ansi = false)
      : out_(out), ctx_(ctx), clock_(clock), ansi_(ansi) {}

  // With the engine's latest, every 100ms or so; `ms` is the time.
  void update(const Engine& e, uint32_t ms) {
    for (Stream& s : s_) {
      if (!s.used) continue;
      Option o = s.o;
      if (!e.follow(o)) {  // the channel's gone, and its text with it
        end(s, ms);
        continue;
      }
      s.o = o;
      take(e, s, ms, false);
      if (o.score < Engine::DROP_AT) {
        if (!s.lowSince) s.lowSince = ms ? ms : 1;
        if (ms - s.lowSince > Engine::LOST_SEC * 1000) {
          take(e, s, ms, true);
          end(s, ms);
          continue;
        }
      } else {
        s.lowSince = 0;
      }
      Option a;
      if (e.bestOf(o.ch, a) && !a.same(o) && a.score >= Engine::LOCK_AT && a.score > o.score + Engine::MARGIN &&
          held(a, ms, SWITCH_MS)) {
        take(e, s, ms, true);
        end(s, ms);
        start(e, a, ms);
        continue;
      }
      flush(s, ms, false);
    }
    const Option* sig;
    int n = e.signals(sig);
    for (int i = 0; i < n; i++) {
      const Option& o = sig[i];
      if (o.fam == AUTO || o.score < Engine::LOCK_AT || onChannel(o.ch)) continue;
      if (held(o, ms, endedLately(o.ch, ms) ? SWITCH_MS : START_MS)) start(e, o, ms);
    }
    if (e.state() == Engine::LOCKED && !onChannel(e.locked().ch)) start(e, e.locked(), ms);
    // Candidates not seen this time start again from nothing.
    int k = 0;
    for (int i = 0; i < nHeld_; i++)
      if (held_[i].at == ms) held_[k++] = held_[i];
    nHeld_ = k;
    emit();
  }

  // Ends every stream, with all its text (before the engine is reset).
  void finish(const Engine& e, uint32_t ms) {
    for (Stream& s : s_) {
      if (!s.used) continue;
      take(e, s, ms, true);
      end(s, ms);
    }
    emit();
  }

  void setSink(Sink* k) { sink_ = k; }

  int active() const {
    int n = 0;
    for (const Stream& s : s_) n += s.used;
    return n;
  }

  static constexpr uint32_t LAG = 16, QUIET_MS = 3000, PAUSE_MS = 2000;
  static constexpr uint32_t START_MS = 1000, SWITCH_MS = 8000;
  static constexpr int WRAP = 64, HARD_WRAP = 90;

 private:
  static constexpr int PEND = 256;
  struct Stream {
    bool used = false;
    int num = 0;
    Option o;
    uint32_t committed = 0;  // where the variant's text has been read to (scrolled + index)
    uint32_t seq = 0, changedAt = 0, lowSince = 0, addedAt = 0, startedAt = 0;
    char pend[PEND];
    int npend = 0;  // final text, not out yet
  };
  // A variant that could start a stream, and since when it's been one.
  struct Held {
    uint32_t ch;
    Family fam;
    uint8_t sub;
    uint32_t since, at;  // `at`: the update it was last seen in
  };

  // `o` has been a candidate for `need` ms, counting this update.
  bool held(const Option& o, uint32_t ms, uint32_t need) {
    for (int i = 0; i < nHeld_; i++) {
      Held& h = held_[i];
      if (h.ch == o.ch && h.fam == o.fam && h.sub == o.sub) {
        h.at = ms;
        return ms - h.since >= need;
      }
    }
    if (nHeld_ < MAX_HELD) held_[nHeld_++] = {o.ch, o.fam, o.sub, ms, ms};
    return need == 0;
  }

  bool endedLately(uint32_t ch, uint32_t ms) const {
    for (const Ended& x : ended_)
      if (x.ch == ch && ms - x.at < SWITCH_MS) return true;
    return false;
  }

  // Output is gathered and handed over in pieces, not a character at a time.
  void write(const char* s) {
    size_t n = strlen(s);
    if (nbuf_ + n >= sizeof(buf_)) emit();
    if (n >= sizeof(buf_)) return;
    memcpy(buf_ + nbuf_, s, n + 1);
    nbuf_ += n;
  }
  void emit() {
    if (nbuf_) out_(ctx_, buf_);
    nbuf_ = 0;
  }

  void stamp() {
    char b[24] = "";
    if (clock_) clock_(b, sizeof(b));
    write(b);
  }

  void colour(const Stream& s, bool on) {
    if (!ansi_) return;
    static const char* const C[] = {"\x1b[1;36m", "\x1b[1;33m", "\x1b[1;35m", "\x1b[1;32m", "\x1b[1;34m", "\x1b[1;31m"};
    write(on ? C[s.num % 6] : "\x1b[0m");
  }

  // Ends the open line, if it has anything on it.
  void endLine() {
    if (col_) write("\r\n");
    col_ = 0;
  }

  bool onChannel(uint32_t ch) const {
    for (const Stream& s : s_)
      if (s.used && s.o.ch == ch) return true;
    return false;
  }

  void start(const Engine& e, const Option& o, uint32_t ms) {
    Stream* s = nullptr;
    for (Stream& x : s_)
      if (!x.used) {
        s = &x;
        break;
      }
    if (!s) return;
    *s = Stream();
    s->used = true;
    s->num = next_;
    next_ = next_ % 99 + 1;
    s->o = o;
    s->startedAt = s->changedAt = ms;
    TextView v = e.text(o);
    s->committed = v.scrolled;
    // The same variant again on its channel: from where the last stream left off, not its text
    // again (which a new stream once repeated, found with tools/rx)
    for (const Ended& x : ended_)
      if (x.at && x.ch == o.ch && x.fam == o.fam && x.sub == o.sub && x.committed > s->committed)
        s->committed = x.committed;
    s->seq = v.seq;
    endLine();
    owner_ = nullptr;
    char lb[24], b[80];
    label(o, lb, sizeof(lb));
    stamp();
    colour(*s, true);
    snprintf(b, sizeof(b), "[%d] %s at %d Hz", s->num, lb, (int)(o.hz + 0.5f));
    write(b);
    colour(*s, false);
    write("\r\n");
    if (sink_) sink_->started(s->num, o);
  }

  void end(Stream& s, uint32_t ms) {
    flush(s, ms, true);
    endLine();
    owner_ = nullptr;
    char lb[24], b[80];
    label(s.o, lb, sizeof(lb));
    ended_[nEnded_++ % Engine::MAX_CH] = {s.o.ch, ms, s.o.fam, s.o.sub, s.committed};
    stamp();
    colour(s, true);
    snprintf(b, sizeof(b), "[%d] %s ended, %lus", s.num, lb, (unsigned long)((ms - s.startedAt) / 1000));
    write(b);
    colour(s, false);
    write("\r\n");
    if (sink_) sink_->ended(s.num);
    s.used = false;
  }

  // Moves the variant's text that's final into pend.
  void take(const Engine& e, Stream& s, uint32_t ms, bool all) {
    TextView v = e.text(s.o);
    size_t n = strlen(v.text);
    if (v.seq != s.seq) {
      s.seq = v.seq;
      s.changedAt = ms;
    }
    if (s.o.fam == CW && !all && ms - s.changedAt < QUIET_MS) {  // LAG characters back, not bytes
      uint32_t chars = 0;
      while (n > 0 && chars < LAG)
        if ((v.text[--n] & 0xC0) != 0x80) chars++;
      if (chars < LAG) n = 0;
    }
    if (s.committed < v.scrolled) s.committed = v.scrolled;  // fell behind: lost, not garbled
    uint32_t to = v.scrolled + (uint32_t)n;
    if (sink_ && s.committed < to) sink_->text(s.num, v.text + (s.committed - v.scrolled), (int)(to - s.committed));
    for (; s.committed < to; s.committed++) {
      if (s.npend == PEND) flush(s, ms, true);
      s.pend[s.npend++] = v.text[s.committed - v.scrolled];
      s.addedAt = ms;
    }
  }

  // A whole line waiting: a line end, or WRAP characters.
  bool wholeLine(const Stream& s) const {
    int chars = 0;
    for (int i = 0; i < s.npend; i++) {
      if (s.pend[i] == '\n') return true;
      if ((s.pend[i] & 0xC0) != 0x80) chars++;
    }
    return chars >= WRAP;
  }

  void flush(Stream& s, uint32_t ms, bool force) {
    if (!s.npend) return;
    bool paused = ms - s.addedAt >= PAUSE_MS;
    if (owner_ != &s) {
      if (!force && !paused && !wholeLine(s)) return;
      endLine();
      owner_ = &s;
    }
    // Whole words only, unless it's paused: so a line that's taken over ends between them.
    int upto = s.npend;
    if (!force && !paused) {
      while (upto > 0 && s.pend[upto - 1] != ' ' && s.pend[upto - 1] != '\n') upto--;
      if (!upto && s.npend >= HARD_WRAP) upto = s.npend;
    }
    char b[2] = {0, 0};
    for (int i = 0; i < upto; i++) {
      char c = s.pend[i];
      bool first = (c & 0xC0) != 0x80;
      if (c == '\n') {
        endLine();
        continue;
      }
      if (c == ' ' && (!col_ || col_ >= WRAP)) {  // no spaces at a line start; wrap at a gap
        if (col_) endLine();
        continue;
      }
      if (first && col_ >= HARD_WRAP) endLine();
      if (!col_) {
        if (c == ' ') continue;
        char lb[24], t[48];
        label(s.o, lb, sizeof(lb));
        colour(s, true);
        snprintf(t, sizeof(t), "[%d %s %d]", s.num, lb, (int)(s.o.hz + 0.5f));
        write(t);
        colour(s, false);
        write(" ");
      }
      b[0] = c;
      write(b);
      if (first) col_++;
    }
    memmove(s.pend, s.pend + upto, s.npend - upto);
    s.npend -= upto;
  }

  Out out_;
  void* ctx_;
  Sink* sink_ = nullptr;
  Clock clock_;
  bool ansi_;
  Stream s_[Engine::MAX_CH];
  const Stream* owner_ = nullptr;  // whose line is open
  int col_ = 0;                    // characters on it (0: none, not even the tag)
  int next_ = 1;
  static constexpr int MAX_HELD = 2 * Engine::MAX_CH;
  Held held_[MAX_HELD];
  int nHeld_ = 0;
  struct Ended {
    uint32_t ch, at;
    Family fam;
    uint8_t sub;
    uint32_t committed;  // where its text had been read to
  } ended_[Engine::MAX_CH] = {};
  uint32_t nEnded_ = 0;
  char buf_[512];
  size_t nbuf_ = 0;
};
}  // namespace Digi
