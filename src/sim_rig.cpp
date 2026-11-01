#include "sim_rig.h"

#include <math.h>
#include <stdlib.h>

#include <algorithm>
#include <chrono>
#include <thread>

namespace {
uint64_t nowMs() {
  using namespace std::chrono;
  return (uint64_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}
uint32_t mix(uint64_t v) {
  v ^= v >> 33;
  v *= 0xff51afd7ed558ccdULL;
  v ^= v >> 33;
  v *= 0xc4ceb9fe1a85ec53ULL;
  v ^= v >> 33;
  return (uint32_t)v;
}
}  // namespace

std::vector<SimSignal> simSignals(long long loHz, long long hiHz) {
  std::vector<SimSignal> out;
  // Fixed stations in 2kHz slots, chosen by hashing the slot: tuning away and back finds them again.
  for (long long slot = loHz / 2000 - 1; slot <= hiHz / 2000 + 1; slot++) {
    uint32_t h = mix((uint64_t)slot);
    if (h % 100 > 34) continue;
    SimSignal s;
    s.hz = slot * 2000 + (h >> 8) % 2000;
    if (s.hz < loHz - 2000 || s.hz > hiHz + 2000) continue;
    s.db = 12.f + (float)((h >> 16) % 40);
    s.kind = (int)((h >> 24) % 3);
    out.push_back(s);
  }
  return out;
}

struct SimRig::Shared {
  std::mutex m;
  const civ::Radio* radio = &civ::kRadio7300;
  uint8_t addr = civ::kIc7300, id = civ::kIc7300;
  bool exchanged = false;
  long long hz = 14074000, otherHz = 14200000;
  int mode = civ::USB, filter = 1, otherMode = civ::USB, otherFilter = 1, width = 34, split = 0, att = 0, atu = 1;
  bool data = true, otherData = false, tx = false;
  int ritHz = 0, ritOn = 0, xitOn = 0;
  int ant = 0, ipPlus = 0;
  int refusals = 0;
  int level[civ::L_COUNT], sw[civ::S_COUNT], tone[civ::TONE_COUNT];
  int scopeOn = 1, scopeOut = 0, scopeMode = civ::SCOPE_CENTER, scopeSpeed = 1;
  long long scopeSpan = 100000;
  int refCenti = 0;
  bool echo = false, silent = false, ignoreUnkey = false, portFails = false;
  int refuse = -1;
  int setFreqs = 0, pttOffs = 0;
  std::vector<civ::Frame> log;
  std::deque<uint8_t> rx;
  uint64_t nextSweep = 0, phase = 0;

  Shared() {
    int lv[civ::L_COUNT] = {};
    lv[civ::L_AF] = 90, lv[civ::L_RF] = 255, lv[civ::L_PBT_IN] = 128, lv[civ::L_PBT_OUT] = 128, lv[civ::L_CW_PITCH] = 128;
    lv[civ::L_POWER] = 127, lv[civ::L_MIC] = 128, lv[civ::L_KEY_SPEED] = 100, lv[civ::L_NOTCH] = 128, lv[civ::L_COMP] = 100;
    lv[civ::L_BKIN_DELAY] = 40, lv[civ::L_NB] = 128, lv[civ::L_NR] = 128, lv[civ::L_MONITOR] = 128, lv[civ::L_VOX] = 128;
    lv[civ::L_ANTIVOX] = 128;
    for (int i = 0; i < civ::L_COUNT; i++) level[i] = lv[i];
    for (int& v : sw) v = 0;
    for (int& v : tone) v = 5;
    sw[civ::S_AGC] = 2;
    sw[civ::S_PREAMP] = 1;
    sw[civ::S_BKIN] = 1;
  }

  void reply(uint8_t cmd, const civ::Bytes& d, uint8_t to = civ::kPc) {
    if (silent) return;
    civ::Bytes f = {0xFE, 0xFE, to, addr, cmd};
    f.insert(f.end(), d.begin(), d.end());
    f.push_back(0xFD);
    rx.insert(rx.end(), f.begin(), f.end());
  }
  void ok() { reply(civ::kOk, {}); }
  void ng() {
    refusals++;
    reply(civ::kNg, {});
  }
  // 1A 05 setting this radio's date, time or UTC offset. Nothing reads them back, so they aren't kept.
  bool clockWrite(const civ::Bytes& d) const {
    const civ::ClockRows* r = civ::clockRows(id);
    if (!r || d.size() < 3) return false;
    const auto is = [&](const uint8_t* row, size_t size) { return d[1] == row[0] && d[2] == row[1] && d.size() == size; };
    return is(r->date, 7) || is(r->time, 5) || is(r->offset, 6);
  }
  // What a receiver has no such command for, going by its CI-V guide.
  bool absent(const civ::Frame& f, uint8_t sub) const {
    if (radio->hasTransmit) return false;
    switch (f.cmd) {
      case 0x07: return !f.data.empty();  // 07 alone is "VFO mode"; there's no second VFO to do anything with
      case 0x0F: case 0x17: case 0x21: case 0x28: return true;
      case 0x14:
        for (int l = 0; l < civ::L_COUNT; l++)
          if (civ::levelSub(l) == sub) return civ::levelIsTx(l);
        return false;
      case 0x15: return sub != civ::meterSub(civ::M_S) && sub != civ::kSquelchStatus && sub != civ::kOverflowStatus;
      case 0x16:
        for (int s = 0; s < civ::S_COUNT; s++)
          if (civ::switchSub(s) == sub) return civ::switchIsTx(s);
        return false;
      case 0x1A: return sub == 0x06;
      case 0x1C: return sub != 0x02;
      case 0x25: case 0x26: return sub != 0x00;
    }
    return false;
  }

  static civ::Bytes prefixed(uint8_t a, const civ::Bytes& b) {
    civ::Bytes d = {a};
    d.insert(d.end(), b.begin(), b.end());
    return d;
  }

  int meter(int m) {
    uint32_t r = mix(nowMs() / 180 + (uint64_t)m * 977);
    switch (m) {
      case civ::M_S: return tx ? 0 : 55 + (int)(r % 60);
      case civ::M_PO: return tx ? 120 + (int)(r % 60) : 0;
      case civ::M_SWR: return tx ? 26 : 0;
      case civ::M_ALC: return tx ? 40 + (int)(r % 50) : 0;
      case civ::M_COMP: return tx ? 60 + (int)(r % 40) : 0;
      case civ::M_VD: return 165;
      case civ::M_ID: return tx ? 90 + (int)(r % 50) : 6;
    }
    return 0;
  }

  void handle(const civ::Frame& f) {
    log.push_back(f);
    // It only takes "who's there" from the broadcast address: that's all anything sends it.
    if (f.to != addr && !(f.to == civ::kBroadcast && f.cmd == 0x19)) return;
    if (refuse == f.cmd) {
      refuse = -1;
      ng();
      return;
    }
    const civ::Bytes& d = f.data;
    uint8_t sub = d.empty() ? 0xFF : d[0];
    if (absent(f, sub)) return ng();
    switch (f.cmd) {
      case 0x03: reply(0x03, civ::bcdLE(hz, 5)); break;
      case 0x04: reply(0x04, {(uint8_t)mode, (uint8_t)filter}); break;
      case 0x05:
        if (d.size() >= 5 && radio->canTune(civ::fromBcdLE(d.data(), 5))) hz = civ::fromBcdLE(d.data(), 5), setFreqs++, ok();
        else ng();
        break;
      case 0x06:
        if (!radio->hasTransmit && !d.empty() && d[0] == civ::DD) return ng();
        if (!d.empty()) mode = d[0];
        if (d.size() >= 2) filter = d[1];
        ok();
        break;
      case 0x07:
        if (sub == 0xD0 && !radio->hasMainSub) return ng();
        if (sub == 0xA0) otherHz = hz, otherMode = mode, otherData = data, otherFilter = filter;
        if (sub == 0xB0 && radio->hasMainSub) {
          exchanged = true;
        } else if (sub == 0xB0) {
          std::swap(hz, otherHz);
          std::swap(mode, otherMode);
          std::swap(data, otherData);
          std::swap(filter, otherFilter);
        }
        ok();
        break;
      case 0x0F:
        if (d.empty()) reply(0x0F, {(uint8_t)split});
        else split = d[0], ok();
        break;
      case 0x11:
        if (d.empty()) {
          reply(0x11, {(uint8_t)att});
        } else {
          bool known = d[0] == 0;
          for (int i = 1; i <= radio->attSteps; i++) known = known || civ::fromBcdBE(&d[0], 1) == radio->attDb * i;
          if (known) att = d[0], ok();
          else ng();
        }
        break;
      case 0x12:
        if (radio->antennas <= 1) return ng();
        if (d.empty()) {
          reply(0x12, {(uint8_t)(hz < radio->antennaSwitchBelowHz ? ant : 0)});
        } else if (d[0] >= radio->antennas || (d[0] > 0 && hz >= radio->antennaSwitchBelowHz)) {
          ng();
        } else {
          ant = d[0], ok();
        }
        break;
      case 0x14:
        for (int l = 0; l < civ::L_COUNT; l++)
          if (civ::levelSub(l) == sub) {
            if (d.size() >= 3) level[l] = civ::fromBcdBE(&d[1], 2), ok();
            else reply(0x14, prefixed(sub, civ::bcdBE(level[l], 2)));
            return;
          }
        ng();
        break;
      case 0x15:
        if (sub == civ::kSquelchStatus) return reply(0x15, {sub, 1});
        if (sub == civ::kOverflowStatus) return reply(0x15, {sub, 0});
        for (int m = 0; m < civ::M_COUNT; m++)
          if (civ::meterSub(m) == sub) {
            reply(0x15, prefixed(sub, civ::bcdBE(meter(m), 2)));
            return;
          }
        ng();
        break;
      case 0x16:
        for (int s = 0; s < civ::S_COUNT; s++)
          if (civ::switchSub(s) == sub) {
            if (d.size() >= 2 && s == civ::S_PREAMP && radio->singlePreamp && d[1] > 1) return ng();
            if (d.size() >= 2) sw[s] = d[1], ok();
            else reply(0x16, {sub, (uint8_t)sw[s]});
            return;
          }
        ng();
        break;
      case 0x17:
      case 0x18:
      case 0x28:
        ok();
        break;
      case 0x19: reply(0x19, {0x00, id}); break;
      case 0x1A:
        if (sub == 0x03) {
          if (d.size() >= 2) width = civ::fromBcdBE(&d[1], 1), ok();
          else reply(0x1A, {0x03, civ::bcdBE(width, 1)[0]});
        } else if (sub == 0x06) {
          if (d.size() >= 2) data = d[1] != 0, ok();
          else reply(0x1A, {0x06, (uint8_t)(data ? 1 : 0), (uint8_t)(data ? filter : 0)});
        } else if (sub == 0x07 && radio->hasIpPlus) {
          if (d.size() >= 2) ipPlus = d[1], ok();
          else reply(0x1A, {0x07, (uint8_t)ipPlus});
        } else if (sub == 0x05 && clockWrite(d)) {
          ok();
        } else if (sub == 0x05 && radio->hasToneMenu() && d.size() >= 3 && d[1] == 0x00) {
          for (int t = 0; t < civ::TONE_COUNT; t++)
            if (civ::toneAddr(t) == d[2]) {
              if (d.size() >= 4) tone[t] = civ::fromBcdBE(&d[3], 1), ok();
              else reply(0x1A, {0x05, 0x00, d[2], civ::bcdBE(tone[t], 1)[0]});
              return;
            }
          ng();
        } else {
          ng();
        }
        break;
      case 0x1C:
        if (sub == 0x00) {
          if (d.size() >= 2) {
            bool on = d[1] != 0;
            if (!on) pttOffs++;
            if (!on && ignoreUnkey) break;
            tx = on;
            ok();
          } else {
            reply(0x1C, {0x00, (uint8_t)(tx ? 1 : 0)});
          }
        } else if (sub == 0x01 && radio->hasTuner) {
          if (d.size() >= 2) atu = d[1] == 2 ? 1 : d[1], ok();
          else reply(0x1C, {0x01, (uint8_t)atu});
        } else {
          ng();
        }
        break;
      case 0x21:
        if (sub == 0x00) {
          if (d.size() >= 4) {
            int v = (int)civ::fromBcdLE(&d[1], 2);
            ritHz = d[3] ? -v : v;
            ok();
          } else {
            civ::Bytes b = civ::bcdLE(abs(ritHz), 2);
            reply(0x21, {0x00, b[0], b[1], (uint8_t)(ritHz < 0 ? 1 : 0)});
          }
        } else if (sub == 0x01) {
          if (d.size() >= 2) ritOn = d[1], ok();
          else reply(0x21, {0x01, (uint8_t)ritOn});
        } else if (sub == 0x02 && radio->hasXit) {
          if (d.size() >= 2) xitOn = d[1], ok();
          else reply(0x21, {0x02, (uint8_t)xitOn});
        } else {
          ng();
        }
        break;
      case 0x25:
        if (sub == 0x01) {
          if (d.size() >= 6) otherHz = civ::fromBcdLE(&d[1], 5), ok();
          else reply(0x25, prefixed(0x01, civ::bcdLE(otherHz, 5)));
        } else {
          ng();
        }
        break;
      case 0x26:
        if (sub == 0x01) {
          if (d.size() >= 4) otherMode = d[1], otherData = d[2] != 0, otherFilter = d[3], ok();
          else reply(0x26, {0x01, (uint8_t)otherMode, (uint8_t)(otherData ? 1 : 0), (uint8_t)otherFilter});
        } else {
          ng();
        }
        break;
      case 0x27: handleScope(d); break;
      default: ng();
    }
  }

  void handleScope(const civ::Bytes& d) {
    if (d.empty()) return ng();
    if (d[0] == 0x12 && radio->hasMainSub) return ok();
    switch (d[0]) {
      case 0x10:
        if (d.size() >= 2) scopeOn = d[1], ok();
        else reply(0x27, {0x10, (uint8_t)scopeOn});
        break;
      case 0x11:
        if (d.size() >= 2) scopeOut = d[1], ok();
        else reply(0x27, {0x11, (uint8_t)scopeOut});
        break;
      case 0x14:
        if (d.size() >= 3) scopeMode = d[2], ok();
        else reply(0x27, {0x14, 0x00, (uint8_t)scopeMode});
        break;
      case 0x15:
        if (d.size() >= 7) scopeSpan = civ::fromBcdLE(&d[2], 5) * 2, ok();
        else {
          civ::Bytes r = {0x15, 0x00}, b = civ::bcdLE(scopeSpan / 2, 5);
          r.insert(r.end(), b.begin(), b.end());
          reply(0x27, r);
        }
        break;
      case 0x19:
        if (d.size() >= 5) refCenti = civ::fromBcdBE(&d[2], 2) * (d[4] ? -1 : 1), ok();
        else {
          civ::Bytes b = civ::bcdBE(abs(refCenti), 2);
          reply(0x27, {0x19, 0x00, b[0], b[1], (uint8_t)(refCenti < 0 ? 1 : 0)});
        }
        break;
      case 0x1A:
        if (d.size() >= 3) scopeSpeed = d[2], ok();
        else reply(0x27, {0x1A, 0x00, (uint8_t)scopeSpeed});
        break;
      default: ng();
    }
  }

  // One sweep, in the 11 divisions the real rig uses over USB.
  void emitSweep() {
    long long lo = scopeMode == civ::SCOPE_CENTER ? hz - scopeSpan / 2 : (hz / 1000000) * 1000000;
    long long hi = lo + scopeSpan;
    std::vector<uint8_t> line(civ::kScopePoints);
    uint32_t t = (uint32_t)(++phase);
    double perPoint = (double)(hi - lo) / civ::kScopePoints;
    std::vector<float> db(civ::kScopePoints, 0.f);
    for (const SimSignal& s : simSignals(lo, hi)) {
      double bw = s.kind == 1 ? 2400 : s.kind == 2 ? 60 : 40;
      long long start = s.kind == 1 ? s.hz : s.hz - (long long)(bw / 2);
      // Stations come and go, so the waterfall shows texture rather than straight lines.
      if (s.kind == 0 && (mix((uint64_t)s.hz + t / 6) & 3) == 0) continue;
      if (s.kind == 1 && (mix((uint64_t)s.hz + t / 14) & 3) == 0) continue;
      int a = (int)((double)(start - lo) / perPoint), b = (int)((double)(start + (long long)bw - lo) / perPoint);
      for (int i = std::max(0, a); i <= std::min(civ::kScopePoints - 1, b); i++) {
        float v = s.db;
        if (s.kind == 1) v -= (float)(mix((uint64_t)i + t) % 14);
        db[(size_t)i] = std::max(db[(size_t)i], v);
      }
    }
    for (int i = 0; i < civ::kScopePoints; i++) {
      int v = 18 + (int)(mix((uint64_t)i * 131 + t) % 12) + (int)(db[(size_t)i] * 1.9f);
      line[(size_t)i] = (uint8_t)std::min(civ::kScopeLevelMax, std::max(0, v));
    }
    civ::Bytes first = {0x00, 0x00, 0x01, 0x11, (uint8_t)scopeMode};
    civ::Bytes a = civ::bcdLE(scopeMode == civ::SCOPE_CENTER ? hz : lo, 5);
    civ::Bytes b = civ::bcdLE(scopeMode == civ::SCOPE_CENTER ? scopeSpan / 2 : hi, 5);
    first.insert(first.end(), a.begin(), a.end());
    first.insert(first.end(), b.begin(), b.end());
    first.push_back(0x00);
    reply(0x27, first);
    size_t at = 0;
    for (int div = 2; div <= 11; div++) {
      civ::Bytes f = {0x00, 0x00, civ::bcdBE(div, 1)[0], 0x11};
      size_t n = div == 11 ? 25 : 50;
      f.insert(f.end(), line.begin() + (long)at, line.begin() + (long)(at + n));
      at += n;
      reply(0x27, f);
    }
  }
};

namespace {
class SimTransport : public Transport {
 public:
  explicit SimTransport(std::shared_ptr<SimRig::Shared> s) : s_(std::move(s)) {}
  bool open(std::string& err) override {
    std::lock_guard<std::mutex> g(s_->m);
    if (s_->portFails) {
      err = "The simulated port is unplugged";
      return false;
    }
    open_ = true;
    return true;
  }
  void close() override { open_ = false; }
  bool isOpen() const override { return open_; }
  int read(uint8_t* buf, int max, int timeoutMs) override {
    uint64_t until = nowMs() + (uint64_t)timeoutMs;
    for (;;) {
      {
        std::lock_guard<std::mutex> g(s_->m);
        if (s_->portFails) return -1;
        uint64_t now = nowMs();
        if (s_->scopeOut && s_->scopeOn && !s_->silent && now >= s_->nextSweep) {
          s_->emitSweep();
          s_->nextSweep = now + (s_->scopeSpeed == 0 ? 50 : s_->scopeSpeed == 1 ? 90 : 160);
        }
        if (!s_->rx.empty()) {
          int n = std::min(max, (int)s_->rx.size());
          for (int i = 0; i < n; i++) {
            buf[i] = s_->rx.front();
            s_->rx.pop_front();
          }
          return n;
        }
      }
      if (nowMs() >= until) return 0;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
  bool write(const uint8_t* buf, int n) override {
    std::lock_guard<std::mutex> g(s_->m);
    if (s_->portFails) return false;
    std::vector<civ::Frame> frames;
    reader_.feed(buf, (size_t)n, frames);
    if (s_->echo && !s_->silent) s_->rx.insert(s_->rx.end(), buf, buf + n);
    if (!s_->silent)
      for (const civ::Frame& f : frames) s_->handle(f);
    return true;
  }

 private:
  std::shared_ptr<SimRig::Shared> s_;
  civ::Reader reader_;
  bool open_ = false;
};
}  // namespace

SimRig::SimRig(const civ::Radio& radio) : s_(std::make_shared<Shared>()) {
  s_->radio = &radio;
  s_->addr = s_->id = radio.addr;
  if (radio.addr == civ::kIc9700) {
    // On 2 m, listening to the FM calling frequency's neighbourhood. No tuner, and the attenuator off.
    s_->hz = 145500000, s_->otherHz = 145600000;
    s_->mode = s_->otherMode = civ::FM;
    s_->data = false;
    s_->width = 0;
  }
  if (radio.addr == civ::kIcR8600) {
    // A shortwave broadcaster, so the antennas can be changed from the start. The second VFO's values are never asked for.
    s_->hz = 9700000, s_->otherHz = 9700000;
    s_->mode = s_->otherMode = civ::AM;
    s_->data = false;
    s_->width = 34;
  }
}
SimRig::~SimRig() = default;
Transport* SimRig::newTransport() { return new SimTransport(s_); }

#define LOCKED std::lock_guard<std::mutex> g(s_->m)
long long SimRig::hz() { LOCKED; return s_->hz; }
int SimRig::mode() { LOCKED; return s_->mode; }
bool SimRig::data() { LOCKED; return s_->data; }
int SimRig::filter() { LOCKED; return s_->filter; }
bool SimRig::tx() { LOCKED; return s_->tx; }
bool SimRig::scopeOutput() { LOCKED; return s_->scopeOut != 0; }
long long SimRig::otherHz() { LOCKED; return s_->otherHz; }
int SimRig::otherMode() { LOCKED; return s_->otherMode; }
int SimRig::att() { LOCKED; return s_->att; }
int SimRig::antenna() { LOCKED; return s_->ant; }
bool SimRig::ipPlus() { LOCKED; return s_->ipPlus != 0; }
int SimRig::refusals() { LOCKED; return s_->refusals; }
bool SimRig::mainSubExchanged() { LOCKED; return s_->exchanged; }
void SimRig::setCivAddress(uint8_t addr, bool idFollows) {
  LOCKED;
  s_->addr = addr;
  if (idFollows) s_->id = addr;
}
int SimRig::setFreqCount() { LOCKED; return s_->setFreqs; }
int SimRig::pttOffCount() { LOCKED; return s_->pttOffs; }
std::vector<civ::Bytes> SimRig::received(uint8_t cmd) {
  LOCKED;
  std::vector<civ::Bytes> out;
  for (const civ::Frame& f : s_->log)
    if (f.cmd == cmd) out.push_back(f.data);
  return out;
}
void SimRig::spinDial(long long hz, bool report) {
  LOCKED;
  s_->hz = hz;
  if (report) s_->reply(0x00, civ::bcdLE(hz, 5), civ::kBroadcast);
}
void SimRig::pressPtt(bool on) { LOCKED; s_->tx = on; }
void SimRig::setEcho(bool on) { LOCKED; s_->echo = on; }
void SimRig::setSilent(bool v) { LOCKED; s_->silent = v; }
void SimRig::setRefuseNext(uint8_t cmd) { LOCKED; s_->refuse = cmd; }
void SimRig::setIgnoreUnkey(bool on) { LOCKED; s_->ignoreUnkey = on; }
void SimRig::setPortFails(bool on) { LOCKED; s_->portFails = on; }
