// The parts below the window: CI-V bytes, frequency entry, the FFT, and Rig against a simulated 7300,
// including the ways a real link goes wrong. Exits non-zero if anything fails.
#include <math.h>
#include <stdio.h>

#include <map>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <thread>

#include "bands.h"
#include "civ.h"
#include "clock_sync.h"
#include "dsp.h"
#include "dxcluster.h"
#include "needle.h"
#include "rig.h"
#include "scan.h"
#include "sim_rig.h"
#include "store.h"
#include "tuning.h"
#include "update_check.h"

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
  // Tone rows, against the IC-7300 Full Manual's table. 10 is BCD, for +5.
  CHECK_HEX(civ::setTone(r, civ::TONE_RX_SSB_BASS, 5), "FE FE 94 E0 1A 05 00 02 05 FD");
  CHECK_HEX(civ::setTone(r, civ::TONE_TX_SSB_TREBLE, 10), "FE FE 94 E0 1A 05 00 13 10 FD");
  CHECK_HEX(civ::setTone(r, civ::TONE_TX_FM_TREBLE, 0), "FE FE 94 E0 1A 05 00 20 00 FD");
  CHECK_HEX(civ::readTone(r, civ::TONE_RX_FM_TREBLE), "FE FE 94 E0 1A 05 00 09 FD");
  CHECK(civ::toneWritable(civ::TONE_TX_FM_TREBLE, civ::kIc7300));
  CHECK(civ::toneWritable(civ::TONE_TX_SSB_TREBLE, civ::kIc7300Mk2));
  CHECK(!civ::toneWritable(civ::TONE_TX_AM_BASS, civ::kIc7300Mk2));  // its SSB-D transmit width
  CHECK(!civ::toneWritable(civ::TONE_RX_SSB_BASS, -1));
  CHECK(!civ::toneWritable(civ::TONE_RX_SSB_BASS, 0xA4));  // another radio at a chosen address
  CHECK(civ::toneRow(civ::CW, true) == -1 && civ::toneRow(civ::RTTY, false) == -1);
  // The radio's clock, whose rows are somewhere else on each radio.
  const civ::ClockRows& clock7300 = *civ::clockRows(civ::kIc7300);
  CHECK_HEX(civ::setClockDate(r, clock7300, 2026, 11, 1), "FE FE 94 E0 1A 05 00 94 20 26 11 01 FD");
  CHECK_HEX(civ::setClockTime(r, clock7300, 23, 59), "FE FE 94 E0 1A 05 00 95 23 59 FD");
  CHECK_HEX(civ::setClockOffset(r, clock7300, 60), "FE FE 94 E0 1A 05 00 96 01 00 00 FD");
  CHECK_HEX(civ::setClockOffset(r, clock7300, -210), "FE FE 94 E0 1A 05 00 96 03 30 01 FD");
  CHECK_HEX(civ::setClockTime(r, *civ::clockRows(civ::kIc7300Mk2), 1, 2), "FE FE 94 E0 1A 05 01 33 01 02 FD");
  CHECK_HEX(civ::setClockOffset(r, *civ::clockRows(civ::kIc7300Mk2), 0), "FE FE 94 E0 1A 05 01 36 00 00 00 FD");
  CHECK_HEX(civ::setClockDate(r, *civ::clockRows(civ::kIc9700), 2026, 1, 2), "FE FE 94 E0 1A 05 01 79 20 26 01 02 FD");
  CHECK_HEX(civ::setClockOffset(r, *civ::clockRows(civ::kIcR8600), 0), "FE FE 94 E0 1A 05 01 35 00 00 00 FD");
  CHECK(civ::clockRows(-1) == nullptr && civ::clockRows(0xA4) == nullptr);  // not said yet, and a radio it doesn't know
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
  // A 9700's numbers mean something else, and it has gaps between its bands.
  {
    const civ::Radio& r = civ::kRadio9700;
    CHECK(parseFreq("145.5", 145000000, r) == 145500000);
    CHECK(parseFreq("145500", 145000000, r) == 145500000);
    CHECK(parseFreq("145500000", 145000000, r) == 145500000);
    CHECK(parseFreq("433.5", 145000000, r) == 433500000);
    CHECK(parseFreq("1296", 145000000, r) == 1296000000);
    CHECK(parseFreq("1296.200.000", 145000000, r) == 1296200000);
    CHECK(parseFreq("1296200", 145000000, r) == 1296200000);
    CHECK(parseFreq("145.5m", 145000000, r) == 145500000);
    CHECK(parseFreq("+5", 145000000, r) == 145005000);
    CHECK(parseFreq("14.074", 145000000, r) == -1);  // HF
    CHECK(parseFreq("200", 145000000, r) == -1);     // between bands
    CHECK(parseFreq("+5000", 145000000, r) == -1);   // off the top of 2 m
    CHECK(tuning::stepFreq(147999000, 1000, 5, r) == 148000000);
    CHECK(tuning::stepFreq(144000000, 1000, -1, r) == 144000000);
    CHECK(tuning::stepFreq(1296200123, 1000, 1, r) == 1296201000);
    CHECK(tuning::formatHz(1296200000) == "1296.200.000");
    CHECK(r.clamp(148000001) == 148000000);
    CHECK(r.clamp(200000000) == 148000000);
    CHECK(r.clamp(400000000) == 430000000);
    CHECK(r.maxWattsAt(145000000) == 100 && r.maxWattsAt(433000000) == 75 && r.maxWattsAt(1296000000) == 10);
    CHECK(civ::kRadio7300.clamp(100000000) == 74800000);
  }
  CHECK(tuning::rfFromAudio(145000000, civ::DV, 1000, 600) == -1);
  CHECK(civ::widthCodeMax(civ::DV) == -1 && civ::widthCodeMax(civ::DD) == -1);
  CHECK(std::string(civ::modeName(civ::DV)) == "DV");
  CHECK(fabs(civ::powerWatts(143, civ::kRadio9700, 433000000) - 37.5) < 0.01);  // 143 is half power
  CHECK(fabs(civ::powerWatts(213, civ::kRadio9700, 1296000000) - 10) < 0.01);
  CHECK(fabs(civ::powerWatts(143, civ::kRadio7300, 14000000) - 50) < 0.01);
  CHECK(fabs(civ::compDb(210, civ::kRadio9700) - 25.5) < 0.01);
  CHECK(civ::radioForId(0x94) == &civ::kRadio7300 && civ::radioForId(0xA2) == &civ::kRadio9700 && !civ::radioForId(0x98));

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

}

static void testBandPlans() {
  namespace fs = std::filesystem;
  bands::Library lib = bands::builtinPlans();
  for (const std::string& e : lib.errors) printf("  %s\n", e.c_str());
  CHECK(lib.errors.empty());
  CHECK(lib.plans.size() == 9);
  // What a transceiver sees. A receiver's own bands are tested below.
  for (bands::Plan& p : lib.plans) p = p.forRadio(false);
  for (const bands::Plan& plan : lib.plans) {
    CHECK(!plan.name.empty() && !plan.source.empty() && !plan.revision.empty());
    for (const bands::Band& b : plan.bands) {
      CHECK(b.segmentAt(b.homeHz) != nullptr);
      for (const bands::Marker& m : b.markers) {
        if (!b.segmentAt(m.hz)) printf("  %s %s m: %s at %lld is in no segment\n", plan.id.c_str(), b.name.c_str(), m.label.c_str(), m.hz);
        CHECK(b.segmentAt(m.hz) != nullptr);
      }
      for (size_t i = 1; i < b.segments.size(); i++) CHECK(b.segments[i].lo >= b.segments[i - 1].hi);
    }
    for (size_t i = 1; i < plan.bands.size(); i++) CHECK(plan.bands[i].lo >= plan.bands[i - 1].hi);
  }

  const bands::Plan* uk = lib.find("uk");
  const bands::Plan* us = lib.find("us");
  const bands::Plan* au = lib.find("australia");
  const bands::Plan* jp = lib.find("japan");
  CHECK(uk && us && au && jp);
  if (!uk || !us || !au || !jp) return;
  CHECK(uk->bandOf(14074000) == 5);
  CHECK(uk->bandOf(15000000) == -1);
  CHECK(uk->bandOf(7250000) == -1);
  CHECK(us->bandOf(7250000) >= 0);
  CHECK(uk->bandOf(70200000) >= 0);
  CHECK(us->bandOf(70200000) == -1);
  // Blocks: inside the band's outside edges, but between its segments, is out of band.
  CHECK(uk->bandOf(5330000) >= 0 && uk->segmentAt(5330000) == nullptr);
  CHECK(uk->segmentAt(5371500) != nullptr);
  CHECK(us->segmentAt(5358500) != nullptr);  // inside the block that replaced that channel in 2026
  CHECK(us->segmentAt(5340000) == nullptr);
  CHECK(au->bandOf(5357000) == -1);
  CHECK(au->segmentAt(3750000) == nullptr);
  CHECK(jp->segmentAt(3590000) == nullptr);
  CHECK(uk->segmentAt(14074000)->use == bands::U_DIGI);
  CHECK(uk->segmentAt(14030000)->use == bands::U_CW);
  CHECK(uk->segmentAt(14200000)->use == bands::U_SSB);

  // ADIF's band doesn't depend on the plan showing.
  CHECK(std::string(bands::adifBand(3900000)) == "80M");
  CHECK(std::string(bands::adifBand(14074000)) == "20M");
  CHECK(std::string(bands::adifBand(15000000)).empty());
  CHECK(std::string(bands::adifBand(145500000)) == "2M");
  CHECK(std::string(bands::adifBand(433500000)) == "70CM");
  CHECK(std::string(bands::adifBand(1296200000)) == "23CM");

  // Every plan has the 9700's three bands, each somewhere the radio can go, and a 7300 is told nothing new.
  for (const bands::Plan& plan : lib.plans) {
    int onNine = 0;
    for (const bands::Band& b : plan.bands) {
      bool nine = civ::kRadio9700.canTune(b.homeHz), seven = civ::kRadio7300.canTune(b.homeHz);
      CHECK(nine != seven);  // each band belongs to exactly one radio
      if (!nine) continue;
      onNine++;
      CHECK(b.unit == (b.name == "2" ? "m" : "cm"));
      for (const bands::Marker& m : b.markers)
        if (m.preset()) {
          if (!civ::kRadio9700.canTune(m.tuneHz())) printf("  %s %s: preset %s is off the radio\n", plan.id.c_str(), b.name.c_str(), m.label.c_str());
          CHECK(civ::kRadio9700.canTune(m.tuneHz()));
          CHECK(m.label.size() <= 5);
        }
    }
    if (onNine != 3) printf("  %s has %d bands for the 9700\n", plan.id.c_str(), onNine);
    CHECK(onNine == 3);
  }
  CHECK(std::string(uk->bands[uk->bandOf(145500000)].title()) == "2 m");
  CHECK(std::string(uk->bands[uk->bandOf(433500000)].title()) == "70 cm");
  CHECK(uk->bands[uk->bandOf(14074000)].title() == "20 m");

  // What goes wrong in a file somebody's edited.
  auto errorOf = [](const std::string& json) {
    bands::Plan p;
    std::string e;
    return bands::parsePlan(json, "t", p, e) ? std::string() : e;
  };
  auto plan = [](const std::string& segs, const std::string& rest = "\"home_khz\": 7010, \"home_mode\": \"CW\"") {
    return "{\"name\": \"T\", \"bands\": [{\"name\": \"40\", " + rest + ", \"segments\": [" + segs + "]}]}";
  };
  const std::string good = "{\"from\": 7000, \"to\": 7040, \"use\": \"cw\"}";
  CHECK(errorOf(plan(good)).empty());
  CHECK(errorOf("\xEF\xBB\xBF" + plan(good)).empty());
  CHECK(errorOf("{\n\"name\": \"T\",\n\"bands\" []\n}").find("line 3") == 0);
  CHECK(errorOf("{\"name\": \"T\",\n}").find("line 2") == 0);
  CHECK(errorOf("{\"name\": \"T\", \"bands\": [").find("line 1") == 0);
  CHECK(!errorOf("").empty());
  CHECK(!errorOf("{\"bands\": []}").empty());
  CHECK(errorOf(plan(good + ", {\"from\": 7030, \"to\": 7050, \"use\": \"ssb\"}")).find("overlap") != std::string::npos);
  CHECK(errorOf(plan("{\"from\": 7040, \"to\": 7000, \"use\": \"cw\"}")).find("before it begins") != std::string::npos);
  CHECK(errorOf(plan("{\"from\": 7000, \"to\": 7040, \"use\": \"morse\"}")).find("morse") != std::string::npos);
  CHECK(errorOf(plan(good, "\"home_khz\": 7100, \"home_mode\": \"CW\"")).find("home_khz") != std::string::npos);
  CHECK(errorOf(plan(good, "\"home_khz\": 7010, \"home_mode\": \"SSB\"")).find("home_mode") != std::string::npos);
  CHECK(errorOf(plan(good, "\"home_khz\": 7010, \"home_mode\": \"CW-D\"")).find("home_mode") != std::string::npos);
  CHECK(errorOf(plan(good, "\"home_khz\": 7010, \"home_mode\": \"CW\", \"markers\": [{\"khz\": 8000, \"label\": \"X\"}]")).find("outside the band") != std::string::npos);
  {
    bands::Plan p;
    std::string e;
    CHECK(bands::parsePlan(plan(good, "\"home_khz\": 7010.5, \"home_mode\": \"CW\", \"markers\": [{\"khz\": 7038.6, \"label\": \"WSPR\", \"mode\": \"USB-D\"}, {\"khz\": 7030, \"label\": \"QRP\"}]"), "t", p, e));
    CHECK(p.bands[0].homeHz == 7010500);
    CHECK(p.bands[0].markers[0].hz == 7038600 && p.bands[0].markers[0].preset() && p.bands[0].markers[0].mode == civ::USB && p.bands[0].markers[0].data);
    CHECK(!p.bands[0].markers[1].preset());
    CHECK(p.bands[0].segments[0].label == "CW");
  }

  // The files in the folder.
  std::error_code ec;
  fs::path dir = fs::temp_directory_path(ec) / ("ic7300-plans-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  bands::installPlans(dir.string());
  auto write = [&](const char* name, const std::string& text) { std::ofstream(dir / name, std::ios::binary) << text; };
  auto read = [&](const char* name) {
    std::ifstream f(dir / name, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), {});
  };
  CHECK(fs::exists(dir / "uk.json") && fs::exists(dir / "README.txt"));
  CHECK(bands::loadPlans(dir.string()).errors.empty());
  CHECK(bands::loadPlans(dir.string()).plans.size() == 9);
  // An edit is used, and installing again doesn't overwrite it.
  write("uk.json", plan(good));
  bands::installPlans(dir.string());
  CHECK(read("uk.json") == plan(good));
  CHECK(bands::loadPlans(dir.string()).find("uk")->name == "T");
  // A copy from before the VHF plans still gets them, without touching the file, and stays in order.
  {
    bands::Library l = bands::loadPlans(dir.string());
    const bands::Plan* edited = l.find("uk");
    CHECK(edited->bandOf(7010000) >= 0 && edited->bandOf(145500000) >= 0 && edited->bandOf(1296200000) >= 0);
    for (size_t i = 1; i < edited->bands.size(); i++) CHECK(edited->bands[i].lo >= edited->bands[i - 1].hi);
    CHECK(read("uk.json") == plan(good));
  }
  // But one with a VHF band of its own has decided for itself.
  write("uk.json", "{\"name\": \"T\", \"bands\": [{\"name\": \"2\", \"home_khz\": 145000, \"home_mode\": \"FM\", \"segments\": [{\"from\": 144000, \"to\": 146000, \"use\": \"fm\"}]}]}");
  CHECK(bands::loadPlans(dir.string()).find("uk")->bandOf(433500000) == -1);
  write("uk.json", plan(good));
  // A unit's checked, and defaults to metres.
  CHECK(errorOf(plan(good, "\"home_khz\": 7010, \"home_mode\": \"CW\", \"unit\": \"ft\"")).find("unit") != std::string::npos);
  // A new file is a new plan.
  write("ireland.json", plan(good));
  CHECK(bands::loadPlans(dir.string()).plans.size() == 10);
  CHECK(bands::loadPlans(dir.string()).find("ireland") != nullptr);
  // A broken file is reported, naming it, and the built-in plan stands in.
  write("uk.json", "{ broken");
  {
    bands::Library l = bands::loadPlans(dir.string());
    CHECK(l.errors.size() == 1 && l.errors[0].find("uk.json") == 0);
    CHECK(l.find("uk") && l.find("uk")->name == "United Kingdom");
  }
  // Reset puts it back and keeps what it replaced.
  CHECK(bands::resetPlan(dir.string(), "uk"));
  CHECK(read("uk.json.bak") == "{ broken");
  CHECK(bands::loadPlans(dir.string()).errors.empty());
  CHECK(!bands::resetPlan(dir.string(), "ireland"));
  fs::remove_all(dir, ec);
}

static void testStations() {
  namespace fs = std::filesystem;
  // What a transceiver sees. A receiver's own stations are tested below.
  bands::Stations lib = bands::builtinStations().forRadio(false);
  for (const std::string& e : lib.errors) printf("  %s\n", e.c_str());
  CHECK(lib.errors.empty());
  CHECK(lib.find("receiver") == nullptr);
  const bands::StationFile* w = lib.find("worldwide");
  CHECK(w != nullptr);
  if (!w) return;
  CHECK(!w->source.empty() && !w->revision.empty() && !w->note.empty());
  CHECK(w->stations.size() > 300);
  bool service[bands::S_COUNT] = {};
  std::map<long long, const bands::Station*> at;
  for (const bands::Station& s : w->stations) {
    service[s.service] = true;
    CHECK(s.preset());
    CHECK(civ::kRadio7300.canTune(s.tuneHz()) || civ::kRadio9700.canTune(s.tuneHz()));
    CHECK(!s.label.empty() && !s.group.empty() && !s.note.empty());
    at[s.hz] = &s;
    // Nothing's listed twice in one group.
    int same = 0;
    for (const bands::Station& o : w->stations) same += o.hz == s.hz && o.group == s.group;
    if (same != 1) printf("  %s: %lld appears %d times\n", s.group.c_str(), s.hz, same);
    CHECK(same == 1);
  }
  // Aviation and space are the services only a receiver's list uses.
  for (int i = 0; i < bands::S_COUNT; i++) CHECK(service[i] == (i != bands::S_AVIATION && i != bands::S_SPACE));
  // A fax flag sits on the signal, and tunes USB below it.
  CHECK(at.count(8040000) && at[8040000]->tuneHz() == 8038100 && at[8040000]->mode == civ::USB);
  CHECK(at.count(518000) && at[518000]->tuneHz() == 516300);
  CHECK(at.count(2182000) && at[2182000]->tuneHz() == 2182000);
  CHECK(at.count(4625000) && at[4625000]->service == bands::S_MYSTERY);
  // The channels a transceiver can hear are on its list, under their numbers.
  CHECK(at.count(27185000) && at[27185000]->label == "CB 19");
  CHECK(at.count(446006250) && at[446006250]->label == "PMR 1");
  CHECK(at.count(4426000) && at[4426000]->label == "Ch 424");
  CHECK(at.count(11175000));
  // Stations that have closed down.
  CHECK(!at.count(3330000) && !at.count(7850000) && !at.count(14670000));  // CHU, June 2026
  CHECK(!at.count(198000));  // BBC Radio 4 long wave, June 2026
  // The 9700's bands have stations too, each on a band of its own.
  {
    int on[3] = {};
    for (const bands::Station& s : w->stations) {
      on[0] += s.hz >= 144000000 && s.hz <= 148000000;
      on[1] += s.hz >= 430000000 && s.hz <= 450000000;
      on[2] += s.hz >= 1240000000 && s.hz <= 1300000000;
    }
    CHECK(on[0] > 0 && on[1] > 0 && on[2] > 0);
    CHECK(at.count(145800000) && at[145800000]->service == bands::S_AMATEUR && at[145800000]->mode == civ::FM);
    CHECK(at.count(436795000) && !civ::kRadio7300.canTune(436795000) && civ::kRadio9700.canTune(436795000));
    CHECK(lib.search("so-50").size() == 1);
    CHECK(lib.rangeAt(446100000) && lib.rangeAt(446100000)->service == bands::S_COMMERCIAL);
    CHECK(lib.rangeAt(145800000) == nullptr);
  }
  CHECK(lib.rangeAt(9500000) && lib.rangeAt(9500000)->service == bands::S_BROADCAST);
  CHECK(lib.rangeAt(14074000) == nullptr);
  bool none[bands::S_COUNT] = {};
  CHECK(lib.rangeAt(9500000, none) == nullptr);

  // Search: any case, every word, and what it names first.
  auto count = [&](const char* q) { return lib.search(q).size(); };
  {
    size_t everything = 0;
    for (const bands::StationFile& f : lib.files) everything += f.stations.size() + f.ranges.size();
    CHECK(count("") == everything);
  }
  CHECK(count("NORTHWOOD") == 4);
  CHECK(count("northwood 8040") == 1);
  CHECK(count("navtex") >= 3);
  CHECK(count("zzzz") == 0);
  CHECK(count("wefax") > 40);
  CHECK(lib.search("  8040  ")[0].station && lib.search("8040")[0].station->hz == 8040000);
  {
    auto hits = lib.search("buzzer");
    // Its group is "Buzzers and markers", so the rest of the group is found too, after the one the search names.
    CHECK(hits.size() == 6 && hits[0].station->hz == 4625000 && hits[0].named && !hits[1].named);
    hits = lib.search("31 m");
    CHECK(!hits.empty() && hits[0].range && hits[0].range->lo == 9400000);
    // GYA's label names it, and comes first although lower ones only mention the word.
    hits = lib.search("gya");
    CHECK(hits.size() >= 4 && hits[0].named && hits[0].station->hz == 2618500 && hits[3].named);
  }

  // The broadcasters. Their schedule changes each season, so these are the names rather than the frequencies.
  const bands::StationFile* broadcast = lib.find("broadcast");
  CHECK(broadcast != nullptr);
  if (!broadcast) return;
  CHECK(!broadcast->source.empty() && !broadcast->revision.empty() && !broadcast->note.empty());
  CHECK(broadcast->stations.size() > 1000);
  {
    std::map<std::string, int> seen;
    for (const bands::Station& s : broadcast->stations) {
      CHECK(s.service == bands::S_BROADCAST && s.mode == civ::AM && !s.data);
      CHECK(civ::kRadio7300.canTune(s.hz) && !s.note.empty() && !s.label.empty() && !s.group.empty());
      CHECK(++seen[s.group + "/" + s.label + "/" + std::to_string(s.hz)] == 1);
      // What EiBi lists that isn't a broadcaster.
      for (const char* word : {"Volmet", "Jammer", "Spy", "Coastguard", "DIGITAL", "Fax"}) CHECK(s.label.find(word) == std::string::npos);
    }
  }
  for (const char* name : {"voice of america", "voa", "world service", "voice of china", "china radio international", "romania", "vatican", "voice of korea"}) {
    if (count(name) < 5) printf("  %s: %d\n", name, (int)count(name));
    CHECK(count(name) >= 5);
  }
  // The exact name comes ahead of those that only have its words somewhere, like a service to North America.
  CHECK(lib.search("voice of america")[0].station->label.find("VOA") == 0);
  CHECK(lib.search("world service")[0].station->label == "BBC");
  CHECK(lib.search("voice of china")[0].phrase);
  CHECK(count("kfi 640") == 1);  // a US clear channel station, with no schedule, so always on
  for (const bands::StationHit& h : lib.search("kfi 640")) CHECK(h.station && h.station->airTimes.empty());

  // When they're on, in UTC.
  {
    bands::StationFile f;
    std::string e;
    CHECK(bands::parseStations("{\"name\": \"T\", \"groups\": [{\"name\": \"G\", \"service\": \"broadcast\", \"mode\": \"AM\", \"stations\": ["
                               "{\"khz\": 6000, \"label\": \"A\", \"on\": [\"0100-0230\", \"2300-0100 135\"]},"
                               "{\"khz\": 6001, \"label\": \"B\"},"
                               "{\"khz\": 6002, \"label\": \"C\", \"on\": [\"0000-2400 7\"]}]}]}", "t", f, e));
    const bands::Station& a = f.stations[0];
    CHECK(a.onAir(1, 60) && a.onAir(1, 149) && !a.onAir(1, 150));
    CHECK(a.onAir(0, 23 * 60 + 30) && !a.onAir(1, 23 * 60 + 30));   // it starts on Monday, and not on Tuesday
    CHECK(a.onAir(1, 30) && !a.onAir(2, 30));                       // so it's still on early on Tuesday, and not Wednesday
    CHECK(!a.onAir(0, 30) && !a.onAir(4, 30) && a.onAir(5, 30));    // nothing starts on Sunday or Thursday; Friday's runs into Saturday
    CHECK(f.stations[1].onAir(3, 700));
    CHECK(f.stations[2].onAir(6, 0) && f.stations[2].onAir(6, 1439) && !f.stations[2].onAir(0, 0));
    for (const char* bad : {"\"2500-0100\"", "\"0100-0100\"", "\"0160-0200\"", "\"0100-0200 8\"", "\"0100-0200 \"", "\"0100\"", "5"})
      CHECK(!bands::parseStations(std::string("{\"name\": \"T\", \"groups\": [{\"name\": \"G\", \"service\": \"broadcast\", \"mode\": \"AM\", \"stations\": [{\"khz\": 6000, \"label\": \"A\", \"on\": [") + bad + "]}]}]}", "t", f, e));
  }

  // What goes wrong in a file somebody's edited.
  auto errorOf = [](const std::string& json) {
    bands::StationFile f;
    std::string e;
    return bands::parseStations(json, "t", f, e) ? std::string() : e;
  };
  auto file = [](const std::string& rest, const std::string& stations) {
    return "{\"name\": \"T\", \"groups\": [{\"name\": \"G\", " + rest + ", \"stations\": [" + stations + "]}]}";
  };
  const std::string naval = "\"service\": \"naval\", \"mode\": \"USB\"";
  const std::string good = "{\"khz\": 8040, \"label\": \"GYA\"}";
  CHECK(errorOf(file(naval, good)).empty());
  CHECK(errorOf(file("\"service\": \"pirate\", \"mode\": \"USB\"", good)).find("pirate") != std::string::npos);
  CHECK(errorOf(file("\"service\": \"naval\", \"mode\": \"SSB\"", good)).find("SSB") != std::string::npos);
  CHECK(errorOf(file(naval, "{\"khz\": 8040}")).find("label") != std::string::npos);
  CHECK(errorOf(file(naval, "{\"label\": \"X\"}")).find("khz") != std::string::npos);
  CHECK(errorOf(file(naval, "{\"khz\": 5, \"label\": \"RJH\"}")).find("receive") != std::string::npos);  // below even an R8600
  CHECK(errorOf("{\"name\": \"T\"}").find("no \"groups\"") != std::string::npos);
  CHECK(errorOf("{\"name\": \"T\",\n\"groups\" []}").find("line 2") == 0);
  CHECK(errorOf("{\"name\": \"T\", \"ranges\": [{\"from\": 9, \"to\": 8, \"service\": \"broadcast\", \"label\": \"X\"}]}").find("before it begins") != std::string::npos);
  {
    bands::StationFile f;
    std::string e;
    CHECK(bands::parseStations(file(naval + ", \"dial_offset_khz\": -1.9, \"note\": \"N\"", good + ", {\"khz\": 5000, \"label\": \"X\", \"mode\": \"AM\", \"dial_khz\": 4999}"), "t", f, e));
    CHECK(f.stations[0].tuneHz() == 8038100 && f.stations[0].note == "N" && f.stations[0].mode == civ::USB);
    CHECK(f.stations[1].tuneHz() == 4999000 && f.stations[1].mode == civ::AM);
  }

  // The files in the folder.
  std::error_code ec;
  fs::path dir = fs::temp_directory_path(ec) / ("ic7300-stations-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  bands::installStations(dir.string());
  auto write = [&](const char* name, const std::string& text) { std::ofstream(dir / name, std::ios::binary) << text; };
  auto read = [&](const char* name) {
    std::ifstream f(dir / name, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), {});
  };
  CHECK(fs::exists(dir / "worldwide.json") && fs::exists(dir / "README.txt"));
  CHECK(bands::loadStations(dir.string()).errors.empty());
  write("worldwide.json", file(naval, good));
  bands::installStations(dir.string());
  CHECK(read("worldwide.json") == file(naval, good));
  // A copy from before the VHF stations still gets them, without touching the file. Groups stay together.
  {
    bands::Stations l = bands::loadStations(dir.string());
    const bands::StationFile* edited = l.find("worldwide");
    int below = 0, above = 0;
    for (const bands::Station& s : edited->stations) (s.hz < 100000000 ? below : above)++;
    CHECK(below == 1 && above > 0);
    CHECK(edited->stations[0].hz < 100000000);
    CHECK(l.rangeAt(446100000) != nullptr);
    CHECK(read("worldwide.json") == file(naval, good));
  }
  // But one with a VHF station of its own has decided for itself.
  write("worldwide.json", file(naval, "{\"khz\": 145000, \"label\": \"V\"}"));
  CHECK(bands::loadStations(dir.string()).find("worldwide")->stations.size() == 1);
  write("worldwide.json", file(naval, good));
  // Your own list goes alongside, and search covers it.
  write("mine.json", file(naval, "{\"khz\": 9000, \"label\": \"Mine\"}"));
  {
    bands::Stations l = bands::loadStations(dir.string());
    auto hits = l.search("mine");
    CHECK(l.files.size() == 4 && !hits.empty() && hits[0].named && hits[0].station->label == "Mine");
  }
  write("worldwide.json", "{ broken");
  {
    bands::Stations l = bands::loadStations(dir.string());
    CHECK(l.errors.size() == 1 && l.errors[0].find("worldwide.json") == 0);
    CHECK(l.find("worldwide") && l.find("worldwide")->stations.size() > 300);
  }
  CHECK(bands::resetStations(dir.string(), "worldwide"));
  CHECK(read("worldwide.json.bak") == "{ broken");
  CHECK(!bands::resetStations(dir.string(), "mine"));
  fs::remove_all(dir, ec);
}

static void testDial() {
  auto at = [](double deg) { return std::pair<double, double>(100 * cos(deg * M_PI / 180), 100 * sin(deg * M_PI / 180)); };
  // Walks the pointer round from one angle to another, adding up the clicks. The default pace is slow.
  auto walk = [&](tuning::Dial& d, double from, double to, int n = 40, double dt = 0.05) {
    int clicks = 0;
    for (int i = 0; i <= n; i++) {
      auto p = at(from + (to - from) * i / n);
      clicks += d.spin(p.first, p.second, 10, dt);
    }
    return clicks;
  };
  const double frame = 1.0 / 60;
  {
    tuning::Dial d;  // grabbing it does nothing, wherever it's grabbed
    auto p = at(123);
    CHECK(d.spin(p.first, p.second, 10, frame) == 0);
    CHECK(d.spin(p.first, p.second, 10, frame) == 0);
    CHECK(d.angle() == 0);
  }
  {
    tuning::Dial d;  // clockwise is up, at 100 clicks a turn. 365 degrees is 101.4 clicks
    CHECK(walk(d, 0, 365) == 101);
    d.idle(frame);
    CHECK(walk(d, 365, 0) == -101);
  }
  {
    tuning::Dial d;  // both ways across the wrap at 180 degrees: 60 degrees is 16.7 clicks
    CHECK(walk(d, 150, 210) == 16);
    d.idle(frame);
    CHECK(walk(d, 210, 150) == -16);
  }
  {
    tuning::Dial d;  // under one click only builds up, and nothing's lost across calls
    CHECK(walk(d, 0, 2) == 0);
    CHECK(walk(d, 2, 4) == 1);
  }
  {
    tuning::Dial d;  // through the middle: no jump, however far round it comes back out
    auto p = at(0);
    d.spin(p.first, p.second, 10, frame);
    CHECK(d.spin(1, 1, 10, frame) == 0);
    p = at(180);
    CHECK(d.spin(p.first, p.second, 10, frame) == 0);
    CHECK(d.angle() == 0);
  }
  {
    tuning::Dial d;  // letting go forgets a part-click, and a slow turn stops dead
    CHECK(walk(d, 0, 3) == 0);
    double was = d.angle();
    int coasted = 0;
    for (int i = 0; i < 120; i++) coasted += d.idle(frame);
    CHECK(coasted == 0);
    CHECK(d.angle() == was);
    CHECK(walk(d, 0, 3) == 0);
  }
  {
    tuning::Dial d;  // spun fast, a turn is worth more than a hundred clicks
    int fast = 0;
    for (int turn = 0; turn < 4; turn++) fast += walk(d, 0, 359, 60, 0.002);
    CHECK(fast > 4 * 100 * 3);
    CHECK(fast <= 4 * 100 * 8);
  }
  {
    tuning::Dial d;  // let go at speed, it runs on the same way, slows, and stops by itself
    CHECK(walk(d, 0, 359, 60, 0.002) > 0);
    int coasted = 0, steps = 0;
    double was = d.angle();
    while (steps < 2000 && (d.idle(frame), d.angle() != was)) {
      was = d.angle();
      steps++;
    }
    tuning::Dial e;
    walk(e, 0, 359, 60, 0.002);
    for (int i = 0; i < 2000; i++) coasted += e.idle(frame);
    CHECK(coasted > 0);
    CHECK(steps < 2000);
    CHECK(e.idle(frame) == 0);
    tuning::Dial back;
    walk(back, 359, 0, 60, 0.002);
    CHECK(back.idle(frame) < 0);
  }
  {
    tuning::Dial d;  // catching it stops the coast at once, and so does stop()
    walk(d, 0, 359, 60, 0.002);
    d.idle(frame);
    auto p = at(10);
    d.spin(p.first, p.second, 10, frame);
    d.spin(p.first, p.second, 10, frame);
    double was = d.angle();
    d.idle(frame);
    d.idle(frame);
    CHECK(d.angle() == was);
    walk(d, 0, 359, 60, 0.002);
    d.idle(frame);
    d.stop();
    was = d.angle();
    d.idle(frame);
    CHECK(d.angle() == was);
  }
  CHECK(tuning::Dial::clickHz(100) == 10);
  CHECK(tuning::Dial::clickHz(1000) == 100);
  CHECK(tuning::Dial::clickHz(1) == 1);
  CHECK(tuning::stepFreq(14074123, 100, 3) == 14074400);   // several clicks in one frame
  CHECK(tuning::stepFreq(14074123, 100, -3) == 14073900);
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

static void testNeedle() {
  // Both meters agree on where things are: S9 is at 120 of 241, and nothing leaves the scale.
  CHECK(fabs(needle::signalShare(120) - 120 / 241.f) < 1e-6);
  CHECK(needle::signalShare(-1) == 0);
  CHECK(needle::signalShare(500) == 1);
  CHECK(fabs(needle::powerShare(50, 100) - needle::kPoFull / 2) < 1e-6);
  CHECK(fabs(needle::powerShare(10, 10) - needle::powerShare(100, 100)) < 1e-6);
  CHECK(needle::powerShare(1000, 100) == 1);
  CHECK(needle::powerShare(5, 0) == 0);

  // It gets to a reading quickly, swings past it only a little, and settles there.
  needle::Needle n;
  float peak = 0, at200ms = 0;
  for (int i = 0; i < 120; i++) {
    n.update(0.5f, 1 / 60.f);
    peak = std::max(peak, n.pos);
    if (i == 11) at200ms = n.pos;
  }
  CHECK(at200ms > 0.4f);
  CHECK(peak > 0.5f && peak < 0.55f);
  CHECK(fabs(n.pos - 0.5f) < 0.001f);

  // It falls back more slowly than it rises.
  needle::Needle up, down;
  down.pos = 1;
  for (int i = 0; i < 6; i++) up.update(1, 1 / 60.f), down.update(0, 1 / 60.f);
  CHECK(up.pos > 1 - down.pos);

  // The stops hold it, and the same reading settles the same at any frame rate, even through a stall.
  needle::Needle pegged;
  pegged.pos = 1, pegged.vel = 50;
  pegged.update(1, 0.1f);
  CHECK(pegged.pos <= needle::kStop);
  needle::Needle rest;
  rest.vel = -50;
  rest.update(0, 0.1f);
  CHECK(rest.pos >= 0);
  needle::Needle slow, fast;
  for (int i = 0; i < 30; i++) slow.update(0.7f, 0.1f);
  for (int i = 0; i < 720; i++) fast.update(0.7f, 1 / 240.f);
  CHECK(fabs(slow.pos - fast.pos) < 0.001f);
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
  {
    store::LogEntry dv = e;
    dv.mode = "DV", dv.hz = 145375000;
    std::string r = store::adifRecord(dv, "");
    CHECK(r.find("<MODE:12>DIGITALVOICE") != std::string::npos && r.find("<SUBMODE:5>DSTAR") != std::string::npos && r.find("<BAND:2>2M") != std::string::npos);
  }

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

// Finding out which radio is on the cable, whatever address was chosen.
static void testDetection() {
  auto startOn = [](SimRig& sim, Rig& rig, uint8_t addr, const civ::Radio* forced = nullptr) {
    rig.start([&sim] { return sim.newTransport(); }, addr, forced);
  };
  auto up = [](Rig& rig) { return waitFor([&] { return rig.state().connected && rig.state().hz > 0; }); };
  {  // The ordinary case for anyone who's used this program: 94h saved, a 9700 plugged in.
    SimRig sim(civ::kRadio9700);
    Rig rig;
    startOn(sim, rig, civ::kIc7300);
    CHECK(!rig.state().connected);
    CHECK(up(rig));
    CHECK(rig.state().radio == &civ::kRadio9700);
    CHECK(waitFor([&] { return rig.state().rigId == civ::kIc9700; }));
    CHECK(rig.state().hz == 145500000);
    CHECK(waitFor([&] { return !sim.received(0x07).empty(); }));
    CHECK(sim.received(0x07)[0] == civ::Bytes{0xD0});  // takes the main band
    // Nothing it doesn't have is asked for.
    sleepMs(1500);
    for (const civ::Bytes& d : sim.received(0x1C)) CHECK(d.empty() || d[0] != 0x01);
    for (const civ::Bytes& d : sim.received(0x21)) CHECK(d.empty() || d[0] != 0x02);
    CHECK(rig.state().lastError.empty());
  }
  {  // A 7300 is found first time, at its own address, and asked nothing of a 9700's.
    SimRig sim;
    Rig rig;
    startOn(sim, rig, civ::kIc7300);
    CHECK(up(rig));
    CHECK(rig.state().radio == &civ::kRadio7300);
    int first = 0;
    for (const civ::Bytes& d : sim.received(0x19)) first += d == civ::Bytes{0x00};
    CHECK(first >= 1);
  }
  {  // Its address changed in the menu, and 19 00 still reporting the model.
    SimRig sim(civ::kRadio9700);
    sim.setCivAddress(0x70, false);
    Rig rig;
    startOn(sim, rig, civ::kIc7300);
    CHECK(up(rig));  // found through the broadcast address
    CHECK(rig.state().radio == &civ::kRadio9700);
  }
  {  // Changed, and 19 00 reporting the new address: it can't tell which, so it needs telling.
    SimRig sim(civ::kRadio9700);
    sim.setCivAddress(0x70, true);
    Rig rig;
    startOn(sim, rig, civ::kIc7300);
    CHECK(waitFor([&] { return rig.state().strangerAddr == 0x70 && rig.state().strangerId == 0x70; }));
    CHECK(!rig.state().connected);  // nothing's sent to a radio we don't know
    CHECK(sim.received(0x05).empty() && sim.received(0x14).empty());
    Rig told;
    startOn(sim, told, 0x70, &civ::kRadio9700);
    CHECK(up(told));
    CHECK(told.state().radio == &civ::kRadio9700);
  }
  {  // Somebody chose this address long ago, for a radio we don't list. It goes on working as a 7300.
    SimRig sim;
    sim.setCivAddress(0x70, true);
    Rig rig;
    startOn(sim, rig, 0x70);
    CHECK(up(rig));
    CHECK(rig.state().radio == &civ::kRadio7300);
  }
  {  // A different radio on the same cable after a gap: found afresh, disarmed, with nothing kept.
    SimRig seven, nine(civ::kRadio9700);
    SimRig* on = &seven;
    Rig rig;
    rig.start([&on] { return on->newTransport(); });
    CHECK(up(rig));
    rig.armTx(true);
    CHECK(rig.state().txArmed);
    seven.setPortFails(true);
    CHECK(waitFor([&] { return !rig.state().connected; }));
    on = &nine;
    CHECK(waitFor([&] { return rig.state().connected && rig.state().radio == &civ::kRadio9700; }, 6000));
    CHECK(!rig.state().txArmed);
    CHECK(waitFor([&] { return rig.state().hz == 145500000; }));
  }
  {  // The 9700's differences, in the bytes that reach it.
    SimRig sim(civ::kRadio9700);
    Rig rig;
    startOn(sim, rig, civ::kIc7300);
    CHECK(up(rig));
    CHECK(waitFor([&] { return rig.state().otherHz == 145600000 && rig.state().otherMode == civ::FM && rig.state().mode == civ::FM; }));
    rig.setAtt(true);
    CHECK(waitFor([&] { return sim.att() == 0x10; }));
    rig.setAtt(false);
    CHECK(waitFor([&] { return sim.att() == 0; }));
    rig.setFreq(433500000);
    CHECK(waitFor([&] { return sim.hz() == 433500000; }));
    rig.setFreq(1296200000);
    CHECK(waitFor([&] { return sim.hz() == 1296200000; }));
    rig.setFreq(200000000);                    // no such frequency: the nearest edge, not a refusal
    CHECK(rig.state().hz == 148000000);
    CHECK(waitFor([&] { return sim.hz() == 148000000; }));
    rig.setFreq(145500000);
    CHECK(waitFor([&] { return sim.hz() == 145500000; }));
    // A/B is two writes, as 07 B0 would swap its bands round.
    sleepMs(500);
    rig.setOtherFreq(145600000);
    CHECK(waitFor([&] { return sim.otherHz() == 145600000; }));
    sleepMs(500);
    rig.vfoSwap();
    CHECK(rig.state().hz == 145600000 && rig.state().otherHz == 145500000);
    CHECK(waitFor([&] { return sim.hz() == 145600000 && sim.otherHz() == 145500000; }));
    CHECK(!sim.mainSubExchanged());
    // A tuner it hasn't got: not even armed does anything go out.
    rig.armTx(true);
    CHECK(!rig.tuneAtu());
    rig.setAtu(true);
    rig.setXitOn(true);
    sleepMs(100);
    for (const civ::Bytes& d : sim.received(0x1C)) CHECK(d.empty() || d[0] != 0x01);
    for (const civ::Bytes& d : sim.received(0x21)) CHECK(d.empty() || d[0] != 0x02);
    rig.armTx(false);
    // DV has no filter byte.
    rig.setMode(civ::DV, false, 2);
    CHECK(waitFor([&] { return sim.mode() == civ::DV; }));
    CHECK(sim.received(0x06).back() == civ::Bytes{civ::DV});
    // The other VFO's mode with data off: no filter, as the guide wants.
    rig.setOtherMode(civ::USB, false, 2);
    CHECK(waitFor([&] { return sim.otherMode() == civ::USB; }));
    CHECK(sim.received(0x26).back() == civ::Bytes({0x01, civ::USB, 0x00, 0x00}));
    // The scope works with the 1.2 GHz edges, and the sub scope is left out.
    rig.setFreq(1296200000);
    rig.setScope(true);
    ScopeLine line;
    CHECK(waitFor([&] { return rig.popScope(line) && line.loHz < 1296200000 && line.hiHz > 1296200000; }));
    CHECK(line.levels.size() == civ::kScopePoints);
    rig.setScope(false);
  }
  {  // An R8600 is found at its own address, whatever was chosen, and nothing's armed.
    SimRig sim(civ::kRadioR8600);
    Rig rig;
    startOn(sim, rig, civ::kIc7300);
    CHECK(up(rig));
    CHECK(rig.state().radio == &civ::kRadioR8600);
    CHECK(waitFor([&] { return rig.state().rigId == civ::kIcR8600 && rig.state().hz == 9700000; }));

    // Antennas: all three below 30 MHz, in the bytes the guide gives.
    CHECK(waitFor([&] { return rig.state().ant == 0 && rig.state().ipPlus == 0; }));
    rig.setAntenna(1);
    CHECK(rig.state().ant == 1);
    CHECK(waitFor([&] { return sim.antenna() == 1; }));
    CHECK(sim.received(0x12).back() == civ::Bytes{0x01});
    rig.setAntenna(2);
    CHECK(waitFor([&] { return sim.antenna() == 2; }));
    rig.setAntenna(3);                         // no such port
    sleepMs(100);
    CHECK(sim.antenna() == 2);

    // Only ANT1 at and above 30 MHz, and nothing else is sent: the radio would refuse it.
    rig.setFreq(29999990);
    CHECK(waitFor([&] { return sim.hz() == 29999990; }));
    sleepMs(500);
    CHECK(rig.state().ant == 2);
    rig.setFreq(30000000);
    CHECK(waitFor([&] { return sim.hz() == 30000000; }));
    sleepMs(500);
    size_t writes = 0;
    for (const civ::Bytes& d : sim.received(0x12)) writes += !d.empty();
    rig.setAntenna(1);
    sleepMs(100);
    size_t after = 0;
    for (const civ::Bytes& d : sim.received(0x12)) after += !d.empty();
    CHECK(after == writes);
    CHECK(waitFor([&] { return rig.state().ant == 0; }));

    // The attenuator's three steps go as BCD: the code is the dB.
    for (int db : {10, 20, 30}) {
      rig.setAttDb(db);
      CHECK(rig.state().attDb == db && rig.state().att == 1);
      CHECK(waitFor([&] { return sim.att() == civ::bcdBE(db, 1)[0]; }));
    }
    rig.setAttDb(0);
    CHECK(waitFor([&] { return sim.att() == 0; }));
    sleepMs(500);
    CHECK(rig.state().attDb == 0 && rig.state().att == 0);

    rig.setIpPlus(true);
    CHECK(waitFor([&] { return sim.ipPlus(); }));
    CHECK(sim.received(0x1A).back() == civ::Bytes({0x07, 0x01}));

    // WFM and the digital modes have no filter byte, S-AM does, and none of them touch data mode.
    rig.setFreq(100000000);
    rig.setMode(civ::WFM, true, 2);
    CHECK(waitFor([&] { return sim.mode() == civ::WFM; }));
    CHECK(sim.received(0x06).back() == civ::Bytes{civ::WFM});
    rig.setMode(civ::S_AM, false, 2);
    CHECK(waitFor([&] { return sim.mode() == civ::S_AM && sim.filter() == 2; }));
    CHECK(sim.received(0x06).back() == civ::Bytes({civ::S_AM, 0x02}));
    rig.setMode(civ::P25, false, 1);
    CHECK(waitFor([&] { return sim.mode() == civ::P25; }));
    CHECK(sim.received(0x06).back() == civ::Bytes{civ::P25});
    CHECK(civ::widthCodeMax(civ::S_AM) == 49 && civ::widthCodeMax(civ::WFM) == -1 && civ::widthCodeMax(civ::P25) == -1);
    CHECK(civ::widthHz(civ::S_AM, 49) == 10000 && civ::widthHz(civ::WFM, 0) == 200000);
    // A preset for a mode it hasn't got: the plainest thing that hears it.
    rig.setMode(civ::DD, false, 1);
    CHECK(waitFor([&] { return sim.mode() == civ::FM; }));

    // Nothing that transmits or needs a second VFO goes out, armed or not.
    rig.armTx(true);
    CHECK(!rig.state().txArmed);
    CHECK(!rig.ptt(true) && !rig.tuneAtu() && !rig.sendCw("CQ") && !rig.playVoice(1) && !rig.playVoice(0));
    rig.stopCw();
    rig.setSplit(true);
    rig.setOtherFreq(14000000);
    rig.setOtherMode(civ::USB, false, 1);
    rig.vfoEqual();
    rig.vfoSwap();
    rig.setRit(100);
    rig.setRitOn(true);
    rig.setXitOn(true);
    rig.setLevel(civ::L_POWER, 10);
    rig.setSwitch(civ::S_VOX, 1);
    sleepMs(800);
    CHECK(!sim.tx() && rig.state().hz == 100000000);
    CHECK(sim.received(0x1C).empty() && sim.received(0x17).empty() && sim.received(0x28).empty());
    CHECK(sim.received(0x0F).empty() && sim.received(0x21).empty() && sim.received(0x25).empty() && sim.received(0x26).empty() && sim.received(0x07).empty());
    for (const civ::Bytes& d : sim.received(0x14)) CHECK(d.empty() || d[0] != civ::levelSub(civ::L_POWER));
    for (const civ::Bytes& d : sim.received(0x1A)) CHECK(d.empty() || d[0] != 0x06);
    // And polling asked it nothing it hasn't got, going by its guide.
    CHECK(sim.refusals() == 0);
    CHECK(waitFor([&] { return rig.state().meter[civ::M_S] >= 0 && rig.state().squelchOpen == 1 && rig.state().overflow == 0; }));

    // The scope, up to its widest, and at the top of its range.
    rig.setFreq(3000000000LL);
    CHECK(waitFor([&] { return sim.hz() == 3000000000LL; }));
    rig.setFreq(5000000);
    rig.setFreq(10000);
    CHECK(waitFor([&] { return sim.hz() == 10000; }));
    rig.setFreq(5);                            // below it: the edge, as everywhere else
    CHECK(rig.state().hz == 10000);
    rig.setFreq(100000000);
    rig.setScopeSpan(5000000);
    rig.setScope(true);
    ScopeLine line;
    CHECK(waitFor([&] { return rig.popScope(line) && line.hiHz - line.loHz == 5000000; }));
    CHECK(line.levels.size() == civ::kScopePoints);
    rig.setScope(false);
  }
  {  // A transceiver is asked exactly what it always was.
    SimRig sim;
    Rig rig;
    startOn(sim, rig, civ::kIc7300);
    CHECK(up(rig));
    sleepMs(1800);
    CHECK(sim.received(0x12).empty());
    bool asked = false;
    for (const civ::Bytes& d : sim.received(0x1A)) asked = asked || (!d.empty() && d[0] == 0x07);
    CHECK(!asked);
    rig.setAntenna(0);
    rig.setAntenna(1);
    rig.setIpPlus(true);
    sleepMs(100);
    CHECK(sim.received(0x12).empty());
    // And it keeps to its own modes, whatever a memory wants.
    rig.setMode(civ::WFM, false, 1);
    CHECK(waitFor([&] { return sim.mode() == civ::FM; }));
    rig.setMode(civ::S_AM, false, 1);
    CHECK(waitFor([&] { return sim.mode() == civ::AM; }));
  }
}

// What a receiver has that a transceiver doesn't: all of its coverage on some band, in every plan, and a long
// list of stations. Each file's checks are that it's internally sound, not that its facts are right.
static void testReceiverData() {
  const civ::Radio& r = civ::kRadioR8600;
  CHECK(civ::radioForId(0x96) == &r);
  CHECK(!r.hasTransmit && !r.hasOtherVfo && !r.hasDataMode && !r.hasRit && r.antennas == 3);
  CHECK(r.canTune(10000) && r.canTune(3000000000LL) && !r.canTune(9999) && !r.canTune(3000000001LL));
  CHECK(tuning::parseFreq("100", 100000000, r) == 100000000);
  CHECK(tuning::parseFreq("198k", 100000000, r) == 198000);
  CHECK(tuning::parseFreq("121.5", 100000000, r) == 121500000);
  CHECK(tuning::parseFreq("145500", 100000000, r) == 145500000);
  CHECK(tuning::parseFreq("3000", 100000000, r) == 3000000000LL);
  CHECK(tuning::parseFreq("3001", 100000000, r) == 3001000);  // past the top in MHz, so it's kHz
  CHECK(tuning::parseFreq("3001m", 100000000, r) == -1);
  CHECK(tuning::rfFromAudio(100000000, civ::WFM, 1000, 600) == -1);
  CHECK(tuning::rfFromAudio(9000000, civ::S_AM, 1000, 600) == -1);
  CHECK(tuning::rfFromAudio(450000000, civ::P25, 1000, 600) == -1);
  CHECK(std::string(civ::modeName(civ::WFM)) == "WFM" && std::string(civ::modeName(civ::NXDN_N)) == "NXDN-N");
  CHECK(civ::modeOn(civ::kRadio9700, civ::DD) == civ::DD && civ::modeOn(civ::kRadio9700, civ::DV) == civ::DV);
  CHECK(civ::modeOn(r, civ::DV) == civ::DV && civ::modeOn(r, civ::DD) == civ::FM && civ::modeOn(r, civ::P25) == civ::P25);

  bands::Library lib = bands::builtinPlans();
  for (const bands::Plan& plan : lib.plans) {
    // No hole anywhere from 10 kHz to 3 GHz, bar a gap inside an amateur band's own blocks.
    long long at = 10000;
    for (const bands::Band& b : plan.bands) {
      if (b.lo > at) printf("  %s: nothing from %lld to %lld\n", plan.id.c_str(), at, b.lo);
      CHECK(b.lo <= at);
      at = std::max(at, b.hi);
    }
    CHECK(at == 3000000000LL);
    int groups = 0;
    for (const bands::Band& b : plan.bands) {
      if (b.group.empty()) continue;
      groups++;
      CHECK(!b.title().empty() && b.title() == b.name && b.unit.empty());
      CHECK(r.canTune(b.homeHz));
      CHECK(b.segmentAt(b.homeHz) != nullptr);
      CHECK(!civ::isDigitalVoice(b.homeMode) || b.homeMode == civ::P25);
    }
    CHECK(groups > 100);
    // A transceiver sees none of them, and in the same order as before.
    bands::Plan t = plan.forRadio(false);
    for (const bands::Band& b : t.bands) CHECK(b.group.empty());
    CHECK(t.bands.size() + (size_t)groups == plan.bands.size());
    // The everyday things are where you'd expect.
    int air = plan.bandOf(121500000), fm = plan.bandOf(90000000), marine = plan.bandOf(156800000);
    CHECK(air >= 0 && plan.bands[(size_t)air].group == "Aviation" && plan.bands[(size_t)air].homeMode == civ::AM);
    CHECK(fm >= 0 && plan.bands[(size_t)fm].homeMode == civ::WFM);
    CHECK(marine >= 0 && plan.bands[(size_t)marine].homeMode == civ::FM);
    CHECK(plan.segmentAt(14074000) && plan.bands[(size_t)plan.bandOf(14074000)].group.empty());
  }
  const bands::Plan* uk = lib.find("uk");
  const bands::Plan* jp = lib.find("japan");
  CHECK(uk && jp);
  if (uk && jp) {
    CHECK(uk->segmentAt(446100000) && uk->bands[(size_t)uk->bandOf(446100000)].name == "PMR446");
    CHECK(uk->bandOf(27881250) >= 0 && uk->bands[(size_t)uk->bandOf(27881250)].name == "UK CB 27/81");
    CHECK(jp->bands[(size_t)jp->bandOf(80000000)].homeMode == civ::WFM);
  }

  // A plan copied from before there were receiver bands keeps them, without treading on its own.
  {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path dir = fs::temp_directory_path(ec) / ("ic7300-rx-plans-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(dir, ec);
    std::ofstream(dir / "uk.json", std::ios::binary)
        << "{\"name\": \"T\", \"bands\": [{\"name\": \"Mine\", \"unit\": \"m\", \"home_khz\": 9500, \"home_mode\": \"AM\", \"segments\": [{\"from\": 9400, \"to\": 9900, \"use\": \"other\", \"label\": \"M\"}]}]}";
    bands::Library l = bands::loadPlans(dir.string());
    CHECK(l.errors.empty());
    const bands::Plan* p = l.find("uk");
    CHECK(p && p->bandOf(121500000) >= 0 && p->bandOf(145500000) >= 0);
    CHECK(p && p->bands[(size_t)p->bandOf(9500000)].name == "Mine");
    if (p)
      for (size_t i = 1; i < p->bands.size(); i++) CHECK(p->bands[i].lo >= p->bands[i - 1].hi);
    // But one with a receiver band of its own has decided for itself.
    std::ofstream(dir / "uk.json", std::ios::binary)
        << "{\"name\": \"T\", \"bands\": [{\"name\": \"Mine\", \"group\": \"G\", \"home_khz\": 9500, \"home_mode\": \"AM\", \"segments\": [{\"from\": 9400, \"to\": 9900, \"use\": \"broadcast\"}]}]}";
    l = bands::loadPlans(dir.string());
    CHECK(l.find("uk")->bandOf(121500000) < 0);
    CHECK(l.find("uk")->bands[(size_t)l.find("uk")->bandOf(9500000)].title() == "Mine");
    fs::remove_all(dir, ec);
  }

  // Stations.
  bands::Stations all = bands::builtinStations();
  CHECK(all.errors.empty());
  const bands::StationFile* f = all.find("receiver");
  CHECK(f != nullptr);
  if (!f) return;
  CHECK(f->receiverOnly && !f->source.empty() && !f->note.empty());
  CHECK(!all.find("worldwide")->receiverOnly);
  CHECK(all.forRadio(true).find("receiver") != nullptr && all.forRadio(false).find("receiver") == nullptr);
  CHECK(f->stations.size() > 500);
  bool service[bands::S_COUNT] = {};
  std::map<std::string, std::map<long long, int>> byGroup;
  for (const bands::Station& s : f->stations) {
    service[s.service] = true;
    CHECK(s.preset() && !s.label.empty() && !s.group.empty() && !s.note.empty());
    CHECK(r.canTune(s.hz) && r.canTune(s.tuneHz()));
    if (++byGroup[s.group][s.hz] != 1) printf("  %s: %lld twice\n", s.group.c_str(), s.hz);
    CHECK(byGroup[s.group][s.hz] == 1);
  }
  for (int i : {bands::S_AVIATION, bands::S_SPACE, bands::S_LAND, bands::S_NAVAL, bands::S_MILITARY, bands::S_BROADCAST, bands::S_GOVERNMENT}) CHECK(service[i]);
  auto find = [&](long long hz) -> const bands::Station* {
    for (const bands::StationFile& file : all.files)
      for (const bands::Station& s : file.stations)
        if (s.hz == hz) return &s;
    return nullptr;
  };
  // The formulas, checked against channels everyone knows.
  CHECK(find(156800000) && find(156800000)->label == "Ch 16" && find(156800000)->mode == civ::FM);
  CHECK(find(156525000) && find(156525000)->label == "Ch 70");
  CHECK(find(161600000) && find(161600000)->label == "Ch 20");   // 157.000 ship, 161.600 coast
  CHECK(find(161975000) && find(162025000));
  CHECK(find(121500000) && find(121500000)->mode == civ::AM);
  CHECK(find(446006250) && find(446006250)->label == "PMR 1" && find(446193750) && find(446193750)->label == "PMR 16");
  CHECK(find(433075000) && find(433075000)->label == "LPD 1" && find(434775000) && find(434775000)->label == "LPD 69");
  CHECK(find(27185000) && find(27185000)->label == "CB 19" && find(27405000) && find(27405000)->label == "CB 40");
  CHECK(find(27601250) && find(27601250)->label == "UK 1" && find(27991250) && find(27991250)->label == "UK 40");
  CHECK(find(462562500) && find(462562500)->label == "FRS 1" && find(462725000) && find(462725000)->label == "FRS 22");
  CHECK(find(476425000) && find(476425000)->label == "UHF 1" && find(477412500) && find(477412500)->label == "UHF 80");
  // A coastguard's working channels, which the marine weather groups list under other names too.
  auto channel = [&](long long hz, const char* label) {
    for (const bands::StationFile& file : all.files)
      for (const bands::Station& s : file.stations)
        if (s.hz == hz && s.label == label) return true;
    return false;
  };
  CHECK(channel(4426000, "Ch 424"));
  CHECK(channel(6501000, "Ch 601"));
  CHECK(channel(8764000, "Ch 816"));
  CHECK(channel(13089000, "Ch 1205"));
  CHECK(channel(17314000, "Ch 1625"));
  CHECK(find(225648000) && find(225648000)->label == "12B");
  CHECK(find(474000000) && find(474000000)->label == "E21" && find(690000000) && find(690000000)->label == "E48");
  CHECK(find(1090000000) && find(1575420000));
  CHECK(all.rangeAt(100000000) && all.rangeAt(100000000)->mode == civ::WFM);
  CHECK(all.rangeAt(9500000)->mode == -1);  // a broadcast band going by its service
  // A receiver's search finds what's on the list, and a transceiver's doesn't.
  CHECK(!all.search("ADS-B").empty() && all.search("ADS-B")[0].named);
  CHECK(all.forRadio(false).search("ADS-B").empty());
  CHECK(all.search("PMR 1 ").size() >= 1);
  // What goes wrong in a file somebody's edited.
  {
    bands::StationFile out;
    std::string e;
    CHECK(!bands::parseStations("{\"name\": \"T\", \"ranges\": [{\"from\": 8, \"to\": 9, \"service\": \"land\", \"label\": \"X\", \"mode\": \"SSB\"}]}", "t", out, e));
    CHECK(e.find("SSB") != std::string::npos);
  }
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

static void testClockSync() {
  using namespace clocksync;
  const uint64_t tag = 0x1122334455667788ULL;
  uint8_t req[kPacketSize];
  makeRequest(req, tag);
  CHECK(req[0] == 0x23);
  CHECK(req[40] == 0x11 && req[47] == 0x88);

  // NTP's seconds count from 1900, and wrap in 2036.
  CHECK(ntpToUnix(2208988800ULL << 32) == 0);
  CHECK(ntpToUnix((3991680000ULL << 32) | 0x80000000ULL) == 1782691200.5);
  CHECK(ntpToUnix(1000ULL << 32) == 4294967296.0 + 1000 - 2208988800.0);

  const double base = 1782691200;
  auto reply = [&](double t2, double t3, uint64_t echo, std::vector<uint8_t>& p) {
    p.assign(kPacketSize, 0);
    p[0] = 0x24, p[1] = 2;
    auto put = [&](int at, uint64_t v) {
      for (int i = 0; i < 8; i++) p[(size_t)at + i] = (uint8_t)(v >> (56 - 8 * i));
    };
    auto toNtp = [](double secs) {
      double ntp = secs + 2208988800.0;
      uint64_t sec = (uint64_t)ntp;
      return (sec << 32) | (uint64_t)((ntp - (double)sec) * 4294967296.0);
    };
    put(24, echo), put(32, toNtp(t2)), put(40, toNtp(t3));
  };
  auto near = [](double a, double b) { return fabs(a - b) < 1e-5; };

  // Out at +0, the server's clock 1.5 s ahead, 20 ms each way, 10 ms in the server.
  std::vector<uint8_t> p;
  reply(base + 1.52, base + 1.53, tag, p);
  Sample s;
  CHECK(readReply(p.data(), p.size(), tag, base, base + 0.05, s));
  CHECK(near(s.offset, 1.5));
  CHECK(near(s.delay, 0.04));

  // And the other way: this computer ahead.
  reply(base - 0.48, base - 0.47, tag, p);
  CHECK(readReply(p.data(), p.size(), tag, base, base + 0.05, s));
  CHECK(near(s.offset, -0.5));

  // What must not be believed, as it would go on to set the clock.
  reply(base + 1, base + 1, tag + 1, p);
  CHECK(!readReply(p.data(), p.size(), tag, base, base + 0.05, s));  // answers some other question
  reply(base + 1, base + 1, tag, p);
  CHECK(!readReply(p.data(), kPacketSize - 1, tag, base, base + 0.05, s));  // cut short
  p[0] = 0x23;
  CHECK(!readReply(p.data(), p.size(), tag, base, base + 0.05, s));  // not from a server
  p[0] = 0xE4;
  CHECK(!readReply(p.data(), p.size(), tag, base, base + 0.05, s));  // its own clock isn't set
  p[0] = 0x24, p[1] = 0;
  CHECK(!readReply(p.data(), p.size(), tag, base, base + 0.05, s));  // telling us to go away
  p[1] = 16;
  CHECK(!readReply(p.data(), p.size(), tag, base, base + 0.05, s));  // not synchronised
  reply(base + 1, base + 2, tag, p);
  CHECK(!readReply(p.data(), p.size(), tag, base, base + 0.05, s));  // spent longer there than the trip took
  reply(base + 1, base + 1, tag, p);
  std::fill(p.begin() + 40, p.begin() + 48, (uint8_t)0);
  CHECK(!readReply(p.data(), p.size(), tag, base, base + 0.05, s));  // no time in it

  auto sample = [](double offset, double delay) {
    Sample x;
    x.offset = offset, x.delay = delay;
    return x;
  };
  Sample best;
  CHECK(!choose({}, best));
  CHECK(choose({sample(1.0, 0.09)}, best) && best.offset == 1.0);
  CHECK(choose({sample(1.0, 0.09), sample(1.1, 0.02)}, best) && best.offset == 1.1);  // the quicker trip wins
  CHECK(!choose({sample(1.0, 0.02), sample(2.0, 0.02)}, best));

  CHECK(judge(0) == V_CLOSE_ENOUGH);
  CHECK(judge(-kCloseEnough) == V_CLOSE_ENOUGH);
  CHECK(judge(0.5) == V_STEP);
  CHECK(judge(-0.5) == V_STEP);
  CHECK(judge(kMostToStep) == V_STEP);
  CHECK(judge(kMostToStep + 1) == V_TOO_FAR);
  CHECK(judge(-kMostToStep - 1) == V_TOO_FAR);
  CHECK(describe(0.3124) == "0.312 s slow");
  CHECK(describe(-1.5) == "1.500 s fast");
}

static void testScan() {
  std::vector<scan::Channel> ch(3);
  ch[0].hz = 14000000, ch[1].hz = 14005000, ch[2].hz = 14010000;
  scan::Config c;
  c.dwell = 0.5, c.settle = 0.25, c.recover = 1.0, c.moved = 0.6, c.dropFor = 2, c.thresholdDb = -24, c.resume = scan::ON_DROP;
  scan::Scanner s;
  const double quiet = -40, loud = -6;

  // Up through the channels and round again, on nothing but quiet.
  s.start(ch, c);
  CHECK(s.running());
  CHECK(s.update(0.0, 7000000, quiet) == scan::TUNE && s.current().hz == 14000000);
  CHECK(s.update(0.1, 14000000, quiet) == scan::NONE);
  CHECK(s.update(0.5, 14000000, quiet) == scan::TUNE && s.current().hz == 14005000);
  CHECK(s.update(1.0, 14005000, quiet) == scan::TUNE && s.current().hz == 14010000);
  CHECK(s.update(1.5, 14010000, quiet) == scan::TUNE && s.current().hz == 14000000 && s.passes() == 1);

  // A loud reading while the rig is still settling doesn't count, as it's the last channel's.
  s.start(ch, c);
  s.update(0.0, 0, quiet);
  CHECK(s.update(0.1, 14000000, loud) == scan::NONE);
  // After settling it does, and it stays until the signal has gone for long enough.
  CHECK(s.update(0.3, 14000000, loud) == scan::HIT && s.state() == scan::STOPPED);
  CHECK(s.update(2.0, 14000000, loud) == scan::NONE);
  CHECK(s.update(3.0, 14000000, quiet) == scan::NONE);
  CHECK(s.update(4.1, 14000000, quiet) == scan::TUNE && s.current().hz == 14005000);
  // The AGC gets longer to let go after a signal.
  CHECK(s.update(4.5, 14005000, loud) == scan::NONE);
  CHECK(s.update(5.2, 14005000, loud) == scan::HIT);

  // Staying put means staying put.
  c.resume = scan::HOLD;
  s.start(ch, c);
  s.update(0.0, 0, quiet);
  CHECK(s.update(0.3, 14000000, loud) == scan::HIT);
  CHECK(s.update(100.0, 14000000, quiet) == scan::NONE && s.state() == scan::STOPPED);
  s.next();
  CHECK(s.current().hz == 14005000 && s.state() == scan::LISTENING);

  c.resume = scan::AFTER_5S;
  s.start(ch, c);
  s.update(0.0, 0, quiet);
  s.update(0.3, 14000000, loud);
  CHECK(s.update(4.0, 14000000, loud) == scan::NONE);
  CHECK(s.update(5.4, 14000000, loud) == scan::TUNE);

  // Somebody tuning ends it, and until that's certain the scan doesn't tune over them.
  c.resume = scan::ON_DROP;
  s.start(ch, c);
  s.update(0.0, 0, quiet);
  CHECK(s.update(0.1, 14000000, quiet) == scan::NONE);
  CHECK(s.update(0.45, 14001000, quiet) == scan::NONE);
  CHECK(s.update(0.9, 14001000, quiet) == scan::NONE);
  CHECK(s.update(1.1, 14001000, quiet) == scan::DIAL_MOVED && !s.running());
  // A stale reading that comes right doesn't.
  s.start(ch, c);
  s.update(0.0, 0, quiet);
  CHECK(s.update(0.2, 13999000, quiet) == scan::NONE);
  CHECK(s.update(0.3, 14000000, quiet) == scan::NONE && s.running());

  // Locking out skips a channel, and locking them all out ends it.
  s.start(ch, c);
  s.update(0.0, 0, quiet);
  s.lockOut();
  CHECK(s.current().hz == 14005000 && s.lockedCount() == 1);
  s.lockOut();
  s.lockOut();
  CHECK(!s.running());
  CHECK(s.update(1.0, 0, quiet) == scan::NOTHING_LEFT);
  CHECK(s.update(1.1, 0, quiet) == scan::NONE);

  // It can start partway through.
  s.start(ch, c, 2);
  CHECK(s.update(0.0, 0, quiet) == scan::TUNE && s.current().hz == 14010000);
  std::vector<scan::Channel> none;
  s.start(none, c);
  CHECK(!s.running());
}

static void testDxSpots() {
  dx::Spot s;
  CHECK(dx::parseSpot("DX de W3LPL:     14025.0  JA1XYZ       CW 12 dB 25 WPM CQ     1205Z", s));
  CHECK(s.spotter == "W3LPL" && s.hz == 14025000 && s.call == "JA1XYZ" && s.utc == "1205" && s.comment == "CW 12 dB 25 WPM CQ" && !s.skimmer);
  CHECK(dx::parseSpot("DX de K1TTT-#:   7012.3  dl1abc       CW 3 dB 18 WPM CQ 2359Z FN32", s));
  CHECK(s.skimmer && s.call == "DL1ABC" && s.hz == 7012300 && s.utc == "2359" && s.comment == "CW 3 dB 18 WPM CQ");
  CHECK(dx::parseSpot("DX de G4ABC:    144300.0  OZ1ABC", s));
  CHECK(s.hz == 144300000 && s.comment.empty() && s.utc.empty());
  CHECK(dx::parseSpot("  dx de G4ABC:  3780.0  GM3ABC  worked at 1200z today 0912Z", s));
  CHECK(s.utc == "0912" && s.comment == "worked at 1200z today");
  CHECK(!dx::parseSpot("VE7CC-1 de G4ABC 1205Z >", s));
  CHECK(!dx::parseSpot("DX de G4ABC: not a frequency", s));
  CHECK(!dx::parseSpot("DX de G4ABC:  14025.0", s));
  CHECK(!dx::parseSpot("", s));

  CHECK(dx::modeHint("FT8 -12 dB") == dx::HINT_FT8);
  CHECK(dx::modeHint("cq ft4") == dx::HINT_FT4);
  CHECK(dx::modeHint("CW up 2") == dx::HINT_CW);
  CHECK(dx::modeHint("loud on usb") == dx::HINT_SSB);
  CHECK(dx::modeHint("psk31 cq") == dx::HINT_DATA);
  CHECK(dx::modeHint("I am here, swamp") == dx::NO_HINT);
  CHECK(dx::modeHint("big signal") == dx::NO_HINT);

  dx::SpotList l;
  dx::Spot a;
  a.call = "G4ABC", a.hz = 14200000, a.gotAt = 100, a.comment = "first";
  l.add(a);
  dx::Spot b = a;
  b.call = "G4DEF", b.gotAt = 110;
  l.add(b);
  CHECK(l.spots().size() == 2 && l.spots()[0].call == "G4DEF");
  dx::Spot again = a;
  again.hz = 14200500, again.gotAt = 120, again.comment = "";
  l.add(again);
  CHECK(l.spots().size() == 2 && l.spots()[0].call == "G4ABC" && l.spots()[0].count == 2 && l.spots()[0].comment == "first");
  dx::Spot elsewhere = a;
  elsewhere.hz = 7100000;
  l.add(elsewhere);
  CHECK(l.spots().size() == 3);
  CHECK(l.prune(165, 60) && l.spots().size() == 2);  // the one from 100 is too old, and those from 110 and 120 aren't
  CHECK(!l.prune(166, 60));
  for (int i = 0; i < 1100; i++) {
    dx::Spot m = a;
    m.call = "M" + std::to_string(i);
    l.add(m);
  }
  CHECK(l.spots().size() == 1000);

  CHECK(dx::countryOf("G4ABC") == "England");
  CHECK(dx::countryOf("GM3ABC") == "Scotland");
  CHECK(dx::countryOf("W1AW") == "United States");
  CHECK(dx::countryOf("KH6ABC") == "Hawaii");
  CHECK(dx::countryOf("JA1XYZ") == "Japan");
  CHECK(dx::countryOf("BV2A") == "Taiwan");
  CHECK(dx::countryOf("BY1AB") == "China");
  CHECK(dx::countryOf("DL1ABC") == "Germany");
  CHECK(dx::countryOf("EA8ABC") == "Canary Islands");
  CHECK(dx::countryOf("VP2MAB") == "Montserrat");
  CHECK(dx::countryOf("3DA0AB") == "Eswatini");
  CHECK(dx::countryOf("3D2AB") == "Fiji");
  CHECK(dx::countryOf("HB9/G3ABC") == "Switzerland");
  CHECK(dx::countryOf("G3ABC/HB9") == "Switzerland");
  CHECK(dx::countryOf("G3ABC/P") == "England");
  CHECK(dx::countryOf("w1aw/m") == "United States");
  CHECK(dx::countryOf("") == "");
  CHECK(dx::countryOf("Q1ABC") == "");

  double km = 0, deg = 0;
  CHECK(dx::pathBetween("IO91", "IO91", km, deg) && km < 1);
  CHECK(dx::pathBetween("IO91WM", "FN31pr", km, deg) && fabs(km - 5500) < 150 && deg > 270 && deg < 300);  // London to Connecticut
  CHECK(!dx::pathBetween("IO91", "ZZ99", km, deg));
  CHECK(!dx::pathBetween("IO9", "IO91", km, deg));
  CHECK(!dx::pathBetween("", "IO91", km, deg));
  double km6 = 0;
  CHECK(dx::pathBetween("IO91", "FN31pr", km6, deg) && dx::pathBetween("IO91", "FN31pr45", km, deg) && km == km6);  // longer locators read as 6
  double lat = 0, lon = 0;
  CHECK(dx::gridToLatLon("io91wm", lat, lon) && fabs(lat - 51.52) < 0.05 && fabs(lon + 0.12) < 0.1);  // London
  CHECK(!dx::gridToLatLon("", lat, lon));
}

static void testLogFields() {
  auto path = std::filesystem::temp_directory_path() / "ic7300_test_log.csv";
  std::string file = path.string();
  std::filesystem::remove(path);
  // A line from before the new fields, then a new one.
  {
    std::ofstream f(file, std::ios::binary);
    f << "2026-01-02 03:04,G4OLD,14205000,USB,59,57,Ann,hello\n";
  }
  store::LogEntry e;
  e.utc = "2026-10-31 19:05", e.utcOff = "2026-10-31 19:09", e.call = "G4ABC", e.hz = 14205000, e.mode = "USB";
  e.rstSent = "59", e.rstRcvd = "57", e.name = "Bob", e.notes = "with, comma", e.power = "100", e.qth = "Leeds", e.locator = "IO93", e.country = "England";
  e.contest = "CQ-WW-SSB", e.exchSent = "007", e.exchRcvd = "14";
  e.distKm = "290", e.distMiles = "180";
  CHECK(store::appendLog(file, e));
  auto log = store::loadLog(file);
  CHECK(log.size() == 2);
  if (log.size() == 2) {
    CHECK(log[0].call == "G4OLD" && log[0].notes == "hello" && log[0].utcOff.empty() && log[0].country.empty() && log[0].distKm.empty());
    CHECK(log[1].distKm == "290" && log[1].distMiles == "180");
    CHECK(log[1].utcOff == e.utcOff && log[1].notes == e.notes && log[1].power == "100" && log[1].qth == "Leeds" && log[1].locator == "IO93");
    CHECK(log[1].country == "England" && log[1].contest == "CQ-WW-SSB" && log[1].exchSent == "007" && log[1].exchRcvd == "14");
  }

  std::string a = store::adifRecord(e, "M0XYZ", "IO91WM");
  CHECK(a.find("<TIME_OFF:4>1909") != std::string::npos && a.find("<QSO_DATE_OFF:8>20261031") != std::string::npos);
  CHECK(a.find("<TX_PWR:3>100") != std::string::npos && a.find("<QTH:5>Leeds") != std::string::npos && a.find("<GRIDSQUARE:4>IO93") != std::string::npos);
  CHECK(a.find("<COUNTRY:7>England") != std::string::npos && a.find("<CONTEST_ID:9>CQ-WW-SSB") != std::string::npos);
  CHECK(a.find("<STX_STRING:3>007") != std::string::npos && a.find("<SRX_STRING:2>14") != std::string::npos && a.find("<MY_GRIDSQUARE:6>IO91WM") != std::string::npos);
  store::LogEntry ft4 = e;
  ft4.mode = "FT4";
  CHECK(store::adifRecord(ft4, "").find("<MODE:4>MFSK") != std::string::npos && store::adifRecord(ft4, "").find("<SUBMODE:3>FT4") != std::string::npos);

  // What it writes, it reads back.
  auto back = store::parseAdif("header text\n<ADIF_VER:5>3.1.4 <EOH>\n" + a);
  CHECK(back.size() == 1);
  if (back.size() == 1) {
    const store::LogEntry& r = back[0];
    CHECK(r.call == "G4ABC" && r.utc == e.utc && r.utcOff == e.utcOff && r.hz == e.hz && r.mode == "USB" && r.rstRcvd == "57");
    CHECK(r.notes == e.notes && r.power == "100" && r.qth == "Leeds" && r.locator == "IO93" && r.country == "England" && r.contest == "CQ-WW-SSB" && r.exchSent == "007");
  }
  // Other programs' files: lower-case names, types in the tag, no header, a band and no frequency, seconds on the time.
  auto other = store::parseAdif(
      "<call:5>w1abc <qso_date:8:D>20250102 <time_on:6>123456 <band:3>40m <mode:3>SSB <eor>\n"
      "<CALL:5>K1ABC<QSO_DATE:8>20250103<TIME_ON:4>0102<FREQ:6>14.074<MODE:4>MFSK<SUBMODE:3>FT4<EOR>\n"
      "<QSO_DATE:8>20250104<TIME_ON:4>0102<EOR>\n");
  CHECK(other.size() == 2);
  if (other.size() == 2) {
    CHECK(other[0].call == "W1ABC" && other[0].utc == "2025-01-02 12:34" && other[0].mode == "LSB" && std::string(bands::adifBand(other[0].hz)) == "40M");
    CHECK(other[1].mode == "FT4" && other[1].hz == 14074000);
  }

  // Importing adds what's new, doesn't repeat what's already there, and puts the log in order.
  auto adi = std::filesystem::temp_directory_path() / "ic7300_test_import.adi";
  auto writeAdi = [&](const std::string& text) {
    std::ofstream f(adi.string(), std::ios::binary);
    f << text;
  };
  writeAdi(a + "<CALL:5>W1ABC<QSO_DATE:8>20250102<TIME_ON:4>1234<BAND:3>40M<MODE:2>CW<EOR>\n");
  int added = 0, updated = 0, skipped = 0;
  std::string error;
  CHECK(store::importAdif(adi.string(), log, "M0XYZ", added, updated, skipped, error));
  CHECK(added == 1 && updated == 1 && skipped == 0 && log.size() == 3 && log[0].call == "W1ABC");  // the one there gains its station
  CHECK(log[0].station == "M0XYZ" && log[2].station == "M0XYZ");
  CHECK(store::importAdif(adi.string(), log, "M0XYZ", added, updated, skipped, error) && added == 0 && updated == 0 && skipped == 2);
  CHECK(!store::importAdif(file + ".nothing", log, "", added, updated, skipped, error) && !error.empty());

  // A paper confirmation on an old record: updates the contact, and doesn't overwrite what's there.
  writeAdi("<CALL:5>w1abc<QSO_DATE:8>20250102<TIME_ON:6>123559<BAND:3>40M<MODE:3>SSB<NAME:3>Sam<COMMENT:2>hi<QSL_RCVD:1>Y<QSLRDATE:8>20250301<EOR>\n");
  CHECK(store::importAdif(adi.string(), log, "M0XYZ", added, updated, skipped, error) && added == 0 && updated == 1 && log.size() == 3);
  CHECK(log[0].qslRcvd == "Y" && log[0].qslRcvdDate == "20250301" && log[0].mode == "CW");
  CHECK(log[0].name == "Sam" && log[0].notes == "hi" && store::confirmedBy(log[0]) == "QSL");
  // An old file saying it isn't confirmed doesn't take that back.
  writeAdi("<CALL:5>W1ABC<QSO_DATE:8>20250102<TIME_ON:4>1234<BAND:3>40M<QSL_RCVD:1>N<EOR>\n");
  CHECK(store::importAdif(adi.string(), log, "M0XYZ", added, updated, skipped, error) && added == 0 && updated == 0 && skipped == 1 && log[0].qslRcvd == "Y");
  // Two minutes on is another contact, and the same moment under another of their callsigns is too.
  writeAdi("<CALL:5>W1ABC<QSO_DATE:8>20250102<TIME_ON:4>1236<BAND:3>40M<EOR>\n<CALL:5>W1ABC<QSO_DATE:8>20250102<TIME_ON:4>1234<BAND:3>40M<STATION_CALLSIGN:6>GB1XYZ<EOR>\n");
  CHECK(store::importAdif(adi.string(), log, "M0XYZ", added, updated, skipped, error) && added == 2 && log.size() == 5);
  // A LoTW report's QSL_RCVD is LoTW's, however the file says it.
  writeAdi("ARRL Logbook of the World Status Report\n<PROGRAMID:4>LoTW\n<eoh>\n<CALL:5>G4ABC<BAND:3>20M<QSO_DATE:8>20261031<TIME_ON:4>1905<STATION_CALLSIGN:5>M0XYZ<QSL_RCVD:1>Y<QSLRDATE:8>20261101<EOR>\n");
  CHECK(store::importAdif(adi.string(), log, "M0XYZ", added, updated, skipped, error) && added == 0 && updated == 1);
  {
    const store::LogEntry& g = log.back();
    CHECK(g.call == "G4ABC" && g.lotwRcvd == "Y" && g.lotwRcvdDate == "20261101" && g.qslRcvd.empty() && store::confirmedBy(g) == "LoTW");
    // And it all goes round the file and through an export.
    std::string out = store::adifRecord(g, "OTHER");
    CHECK(out.find("<LOTW_QSL_RCVD:1>Y") != std::string::npos && out.find("<STATION_CALLSIGN:5>M0XYZ") != std::string::npos);
    CHECK(store::saveLog(file, log));
    auto reread = store::loadLog(file);
    CHECK(reread.size() == log.size() && reread.back().lotwRcvd == "Y" && reread.back().station == "M0XYZ");
  }
  // An eQSL inbox has the other station's clock, and files their sending under this log's receiving.
  writeAdi("Received eQSLs for M0XYZ\n<PROGRAMID:21>eQSL.cc DownloadInBox\n<EOH>\n<CALL:5>G4ABC<BAND:3>20M<QSO_DATE:8>20261031<TIME_ON:4>1925<QSL_SENT:1>Y<QSL_SENT_VIA:1>E<EOR>\n");
  CHECK(store::importAdif(adi.string(), log, "M0XYZ", added, updated, skipped, error) && added == 0 && updated == 1);
  CHECK(log.back().eqslRcvd == "Y" && log.back().qslSent.empty() && store::confirmedBy(log.back()) == "LoTW, eQSL");

  CHECK(store::backupFile(file));
  CHECK(std::filesystem::exists(file + ".bak"));
  CHECK(store::backupFile(file + ".nothing"));
  std::filesystem::remove(path);
  std::filesystem::remove(file + ".bak");
  std::filesystem::remove(adi);
}

static void testUpdateCheck() {
  CHECK(update::validStamp("2026-11-01 14:05"));
  CHECK(!update::validStamp("2026-11-01"));
  CHECK(!update::validStamp("404: Not Found"));
  CHECK(!update::validStamp(""));
  CHECK(update::isNewer("2026-11-02 09:00", "2026-11-01 14:05"));
  CHECK(update::isNewer("2026-11-01 14:06", "2026-11-01 14:05"));
  CHECK(!update::isNewer("2026-11-01 14:05", "2026-11-01 14:05"));
  CHECK(!update::isNewer("2026-10-31 23:59", "2026-11-01 14:05"));
  CHECK(!update::isNewer("", "2026-11-01 14:05"));
  CHECK(!update::isNewer("<html>", "2026-11-01 14:05"));
}

int main() {
  struct {
    const char* name;
    void (*fn)();
  } tests[] = {
      {"CI-V bytes", testBytes}, {"frame reader", testReader}, {"frequency entry", testTuning}, {"band plans", testBandPlans}, {"stations", testStations}, {"a receiver's bands and stations", testReceiverData}, {"tuning dial", testDial}, {"FFT", testDsp}, {"needle meter", testNeedle},
      {"log and CSV", testStore}, {"log fields and ADIF import", testLogFields}, {"scanning", testScan}, {"DX spots", testDxSpots}, {"rig basics", testRigBasics}, {"which radio", testDetection}, {"dial spin", testCoalesce},
      {"stale readings", testStaleReadings}, {"echo back", testEcho}, {"refused commands", testRefused},
      {"scope", testScope}, {"TX guards", testTxGuards}, {"TX watchdog", testWatchdog}, {"un-key retry", testUnkeyRetry},
      {"un-key across an outage", testUnkeyAcrossOutage}, {"un-key at shutdown", testShutdownUnkeys},
      {"link loss", testLinkLoss}, {"port errors", testPortBusyMessage}, {"setting the clock", testClockSync}, {"newer build", testUpdateCheck},
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
