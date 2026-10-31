#include "rig.h"

#include <string.h>

#include <algorithm>
#include <chrono>

namespace {
uint64_t nowMs() {
  using namespace std::chrono;
  return (uint64_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}
constexpr uint32_t key(uint8_t cmd, int sub = -1) { return (uint32_t)cmd << 8 | (uint32_t)(sub < 0 ? 0xFF : sub); }

// Shared keys for what can be both written and read, so a read's old answer can be ignored after a write.
constexpr uint32_t K_FREQ = key(0x03), K_MODE = key(0x04), K_DATA = key(0x1A, 0x06);
constexpr uint32_t K_OTHER_FREQ = key(0x25, 1), K_OTHER_MODE = key(0x26, 1), K_WIDTH = key(0x1A, 0x03);
constexpr uint32_t K_SPLIT = key(0x0F), K_ATT = key(0x11), K_ATU = key(0x1C, 1), K_TX = key(0x1C, 0);
constexpr uint32_t K_RIT = key(0x21, 0), K_RIT_ON = key(0x21, 1), K_XIT_ON = key(0x21, 2);
constexpr uint32_t K_SCOPE_ON = key(0x27, 0x10), K_SCOPE_OUT = key(0x27, 0x11), K_SCOPE_MODE = key(0x27, 0x14);
constexpr uint32_t K_SCOPE_SPAN = key(0x27, 0x15), K_SCOPE_REF = key(0x27, 0x19), K_SCOPE_SPEED = key(0x27, 0x1A);

constexpr int kReplyTimeoutMs = 250;
constexpr int kLinkLostMs = 2500;   // no frame at all for this long: the rig's off or gone
constexpr int kPollGapMs = 12;      // room between polls, for the rig's sake
constexpr int kHoldMs = 400;
constexpr int kUnkeyRetryMs = 250;
}  // namespace

RigState::RigState() {
  for (int& v : level) v = -1;
  for (int& v : sw) v = -1;
  for (int& v : meter) v = -1;
}

Rig::Rig() = default;
Rig::~Rig() { stop(); }

void Rig::start(std::function<Transport*()> factory, uint8_t addr) {
  stop();
  {
    std::lock_guard<std::mutex> g(m_);
    RigState fresh;
    fresh.txLimit = st_.txLimit;
    st_ = fresh;
    queue_.clear();
    scopeLines_.clear();
    holds_.clear();
    keyWanted_ = keyedByUs_ = false;
    lastUnkeySent_ = txReadAt_ = 0;
    // wantScope_ carries over: it's what the window asked for
  }
  factory_ = std::move(factory);
  addr_ = addr;
  quit_ = false;
  nextOpenAt_ = 0;
  buildPolls();
  thread_ = std::thread(&Rig::run, this);
}

void Rig::stop() {
  if (!thread_.joinable()) return;
  quit_ = true;
  thread_.join();
}

RigState Rig::state() const {
  std::lock_guard<std::mutex> g(m_);
  return st_;
}

bool Rig::popScope(ScopeLine& out) {
  std::lock_guard<std::mutex> g(m_);
  if (scopeLines_.empty()) return false;
  out = std::move(scopeLines_.front());
  scopeLines_.pop_front();
  return true;
}

// ---- the window's side ----

void Rig::post(uint32_t k, civ::Bytes bytes, const char* what, bool atFront) {
  // m_ is held. A command with no link would only go out stale later on.
  if (!st_.connected) return;
  for (Cmd& c : queue_)
    if (c.key == k) {
      c.bytes = std::move(bytes);
      return;
    }
  Cmd c{k, std::move(bytes), what};
  if (atFront) queue_.push_front(std::move(c));
  else queue_.push_back(std::move(c));
}

void Rig::hold(uint32_t k) {
  uint64_t now = nowMs(), until = now + kHoldMs;
  for (auto& h : holds_)
    if (h.first == k) {
      h.second = until;
      return;
    }
  // CW chunks each have their own key, so old entries are swept out.
  if (holds_.size() > 128)
    holds_.erase(std::remove_if(holds_.begin(), holds_.end(), [&](const std::pair<uint32_t, uint64_t>& h) { return h.second <= now; }),
                 holds_.end());
  holds_.push_back({k, until});
}

bool Rig::held(uint32_t k, uint64_t now) const {
  for (auto& h : holds_)
    if (h.first == k) return now < h.second;
  return false;
}

void Rig::setFreq(long long hz) {
  hz = std::min(civ::kMaxHz, std::max(civ::kMinHz, hz));
  std::lock_guard<std::mutex> g(m_);
  post(K_FREQ, civ::setFreq(addr_, hz), "set that frequency");
  st_.hz = hz;
  hold(K_FREQ);
}

void Rig::setOtherFreq(long long hz) {
  hz = std::min(civ::kMaxHz, std::max(civ::kMinHz, hz));
  std::lock_guard<std::mutex> g(m_);
  post(K_OTHER_FREQ, civ::setOtherFreq(addr_, hz), "set the other VFO");
  st_.otherHz = hz;
  hold(K_OTHER_FREQ);
}

void Rig::setMode(int mode, bool data, int filter) {
  filter = std::min(3, std::max(1, filter));
  std::lock_guard<std::mutex> g(m_);
  post(K_MODE, civ::setMode(addr_, mode, filter), "set that mode");
  bool d = data && civ::hasData(mode);
  if (civ::hasData(mode)) post(K_DATA, civ::setData(addr_, d, filter), "set data mode");
  st_.mode = mode, st_.data = d, st_.filter = filter;
  hold(K_MODE);
  hold(K_DATA);
  hold(K_WIDTH);
}

void Rig::setOtherMode(int mode, bool data, int filter) {
  filter = std::min(3, std::max(1, filter));
  std::lock_guard<std::mutex> g(m_);
  bool d = data && civ::hasData(mode);
  post(K_OTHER_MODE, civ::setOtherMode(addr_, mode, d, filter), "set the other VFO's mode");
  st_.otherMode = mode, st_.otherData = d, st_.otherFilter = filter;
  hold(K_OTHER_MODE);
}

void Rig::setLevel(int l, int v) {
  v = std::min(255, std::max(0, v));
  uint32_t k = key(0x14, civ::levelSub(l));
  std::lock_guard<std::mutex> g(m_);
  post(k, civ::setLevel(addr_, l, v), "set that level");
  st_.level[l] = v;
  hold(k);
}

void Rig::setSwitch(int s, int v) {
  uint32_t k = key(0x16, civ::switchSub(s));
  std::lock_guard<std::mutex> g(m_);
  post(k, civ::setSwitch(addr_, s, v), "change that setting");
  st_.sw[s] = v;
  hold(k);
}

void Rig::setAtt(bool on) {
  std::lock_guard<std::mutex> g(m_);
  post(K_ATT, civ::setAtt(addr_, on), "set the attenuator");
  st_.att = on;
  hold(K_ATT);
}

void Rig::setSplit(bool on) {
  std::lock_guard<std::mutex> g(m_);
  post(K_SPLIT, civ::setSplit(addr_, on), "set split");
  st_.split = on;
  hold(K_SPLIT);
}

void Rig::setWidth(int code) {
  std::lock_guard<std::mutex> g(m_);
  post(K_WIDTH, civ::setWidth(addr_, code), "set the filter width");
  st_.width = code;
  hold(K_WIDTH);
}

void Rig::setRit(int hz) {
  hz = std::min(9999, std::max(-9999, hz));
  std::lock_guard<std::mutex> g(m_);
  post(K_RIT, civ::setRit(addr_, hz), "set the RIT offset");
  st_.ritHz = hz;
  hold(K_RIT);
}

void Rig::setRitOn(bool on) {
  std::lock_guard<std::mutex> g(m_);
  post(K_RIT_ON, civ::ritOn(addr_, on), "set RIT");
  st_.ritOn = on;
  hold(K_RIT_ON);
}

void Rig::setXitOn(bool on) {
  std::lock_guard<std::mutex> g(m_);
  post(K_XIT_ON, civ::xitOn(addr_, on), "set XIT");
  st_.xitOn = on;
  hold(K_XIT_ON);
}

void Rig::vfoEqual() {
  std::lock_guard<std::mutex> g(m_);
  post(key(0x07, 0xA0), civ::vfoEqual(addr_), "copy the VFO");
}

void Rig::vfoSwap() {
  std::lock_guard<std::mutex> g(m_);
  post(key(0x07, 0xB0), civ::vfoSwap(addr_), "swap the VFOs");
  std::swap(st_.hz, st_.otherHz);
  std::swap(st_.mode, st_.otherMode);
  std::swap(st_.data, st_.otherData);
  std::swap(st_.filter, st_.otherFilter);
  for (uint32_t k : {K_FREQ, K_MODE, K_DATA, K_OTHER_FREQ, K_OTHER_MODE}) hold(k);
}

void Rig::setAtu(bool on) {
  std::lock_guard<std::mutex> g(m_);
  post(K_ATU, civ::atu(addr_, on ? 1 : 0), "set the tuner");
  st_.atu = on;
  hold(K_ATU);
}

void Rig::powerOff() {
  std::lock_guard<std::mutex> g(m_);
  post(key(0x18), civ::powerOff(addr_), "power off");
}

void Rig::powerOn() {
  // The rig's asleep, so nothing's connected and post would refuse. The worker's next write goes first.
  std::lock_guard<std::mutex> g(m_);
  Cmd c{key(0x18), {}, "power on"};
  // At 115200 the rig needs about 150 FEs to wake its CI-V (Hamlib's icom_set_powerstat).
  c.bytes.assign(150, 0xFE);
  civ::Bytes on = civ::powerOn(addr_);
  c.bytes.insert(c.bytes.end(), on.begin(), on.end());
  queue_.push_front(std::move(c));
}

void Rig::setScope(bool output) {
  std::lock_guard<std::mutex> g(m_);
  wantScope_ = output;
  if (output) post(K_SCOPE_ON, civ::scopeOn(addr_, true), "turn the scope on");
  post(K_SCOPE_OUT, civ::scopeOutput(addr_, output), "start the scope data");
  st_.scopeOutput = output;
  hold(K_SCOPE_OUT);
}

void Rig::setScopeSpan(long long hz) {
  std::lock_guard<std::mutex> g(m_);
  post(K_SCOPE_SPAN, civ::scopeSpan(addr_, hz), "set the span");
  st_.scopeSpan = hz;
  hold(K_SCOPE_SPAN);
}

void Rig::setScopeMode(int mode) {
  std::lock_guard<std::mutex> g(m_);
  post(K_SCOPE_MODE, civ::scopeMode(addr_, mode), "set the scope mode");
  st_.scopeMode = mode;
  hold(K_SCOPE_MODE);
}

void Rig::setScopeRef(double db) {
  std::lock_guard<std::mutex> g(m_);
  post(K_SCOPE_REF, civ::scopeRef(addr_, db), "set the scope reference");
  st_.scopeRef = db, st_.scopeRefKnown = true;
  hold(K_SCOPE_REF);
}

void Rig::setScopeSpeed(int s) {
  std::lock_guard<std::mutex> g(m_);
  post(K_SCOPE_SPEED, civ::scopeSpeed(addr_, s), "set the sweep speed");
  st_.scopeSpeed = s;
  hold(K_SCOPE_SPEED);
}

void Rig::armTx(bool on) {
  std::lock_guard<std::mutex> g(m_);
  st_.txArmed = on;
  if (!on) keyWanted_ = false;  // disarming also lets go
}

bool Rig::ptt(bool on) {
  std::lock_guard<std::mutex> g(m_);
  if (on && (!st_.txArmed || !st_.connected)) return false;
  keyWanted_ = on;
  if (on) {
    post(K_TX, civ::ptt(addr_, true), "transmit", true);
    keyedByUs_ = true;
    keyedAt_ = nowMs();
    st_.tx = true;
    hold(K_TX);
  }
  // un-keying is done by serviceTx, which doesn't wait its turn
  return true;
}

bool Rig::tuneAtu() {
  std::lock_guard<std::mutex> g(m_);
  if (!st_.txArmed || !st_.connected) return false;
  post(K_ATU, civ::atu(addr_, 2), "start the tuner");
  st_.atu = 2;
  hold(K_ATU);
  return true;
}

bool Rig::sendCw(const std::string& text) {
  std::lock_guard<std::mutex> g(m_);
  if (!st_.txArmed || !st_.connected) return false;
  // Each chunk has its own key, so they queue in order rather than replacing each other.
  static uint32_t seq = 0;
  for (const std::string& chunk : civ::cwChunks(text))
    post(key(0x17, 0) | (++seq << 16), civ::cwText(addr_, chunk), "send CW");
  return true;
}

void Rig::stopCw() {
  std::lock_guard<std::mutex> g(m_);
  // Whatever's still queued mustn't follow the stop.
  queue_.erase(std::remove_if(queue_.begin(), queue_.end(), [](const Cmd& c) { return (c.key & 0xFF00) == (0x17u << 8); }),
               queue_.end());
  post(key(0x17, 0xFF), civ::cwStop(addr_), "stop CW", true);
}

bool Rig::playVoice(int n) {
  std::lock_guard<std::mutex> g(m_);
  if (n != 0 && (!st_.txArmed || !st_.connected)) return false;
  post(key(0x28), civ::voiceMemory(addr_, n), "play that voice memory");
  return true;
}

void Rig::setTxLimit(int seconds) {
  std::lock_guard<std::mutex> g(m_);
  st_.txLimit = std::max(5, seconds);
}

// ---- the rig thread ----

void Rig::buildPolls() {
  polls_.clear();
  auto add = [&](civ::Bytes b, int period, PollItem::When w = PollItem::ALWAYS) {
    polls_.push_back({std::move(b), period, 0, w});
  };
  uint8_t a = addr_;
  add(civ::frame(a, 0x19, {0x00}), 5000);
  add(civ::frame(a, 0x15, {civ::meterSub(civ::M_S)}), 100, PollItem::RX);
  for (int m : {civ::M_PO, civ::M_SWR, civ::M_ALC, civ::M_COMP, civ::M_ID})
    add(civ::frame(a, 0x15, {civ::meterSub(m)}), 150, PollItem::TX);
  add(civ::frame(a, 0x15, {civ::meterSub(civ::M_VD)}), 3000);
  add(civ::frame(a, 0x1C, {0x00}), 200);
  add(civ::frame(a, 0x03), 250);
  add(civ::frame(a, 0x04), 500);
  add(civ::frame(a, 0x1A, {0x06}), 700);
  add(civ::frame(a, 0x25, {0x01}), 800);
  add(civ::frame(a, 0x26, {0x01}), 1200);
  add(civ::frame(a, 0x1A, {0x03}), 800);
  // What a knob on the rig might have changed: seen within a few seconds.
  for (int l = 0; l < civ::L_COUNT; l++) add(civ::frame(a, 0x14, {civ::levelSub(l)}), 2500);
  for (int s = 0; s < civ::S_COUNT; s++) add(civ::frame(a, 0x16, {civ::switchSub(s)}), 2500);
  add(civ::frame(a, 0x11), 1500);
  add(civ::frame(a, 0x0F), 1000);
  add(civ::frame(a, 0x1C, {0x01}), 1000);
  add(civ::frame(a, 0x21, {0x00}), 1500);
  add(civ::frame(a, 0x21, {0x01}), 1500);
  add(civ::frame(a, 0x21, {0x02}), 1500);
  add(civ::frame(a, 0x27, {0x10}), 3000);
  add(civ::frame(a, 0x27, {0x11}), 3000);
  add(civ::frame(a, 0x27, {0x14, 0x00}), 3000);
  add(civ::frame(a, 0x27, {0x15, 0x00}), 3000);
  add(civ::frame(a, 0x27, {0x19, 0x00}), 3000);
  add(civ::frame(a, 0x27, {0x1A, 0x00}), 3000);
}

bool Rig::write(const civ::Bytes& b) {
  if (!tp_ || !tp_->isOpen()) return false;
  if (tp_->write(b.data(), (int)b.size())) return true;
  closeLink("Writing to the port failed (unplugged?)");
  return false;
}

void Rig::openLink() {
  std::string err;
  tp_.reset(factory_());
  if (tp_->open(err)) {
    reader_.reset();
    out_ = Outstanding();
    assemblingValid_ = false;
    lastFrameAt_ = 0;
    for (auto& p : polls_) p.last = 0;
    std::lock_guard<std::mutex> g(m_);
    st_.portOpen = true;
    st_.portError.clear();
  } else {
    tp_.reset();
    nextOpenAt_ = nowMs() + 2000;
    std::lock_guard<std::mutex> g(m_);
    st_.portOpen = false;
    st_.portError = err;
  }
}

void Rig::closeLink(const std::string& why) {
  if (tp_) tp_->close();
  tp_.reset();
  out_ = Outstanding();
  nextOpenAt_ = nowMs() + 2000;
  linkDown();
  std::lock_guard<std::mutex> g(m_);
  st_.portOpen = false;
  st_.portError = why;
}

void Rig::linkUp() {
  for (auto& p : polls_) p.last = 0;  // read everything again: it may have changed while we were away
  std::lock_guard<std::mutex> g(m_);
  st_.connected = true;
  // A power cycle forgets the data output, so it's asked for every time the rig comes back.
  if (wantScope_) {
    post(K_SCOPE_ON, civ::scopeOn(addr_, true), "turn the scope on");
    post(K_SCOPE_OUT, civ::scopeOutput(addr_, true), "start the scope data");
  }
}

void Rig::linkDown() {
  std::lock_guard<std::mutex> g(m_);
  st_.connected = false;
  // Stale commands mustn't fire when it returns, or the dial would jump. Un-keying isn't queued: see
  // serviceTx, which is still trying if the rig comes back.
  queue_.clear();
  holds_.clear();
  st_.tx = false;
  keyWanted_ = false;
}

void Rig::pumpFrame(const civ::Frame& f) {
  if (f.from != addr_) return;  // our own echo (CI-V USB Echo Back on), or another controller's
  if (f.to != civ::kPc && f.to != civ::kBroadcast) return;
  uint64_t now = nowMs();
  lastFrameAt_ = now;
  bool scopeData = f.cmd == 0x27 && !f.data.empty() && f.data[0] == 0x00;
  bool wasDown;
  {
    std::lock_guard<std::mutex> g(m_);
    wasDown = !st_.connected;
    if (f.cmd == civ::kNg) {
      if (out_.active && !out_.what.empty()) {
        st_.lastError = "The rig wouldn't " + out_.what;
        st_.lastErrorSeq++;
      }
    } else if (f.cmd != civ::kOk) {
      applyLocked(f);
    }
  }
  if (wasDown) linkUp();
  // Only a direct reply ends the wait, as the rig also sends things nobody asked for.
  if (out_.active && f.to == civ::kPc && !scopeData && (f.cmd == civ::kOk || f.cmd == civ::kNg || f.cmd == out_.cmd))
    out_.active = false;
}

void Rig::applyLocked(const civ::Frame& f) {
  uint64_t now = nowMs();
  const civ::Bytes& d = f.data;
  switch (f.cmd) {
    case 0x00:
    case 0x03:
      if (d.size() >= 5 && !held(K_FREQ, now)) {
        long long hz = civ::fromBcdLE(d.data(), 5);
        if (hz > 0) st_.hz = hz;
      }
      break;
    case 0x01:
    case 0x04:
      if (!d.empty() && !held(K_MODE, now)) {
        st_.mode = d[0];
        if (d.size() >= 2) st_.filter = d[1];
      }
      break;
    case 0x0F:
      if (!d.empty() && !held(K_SPLIT, now)) st_.split = d[0] == 1;
      break;
    case 0x11:
      if (!d.empty() && !held(K_ATT, now)) st_.att = d[0] != 0;
      break;
    case 0x14:
      if (d.size() >= 3)
        for (int l = 0; l < civ::L_COUNT; l++)
          if (civ::levelSub(l) == d[0] && !held(key(0x14, d[0]), now)) st_.level[l] = civ::fromBcdBE(&d[1], 2);
      break;
    case 0x15:
      if (d.size() >= 3)
        for (int m = 0; m < civ::M_COUNT; m++)
          if (civ::meterSub(m) == d[0]) st_.meter[m] = civ::fromBcdBE(&d[1], 2);
      break;
    case 0x16:
      if (d.size() >= 2)
        for (int s = 0; s < civ::S_COUNT; s++)
          if (civ::switchSub(s) == d[0] && !held(key(0x16, d[0]), now)) st_.sw[s] = civ::fromBcdBE(&d[1], 1);
      break;
    case 0x19:
      if (d.size() >= 2 && d[0] == 0x00) st_.rigId = d[1];
      break;
    case 0x1A:
      if (d.size() >= 2 && d[0] == 0x03 && !held(K_WIDTH, now)) st_.width = civ::fromBcdBE(&d[1], 1);
      if (d.size() >= 2 && d[0] == 0x06 && !held(K_DATA, now)) {
        st_.data = d[1] != 0;
        if (st_.data && d.size() >= 3 && d[2] && !held(K_MODE, now)) st_.filter = d[2];  // data mode's own filter
      }
      break;
    case 0x1C:
      if (d.size() >= 2 && d[0] == 0x00 && !held(K_TX, now)) {
        st_.tx = d[1] != 0;
        txReadAt_ = now;
      }
      if (d.size() >= 2 && d[0] == 0x01 && !held(K_ATU, now)) st_.atu = d[1];
      break;
    case 0x21:
      if (d.size() >= 4 && d[0] == 0x00 && !held(K_RIT, now)) {
        int v = (int)civ::fromBcdLE(&d[1], 2);
        st_.ritHz = d[3] ? -v : v;
      }
      if (d.size() >= 2 && d[0] == 0x01 && !held(K_RIT_ON, now)) st_.ritOn = d[1] != 0;
      if (d.size() >= 2 && d[0] == 0x02 && !held(K_XIT_ON, now)) st_.xitOn = d[1] != 0;
      break;
    case 0x25:
      if (d.size() >= 6 && d[0] == 0x01 && !held(K_OTHER_FREQ, now)) st_.otherHz = civ::fromBcdLE(&d[1], 5);
      if (d.size() >= 6 && d[0] == 0x00 && !held(K_FREQ, now)) st_.hz = civ::fromBcdLE(&d[1], 5);
      break;
    case 0x26:
      if (d.size() >= 4 && d[0] == 0x01 && !held(K_OTHER_MODE, now)) {
        st_.otherMode = d[1], st_.otherData = d[2] != 0, st_.otherFilter = d[3];
      }
      break;
    case 0x27:
      applyScopeLocked(f);
      break;
  }
}

void Rig::applyScopeLocked(const civ::Frame& f) {
  uint64_t now = nowMs();
  const civ::Bytes& d = f.data;
  if (d.empty()) return;
  switch (d[0]) {
    case 0x00: {
      // A sweep arrives in divisions, 1 of 11 (Hamlib's icom_parse_spectrum_frame):
      // 00 <scope> <division> <of> and then, in division 1 only, <mode> <5 BCD> <5 BCD> <out of range>.
      if (d.size() < 4) return;
      int division = civ::fromBcdBE(&d[2], 1), of = civ::fromBcdBE(&d[3], 1);
      const uint8_t* p = d.data() + 4;
      size_t n = d.size() - 4;
      if (division == 1) {
        if (n < 12) return;
        assembling_ = ScopeLine();
        assembling_.mode = p[0];
        long long a = civ::fromBcdLE(p + 1, 5), b = civ::fromBcdLE(p + 6, 5);
        if (p[0] == civ::SCOPE_CENTER) {
          assembling_.loHz = a - b, assembling_.hiHz = a + b;  // b is +/-
        } else {
          assembling_.loHz = a, assembling_.hiHz = b;
        }
        assembling_.outOfRange = p[11] != 0;
        p += 12, n -= 12;
        assemblingValid_ = true;
      }
      if (!assemblingValid_) return;  // joined part way through a sweep
      assembling_.levels.insert(assembling_.levels.end(), p, p + n);
      if (division == of) {
        assemblingValid_ = false;
        if (assembling_.levels.empty()) return;
        scopeLines_.push_back(std::move(assembling_));
        while (scopeLines_.size() > 32) scopeLines_.pop_front();  // the window's been busy or minimised
      }
      break;
    }
    case 0x10:
      if (d.size() >= 2 && !held(K_SCOPE_ON, now)) st_.scopeOn = d[1];
      break;
    case 0x11:
      if (d.size() >= 2 && !held(K_SCOPE_OUT, now)) st_.scopeOutput = d[1];
      break;
    case 0x14:
      if (d.size() >= 3 && !held(K_SCOPE_MODE, now)) st_.scopeMode = d[2];
      break;
    case 0x15:
      if (d.size() >= 7 && !held(K_SCOPE_SPAN, now)) st_.scopeSpan = civ::fromBcdLE(&d[2], 5) * 2;
      break;
    case 0x19:
      if (d.size() >= 5 && !held(K_SCOPE_REF, now)) {
        double db = civ::fromBcdBE(&d[2], 2) / 100.0;
        st_.scopeRef = d[4] ? -db : db;
        st_.scopeRefKnown = true;
      }
      break;
    case 0x1A:
      if (d.size() >= 3 && !held(K_SCOPE_SPEED, now)) st_.scopeSpeed = d[2];
      break;
  }
}

// PTT's safety. Runs every pass, ahead of the queue and without waiting for any reply.
void Rig::serviceTx(uint64_t now) {
  bool send = false;
  {
    std::lock_guard<std::mutex> g(m_);
    if (keyedByUs_ && keyWanted_) {
      st_.txSeconds = (int)((now - keyedAt_) / 1000);
      if (now - keyedAt_ >= (uint64_t)st_.txLimit * 1000) {
        keyWanted_ = false;  // the watchdog
        st_.lastError = "Transmit time limit reached: PTT released";
        st_.lastErrorSeq++;
        st_.txLimitHits++;
      }
    }
    if (keyedByUs_ && !keyWanted_) {
      // Keep saying it until the rig itself says it stopped: a reading taken after an un-key went out.
      // st_.tx alone won't do, as losing the link clears it while the rig may still be sending.
      if (now - lastUnkeySent_ >= kUnkeyRetryMs) send = true;
      bool confirmed = st_.connected && lastUnkeySent_ != 0 && txReadAt_ > lastUnkeySent_ && !st_.tx;
      if (confirmed) {
        keyedByUs_ = false;
        lastUnkeySent_ = 0;
        st_.txSeconds = 0;
        send = false;
      } else if (send) {
        lastUnkeySent_ = now;
        // The first send drops any queued PTT-on, so a late key-up can't undo this.
        queue_.erase(std::remove_if(queue_.begin(), queue_.end(), [](const Cmd& c) { return c.key == K_TX; }), queue_.end());
      }
    }
  }
  if (send) write(civ::ptt(addr_, false));
}

void Rig::sendNext(uint64_t now) {
  Cmd c;
  bool have = false;
  {
    std::lock_guard<std::mutex> g(m_);
    if (!queue_.empty()) {
      c = std::move(queue_.front());
      queue_.pop_front();
      have = true;
      hold(c.key);
    }
  }
  if (have) {
    uint8_t cmd = c.bytes.size() >= 5 ? c.bytes[c.bytes.size() >= 150 ? 154 : 4] : 0;
    if (write(c.bytes)) {
      out_.active = true, out_.cmd = cmd, out_.sentAt = now, out_.what = c.what;
      lastSendAt_ = now;
    }
    return;
  }
  if (now - lastSendAt_ < (uint64_t)kPollGapMs) return;
  bool tx;
  {
    std::lock_guard<std::mutex> g(m_);
    tx = st_.tx;
  }
  // The most overdue poll that applies now.
  PollItem* best = nullptr;
  int64_t bestLate = -1;
  for (PollItem& p : polls_) {
    if ((p.when == PollItem::RX && tx) || (p.when == PollItem::TX && !tx)) continue;
    int64_t late = (int64_t)(now - p.last) - p.periodMs;
    if (late >= 0 && late > bestLate) bestLate = late, best = &p;
  }
  if (!best) return;
  best->last = now;
  if (write(best->bytes)) {
    out_.active = true, out_.cmd = best->bytes[4], out_.sentAt = now, out_.what.clear();
    lastSendAt_ = now;
  }
}

void Rig::shutdownLink() {
  if (!tp_ || !tp_->isOpen()) return;
  bool mustUnkey, scope;
  {
    std::lock_guard<std::mutex> g(m_);
    mustUnkey = keyedByUs_ || st_.tx;
    scope = wantScope_ || st_.scopeOutput == 1;
  }
  // Written blind and more than once: there's no thread left afterwards to notice a lost frame.
  if (mustUnkey) {
    uint8_t b[64];
    for (int i = 0; i < 3 && tp_ && tp_->isOpen(); i++) {
      write(civ::ptt(addr_, false));
      if (tp_ && tp_->isOpen()) tp_->read(b, sizeof b, 40);
    }
  }
  if (scope) write(civ::scopeOutput(addr_, false));  // or the rig streams into a port nobody's reading
  if (tp_ && tp_->isOpen()) {
    uint8_t b[64];
    tp_->read(b, sizeof b, 30);
  }
  if (tp_) tp_->close();
  tp_.reset();
  std::lock_guard<std::mutex> g(m_);
  st_.portOpen = false;
  st_.connected = false;
}

void Rig::run() {
  uint8_t buf[512];
  std::vector<civ::Frame> frames;
  while (!quit_) {
    uint64_t now = nowMs();
    if (!tp_ || !tp_->isOpen()) {
      if (now >= nextOpenAt_) openLink();
      if (!tp_ || !tp_->isOpen()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        continue;
      }
    }
    int n = tp_->read(buf, sizeof buf, 3);
    if (n < 0) {
      closeLink("The port has gone (unplugged?)");
      continue;
    }
    if (n > 0) {
      frames.clear();
      reader_.feed(buf, (size_t)n, frames);
      for (const civ::Frame& f : frames) pumpFrame(f);
    }
    now = nowMs();
    serviceTx(now);
    if (!tp_ || !tp_->isOpen()) continue;

    if (out_.active && now - out_.sentAt > (uint64_t)kReplyTimeoutMs) out_.active = false;  // try the next thing
    bool connected;
    {
      std::lock_guard<std::mutex> g(m_);
      connected = st_.connected;
    }
    if (connected && lastFrameAt_ && now - lastFrameAt_ > (uint64_t)kLinkLostMs) linkDown();
    if (!out_.active) sendNext(now);
  }
  shutdownLink();
}
