// ic7300probe: checks this app's CI-V against a real radio, from the console.
//   ic7300probe                      finds the port, reads everything it can, listens for scope data
//   ic7300probe COM11                on that port
//   ic7300probe COM11 --write-same   also sends the frequency and mode the rig is already on
//
// It is safe to run on a live station. It never keys the transmitter and never starts the tuner.
// Without --write-same, its only writes are "scope data on" and, on the way out, "scope data off".
// --write-same proves that writes are accepted, but changes nothing, as it sends the current values.
#include <stdio.h>
#include <string.h>

#include <chrono>
#include <string>
#include <thread>

#include "rig.h"
#include "tuning.h"

static void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

int main(int argc, char** argv) {
  std::string port;
  bool writeSame = false;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--write-same")) writeSame = true;
    else port = argv[i];
  }
  auto ports = listSerialPorts();
  printf("Serial ports:\n");
  for (auto& p : ports) printf("  %s  %s\n", p.name.c_str(), p.desc.c_str());
  if (port.empty()) port = guessRigPort(ports);
  if (port.empty()) {
    printf("No IC-7300 found. Name the port: ic7300probe COM11\n");
    return 2;
  }
  printf("Using %s at 115200, CI-V address 94h\n\n", port.c_str());

  Rig rig;
  rig.start([&] { return newSerialTransport(port, 115200); });

  RigState s;
  for (int i = 0; i < 400; i++) {
    s = rig.state();
    if (s.connected) break;
    if (!s.portError.empty() && i > 20) break;
    sleepMs(25);
  }
  if (!s.connected) {
    printf("FAIL: no answer from the rig. %s\n", s.portError.empty() ? "The port opened but nothing replied." : s.portError.c_str());
    printf("Check: rig powered on; CI-V USB Baud Rate 115200; CI-V Address 94h.\n");
    printf("If the port's in use, close Station or MorseRx first: a port can only have one owner.\n");
    return 1;
  }

  sleepMs(4000);  // long enough for a full polling cycle
  s = rig.state();
  int fails = 0;
  auto require = [&](const char* what, bool ok, const std::string& detail) {
    printf("  %-5s %-22s %s\n", ok ? "ok" : "FAIL", what, detail.c_str());
    if (!ok) fails++;
  };
  auto n = [](long long v) { return std::to_string(v); };
  printf("Identity and dial:\n");
  char id[16];
  snprintf(id, sizeof id, "%02Xh", s.rigId);
  require("rig id", s.rigId == 0x94, id);
  require("frequency", s.hz > 0, tuning::formatHz(s.hz));
  require("mode", s.mode >= 0, std::string(civ::modeName(s.mode)) + (s.data ? "-D" : "") + " FIL" + n(s.filter));
  require("filter width", s.width >= 0, n(civ::widthHz(s.mode, s.width)) + " Hz");
  require("other VFO", s.otherHz > 0, tuning::formatHz(s.otherHz));
  require("other VFO mode", s.otherMode >= 0, civ::modeName(s.otherMode));
  require("split", s.split >= 0, n(s.split));
  require("attenuator", s.att >= 0, n(s.att));
  require("tuner", s.atu >= 0, n(s.atu));
  require("RIT", s.ritOn >= 0, n(s.ritOn) + ", offset " + n(s.ritHz) + " Hz");
  require("XIT", s.xitOn >= 0, n(s.xitOn));

  printf("Levels:\n");
  static const char* levelNames[civ::L_COUNT] = {"AF", "RF", "SQL", "NR", "PBT inside", "PBT outside", "CW pitch", "RF power", "Mic",
                                                 "Key speed", "Notch", "Comp", "BK-IN delay", "NB", "Monitor", "VOX", "Anti-VOX"};
  for (int l = 0; l < civ::L_COUNT; l++) require(levelNames[l], s.level[l] >= 0, n(s.level[l]));

  printf("Switches:\n");
  static const char* swNames[civ::S_COUNT] = {"Preamp", "AGC", "NB", "NR", "Auto notch", "Tone", "TSQL", "Comp", "Monitor", "VOX", "BK-IN",
                                              "Manual notch", "Dial lock", "DSP filter", "Notch width", "SSB TX width"};
  for (int i = 0; i < civ::S_COUNT; i++) require(swNames[i], s.sw[i] >= 0, n(s.sw[i]));

  printf("Meters:\n");
  require("S meter", s.meter[civ::M_S] >= 0, n(s.meter[civ::M_S]) + " raw");
  require("Vd", s.meter[civ::M_VD] >= 0, std::to_string(civ::voltsVd(s.meter[civ::M_VD])) + " V");

  printf("Scope:\n");
  rig.setScope(true);
  ScopeLine line;
  int sweeps = 0, wrongLength = 0;
  auto t0 = std::chrono::steady_clock::now();
  while (std::chrono::steady_clock::now() - t0 < std::chrono::seconds(4)) {
    while (rig.popScope(line)) {
      sweeps++;
      if ((int)line.levels.size() != civ::kScopePoints) wrongLength++;
    }
    sleepMs(10);
  }
  s = rig.state();
  require("scope sweeps", sweeps > 0, n(sweeps / 4) + " a second");
  if (sweeps == 0) printf("        If this fails, set the rig's CI-V USB Port to Unlink from [REMOTE] and CI-V USB Baud Rate to 115200.\n");
  if (sweeps) {
    require("sweep length", wrongLength == 0, n((long long)line.levels.size()) + " points");
    require("sweep edges", line.hiHz > line.loHz, tuning::formatHz(line.loHz) + " to " + tuning::formatHz(line.hiHz));
    int peak = 0;
    for (uint8_t v : line.levels) peak = v > peak ? v : peak;
    require("sweep levels", peak <= civ::kScopeLevelMax, "peak " + n(peak) + " of " + n(civ::kScopeLevelMax));
  }
  require("span", s.scopeSpan > 0, n(s.scopeSpan) + " Hz");
  require("scope mode", s.scopeMode >= 0, n(s.scopeMode));
  require("sweep speed", s.scopeSpeed >= 0, n(s.scopeSpeed));
  require("reference", s.scopeRefKnown, std::to_string(s.scopeRef) + " dB");

  if (writeSame) {
    printf("Writes (the values it already has):\n");
    uint64_t errs = s.lastErrorSeq;
    long long hz = s.hz;
    int mode = s.mode, filter = s.filter;
    bool data = s.data;
    rig.setFreq(hz);
    rig.setMode(mode, data, filter);
    rig.setLevel(civ::L_AF, s.level[civ::L_AF]);
    rig.setScopeSpan(s.scopeSpan);
    sleepMs(800);
    s = rig.state();
    require("writes accepted", s.lastErrorSeq == errs, s.lastErrorSeq == errs ? "" : s.lastError);
    require("dial unchanged", s.hz == hz, tuning::formatHz(s.hz));
    require("mode unchanged", s.mode == mode && s.data == data, civ::modeName(s.mode));
  }

  rig.stop();
  printf("\n%s\n", fails ? "SOME CHECKS FAILED" : "ALL CHECKS PASSED");
  return fails ? 1 : 0;
}
