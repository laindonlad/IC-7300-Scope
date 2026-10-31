// The parts below the window: CI-V bytes, frequency entry, the FFT, and Rig against a simulated 7300,
// including the ways a real link goes wrong. Exits non-zero if anything fails.
#include <math.h>
#include <stdio.h>

#include <chrono>
#include <functional>
#include <string>
#include <thread>

#include "bands.h"
#include "civ.h"
#include "dsp.h"
#include "rig.h"
#include "sim_rig.h"
#include "store.h"
#include "tuning.h"

static int failures = 0, checks = 0;
#define CHECK(c)                                          \
  do {                                                    \
    checks++;                                             \
    if (!(c)) {                                           \
      failures++;                                         \
      printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); \
    }                                                     \
  } while (0)

static std::string hex(const civ::Bytes& b) {
  std::string s;
  char t[4];
  for (uint8_t v : b) {
    snprintf(t, sizeof t, "%02X ", v);
    s += t;
  }
  if (!s.empty()) s.pop_back();
  return s;
}
#define CHECK_HEX(bytes, want)                                                  \
  do {                                                                          \
    std::string got = hex(bytes);                                               \
    checks++;                                                                   \
    if (got != (want)) {                                                        \
      failures++;                                                               \
      printf("  FAIL %s:%d  got [%s] want [%s]\n", __FILE__, __LINE__, got.c_str(), want); \
    }                                                                           \
  } while (0)

static void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
static bool waitFor(const std::function<bool()>& pred, int ms = 3000) {
  for (int t = 0; t < ms; t += 5) {
    if (pred()) return true;
    sleepMs(5);
  }
  return pred();
}

static void testBytes() {
  const uint8_t r = civ::kIc7300;
  // The IC-7300 manual's own worked forms.
  CHECK_HEX(civ::setFreq(r, 14074000), "FE FE 94 E0 05 00 40 07 14 00 FD");
  CHECK_HEX(civ::setFreq(r, 7123456), "FE FE 94 E0 05 56 34 12 07 00 FD");
  CHECK(civ::fromBcdLE(civ::bcdLE(74800000, 5).data(), 5) == 74800000);
  CHECK_HEX(civ::setMode(r, civ::CW_R, 2), "FE FE 94 E0 06 07 02 FD");
  CHECK_HEX(civ::setData(r, true, 1), "FE FE 94 E0 1A 06 01 01 FD");
  CHECK_HEX(civ::setData(r, false, 1), "FE FE 94 E0 1A 06 00 00 FD");
  CHECK_HEX(civ::setLevel(r, civ::L_AF, 128), "FE FE 94 E0 14 01 01 28 FD");
  CHECK_HEX(civ::setLevel(r, civ::L_POWER, 255), "FE FE 94 E0 14 0A 02 55 FD");
  CHECK_HEX(civ::setSwitch(r, civ::S_NB, 1), "FE FE 94 E0 16 22 01 FD");
  CHECK_HEX(civ::setSwitch(r, civ::S_PREAMP, 2), "FE FE 94 E0 16 02 02 FD");
  CHECK_HEX(civ::setAtt(r, true), "FE FE 94 E0 11 20 FD");
  CHECK_HEX(civ::setSplit(r, true), "FE FE 94 E0 0F 01 FD");
  CHECK_HEX(civ::vfoEqual(r), "FE FE 94 E0 07 A0 FD");
  CHECK_HEX(civ::vfoSwap(r), "FE FE 94 E0 07 B0 FD");
  CHECK_HEX(civ::ptt(r, true), "FE FE 94 E0 1C 00 01 FD");
  CHECK_HEX(civ::atu(r, 2), "FE FE 94 E0 1C 01 02 FD");
  CHECK_HEX(civ::setRit(r, -125), "FE FE 94 E0 21 00 25 01 01 FD");
  CHECK_HEX(civ::setRit(r, 1234), "FE FE 94 E0 21 00 34 12 00 FD");
  CHECK_HEX(civ::setWidth(r, 34), "FE FE 94 E0 1A 03 34 FD");
  CHECK_HEX(civ::voiceMemory(r, 3), "FE FE 94 E0 28 00 03 FD");
  CHECK_HEX(civ::setOtherFreq(r, 14200000), "FE FE 94 E0 25 01 00 00 20 14 00 FD");
  CHECK_HEX(civ::setOtherMode(r, civ::USB, true, 2), "FE FE 94 E0 26 01 01 01 02 FD");
  CHECK_HEX(civ::scopeOutput(r, true), "FE FE 94 E0 27 11 01 FD");
  CHECK_HEX(civ::scopeSpan(r, 50000), "FE FE 94 E0 27 15 00 00 50 02 00 00 FD");
  CHECK_HEX(civ::scopeRef(r, -10.5), "FE FE 94 E0 27 19 00 10 50 01 FD");
  CHECK_HEX(civ::scopeRef(r, 5), "FE FE 94 E0 27 19 00 05 00 00 FD");
  CHECK_HEX(civ::scopeMode(r, civ::SCOPE_FIXED), "FE FE 94 E0 27 14 00 01 FD");
  CHECK_HEX(civ::cwStop(r), "FE FE 94 E0 17 FF FD");
  CHECK_HEX(civ::cwText(r, "CQ"), "FE FE 94 E0 17 43 51 FD");

  auto chunks = civ::cwChunks("cq de m0abc! 1234567890 1234567890 1234567890");
  CHECK(chunks.size() == 2);
  CHECK(chunks[0].size() == 30);
  CHECK(chunks[0].substr(0, 11) == "CQ DE M0ABC");
  CHECK(chunks[0].find('!') == std::string::npos);

  CHECK(civ::widthHz(civ::USB, 34) == 3000);
  CHECK(civ::widthHz(civ::USB, 0) == 50);
  CHECK(civ::widthHz(civ::CW, 9) == 500);
  CHECK(civ::widthHz(civ::USB, 10) == 600);
  CHECK(civ::widthHz(civ::USB, 40) == 3600);
  CHECK(civ::widthHz(civ::AM, 49) == 10000);
  CHECK(civ::widthHz(civ::RTTY, 31) == 2700);

  CHECK(civ::smeterDb(120) == 0);
  CHECK(civ::powerWatts(213) == 100);
  CHECK(civ::powerWatts(143) == 50);
  CHECK(civ::swrRatio(80) == 2.0);
  CHECK(fabs(civ::voltsVd(13) - 10) < 1e-9);
}

static void testReader() {
  civ::Reader rd;
  std::vector<civ::Frame> out;
  auto feed = [&](std::initializer_list<uint8_t> b) {
    std::vector<uint8_t> v(b);
    rd.feed(v.data(), v.size(), out);
  };
  feed({0xFE, 0xFE, 0xE0, 0x94, 0x03, 0x00, 0x40, 0x07, 0x14, 0x00, 0xFD});
  CHECK(out.size() == 1 && out[0].cmd == 0x03 && out[0].from == 0x94 && out[0].to == 0xE0 && out[0].data.size() == 5);
  out.clear();
  feed({0xFE, 0xFE, 0xE0});  // split across reads
  feed({0x94, 0xFB});
  feed({0xFD});
  CHECK(out.size() == 1 && out[0].cmd == 0xFB);
  out.clear();
  feed({0x12, 0x34, 0xFD, 0xFE, 0xFE, 0xE0, 0x94, 0xFB, 0xFD});  // junk first
  CHECK(out.size() == 1);
  out.clear();
  feed({0xFE, 0xFE, 0xE0, 0x94, 0x03, 0xFC, 0xFD});  // a collision
  CHECK(out.empty());
  feed({0xFE, 0xFE, 0xE0, 0x94, 0x03, 0x11, 0xFE, 0xFE, 0xE0, 0x94, 0xFB, 0xFD});  // cut off by a new frame
  CHECK(out.size() == 1 && out[0].cmd == 0xFB);
}

static void testTuning() {
  using tuning::parseFreq;
  long long d = 14074000;
  CHECK(parseFreq("14.074", d) == 14074000);
  CHECK(parseFreq("14074", d) == 14074000);
  CHECK(parseFreq("14074.5", d) == 14074500);
  CHECK(parseFreq("14074000", d) == 14074000);
  CHECK(parseFreq("7", d) == 7000000);
  CHECK(parseFreq("3.573", d) == 3573000);
  CHECK(parseFreq("3573", d) == 3573000);
  CHECK(parseFreq("198", d) == 198000);
  CHECK(parseFreq("198k", d) == 198000);
  CHECK(parseFreq("5m", d) == 5000000);
  CHECK(parseFreq("14.074.123", d) == 14074123);
  CHECK(parseFreq("50.313", d) == 50313000);
  CHECK(parseFreq("70.1", d) == 70100000);
  CHECK(parseFreq("+5", d) == 14079000);
  CHECK(parseFreq("-1.5", d) == 14072500);
  CHECK(parseFreq("", d) == -1);
  CHECK(parseFreq("abc", d) == -1);
  CHECK(parseFreq("14.0a", d) == -1);
  CHECK(parseFreq("0", d) == -1);
  CHECK(parseFreq("144.8m", d) == -1);   // beyond the rig
  CHECK(parseFreq("10k", d) == -1);      // below it

  CHECK(tuning::formatHz(14074000) == "14.074.000");
  CHECK(tuning::formatHz(7000500) == "7.000.500");
  CHECK(tuning::stepFreq(14074123, 1000, 1) == 14075000);
  CHECK(tuning::stepFreq(14074123, 1000, -1) == 14074000);
  CHECK(tuning::stepFreq(14074000, 1000, -1) == 14073000);
  CHECK(tuning::stepFreq(14074000, 1000, 3) == 14077000);
  CHECK(tuning::stepFreq(30000, 1000, -1) == 30000);

  // A 1kHz tone: 1kHz above the dial in USB, below in LSB.
  CHECK(tuning::rfFromAudio(14000000, civ::USB, 1000, 600) == 14001000);
  CHECK(tuning::rfFromAudio(7000000, civ::LSB, 1000, 600) == 6999000);
  // CW: a station at the pitch is on the dial; 100Hz higher in the audio is 100Hz up (CW) or down (CW-R).
  CHECK(tuning::rfFromAudio(14000000, civ::CW, 600, 600) == 14000000);
  CHECK(tuning::rfFromAudio(14000000, civ::CW, 700, 600) == 14000100);
  CHECK(tuning::rfFromAudio(14000000, civ::CW_R, 700, 600) == 13999900);
  CHECK(tuning::rfFromAudio(14000000, civ::AM, 700, 600) == -1);
  // Moving the dial so the signal lands at the target tone, then checking it did.
  long long dial = tuning::dialToCentre(14000000, civ::USB, 2000, 1500);
  CHECK(dial == 14000500);
  CHECK(tuning::rfFromAudio(dial, civ::USB, 1500, 600) == tuning::rfFromAudio(14000000, civ::USB, 2000, 600));
  dial = tuning::dialToCentre(7000000, civ::LSB, 2000, 1500);
  CHECK(dial == 6999500);
  CHECK(tuning::rfFromAudio(dial, civ::LSB, 1500, 600) == tuning::rfFromAudio(7000000, civ::LSB, 2000, 600));
  dial = tuning::dialToCentre(14000000, civ::CW, 800, 600);
  CHECK(tuning::rfFromAudio(dial, civ::CW, 600, 600) == tuning::rfFromAudio(14000000, civ::CW, 800, 600));

  CHECK(tuning::cwPitchHz(0) == 300);
  CHECK(tuning::cwPitchHz(255) == 900);
  CHECK(tuning::keySpeedWpm(255) == 48);
  CHECK(tuning::keySpeedWpm(tuning::keySpeedRaw(20)) == 20);

  CHECK(bands::bandOf(14074000) == 5);
  CHECK(bands::bandOf(15000000) == -1);
  int n;
  const bands::Band* t = bands::table(n);
  for (int i = 0; i < n; i++) {
    CHECK(t[i].homeHz >= t[i].lo && t[i].homeHz <= t[i].hi);
    for (const auto& p : t[i].presets)
      if (p.name) CHECK(p.hz >= t[i].lo && p.hz <= t[i].hi);
    for (const auto& s : t[i].segments)
      if (s.hi) CHECK(s.lo >= t[i].lo && s.hi <= t[i].hi && s.lo < s.hi);
  }
}

static void testDsp() {
  int n = 4096;
  dsp::Spectrum sp(n);
  std::vector<float> in((size_t)n), db((size_t)sp.bins());
  // A tone exactly on bin 256 (3kHz at 48kHz), half scale.
  for (int i = 0; i < n; i++) in[(size_t)i] = 0.5f * (float)sin(2 * M_PI * 256 * i / n);
  sp.compute(in.data(), db.data());
  int peak = 0;
  for (int i = 0; i < sp.bins(); i++)
    if (db[(size_t)i] > db[(size_t)peak]) peak = i;
  CHECK(peak == 256);
  CHECK(fabs(db[(size_t)peak] - (-6.02f)) < 0.1f);
  CHECK(db[300] < -60.f);  // and clean away from it
  for (auto& v : in) v = 0;
  sp.compute(in.data(), db.data());
  CHECK(db[100] == -160.f);
  CHECK(dsp::dbToLevel(0) == 255 && dsp::dbToLevel(-200) == 0);
  CHECK(fabs(dsp::levelToDb(dsp::dbToLevel(-70)) - (-70)) < 1.f);
}

static void testStore() {
  store::LogEntry e;
  e.utc = "2026-10-31 19:05";
  e.call = "G4ABC";
  e.hz = 14205000;
  e.mode = "USB";
  e.rstSent = "59";
  e.rstRcvd = "57";
  e.name = "Sam";
  e.notes = "first QSO";
  std::string a = store::adifRecord(e, "M0XYZ");
  CHECK(a.find("<CALL:5>G4ABC") != std::string::npos);
  CHECK(a.find("<QSO_DATE:8>20261031") != std::string::npos);
  CHECK(a.find("<TIME_ON:4>1905") != std::string::npos);
  CHECK(a.find("<FREQ:9>14.205000") != std::string::npos);
  CHECK(a.find("<BAND:3>20M") != std::string::npos);
  CHECK(a.find("<MODE:3>SSB") != std::string::npos);
  CHECK(a.find("<STATION_CALLSIGN:5>M0XYZ") != std::string::npos);
  CHECK(a.find("<EOR>") != std::string::npos);
  e.mode = "USB-D";
  CHECK(store::adifRecord(e, "").find("<MODE") == std::string::npos);  // could be any digital mode: not guessed
  CHECK(store::adifRecord(e, "").find("rig mode USB-D") != std::string::npos);
  e.mode = "CW";
  CHECK(store::adifRecord(e, "").find("<MODE:2>CW") != std::string::npos);
  e.hz = 13000000;
  CHECK(store::adifRecord(e, "").find("<BAND") == std::string::npos);  // no band, no guess

  std::vector<std::string> f = store::splitCsv(store::joinCsv({"a,b", "c\"d", "", "e"}));
  CHECK(f.size() == 4 && f[0] == "a,b" && f[1] == "c\"d" && f[2] == "" && f[3] == "e");
}

struct Harness {
  SimRig sim;
  Rig rig;
  Harness() {
    rig.start([this] { return sim.newTransport(); });
  }
  bool up() {
    return waitFor([&] {
      RigState s = rig.state();
      return s.connected && s.hz > 0 && s.mode >= 0;
    });
  }
};

static void testRigBasics() {
  Harness h;
  CHECK(h.up());
  RigState s = h.rig.state();
  CHECK(s.hz == 14074000);
  CHECK(s.mode == civ::USB);
  CHECK(waitFor([&] { return h.rig.state().rigId == 0x94; }));
  CHECK(waitFor([&] { return h.rig.state().level[civ::L_AF] == 90; }));
  CHECK(waitFor([&] { return h.rig.state().sw[civ::S_AGC] == 2; }));
  CHECK(waitFor([&] { return h.rig.state().data; }));
  CHECK(waitFor([&] { return h.rig.state().otherHz == 14200000; }));
  CHECK(waitFor([&] { return h.rig.state().meter[civ::M_S] >= 0; }));

  h.rig.setFreq(7074000);
  CHECK(h.rig.state().hz == 7074000);  // shown at once
  CHECK(waitFor([&] { return h.sim.hz() == 7074000; }));

  h.rig.setMode(civ::LSB, false, 2);
  CHECK(waitFor([&] { return h.sim.mode() == civ::LSB && h.sim.filter() == 2 && !h.sim.data(); }));
  h.rig.setMode(civ::USB, true, 1);
  CHECK(waitFor([&] { return h.sim.mode() == civ::USB && h.sim.data(); }));
  h.rig.setMode(civ::CW, true, 1);  // CW has no data mode
  CHECK(waitFor([&] { return h.sim.mode() == civ::CW; }));
  CHECK(!h.rig.state().data);

  h.rig.setLevel(civ::L_RF, 200);
  CHECK(waitFor([&] { return !h.sim.received(0x14).empty(); }));

  // A swap shows at once and sticks.
  h.rig.setFreq(14074000);
  h.rig.setOtherFreq(14300000);
  CHECK(waitFor([&] { return h.sim.hz() == 14074000; }));
  sleepMs(50);
  h.rig.vfoSwap();
  CHECK(h.rig.state().hz == 14300000);
  CHECK(waitFor([&] { return h.sim.hz() == 14300000; }));
}

// A dial spun as fast as a mouse wheel can go: no backlog, and the last one wins.
static void testCoalesce() {
  Harness h;
  CHECK(h.up());
  long long hz = 14000000;
  int sends = 500;
  for (int i = 0; i < sends; i++) {
    hz += 10;
    h.rig.setFreq(hz);
  }
  CHECK(waitFor([&] { return h.sim.hz() == hz; }));
  printf("  %d dial moves reached the rig as %d commands\n", sends, h.sim.setFreqCount());
  CHECK(h.sim.setFreqCount() < sends / 5);
}

// While a move's on its way, a stale reading mustn't put the display back.
static void testStaleReadings() {
  Harness h;
  CHECK(h.up());
  for (int i = 0; i < 40; i++) {
    long long want = 14000000 + i * 1000;
    h.rig.setFreq(want);
    CHECK(h.rig.state().hz == want);
    sleepMs(8);
    CHECK(h.rig.state().hz == want);
  }
  // ...but the hold ends, and a knob turned on the radio shows up, with or without a transceive report.
  sleepMs(500);
  h.sim.spinDial(21074000, false);
  CHECK(waitFor([&] { return h.rig.state().hz == 21074000; }, 1500));
  h.sim.spinDial(21100000, true);
  CHECK(waitFor([&] { return h.rig.state().hz == 21100000; }, 300));
}

static void testEcho() {
  Harness h;
  h.sim.setEcho(true);
  CHECK(h.up());
  h.rig.setFreq(10136000);
  CHECK(waitFor([&] { return h.sim.hz() == 10136000; }));
  CHECK(waitFor([&] { return h.rig.state().meter[civ::M_S] >= 0; }));
  sleepMs(600);
  CHECK(h.rig.state().hz == 10136000);
}

static void testRefused() {
  Harness h;
  CHECK(h.up());
  uint64_t before = h.rig.state().lastErrorSeq;
  h.sim.setRefuseNext(0x05);
  h.rig.setFreq(14000000);
  CHECK(waitFor([&] { return h.rig.state().lastErrorSeq > before; }));
  CHECK(h.rig.state().lastError.find("wouldn't") != std::string::npos);
  // and the display goes back to the truth
  CHECK(waitFor([&] { return h.rig.state().hz == 14074000; }, 1500));
  // a refused poll is nobody's business
  before = h.rig.state().lastErrorSeq;
  h.sim.setRefuseNext(0x03);
  sleepMs(400);
  CHECK(h.rig.state().lastErrorSeq == before);
}

static void testScope() {
  Harness h;
  CHECK(h.up());
  CHECK(!h.sim.scopeOutput());
  h.rig.setScope(true);
  CHECK(waitFor([&] { return h.sim.scopeOutput(); }));
  h.rig.setScopeSpan(50000);
  CHECK(waitFor([&] { return !h.sim.received(0x27).empty(); }));
  ScopeLine l;
  int got = 0;
  bool fine = true;
  CHECK(waitFor([&] {
    while (h.rig.popScope(l)) {
      got++;
      if ((int)l.levels.size() != civ::kScopePoints) fine = false;
    }
    return got >= 5;
  }));
  CHECK(fine);
  CHECK(l.hiHz - l.loHz == 50000);
  CHECK((l.loHz + l.hiHz) / 2 == h.sim.hz());
  CHECK(!l.outOfRange);
  // The scope traffic mustn't crowd out commands.
  h.rig.setFreq(14100000);
  CHECK(waitFor([&] { return h.sim.hz() == 14100000; }, 300));
  // Fixed mode's frame gives the edges directly.
  h.rig.setScopeMode(civ::SCOPE_FIXED);
  CHECK(waitFor([&] {
    return h.rig.popScope(l) && l.mode == civ::SCOPE_FIXED && l.loHz == 14000000 && l.hiHz == 14050000;
  }));
  // Stopping leaves the rig quiet.
  h.rig.stop();
  CHECK(!h.sim.scopeOutput());
}

static void testTxGuards() {
  Harness h;
  CHECK(h.up());
  // Disarmed: nothing keys, by any route.
  CHECK(!h.rig.ptt(true));
  CHECK(!h.rig.tuneAtu());
  CHECK(!h.rig.sendCw("CQ"));
  CHECK(!h.rig.playVoice(1));
  sleepMs(100);
  CHECK(!h.sim.tx());
  CHECK(h.sim.received(0x17).empty());
  CHECK(h.sim.received(0x28).empty());

  h.rig.armTx(true);
  CHECK(h.rig.ptt(true));
  CHECK(waitFor([&] { return h.sim.tx(); }));
  CHECK(h.rig.state().tx);
  CHECK(h.rig.ptt(false));
  CHECK(waitFor([&] { return !h.sim.tx(); }));
  CHECK(waitFor([&] { return !h.rig.state().tx; }));

  // Disarming lets go too.
  CHECK(h.rig.ptt(true));
  CHECK(waitFor([&] { return h.sim.tx(); }));
  h.rig.armTx(false);
  CHECK(waitFor([&] { return !h.sim.tx(); }));
  CHECK(!h.rig.ptt(true));

  // CW goes in order and is cut to frame size, and stop drops the rest.
  h.rig.armTx(true);
  CHECK(h.rig.sendCw("cq cq cq de m0abc m0abc m0abc k k k"));
  CHECK(waitFor([&] { return h.sim.received(0x17).size() == 2; }));
  auto cw = h.sim.received(0x17);
  CHECK(cw[0].size() == 30 && cw[0][0] == 'C');
}

static void testWatchdog() {
  Harness h;
  CHECK(h.up());
  h.rig.armTx(true);
  h.rig.setTxLimit(5);  // the shortest allowed
  CHECK(h.rig.ptt(true));
  CHECK(waitFor([&] { return h.sim.tx(); }));
  CHECK(waitFor([&] { return h.rig.state().txSeconds >= 1; }, 2000));
  CHECK(h.sim.tx());
  CHECK(waitFor([&] { return !h.sim.tx(); }, 6000));
  RigState s = h.rig.state();
  CHECK(s.txLimitHits == 1);
  CHECK(s.lastError.find("limit") != std::string::npos);
}

// The rig ignores the un-key (a lost frame): the app must keep trying until it stops.
static void testUnkeyRetry() {
  Harness h;
  CHECK(h.up());
  h.rig.armTx(true);
  CHECK(h.rig.ptt(true));
  CHECK(waitFor([&] { return h.sim.tx(); }));
  h.sim.setIgnoreUnkey(true);
  h.rig.ptt(false);
  sleepMs(900);
  CHECK(h.sim.tx());
  int tries = h.sim.pttOffCount();
  printf("  un-key sent %d times in 900ms while the rig ignored it\n", tries);
  CHECK(tries >= 3);
  h.sim.setIgnoreUnkey(false);
  CHECK(waitFor([&] { return !h.sim.tx(); }));
  // Confirmed off, so it stops sending.
  CHECK(waitFor([&] { return !h.rig.state().tx; }));
  sleepMs(400);
  int settled = h.sim.pttOffCount();
  sleepMs(600);
  CHECK(h.sim.pttOffCount() == settled);
}

// The rig goes silent while keyed, then returns: un-key must still reach it.
static void testUnkeyAcrossOutage() {
  Harness h;
  CHECK(h.up());
  h.rig.armTx(true);
  CHECK(h.rig.ptt(true));
  CHECK(waitFor([&] { return h.sim.tx(); }));
  h.sim.setSilent(true);
  CHECK(waitFor([&] { return !h.rig.state().connected; }, 4000));
  CHECK(h.sim.tx());  // the sim has heard nothing: it's still sending
  h.sim.setSilent(false);
  CHECK(waitFor([&] { return !h.sim.tx(); }, 3000));
}

static void testShutdownUnkeys() {
  Harness h;
  CHECK(h.up());
  h.rig.armTx(true);
  CHECK(h.rig.ptt(true));
  CHECK(waitFor([&] { return h.sim.tx(); }));
  h.rig.stop();
  CHECK(!h.sim.tx());
}

static void testLinkLoss() {
  Harness h;
  CHECK(h.up());
  h.rig.setScope(true);
  CHECK(waitFor([&] { return h.sim.scopeOutput(); }));
  // Rig switched off: the port stays open but nothing answers.
  h.sim.setSilent(true);
  CHECK(waitFor([&] { return !h.rig.state().connected; }, 4000));
  CHECK(h.rig.state().portOpen);
  h.rig.setFreq(3573000);  // must not fire when it comes back
  h.sim.setSilent(false);
  CHECK(waitFor([&] { return h.rig.state().connected; }));
  sleepMs(300);
  CHECK(h.sim.hz() == 14074000);
  CHECK(waitFor([&] { return h.rig.state().hz == 14074000; }));

  // Cable out: the port itself fails, then reopens.
  h.sim.setPortFails(true);
  CHECK(waitFor([&] { return !h.rig.state().portOpen; }));
  CHECK(!h.rig.state().portError.empty());
  h.sim.setPortFails(false);
  CHECK(waitFor([&] { return h.rig.state().connected; }, 5000));
  CHECK(h.rig.state().portError.empty());
  // The scope comes back with it.
  ScopeLine l;
  while (h.rig.popScope(l)) {}
  CHECK(waitFor([&] { return h.rig.popScope(l); }));
}

static void testPortBusyMessage() {
  // Not simulated: a port that can't exist fails with a useful reason and doesn't hang.
  std::string err;
  std::unique_ptr<Transport> t(newSerialTransport("COM250-not-real", 115200));
  CHECK(!t->open(err));
  CHECK(!err.empty());
  CHECK(!t->isOpen());
}

int main() {
  struct {
    const char* name;
    void (*fn)();
  } tests[] = {
      {"CI-V bytes", testBytes}, {"frame reader", testReader}, {"frequency entry", testTuning}, {"FFT", testDsp},
      {"log and CSV", testStore}, {"rig basics", testRigBasics}, {"dial spin", testCoalesce},
      {"stale readings", testStaleReadings}, {"echo back", testEcho}, {"refused commands", testRefused},
      {"scope", testScope}, {"TX guards", testTxGuards}, {"TX watchdog", testWatchdog}, {"un-key retry", testUnkeyRetry},
      {"un-key across an outage", testUnkeyAcrossOutage}, {"un-key at shutdown", testShutdownUnkeys},
      {"link loss", testLinkLoss}, {"port errors", testPortBusyMessage},
  };
  for (auto& t : tests) {
    int before = failures;
    printf("%s\n", t.name);
    t.fn();
    if (failures == before) printf("  ok\n");
  }
  printf("\n%d checks, %d failed\n", checks, failures);
  return failures ? 1 : 0;
}
