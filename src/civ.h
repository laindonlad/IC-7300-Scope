#pragma once
// Icom CI-V as the IC-7300 speaks it: FE FE <to> <from> <cmd> [data...] FD.
// Grown from the decoder project's tools/rx/civ.h. The command bytes were checked against Hamlib's
// rigs/icom (icom_defs.h, icom.c, ic7300.c). Plain C++: no OS, no threads, so the tests can check bytes.
//
// Deliberately absent: 1A 05 menu writes. Their addresses differ between the 7300 and the 7300 MK2
// (see ic7300.c's two extcmds tables), and a wrong address silently changes some other menu item.
#include <stdint.h>
#include <stddef.h>

#include <string>
#include <vector>

namespace civ {
using Bytes = std::vector<uint8_t>;

constexpr uint8_t kPc = 0xE0, kIc7300 = 0x94, kBroadcast = 0x00;
constexpr uint8_t kOk = 0xFB, kNg = 0xFA;
constexpr long long kMinHz = 30000, kMaxHz = 74800000;  // the 7300's receive range

inline Bytes frame(uint8_t rig, uint8_t cmd, const Bytes& data = {}) {
  Bytes f = {0xFE, 0xFE, rig, kPc, cmd};
  f.insert(f.end(), data.begin(), data.end());
  f.push_back(0xFD);
  return f;
}

// Frequencies: little-endian BCD, 1Hz/10Hz digits first.
inline Bytes bcdLE(long long v, int bytes) {
  Bytes b((size_t)bytes);
  for (int i = 0; i < bytes; i++) {
    b[(size_t)i] = (uint8_t)((v / 10 % 10) << 4 | (v % 10));
    v /= 100;
  }
  return b;
}
inline long long fromBcdLE(const uint8_t* p, int bytes) {
  long long v = 0;
  for (int i = bytes - 1; i >= 0; i--) v = v * 100 + (p[i] >> 4) * 10 + (p[i] & 15);
  return v;
}
// Levels: big-endian BCD, so 128 is 01 28.
inline Bytes bcdBE(int v, int bytes) {
  Bytes b((size_t)bytes);
  for (int i = bytes - 1; i >= 0; i--) {
    b[(size_t)i] = (uint8_t)((v / 10 % 10) << 4 | (v % 10));
    v /= 100;
  }
  return b;
}
inline int fromBcdBE(const uint8_t* p, int bytes) {
  int v = 0;
  for (int i = 0; i < bytes; i++) v = v * 100 + (p[i] >> 4) * 10 + (p[i] & 15);
  return v;
}

enum Mode : uint8_t { LSB = 0, USB = 1, AM = 2, CW = 3, RTTY = 4, FM = 5, CW_R = 7, RTTY_R = 8 };
inline bool hasData(int m) { return m == LSB || m == USB || m == AM || m == FM; }
inline const char* modeName(int m) {
  switch (m) {
    case LSB: return "LSB";
    case USB: return "USB";
    case AM: return "AM";
    case CW: return "CW";
    case RTTY: return "RTTY";
    case FM: return "FM";
    case CW_R: return "CW-R";
    case RTTY_R: return "RTTY-R";
  }
  return "?";
}

// 14 xx: the rig's knobs and sliders, 0-255.
enum Level { L_AF, L_RF, L_SQL, L_NR, L_PBT_IN, L_PBT_OUT, L_CW_PITCH, L_POWER, L_MIC, L_KEY_SPEED,
             L_NOTCH, L_COMP, L_BKIN_DELAY, L_NB, L_MONITOR, L_VOX, L_ANTIVOX, L_COUNT };
inline uint8_t levelSub(int l) {
  static const uint8_t subs[L_COUNT] = {0x01, 0x02, 0x03, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B,
                                        0x0C, 0x0D, 0x0E, 0x0F, 0x12, 0x15, 0x16, 0x17};
  return subs[l];
}
// 16 xx: functions, mostly on/off.
enum Switch { S_PREAMP, S_AGC, S_NB, S_NR, S_AN, S_TONE, S_TSQL, S_COMP, S_MON, S_VOX, S_BKIN, S_MN,
              S_LOCK, S_DSP_FILTER, S_NOTCH_WIDTH, S_SSB_TX_BW, S_COUNT };
inline uint8_t switchSub(int s) {
  static const uint8_t subs[S_COUNT] = {0x02, 0x12, 0x22, 0x40, 0x41, 0x42, 0x43, 0x44,
                                        0x45, 0x46, 0x47, 0x48, 0x50, 0x56, 0x57, 0x58};
  return subs[s];
}
// 15 xx: meters, read-only.
enum Meter { M_S, M_PO, M_SWR, M_ALC, M_COMP, M_VD, M_ID, M_COUNT };
inline uint8_t meterSub(int m) {
  static const uint8_t subs[M_COUNT] = {0x02, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16};
  return subs[m];
}

inline Bytes setFreq(uint8_t rig, long long hz) { return frame(rig, 0x05, bcdLE(hz, 5)); }
inline Bytes setOtherFreq(uint8_t rig, long long hz) {
  Bytes d = bcdLE(hz, 5);
  d.insert(d.begin(), 0x01);
  return frame(rig, 0x25, d);
}
inline Bytes setMode(uint8_t rig, int mode, int filter) { return frame(rig, 0x06, {(uint8_t)mode, (uint8_t)filter}); }
inline Bytes setData(uint8_t rig, bool on, int filter) {
  return frame(rig, 0x1A, {0x06, (uint8_t)(on ? 1 : 0), (uint8_t)(on ? filter : 0)});
}
inline Bytes setOtherMode(uint8_t rig, int mode, bool data, int filter) {
  return frame(rig, 0x26, {0x01, (uint8_t)mode, (uint8_t)(data ? 1 : 0), (uint8_t)filter});
}
inline Bytes setLevel(uint8_t rig, int level, int v) {
  Bytes d = bcdBE(v, 2);
  d.insert(d.begin(), levelSub(level));
  return frame(rig, 0x14, d);
}
inline Bytes setSwitch(uint8_t rig, int sw, int v) { return frame(rig, 0x16, {switchSub(sw), (uint8_t)v}); }
inline Bytes setAtt(uint8_t rig, bool on) { return frame(rig, 0x11, {(uint8_t)(on ? 0x20 : 0x00)}); }
inline Bytes setSplit(uint8_t rig, bool on) { return frame(rig, 0x0F, {(uint8_t)(on ? 1 : 0)}); }
inline Bytes vfoEqual(uint8_t rig) { return frame(rig, 0x07, {0xA0}); }
inline Bytes vfoSwap(uint8_t rig) { return frame(rig, 0x07, {0xB0}); }
inline Bytes ptt(uint8_t rig, bool on) { return frame(rig, 0x1C, {0x00, (uint8_t)(on ? 1 : 0)}); }
// 0 off, 1 in line, 2 start a tune (which transmits)
inline Bytes atu(uint8_t rig, int v) { return frame(rig, 0x1C, {0x01, (uint8_t)v}); }
// RIT offset: 2 BCD bytes (10Hz/1Hz, 1kHz/100Hz), then the sign. +/-9999Hz.
inline Bytes setRit(uint8_t rig, int hz) {
  int a = hz < 0 ? -hz : hz;
  Bytes b = bcdLE(a, 2);
  return frame(rig, 0x21, {0x00, b[0], b[1], (uint8_t)(hz < 0 ? 1 : 0)});
}
inline Bytes ritOn(uint8_t rig, bool on) { return frame(rig, 0x21, {0x01, (uint8_t)(on ? 1 : 0)}); }
inline Bytes xitOn(uint8_t rig, bool on) { return frame(rig, 0x21, {0x02, (uint8_t)(on ? 1 : 0)}); }
inline Bytes setWidth(uint8_t rig, int code) {
  return frame(rig, 0x1A, {0x03, bcdBE(code, 1)[0]});
}
inline Bytes powerOn(uint8_t rig) { return frame(rig, 0x18, {0x01}); }
inline Bytes powerOff(uint8_t rig) { return frame(rig, 0x18, {0x00}); }
// Plays a TX voice memory, T1-T8. 0 stops one playing.
inline Bytes voiceMemory(uint8_t rig, int n) { return frame(rig, 0x28, {0x00, bcdBE(n, 1)[0]}); }
inline Bytes cwStop(uint8_t rig) { return frame(rig, 0x17, {0xFF}); }

// The characters command 17 can send; anything else is dropped rather than refused whole.
inline bool cwSendable(char c) {
  if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return true;
  for (const char* p = " /?.-,:'()=+\"@"; *p; p++)
    if (c == *p) return true;
  return false;
}
// Upper-cased and cut into the 30 characters a frame holds. Each string goes in its own frame.
inline std::vector<std::string> cwChunks(const std::string& text) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : text) {
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    if (!cwSendable(c)) continue;
    cur += c;
    if (cur.size() == 30) {
      out.push_back(cur);
      cur.clear();
    }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}
inline Bytes cwText(uint8_t rig, const std::string& upTo30) {
  return frame(rig, 0x17, Bytes(upTo30.begin(), upTo30.end()));
}

// The scope (27 xx). Most carry a leading 00: which scope, and the 7300 only has the one.
inline Bytes scopeOn(uint8_t rig, bool on) { return frame(rig, 0x27, {0x10, (uint8_t)(on ? 1 : 0)}); }
inline Bytes scopeOutput(uint8_t rig, bool on) { return frame(rig, 0x27, {0x11, (uint8_t)(on ? 1 : 0)}); }
enum ScopeMode { SCOPE_CENTER = 0, SCOPE_FIXED = 1, SCOPE_SCROLL_C = 2, SCOPE_SCROLL_F = 3 };
inline Bytes scopeMode(uint8_t rig, int m) { return frame(rig, 0x27, {0x14, 0x00, (uint8_t)m}); }
// The whole width shown, in Hz. The rig takes it as +/-, so half of it is sent.
inline Bytes scopeSpan(uint8_t rig, long long spanHz) {
  Bytes d = {0x15, 0x00};
  Bytes b = bcdLE(spanHz / 2, 5);
  d.insert(d.end(), b.begin(), b.end());
  return frame(rig, 0x27, d);
}
// -20 to +20dB in 0.5dB steps; the rig's unit is 0.01dB.
inline Bytes scopeRef(uint8_t rig, double db) {
  int centi = (int)((db < 0 ? -db : db) * 2.0 + 0.5) * 50;
  Bytes b = bcdBE(centi, 2);
  return frame(rig, 0x27, {0x19, 0x00, b[0], b[1], (uint8_t)(db < 0 ? 1 : 0)});
}
inline Bytes scopeSpeed(uint8_t rig, int s) { return frame(rig, 0x27, {0x1A, 0x00, (uint8_t)s}); }  // 0 fast, 1 mid, 2 slow
inline Bytes scopeHold(uint8_t rig, bool on) { return frame(rig, 0x27, {0x17, 0x00, (uint8_t)(on ? 1 : 0)}); }
constexpr int kScopeLevelMax = 160;   // the waveform's values run 0-160
constexpr int kScopePoints = 475;
// Full widths the 7300 offers (its display says +/-2.5k to +/-500k).
constexpr long long kScopeSpans[] = {5000, 10000, 20000, 50000, 100000, 200000, 500000, 1000000};

// Filter width codes (1A 03). SSB, CW and RTTY share a table; AM runs in 200Hz steps. FM has none.
inline int widthHz(int mode, int code) {
  if (code < 0) return 0;
  if (mode == AM) return (code + 1) * 200;
  return code < 10 ? (code + 1) * 50 : (code - 4) * 100;
}
inline int widthCodeMax(int mode) {
  if (mode == AM) return 49;
  if (mode == RTTY || mode == RTTY_R) return 31;
  if (mode == FM) return -1;
  return 40;
}

// ---- meter calibration ----
struct CalPoint {
  int raw;
  double value;
};
inline double interpolate(const CalPoint* t, int n, int raw) {
  if (raw <= t[0].raw) return t[0].value;
  for (int i = 1; i < n; i++)
    if (raw <= t[i].raw)
      return t[i - 1].value + (t[i].value - t[i - 1].value) * (raw - t[i - 1].raw) / (double)(t[i].raw - t[i - 1].raw);
  return t[n - 1].value;
}
// dB relative to S9; a rig reading of 120 is S9. Hamlib's table, measured on live signals.
inline double smeterDb(int raw) {
  static const CalPoint t[] = {{0, -54}, {10, -48}, {30, -36}, {60, -24}, {90, -12}, {120, 0}, {241, 64}};
  return interpolate(t, 7, raw);
}
inline double powerWatts(int raw) {
  static const CalPoint t[] = {{0, 0}, {21, 5}, {43, 10}, {65, 15}, {83, 20}, {95, 25}, {105, 30},
                               {114, 35}, {124, 40}, {143, 50}, {183, 75}, {213, 100}, {255, 120}};
  return interpolate(t, 13, raw);
}
inline double swrRatio(int raw) {
  static const CalPoint t[] = {{0, 1.0}, {48, 1.5}, {80, 2.0}, {120, 3.0}, {240, 6.0}};
  return interpolate(t, 5, raw);
}
inline double compDb(int raw) {
  static const CalPoint t[] = {{0, 0}, {130, 15}, {241, 30}};
  return interpolate(t, 3, raw);
}
inline double voltsVd(int raw) {
  static const CalPoint t[] = {{0, 0}, {13, 10}, {241, 16}};
  return interpolate(t, 3, raw);
}
inline double ampsId(int raw) {
  static const CalPoint t[] = {{0, 0}, {97, 10}, {146, 15}, {241, 25}};
  return interpolate(t, 4, raw);
}

// ---- reading ----
struct Frame {
  uint8_t to = 0, from = 0, cmd = 0;
  Bytes data;
};

class Reader {
 public:
  // Bytes off the port: whole frames go to out. FC is a collision on the CI-V bus, so the frame's dropped.
  void feed(const uint8_t* p, size_t n, std::vector<Frame>& out) {
    for (size_t i = 0; i < n; i++) {
      uint8_t c = p[i];
      if (c == 0xFE) {
        if (buf_.empty() || (buf_.size() == 1 && buf_[0] == 0xFE)) buf_.push_back(c);
        else buf_.assign(1, c);  // a new frame's start: what came before was cut off
        continue;
      }
      if (buf_.size() < 2) {
        buf_.clear();
        continue;
      }
      if (c == 0xFC) {
        buf_.clear();
        continue;
      }
      if (c == 0xFD) {
        if (buf_.size() >= 5) {
          Frame f;
          f.to = buf_[2], f.from = buf_[3], f.cmd = buf_[4];
          f.data.assign(buf_.begin() + 5, buf_.end());
          out.push_back(f);
        }
        buf_.clear();
        continue;
      }
      buf_.push_back(c);
      if (buf_.size() > 256) buf_.clear();  // the longest real frame is about 60 bytes
    }
  }
  void reset() { buf_.clear(); }

 private:
  Bytes buf_;
};
}  // namespace civ
