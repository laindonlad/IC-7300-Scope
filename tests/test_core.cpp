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

}

static void testBandPlans() {
  namespace fs = std::filesystem;
  bands::Library lib = bands::builtinPlans();
  for (const std::string& e : lib.errors) printf("  %s\n", e.c_str());
  CHECK(lib.errors.empty());
  CHECK(lib.plans.size() == 9);
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
    for (size_t i = 1; i < plan.bands.size(); i++) CHECK(plan.bands[i].lo > plan.bands[i - 1].hi);
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
  bands::Stations lib = bands::builtinStations();
  for (const std::string& e : lib.errors) printf("  %s\n", e.c_str());
  CHECK(lib.errors.empty());
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
    CHECK(s.tuneHz() >= 30000 && s.tuneHz() <= 74800000);
    CHECK(!s.label.empty() && !s.group.empty() && !s.note.empty());
    at[s.hz] = &s;
    // Nothing's listed twice in one group.
    int same = 0;
    for (const bands::Station& o : w->stations) same += o.hz == s.hz && o.group == s.group;
    if (same != 1) printf("  %s: %lld appears %d times\n", s.group.c_str(), s.hz, same);
    CHECK(same == 1);
  }
  for (bool any : service) CHECK(any);
  // A fax flag sits on the signal, and tunes USB below it.
  CHECK(at.count(8040000) && at[8040000]->tuneHz() == 8038100 && at[8040000]->mode == civ::USB);
  CHECK(at.count(518000) && at[518000]->tuneHz() == 516300);
  CHECK(at.count(2182000) && at[2182000]->tuneHz() == 2182000);
  CHECK(at.count(4625000) && at[4625000]->service == bands::S_MILITARY);
  CHECK(at.count(11175000));
  // Stations that have closed down.
  CHECK(!at.count(3330000) && !at.count(7850000) && !at.count(14670000));  // CHU, June 2026
  CHECK(!at.count(198000));  // BBC Radio 4 long wave, June 2026
  CHECK(lib.rangeAt(9500000) && lib.rangeAt(9500000)->service == bands::S_BROADCAST);
  CHECK(lib.rangeAt(14074000) == nullptr);
  bool none[bands::S_COUNT] = {};
  CHECK(lib.rangeAt(9500000, none) == nullptr);

  // Search: any case, every word, and what it names first.
  auto count = [&](const char* q) { return lib.search(q).size(); };
  CHECK(count("") == w->stations.size() + w->ranges.size());
  CHECK(count("NORTHWOOD") == 4);
  CHECK(count("northwood 8040") == 1);
  CHECK(count("navtex") >= 3);
  CHECK(count("zzzz") == 0);
  CHECK(count("wefax") > 40);
  CHECK(lib.search("  8040  ")[0].station && lib.search("8040")[0].station->hz == 8040000);
  {
    auto hits = lib.search("buzzer");
    CHECK(hits.size() == 1 && hits[0].station->hz == 4625000 && hits[0].named);
    hits = lib.search("31 m");
    CHECK(!hits.empty() && hits[0].range && hits[0].range->lo == 9400000);
    // GYA's label names it, and comes first although lower ones only mention the word.
    hits = lib.search("gya");
    CHECK(hits.size() == 4 && hits[0].named && hits[0].station->hz == 2618500);
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
  CHECK(errorOf(file(naval, "{\"khz\": 25, \"label\": \"RJH\"}")).find("receive") != std::string::npos);
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
  CHECK(bands::loadStations(dir.string()).find("worldwide")->stations.size() == 1);
  // Your own list goes alongside, and search covers it.
  write("mine.json", file(naval, "{\"khz\": 9000, \"label\": \"Mine\"}"));
  {
    bands::Stations l = bands::loadStations(dir.string());
    CHECK(l.files.size() == 2 && l.search("mine").size() == 1);
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
      {"CI-V bytes", testBytes}, {"frame reader", testReader}, {"frequency entry", testTuning}, {"band plans", testBandPlans}, {"stations", testStations}, {"tuning dial", testDial}, {"FFT", testDsp},
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
