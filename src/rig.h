#pragma once
// The rig, from the window's side. One thread owns the port: it sends what's asked, polls the rest
// round-robin, and reads whatever the rig sends, keeping RigState up to date.
//
// Every setter returns at once and shows its result in state() at once, so the window never waits on
// the radio. Queued commands of the same kind replace each other, so a spun dial can't build a backlog.
// While a command is on its way, older readings of that value are ignored or the display would jump back.
//
// Transmit is guarded here and not in the window, so no caller can get round it: nothing keys until
// armTx(true), PTT has a time limit, and the thread keeps sending un-key until the rig says it stopped.
#include <stdint.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "civ.h"
#include "transport.h"

struct RigState {
  bool portOpen = false, connected = false;  // connected: the rig has answered lately
  std::string portError;
  std::string lastError;          // the latest refused command; cleared by the next success
  uint64_t lastErrorSeq = 0;

  long long hz = -1, otherHz = -1;     // the selected VFO, and the other one
  int mode = -1, filter = 0;           // civ::Mode, FIL1-3
  bool data = false;
  int otherMode = -1, otherFilter = 0;
  bool otherData = false;
  int width = -1;                      // 1A 03's code; civ::widthHz turns it into Hz
  int split = -1, att = -1, atu = -1;  // atu: 0 off, 1 in line, 2 tuning
  int ritOn = -1, xitOn = -1, ritHz = 0;
  bool tx = false;
  int level[civ::L_COUNT];   // 0-255, -1 not known yet
  int sw[civ::S_COUNT];
  int meter[civ::M_COUNT];
  int rigId = -1;            // what 19 00 says (94h on a 7300)

  int scopeOn = -1, scopeOutput = -1, scopeMode = -1, scopeSpeed = -1;
  long long scopeSpan = -1;
  double scopeRef = 0;
  bool scopeRefKnown = false;

  bool txArmed = false;
  int txSeconds = 0;    // how long we have held PTT
  int txLimit = 180;    // the watchdog, seconds
  uint64_t txLimitHits = 0;

  RigState();
};

// One sweep of the rig's scope.
struct ScopeLine {
  long long loHz = 0, hiHz = 0;
  int mode = 0;
  bool outOfRange = false;
  std::vector<uint8_t> levels;  // 0..civ::kScopeLevelMax
};

class Rig {
 public:
  Rig();
  ~Rig();  // lets go of PTT first

  // Connects via what factory makes, and retries for as long as it runs. Replaces any earlier link.
  void start(std::function<Transport*()> factory, uint8_t addr = civ::kIc7300);
  void stop();
  bool running() const { return thread_.joinable(); }

  RigState state() const;
  bool popScope(ScopeLine& out);  // false when there's no new sweep

  void setFreq(long long hz);
  void setOtherFreq(long long hz);
  void setMode(int mode, bool data, int filter);
  void setOtherMode(int mode, bool data, int filter);
  void setLevel(int level, int v);
  void setSwitch(int sw, int v);
  void setAtt(bool on);
  void setSplit(bool on);
  void setWidth(int code);
  void setRit(int hz);
  void setRitOn(bool on);
  void setXitOn(bool on);
  void vfoEqual();
  void vfoSwap();
  void setAtu(bool on);
  void powerOn();
  void powerOff();

  void setScope(bool output);  // turns the sweep stream on or off; survives reconnects
  void setScopeSpan(long long hz);
  void setScopeMode(int mode);
  void setScopeRef(double db);
  void setScopeSpeed(int s);

  // Transmit. All refused (false) until armed.
  void armTx(bool on);
  bool ptt(bool on);
  bool tuneAtu();  // the 7300 transmits while it tunes
  bool sendCw(const std::string& text);
  void stopCw();
  bool playVoice(int n);
  void setTxLimit(int seconds);

 private:
  struct Cmd {
    uint32_t key;
    civ::Bytes bytes;
    std::string what;
  };
  struct PollItem {
    civ::Bytes bytes;
    int periodMs;
    uint64_t last;
    enum When { ALWAYS, RX, TX } when;
  };
  struct Outstanding {
    bool active = false;
    uint8_t cmd = 0;
    uint64_t sentAt = 0;
    std::string what;  // empty for a poll, whose refusal isn't worth reporting
  };

  void run();
  void buildPolls();
  void openLink();
  void closeLink(const std::string& why);
  void pumpFrame(const civ::Frame& f);
  void applyLocked(const civ::Frame& f);
  void applyScopeLocked(const civ::Frame& f);
  void sendNext(uint64_t now);
  bool write(const civ::Bytes& b);
  void serviceTx(uint64_t now);
  void linkUp();
  void linkDown();
  void shutdownLink();

  void post(uint32_t key, civ::Bytes bytes, const char* what, bool atFront = false);
  void hold(uint32_t key);
  bool held(uint32_t key, uint64_t now) const;

  mutable std::mutex m_;
  RigState st_;
  std::deque<Cmd> queue_;
  std::deque<ScopeLine> scopeLines_;
  std::vector<std::pair<uint32_t, uint64_t>> holds_;
  bool wantScope_ = false;
  bool keyWanted_ = false, keyedByUs_ = false;
  uint64_t keyedAt_ = 0, lastUnkeySent_ = 0, txReadAt_ = 0;

  std::function<Transport*()> factory_;
  std::unique_ptr<Transport> tp_;
  uint8_t addr_ = civ::kIc7300;
  std::atomic<bool> quit_{false};
  std::thread thread_;

  // the thread's own
  civ::Reader reader_;
  Outstanding out_;
  std::vector<PollItem> polls_;
  uint64_t lastFrameAt_ = 0, lastSendAt_ = 0, nextOpenAt_ = 0;
  ScopeLine assembling_;
  bool assemblingValid_ = false;
};
