#pragma once
// A pretend IC-7300, IC-9700 or IC-R8600 on the far end of a Transport, speaking CI-V as Rig sends it. It drives the
// tests and --demo. It's written from the same reading of the protocol as rig.cpp, so passing against it
// shows the app is self-consistent and handles replies, echoes and faults. Only ic7300probe, against the
// real radio, shows the protocol is right.
#include <stdint.h>

#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include "civ.h"
#include "transport.h"

// What's on the air around a frequency, the same for the sim's scope and its audio.
struct SimSignal {
  long long hz;
  float db;      // relative to noise
  int kind;      // 0 CW, 1 SSB, 2 narrow digital
};
std::vector<SimSignal> simSignals(long long loHz, long long hiHz);

class SimRig {
 public:
  struct Shared;
  explicit SimRig(const civ::Radio& radio = civ::kRadio7300);
  ~SimRig();
  Transport* newTransport();  // each lives as long as Rig wants; the radio's state carries on

  // What the rig has now.
  long long hz();
  int mode();
  bool data();
  int filter();
  bool tx();
  bool scopeOutput();
  long long otherHz();
  int otherMode();
  int att();
  int antenna();
  bool ipPlus();
  bool mainSubExchanged();  // a 07 B0 reached a 9700, which moves its bands round under the operator
  int setFreqCount();    // how many 05s arrived
  int pttOffCount();
  std::vector<civ::Bytes> received(uint8_t cmd);  // the frames with that command, oldest first
  int refusals();        // how many NGs it has sent. A receiver is asked only for what it has

  // Doing things behind the app's back, as a knob on the rig would.
  void spinDial(long long hz, bool transceiveReport);
  void pressPtt(bool on);

  // Faults.
  void setEcho(bool on);
  void setSilent(bool silent);     // rig off: nothing comes back at all
  void setRefuseNext(uint8_t cmd); // answer that command with NG, once
  void setIgnoreUnkey(bool on);    // a lost frame, as far as the app can tell
  void setPortFails(bool on);      // the USB cable's out
  void setCivAddress(uint8_t addr, bool idFollows);  // changed in its menu; does 19 00 report the new one?

 private:
  std::shared_ptr<Shared> s_;
};
