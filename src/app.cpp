#define IMGUI_DEFINE_MATH_OPERATORS  // has to come before the first imgui.h
#include "app.h"

#include <GLFW/glfw3.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <array>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "audio.h"
#include "needle.h"
#include "bands.h"
#include "civ.h"
#include "clock_sync.h"
#include "demo_audio.h"
#include "dsp.h"
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "imgui_internal.h"  // ImRect, for the self-test's map of controls
#include "rig.h"
#include "sim_rig.h"
#include "store.h"
#include "transport.h"
#include "tuning.h"
#include "waterfall.h"

#include "font_cousine.inc"
#include "font_roboto.inc"

namespace {
const ImU32 C_BG = IM_COL32(16, 19, 24, 255);
const ImU32 C_PANEL = IM_COL32(24, 28, 35, 255);
const ImU32 C_DIM = IM_COL32(125, 135, 150, 255);
const ImU32 C_TEXT = IM_COL32(222, 228, 235, 255);
const ImU32 C_ACCENT = IM_COL32(64, 190, 225, 255);
const ImU32 C_AMBER = IM_COL32(255, 190, 70, 255);
const ImU32 C_GREEN = IM_COL32(90, 210, 120, 255);
const ImU32 C_RED = IM_COL32(235, 70, 60, 255);
const ImVec4 V_ACCENT(0.15f, 0.47f, 0.60f, 1), V_ACCENT_HI(0.20f, 0.56f, 0.70f, 1);
const ImVec4 V_RED(0.72f, 0.16f, 0.14f, 1), V_RED_HI(0.86f, 0.22f, 0.18f, 1);
const ImVec4 V_AMBER(0.60f, 0.42f, 0.08f, 1), V_AMBER_HI(0.72f, 0.52f, 0.12f, 1);

const long long kSteps[] = {1, 10, 100, 500, 1000, 2500, 5000, 9000, 10000, 100000, 1000000};
const char* const kStepNames[] = {"1 Hz", "10 Hz", "100 Hz", "500 Hz", "1 kHz", "2.5 kHz", "5 kHz", "9 kHz", "10 kHz", "100 kHz", "1 MHz"};
const int kStepCount = 11;
const int kFftSizes[] = {2048, 4096, 8192, 16384};
const char* const kFftNames[] = {"2048 (23 Hz)", "4096 (12 Hz)", "8192 (6 Hz)", "16384 (3 Hz)"};
const float kAudioRanges[] = {3000, 4000, 6000, 12000, 24000};
const char* const kAudioRangeNames[] = {"0-3 kHz", "0-4 kHz", "0-6 kHz", "0-12 kHz", "0-24 kHz"};
const char* const kSpanNames[] = {"+/-2.5k", "+/-5k", "+/-10k", "+/-25k", "+/-50k", "+/-100k", "+/-250k", "+/-500k", "+/-1M", "+/-2.5M"};
const int kWaterfallRows = 1024;

std::string utcNow(const char* fmt) {
  time_t t = time(nullptr);
  struct tm u;
#ifdef _WIN32
  gmtime_s(&u, &t);
#else
  gmtime_r(&t, &u);
#endif
  char b[64];
  strftime(b, sizeof b, fmt, &u);
  return b;
}

std::string modeLabel(int mode, bool data) {
  if (mode < 0) return "---";
  return std::string(civ::modeName(mode)) + (data && civ::hasData(mode) ? "-D" : "");
}

std::string smeterText(int raw) {
  if (raw < 0) return "--";
  double db = civ::smeterDb(raw);
  char b[32];
  if (db > 0.5) snprintf(b, sizeof b, "S9+%d", (int)lround(db));
  else snprintf(b, sizeof b, "S%d", std::max(0, (int)lround(9 + db / 6)));
  return b;
}

double niceStep(double span, int wanted) {
  double raw = span / wanted, mag = pow(10.0, floor(log10(raw))), f = raw / mag;
  return (f < 1.5 ? 1 : f < 3.5 ? 2 : f < 7.5 ? 5 : 10) * mag;
}

void glfwError(int code, const char* msg) { fprintf(stderr, "GLFW %d: %s\n", code, msg); }

// The window icon: a tiny waterfall, drawn here so there's no image file to carry about.
#ifndef _WIN32  // on Windows the icon is in the executable: see res/ic7300console.rc
void setIcon(GLFWwindow* w) {
  static unsigned char px[32 * 32 * 4];
  for (int y = 0; y < 32; y++)
    for (int x = 0; x < 32; x++) {
      unsigned char* p = &px[(y * 32 + x) * 4];
      double a = exp(-pow((x - 10 - y / 8.0) / 2.2, 2)) + 0.75 * exp(-pow((x - 23) / 1.6, 2));
      p[0] = (unsigned char)std::min(255.0, 20 + 230 * a);
      p[1] = (unsigned char)std::min(255.0, 40 + 210 * a);
      p[2] = (unsigned char)std::min(255.0, 90 + 130 * a);
      p[3] = 255;
    }
  GLFWimage img = {32, 32, px};
  glfwSetWindowIcon(w, 1, &img);
}
#endif

bool saveBmp(const std::string& path, int w, int h) {
  std::vector<unsigned char> px((size_t)w * h * 4);
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
  for (size_t i = 0; i < px.size(); i += 4) std::swap(px[i], px[i + 2]);
  FILE* f = fopen(path.c_str(), "wb");
  if (!f) return false;
  unsigned int size = (unsigned int)px.size();
  unsigned char hdr[54] = {'B', 'M'};
  auto put32 = [&](int at, unsigned int v) {
    for (int i = 0; i < 4; i++) hdr[at + i] = (unsigned char)(v >> (8 * i));
  };
  put32(2, 54 + size);
  put32(10, 54);
  put32(14, 40);
  put32(18, (unsigned int)w);
  put32(22, (unsigned int)h);
  hdr[26] = 1, hdr[28] = 32;
  put32(34, size);
  fwrite(hdr, 1, 54, f);
  fwrite(px.data(), 1, px.size(), f);
  fclose(f);
  return true;
}

// The colours of the band plan, the same in the map and on the scope.
ImU32 segmentColour(bands::Use use, int alpha) {
  ImU32 c = use == bands::U_CW       ? IM_COL32(50, 140, 225, 255)
            : use == bands::U_DIGI   ? IM_COL32(180, 80, 205, 255)
            : use == bands::U_SSB    ? IM_COL32(55, 165, 90, 255)
            : use == bands::U_FM     ? IM_COL32(225, 135, 40, 255)
            : use == bands::U_BEACON ? IM_COL32(205, 175, 40, 255)
            : use == bands::U_SAT    ? IM_COL32(40, 170, 175, 255)
            : use == bands::U_BROADCAST ? IM_COL32(190, 90, 60, 255)
            : use == bands::U_AIR    ? IM_COL32(70, 110, 215, 255)
            : use == bands::U_MARINE ? IM_COL32(30, 130, 150, 255)
            : use == bands::U_MOBILE ? IM_COL32(150, 130, 70, 255)
            : use == bands::U_FIXED  ? IM_COL32(95, 100, 115, 255)
            : use == bands::U_NAV    ? IM_COL32(140, 90, 180, 255)
                                     : IM_COL32(120, 125, 135, 255);
  return (c & 0x00FFFFFF) | ((ImU32)alpha << IM_COL32_A_SHIFT);
}
// What a portion of a band is for, in plain words. "Other" has no plain word, so it uses its own label.
std::string portionName(const bands::Segment& s) { return s.use == bands::U_OTHER ? s.label : bands::useName(s.use); }

class App {
 public:
  explicit App(const AppOptions& o) : opt_(o) {}
  int run();

 private:
  // ---- set-up and what happens every frame ----
  void setupStyle(float scale);
  void loadSettings();
  void saveSettings();
  void connect();
  void frame();
  void pumpAudio();
  void pumpScope();
  void keepAudioOpen();
  void rememberBand();
  void handleKeys();

  // ---- the window ----
  void drawMenu();
  void drawHeader();
  void drawVfo();
  void drawFreqDigits(long long hz, bool tx);
  void drawMeters();
  void drawNeedleMeter(ImVec2 org, float w, float h);
  void drawNeedleFace(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float reading);
  struct MeterRow {
    const char* label;
    float frac;
    ImU32 col;
    std::string val;
    float red;
  };
  std::array<MeterRow, 3> lowerMeters() const;
  void drawTxPanel();
  void drawReceiverPanel();
  void drawBandMenu();
  void drawReceiverModes(int filter);
  void drawBandRow();
  void drawControls();
  void drawCentre();
  void drawBandMap(double viewLo, double viewHi);
  void drawScopePane();
  void drawAudioPane();
  void drawRightTabs();
  void drawDial();
  void drawKeypad();
  void drawMemories();
  void drawLog();
  void drawCwVoice();
  void drawStatus();
  void drawSettings();
  void drawHelp();

  // helpers
  float S(float v) const { return v * scale_; }
  bool toggle(const char* label, bool on, ImVec2 size = ImVec2(0, 0), const char* name = nullptr);
  bool panelButton(const char* label, const ImVec2& size = ImVec2(0, 0), ImGuiButtonFlags flags = 0);
  bool actionButton(const char* label, ImVec2 size, const ImVec4& col, const ImVec4& hi, const char* name = nullptr);
  void levelRow(const char* label, int level, const char* fmtOverride = nullptr);
  void switchLevelRow(const char* label, int sw, int level);
  void radioRow(const char* label, int sw, const std::vector<std::pair<const char*, int>>& opts);
  void flow(float nextWidth);
  void note(const std::string& name);
  void say(const std::string& s) { status_ = s, statusUntil_ = glfwGetTime() + 8; }
  const civ::Radio& radio() const { return *st_.radio; }
  bool bandOnRig(const bands::Band& b) const { return radio().canTune(b.homeHz); }
  int nextBand(int here, int dir) const;
  std::string windowTitle() const;
  bool tuneTo(long long hz);  // false if this radio has no such frequency, and says so
  void tuneBy(long long d);
  void goBand(int i);
  void goMarker(const bands::Marker& m);
  void goRange(const bands::StationRange& r);
  void drawStationsMenu();
  void drawStationSearch();
  const bands::StationRange* rangeHere(long long hz) const { return showStations_ ? stations_.rangeAt(hz, showService_) : nullptr; }
  void choosePlan(const std::string& id);
  void reloadPlans();
  void loadBandMem();
  void useStations(bands::Stations loaded);
  void saveMemoryHere();
  void commitEntry();
  void setStep(int i) { stepIdx_ = std::max(0, std::min(kStepCount - 1, i)); }
  void startRecording();
  void startClockSync();
  void applyFft();
  void autoAudioLevels();
  void autoScopeLevels();

  struct PlotArea {
    ImVec2 p0, p1;     // the whole thing
    ImVec2 w0, w1;     // the waterfall part
    bool hovered = false, clicked = false, rclicked = false;
    double x = 0;      // the domain value under the mouse
    float wheel = 0;
  };
  PlotArea drawPlot(const char* id, double xLo, double xHi, const std::vector<float>& line, double lineLo, double lineHi, float yLo,
                    float yHi, Waterfall& wf, float u0, float u1, const std::function<std::string(double)>& axisLabel, double axisStep,
                    float rowHeight, float stripH = 0);
  // The flags along the top of the spectrum, which the plan's and the stations' share room between.
  struct FlagLayer {
    int rows = 1;
    std::vector<std::pair<float, float>> taken[3];
    const bands::Marker* hit = nullptr;  // under the mouse
    float bestDist = 0;
  };
  int freeFlagRow(const FlagLayer& l, float x0, float x1) const;
  void drawFlag(const PlotArea& a, double lo, double hi, float stripH, FlagLayer& l, const bands::Marker& m, ImU32 col, bool filled, const std::string& rectName);
  void drawPlanOverlay(const PlotArea& a, double lo, double hi, float stripH, FlagLayer& l);
  void drawStationOverlay(const PlotArea& a, double lo, double hi, float stripH, FlagLayer& l);

  // ---- self-test ----
  struct Step {
    int waitFrames;
    std::function<void()> fn;
  };
  void buildSelfTest();
  void runSelfTest();
  void click(const std::string& name, float fx = 0.5f, float fy = 0.5f, int button = 0);
  void typeText(const std::string& s);
  void pressKey(ImGuiKey k);
  void expect(const std::string& what, bool ok);

  AppOptions opt_;
  GLFWwindow* win_ = nullptr;
  float scale_ = 1;
  ImFont* fText_ = nullptr;
  ImFont* fSmall_ = nullptr;
  ImFont* fMid_ = nullptr;
  ImFont* fFreq_ = nullptr;
  ImFont* fFreqSmall_ = nullptr;
  uint64_t frameCount_ = 0;

  Rig rig_;
  std::unique_ptr<SimRig> sim_;
  RigState st_;
  AudioEngine audio_;
  DemoAudio demoAudio_;

  store::Settings cfg_;
  std::string cfgPath_, memPath_, logPath_;

  // connection
  std::string port_;
  int baud_ = 115200, addr_ = 0x94;
  int radioChoice_ = 0;  // Settings: 0 finds out, 1 is a 7300, 2 a 9700, 3 an R8600
  const civ::Radio* seenRadio_ = &civ::kRadio7300;  // the last one to answer
  std::string audioIn_, audioOut_;
  std::string audioError_;
  double audioRetryAt_ = 0;

  // tuning
  int stepIdx_ = 2;
  tuning::Dial dial_;
  std::string entry_;
  double entryErrorUntil_ = 0;
  struct BandMem {
    bool valid = false;
    long long hz = 0;
    int mode = 0, filter = 1;
    bool data = false;
  };
  std::map<std::string, BandMem> bandMem_;  // by band name, as plans differ in which bands they have
  bands::Library plans_;
  bands::Plan plan_;     // the chosen plan, as it applies to the radio that's connected
  const civ::Radio* planRadio_ = nullptr;  // which that is
  std::string plansDir_;
  bool showBandPlan_ = true;
  bands::Stations allStations_;  // the same whichever plan is chosen
  bands::Stations stations_;     // those of them for the radio that's connected
  std::string stationsDir_;
  bool showStations_ = true;
  bool onAirOnly_ = true;  // a broadcaster gets a flag only while it's scheduled
  bool showService_[bands::S_COUNT] = {true, true, true, true, true, true, true, true, true};
  bool stationSearchOpen_ = false, stationSearchFocus_ = false;
  char stationQuery_[64] = "";
  // Kept from frame to frame: there are thousands of stations to look through. They point into stations_.
  std::vector<bands::StationHit> stationHits_;
  std::string stationHitsFor_;
  bool stationHitsStale_ = true;
  size_t stationHitCount_ = 0;
  int smeterPeak_ = 0;
  bool needleMeter_ = false;  // the top meter is a needle rather than bars: a click swaps them
  needle::Needle needle_;
  float meterLamp_ = 0;       // how bright the needle meter's backlight is, 0 to 1

  // audio view
  std::unique_ptr<dsp::Spectrum> spectrum_;
  int fftIdx_ = 2, audioRangeIdx_ = 0, audioLinesPerSec_ = 25;
  float audioFloorDb_ = -110, audioRangeDb_ = 60;
  std::vector<float> audioBuf_, fftIn_, audioDb_, audioLine_;
  size_t audioPos_ = 0;
  int audioAutoLeft_ = 0, scopeAutoLeft_ = 0;
  std::vector<uint8_t> rowBuf_;
  Waterfall wfAudio_;
  bool monitor_ = false;
  float monitorGain_ = 0.7f;
  float peakDb_ = -100, peakHold_ = 0;
  store::WavWriter rec_;

  // scope view
  Waterfall wfScope_;
  std::vector<float> scopeLine_;
  long long scopeLo_ = 0, scopeHi_ = 0;
  bool scopeOor_ = false;
  long long scopeMenuHz_ = 0;
  double scopeLastAt_ = 0;
  bool scopeEnabled_ = true;
  float scopeFloor_ = 12, scopeRange_ = 110;
  int palette_ = PAL_CLASSIC;
  bool showScope_ = true, showAudio_ = true;
  float splitFrac_ = 0.5f;

  // tabs
  std::vector<store::Memory> memories_;
  char memName_[64] = "", memFilter_[64] = "";
  std::vector<store::LogEntry> log_;
  char logCall_[24] = "", logSent_[8] = "59", logRcvd_[8] = "59", logName_[32] = "", logNotes_[128] = "";
  bool logFocusCall_ = false;
  char callsign_[24] = "";
  char macros_[5][96] = {"CQ CQ CQ DE {CALL} {CALL} K", "{CALL}", "TNX FB QSO 73 DE {CALL} SK", "UR 599 599 BK", "QRZ? DE {CALL} K"};
  char cwLine_[128] = "";

  // settings and dialogs
  bool openSettings_ = false, openHelp_ = false, openPowerOff_ = false;
  std::vector<SerialPortInfo> ports_;
  std::vector<std::string> inDevs_, outDevs_;
  char addrText_[8] = "94";
  int txLimit_ = 180;

  std::string status_;
  double statusUntil_ = 0;
  uint64_t lastErrorSeq_ = 0;

  // setting the computer's clock. It can wait on the network or a permission prompt for as long as it likes,
  // so its thread owns its half of this and is left to finish alone if the window closes first.
  struct ClockSync {
    std::atomic<bool> done{false};
    clocksync::Report report;
  };
  std::shared_ptr<ClockSync> clockSync_;
  std::string clockResult_;

  // self-test
  std::map<std::string, ImRect> rects_;
  std::vector<Step> steps_;
  size_t stepAt_ = 0;
  int stepWait_ = 0;
  double stepUntil_ = 0;
  std::vector<std::string> results_;
  int testFails_ = 0;
  bool testDone_ = false;
  // injected input, applied one piece a frame so ImGui sees press then release
  std::vector<std::function<void(ImGuiIO&)>> inject_;
};

// ---------------------------------------------------------------- set-up

void App::setupStyle(float scale) {
  ImGuiStyle& s = ImGui::GetStyle();
  ImGui::StyleColorsDark();
  s.WindowRounding = 0, s.ChildRounding = 4, s.FrameRounding = 3, s.GrabRounding = 3, s.PopupRounding = 4, s.TabRounding = 3;
  s.WindowBorderSize = 0, s.ChildBorderSize = 1, s.FrameBorderSize = 0;
  s.WindowPadding = ImVec2(8, 6), s.FramePadding = ImVec2(6, 3), s.ItemSpacing = ImVec2(5, 4), s.ItemInnerSpacing = ImVec2(4, 3);
  s.GrabMinSize = 9, s.ScrollbarSize = 11;
  ImVec4* c = s.Colors;
  c[ImGuiCol_WindowBg] = ImGui::ColorConvertU32ToFloat4(C_BG);
  c[ImGuiCol_ChildBg] = ImGui::ColorConvertU32ToFloat4(C_PANEL);
  c[ImGuiCol_PopupBg] = ImVec4(0.11f, 0.13f, 0.16f, 0.98f);
  c[ImGuiCol_Border] = ImVec4(0.21f, 0.25f, 0.30f, 1);
  c[ImGuiCol_FrameBg] = ImVec4(0.14f, 0.17f, 0.21f, 1);
  c[ImGuiCol_FrameBgHovered] = ImVec4(0.19f, 0.23f, 0.28f, 1);
  c[ImGuiCol_FrameBgActive] = ImVec4(0.21f, 0.26f, 0.32f, 1);
  c[ImGuiCol_Button] = ImVec4(0.13f, 0.14f, 0.16f, 1);
  c[ImGuiCol_ButtonHovered] = ImVec4(0.21f, 0.24f, 0.28f, 1);
  c[ImGuiCol_ButtonActive] = V_ACCENT;
  c[ImGuiCol_SliderGrab] = V_ACCENT_HI;
  c[ImGuiCol_SliderGrabActive] = ImVec4(0.3f, 0.75f, 0.9f, 1);
  c[ImGuiCol_CheckMark] = ImVec4(0.3f, 0.75f, 0.9f, 1);
  c[ImGuiCol_Header] = ImVec4(0.17f, 0.20f, 0.25f, 1);
  c[ImGuiCol_HeaderHovered] = ImVec4(0.22f, 0.27f, 0.33f, 1);
  c[ImGuiCol_HeaderActive] = V_ACCENT;
  c[ImGuiCol_Tab] = ImVec4(0.14f, 0.17f, 0.21f, 1);
  c[ImGuiCol_TabHovered] = V_ACCENT_HI;
  c[ImGuiCol_TabSelected] = V_ACCENT;
  c[ImGuiCol_MenuBarBg] = ImVec4(0.09f, 0.11f, 0.14f, 1);
  c[ImGuiCol_TitleBgActive] = V_ACCENT;
  c[ImGuiCol_Text] = ImGui::ColorConvertU32ToFloat4(C_TEXT);
  c[ImGuiCol_TextDisabled] = ImGui::ColorConvertU32ToFloat4(C_DIM);
  s.ScaleAllSizes(scale);

  ImGuiIO& io = ImGui::GetIO();
  ImFontConfig cfg;
  cfg.FontDataOwnedByAtlas = false;
  cfg.OversampleH = 2;
  void* roboto = const_cast<unsigned char*>(kRobotoMedium);
  void* cousine = const_cast<unsigned char*>(kCousine);
  fText_ = io.Fonts->AddFontFromMemoryTTF(roboto, sizeof kRobotoMedium, 15.f * scale, &cfg);
  fSmall_ = io.Fonts->AddFontFromMemoryTTF(roboto, sizeof kRobotoMedium, 12.5f * scale, &cfg);
  fMid_ = io.Fonts->AddFontFromMemoryTTF(roboto, sizeof kRobotoMedium, 19.f * scale, &cfg);
  fFreq_ = io.Fonts->AddFontFromMemoryTTF(cousine, sizeof kCousine, 56.f * scale, &cfg);
  fFreqSmall_ = io.Fonts->AddFontFromMemoryTTF(cousine, sizeof kCousine, 21.f * scale, &cfg);
}

void App::loadSettings() {
  std::string dir = store::dataDir();
  // The demo keeps to its own files, so trying it can't disturb a real set-up.
  std::string prefix = opt_.demo ? "demo-" : "";
  cfgPath_ = dir + prefix + "settings.ini";
  memPath_ = dir + prefix + "memories.csv";
  logPath_ = dir + prefix + "log.csv";
  cfg_.load(cfgPath_);
  port_ = cfg_.get("port");
  baud_ = cfg_.getInt("baud", 115200);
  addr_ = cfg_.getInt("addr", 0x94);
  snprintf(addrText_, sizeof addrText_, "%02X", addr_);
  radioChoice_ = std::max(0, std::min(3, cfg_.getInt("radio", 0)));
  if (const civ::Radio* r = civ::radioForId((uint8_t)cfg_.getInt("last_radio", civ::kIc7300))) seenRadio_ = r;
  audioIn_ = cfg_.get("audio_in");
  audioOut_ = cfg_.get("audio_out");
  monitor_ = cfg_.getInt("monitor", 0) != 0;
  monitorGain_ = (float)cfg_.getDouble("monitor_gain", 0.7);
  stepIdx_ = std::max(0, std::min(kStepCount - 1, cfg_.getInt("step", 2)));
  fftIdx_ = std::max(0, std::min(3, cfg_.getInt("fft", 2)));
  audioRangeIdx_ = std::max(0, std::min(4, cfg_.getInt("audio_range", 0)));
  audioLinesPerSec_ = std::max(5, std::min(60, cfg_.getInt("audio_lines", 25)));
  audioFloorDb_ = (float)cfg_.getDouble("audio_floor", -110);
  audioRangeDb_ = (float)cfg_.getDouble("audio_span", 60);
  audioAutoLeft_ = cfg_.get("audio_floor").empty() ? 50 : 0;
  scopeAutoLeft_ = cfg_.get("scope_floor").empty() ? 12 : 0;
  scopeFloor_ = (float)cfg_.getDouble("scope_floor", 12);
  scopeRange_ = (float)cfg_.getDouble("scope_span", 110);
  palette_ = std::max(0, std::min(PAL_COUNT - 1, cfg_.getInt("palette", 0)));
  scopeEnabled_ = cfg_.getInt("scope", 1) != 0;
  showScope_ = cfg_.getInt("show_scope", 1) != 0;
  showAudio_ = cfg_.getInt("show_audio", 1) != 0;
  splitFrac_ = (float)cfg_.getDouble("split", 0.5);
  txLimit_ = std::max(5, cfg_.getInt("tx_limit", 180));
  snprintf(callsign_, sizeof callsign_, "%s", cfg_.get("callsign").c_str());
  for (int i = 0; i < 5; i++) {
    std::string m = cfg_.get("macro" + std::to_string(i));
    if (!m.empty()) snprintf(macros_[i], sizeof macros_[i], "%s", m.c_str());
  }
  // The self-test uses only what's built in, so it can't depend on or disturb anyone's edited plans.
  plansDir_ = dir + "bandplans";
  stationsDir_ = dir + "stations";
  if (!opt_.selfTest.empty()) {
    plans_ = bands::builtinPlans();
    useStations(bands::builtinStations());
  } else {
    bands::installPlans(plansDir_);
    plans_ = bands::loadPlans(plansDir_);
    bands::installStations(stationsDir_);
    useStations(bands::loadStations(stationsDir_));
    showStations_ = cfg_.getInt("show_stations", 1) != 0;
    onAirOnly_ = cfg_.getInt("stations_on_air_only", 1) != 0;
    for (int i = 0; i < bands::S_COUNT; i++) showService_[i] = cfg_.getInt("show_service" + std::to_string(i), 1) != 0;
  }
  needleMeter_ = opt_.needle || cfg_.getInt("needle_meter", 0) != 0;
  showBandPlan_ = !opt_.selfTest.empty() || cfg_.getInt("show_bandplan", 1) != 0;
  choosePlan(opt_.selfTest.empty() ? cfg_.get("band_plan", "uk") : "uk");
  memories_ = store::loadMemories(memPath_);
  log_ = store::loadLog(logPath_);
}

// Saved by band name. Earlier versions saved by position in a fixed list of twelve, which these read.
void App::loadBandMem() {
  static const char* const kOldOrder[] = {"160", "80", "60", "40", "30", "20", "17", "15", "12", "10", "6", "4"};
  for (const bands::Plan& plan : plans_.plans)
    for (const bands::Band& band : plan.bands) {
      if (bandMem_.count(band.name)) continue;
      std::string v = cfg_.get("band_" + band.name);
      for (int i = 0; i < 12 && v.empty(); i++)
        if (band.name == kOldOrder[i]) v = cfg_.get("band" + std::to_string(i));
      BandMem b;
      auto p = store::splitCsv(v);
      if (p.size() == 4) {
        b.hz = atoll(p[0].c_str()), b.mode = atoi(p[1].c_str()), b.data = p[2] == "1", b.filter = atoi(p[3].c_str());
        b.valid = b.hz > 0;
      }
      bandMem_[band.name] = b;
    }
}

void App::choosePlan(const std::string& id) {
  const bands::Plan* p = plans_.find(id);
  if (!p) p = plans_.find("uk");
  if (!p && !plans_.plans.empty()) p = &plans_.plans[0];
  if (p) plan_ = p->forRadio(!radio().hasTransmit);
  loadBandMem();
}

void App::useStations(bands::Stations loaded) {
  allStations_ = std::move(loaded);
  stations_ = allStations_.forRadio(!radio().hasTransmit);
  stationHitsStale_ = true;
}

void App::reloadPlans() {
  bands::installPlans(plansDir_);
  plans_ = bands::loadPlans(plansDir_);
  choosePlan(plan_.id);
  bands::installStations(stationsDir_);
  useStations(bands::loadStations(stationsDir_));
  say(!plans_.errors.empty()      ? "A band plan has a problem: " + plans_.errors[0]
      : !stations_.errors.empty() ? "A station list has a problem: " + stations_.errors[0]
                                  : "Band plans and stations reloaded");
}

void App::saveSettings() {
  cfg_.set("port", port_);
  cfg_.setInt("baud", baud_);
  cfg_.setInt("addr", addr_);
  cfg_.setInt("radio", radioChoice_);
  cfg_.setInt("last_radio", seenRadio_->addr);
  cfg_.set("audio_in", audioIn_);
  cfg_.set("audio_out", audioOut_);
  cfg_.setInt("monitor", monitor_);
  cfg_.setDouble("monitor_gain", monitorGain_);
  cfg_.setInt("step", stepIdx_);
  cfg_.setInt("fft", fftIdx_);
  cfg_.setInt("audio_range", audioRangeIdx_);
  cfg_.setInt("audio_lines", audioLinesPerSec_);
  cfg_.setDouble("audio_floor", audioFloorDb_);
  cfg_.setDouble("audio_span", audioRangeDb_);
  cfg_.setDouble("scope_floor", scopeFloor_);
  cfg_.setDouble("scope_span", scopeRange_);
  cfg_.setInt("palette", palette_);
  cfg_.setInt("needle_meter", needleMeter_);
  cfg_.setInt("scope", scopeEnabled_);
  cfg_.setInt("show_scope", showScope_);
  cfg_.setInt("show_audio", showAudio_);
  cfg_.setDouble("split", splitFrac_);
  cfg_.setInt("tx_limit", txLimit_);
  cfg_.set("callsign", callsign_);
  for (int i = 0; i < 5; i++) cfg_.set("macro" + std::to_string(i), macros_[i]);
  cfg_.set("band_plan", plan_.id);
  cfg_.setInt("show_bandplan", showBandPlan_);
  cfg_.setInt("show_stations", showStations_);
  cfg_.setInt("stations_on_air_only", onAirOnly_);
  for (int i = 0; i < bands::S_COUNT; i++) cfg_.setInt("show_service" + std::to_string(i), showService_[i]);
  for (const auto& e : bandMem_) {
    const BandMem& b = e.second;
    if (b.valid)
      cfg_.set("band_" + e.first, store::joinCsv({std::to_string(b.hz), std::to_string(b.mode), b.data ? "1" : "0", std::to_string(b.filter)}));
  }
  int w, h;
  glfwGetWindowSize(win_, &w, &h);
  cfg_.setInt("win_w", w);
  cfg_.setInt("win_h", h);
  cfg_.save(cfgPath_);
}

void App::connect() {
  rig_.setScope(scopeEnabled_);
  rig_.setTxLimit(txLimit_);
  if (opt_.demo) {
    rig_.start([this] { return sim_->newTransport(); });
    return;
  }
  if (opt_.noRig) return;
  if (!opt_.port.empty()) port_ = opt_.port;
  if (port_.empty()) port_ = guessRigPort(listSerialPorts());
  std::string port = port_;
  int baud = baud_;
  const civ::Radio* forced = radioChoice_ == 1 ? &civ::kRadio7300 : radioChoice_ == 2 ? &civ::kRadio9700 : radioChoice_ == 3 ? &civ::kRadioR8600 : nullptr;
  rig_.start([port, baud] { return newSerialTransport(port, baud); }, (uint8_t)addr_, forced, seenRadio_);
}

std::string App::windowTitle() const {
  // The name covers all the radios now, so only the brackets say which this is.
  if (opt_.demo) return std::string(APP_NAME "  [DEMO: simulated ") + seenRadio_->name + "]";
  return std::string(APP_NAME "  [") + seenRadio_->name + "]";
}

void App::applyFft() {
  int n = kFftSizes[fftIdx_];
  spectrum_.reset(new dsp::Spectrum(n));
  fftIn_.assign((size_t)n, 0.f);
  audioDb_.assign((size_t)spectrum_->bins(), -140.f);
  audioLine_.assign((size_t)spectrum_->bins(), -140.f);
  rowBuf_.assign((size_t)spectrum_->bins(), 0);
  wfAudio_.init(spectrum_->bins(), kWaterfallRows);
}

void App::keepAudioOpen() {
  if (opt_.demo) {
    if (!audio_.capturing()) {
      DemoAudio* d = &demoAudio_;
      audio_.startGenerated([d](float* o, int n) { d->generate(o, n); });
    }
    return;
  }
  double now = glfwGetTime();
  if (audio_.capturing() && !audio_.lost()) return;
  if (now < audioRetryAt_) return;
  audioRetryAt_ = now + 3;
  if (audio_.capturing()) audio_.stopCapture();
  std::string in = audioIn_.empty() ? audio_.guessRigInput() : audioIn_;
  if (in.empty()) {
    audioError_ = "The rig's USB audio isn't there (radio off?). Choose a device in Settings.";
    return;
  }
  std::string err;
  if (audio_.startCapture(in, err)) audioError_.clear();
  else audioError_ = err;
}

// ---------------------------------------------------------------- what happens every frame

void App::pumpAudio() {
  if (!spectrum_) return;
  float tmp[8192];
  size_t got;
  while ((got = audio_.read(tmp, 8192)) > 0) {
    audioBuf_.insert(audioBuf_.end(), tmp, tmp + got);
    if (rec_.isOpen()) rec_.write(tmp, got);
  }
  float pk = audio_.takePeak();
  float db = pk > 1e-6f ? 20.f * log10f(pk) : -120.f;
  peakDb_ = std::max(db, peakDb_ - 0.8f);
  if (db > -1.f) peakHold_ = 2.f;
  peakHold_ = std::max(0.f, peakHold_ - ImGui::GetIO().DeltaTime);

  int n = spectrum_->size();
  int hop = std::max(64, AudioEngine::kRate / std::max(1, audioLinesPerSec_));
  // Falling more than a second behind (a minimised window, say): start again from now.
  if (audioBuf_.size() - audioPos_ > (size_t)AudioEngine::kRate + (size_t)n) audioPos_ = audioBuf_.size() - (size_t)n;
  size_t need = (size_t)std::max(n, hop);
  while (audioBuf_.size() - audioPos_ >= need) {
    spectrum_->compute(&audioBuf_[audioPos_], audioDb_.data());
    audioPos_ += (size_t)hop;
    const int bins = spectrum_->bins();
    for (int i = 0; i < bins; i++) {
      audioLine_[(size_t)i] += 0.4f * (audioDb_[(size_t)i] - audioLine_[(size_t)i]);
      rowBuf_[(size_t)i] = dsp::dbToLevel(audioDb_[(size_t)i]);
    }
    wfAudio_.pushRow(rowBuf_.data(), bins);
    // The first run has no saved levels: take them from what's actually coming in, once it's settled.
    if (audioAutoLeft_ > 0 && --audioAutoLeft_ == 0) autoAudioLevels();
  }
  if (audioPos_ > 1u << 18) {
    audioBuf_.erase(audioBuf_.begin(), audioBuf_.begin() + (long)audioPos_);
    audioPos_ = 0;
  }
}

void App::pumpScope() {
  ScopeLine l;
  std::vector<uint8_t> row;
  while (rig_.popScope(l)) {
    long long oldSpan = scopeHi_ - scopeLo_, span = l.hiHz - l.loHz;
    long long moved = llabs((l.loHz + l.hiHz) / 2 - (scopeLo_ + scopeHi_) / 2);
    // History from another span, or from far away, would only mislead.
    if (span != oldSpan || (span > 0 && moved > span / 4)) wfScope_.clear();
    scopeLo_ = l.loHz, scopeHi_ = l.hiHz, scopeOor_ = l.outOfRange;
    scopeLastAt_ = glfwGetTime();
    scopeLine_.resize(l.levels.size());
    row.resize(l.levels.size());
    for (size_t i = 0; i < l.levels.size(); i++) {
      scopeLine_[i] = l.levels[i];
      row[i] = (uint8_t)std::min(255, (int)l.levels[i] * 255 / civ::kScopeLevelMax);
    }
    wfScope_.pushRow(row.data(), (int)row.size());
    if (scopeAutoLeft_ > 0 && --scopeAutoLeft_ == 0) autoScopeLevels();
  }
}

void App::rememberBand() {
  if (!st_.connected || st_.tx || st_.hz <= 0 || st_.mode < 0) return;
  int b = plan_.bandOf(st_.hz);
  if (b < 0 || !plan_.segmentAt(st_.hz)) return;
  BandMem& m = bandMem_[plan_.bands[(size_t)b].name];
  m.valid = true, m.hz = st_.hz, m.mode = st_.mode, m.data = st_.data, m.filter = st_.filter ? st_.filter : 1;
}

bool App::tuneTo(long long hz) {
  // A click just past an edge means the edge. Further off, it's a frequency this radio doesn't have, such as
  // an HF memory on a 9700, and going to whichever band happens to be closest would only surprise.
  if (llabs(radio().clamp(hz) - hz) > 1000000) {
    say(tuning::formatHz(hz) + " isn't on the " + radio().name + ", which tunes " + radio().rangeText);
    return false;
  }
  rig_.setFreq(hz);
  return true;
}

void App::tuneBy(long long d) {
  const civ::HzRange& r = radio().nearest(st_.hz);
  rig_.setFreq(std::min(r.hi, std::max(r.lo, st_.hz + d)));
}

// The next band up or down that this radio has, going round. -1 if the plan has none for it.
int App::nextBand(int here, int dir) const {
  int n = (int)plan_.bands.size();
  for (int k = 1; k <= n; k++) {
    int i = here < 0 ? (dir > 0 ? k - 1 : n - k) : ((here + dir * k) % n + n) % n;
    if (bandOnRig(plan_.bands[(size_t)i])) return i;
  }
  return -1;
}

void App::goBand(int i) {
  const bands::Band& band = plan_.bands[(size_t)i];
  const BandMem& m = bandMem_[band.name];
  // Where you were may not be in the band under a different plan.
  if (m.valid && band.segmentAt(m.hz)) {
    tuneTo(m.hz);
    rig_.setMode(m.mode, m.data, m.filter);
  } else {
    tuneTo(band.homeHz);
    rig_.setMode(band.homeMode, false, st_.filter ? st_.filter : 1);
  }
}

void App::goMarker(const bands::Marker& m) {
  if (tuneTo(m.tuneHz()) && m.preset()) rig_.setMode(m.mode, m.data, 1);
}

// The middle of a broadcast band is as good a place as any to start listening.
void App::goRange(const bands::StationRange& r) {
  if (tuneTo((r.lo + r.hi) / 2) && (r.mode >= 0 || r.service == bands::S_BROADCAST)) rig_.setMode(r.mode >= 0 ? r.mode : (int)civ::AM, false, 1);
}

void App::commitEntry() {
  long long hz = tuning::parseFreq(entry_, st_.hz, radio());
  if (hz < 0) {
    say("\"" + entry_ + "\" isn't a frequency the " + radio().name + " can tune (" + radio().rangeText + ")");
    entryErrorUntil_ = glfwGetTime() + 1.5;
    return;
  }
  tuneTo(hz);
  entry_.clear();
}

void App::handleKeys() {
  ImGuiIO& io = ImGui::GetIO();
  if (io.WantTextInput) return;
  if (ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) return;

  // Typing a frequency works with nothing focused: just start typing.
  for (int i = 0; i < io.InputQueueCharacters.Size; i++) {
    ImWchar c = io.InputQueueCharacters[i];
    bool digit = c >= '0' && c <= '9';
    bool leading = entry_.empty() && (c == '+' || c == '-');
    bool punct = !entry_.empty() && (c == '.' || c == ',' || c == 'k' || c == 'K' || c == 'm' || c == 'M');
    bool dotFirst = entry_.empty() && c == '.';
    if ((digit || leading || punct || dotFirst) && entry_.size() < 16) entry_ += (char)(c == ',' ? '.' : c);
  }
  if (!entry_.empty()) {
    if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) commitEntry();
    else if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) entry_.clear();
    else if (ImGui::IsKeyPressed(ImGuiKey_Backspace, true)) entry_.pop_back();
    return;  // the arrows and F-keys mustn't act behind a half-typed frequency
  }

  bool ctrl = io.KeyCtrl, shift = io.KeyShift;
  if (ctrl && ImGui::IsKeyPressed(ImGuiKey_F, false)) stationSearchOpen_ = stationSearchFocus_ = true;
  if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
    if (!radio().hasTransmit) say(std::string("The ") + radio().name + " is a receiver: it can't transmit.");
    else if (!st_.txArmed) say("Transmit isn't armed. Press ARM TX first.");
    else rig_.ptt(!st_.tx);
  }
  if (!st_.connected) return;
  long long step = kSteps[stepIdx_] * (shift ? 10 : 1);
  if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, true)) tuneTo(tuning::stepFreq(st_.hz, step, 1, radio()));
  if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true)) tuneTo(tuning::stepFreq(st_.hz, step, -1, radio()));
  if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, false)) setStep(stepIdx_ + 1);
  if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, false)) setStep(stepIdx_ - 1);
  int here = plan_.bandOf(st_.hz);
  for (int dir : {1, -1})
    if (ImGui::IsKeyPressed(dir > 0 ? ImGuiKey_PageUp : ImGuiKey_PageDown, false)) {
      int to = nextBand(here, dir);
      if (to >= 0) goBand(to);
    }
  int f = st_.filter ? st_.filter : 1;
  static const struct {
    ImGuiKey key;
    int mode;
  } modeKeys[] = {{ImGuiKey_F1, civ::LSB}, {ImGuiKey_F2, civ::USB}, {ImGuiKey_F3, civ::CW}, {ImGuiKey_F4, civ::CW_R},
                  {ImGuiKey_F5, civ::AM}, {ImGuiKey_F6, civ::FM}, {ImGuiKey_F7, civ::RTTY}, {ImGuiKey_F8, civ::RTTY_R}};
  for (auto& k : modeKeys)
    if (ImGui::IsKeyPressed(k.key, false)) rig_.setMode(k.mode, false, f);
  if (ImGui::IsKeyPressed(ImGuiKey_F9, false) && radio().hasDataMode && civ::hasData(st_.mode)) rig_.setMode(st_.mode, !st_.data, f);
  if (ImGui::IsKeyPressed(ImGuiKey_F10, false) && !(radio().hasReceiverModes && civ::hasNoFilters(st_.mode))) rig_.setMode(st_.mode, st_.data, f % 3 + 1);
  if (ImGui::IsKeyPressed(ImGuiKey_F11, false)) rig_.setSplit(st_.split != 1);
  if (ImGui::IsKeyPressed(ImGuiKey_F12, false)) rig_.vfoSwap();
}

// ---------------------------------------------------------------- small widgets

void App::note(const std::string& name) {
  if (!opt_.selfTest.empty()) rects_[name] = ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
}

static ImVec4 mixColour(const ImVec4& c, float to, float t) {
  return ImVec4(c.x + (to - c.x) * t, c.y + (to - c.y) * t, c.z + (to - c.z) * t, c.w);
}

// A rounded rectangle shaded from top to bottom. ImGui's own gradient rectangle can't round its corners.
static void gradientRect(ImDrawList* dl, ImVec2 a, ImVec2 b, const ImVec4& top, const ImVec4& bottom, float rounding, ImDrawFlags corners = 0) {
  int start = dl->VtxBuffer.Size;
  // The shading replaces the colour but keeps this alpha, which is how a disabled button still fades.
  dl->AddRectFilled(a, b, ImGui::GetColorU32(ImVec4(1, 1, 1, top.w)), rounding, corners);
  ImGui::ShadeVertsLinearColorGradientKeepAlpha(dl, start, dl->VtxBuffer.Size, a, ImVec2(a.x, b.y), ImGui::ColorConvertFloat4ToU32(top), ImGui::ColorConvertFloat4ToU32(bottom));
}

// ImGui::Button's behaviour with a rig's front-panel look: a bevelled rim round a glossy face, the shine stopping
// sharply halfway down. The colour still comes from the style, so toggles and red and amber buttons keep theirs.
bool App::panelButton(const char* label, const ImVec2& sizeArg, ImGuiButtonFlags flags) {
  ImGuiWindow* window = ImGui::GetCurrentWindow();
  if (window->SkipItems) return false;
  const ImGuiStyle& style = ImGui::GetStyle();
  ImGuiID id = window->GetID(label);
  ImVec2 textSize = ImGui::CalcTextSize(label, nullptr, true);
  ImVec2 pos = window->DC.CursorPos;
  if ((flags & ImGuiButtonFlags_AlignTextBaseLine) && style.FramePadding.y < window->DC.CurrLineTextBaseOffset)
    pos.y += window->DC.CurrLineTextBaseOffset - style.FramePadding.y;
  ImVec2 size = ImGui::CalcItemSize(sizeArg, textSize.x + style.FramePadding.x * 2, textSize.y + style.FramePadding.y * 2);
  ImRect bb(pos, pos + size);
  ImGui::ItemSize(size, style.FramePadding.y);
  if (!ImGui::ItemAdd(bb, id)) return false;
  bool hovered, held;
  bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held, flags);
  bool down = held && hovered;
  ImGui::RenderNavCursor(bb, id);

  ImVec4 c = style.Colors[down ? ImGuiCol_ButtonActive : hovered ? ImGuiCol_ButtonHovered : ImGuiCol_Button];
  ImDrawList* dl = window->DrawList;
  // Whole pixels, or the rim and the line across the face go soft.
  ImVec2 a = ImFloor(bb.Min), b = ImFloor(bb.Max);
  float rim = std::max(1.f, (float)(int)S(1.25f));
  float r = style.FrameRounding + rim;
  ImVec2 inset(rim, rim);
  dl->AddRectFilled(a, b, ImGui::GetColorU32(ImVec4(0, 0, 0, 0.55f)), r);
  ImVec2 ra = a + ImVec2(1, 1), rb = b - ImVec2(1, 1);
  ImVec2 fa = ra + inset, fb = rb - inset;
  float mid = (float)(int)((fa.y + fb.y) / 2);
  float faceR = style.FrameRounding;
  if (down) {
    // Pushed in: the light now catches the lower edge of the rim, and the shine has gone.
    gradientRect(dl, ra, rb, mixColour(c, 0, 0.45f), mixColour(c, 1, 0.30f), r - 1);
    gradientRect(dl, fa, fb, mixColour(c, 0, 0.50f), mixColour(c, 0, 0.10f), faceR);
  } else {
    gradientRect(dl, ra, rb, mixColour(c, 1, 0.50f), mixColour(c, 1, 0.12f), r - 1);
    gradientRect(dl, fa, ImVec2(fb.x, mid), mixColour(c, 1, 0.30f), mixColour(c, 1, 0.09f), faceR, ImDrawFlags_RoundCornersTop);
    gradientRect(dl, ImVec2(fa.x, mid), fb, mixColour(c, 0, 0.50f), mixColour(c, 0, 0.08f), faceR, ImDrawFlags_RoundCornersBottom);
  }

  ImVec2 push = down ? ImVec2(0, std::max(1.f, (float)(int)S(1))) : ImVec2(0, 0);
  ImVec2 shadow(0, std::max(1.f, (float)(int)S(1)));
  ImVec2 ta = bb.Min + style.FramePadding + push, tb = bb.Max - style.FramePadding + push;
  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0, 0, 0, 0.8f));
  ImGui::RenderTextClipped(ta + shadow, tb + shadow, label, nullptr, &textSize, style.ButtonTextAlign, &bb);
  ImGui::PopStyleColor();
  ImGui::RenderTextClipped(ta, tb, label, nullptr, &textSize, style.ButtonTextAlign, &bb);
  return pressed;
}

bool App::toggle(const char* label, bool on, ImVec2 size, const char* name) {
  if (on) {
    ImGui::PushStyleColor(ImGuiCol_Button, V_ACCENT);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, V_ACCENT_HI);
  }
  bool r = panelButton(label, size);
  if (on) ImGui::PopStyleColor(2);
  note(name ? name : label);
  return r;
}

bool App::actionButton(const char* label, ImVec2 size, const ImVec4& col, const ImVec4& hi, const char* name) {
  ImGui::PushStyleColor(ImGuiCol_Button, col);
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hi);
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, hi);
  bool r = panelButton(label, size);
  ImGui::PopStyleColor(3);
  note(name ? name : label);
  return r;
}

void App::flow(float nextWidth) {
  float right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
  ImGui::SameLine();
  if (ImGui::GetCursorScreenPos().x + nextWidth > right) ImGui::NewLine();
}

void App::levelRow(const char* label, int level, const char* fmtOverride) {
  ImGui::PushID(label);
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(label);
  ImGui::SameLine(S(84));
  int v = std::max(0, st_.level[level]);
  char fmt[32];
  if (fmtOverride) snprintf(fmt, sizeof fmt, "%s", fmtOverride);
  else snprintf(fmt, sizeof fmt, "%d%%%%", (int)lround(v * 100.0 / 255.0));
  ImGui::SetNextItemWidth(-1);
  if (ImGui::SliderInt("##v", &v, 0, 255, fmt)) rig_.setLevel(level, v);
  note(std::string("level:") + label);
  if (ImGui::IsItemHovered() && ImGui::GetIO().MouseWheel != 0.f)
    rig_.setLevel(level, std::max(0, std::min(255, v + (int)ImGui::GetIO().MouseWheel * 5)));
  ImGui::PopID();
}

void App::switchLevelRow(const char* label, int sw, int level) {
  ImGui::PushID(label);
  bool on = st_.sw[sw] > 0;
  if (toggle(label, on, ImVec2(S(79), 0), (std::string("sw:") + label).c_str())) rig_.setSwitch(sw, on ? 0 : 1);
  ImGui::SameLine(S(84));
  int v = std::max(0, st_.level[level]);
  char fmt[32];
  snprintf(fmt, sizeof fmt, "%d%%%%", (int)lround(v * 100.0 / 255.0));
  ImGui::SetNextItemWidth(-1);
  if (ImGui::SliderInt("##v", &v, 0, 255, fmt)) rig_.setLevel(level, v);
  if (ImGui::IsItemHovered() && ImGui::GetIO().MouseWheel != 0.f)
    rig_.setLevel(level, std::max(0, std::min(255, v + (int)ImGui::GetIO().MouseWheel * 5)));
  ImGui::PopID();
}

void App::radioRow(const char* label, int sw, const std::vector<std::pair<const char*, int>>& opts) {
  ImGui::PushID(label);
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(label);
  ImGui::SameLine(S(84));
  float w = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * (float)(opts.size() - 1)) / (float)opts.size();
  bool first = true;
  for (const auto& o : opts) {
    if (!first) ImGui::SameLine();
    first = false;
    if (toggle(o.first, st_.sw[sw] == o.second, ImVec2(w, 0), (std::string(label) + ":" + o.first).c_str())) rig_.setSwitch(sw, o.second);
  }
  ImGui::PopID();
}

// ---------------------------------------------------------------- the header

void App::drawFreqDigits(long long hz, bool tx) {
  ImGui::PushFont(fFreq_);
  float dw = ImGui::CalcTextSize("0").x, h = ImGui::GetTextLineHeight(), dotW = dw * 0.5f;
  ImVec2 p = ImGui::GetCursorScreenPos();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImU32 col = tx ? C_RED : C_AMBER;
  float x = p.x;
  // 74.800.000 on a 7300, 1296.200.000 on a 9700.
  int digits = radio().topHz() >= 1000000000 ? 10 : 8;
  long long pow10 = digits == 10 ? 1000000000 : 10000000;
  for (int i = 0; i < digits; i++, pow10 /= 10) {
    int d = hz < 0 ? 0 : (int)(hz / pow10 % 10);
    bool blank = hz >= 0 && i < digits - 7 && hz < pow10;
    ImGui::PushID(i);
    ImGui::SetCursorScreenPos(ImVec2(x, p.y));
    ImGui::InvisibleButton("##digit", ImVec2(dw, h));
    note("digit:" + std::to_string(i));
    bool hovered = ImGui::IsItemHovered(), hit = ImGui::IsItemClicked();
    if (hovered) {
      float mid = p.y + h / 2, my = ImGui::GetIO().MousePos.y;
      dl->AddRectFilled(ImVec2(x, my < mid ? p.y : mid), ImVec2(x + dw, my < mid ? mid : p.y + h), IM_COL32(255, 255, 255, 22), 2);
      float wheel = ImGui::GetIO().MouseWheel;
      if (wheel != 0.f && st_.connected) tuneBy(pow10 * (wheel > 0 ? 1 : -1));
      if (hit && st_.connected) tuneBy(my < mid ? pow10 : -pow10);
    }
    if (hz < 0) {
      dl->AddText(ImVec2(x, p.y), C_DIM, "-");
    } else if (!blank) {
      char c[2] = {(char)('0' + d), 0};
      dl->AddText(ImVec2(x, p.y), col, c);
    }
    ImGui::PopID();
    x += dw;
    if (i == digits - 7 || i == digits - 4) {
      dl->AddText(ImVec2(x - dotW * 0.5f, p.y), hz < 0 ? C_DIM : col, ".");
      x += dotW;
    }
  }
  ImGui::PopFont();
  ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h));
}

void App::drawVfo() {
  ImGui::BeginChild("vfo", ImVec2(S(405), S(124)), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  if (!entry_.empty()) {
    // What's being typed, and what it'll be taken to mean.
    long long hz = tuning::parseFreq(entry_, st_.hz, radio());
    bool bad = glfwGetTime() < entryErrorUntil_;
    ImGui::PushFont(fFreq_);
    ImGui::TextColored(ImColor(bad ? C_RED : C_ACCENT), "%s_", entry_.c_str());
    ImGui::PopFont();
    ImGui::PushFont(fMid_);
    if (hz >= 0) ImGui::TextColored(ImColor(C_GREEN), "Enter: %s MHz", tuning::formatHz(hz).c_str());
    else ImGui::TextColored(ImColor(C_DIM), "keep typing, or Esc to cancel");
    ImGui::PopFont();
  } else {
    drawFreqDigits(st_.hz, st_.tx);
    ImGui::PushFont(fMid_);
    ImGui::TextColored(ImColor(C_ACCENT), "%s", modeLabel(st_.mode, st_.data).c_str());
    ImGui::PopFont();
    ImGui::SameLine();
    int w = civ::widthHz(st_.mode, st_.width);
    ImGui::AlignTextToFramePadding();
    if (radio().hasReceiverModes && civ::hasNoFilters(st_.mode) && w > 0) ImGui::Text("%.2f kHz", w / 1000.0);
    else if (st_.filter > 0 && w > 0 && st_.mode != civ::FM) ImGui::Text("FIL%d  %.2f kHz", st_.filter, w / 1000.0);
    else if (st_.filter > 0) ImGui::Text("FIL%d", st_.filter);
    if (st_.split == 1) {
      ImGui::SameLine();
      ImGui::TextColored(ImColor(C_AMBER), "SPLIT");
    }
    if (st_.ritOn == 1 || st_.xitOn == 1) {
      ImGui::SameLine();
      ImGui::TextColored(ImColor(C_AMBER), "%s %+d", st_.ritOn == 1 ? (st_.xitOn == 1 ? "RIT/XIT" : "RIT") : "XIT", st_.ritHz);
    }
    if (st_.sw[civ::S_LOCK] == 1) {
      ImGui::SameLine();
      ImGui::TextColored(ImColor(C_RED), "LOCK");
    }
  }
  if (radio().hasOtherVfo) {
    // The other VFO, always in view: it's where a split transmits.
    ImGui::TextDisabled("Other VFO");
    ImGui::SameLine();
    ImGui::PushFont(fFreqSmall_);
    ImGui::TextUnformatted(tuning::formatHz(st_.otherHz).c_str());
    ImGui::PopFont();
    ImGui::SameLine();
    ImGui::TextDisabled("%s", modeLabel(st_.otherMode, st_.otherData).c_str());
  } else {
    // A receiver has the one VFO, so what's worth seeing here is what it's listening with.
    ImGui::TextDisabled("Listening on");
    ImGui::SameLine();
    ImGui::PushFont(fFreqSmall_);
    bool hf = st_.hz < radio().antennaSwitchBelowHz;
    ImGui::Text("ANT%d", hf && st_.ant >= 0 ? st_.ant + 1 : 1);
    ImGui::PopFont();
    if (st_.ipPlus == 1) {
      ImGui::SameLine();
      ImGui::TextColored(ImColor(C_ACCENT), "IP+");
    }
  }
  ImGui::EndChild();
}

void App::drawMeters() {
  ImGui::BeginChild("meters", ImVec2(S(340), S(124)), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 org = ImGui::GetCursorScreenPos();
  float w = ImGui::GetContentRegionAvail().x, panelH = ImGui::GetContentRegionAvail().y;
  if (ImGui::InvisibleButton("meterswap", ImVec2(w, panelH))) needleMeter_ = !needleMeter_;
  note("meters");
  if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
  ImGui::SetItemTooltip(needleMeter_ ? "Click for the bar meter" : "Click for the needle meter");
  if (needleMeter_) {
    drawNeedleMeter(org, w, panelH);
    ImGui::EndChild();
    return;
  }
  float labelW = S(40), valW = S(62), barW = w - labelW - valW;

  auto bar = [&](float y, float h, const char* label, float frac, ImU32 col, const std::string& val, float red = 2.f) {
    float x0 = org.x + labelW, x1 = x0 + barW;
    dl->AddText(fSmall_, S(12.5f), ImVec2(org.x, y + (h - S(13)) / 2), C_DIM, label);
    dl->AddRectFilled(ImVec2(x0, y), ImVec2(x1, y + h), IM_COL32(10, 12, 15, 255), 2);
    frac = frac < 0 ? 0 : frac > 1 ? 1 : frac;
    if (frac > 0) {
      float split = x0 + barW * std::min(frac, red);
      dl->AddRectFilled(ImVec2(x0, y), ImVec2(split, y + h), col, 2);
      if (frac > red) dl->AddRectFilled(ImVec2(split, y), ImVec2(x0 + barW * frac, y + h), C_RED, 2);
    }
    dl->AddText(ImVec2(x1 + S(6), y + (h - S(15)) / 2), C_TEXT, val.c_str());
  };

  float y = org.y;
  bool tx = st_.tx;
  // The top bar is what matters now: signal on receive, power on transmit.
  if (!tx) {
    int raw = st_.meter[civ::M_S];
    smeterPeak_ = std::max(raw, smeterPeak_ - 2);
    float h = S(28);
    float red = 120.f / 241.f;
    bar(y, h, "S", raw < 0 ? 0 : raw / 241.f, C_GREEN, smeterText(raw), red);
    float x0 = org.x + labelW;
    if (smeterPeak_ > 0) {
      float px = x0 + barW * std::min(1.f, smeterPeak_ / 241.f);
      dl->AddLine(ImVec2(px, y), ImVec2(px, y + h), C_TEXT, 1.5f);
    }
    y += h + S(1);
    static const struct {
      int raw;
      const char* t;
    } ticks[] = {{10, "1"}, {30, "3"}, {60, "5"}, {90, "7"}, {120, "9"}, {158, "+20"}, {195, "+40"}, {233, "+60"}};
    for (auto& t : ticks) {
      float px = x0 + barW * t.raw / 241.f;
      dl->AddText(fSmall_, S(12.5f), ImVec2(px - S(4), y), t.raw > 120 ? C_RED : C_DIM, t.t);
    }
    y += S(16);
    if (!radio().hasTransmit) {
      // Nothing to show for power or SWR. What matters on a receiver is whether it's overloaded.
      float ty = y + S(6);
      dl->AddText(ImVec2(org.x, ty), st_.squelchOpen == 1 ? C_GREEN : C_DIM, st_.squelchOpen == 1 ? "Squelch open" : "Squelch closed");
      const char* over = st_.overflow == 1 ? "OVERLOAD" : "No overload";
      float ow = ImGui::CalcTextSize(over).x;
      dl->AddText(ImVec2(org.x + w - ow, ty), st_.overflow == 1 ? C_RED : C_DIM, over);
      ImGui::EndChild();
      return;
    }
  } else {
    int raw = st_.meter[civ::M_PO];
    double watts = raw < 0 ? 0 : civ::powerWatts(raw, radio(), st_.hz);
    int fullWatts = radio().maxWattsAt(st_.hz);
    char b[32];
    snprintf(b, sizeof b, fullWatts <= 10 ? "%.1f W" : "%.0f W", watts);
    bar(y, S(28), "Po", (float)(watts / fullWatts), C_AMBER, b, 1.0f);
    y += S(45);
  }
  float h = S(15);
  for (const MeterRow& r : lowerMeters()) {
    bar(y, h, r.label, r.frac, r.col, r.val, r.red);
    y += h + S(3);
  }
  ImGui::EndChild();
}

// The smaller meters under the top one, which are the same whichever kind that is.
std::array<App::MeterRow, 3> App::lowerMeters() const {
  bool tx = st_.tx;
  char b[32];
  double swr = st_.meter[civ::M_SWR] < 0 ? 1 : civ::swrRatio(st_.meter[civ::M_SWR]);
  snprintf(b, sizeof b, "%.1f", swr);
  MeterRow swrRow{"SWR", tx ? (float)((swr - 1) / 2.0) : 0, C_GREEN, tx ? b : "--", 0.5f};
  int alc = st_.meter[civ::M_ALC];
  MeterRow alcRow{"ALC", tx && alc > 0 ? alc / 120.f : 0, C_GREEN, tx && alc >= 0 ? std::to_string(alc * 100 / 120) + " %" : "--", 1.0f};
  if (tx) {
    int comp = st_.meter[civ::M_COMP];
    snprintf(b, sizeof b, "%.0f dB", comp < 0 ? 0.0 : civ::compDb(comp, radio()));
    return {swrRow, alcRow, MeterRow{"COMP", comp < 0 ? 0 : comp / 241.f, C_ACCENT, b, 2.f}};
  }
  int vd = st_.meter[civ::M_VD];
  if (vd >= 0) snprintf(b, sizeof b, "%.1f V", civ::voltsVd(vd));
  else snprintf(b, sizeof b, "--");
  return {swrRow, alcRow, MeterRow{"Vd", vd < 0 ? 0 : (float)(civ::voltsVd(vd) / 16.0), C_ACCENT, b, 2.f}};
}

// The needle takes the left of the panel. The right keeps the numbers and the meters a needle can't show,
// because SWR and ALC still matter while transmitting.
void App::drawNeedleMeter(ImVec2 org, float w, float h) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  bool tx = st_.tx;
  float reading = 0;
  char top[32];
  if (tx) {
    int raw = st_.meter[civ::M_PO];
    double watts = raw < 0 ? 0 : civ::powerWatts(raw, radio(), st_.hz);
    int fullWatts = radio().maxWattsAt(st_.hz);
    reading = needle::powerShare(watts, fullWatts);
    snprintf(top, sizeof top, fullWatts <= 10 ? "%.1f W" : "%.0f W", watts);
  } else {
    int raw = st_.meter[civ::M_S];
    if (st_.connected) reading = needle::signalShare(raw);
    snprintf(top, sizeof top, "%s", smeterText(raw).c_str());
  }
  float dt = ImGui::GetIO().DeltaTime;
  needle_.update(reading, dt);
  // The lamp warms up and fades rather than switching, and a dead radio leaves it nearly dark.
  meterLamp_ += ((st_.connected ? 1.f : 0.1f) - meterLamp_) * (1.f - expf(-dt / 0.25f));

  float faceW = S(208);
  drawNeedleFace(dl, org, ImVec2(org.x + faceW, org.y + h), needle_.pos);

  float x0 = org.x + faceW + S(8), x1 = org.x + w;
  auto rightText = [&](ImFont* font, float size, float y, ImU32 col, const char* t) {
    dl->AddText(font, size, ImVec2(x1 - font->CalcTextSizeA(size, FLT_MAX, 0, t).x, y), col, t);
  };
  dl->AddText(fSmall_, S(12.5f), ImVec2(x0, org.y + S(5)), C_DIM, tx ? "Po" : "S");
  rightText(fMid_, S(19), org.y, C_TEXT, top);
  float y = org.y + S(25);
  float rowH = (org.y + h - y) / 3;
  if (!radio().hasTransmit) {
    // Nothing to show for power or SWR. What matters on a receiver is whether it's overloaded.
    bool open = st_.squelchOpen == 1, over = st_.overflow == 1;
    dl->AddText(fSmall_, S(12.5f), ImVec2(x0, y + rowH * 0.5f), open ? C_GREEN : C_DIM, open ? "Squelch open" : "Squelch closed");
    dl->AddText(fSmall_, S(12.5f), ImVec2(x0, y + rowH * 1.5f), over ? C_RED : C_DIM, over ? "OVERLOAD" : "No overload");
    return;
  }
  for (const MeterRow& r : lowerMeters()) {
    dl->AddText(fSmall_, S(12.5f), ImVec2(x0, y), C_DIM, r.label);
    rightText(fSmall_, S(12.5f), y, C_TEXT, r.val.c_str());
    float by = y + S(16), bh = S(5), f = needle::clampShare(r.frac);
    dl->AddRectFilled(ImVec2(x0, by), ImVec2(x1, by + bh), IM_COL32(10, 12, 15, 255), 2);
    if (f > 0) {
      float split = x0 + (x1 - x0) * std::min(f, r.red);
      dl->AddRectFilled(ImVec2(x0, by), ImVec2(split, by + bh), r.col, 2);
      if (f > r.red) dl->AddRectFilled(ImVec2(split, by), ImVec2(x0 + (x1 - x0) * f, by + bh), C_RED, 2);
    }
    y += rowH;
  }
}

// A backlit meter face: a warm card lit from a lamp behind the bottom edge, printed with its scales, under a
// needle whose pivot hides behind a black cowl. All of it's in proportion to the height, 100 units.
void App::drawNeedleFace(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float reading) {
  const float kPi = 3.14159265f;
  const float kHalfSweep = 44.f * kPi / 180.f;
  float k = (p1.y - p0.y) / 100.f;
  float inset = 2.5f * k;
  ImVec2 f0(p0.x + inset, p0.y + inset), f1(p1.x - inset, p1.y - inset);
  float cx = (p0.x + p1.x) / 2, pivotY = p0.y + 130 * k;
  ImVec2 pivot(cx, pivotY);
  auto angleOf = [&](float share) { return (share - 0.5f) * 2 * kHalfSweep; };
  auto at = [&](float share, float r) { float a = angleOf(share); return ImVec2(cx + r * sinf(a), pivotY - r * cosf(a)); };

  // The lit card, as a mesh so the light can fall away from the lamp and into the corners.
  auto mix = [](float a, float b, float t) { return a + (b - a) * t; };
  auto faceColour = [&](float x, float y) {
    float dx = (x - cx) / ((f1.x - f0.x) * 0.62f), dy = (y - (f1.y + 8 * k)) / ((f1.y - f0.y) * 1.05f);
    float glow = expf(-(dx * dx + dy * dy) * 1.5f);
    float lit = meterLamp_;
    int r = (int)mix(46, mix(176, 255, glow), lit), g = (int)mix(40, mix(100, 222, glow), lit), b = (int)mix(32, mix(26, 138, glow), lit);
    return (ImU32)IM_COL32(r, g, b, 255);
  };
  const int kCols = 24, kRows = 12;
  dl->PrimReserve(kCols * kRows * 6, (kCols + 1) * (kRows + 1));
  ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
  unsigned int first = dl->_VtxCurrentIdx;
  for (int j = 0; j <= kRows; j++)
    for (int i = 0; i <= kCols; i++) {
      float x = mix(f0.x, f1.x, (float)i / kCols), y = mix(f0.y, f1.y, (float)j / kRows);
      dl->PrimWriteVtx(ImVec2(x, y), uv, faceColour(x, y));
    }
  for (int j = 0; j < kRows; j++)
    for (int i = 0; i < kCols; i++) {
      unsigned int a = first + j * (kCols + 1) + i, b = a + 1, c = a + kCols + 1, d = c + 1;
      dl->PrimWriteIdx((ImDrawIdx)a), dl->PrimWriteIdx((ImDrawIdx)b), dl->PrimWriteIdx((ImDrawIdx)d);
      dl->PrimWriteIdx((ImDrawIdx)a), dl->PrimWriteIdx((ImDrawIdx)d), dl->PrimWriteIdx((ImDrawIdx)c);
    }

  dl->PushClipRect(f0, f1, true);
  // Printed ink, so the red only looks red with the lamp behind it.
  const ImU32 ink = IM_COL32(40, 26, 14, 255), inkRed = IM_COL32((int)mix(66, 172, meterLamp_), (int)mix(36, 28, meterLamp_), (int)mix(26, 18, meterLamp_), 255);
  auto arc = [&](float r, float from, float to, ImU32 col, float thick) {
    dl->PathArcTo(pivot, r, angleOf(from) - kPi / 2, angleOf(to) - kPi / 2, 48);
    dl->PathStroke(col, ImDrawFlags_None, thick);
  };
  auto tick = [&](float share, float r0, float r1, ImU32 col, float thick) { dl->AddLine(at(share, r0), at(share, r1), col, thick); };
  auto label = [&](ImVec2 c, float size, ImU32 col, const char* t) {
    ImVec2 sz = fSmall_->CalcTextSizeA(size, FLT_MAX, 0, t);
    dl->AddText(fSmall_, size, ImVec2(c.x - sz.x / 2, c.y - sz.y / 2), col, t);
  };

  // The S scale along the top: black to S9, then the red band.
  const float kR1 = 106 * k, kS9 = needle::signalShare(120);
  arc(kR1, 0, kS9, ink, 1.3f * k);
  arc(kR1 + 1.2f * k, kS9, 1, inkRed, 3.4f * k);
  static const struct {
    int raw;
    const char* t;
  } kSTicks[] = {{10, "1"}, {20, nullptr}, {30, "3"}, {45, nullptr}, {60, "5"}, {75, nullptr}, {90, "7"}, {105, nullptr}, {120, "9"},
                 {139, nullptr}, {158, "+20"}, {177, nullptr}, {195, "+40"}, {214, nullptr}, {233, "+60"}};
  for (auto& t : kSTicks) {
    float share = needle::signalShare(t.raw);
    ImU32 col = t.raw > 120 ? inkRed : ink;
    tick(share, kR1, kR1 + (t.t ? 7 : 4.5f) * k, col, (t.t ? 1.6f : 1.1f) * k);
    if (t.t) label(at(share, kR1 + 15 * k), S(12), col, t.t);
  }
  label(at(-0.07f, kR1 - 2 * k), S(12.5f), ink, "S");

  // The power scale under it, in percent so that it holds for every band's full power.
  const float kR2 = 83 * k;
  if (radio().hasTransmit) {
    arc(kR2, 0, needle::kPoFull, ink, 1.1f * k);
    arc(kR2 - 0.6f * k, needle::kPoFull, 1, inkRed, 2.4f * k);
    for (int pct = 0; pct <= 100; pct += 5) {
      bool major = pct % 25 == 0;
      float share = needle::kPoFull * pct / 100.f;
      tick(share, kR2, kR2 - (major ? 6 : 3) * k, ink, (major ? 1.4f : 0.9f) * k);
      if (major) {
        char t[8];
        snprintf(t, sizeof t, "%d", pct);
        label(at(share, kR2 - 13 * k), S(10.5f), ink, t);
      }
    }
    label(at(-0.13f, kR2 - 3 * k), S(10.5f), ink, "Po %");
  } else {
    label(ImVec2(cx, pivotY - kR2), S(10.5f), ink, "SIGNAL");
  }

  // The transmit lamp, in the corner of the card.
  if (radio().hasTransmit) {
    ImVec2 led(f0.x + 10 * k, f0.y + 10 * k);
    bool tx = st_.tx;
    if (tx) dl->AddCircleFilled(led, 7 * k, IM_COL32(255, 60, 30, 70), 20);
    dl->AddCircleFilled(led, 3.2f * k, tx ? IM_COL32(255, 70, 40, 255) : IM_COL32(96, 40, 22, 255), 16);
    dl->AddText(fSmall_, S(10.5f), ImVec2(led.x + 6 * k, led.y - S(6)), ink, "TX");
  }

  // The needle throws a soft shadow on the card, and goes behind the cowl.
  ImVec2 a0 = at(reading, 40 * k), a1 = at(reading, kR1 + 5 * k);
  ImVec2 off(1.6f * k, 1.2f * k);
  dl->AddLine(ImVec2(a0.x + off.x, a0.y + off.y), ImVec2(a1.x + off.x, a1.y + off.y), IM_COL32(40, 20, 0, (int)(70 * meterLamp_)), 2.0f * k);
  dl->AddLine(a0, a1, IM_COL32(20, 16, 14, 255), 1.5f * k);
  dl->AddCircleFilled(pivot, 52 * k, IM_COL32(20, 20, 23, 255), 64);
  dl->AddCircle(pivot, 52 * k, IM_COL32(255, 205, 130, (int)(90 * meterLamp_)), 64, 1.0f * k);

  // Glass: the shade under the top of the case, and a faint reflection.
  dl->AddRectFilledMultiColor(f0, ImVec2(f1.x, f0.y + 9 * k), IM_COL32(0, 0, 0, 120), IM_COL32(0, 0, 0, 120), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0));
  dl->AddQuadFilled(ImVec2(f0.x + 24 * k, f0.y), ImVec2(f0.x + 62 * k, f0.y), ImVec2(f0.x + 30 * k, f1.y), ImVec2(f0.x - 8 * k, f1.y), IM_COL32(255, 255, 255, 12));
  dl->PopClipRect();

  // The case.
  dl->AddRect(ImVec2(p0.x + inset / 2, p0.y + inset / 2), ImVec2(p1.x - inset / 2, p1.y - inset / 2), IM_COL32(14, 14, 16, 255), 4 * k, 0, inset + 0.5f * k);
  dl->AddRect(p0, p1, IM_COL32(70, 74, 82, 255), 4.5f * k, 0, 0.8f * k);
}

void App::saveMemoryHere() {
  store::Memory m;
  m.hz = st_.hz, m.mode = st_.mode, m.data = st_.data, m.filter = st_.filter ? st_.filter : 1;
  m.name = utcNow("%H%MZ");
  memories_.push_back(m);
  store::saveMemories(memPath_, memories_);
  say("Saved " + tuning::formatHz(m.hz) + " to memories");
}

// Where a transceiver has its transmit buttons, a receiver has its antennas.
void App::drawReceiverPanel() {
  ImGui::BeginChild("tx", ImVec2(0, S(124)), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  float gap = ImGui::GetStyle().ItemSpacing.x;
  int count = radio().antennas;
  float w = (ImGui::GetContentRegionAvail().x - gap * (float)(count - 1)) / (float)count;
  bool hf = st_.hz < radio().antennaSwitchBelowHz;
  static const char* const kPorts[] = {"Type-N, 50 ohms: 10 kHz to 3 GHz", "SO-239, 50 ohms: 10 kHz to 30 MHz", "RCA, 500 ohms: 10 kHz to 30 MHz"};
  ImGui::BeginDisabled(!st_.connected);
  for (int i = 0; i < count; i++) {
    char label[8], name[8];
    snprintf(label, sizeof label, "ANT%d", i + 1);
    snprintf(name, sizeof name, "ant:%d", i + 1);
    if (i) ImGui::SameLine();
    ImGui::BeginDisabled(i > 0 && !hf);
    bool on = hf ? st_.ant == i : i == 0;
    if (toggle(label, on, ImVec2(w, S(52)), name)) rig_.setAntenna(i);
    ImGui::SetItemTooltip("%s", kPorts[std::min(i, 2)]);
    ImGui::EndDisabled();
  }
  if (panelButton("Save to memories", ImVec2(-1, 0))) saveMemoryHere();
  note("quickmem");
  ImGui::SetItemTooltip("Save this frequency and mode to memories");
  ImGui::EndDisabled();
  ImGui::TextDisabled(hf ? "Receive only. Hold the mouse over an antenna for its port." : "Receive only. Above 30 MHz it can only use ANT1.");
  ImGui::EndChild();
}

void App::drawTxPanel() {
  ImGui::BeginChild("tx", ImVec2(0, S(124)), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  float gap = ImGui::GetStyle().ItemSpacing.x;
  bool tuner = radio().hasTuner;
  float w = tuner ? (ImGui::GetContentRegionAvail().x - gap * 3) / 4 : (ImGui::GetContentRegionAvail().x - gap) / 2;
  float h = S(52);
  bool armed = st_.txArmed;
  if (actionButton(armed ? "ARMED" : "ARM TX", ImVec2(w, h), armed ? V_AMBER : ImGui::GetStyle().Colors[ImGuiCol_Button],
                   armed ? V_AMBER_HI : ImGui::GetStyle().Colors[ImGuiCol_ButtonHovered], "arm"))
    rig_.armTx(!armed);
  ImGui::SetItemTooltip("Nothing here can transmit until this is on. Off at every start.\nTurning it off also releases PTT.");
  ImGui::SameLine();
  if (actionButton(st_.tx ? "TX ON" : "PTT", ImVec2(w, h), st_.tx ? V_RED_HI : (armed ? V_RED : ImGui::GetStyle().Colors[ImGuiCol_Button]), V_RED_HI, "ptt")) {
    if (!armed) say("Transmit isn't armed. Press ARM TX first.");
    else if (!st_.connected) say("The rig isn't connected");
    else rig_.ptt(!st_.tx);
  }
  ImGui::SetItemTooltip("Click on, click off. Ctrl+Space does the same.\nReleases by itself after %d s (Settings).", st_.txLimit);
  if (tuner) {
    ImGui::SameLine();
    if (actionButton(st_.atu == 2 ? "TUNING" : "TUNE", ImVec2(w, h), st_.atu == 2 ? V_RED_HI : ImGui::GetStyle().Colors[ImGuiCol_Button], V_RED_HI, "tune")) {
      if (!armed) say("Transmit isn't armed. Press ARM TX first.");
      else rig_.tuneAtu();
    }
    ImGui::SetItemTooltip("Starts the rig's antenna tuner. This transmits a carrier.");
    ImGui::SameLine();
    if (toggle("ATU", st_.atu >= 1, ImVec2(w, h))) rig_.setAtu(st_.atu < 1);
    ImGui::SetItemTooltip("The antenna tuner in or out of line");
  }

  float w5 = (ImGui::GetContentRegionAvail().x - gap * 3) / 4;
  ImGui::BeginDisabled(!st_.connected);
  if (toggle("SPLIT", st_.split == 1, ImVec2(w5, 0))) rig_.setSplit(st_.split != 1);
  ImGui::SetItemTooltip("Receive on this VFO, transmit on the other. Right-click for a quick offset.");
  if (ImGui::BeginPopupContextItem("splitmenu")) {
    for (int k : {1, 2, 5, 10, -1, -2, -5}) {
      char b[32];
      snprintf(b, sizeof b, "Split %+d kHz", k);
      if (ImGui::MenuItem(b)) {
        rig_.setOtherFreq(st_.hz + k * 1000);
        rig_.setOtherMode(st_.mode, st_.data, st_.filter ? st_.filter : 1);
        rig_.setSplit(true);
      }
    }
    ImGui::EndPopup();
  }
  ImGui::SameLine();
  if (panelButton("A/B", ImVec2(w5, 0))) rig_.vfoSwap();
  note("swap");
  ImGui::SetItemTooltip("Swap the two VFOs");
  ImGui::SameLine();
  if (panelButton("A=B", ImVec2(w5, 0))) rig_.vfoEqual();
  ImGui::SetItemTooltip("Copy this VFO to the other");
  ImGui::SameLine();
  if (panelButton("Mem", ImVec2(w5, 0))) saveMemoryHere();
  note("quickmem");
  ImGui::SetItemTooltip("Save this frequency and mode to memories");
  ImGui::EndDisabled();

  if (st_.tx) {
    ImGui::TextColored(ImColor(C_RED), "TRANSMITTING");
    if (st_.txSeconds > 0) {
      ImGui::SameLine();
      ImGui::Text("%d s of %d s", st_.txSeconds, st_.txLimit);
    }
  } else if (armed) {
    ImGui::TextColored(ImColor(C_AMBER), "Armed: the buttons above will transmit");
  } else {
    ImGui::TextDisabled("Transmit locked");
  }
  ImGui::EndChild();
}

void App::drawHeader() {
  ImGui::BeginDisabled(!st_.connected);
  drawVfo();
  ImGui::EndDisabled();
  ImGui::SameLine();
  drawMeters();  // stays clickable with no radio, for swapping the meter
  ImGui::SameLine();
  if (radio().hasTransmit) drawTxPanel();
  else drawReceiverPanel();
}

// A receiver's coverage runs to far too many bands for a row of buttons, so they go on a menu by what they're for.
void App::drawBandMenu() {
  int here = plan_.bandOf(st_.hz);
  std::string label = (here >= 0 ? plan_.bands[(size_t)here].title() : std::string("Bands")) + "##bandmenu";
  if (panelButton(label.c_str(), ImVec2(S(150), S(30)))) ImGui::OpenPopup("bandmenu");
  note("bandmenu");
  ImGui::SetItemTooltip("The band you're in. Click for every band the receiver covers.\nPage Up and Page Down go through them in order.");
  if (!ImGui::BeginPopup("bandmenu")) return;
  std::vector<std::string> groups;
  auto groupOf = [](const bands::Band& b) { return b.group.empty() ? std::string("Amateur") : b.group; };
  for (const bands::Band& b : plan_.bands)
    if (std::find(groups.begin(), groups.end(), groupOf(b)) == groups.end()) groups.push_back(groupOf(b));
  for (const std::string& g : groups) {
    bool open = ImGui::BeginMenu(g.c_str());
    note("bandgroup:" + g);
    if (!open) continue;
    for (int i = 0; i < (int)plan_.bands.size(); i++) {
      const bands::Band& band = plan_.bands[(size_t)i];
      if (groupOf(band) != g) continue;
      ImGui::PushID(i);
      bool anyPreset = false;
      for (const bands::Marker& p : band.markers) anyPreset = anyPreset || p.preset();
      if (!anyPreset) {
        if (ImGui::MenuItem(band.title().c_str(), nullptr, here == i)) goBand(i);
        note("band:" + band.name);
      } else if (ImGui::BeginMenu(band.title().c_str())) {
        if (ImGui::MenuItem("Go to the band", nullptr, here == i)) goBand(i);
        note("band:" + band.name);
        ImGui::Separator();
        for (const bands::Marker& p : band.markers)
          if (p.preset()) {
            char b[64];
            snprintf(b, sizeof b, "%-5s  %s", p.label.c_str(), tuning::formatHz(p.hz).c_str());
            if (ImGui::MenuItem(b)) goMarker(p);
            if (!p.note.empty()) ImGui::SetItemTooltip("%s", p.note.c_str());
          }
        ImGui::EndMenu();
      }
      ImGui::PopID();
    }
    ImGui::EndMenu();
  }
  ImGui::EndPopup();
}

// WFM, and the modes a transceiver hasn't got. Most of these are for one service or another, so they're on menus.
void App::drawReceiverModes(int filter) {
  if (toggle("WFM", st_.mode == civ::WFM, ImVec2(S(46), S(30)), "mode:WFM")) rig_.setMode(civ::WFM, false, filter);
  ImGui::SetItemTooltip("Wide FM, for FM broadcast and other wide signals");
  ImGui::SameLine();
  auto menu = [&](const char* button, const char* id, const std::vector<std::pair<const char*, int>>& modes, const char* tip) {
    bool on = false;
    const char* shown = button;
    for (const auto& m : modes)
      if (st_.mode == m.second) on = true, shown = m.first;
    if (toggle((std::string(shown) + "##" + id).c_str(), on, ImVec2(S(72), S(30)), id)) ImGui::OpenPopup(id);
    ImGui::SetItemTooltip("%s", tip);
    if (ImGui::BeginPopup(id)) {
      for (const auto& m : modes) {
        if (ImGui::MenuItem(m.first, nullptr, st_.mode == m.second)) rig_.setMode(m.second, false, filter);
        note(std::string("mode:") + m.first);
      }
      ImGui::EndPopup();
    }
    ImGui::SameLine();
  };
  menu("S-AM", "modes:sam", {{"S-AM", civ::S_AM}, {"S-AM(L)", civ::S_AM_L}, {"S-AM(U)", civ::S_AM_U}},
       "Synchronous AM: both sidebands, or just the lower or upper one to dodge interference");
  menu("Digital", "modes:digital", {{"DV", civ::DV}, {"P25", civ::P25}, {"dPMR", civ::DPMR}, {"NXDN-VN", civ::NXDN_VN}, {"NXDN-N", civ::NXDN_N}, {"DCR", civ::DCR}},
       "D-STAR, P25, dPMR, NXDN and DCR digital voice");
}

void App::drawBandRow() {
  ImGui::BeginDisabled(!st_.connected);
  int here = plan_.bandOf(st_.hz);
  float bw = S(37);
  bool anyBand = false;
  if (!radio().hasTransmit) {
    drawBandMenu();
    anyBand = true;
  }
  for (int i = 0; i < (int)plan_.bands.size() && radio().hasTransmit; i++) {
    const bands::Band& band = plan_.bands[(size_t)i];
    if (!bandOnRig(band)) continue;
    if (anyBand) ImGui::SameLine();
    anyBand = true;
    ImGui::PushID(i);
    if (toggle(band.name.c_str(), here == i, ImVec2(bw, S(30)), ("band:" + band.name).c_str())) goBand(i);
    ImGui::SetItemTooltip("%s. Goes back to where you were on this band.\nRight-click: CW, FT8 and so on.", band.title().c_str());
    if (ImGui::BeginPopupContextItem("presets")) {
      ImGui::TextDisabled("%s", band.title().c_str());
      for (const bands::Marker& p : band.markers)
        if (p.preset()) {
          char b[64];
          snprintf(b, sizeof b, "%-5s  %s", p.label.c_str(), tuning::formatHz(p.hz).c_str());
          if (ImGui::MenuItem(b)) goMarker(p);
          note("preset:" + band.name + ":" + p.label);
        }
      ImGui::EndPopup();
    }
    ImGui::PopID();
  }
  if (!anyBand) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("The %s plan has no bands for the %s", plan_.name.c_str(), radio().name);
  }
  ImGui::SameLine(0, S(14));
  static const struct {
    const char* name;
    int mode;
  } modes[] = {{"LSB", civ::LSB}, {"USB", civ::USB}, {"CW", civ::CW}, {"CW-R", civ::CW_R}, {"AM", civ::AM}, {"FM", civ::FM}, {"RTTY", civ::RTTY}, {"RTTY-R", civ::RTTY_R}};
  int filter = st_.filter ? st_.filter : 1;
  for (const auto& m : modes) {
    float mw = S(m.mode == civ::RTTY_R ? 56 : m.mode == civ::RTTY || m.mode == civ::CW_R ? 46 : 38);
    if (toggle(m.name, st_.mode == m.mode, ImVec2(mw, S(30)), (std::string("mode:") + m.name).c_str())) rig_.setMode(m.mode, false, filter);
    ImGui::SameLine();
  }
  if (radio().hasDigitalVoice) {
    if (toggle("DV", st_.mode == civ::DV, ImVec2(S(38), S(30)), "mode:DV")) rig_.setMode(civ::DV, false, filter);
    ImGui::SetItemTooltip("D-STAR digital voice");
    ImGui::SameLine();
    ImGui::BeginDisabled(st_.hz < 1240000000);  // only the 1200 MHz band has it
    if (toggle("DD", st_.mode == civ::DD, ImVec2(S(38), S(30)), "mode:DD")) rig_.setMode(civ::DD, false, filter);
    ImGui::SetItemTooltip("D-STAR high-speed data, on 23 cm only");
    ImGui::EndDisabled();
    ImGui::SameLine();
  }
  if (radio().hasReceiverModes) drawReceiverModes(filter);
  if (radio().hasDataMode) {
    ImGui::BeginDisabled(!civ::hasData(st_.mode));
    if (toggle("DATA", st_.data, ImVec2(S(46), S(30)), "data")) rig_.setMode(st_.mode, !st_.data, filter);
    ImGui::SetItemTooltip("Data mode: the rig takes its transmit audio from the USB sound card (with DATA MOD set to USB)");
    ImGui::EndDisabled();
  }
  ImGui::SameLine(0, S(14));
  ImGui::BeginDisabled(civ::hasNoFilters(st_.mode) && radio().hasReceiverModes);
  for (int f = 1; f <= 3; f++) {
    char b[8];
    snprintf(b, sizeof b, "FIL%d", f);
    if (toggle(b, st_.filter == f, ImVec2(S(38), S(30)), b)) rig_.setMode(st_.mode, st_.data, f);
    ImGui::SameLine();
  }
  ImGui::EndDisabled();
  ImGui::EndDisabled();
  flow(S(125));
  ImGui::AlignTextToFramePadding();
  ImGui::TextDisabled("Step");
  ImGui::SameLine();
  ImGui::SetNextItemWidth(S(80));
  if (ImGui::BeginCombo("##step", kStepNames[stepIdx_])) {
    for (int i = 0; i < kStepCount; i++)
      if (ImGui::Selectable(kStepNames[i], i == stepIdx_)) stepIdx_ = i;
    ImGui::EndCombo();
  }
  ImGui::SetItemTooltip("Left and Right arrows tune by this; Up and Down change it.\nWheel over the waterfalls uses it too.");
}

// ---------------------------------------------------------------- the left-hand controls

void App::drawControls() {
  ImGui::BeginChild("controls", ImVec2(S(292), 0), ImGuiChildFlags_Borders);
  ImGui::BeginDisabled(!st_.connected);
  auto heading = [&](const char* t) {
    ImGui::Spacing();
    ImGui::TextColored(ImColor(C_ACCENT), "%s", t);
    ImGui::Separator();
  };
  heading("RECEIVE");
  levelRow("AF", civ::L_AF);
  levelRow("RF gain", civ::L_RF);
  levelRow("Squelch", civ::L_SQL);
  if (radio().hasExtPreamp) {
    radioRow("Preamp", civ::S_PREAMP, {{"OFF", 0}, {"ON", 1}, {"EXT", 2}, {"BOTH", 3}});
    ImGui::SetItemTooltip("The internal preamp, and any external one the rig powers (set up in its menu)");
  } else if (radio().singlePreamp) {
    radioRow("Preamp", civ::S_PREAMP, {{"OFF", 0}, {"ON", 1}});
  } else {
    radioRow("Preamp", civ::S_PREAMP, {{"OFF", 0}, {"P1", 1}, {"P2", 2}});
  }
  if (radio().hasIpPlus) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("IP+");
    ImGui::SameLine(S(84));
    if (toggle(st_.ipPlus == 1 ? "ON##ipplus" : "OFF##ipplus", st_.ipPlus == 1, ImVec2(-1, 0), "ipplus")) rig_.setIpPlus(st_.ipPlus != 1);
    ImGui::SetItemTooltip("Improves how it copes with strong signals, at some cost in sensitivity");
  }
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted("Attenuator");
  ImGui::SameLine(S(84));
  if (radio().attSteps > 1) {
    int steps = radio().attSteps + 1;
    float aw = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * (float)(steps - 1)) / (float)steps;
    for (int i = 0; i < steps; i++) {
      int db = radio().attDb * i;
      char attLabel[24], attName[16];
      snprintf(attLabel, sizeof attLabel, i ? "%d##att" : "OFF##att", db);
      snprintf(attName, sizeof attName, "att:%d", db);
      if (i) ImGui::SameLine();
      if (toggle(attLabel, st_.att >= 0 && st_.attDb == db, ImVec2(aw, 0), attName)) rig_.setAttDb(db);
    }
  } else {
    char attLabel[16];
    snprintf(attLabel, sizeof attLabel, "%d dB", radio().attDb);
    if (toggle(attLabel, st_.att == 1, ImVec2(-1, 0), "att")) rig_.setAtt(st_.att != 1);
  }
  radioRow("AGC", civ::S_AGC, {{"FAST", 1}, {"MID", 2}, {"SLOW", 3}});

  heading("FILTER");
  {
    int maxCode = civ::widthCodeMax(st_.mode);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Width");
    ImGui::SameLine(S(84));
    ImGui::SetNextItemWidth(-1);
    if (maxCode > 0) {
      int v = std::max(0, std::min(maxCode, st_.width));
      char fmt[32];
      snprintf(fmt, sizeof fmt, "%d Hz", civ::widthHz(st_.mode, v));
      if (ImGui::SliderInt("##width", &v, 0, maxCode, fmt)) rig_.setWidth(v);
      note("width");
      if (ImGui::IsItemHovered() && ImGui::GetIO().MouseWheel != 0.f)
        rig_.setWidth(std::max(0, std::min(maxCode, v + (int)ImGui::GetIO().MouseWheel)));
    } else {
      ImGui::TextDisabled("fixed by the mode");
    }
  }
  radioRow("Shape", civ::S_DSP_FILTER, {{"SHARP", 0}, {"SOFT", 1}});
  auto pbt = [&](const char* label, int lv) {
    int v = std::max(0, st_.level[lv]);
    char fmt[32];
    snprintf(fmt, sizeof fmt, "%+d", v - 128);
    levelRow(label, lv, fmt);
  };
  pbt("PBT inner", civ::L_PBT_IN);
  pbt("PBT outer", civ::L_PBT_OUT);
  ImGui::SetCursorPosX(S(84) + ImGui::GetStyle().WindowPadding.x);
  if (panelButton("Centre PBT", ImVec2(-1, 0))) {
    rig_.setLevel(civ::L_PBT_IN, 128);
    rig_.setLevel(civ::L_PBT_OUT, 128);
  }

  heading("NOISE");
  switchLevelRow("NB", civ::S_NB, civ::L_NB);
  switchLevelRow("NR", civ::S_NR, civ::L_NR);
  switchLevelRow("Notch", civ::S_MN, civ::L_NOTCH);
  radioRow("Notch width", civ::S_NOTCH_WIDTH, {{"WIDE", 0}, {"MID", 1}, {"NAR", 2}});
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted("Auto notch");
  ImGui::SameLine(S(84));
  if (toggle("AN", st_.sw[civ::S_AN] == 1, ImVec2(-1, 0), "an")) rig_.setSwitch(civ::S_AN, st_.sw[civ::S_AN] == 1 ? 0 : 1);

  if (radio().hasRit) {
    heading("RIT / XIT");
    bool xit = radio().hasXit;
    float w = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * (xit ? 2 : 1)) / (xit ? 3 : 2);
    if (toggle("RIT", st_.ritOn == 1, ImVec2(w, 0))) rig_.setRitOn(st_.ritOn != 1);
    ImGui::SameLine();
    if (xit) {
      if (toggle("XIT", st_.xitOn == 1, ImVec2(w, 0))) rig_.setXitOn(st_.xitOn != 1);
      ImGui::SameLine();
    }
    if (panelButton("Clear", ImVec2(w, 0))) rig_.setRit(0);
    int v = st_.ritHz;
    ImGui::SetNextItemWidth(-1);
    if (ImGui::SliderInt("##rit", &v, -2000, 2000, "%+d Hz")) rig_.setRit(v);
    if (ImGui::IsItemHovered() && ImGui::GetIO().MouseWheel != 0.f) rig_.setRit(st_.ritHz + (int)ImGui::GetIO().MouseWheel * 10);
  }

  if (!radio().hasTransmit) {
    // CW pitch is the one thing here that still means something: it's where the receiver puts a CW signal.
    heading("CW");
    char fmt[32];
    snprintf(fmt, sizeof fmt, "%d Hz", tuning::cwPitchHz(st_.level[civ::L_CW_PITCH]));
    levelRow("CW pitch", civ::L_CW_PITCH, fmt);
    ImGui::EndDisabled();
    ImGui::EndChild();
    return;
  }

  heading("TRANSMIT");
  {
    int v = std::max(0, st_.level[civ::L_POWER]);
    char fmt[32];
    int fullWatts = radio().maxWattsAt(st_.hz);
    if (fullWatts <= 10) snprintf(fmt, sizeof fmt, "%.1f W", v * (double)fullWatts / 255.0);
    else snprintf(fmt, sizeof fmt, "%d W", (int)lround(v * (double)fullWatts / 255.0));
    levelRow("RF power", civ::L_POWER, fmt);
  }
  levelRow("Mic gain", civ::L_MIC);
  switchLevelRow("Comp", civ::S_COMP, civ::L_COMP);
  radioRow("SSB TX BW", civ::S_SSB_TX_BW, {{"WIDE", 0}, {"MID", 1}, {"NAR", 2}});
  switchLevelRow("Monitor", civ::S_MON, civ::L_MONITOR);
  switchLevelRow("VOX", civ::S_VOX, civ::L_VOX);
  levelRow("Anti-VOX", civ::L_ANTIVOX);

  heading("CW");
  radioRow("Break-in", civ::S_BKIN, {{"OFF", 0}, {"SEMI", 1}, {"FULL", 2}});
  {
    char fmt[32];
    snprintf(fmt, sizeof fmt, "%d WPM", tuning::keySpeedWpm(st_.level[civ::L_KEY_SPEED]));
    levelRow("Key speed", civ::L_KEY_SPEED, fmt);
    snprintf(fmt, sizeof fmt, "%d Hz", tuning::cwPitchHz(st_.level[civ::L_CW_PITCH]));
    levelRow("CW pitch", civ::L_CW_PITCH, fmt);
    levelRow("BK-IN delay", civ::L_BKIN_DELAY);
  }
  ImGui::EndDisabled();
  ImGui::EndChild();
}

// ---------------------------------------------------------------- the waterfalls

App::PlotArea App::drawPlot(const char* id, double xLo, double xHi, const std::vector<float>& line, double lineLo, double lineHi,
                            float yLo, float yHi, Waterfall& wf, float u0, float u1, const std::function<std::string(double)>& axisLabel,
                            double axisStep, float rowHeight, float stripH) {
  PlotArea a;
  ImVec2 avail = ImGui::GetContentRegionAvail();
  avail.y = std::max(avail.y, S(60));
  a.p0 = ImGui::GetCursorScreenPos();
  a.p1 = ImVec2(a.p0.x + avail.x, a.p0.y + avail.y);
  ImGui::InvisibleButton(id, avail, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
  note(id);
  a.hovered = ImGui::IsItemHovered();
  a.clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
  a.rclicked = ImGui::IsItemClicked(ImGuiMouseButton_Right);
  ImGuiIO& io = ImGui::GetIO();
  float w = avail.x, h = avail.y;
  a.x = xLo + (xHi - xLo) * (double)((io.MousePos.x - a.p0.x) / w);
  a.wheel = a.hovered ? io.MouseWheel : 0.f;

  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->PushClipRect(a.p0, a.p1, true);
  float specH = floorf(h * 0.28f), axisH = S(17);
  ImVec2 s0 = a.p0, s1(a.p1.x, a.p0.y + specH);
  a.w0 = ImVec2(a.p0.x, s1.y + axisH + stripH);  // the band plan's strip goes in the gap, drawn by the caller
  a.w1 = a.p1;

  // Spectrum: one column a pixel, each the strongest value it covers.
  dl->AddRectFilled(s0, s1, IM_COL32(8, 10, 13, 255));
  float yr = std::max(1.f, yHi - yLo);
  for (int g = 1; g < 4; g++) {
    float gy = s0.y + specH * (float)g / 4;
    dl->AddLine(ImVec2(s0.x, gy), ImVec2(s1.x, gy), IM_COL32(255, 255, 255, 14));
  }
  if (!line.empty() && lineHi > lineLo) {
    int n = (int)line.size();
    double perPx = (xHi - xLo) / (double)w, perBin = (lineHi - lineLo) / n;
    static std::vector<ImVec2> pts;
    pts.clear();
    for (int px = 0; px < (int)w; px++) {
      double f0 = xLo + perPx * px, f1 = f0 + perPx;
      int b0 = (int)((f0 - lineLo) / perBin), b1 = (int)((f1 - lineLo) / perBin);
      b0 = std::max(0, std::min(n - 1, b0));
      b1 = std::max(b0, std::min(n - 1, b1));
      float v = line[(size_t)b0];
      for (int b = b0 + 1; b <= b1; b++) v = std::max(v, line[(size_t)b]);
      float t = (v - yLo) / yr;
      t = t < 0 ? 0 : t > 1 ? 1 : t;
      float y = s1.y - t * specH;
      float x = s0.x + (float)px + 0.5f;
      dl->AddLine(ImVec2(x, y), ImVec2(x, s1.y), IM_COL32(40, 130, 170, 90));
      pts.push_back(ImVec2(x, y));
    }
    if (pts.size() > 1) dl->AddPolyline(pts.data(), (int)pts.size(), IM_COL32(150, 225, 250, 255), ImDrawFlags_None, 1.f);
  }

  // Axis.
  dl->AddRectFilled(ImVec2(a.p0.x, s1.y), ImVec2(a.p1.x, a.w0.y), IM_COL32(18, 21, 27, 255));
  if (axisStep > 0 && xHi > xLo) {
    float labelEnd = a.p0.x;  // a long label pushed in from the edge mustn't run into the next
    for (double v = ceil(xLo / axisStep) * axisStep; v < xHi; v += axisStep) {
      float x = a.p0.x + (float)((v - xLo) / (xHi - xLo)) * w;
      dl->AddLine(ImVec2(x, s0.y), ImVec2(x, s1.y), IM_COL32(255, 255, 255, 18));
      dl->AddLine(ImVec2(x, a.w0.y - S(3)), ImVec2(x, a.w0.y), C_DIM);
      std::string t = axisLabel(v);
      float tw = ImGui::CalcTextSize(t.c_str()).x * (S(12.5f) / ImGui::GetFontSize());
      float tx = std::max(a.p0.x + S(2), x - tw / 2);
      if (tx < labelEnd) continue;
      dl->AddText(fSmall_, S(12.5f), ImVec2(tx, s1.y + S(1)), C_TEXT, t.c_str());
      labelEnd = tx + tw + S(4);
    }
  }
  // Waterfall.
  dl->AddRectFilled(a.w0, a.w1, IM_COL32(0, 0, 10, 255));
  wf.draw(dl, a.w0, a.w1, u0, u1, std::max(1.f, roundf(scale_ * rowHeight)));
  dl->PopClipRect();
  return a;
}

// The whole of the band the dial is in, each portion named in plain words, with the part the scope is
// showing outlined and the dial marked. For finding your way round a band without knowing it already.
void App::drawBandMap(double viewLo, double viewHi) {
  float w = ImGui::GetContentRegionAvail().x, h = S(20);
  ImVec2 p0 = ImGui::GetCursorScreenPos(), p1(p0.x + w, p0.y + h);
  ImGui::InvisibleButton("bandmap", ImVec2(w, h));
  note("bandmap");
  bool hovered = ImGui::IsItemHovered(), clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(p0, p1, IM_COL32(12, 14, 18, 255));
  float mx = ImGui::GetIO().MousePos.x, textY = p0.y + (h - S(12.5f)) / 2;

  int here = plan_.bandOf(st_.hz);
  if (here < 0) {
    std::string t = st_.hz > 0 ? "Not on a band in the " + plan_.name + " plan" : "";
    if (const bands::StationRange* r = rangeHere(st_.hz)) t += ".  In the " + r->label + " band";
    dl->AddText(fSmall_, S(12.5f), ImVec2(p0.x + S(6), textY), C_DIM, t.c_str());
    return;
  }
  const bands::Band& band = plan_.bands[(size_t)here];
  auto edge = [](long long hz) {
    char b[32];
    snprintf(b, sizeof b, hz % 1000 ? "%.4f" : "%.3f", (double)hz / 1e6);
    return std::string(b);
  };
  auto widthOf = [&](const std::string& t) { return fSmall_->CalcTextSizeA(S(12.5f), FLT_MAX, 0, t.c_str()).x; };
  // Where it begins, on the left, and where it ends, on the right.
  std::string left = band.title() + "  " + edge(band.lo), right = edge(band.hi);
  dl->AddText(fSmall_, S(12.5f), ImVec2(p0.x + S(4), textY), C_TEXT, left.c_str());
  float rightW = widthOf(right);
  dl->AddText(fSmall_, S(12.5f), ImVec2(p1.x - rightW - S(4), textY), C_TEXT, right.c_str());
  float x0 = p0.x + widthOf(left) + S(10), x1 = p1.x - rightW - S(10);
  if (x1 <= x0) return;
  auto toX = [&](double f) { return x0 + (float)((f - (double)band.lo) / (double)(band.hi - band.lo)) * (x1 - x0); };

  const bands::Segment* over = nullptr;
  for (const bands::Segment& s : band.segments) {
    float a = toX((double)s.lo), b = toX((double)s.hi);
    b = std::max(b, a + 1.f);
    dl->AddRectFilled(ImVec2(a, p0.y + S(1)), ImVec2(b, p1.y - S(1)), segmentColour(s.use, 170));
    std::string word = portionName(s);
    float tw = widthOf(word);
    if (tw + S(4) < b - a) dl->AddText(fSmall_, S(12.5f), ImVec2((a + b - tw) / 2, textY), IM_COL32(255, 255, 255, 255), word.c_str());
    if (hovered && mx >= a && mx < b) over = &s;
  }
  // What the scope is showing, then the dial.
  float va = std::max(x0, toX(viewLo)), vb = std::min(x1, toX(viewHi));
  if (vb > va) dl->AddRect(ImVec2(va, p0.y), ImVec2(vb, p1.y), IM_COL32(255, 255, 255, 200), 2);
  float dx = toX((double)st_.hz);
  dl->AddLine(ImVec2(dx, p0.y), ImVec2(dx, p1.y), st_.tx ? C_RED : IM_COL32(255, 200, 80, 255), 2.f);

  if (hovered && mx >= x0 && mx <= x1) {
    double f = (double)band.lo + (double)((mx - x0) / (x1 - x0)) * (double)(band.hi - band.lo);
    if (over) ImGui::SetTooltip("%s: %s\n%s to %s MHz\nClick to tune here", portionName(*over).c_str(), over->label.c_str(), edge(over->lo).c_str(), edge(over->hi).c_str());
    else ImGui::SetTooltip("Out of band under the %s plan", plan_.name.c_str());
    if (clicked && over && st_.connected) tuneTo(std::max(over->lo, std::min(over->hi, llround(f / 1000) * 1000)));
  }
}

int App::freeFlagRow(const FlagLayer& l, float x0, float x1) const {
  for (int r = 0; r < l.rows; r++) {
    bool free = true;
    for (const auto& p : l.taken[r])
      if (x0 < p.second + S(2) && x1 > p.first - S(2)) free = false;
    if (free) return r;
  }
  return -1;
}

// One flag, in up to three rows along the top of the spectrum. A flag with no room left keeps its tick,
// which still shows what it is under the mouse.
void App::drawFlag(const PlotArea& a, double lo, double hi, float stripH, FlagLayer& l, const bands::Marker& m, ImU32 col, bool filled, const std::string& rectName) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  float y0 = a.w0.y - stripH, y1 = a.w0.y, specBottom = y0 - S(17);
  float x = a.p0.x + (float)(((double)m.hz - lo) / (hi - lo)) * (a.p1.x - a.p0.x);
  float tw = fSmall_->CalcTextSizeA(S(12.5f), FLT_MAX, 0, m.label.c_str()).x;
  float lx0 = x - tw / 2 - S(3), lx1 = x + tw / 2 + S(3);
  int row = freeFlagRow(l, lx0, lx1);
  float top = a.p0.y + S(1);
  dl->AddLine(ImVec2(x, top), ImVec2(x, specBottom), (col & 0x00FFFFFF) | IM_COL32(0, 0, 0, 110));
  dl->AddLine(ImVec2(x, y0), ImVec2(x, y1), IM_COL32(255, 255, 255, 230));
  ImVec2 mouse = ImGui::GetIO().MousePos;
  bool inLabel = false;
  if (row >= 0) {
    l.taken[row].push_back({lx0, lx1});
    float ly = top + (float)row * S(16);
    ImVec2 r0(lx0, ly), r1(lx1, ly + S(15));
    // The plan's flags are outlined and the stations' solid, so they can be told apart at a glance.
    dl->AddRectFilled(r0, r1, filled ? (col & 0x00FFFFFF) | IM_COL32(0, 0, 0, 215) : IM_COL32(12, 15, 20, 225), 3);
    dl->AddRect(r0, r1, col, 3);
    dl->AddText(fSmall_, S(12.5f), ImVec2(x - tw / 2, ly + S(1)), filled ? IM_COL32(255, 255, 255, 255) : C_TEXT, m.label.c_str());
    inLabel = mouse.x >= lx0 && mouse.x <= lx1 && mouse.y >= r0.y && mouse.y <= r1.y;
    if (!opt_.selfTest.empty()) rects_[rectName] = ImRect(r0, r1);
  }
  float dist = fabsf(mouse.x - x);
  if (a.hovered && (inLabel || (dist < l.bestDist && mouse.y < specBottom))) {
    l.hit = &m;
    l.bestDist = inLabel ? 0 : dist;
  }
}

ImU32 serviceColour(bands::Service s) {
  switch (s) {
    case bands::S_COMMERCIAL: return IM_COL32(190, 70, 130, 255);
    case bands::S_GOVERNMENT: return IM_COL32(95, 105, 125, 255);
    case bands::S_NAVAL: return IM_COL32(25, 110, 165, 255);
    case bands::S_MILITARY: return IM_COL32(120, 125, 40, 255);
    case bands::S_AMATEUR: return IM_COL32(40, 135, 90, 255);
    case bands::S_AVIATION: return IM_COL32(50, 90, 190, 255);
    case bands::S_SPACE: return IM_COL32(105, 75, 170, 255);
    case bands::S_LAND: return IM_COL32(125, 105, 60, 255);
    default: return IM_COL32(170, 85, 40, 255);
  }
}

// Whatever else is on the air, whichever plan is chosen. Broadcast bands and the like are a bar along the
// bottom of the spectrum with their name in a free spot, since they're often wider than the screen.
void App::drawStationOverlay(const PlotArea& a, double lo, double hi, float stripH, FlagLayer& l) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  auto toX = [&](double f) { return a.p0.x + (float)((f - lo) / (hi - lo)) * (a.p1.x - a.p0.x); };
  float specBottom = a.w0.y - stripH - S(17);
  long long now = (long long)time(nullptr);
  int weekday = (int)((now / 86400 + 3) % 7), minute = (int)(now % 86400 / 60);  // 1 January 1970 was a Thursday
  for (const bands::StationFile& f : stations_.files)
    for (const bands::Station& s : f.stations)
      if (showService_[s.service] && (double)s.hz >= lo && (double)s.hz <= hi && (!onAirOnly_ || s.onAir(weekday, minute)))
        drawFlag(a, lo, hi, stripH, l, s, serviceColour(s.service), true, "station:" + std::to_string(s.hz));
  for (const bands::StationFile& f : stations_.files)
    for (const bands::StationRange& r : f.ranges) {
      if (!showService_[r.service] || (double)r.hi <= lo || (double)r.lo >= hi) continue;
      float x0 = std::max(a.p0.x, toX((double)r.lo)), x1 = std::min(a.p1.x, toX((double)r.hi));
      ImU32 col = serviceColour(r.service);
      dl->AddRectFilled(ImVec2(x0, specBottom - S(3)), ImVec2(x1, specBottom), col);
      float tw = fSmall_->CalcTextSizeA(S(12.5f), FLT_MAX, 0, r.label.c_str()).x;
      float cx = (x0 + x1) / 2, lx0 = cx - tw / 2 - S(3), lx1 = cx + tw / 2 + S(3);
      int row = lx1 - lx0 < x1 - x0 ? freeFlagRow(l, lx0, lx1) : -1;
      if (row < 0) continue;
      l.taken[row].push_back({lx0, lx1});
      float ly = a.p0.y + S(1) + (float)row * S(16);
      dl->AddRectFilled(ImVec2(lx0, ly), ImVec2(lx1, ly + S(15)), (col & 0x00FFFFFF) | IM_COL32(0, 0, 0, 130), 3);
      dl->AddText(fSmall_, S(12.5f), ImVec2(cx - tw / 2, ly + S(1)), IM_COL32(255, 255, 255, 230), r.label.c_str());
    }
}

// On the scope: the plan's strip with each portion named, the band's edges with everything outside them
// shaded, and flags on the frequencies worth knowing.
void App::drawPlanOverlay(const PlotArea& a, double lo, double hi, float stripH, FlagLayer& l) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  auto toX = [&](double f) { return a.p0.x + (float)((f - lo) / (hi - lo)) * (a.p1.x - a.p0.x); };
  auto widthOf = [&](const char* t) { return fSmall_->CalcTextSizeA(S(12.5f), FLT_MAX, 0, t).x; };
  float y0 = a.w0.y - stripH, y1 = a.w0.y, textY = y0 + (stripH - S(12.5f)) / 2;
  float specBottom = y0 - S(17);
  const ImU32 edgeCol = IM_COL32(255, 80, 70, 255);

  auto outOfBand = [&](double from, double to) {
    float x0 = toX(from), x1 = toX(to);
    // "Out of band" is about where you may transmit, which is nothing to a receiver.
    if (x1 - x0 < 1.f || !radio().hasTransmit) return;
    dl->AddRectFilled(ImVec2(x0, a.p0.y), ImVec2(x1, specBottom), IM_COL32(0, 0, 0, 150));
    dl->AddRectFilled(ImVec2(x0, a.w0.y), ImVec2(x1, a.p1.y), IM_COL32(0, 0, 0, 150));
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), IM_COL32(70, 18, 18, 255));
    const char* t = "Out of band";
    float tw = widthOf(t);
    if (tw + S(6) < x1 - x0) dl->AddText(fSmall_, S(12.5f), ImVec2((x0 + x1 - tw) / 2, textY), IM_COL32(255, 170, 160, 255), t);
    if (from > lo) dl->AddLine(ImVec2(x0, a.p0.y), ImVec2(x0, a.p1.y), edgeCol, 1.5f);
    if (to < hi) dl->AddLine(ImVec2(x1, a.p0.y), ImVec2(x1, a.p1.y), edgeCol, 1.5f);
  };
  double covered = lo;
  for (const bands::Band& band : plan_.bands)
    for (const bands::Segment& s : band.segments) {
      if ((double)s.hi <= lo || (double)s.lo >= hi) continue;
      if ((double)s.lo > covered) outOfBand(covered, (double)s.lo);
      covered = std::max(covered, (double)s.hi);
      float x0 = std::max(a.p0.x, toX((double)s.lo)), x1 = std::min(a.p1.x, toX((double)s.hi));
      dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), segmentColour(s.use, 170));
      std::string word = portionName(s);
      float tw = widthOf(word.c_str());
      if (tw + S(6) < x1 - x0) dl->AddText(fSmall_, S(12.5f), ImVec2((x0 + x1 - tw) / 2, textY), IM_COL32(255, 255, 255, 255), word.c_str());
    }
  if (covered < hi) outOfBand(covered, hi);

  // Say where the band itself begins and ends, on the inside of each edge. 60 m's inner blocks have only
  // the line, or there'd be no room.
  auto edgeLabel = [&](double f, const std::string& t, bool toRight) {
    if (f <= lo || f >= hi) return;
    float tw = widthOf(t.c_str()), x = toX(f);
    float tx = toRight ? x + S(5) : x - tw - S(5), ty = a.w0.y + S(3);
    dl->AddRectFilled(ImVec2(tx - S(3), ty - S(1)), ImVec2(tx + tw + S(3), ty + S(15)), IM_COL32(0, 0, 0, 190), 3);
    dl->AddText(fSmall_, S(12.5f), ImVec2(tx, ty), IM_COL32(255, 170, 160, 255), t.c_str());
  };
  for (const bands::Band& band : plan_.bands) {
    char b[64];
    snprintf(b, sizeof b, "%s begins %s", band.title().c_str(), tuning::formatHz(band.lo).c_str());
    edgeLabel((double)band.lo, b, true);
    snprintf(b, sizeof b, "%s ends %s", band.title().c_str(), tuning::formatHz(band.hi).c_str());
    edgeLabel((double)band.hi, b, false);
  }

  // Presets get first pick of the room.
  for (const bands::Band& band : plan_.bands) {
    if ((double)band.hi <= lo || (double)band.lo >= hi) continue;
    for (int pass = 0; pass < 2; pass++)
      for (const bands::Marker& m : band.markers) {
        if (m.preset() != (pass == 0) || (double)m.hz < lo || (double)m.hz > hi) continue;
        const bands::Segment* seg = band.segmentAt(m.hz);
        drawFlag(a, lo, hi, stripH, l, m, seg ? segmentColour(seg->use, 255) : C_TEXT, false, "marker:" + band.name + ":" + m.label);
      }
  }
}

void App::drawStationsMenu() {
  if (!ImGui::BeginMenu("Stations")) return;
  if (ImGui::MenuItem("Find a station...", "Ctrl+F")) stationSearchOpen_ = stationSearchFocus_ = true;
  ImGui::MenuItem("Show on the scope", nullptr, &showStations_);
  ImGui::MenuItem("Only what's scheduled now", nullptr, &onAirOnly_);
  ImGui::SetItemTooltip("Flags a broadcaster only during its scheduled hours. This menu and Find a station list them all,\nwith their times in UTC. Being scheduled doesn't mean you'll hear it.");
  ImGui::Separator();
  auto item =[&](const std::string& text, const std::string& tip, long long hz) {
    bool chosen = ImGui::MenuItem(text.c_str(), nullptr, false, st_.connected && radio().canTune(hz));
    if (!tip.empty()) ImGui::SetItemTooltip("%s", tip.c_str());
    return chosen;
  };
  for (int i = 0; i < bands::S_COUNT; i++) {
    bands::Service service = (bands::Service)i;
    if (!ImGui::BeginMenu(bands::serviceName(service))) continue;
    ImGui::MenuItem("Show on the scope", nullptr, &showService_[i]);
    ImGui::Separator();
    for (const bands::StationFile& f : stations_.files) {
      // The stations in a group are next to each other in the file.
      for (size_t s = 0; s < f.stations.size();) {
        const std::string& group = f.stations[s].group;
        size_t end = s;
        while (end < f.stations.size() && f.stations[end].group == group) end++;
        if (f.stations[s].service == service && ImGui::BeginMenu((group + "##" + f.id).c_str())) {
          for (size_t k = s; k < end; k++) {
            const bands::Station& st = f.stations[k];
            if (item(st.label + "   " + tuning::formatHz(st.hz), st.note, st.tuneHz())) goMarker(st);
          }
          ImGui::EndMenu();
        }
        s = end;
      }
      bool any = false;
      for (const bands::StationRange& r : f.ranges) any = any || r.service == service;
      if (any && ImGui::BeginMenu(("Bands##" + f.id).c_str())) {
        for (const bands::StationRange& r : f.ranges)
          if (r.service == service && item(r.label + "   " + tuning::formatHz(r.lo) + " to " + tuning::formatHz(r.hi), r.note, (r.lo + r.hi) / 2)) goRange(r);
        ImGui::EndMenu();
      }
    }
    ImGui::EndMenu();
  }
  ImGui::EndMenu();
}

void App::drawStationSearch() {
  if (!stationSearchOpen_) return;
  ImGui::SetNextWindowSize(ImVec2(S(560), S(420)), ImGuiCond_FirstUseEver);
  if (stationSearchFocus_) ImGui::SetNextWindowFocus();
  if (!ImGui::Begin("Find a station", &stationSearchOpen_, ImGuiWindowFlags_NoCollapse)) {
    ImGui::End();
    return;
  }
  if (stationSearchFocus_) ImGui::SetKeyboardFocusHere();
  stationSearchFocus_ = false;
  ImGui::SetNextItemWidth(-1);
  bool enter = ImGui::InputTextWithHint("##stationquery", "Name, type, place or frequency in kHz", stationQuery_, sizeof stationQuery_, ImGuiInputTextFlags_EnterReturnsTrue);
  note("stationquery");
  if (stationHitsStale_ || stationHitsFor_ != stationQuery_) {
    stationHits_ = stations_.search(stationQuery_);
    stationHitsFor_ = stationQuery_;
    stationHitsStale_ = false;
  }
  const std::vector<bands::StationHit>& hits = stationHits_;
  stationHitCount_ = hits.size();
  ImGui::TextDisabled(st_.connected ? "%d found. Click one to go there, or press Enter for the first." : "%d found. Connect the rig to go there.", (int)hits.size());
  ImGui::BeginChild("stationhits", ImVec2(0, 0), ImGuiChildFlags_Borders);
  ImGui::BeginDisabled(!st_.connected);
  auto go = [&](const bands::StationHit& h) {
    if (h.station) goMarker(*h.station);
    else goRange(*h.range);
  };
  // Only the rows in view are drawn.
  ImGuiListClipper clipper;
  clipper.Begin((int)hits.size());
  while (clipper.Step())
    for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++) {
      const bands::StationHit& h = hits[(size_t)i];
      const std::string& label = h.station ? h.station->label : h.range->label;
      const std::string& tip = h.station ? h.station->note : h.range->note;
      ImGui::PushID(i);
      ImGui::BeginDisabled(!radio().canTune(h.station ? h.station->tuneHz() : (h.range->lo + h.range->hi) / 2));
      if (ImGui::Selectable("##hit", false, ImGuiSelectableFlags_AllowOverlap)) go(h);
      note("stationhit:" + std::to_string(i));
      if (!tip.empty()) ImGui::SetItemTooltip("%s", tip.c_str());
      ImGui::SameLine(S(6));
      ImGui::TextUnformatted(label.c_str());
      ImGui::SameLine(S(160));
      ImGui::TextUnformatted(tuning::formatHz(h.hz).c_str());
      ImGui::SameLine(S(270));
      ImGui::TextDisabled("%s", h.station ? h.station->group.c_str() : bands::serviceName(h.range->service));
      ImGui::EndDisabled();
      ImGui::PopID();
    }
  ImGui::EndDisabled();
  ImGui::EndChild();
  if (enter && !hits.empty() && st_.connected) go(hits[0]);
  if (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) && ImGui::IsKeyPressed(ImGuiKey_Escape)) stationSearchOpen_ = false;
  ImGui::End();
}

void App::drawScopePane() {
  ImGui::BeginChild("scopepane", ImVec2(0, ImGui::GetContentRegionAvail().y * (showAudio_ ? splitFrac_ : 1.f) - (showAudio_ ? S(3) : 0)), ImGuiChildFlags_Borders,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::TextColored(ImColor(C_ACCENT), "BAND");
  ImGui::SameLine();
  ImGui::BeginDisabled(!st_.connected);
  bool on = scopeEnabled_;
  if (toggle(on ? "On" : "Off", on, ImVec2(S(34), 0), "scopeon")) {
    scopeEnabled_ = !on;
    rig_.setScope(scopeEnabled_);
  }
  ImGui::SetItemTooltip("Streams the rig's own spectrum scope over CI-V");
  int spanIdx = 4;
  for (int i = 0; i < radio().scopeSpans; i++)
    if (st_.scopeSpan == civ::kScopeSpans[i]) spanIdx = i;
  flow(S(80));
  ImGui::SetNextItemWidth(S(80));
  if (ImGui::BeginCombo("##span", kSpanNames[spanIdx])) {
    for (int i = 0; i < radio().scopeSpans; i++)
      if (ImGui::Selectable(kSpanNames[i], i == spanIdx)) rig_.setScopeSpan(civ::kScopeSpans[i]);
    ImGui::EndCombo();
  }
  ImGui::SetItemTooltip("Span, in Centre mode");
  flow(S(120));
  if (toggle("Centre", st_.scopeMode == civ::SCOPE_CENTER, ImVec2(S(52), 0), "scopecentre")) rig_.setScopeMode(civ::SCOPE_CENTER);
  ImGui::SameLine();
  if (toggle("Fixed", st_.scopeMode == civ::SCOPE_FIXED, ImVec2(S(46), 0), "scopefixed")) rig_.setScopeMode(civ::SCOPE_FIXED);
  ImGui::SetItemTooltip("Centre follows the dial. Fixed uses the band edges set on the rig.");
  flow(S(130));
  ImGui::TextDisabled("Ref");
  ImGui::SameLine();
  float ref = (float)st_.scopeRef;
  ImGui::SetNextItemWidth(S(90));
  if (ImGui::SliderFloat("##ref", &ref, -20.f, 20.f, "%+.1f dB")) rig_.setScopeRef(roundf(ref * 2) / 2);
  ImGui::SetItemTooltip("The rig's scope reference level");
  flow(S(90));
  static const char* speedNames[] = {"Fast", "Mid", "Slow"};
  ImGui::SetNextItemWidth(S(64));
  if (ImGui::BeginCombo("##speed", st_.scopeSpeed >= 0 && st_.scopeSpeed <= 2 ? speedNames[st_.scopeSpeed] : "Speed")) {
    for (int i = 0; i < 3; i++)
      if (ImGui::Selectable(speedNames[i], i == st_.scopeSpeed)) rig_.setScopeSpeed(i);
    ImGui::EndCombo();
  }
  ImGui::SetItemTooltip("Sweep speed");
  ImGui::EndDisabled();
  flow(S(50));
  if (panelButton("Auto##s")) autoScopeLevels();
  ImGui::SetItemTooltip("Set the colours from the noise floor");
  flow(S(60));
  if (panelButton("Display##s")) ImGui::OpenPopup("scopedisplay");
  if (ImGui::BeginPopup("scopedisplay")) {
    ImGui::SetNextItemWidth(S(160));
    ImGui::SliderFloat("Floor", &scopeFloor_, 0.f, 120.f, "%.0f");
    ImGui::SetNextItemWidth(S(160));
    ImGui::SliderFloat("Range", &scopeRange_, 20.f, 160.f, "%.0f");
    ImGui::SetNextItemWidth(S(160));
    ImGui::Combo("Colours", &palette_, kPaletteNames, PAL_COUNT);
    ImGui::EndPopup();
  }

  float k = 255.f / civ::kScopeLevelMax;
  wfScope_.setDisplay(scopeFloor_ * k, (scopeFloor_ + scopeRange_) * k, palette_);
  double span = (double)(scopeHi_ - scopeLo_);
  double lo = (double)scopeLo_, hi = (double)scopeHi_;
  if (span <= 0) {
    // Nothing yet: lay the axis out from what we know, so it doesn't jump when the first sweep lands.
    double c = st_.hz > 0 ? (double)st_.hz : 14000000.0;
    double sp = st_.scopeSpan > 0 ? (double)st_.scopeSpan : 100000.0;
    lo = c - sp / 2, hi = c + sp / 2, span = sp;
  }
  double step = niceStep(span, 8);
  auto label = [&](double v) {
    char b[32];
    if (step >= 1000) snprintf(b, sizeof b, "%.3f", v / 1e6);
    else snprintf(b, sizeof b, "%.4f", v / 1e6);
    return std::string(b);
  };
  // The rig sends only about 4 sweeps a second (measured on a real 7300, polling or not), so each gets a
  // taller row than the audio's 25 lines a second, or the band waterfall would barely move.
  if (showBandPlan_) drawBandMap(lo, hi);
  float stripH = showBandPlan_ ? S(16) : 0;
  PlotArea a = drawPlot("scopeplot", lo, hi, scopeLine_, lo, hi, scopeFloor_, scopeFloor_ + scopeRange_, wfScope_, 0.f, 1.f, label, step, 4.f, stripH);
  auto toX = [&](double f) { return a.p0.x + (float)((f - lo) / (hi - lo)) * (a.p1.x - a.p0.x); };
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->PushClipRect(a.p0, a.p1, true);

  // Flags, which the plan's get first pick of the room for.
  FlagLayer flags;
  flags.rows = std::max(1, std::min(3, (int)((a.w0.y - stripH - S(17) - a.p0.y) / S(16))));
  flags.bestDist = S(4);
  if (showBandPlan_) drawPlanOverlay(a, lo, hi, stripH, flags);
  if (showStations_) drawStationOverlay(a, lo, hi, stripH, flags);
  const bands::Marker* marker = flags.hit;
  if (marker) {
    std::string t = marker->label + "   " + tuning::formatHz(marker->hz);
    if (!marker->note.empty()) t += "\n" + marker->note;
    if (!marker->preset()) t += "\nClick to tune there";
    else if (marker->tuneHz() != marker->hz) t += std::string("\nClick to tune ") + civ::modeName(marker->mode) + (marker->data ? "-D" : "") + " at " + tuning::formatHz(marker->tuneHz());
    else t += std::string("\nClick to go there in ") + civ::modeName(marker->mode) + (marker->data ? "-D" : "");
    ImGui::SetTooltip("%s", t.c_str());
  }
  // The passband and the dial.
  if (st_.hz > 0 && st_.mode >= 0) {
    double pl, ph;
    tuning::rfPassband(st_.hz, st_.mode, civ::widthHz(st_.mode, st_.width), pl, ph);
    float x0 = toX(pl), x1 = std::max(toX(ph), x0 + 1.f);
    dl->AddRectFilled(ImVec2(x0, a.p0.y), ImVec2(x1, a.p1.y), IM_COL32(255, 255, 255, 30));
    float dx = toX((double)st_.hz);
    dl->AddLine(ImVec2(dx, a.p0.y), ImVec2(dx, a.p1.y), st_.tx ? C_RED : IM_COL32(255, 200, 80, 220), 1.f);
    if (st_.split == 1 && st_.otherHz > 0) {
      float tx = toX((double)st_.otherHz);
      dl->AddLine(ImVec2(tx, a.p0.y), ImVec2(tx, a.p1.y), C_RED, 1.f);
      dl->AddText(fSmall_, S(12.5f), ImVec2(tx + S(3), a.p0.y + S(2)), C_RED, "TX");
    }
  }
  const char* banner = nullptr;
  if (!st_.connected) banner = "The rig isn't connected";
  else if (!scopeEnabled_) banner = "Scope data is off";
  else if (glfwGetTime() - scopeLastAt_ > 3.0) banner = "No scope data. On the rig: CI-V USB Port = Unlink from [REMOTE], CI-V USB Baud Rate = 115200";
  else if (scopeOor_) banner = "Out of the scope's range";
  if (banner) {
    ImVec2 ts = ImGui::CalcTextSize(banner);
    ImVec2 c((a.p0.x + a.p1.x - ts.x) / 2, (a.w0.y + a.w1.y - ts.y) / 2);
    dl->AddRectFilled(ImVec2(c.x - S(8), c.y - S(4)), ImVec2(c.x + ts.x + S(8), c.y + ts.y + S(4)), IM_COL32(0, 0, 0, 170), 4);
    dl->AddText(c, C_TEXT, banner);
  }
  if (a.hovered) {
    float mx = ImGui::GetIO().MousePos.x;
    dl->AddLine(ImVec2(mx, a.p0.y), ImVec2(mx, a.p1.y), IM_COL32(255, 255, 255, 110));
    std::string t = tuning::formatHz(llround(a.x / 100) * 100);
    if (showBandPlan_) {
      const bands::Segment* seg = plan_.segmentAt(llround(a.x));
      if (seg) t += "  " + portionName(*seg);
      else if (radio().hasTransmit) t += "  Out of band";
    }
    if (const bands::StationRange* r = rangeHere(llround(a.x))) t += "  " + r->label;
    float tw = ImGui::CalcTextSize(t.c_str()).x;
    float lx = std::min(mx + S(6), a.p1.x - tw - S(6));
    dl->AddRectFilled(ImVec2(lx - S(3), a.p0.y + S(2)), ImVec2(lx + tw + S(3), a.p0.y + S(20)), IM_COL32(0, 0, 0, 190), 3);
    dl->AddText(ImVec2(lx, a.p0.y + S(3)), C_AMBER, t.c_str());
  }
  dl->PopClipRect();

  if (st_.connected && hi > lo && a.hovered) {
    // Snapped so a click lands on a tidy frequency: 100 Hz, or 10 Hz when zoomed right in.
    long long snap = span <= 10000 ? 10 : 100;
    long long hz = llround(a.x / (double)snap) * snap;
    if (a.clicked) {
      if (marker) goMarker(*marker);
      else tuneTo(hz);
    }
    if (a.wheel != 0.f) tuneTo(tuning::stepFreq(st_.hz, kSteps[stepIdx_], a.wheel > 0 ? 1 : -1, radio()));
    if (a.rclicked) {
      scopeMenuHz_ = hz;
      ImGui::OpenPopup("scopemenu");
    }
  }
  if (ImGui::BeginPopup("scopemenu")) {
    long long lastHz = scopeMenuHz_;
    ImGui::TextDisabled("%s", tuning::formatHz(lastHz).c_str());
    if (ImGui::MenuItem("Tune here")) tuneTo(lastHz);
    if (radio().hasTransmit && ImGui::MenuItem("Transmit here (split)")) {
      rig_.setOtherFreq(lastHz);
      rig_.setOtherMode(st_.mode, st_.data, st_.filter ? st_.filter : 1);
      rig_.setSplit(true);
    }
    if (ImGui::MenuItem("Save to memories")) {
      store::Memory m;
      m.hz = lastHz, m.mode = st_.mode, m.data = st_.data, m.filter = st_.filter ? st_.filter : 1;
      m.name = utcNow("%H%MZ");
      memories_.push_back(m);
      store::saveMemories(memPath_, memories_);
    }
    ImGui::EndPopup();
  }
  ImGui::EndChild();
}

void App::startRecording() {
  std::string path = store::dataDir() + "rx-" + utcNow("%Y%m%d-%H%M%S") + "-" + std::to_string(st_.hz > 0 ? st_.hz : 0) + ".wav";
  if (rec_.open(path, AudioEngine::kRate)) say("Recording to " + path);
  else say("Can't write " + path);
}

// From the sweep itself: the rig's levels vary a lot with band and Ref (a quiet 40m peaked at 29 of 160).
void App::autoScopeLevels() {
  if (scopeLine_.empty()) return;
  std::vector<float> s = scopeLine_;
  std::sort(s.begin(), s.end());
  float median = s[s.size() / 2], high = s[s.size() * 99 / 100];
  scopeFloor_ = std::max(0.f, median - 3.f);
  scopeRange_ = std::max(25.f, std::min(160.f, (high - scopeFloor_) * 1.4f));
}

void App::autoAudioLevels() {
  if (!spectrum_) return;
  int top = std::max(8, (int)(kAudioRanges[audioRangeIdx_] / (AudioEngine::kRate / 2.f) * (float)audioLine_.size()));
  std::vector<float> s(audioLine_.begin(), audioLine_.begin() + std::min(top, (int)audioLine_.size()));
  std::nth_element(s.begin(), s.begin() + (long)s.size() / 2, s.end());
  audioFloorDb_ = std::max(-140.f, std::min(-30.f, s[s.size() / 2] - 6.f));
  audioRangeDb_ = 55.f;
}

void App::drawAudioPane() {
  ImGui::BeginChild("audiopane", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::TextColored(ImColor(C_ACCENT), "AUDIO");
  ImGui::SameLine();
  ImGui::SetNextItemWidth(S(76));
  if (ImGui::BeginCombo("##arange", kAudioRangeNames[audioRangeIdx_])) {
    for (int i = 0; i < 5; i++)
      if (ImGui::Selectable(kAudioRangeNames[i], i == audioRangeIdx_)) audioRangeIdx_ = i;
    ImGui::EndCombo();
  }
  flow(S(50));
  if (panelButton("Auto##a")) autoAudioLevels();
  ImGui::SetItemTooltip("Set the colours from the noise floor");
  flow(S(60));
  if (panelButton("Display##a")) ImGui::OpenPopup("audiodisplay");
  if (ImGui::BeginPopup("audiodisplay")) {
    ImGui::SetNextItemWidth(S(160));
    ImGui::SliderFloat("Floor", &audioFloorDb_, -140.f, -30.f, "%.0f dB");
    ImGui::SetNextItemWidth(S(160));
    ImGui::SliderFloat("Range", &audioRangeDb_, 20.f, 110.f, "%.0f dB");
    ImGui::SetNextItemWidth(S(160));
    ImGui::Combo("Colours", &palette_, kPaletteNames, PAL_COUNT);
    ImGui::SetNextItemWidth(S(160));
    ImGui::SliderInt("Speed", &audioLinesPerSec_, 5, 60, "%d lines/s");
    ImGui::SetNextItemWidth(S(160));
    if (ImGui::BeginCombo("FFT size", kFftNames[fftIdx_])) {
      for (int i = 0; i < 4; i++)
        if (ImGui::Selectable(kFftNames[i], i == fftIdx_) && i != fftIdx_) {
          fftIdx_ = i;
          applyFft();
        }
      ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("Larger shows finer detail but blurs fast changes");
    ImGui::EndPopup();
  }
  flow(S(40));
  if (rec_.isOpen()) {
    char b[32];
    snprintf(b, sizeof b, "REC %.0fs", rec_.seconds());
    if (actionButton(b, ImVec2(0, 0), V_RED, V_RED_HI, "rec")) {
      say("Saved " + rec_.path());
      rec_.close();
    }
  } else if (panelButton("Rec")) {
    startRecording();
  }
  ImGui::SetItemTooltip("Record the receiver to a WAV file");
  flow(S(120));
  bool mon = audio_.monitoring();
  if (toggle("Listen", mon, ImVec2(0, 0), "listen")) {
    if (mon) {
      audio_.stopMonitor();
      monitor_ = false;
    } else {
      std::string err;
      if (audio_.startMonitor(audioOut_, err)) monitor_ = true;
      else say(err);
    }
  }
  ImGui::SetItemTooltip("Play the receiver through this computer's speakers. Muted while transmitting.");
  ImGui::SameLine();
  ImGui::SetNextItemWidth(S(60));
  if (ImGui::SliderFloat("##mon", &monitorGain_, 0.f, 1.5f, "%.1f")) audio_.setMonitorGain(monitorGain_);
  flow(S(90));
  // The level coming in. The rig's USB AF Output Level sets this; clipping ruins a waterfall.
  {
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = S(70), h = ImGui::GetFrameHeight();
    ImGui::Dummy(ImVec2(w, h));
    ImGui::SetItemTooltip("Audio level from the rig: %.0f dBFS. Set by the rig's USB AF Output Level.", peakDb_);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float y0 = p.y + h * 0.3f, y1 = p.y + h * 0.7f;
    dl->AddRectFilled(ImVec2(p.x, y0), ImVec2(p.x + w, y1), IM_COL32(10, 12, 15, 255), 2);
    float f = std::max(0.f, std::min(1.f, (peakDb_ + 60.f) / 60.f));
    dl->AddRectFilled(ImVec2(p.x, y0), ImVec2(p.x + w * f, y1), peakHold_ > 0 ? C_RED : C_GREEN, 2);
  }

  wfAudio_.setDisplay((audioFloorDb_ + 140.f) * (255.f / 140.f), (audioFloorDb_ + audioRangeDb_ + 140.f) * (255.f / 140.f), palette_);
  double top = kAudioRanges[audioRangeIdx_];
  double nyq = AudioEngine::kRate / 2.0;
  double step = niceStep(top, 10);
  auto label = [&](double v) {
    char b[32];
    if (top > 6000) snprintf(b, sizeof b, "%.0fk", v / 1000);
    else snprintf(b, sizeof b, "%.0f", v);
    return std::string(b);
  };
  PlotArea a = drawPlot("audioplot", 0, top, audioLine_, 0, nyq, audioFloorDb_, audioFloorDb_ + audioRangeDb_, wfAudio_, 0.f, (float)(top / nyq), label, step, 1.f);
  auto toX = [&](double f) { return a.p0.x + (float)(f / top) * (a.p1.x - a.p0.x); };
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->PushClipRect(a.p0, a.p1, true);
  int pitch = tuning::cwPitchHz(st_.level[civ::L_CW_PITCH]);
  if (st_.mode >= 0 && st_.width >= 0) {
    double pl, ph;
    tuning::audioPassband(st_.mode, civ::widthHz(st_.mode, st_.width), pitch, pl, ph);
    dl->AddRectFilled(ImVec2(toX(pl), a.p0.y), ImVec2(toX(ph), a.p1.y), IM_COL32(255, 255, 255, 22));
    dl->AddLine(ImVec2(toX(pl), a.p0.y), ImVec2(toX(pl), a.p1.y), IM_COL32(255, 200, 80, 150));
    dl->AddLine(ImVec2(toX(ph), a.p0.y), ImVec2(toX(ph), a.p1.y), IM_COL32(255, 200, 80, 150));
  }
  if (st_.mode == civ::CW || st_.mode == civ::CW_R) {
    float x = toX(pitch);
    dl->AddLine(ImVec2(x, a.p0.y), ImVec2(x, a.p1.y), C_AMBER, 1.f);
  }
  bool haveAudio = audio_.capturing() && !audio_.lost();
  if (!haveAudio && !audioError_.empty()) {
    ImVec2 ts = ImGui::CalcTextSize(audioError_.c_str());
    ImVec2 c((a.p0.x + a.p1.x - ts.x) / 2, (a.w0.y + a.w1.y - ts.y) / 2);
    dl->AddRectFilled(ImVec2(c.x - S(8), c.y - S(4)), ImVec2(c.x + ts.x + S(8), c.y + ts.y + S(4)), IM_COL32(0, 0, 0, 170), 4);
    dl->AddText(c, C_TEXT, audioError_.c_str());
  }
  // Where a click would take the dial, in plain terms: data modes keep theirs still.
  double target = -1;
  if (st_.mode == civ::CW || st_.mode == civ::CW_R) target = pitch;
  else if (st_.mode == civ::RTTY || st_.mode == civ::RTTY_R) target = 2125 + 85;
  else if ((st_.mode == civ::USB || st_.mode == civ::LSB) && !st_.data) target = 1500;
  if (a.hovered) {
    float mx = ImGui::GetIO().MousePos.x;
    dl->AddLine(ImVec2(mx, a.p0.y), ImVec2(mx, a.p1.y), IM_COL32(255, 255, 255, 110));
    long long rf = st_.hz > 0 ? tuning::rfFromAudio(st_.hz, st_.mode, a.x, pitch) : -1;
    char b[64];
    if (rf > 0) snprintf(b, sizeof b, "%.0f Hz  =  %s", a.x, tuning::formatHz(rf).c_str());
    else snprintf(b, sizeof b, "%.0f Hz", a.x);
    float tw = ImGui::CalcTextSize(b).x;
    float lx = std::min(mx + S(6), a.p1.x - tw - S(6));
    dl->AddRectFilled(ImVec2(lx - S(3), a.p0.y + S(2)), ImVec2(lx + tw + S(3), a.p0.y + S(20)), IM_COL32(0, 0, 0, 190), 3);
    dl->AddText(ImVec2(lx, a.p0.y + S(3)), C_AMBER, b);
  }
  dl->PopClipRect();
  if (a.hovered && st_.connected) {
    if (a.clicked && target > 0) tuneTo(tuning::dialToCentre(st_.hz, st_.mode, a.x, target));
    else if (a.clicked && st_.data) say("Data mode: the dial stays put, so your digital program keeps its place");
    if (a.wheel != 0.f) tuneTo(tuning::stepFreq(st_.hz, kSteps[stepIdx_], a.wheel > 0 ? 1 : -1, radio()));
  }
  ImGui::EndChild();
}

void App::drawCentre() {
  ImGui::BeginChild("centre", ImVec2(-S(297), 0), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  float total = ImGui::GetContentRegionAvail().y;
  if (showScope_) {
    drawScopePane();
    if (showAudio_) {
      // The grab bar between them.
      ImVec2 p = ImGui::GetCursorScreenPos();
      float w = ImGui::GetContentRegionAvail().x;
      ImGui::InvisibleButton("##splitter", ImVec2(w, S(5)));
      if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
      if (ImGui::IsItemActive()) splitFrac_ = std::max(0.15f, std::min(0.85f, splitFrac_ + ImGui::GetIO().MouseDelta.y / total));
      ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x + w / 2 - S(20), p.y + S(1.5f)), ImVec2(p.x + w / 2 + S(20), p.y + S(3.5f)), C_DIM, 1);
    }
  }
  if (showAudio_) drawAudioPane();
  ImGui::EndChild();
}

// ---------------------------------------------------------------- the right-hand tabs

// A disc whose shade depends on the direction from its centre, which is how a turned metal surface catches
// the light. ImGui has no gradients of this kind, so it's a fan of triangles with a colour at each vertex.
template <typename Shade>
static void shadedDisc(ImDrawList* dl, ImVec2 c, float r, Shade shade) {
  const int n = 96;
  const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
  dl->PrimReserve(n * 3, n + 1);
  const ImDrawIdx base = (ImDrawIdx)dl->_VtxCurrentIdx;
  float sum = 0;
  for (int i = 0; i < n; i++) sum += shade((float)(i * 2.0 * M_PI / n));
  int mid = (int)(sum / n);
  dl->PrimWriteVtx(c, uv, IM_COL32(mid, mid, mid, 255));
  for (int i = 0; i < n; i++) {
    float t = (float)(i * 2.0 * M_PI / n);
    int v = std::max(0, std::min(255, (int)shade(t)));
    dl->PrimWriteVtx(ImVec2(c.x + cosf(t) * r, c.y + sinf(t) * r), uv, IM_COL32(v, v, v, 255));
  }
  for (int i = 0; i < n; i++) {
    dl->PrimWriteIdx(base);
    dl->PrimWriteIdx((ImDrawIdx)(base + 1 + i));
    dl->PrimWriteIdx((ImDrawIdx)(base + 1 + (i + 1) % n));
  }
}

static std::string hzName(long long hz) {
  char b[32];
  if (hz >= 1000000) snprintf(b, sizeof b, "%g MHz", (double)hz / 1e6);
  else if (hz >= 1000) snprintf(b, sizeof b, "%g kHz", (double)hz / 1e3);
  else snprintf(b, sizeof b, "%lld Hz", hz);
  return b;
}

void App::drawDial() {
  // It takes what the pane has left, so the keypad and the band's presets below it still fit.
  const float d = std::max(S(84), std::min(S(170), ImGui::GetWindowHeight() - S(385)));
  const float r = d / 2;
  ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (ImGui::GetContentRegionAvail().x - d) / 2);
  ImGui::BeginDisabled(!st_.connected);
  ImGui::InvisibleButton("##dial", ImVec2(d, d));
  ImGui::EndDisabled();
  note("dial");
  const ImVec2 a = ImGui::GetItemRectMin();
  const ImVec2 c(a.x + r, a.y + r);
  ImGuiIO& io = ImGui::GetIO();
  const bool active = ImGui::IsItemActive(), hovered = ImGui::IsItemHovered();
  const bool live = st_.connected;
  const long long step = kSteps[stepIdx_] * (io.KeyShift ? 10 : 1);
  const long long clickHz = tuning::Dial::clickHz(step);

  // Held, it follows the pointer even off the dial, so a wide circle of the arm keeps tuning. A coast
  // ends if the link goes or the rig starts transmitting: the dial mustn't wander on its own then.
  int clicks = 0;
  if (active) {
    clicks = dial_.spin(io.MousePos.x - c.x, io.MousePos.y - c.y, r * 0.1f, io.DeltaTime);
  } else {
    if (!live || st_.tx) dial_.stop();
    clicks = dial_.idle(io.DeltaTime);
  }
  if (clicks && live) tuneTo(tuning::stepFreq(st_.hz, clickHz, clicks, radio()));
  if (hovered && live && io.MouseWheel != 0.f) {
    int notch = io.MouseWheel > 0 ? 1 : -1;
    dial_.nudge(notch * tuning::Dial::kClicksPerStep);
    tuneTo(tuning::stepFreq(st_.hz, step, notch, radio()));
  }
  if ((hovered || active) && live) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
  ImGui::SetItemTooltip("Click and spin, like a VFO knob. Clockwise tunes up.\nTurn slowly to fine tune. Spin fast to move further, and let go to let it run on.\nShift: ten times the rate. The wheel moves it by the step.");

  // Lit from the top left, and that light stays put as the knob turns: only the flutes and dimple move.
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float lightAt = (float)(-0.75 * M_PI);
  auto lit = [lightAt](float t) { return 0.5f + 0.5f * cosf(t - lightAt); };
  const float turned = (float)dial_.angle();
  const float skirt = r * 0.97f, cap = r * 0.70f;

  // The shadow it casts on the panel.
  dl->AddCircleFilled(ImVec2(c.x + r * 0.03f, c.y + r * 0.05f), skirt, IM_COL32(0, 0, 0, 120), 96);
  // The rubber grip.
  shadedDisc(dl, c, skirt, [&](float t) { return 22.f + 34.f * lit(t); });
  const int flutes = 44;
  for (int i = 0; i < flutes; i++) {
    float t = turned + (float)(i * 2.0 * M_PI / flutes);
    ImVec2 u(cosf(t), sinf(t));
    ImVec2 from(c.x + u.x * r * 0.79f, c.y + u.y * r * 0.79f), to(c.x + u.x * skirt, c.y + u.y * skirt);
    // A groove: its dark floor, and a lit edge beside it that's brighter facing the light.
    dl->AddLine(from, to, IM_COL32(6, 6, 7, 255), S(2.2f));
    ImVec2 side(-u.y * S(1.6f), u.x * S(1.6f));
    int e = 44 + (int)(70 * lit(t));
    dl->AddLine(ImVec2(from.x + side.x, from.y + side.y), ImVec2(to.x + side.x, to.y + side.y), IM_COL32(e, e, e + 3, 255), S(1));
  }
  dl->AddCircle(c, skirt, IM_COL32(10, 10, 11, 255), 96, S(1.5f));
  // The shoulder between grip and cap.
  shadedDisc(dl, c, r * 0.78f, [&](float t) { return 18.f + 74.f * lit(t); });
  shadedDisc(dl, c, r * 0.74f, [&](float t) { return 92.f - 72.f * lit(t); });
  // The cap: spun aluminium, whose fine circular grain shows two bright wedges opposite each other.
  shadedDisc(dl, c, cap, [&](float t) {
    float sheen = powf(fabsf(cosf(t - lightAt)), 7.f);
    float second = powf(fabsf(cosf(t - lightAt - 1.1f)), 18.f);
    return 104.f + 108.f * sheen + 34.f * second;
  });
  dl->AddCircle(c, cap, IM_COL32(236, 238, 240, 150), 96, S(1));
  dl->AddCircle(c, cap - S(1.5f), IM_COL32(40, 42, 46, 110), 96, S(1));

  // The finger dimple, starting at the top. It's a hollow, so it's lit the opposite way to the rest.
  const float t = turned - (float)M_PI / 2;
  const float well = r * 0.17f;
  const ImVec2 dimple(c.x + cosf(t) * r * 0.45f, c.y + sinf(t) * r * 0.45f);
  shadedDisc(dl, dimple, well, [&](float a2) { return 150.f - 118.f * lit(a2); });
  shadedDisc(dl, dimple, well * 0.72f, [&](float a2) { return 66.f + 44.f * (1.f - lit(a2)); });
  dl->AddCircle(dimple, well, IM_COL32(30, 31, 34, 170), 40, S(1));

  // Pressed, it sits a little darker in the hand. Off the air, it's dimmed like any other disabled control.
  if (!live) dl->AddCircleFilled(c, r, IM_COL32(16, 18, 23, 150), 96);
  else if (active) dl->AddCircleFilled(c, cap, IM_COL32(0, 0, 0, 26), 96);

  std::string rate = "1 turn = " + hzName(step * tuning::Dial::kClicksPerStep);
  ImVec2 ts = ImGui::CalcTextSize(rate.c_str());
  ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (ImGui::GetContentRegionAvail().x - ts.x) / 2);
  ImGui::TextDisabled("%s", rate.c_str());
}

void App::drawKeypad() {
  drawDial();
  ImGui::TextDisabled(radio().topHz() > 1000000000 ? "Or just type: 145.5  433.5  1296.2  +5" : "Or just type: 14.074  7  198k  +5");
  ImGui::PushFont(fMid_);
  float gap = ImGui::GetStyle().ItemSpacing.x;
  float w = (ImGui::GetContentRegionAvail().x - gap * 3) / 4, h = S(28);
  const char* rows[4][4] = {{"7", "8", "9", "<"}, {"4", "5", "6", "kHz"}, {"1", "2", "3", "MHz"}, {".", "0", "+/-", "Clr"}};
  for (int r = 0; r < 4; r++)
    for (int c = 0; c < 4; c++) {
      if (c) ImGui::SameLine();
      const char* k = rows[r][c];
      if (panelButton(k, ImVec2(w, h))) {
        if (!strcmp(k, "<")) {
          if (!entry_.empty()) entry_.pop_back();
        } else if (!strcmp(k, "Clr")) {
          entry_.clear();
        } else if (!strcmp(k, "kHz")) {
          if (!entry_.empty()) {
            entry_ += "k";
            commitEntry();
          }
        } else if (!strcmp(k, "MHz")) {
          if (!entry_.empty()) {
            entry_ += "m";
            commitEntry();
          }
        } else if (!strcmp(k, "+/-")) {
          if (entry_.empty() || entry_[0] == '-') entry_ = "+" + (entry_.empty() ? "" : entry_.substr(1));
          else if (entry_[0] == '+') entry_[0] = '-';
          else entry_ = "+" + entry_;
        } else {
          entry_ += k;
        }
      }
      note(std::string("key:") + k);
    }
  ImGui::BeginDisabled(entry_.empty() || !st_.connected);
  if (actionButton("ENTER", ImVec2(-1, S(30)), V_ACCENT, V_ACCENT_HI, "key:enter")) commitEntry();
  ImGui::EndDisabled();
  ImGui::PopFont();
  ImGui::Spacing();

  int here = plan_.bandOf(st_.hz);
  if (here >= 0) {
    const bands::Band& band = plan_.bands[(size_t)here];
    ImGui::TextColored(ImColor(C_ACCENT), "ON %s", band.title().c_str());
    if (const bands::Segment* seg = band.segmentAt(st_.hz)) {
      ImGui::SameLine();
      ImGui::TextColored(ImColor(segmentColour(seg->use, 255)), "%s", portionName(*seg).c_str());
      if (seg->label != portionName(*seg)) ImGui::TextDisabled("%s", seg->label.c_str());
    } else {
      ImGui::TextColored(ImColor(C_RED), "Between this band's blocks");
    }
    ImGui::Separator();
    ImGui::BeginDisabled(!st_.connected);
    for (const bands::Marker& p : band.markers)
      if (p.preset()) {
        char b[64];
        snprintf(b, sizeof b, "%-5s  %s", p.label.c_str(), tuning::formatHz(p.hz).c_str());
        if (panelButton(b, ImVec2(-1, 0))) goMarker(p);
      }
    ImGui::EndDisabled();
  } else if (st_.hz > 0) {
    ImGui::TextDisabled("Outside the amateur bands");
    ImGui::TextDisabled("in the %s plan", plan_.name.c_str());
  }
}

void App::drawMemories() {
  ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - S(56));
  ImGui::InputTextWithHint("##memname", "Name", memName_, sizeof memName_);
  note("memname");
  ImGui::SameLine();
  ImGui::BeginDisabled(!st_.connected);
  if (panelButton("Save", ImVec2(-1, 0))) {
    store::Memory m;
    m.name = memName_[0] ? memName_ : utcNow("%H%MZ");
    m.hz = st_.hz, m.mode = st_.mode, m.data = st_.data, m.filter = st_.filter ? st_.filter : 1;
    memories_.push_back(m);
    store::saveMemories(memPath_, memories_);
    memName_[0] = 0;
  }
  note("memsave");
  ImGui::EndDisabled();
  ImGui::SetNextItemWidth(-1);
  ImGui::InputTextWithHint("##memfilter", "Filter", memFilter_, sizeof memFilter_);
  int del = -1;
  if (ImGui::BeginTable("mems", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerH, ImVec2(0, 0))) {
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("MHz", ImGuiTableColumnFlags_WidthFixed, S(72));
    ImGui::TableSetupColumn("Mode", ImGuiTableColumnFlags_WidthFixed, S(44));
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, S(14));
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableHeadersRow();
    std::string filter = memFilter_;
    for (char& c : filter) c = (char)tolower((unsigned char)c);
    for (int i = 0; i < (int)memories_.size(); i++) {
      const store::Memory& m = memories_[(size_t)i];
      if (!filter.empty()) {
        std::string hay = m.name;
        for (char& c : hay) c = (char)tolower((unsigned char)c);
        if (hay.find(filter) == std::string::npos) continue;
      }
      ImGui::PushID(i);
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      if (ImGui::Selectable(m.name.c_str(), m.hz == st_.hz, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap) && st_.connected) {
        if (tuneTo(m.hz)) rig_.setMode(m.mode, m.data, m.filter);
      }
      note("mem:" + std::to_string(i));
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(tuning::formatHz(m.hz).c_str());
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(modeLabel(m.mode, m.data).c_str());
      ImGui::TableNextColumn();
      ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, 0));
      if (panelButton("x", ImVec2(0, 0), ImGuiButtonFlags_AlignTextBaseLine)) del = i;
      ImGui::PopStyleVar();
      ImGui::PopID();
    }
    ImGui::EndTable();
  }
  if (del >= 0) {
    memories_.erase(memories_.begin() + del);
    store::saveMemories(memPath_, memories_);
  }
}

void App::drawLog() {
  // Upper-case as you type: callsigns always are.
  for (char* p = logCall_; *p; p++) *p = (char)toupper((unsigned char)*p);
  ImGui::SetNextItemWidth(-1);
  if (logFocusCall_) {
    ImGui::SetKeyboardFocusHere();
    logFocusCall_ = false;
  }
  bool enter = ImGui::InputTextWithHint("##call", "Callsign", logCall_, sizeof logCall_, ImGuiInputTextFlags_EnterReturnsTrue);
  if (logCall_[0]) {
    int worked = 0;
    for (const store::LogEntry& e : log_) worked += e.call == logCall_;
    if (worked) ImGui::TextColored(ImColor(C_AMBER), "Worked %d time%s before", worked, worked == 1 ? "" : "s");
  }
  float w = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) / 2;
  ImGui::SetNextItemWidth(w);
  ImGui::InputTextWithHint("##sent", "RST sent", logSent_, sizeof logSent_);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(w);
  ImGui::InputTextWithHint("##rcvd", "RST rcvd", logRcvd_, sizeof logRcvd_);
  ImGui::SetNextItemWidth(-1);
  ImGui::InputTextWithHint("##lname", "Name", logName_, sizeof logName_);
  ImGui::SetNextItemWidth(-1);
  enter |= ImGui::InputTextWithHint("##lnotes", "Notes", logNotes_, sizeof logNotes_, ImGuiInputTextFlags_EnterReturnsTrue);
  ImGui::TextDisabled("%s  %s  %s UTC", tuning::formatHz(st_.hz).c_str(), modeLabel(st_.mode, st_.data).c_str(), utcNow("%H:%M").c_str());
  ImGui::BeginDisabled(!logCall_[0]);
  bool add = panelButton("Log it", ImVec2(w, 0));
  ImGui::EndDisabled();
  if ((add || enter) && logCall_[0]) {
    store::LogEntry e;
    e.utc = utcNow("%Y-%m-%d %H:%M");
    e.call = logCall_, e.hz = st_.hz > 0 ? st_.hz : 0, e.mode = modeLabel(st_.mode, st_.data);
    e.rstSent = logSent_, e.rstRcvd = logRcvd_, e.name = logName_, e.notes = logNotes_;
    log_.push_back(e);
    if (!store::appendLog(logPath_, e)) say("Couldn't save the log entry to " + logPath_);
    logCall_[0] = logName_[0] = logNotes_[0] = 0;
    logFocusCall_ = true;
  }
  ImGui::SameLine();
  if (panelButton("Export ADIF", ImVec2(w, 0))) {
    std::string path = store::dataDir() + "log.adi";
    if (store::exportAdif(path, log_, callsign_)) say("Exported " + std::to_string(log_.size()) + " QSOs to " + path);
    else say("Couldn't write " + path);
  }
  if (ImGui::BeginTable("log", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerH, ImVec2(0, 0))) {
    ImGui::TableSetupColumn("UTC", ImGuiTableColumnFlags_WidthFixed, S(92));
    ImGui::TableSetupColumn("Call", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("kHz", ImGuiTableColumnFlags_WidthFixed, S(58));
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableHeadersRow();
    for (int i = (int)log_.size() - 1; i >= 0; i--) {
      const store::LogEntry& e = log_[(size_t)i];
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(e.utc.size() > 5 ? e.utc.c_str() + 5 : e.utc.c_str());
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(e.call.c_str());
      if (ImGui::IsItemHovered() && (!e.notes.empty() || !e.name.empty())) ImGui::SetTooltip("%s %s  %s/%s  %s", e.name.c_str(), e.mode.c_str(), e.rstSent.c_str(), e.rstRcvd.c_str(), e.notes.c_str());
      ImGui::TableNextColumn();
      ImGui::Text("%.1f", (double)e.hz / 1000.0);
    }
    ImGui::EndTable();
  }
}

void App::drawCwVoice() {
  auto expand = [&](std::string s) {
    size_t at;
    while ((at = s.find("{CALL}")) != std::string::npos) s.replace(at, 6, callsign_);
    return s;
  };
  auto send = [&](const std::string& text) {
    if (!st_.txArmed) say("Transmit isn't armed. Press ARM TX first.");
    else if (text.find("{CALL}") == std::string::npos && expand(text).empty()) say("Nothing to send");
    else if (st_.sw[civ::S_BKIN] == 0) {
      rig_.sendCw(expand(text));
      say("Break-in is off, so the rig will key the sidetone only. Turn BK-IN on to transmit.");
    } else rig_.sendCw(expand(text));
  };
  ImGui::TextColored(ImColor(C_ACCENT), "CW KEYER");
  ImGui::Separator();
  ImGui::TextDisabled("Mode must be CW. Sends through the rig's keyer.");
  ImGui::SetNextItemWidth(-1);
  if (ImGui::InputTextWithHint("##cw", "Type and press Enter to send", cwLine_, sizeof cwLine_, ImGuiInputTextFlags_EnterReturnsTrue) && cwLine_[0]) {
    send(cwLine_);
    cwLine_[0] = 0;
    ImGui::SetKeyboardFocusHere(-1);
  }
  for (int i = 0; i < 5; i++) {
    ImGui::PushID(i);
    char label[8];
    snprintf(label, sizeof label, "M%d", i + 1);
    if (panelButton(label, ImVec2(S(34), 0))) send(macros_[i]);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##m", macros_[i], sizeof macros_[i]);
    ImGui::PopID();
  }
  if (actionButton("STOP SENDING", ImVec2(-1, 0), V_RED, V_RED_HI)) rig_.stopCw();
  ImGui::TextDisabled("{CALL} is your callsign, from Settings.");

  ImGui::Spacing();
  ImGui::TextColored(ImColor(C_ACCENT), "VOICE MEMORY");
  ImGui::Separator();
  ImGui::TextDisabled("Plays what's recorded on the rig. Mode must be SSB, AM or FM.");
  float w = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 3) / 4;
  for (int i = 1; i <= 8; i++) {
    if ((i - 1) % 4) ImGui::SameLine();
    char b[8];
    snprintf(b, sizeof b, "T%d", i);
    if (panelButton(b, ImVec2(w, 0))) {
      if (!st_.txArmed) say("Transmit isn't armed. Press ARM TX first.");
      else rig_.playVoice(i);
    }
  }
  if (actionButton("STOP PLAYING", ImVec2(-1, 0), V_RED, V_RED_HI)) rig_.playVoice(0);
}

void App::drawRightTabs() {
  ImGui::BeginChild("right", ImVec2(0, 0), ImGuiChildFlags_Borders);
  if (ImGui::BeginTabBar("tabs")) {
    auto tab = [&](const char* name, int i) {
      return ImGui::BeginTabItem(name, nullptr, frameCount_ == 1 && opt_.tab == i ? ImGuiTabItemFlags_SetSelected : 0);
    };
    if (tab("Keypad", 0)) {
      drawKeypad();
      ImGui::EndTabItem();
    }
    if (tab("Memories", 1)) {
      drawMemories();
      ImGui::EndTabItem();
    }
    if (tab("Log", 2)) {
      drawLog();
      ImGui::EndTabItem();
    }
    if (radio().hasTransmit && tab("CW / Voice", 3)) {
      drawCwVoice();
      ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
  }
  ImGui::EndChild();
}

// ---------------------------------------------------------------- the rest of the window

void App::drawStatus() {
  ImGui::AlignTextToFramePadding();
  ImU32 dot = st_.connected ? C_GREEN : st_.portOpen ? C_AMBER : C_RED;
  ImVec2 p = ImGui::GetCursorScreenPos();
  ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + S(5), p.y + ImGui::GetFrameHeight() / 2), S(4), dot);
  ImGui::Dummy(ImVec2(S(10), 1));
  ImGui::SameLine();
  ImGui::AlignTextToFramePadding();
  if (opt_.demo) {
    ImGui::TextColored(ImColor(C_AMBER), "DEMO: a simulated radio. Nothing is connected.");
  } else if (opt_.noRig) {
    ImGui::TextDisabled("Audio only: leaving the serial port alone");
  } else if (st_.connected) {
    ImGui::Text("%s  %s  %d baud", radio().name, port_.c_str(), baud_);
  } else if (!st_.portError.empty()) {
    bool busy = st_.portError.find("in use") != std::string::npos;
    ImGui::TextColored(ImColor(C_RED), "%s%s", st_.portError.c_str(), busy ? ". Close the other program: a port has one owner." : "");
  } else if (st_.portOpen && st_.strangerId >= 0) {
    ImGui::TextColored(ImColor(C_AMBER), "A radio answered at %02Xh, reporting %02Xh. It isn't an IC-7300, IC-9700 or IC-R8600: to try it, put %02X in Settings as the CI-V address.",
                       st_.strangerAddr, st_.strangerId, st_.strangerAddr);
  } else if (st_.portOpen) {
    ImGui::TextColored(ImColor(C_AMBER), "%s open, but no answer: is the rig on, at %d baud? A 9700 shows two ports: use its Enhanced one.", port_.c_str(), baud_);
  } else {
    ImGui::TextDisabled("Connecting...");
  }
  double now = glfwGetTime();
  if (st_.lastErrorSeq != lastErrorSeq_) {
    lastErrorSeq_ = st_.lastErrorSeq;
    // Main and sub can't share a band, so a band change can be refused for that. It may not be the reason.
    bool maybeSub = radio().hasMainSub && st_.lastError.find("frequency") != std::string::npos;
    if (!st_.lastError.empty()) say(st_.lastError + (maybeSub ? ". Is the SUB band on that band?" : ""));
  }
  if (now < statusUntil_) {
    ImGui::SameLine(0, S(20));
    ImGui::TextColored(ImColor(C_AMBER), "%s", status_.c_str());
  }
  if (clockSync_ && clockSync_->done) {
    clockResult_ = clockSync_->report.message;
    say(clockResult_);
    clockSync_.reset();
  }
  std::string right = utcNow("%H:%M:%S UTC");
  float rw = ImGui::CalcTextSize(right.c_str()).x, bw = S(58);
  ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - rw - bw - ImGui::GetStyle().ItemSpacing.x);
  ImGui::BeginDisabled(clockSync_ != nullptr);
  if (toggle(clockSync_ ? "Wait##clocksync" : "Sync##clocksync", false, ImVec2(bw, 0), "clocksync")) startClockSync();
  ImGui::EndDisabled();
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    ImGui::SetTooltip("%s%s%s",
                      opt_.demo ? "Checks this computer's clock against internet time (NTP).\nIn the demo it won't change it."
                                : "Sets this computer's clock from internet time (NTP), for FT8, FT4 and the other timed modes.\nWindows may ask for permission.",
                      clockResult_.empty() ? "" : "\n\nLast time: ", clockResult_.c_str());
  }
  ImGui::SameLine();
  ImGui::TextUnformatted(right.c_str());
}

void App::startClockSync() {
  auto job = std::make_shared<ClockSync>();
  clockSync_ = job;
  if (!opt_.selfTest.empty()) {  // a scripted run has no business on the network
    job->report.ok = true;
    job->report.message = "Self-test: no time server asked.";
    job->done = true;
    return;
  }
  bool change = !opt_.demo;  // the demo touches nothing real
  std::thread([job, change] {
    job->report = clocksync::jog(change);
    job->done = true;
  }).detach();
}

void App::drawMenu() {
  if (!ImGui::BeginMenuBar()) return;
  if (ImGui::BeginMenu("Rig")) {
    if (ImGui::MenuItem("Settings...")) openSettings_ = true;
    ImGui::Separator();
    if (ImGui::MenuItem("Power on")) rig_.powerOn();
    if (ImGui::MenuItem("Power off...", nullptr, false, st_.connected)) openPowerOff_ = true;
    ImGui::Separator();
    if (ImGui::MenuItem("Open data folder")) store::openFolder(store::dataDir());
    if (ImGui::MenuItem("Quit")) glfwSetWindowShouldClose(win_, 1);
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("View")) {
    ImGui::MenuItem("Band scope", nullptr, &showScope_);
    ImGui::MenuItem("Audio waterfall", nullptr, &showAudio_);
    ImGui::MenuItem("Band plan on the scope", nullptr, &showBandPlan_);
    ImGui::EndMenu();
  }
  drawStationsMenu();
  if (ImGui::BeginMenu("Help")) {
    if (ImGui::MenuItem("Keys and rig set-up...")) openHelp_ = true;
    ImGui::EndMenu();
  }
  ImGui::EndMenuBar();
}

void App::drawSettings() {
  if (openSettings_) {
    ports_ = listSerialPorts();
    inDevs_ = audio_.captureDevices();
    outDevs_ = audio_.playbackDevices();
    ImGui::OpenPopup("Settings");
    openSettings_ = false;
  }
  ImGui::SetNextWindowSize(ImVec2(S(1000), 0));
  if (!ImGui::BeginPopupModal("Settings", nullptr, ImGuiWindowFlags_NoResize)) return;
  // Two columns: the band plan and station notes would make one column too tall.
  if (!ImGui::BeginTable("settingsColumns", 2, ImGuiTableFlags_SizingStretchSame)) {
    ImGui::EndPopup();
    return;
  }
  ImGui::TableNextColumn();
  ImGui::TextColored(ImColor(C_ACCENT), "THE RIG");
  ImGui::Separator();
  ImGui::SetNextItemWidth(S(340));
  if (ImGui::BeginCombo("Port", port_.empty() ? "(choose)" : port_.c_str())) {
    for (const SerialPortInfo& p : ports_) {
      std::string l = p.name + (p.desc.empty() ? "" : "  " + p.desc);
      if (ImGui::Selectable(l.c_str(), p.name == port_)) port_ = p.name;
    }
    ImGui::EndCombo();
  }
  ImGui::SetNextItemWidth(S(120));
  if (ImGui::BeginCombo("Baud", std::to_string(baud_).c_str())) {
    for (int b : {115200, 57600, 38400, 19200, 9600, 4800})
      if (ImGui::Selectable(std::to_string(b).c_str(), b == baud_)) baud_ = b;
    ImGui::EndCombo();
  }
  static const char* const kRadioChoices[] = {"Find out by asking it", "IC-7300", "IC-9700", "IC-R8600"};
  ImGui::SetNextItemWidth(S(200));
  ImGui::Combo("Radio", &radioChoice_, kRadioChoices, 4);
  ImGui::SetItemTooltip("It normally works this out. Choose one if it gets it wrong, as could happen if the radio's CI-V address has been changed.");
  ImGui::SetNextItemWidth(S(60));
  ImGui::InputText("CI-V address (hex)", addrText_, sizeof addrText_, ImGuiInputTextFlags_CharsHexadecimal);
  ImGui::SetItemTooltip("Tried first. If nothing answers there, it tries 94h, A2h, 96h and the broadcast address.");
  ImGui::TextDisabled("The scope stream needs 115200.");

  ImGui::Spacing();
  ImGui::TextColored(ImColor(C_ACCENT), "SOUND");
  ImGui::Separator();
  ImGui::SetNextItemWidth(S(340));
  if (ImGui::BeginCombo("From the rig", audioIn_.empty() ? "(find the USB Audio CODEC)" : audioIn_.c_str())) {
    if (ImGui::Selectable("(find the USB Audio CODEC)", audioIn_.empty())) audioIn_.clear();
    for (const std::string& d : inDevs_)
      if (ImGui::Selectable(d.c_str(), d == audioIn_)) audioIn_ = d;
    ImGui::EndCombo();
  }
  ImGui::SetNextItemWidth(S(340));
  if (ImGui::BeginCombo("Listen on", audioOut_.empty() ? "(system default)" : audioOut_.c_str())) {
    if (ImGui::Selectable("(system default)", audioOut_.empty())) audioOut_.clear();
    for (const std::string& d : outDevs_)
      if (ImGui::Selectable(d.c_str(), d == audioOut_)) audioOut_ = d;
    ImGui::EndCombo();
  }

  ImGui::Spacing();
  ImGui::TextColored(ImColor(C_ACCENT), "YOU");
  ImGui::Separator();
  ImGui::SetNextItemWidth(S(160));
  ImGui::InputText("Callsign", callsign_, sizeof callsign_, ImGuiInputTextFlags_CharsUppercase);
  ImGui::SetNextItemWidth(S(160));
  ImGui::SliderInt("PTT time limit", &txLimit_, 5, 600, "%d s");
  ImGui::SetItemTooltip("PTT from this program lets go by itself after this long");

  ImGui::TableNextColumn();
  ImGui::TextColored(ImColor(C_ACCENT), "BAND PLAN");
  ImGui::Separator();
  ImGui::SetNextItemWidth(S(340));
  if (ImGui::BeginCombo("Country or region", plan_.name.c_str())) {
    std::string chosen;
    for (const bands::Plan& p : plans_.plans)
      if (ImGui::Selectable(p.name.c_str(), p.id == plan_.id)) chosen = p.id;
    ImGui::EndCombo();
    if (!chosen.empty()) choosePlan(chosen);
  }
  ImGui::PushStyleColor(ImGuiCol_Text, C_DIM);
  ImGui::TextWrapped("%s", plan_.source.c_str());
  if (!plan_.note.empty()) ImGui::TextWrapped("%s", plan_.note.c_str());
  ImGui::PopStyleColor();
  static const bands::Library kBuiltin = bands::builtinPlans();
  const bands::Plan* builtin = kBuiltin.find(plan_.id);
  if (builtin && builtin->revision > plan_.revision)
    ImGui::TextColored(ImColor(C_AMBER), "This program has a newer copy of this plan (%s). Reset uses it.", builtin->revision.c_str());
  for (const std::string& e : plans_.errors) {
    ImGui::PushStyleColor(ImGuiCol_Text, C_RED);
    ImGui::TextWrapped("%s", e.c_str());
    ImGui::PopStyleColor();
  }
  if (panelButton("Open plans folder")) {
    bands::installPlans(plansDir_);
    store::openFolder(plansDir_);
  }
  ImGui::SetItemTooltip("Each plan is a JSON file you can edit. README.txt there explains the format.");
  ImGui::SameLine();
  if (panelButton("Reload")) reloadPlans();
  ImGui::SetItemTooltip("Reads the files again, after you've edited one");
  ImGui::SameLine();
  ImGui::BeginDisabled(!builtin);
  if (panelButton("Reset this plan")) {
    if (bands::resetPlan(plansDir_, plan_.id)) reloadPlans();
    else say("Can't write to " + plansDir_);
  }
  ImGui::SetItemTooltip("Puts the built-in copy back. Your file is kept as .json.bak");
  ImGui::EndDisabled();

  ImGui::Spacing();
  ImGui::TextColored(ImColor(C_ACCENT), "STATIONS");
  ImGui::Separator();
  ImGui::PushStyleColor(ImGuiCol_Text, C_DIM);
  ImGui::TextWrapped("Shown on the scope whichever band plan is chosen. See the Stations menu.");
  for (const bands::StationFile& f : stations_.files) {
    ImGui::TextWrapped("%s (%d stations, revision %s). %s", f.name.c_str(), (int)f.stations.size(), f.revision.c_str(), f.note.c_str());
    ImGui::SetItemTooltip("%s", f.source.c_str());
  }
  ImGui::PopStyleColor();
  for (const std::string& e : stations_.errors) {
    ImGui::PushStyleColor(ImGuiCol_Text, C_RED);
    ImGui::TextWrapped("%s", e.c_str());
    ImGui::PopStyleColor();
  }
  if (panelButton("Open stations folder")) {
    bands::installStations(stationsDir_);
    store::openFolder(stationsDir_);
  }
  ImGui::SetItemTooltip("Each list is a JSON file you can edit, and you can add your own. README.txt there explains the format. Reload above reads them again.");
  ImGui::SameLine();
  if (panelButton("Reset station list")) {
    bool ok = bands::resetStations(stationsDir_, "worldwide") && bands::resetStations(stationsDir_, "broadcast");
    if (ok && !radio().hasTransmit) ok = bands::resetStations(stationsDir_, "receiver");
    if (ok) reloadPlans();
    else say("Can't write to " + stationsDir_);
  }
  ImGui::SetItemTooltip(radio().hasTransmit ? "Puts the built-in worldwide and broadcaster lists back. Your files are kept as .json.bak"
                                            : "Puts the built-in worldwide, broadcaster and receiver lists back. Your files are kept as .json.bak");
  ImGui::EndTable();

  ImGui::Spacing();
  ImGui::Separator();
  if (panelButton("Apply and reconnect", ImVec2(S(170), 0))) {
    addr_ = (int)strtol(addrText_, nullptr, 16);
    if (addr_ <= 0 || addr_ > 0xDF) addr_ = 0x94;
    snprintf(addrText_, sizeof addrText_, "%02X", addr_);
    rig_.setTxLimit(txLimit_);
    if (!opt_.demo && !opt_.noRig) connect();
    audio_.stopCapture();
    audioRetryAt_ = 0;
    saveSettings();
    ImGui::CloseCurrentPopup();
  }
  ImGui::SameLine();
  if (panelButton("Close", ImVec2(S(90), 0))) {
    rig_.setTxLimit(txLimit_);
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndPopup();
}

void App::drawHelp() {
  if (openHelp_) {
    ImGui::OpenPopup("Keys and rig set-up");
    openHelp_ = false;
  }
  ImGui::SetNextWindowSize(ImVec2(S(640), 0));
  if (ImGui::BeginPopupModal("Keys and rig set-up", nullptr, ImGuiWindowFlags_NoResize)) {
    ImGui::TextColored(ImColor(C_ACCENT), "ON THE RIG  (MENU > SET > Connectors > CI-V)");
    ImGui::Separator();
    ImGui::BulletText("CI-V USB Port: Unlink from [REMOTE]");
    ImGui::BulletText("CI-V USB Baud Rate: 115200");
    ImGui::BulletText("CI-V USB Echo Back: OFF  (ON works too)");
    ImGui::BulletText("CI-V Address: leave it at the default, 94h on an IC-7300 or A2h on an IC-9700");
    ImGui::BulletText("USB AF Output Level (Connectors): raise it until the level bar sits below red");
    ImGui::BulletText("USB SEND and USB Keying (Connectors > USB SEND/Keying): OFF");
    ImGui::Spacing();
    ImGui::TextColored(ImColor(C_ACCENT), "KEYS");
    ImGui::Separator();
    if (ImGui::BeginTable("keys", 2)) {
      static const char* rows[][2] = {
          {"Digits", "Start a frequency: 14.074, 14074, 7, 198k, +5. Enter goes there, Esc cancels."},
          {"Left, Right", "Tune by the step. Hold Shift for ten times that."},
          {"Up, Down", "Change the step"},
          {"Page Up, Page Down", "Next or previous band"},
          {"Ctrl+F", "Find a station by name"},
          {"F1 to F8", "LSB, USB, CW, CW-R, AM, FM, RTTY, RTTY-R"},
          {"F9, F10", "Data mode, next filter"},
          {"F11, F12", "Split, swap VFOs"},
          {"Ctrl+Space", "PTT on or off, once armed"},
          {"Tuning dial", "Click and spin it. One turn is ten steps. Spin fast to move further; let go and it runs on."},
          {"Wheel", "On a digit tunes that digit. On a waterfall or the dial, by the step. On a slider, moves it."},
          {"Click a digit", "Top half up, bottom half down"},
          {"Click the band scope", "Tune there. Right-click for split and memories."},
          {"Click the audio waterfall", "Moves that signal to the pitch (not in data modes)"},
          {"Right-click a band", "CW, FT8, FT4, WSPR, SSB frequencies"},
      };
      ImGui::TableSetupColumn("k", ImGuiTableColumnFlags_WidthFixed, S(170));
      for (auto& r : rows) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(r[0]);
        ImGui::TableNextColumn();
        ImGui::TextWrapped("%s", r[1]);
      }
      ImGui::EndTable();
    }
    ImGui::Spacing();
    if (panelButton("Close", ImVec2(S(90), 0))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }

  if (openPowerOff_) {
    ImGui::OpenPopup("Power off the rig?");
    openPowerOff_ = false;
  }
  if (ImGui::BeginPopupModal("Power off the rig?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::TextUnformatted("The rig will switch off. Rig > Power on brings it back while its USB stays powered.");
    if (actionButton("Power off", ImVec2(S(110), 0), V_RED, V_RED_HI)) {
      rig_.powerOff();
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (panelButton("Cancel", ImVec2(S(90), 0))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }
}

void App::frame() {
  frameCount_++;
  st_ = rig_.state();
  if (st_.radio != planRadio_) {
    // A receiver has more bands and stations than a transceiver would want to see.
    planRadio_ = st_.radio;
    choosePlan(plan_.id);
    stations_ = allStations_.forRadio(!radio().hasTransmit);
    stationHitsStale_ = true;
  }
  if (st_.connected && st_.radio != seenRadio_) {
    seenRadio_ = st_.radio;
    rects_.clear();  // the self-test's record of what's on screen: buttons are about to come and go
    say(std::string("This is an ") + seenRadio_->name + (seenRadio_->hasTransmit ? "" : ", a receiver: it can't transmit"));
    glfwSetWindowTitle(win_, windowTitle().c_str());
  }
  if (opt_.demo) {
    demoAudio_.dialHz = st_.hz > 0 ? st_.hz : 14074000;
    demoAudio_.mode = st_.mode >= 0 ? st_.mode : civ::USB;
    demoAudio_.pitch = tuning::cwPitchHz(st_.level[civ::L_CW_PITCH]);
  }
  keepAudioOpen();
  audio_.setMonitorMuted(st_.tx);
  pumpAudio();
  pumpScope();
  rememberBand();

  ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(vp->WorkPos);
  ImGui::SetNextWindowSize(vp->WorkSize);
  ImGui::Begin("##main", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_MenuBar |
                   ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse);
  drawMenu();
  handleKeys();
  drawHeader();
  drawBandRow();
  float statusH = ImGui::GetFrameHeightWithSpacing();
  ImGui::BeginChild("middle", ImVec2(0, -statusH), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  drawControls();
  ImGui::SameLine();
  drawCentre();
  ImGui::SameLine();
  drawRightTabs();
  ImGui::EndChild();
  drawStatus();
  drawSettings();
  if (!opt_.find.empty() && frameCount_ == 2) {
    snprintf(stationQuery_, sizeof stationQuery_, "%s", opt_.find.c_str());
    stationSearchOpen_ = true;
  }
  drawStationSearch();
  drawHelp();
  ImGui::End();
}

// ---------------------------------------------------------------- self-test: real clicks and keys, through ImGui

void App::expect(const std::string& what, bool ok) {
  results_.push_back(std::string(ok ? "ok    " : "FAIL  ") + what);
  if (!ok) testFails_++;
}

void App::click(const std::string& name, float fx, float fy, int button) {
  inject_.push_back([this, name, fx, fy](ImGuiIO& io) {
    auto it = rects_.find(name);
    if (it == rects_.end()) {
      expect("no such control: " + name, false);
      return;
    }
    ImVec2 a = it->second.Min, b = it->second.Max;
    io.AddMousePosEvent(a.x + (b.x - a.x) * fx, a.y + (b.y - a.y) * fy);
  });
  inject_.push_back([](ImGuiIO&) {});  // a frame to hover
  inject_.push_back([button](ImGuiIO& io) { io.AddMouseButtonEvent(button, true); });
  inject_.push_back([button](ImGuiIO& io) { io.AddMouseButtonEvent(button, false); });
}

void App::typeText(const std::string& s) {
  for (char c : s) inject_.push_back([c](ImGuiIO& io) { io.AddInputCharacter((unsigned int)c); });
}

void App::pressKey(ImGuiKey k) {
  inject_.push_back([k](ImGuiIO& io) { io.AddKeyEvent(k, true); });
  inject_.push_back([k](ImGuiIO& io) { io.AddKeyEvent(k, false); });
}

void App::buildSelfTest() {
  auto step = [&](int wait, std::function<void()> fn) { steps_.push_back({wait, std::move(fn)}); };
  const int settle = 45;  // frames for a command to reach the sim and come back
  auto hoverOut = [this] { inject_.push_back([](ImGuiIO& io) { io.AddMousePosEvent(-1000, -1000); }); };

  step(120, [this] { expect("rig connected", st_.connected && st_.hz > 0); });

  // ---- punching in frequencies, as MHz, kHz, Hz, relative
  step(1, [this] { typeText("7074"); pressKey(ImGuiKey_Enter); });
  step(settle, [this] { expect("typed 7074 + Enter goes to 7.074 MHz", sim_->hz() == 7074000); });
  step(1, [this] { typeText("14.2"); pressKey(ImGuiKey_Enter); });
  step(settle, [this] { expect("typed 14.2 + Enter goes to 14.2 MHz", sim_->hz() == 14200000); });
  step(1, [this] { typeText("+5"); pressKey(ImGuiKey_KeypadEnter); });
  step(settle, [this] { expect("typed +5 goes up 5 kHz", sim_->hz() == 14205000); });
  step(1, [this] { typeText("999999999"); pressKey(ImGuiKey_Enter); });
  step(settle, [this] {
    expect("impossible frequency refused", sim_->hz() == 14205000 && !entry_.empty());
    pressKey(ImGuiKey_Escape);
  });
  step(10, [this] { expect("Esc clears the entry", entry_.empty()); });
  step(1, [this] { typeText("5"); pressKey(ImGuiKey_Escape); });
  step(settle, [this] { expect("Esc cancels without tuning", sim_->hz() == 14205000); });

  // ---- keypad
  step(1, [this] { click("key:3"); click("key:."); click("key:5"); click("key:7"); click("key:3"); click("key:MHz"); });
  step(settle, [this] { expect("keypad 3.573 MHz", sim_->hz() == 3573000); });
  step(1, [this] { click("key:7"); click("key:0"); click("key:7"); click("key:4"); click("key:enter"); });
  step(settle, [this] { expect("keypad 7074 ENTER", sim_->hz() == 7074000); });

  // ---- bands: first visit goes home; coming back returns to where you were
  step(1, [this] { click("band:20"); });
  step(settle, [this] { expect("band 20 goes to the band", plan_.bandOf(sim_->hz()) == 5); });
  step(1, [this] { click("band:40"); });
  step(settle, [this] { expect("band 40 returns to 7.074", sim_->hz() == 7074000); });
  step(1, [this] { click("band:15"); });
  step(settle, [this] {
    expect("band 15 first visit: 21.25 USB", sim_->hz() == 21250000 && sim_->mode() == civ::USB);
    pressKey(ImGuiKey_PageDown);
  });
  step(settle, [this] {
    expect("Page Down goes to the next band down", plan_.bandOf(sim_->hz()) == 6);
    pressKey(ImGuiKey_PageUp);
  });
  step(settle, [this] { expect("Page Up comes back", sim_->hz() == 21250000); });
  step(1, [this] { click("band:20", 0.5f, 0.5f, 1); });
  step(12, [this] { click("preset:20:FT8"); });
  step(settle, [this] { expect("20 m right-click FT8: 14.074 USB-D", sim_->hz() == 14074000 && sim_->mode() == civ::USB && sim_->data()); });

  // ---- band plans: the map, the flags, and choosing another country
  step(1, [this] { click("bandmap", 0.5f, 0.5f); });
  step(settle, [this] {
    expect("clicking the band map tunes within the band", plan_.segmentAt(sim_->hz()) && plan_.bandOf(sim_->hz()) == 5);
    typeText("14075");
    pressKey(ImGuiKey_Enter);
  });
  step(settle, [this] { click("marker:20:WSPR"); });
  step(settle, [this] {
    expect("clicking a flag goes there with its mode", sim_->hz() == 14095600 && sim_->mode() == civ::USB && sim_->data());
    expect("the UK plan has 4 m but not 7.25 MHz", plan_.bandOf(70200000) >= 0 && plan_.bandOf(7250000) < 0);
    choosePlan("us");
    expect("the US plan has 7.25 MHz but not 4 m", plan_.bandOf(7250000) >= 0 && plan_.bandOf(70200000) < 0);
  });
  step(10, [this] { click("band:60"); });
  step(settle, [this] {
    expect("a band goes home under the new plan", plan_.segmentAt(sim_->hz()) && plan_.bandOf(sim_->hz()) == plan_.bandOf(5371500));
    choosePlan("uk");
  });
  step(10, [this] { click("band:20", 0.5f, 0.5f, 1); });
  step(12, [this] { click("preset:20:FT8"); });
  step(settle, [this] { expect("back on the UK plan, FT8 is where it was", sim_->hz() == 14074000 && sim_->data()); });

  // ---- stations: on the scope whichever plan is chosen, and found by name
  step(1, [this] { typeText("8040"); pressKey(ImGuiKey_Enter); });
  step(settle, [this] { click("station:8040000"); });
  step(settle, [this] {
    expect("clicking a station's flag tunes below it, for a decoder", sim_->hz() == 8038100 && sim_->mode() == civ::USB && !sim_->data());
    choosePlan("us");
    rects_.erase("station:8040000");
  });
  step(settle, [this] {
    expect("a station's flag stays under another plan", rects_.count("station:8040000") == 1);
    choosePlan("uk");
    showService_[bands::S_NAVAL] = false;
  });
  step(2, [this] { rects_.erase("station:8040000"); });
  step(settle, [this] {
    expect("hiding a service hides its flags", rects_.count("station:8040000") == 0);
    showService_[bands::S_NAVAL] = true;
    stationSearchOpen_ = stationSearchFocus_ = true;
  });
  step(settle, [this] { typeText("northwood"); });
  step(settle, [this] {
    expect("searching by name finds the station", stationHitCount_ == 4);
    click("stationhit:0");
  });
  step(settle, [this] {
    expect("clicking a result goes there", sim_->hz() == 2616600 && sim_->mode() == civ::USB);
    stationQuery_[0] = 0;
    stationSearchOpen_ = false;
  });
  step(10, [this] { click("band:20", 0.5f, 0.5f, 1); });
  step(12, [this] { click("preset:20:FT8"); });
  step(settle, [this] { expect("back on the band after listening elsewhere", sim_->hz() == 14074000 && sim_->data()); });

  // ---- modes and filters
  step(1, [this] { click("data"); });
  step(settle, [this] { expect("DATA turns off", !sim_->data()); });
  step(1, [this] { click("mode:CW"); });
  step(settle, [this] { expect("mode CW", sim_->mode() == civ::CW); });
  step(1, [this] { click("mode:LSB"); });
  step(settle, [this] { expect("mode LSB", sim_->mode() == civ::LSB); });
  step(1, [this] { click("FIL2"); });
  step(settle, [this] { expect("FIL2", sim_->filter() == 2); });
  step(1, [this] { pressKey(ImGuiKey_F2); });
  step(settle, [this] { expect("F2 is USB", sim_->mode() == civ::USB); });
  step(1, [this] { pressKey(ImGuiKey_F10); });
  step(settle, [this] { expect("F10 cycles the filter", sim_->filter() == 3); });

  // ---- tuning controls
  step(1, [this] { typeText("14000"); pressKey(ImGuiKey_Enter); stepIdx_ = 4; });
  step(settle, [this] { pressKey(ImGuiKey_RightArrow); });
  step(settle, [this] { expect("Right arrow: up a 1 kHz step", sim_->hz() == 14001000); pressKey(ImGuiKey_LeftArrow); });
  step(settle, [this] { expect("Left arrow: down a step", sim_->hz() == 14000000); });
  step(1, [this] { pressKey(ImGuiKey_UpArrow); });
  step(5, [this] { expect("Up arrow raises the step", stepIdx_ == 5); stepIdx_ = 4; });
  step(1, [this] { click("digit:4", 0.5f, 0.25f); });  // the 1kHz digit, top half
  step(settle, [this] { expect("clicking the top of a digit tunes up", sim_->hz() == 14001000); click("digit:4", 0.5f, 0.75f); });
  step(settle, [this] { expect("the bottom tunes down", sim_->hz() == 14000000); });
  step(1, [this] {
    inject_.push_back([this](ImGuiIO& io) {
      const ImRect& r = rects_["digit:3"];
      io.AddMousePosEvent((r.Min.x + r.Max.x) / 2, (r.Min.y + r.Max.y) / 2);
    });
    inject_.push_back([](ImGuiIO&) {});
    inject_.push_back([](ImGuiIO& io) { io.AddMouseWheelEvent(0, 1); });
  });
  step(settle, [this] { expect("wheel on a digit tunes that digit", sim_->hz() == 14010000); });

  // ---- the tuning dial: a drag round its rim, in the small moves a hand makes
  auto spinDial = [this](float fromDeg, float toDeg) {
    inject_.push_back([this, fromDeg](ImGuiIO& io) {
      ImRect r = rects_["dial"];
      float rad = fromDeg * (float)M_PI / 180, c = r.GetWidth() / 2;
      io.AddMousePosEvent(r.Min.x + c + cosf(rad) * c * 0.7f, r.Min.y + c + sinf(rad) * c * 0.7f);
    });
    inject_.push_back([](ImGuiIO&) {});
    inject_.push_back([](ImGuiIO& io) { io.AddMouseButtonEvent(0, true); });
    inject_.push_back([](ImGuiIO&) {});
    const int moves = 90;  // unhurried, whatever the frame rate, so it's plain tuning with no speed-up
    for (int i = 1; i <= moves; i++) {
      float deg = fromDeg + (toDeg - fromDeg) * (float)i / moves;
      inject_.push_back([this, deg](ImGuiIO& io) {
        ImRect r = rects_["dial"];
        float rad = deg * (float)M_PI / 180, c = r.GetWidth() / 2;
        io.AddMousePosEvent(r.Min.x + c + cosf(rad) * c * 0.7f, r.Min.y + c + sinf(rad) * c * 0.7f);
      });
    }
    inject_.push_back([](ImGuiIO&) {});
    inject_.push_back([](ImGuiIO& io) { io.AddMouseButtonEvent(0, false); });
  };
  step(1, [this] { typeText("14100"); pressKey(ImGuiKey_Enter); setStep(4); });  // 1 kHz
  step(settle, [this] { click("dial", 0.85f, 0.5f); });
  step(settle, [this] { expect("clicking the dial without spinning it stays put", sim_->hz() == 14100000); });
  // A click is a tenth of the step, 100 Hz, every 3.6 degrees. So 95 degrees is 26 clicks, and the part-click
  // left over is forgotten on release.
  step(1, [spinDial] { spinDial(0, 95); });
  step(settle, [this] { expect("spinning the dial clockwise tunes up", sim_->hz() == 14102600); });
  step(1, [spinDial] { spinDial(95, 0); });
  step(settle, [this] { expect("spinning it anticlockwise tunes down", sim_->hz() == 14100000); });
  // Across the point where the angle wraps, which mustn't read as most of a turn the other way.
  step(1, [spinDial] { spinDial(170, 265); });
  step(settle, [this] { expect("spinning across the wrap tunes the right way", sim_->hz() == 14102600); setStep(2); });

  // ---- waterfalls
  step(1, [this] { typeText("14100"); pressKey(ImGuiKey_Enter); });
  step(60, [this] { expect("scope sweeps arriving", !scopeLine_.empty() && scopeHi_ > scopeLo_); click("scopeplot", 0.75f, 0.7f); });
  step(settle, [this] {
    // The sweep is centred on the dial, so 3/4 across is a quarter of the span up.
    long long want = 14100000 + (scopeHi_ - scopeLo_) / 4;
    expect("click on the band scope tunes there", llabs(sim_->hz() - want) < 400);
    typeText("14100");
    pressKey(ImGuiKey_Enter);
    pressKey(ImGuiKey_F2);
  });
  step(settle, [this] {
    // USB, not data: a signal at 3/4 of 0-3kHz (2250Hz) moves to 1500Hz: the dial goes up 750Hz.
    expect("USB for the audio click", sim_->mode() == civ::USB && !sim_->data());
    click("audioplot", 0.75f, 0.7f);
  });
  step(settle, [this] { expect("click on the audio waterfall centres the signal", llabs(sim_->hz() - 14100750) < 30); });
  step(1, [this] { click("mode:CW"); typeText("14100"); pressKey(ImGuiKey_Enter); });
  step(settle, [this] {
    // A signal at 1/2 of 0-3k (1500Hz) must move to the pitch.
    click("audioplot", 0.5f, 0.7f);
  });
  step(settle, [this] {
    int pitch = tuning::cwPitchHz(st_.level[civ::L_CW_PITCH]);
    expect("in CW it moves to the pitch", llabs(sim_->hz() - (14100000 + (1500 - pitch))) < 30);
  });
  step(1, [this] { click("mode:USB"); click("data"); typeText("14100"); pressKey(ImGuiKey_Enter); });
  step(settle, [this] {
    expect("back to USB-D", sim_->data());
    click("audioplot", 0.5f, 0.7f);
  });
  step(settle, [this] { expect("data mode: audio click leaves the dial alone", sim_->hz() == 14100000); });
  step(1, [this] { rig_.setScopeSpan(10000); });
  step(30, [this] { expect("span change reaches the rig", !sim_->received(0x27).empty()); });
  step(30, [this] { expect("the scope follows the span", scopeHi_ - scopeLo_ == 10000); rig_.setScopeSpan(100000); });

  // ---- the receive controls reach the rig
  step(1, [this] { click("sw:NB"); });
  step(settle, [this] {
    expect("NB button sets 16 22 01", [&] { for (auto& d : sim_->received(0x16)) if (d.size() == 2 && d[0] == 0x22 && d[1] == 1) return true; return false; }());
    click("Preamp:P2");
  });
  step(settle, [this] {
    expect("preamp P2 sets 16 02 02", [&] { for (auto& d : sim_->received(0x16)) if (d.size() == 2 && d[0] == 0x02 && d[1] == 2) return true; return false; }());
    click("att");
  });
  step(settle, [this] {
    expect("attenuator sets 11 20", [&] { for (auto& d : sim_->received(0x11)) if (d.size() == 1 && d[0] == 0x20) return true; return false; }());
    click("level:AF", 0.25f, 0.5f);
  });
  step(settle, [this] {
    expect("dragging AF to a quarter sets about 64", st_.level[civ::L_AF] > 40 && st_.level[civ::L_AF] < 90);
    click("swap");
  });
  step(settle, [this] { expect("A/B swaps the VFOs", sim_->hz() != 14100000 && st_.otherHz == 14100000); click("swap"); });

  // ---- transmit stays locked until armed
  step(settle, [this] { click("ptt"); click("tune"); });
  step(settle, [this] {
    expect("PTT and TUNE do nothing unarmed", !sim_->tx() && !st_.tx);
    expect("tuner not started unarmed", [&] { for (auto& d : sim_->received(0x1C)) if (d.size() == 2 && d[0] == 1 && d[1] == 2) return false; return true; }());
    inject_.push_back([](ImGuiIO& io) { io.AddKeyEvent(ImGuiMod_Ctrl, true); });
    inject_.push_back([](ImGuiIO& io) { io.AddKeyEvent(ImGuiKey_Space, true); });
    inject_.push_back([](ImGuiIO& io) { io.AddKeyEvent(ImGuiKey_Space, false); });
    inject_.push_back([](ImGuiIO& io) { io.AddKeyEvent(ImGuiMod_Ctrl, false); });
  });
  step(settle, [this] { expect("Ctrl+Space does nothing unarmed", !sim_->tx()); click("arm"); });
  step(10, [this] { expect("ARM arms", st_.txArmed); click("ptt"); });
  step(settle, [this] { expect("PTT keys once armed", sim_->tx() && st_.tx); click("ptt"); });
  step(settle, [this] { expect("PTT again releases", !sim_->tx()); click("ptt"); });
  step(settle, [this] { expect("keyed again", sim_->tx()); click("arm"); });
  step(settle, [this] { expect("disarming releases", !sim_->tx() && !st_.txArmed); });

  // ---- the needle meter: a click on the meter swaps it, and it follows what the bars follow
  step(1, [this] { needleMeter_ = false; click("meters"); });
  step(10, [this] { expect("a click on the meter gives the needle", needleMeter_); });
  step(90, [this] {
    expect("the needle sits on the S reading", fabsf(needle_.pos - needle::signalShare(st_.meter[civ::M_S])) < 0.2f);
    expect("the backlight is on with a radio connected", meterLamp_ > 0.9f);
    click("arm");
  });
  step(10, [this] { click("ptt"); });
  step(60, [this] {
    expect("transmitting, the needle reads power", st_.tx && fabsf(needle_.pos - needle::powerShare(civ::powerWatts(st_.meter[civ::M_PO], radio(), st_.hz), radio().maxWattsAt(st_.hz))) < 0.1f);
    click("arm");
  });
  step(settle, [this] {
    expect("the needle meter doesn't stop PTT letting go", !sim_->tx());
    click("meters");
  });
  step(10, [this] { expect("another click brings the bars back", !needleMeter_); });

  // ---- memories
  step(1, [this, hoverOut] {
    hoverOut();
    typeText("7030");
    pressKey(ImGuiKey_Enter);
  });
  step(settle, [this] { snprintf(memName_, sizeof memName_, "Test"); opt_.tab = 1; frameCount_ = 0; });
  step(10, [this] { click("memsave"); });
  step(10, [this] {
    expect("memory saved", !memories_.empty() && memories_.back().hz == 7030000);
    typeText("14100");
    pressKey(ImGuiKey_Enter);
  });
  step(settle, [this] { click("mem:" + std::to_string((int)memories_.size() - 1)); });
  step(settle, [this] {
    expect("clicking a memory recalls it", sim_->hz() == 7030000);
    if (!memories_.empty()) memories_.pop_back();
    store::saveMemories(memPath_, memories_);
  });

  // ---- the same window, and now the cable goes to an IC-9700
  step(1, [this] {
    rig_.stop();
    sim_.reset(new SimRig(civ::kRadio9700));
    rects_.clear();
    rig_.start([this] { return sim_->newTransport(); });
  });
  step(90, [this] {
    expect("9700: found by asking, with the 7300's address still chosen", st_.connected && st_.radio == &civ::kRadio9700 && st_.rigId == civ::kIc9700);
    expect("9700: the band buttons are its own", rects_.count("band:2") && rects_.count("band:70") && rects_.count("band:23") && !rects_.count("band:20") && !rects_.count("band:6"));
    expect("9700: no tuner buttons", !rects_.count("tune"));
    expect("9700: the title says so", windowTitle().find("IC-9700") != std::string::npos);
    click("band:70");
  });
  step(settle, [this] {
    expect("9700: 70 goes to 70 cm", sim_->hz() >= 430000000 && sim_->hz() <= 450000000);
    click("band:23");
  });
  step(settle, [this] {
    expect("9700: 23 goes to 23 cm", sim_->hz() >= 1240000000 && sim_->hz() <= 1300000000);
    click("mode:DD");
  });
  step(settle, [this] {
    expect("9700: DD can be chosen on 23 cm", sim_->mode() == civ::DD);
    pressKey(ImGuiKey_PageUp);
  });
  step(settle, [this] {
    expect("9700: Page Up wraps round its own bands", sim_->hz() >= 144000000 && sim_->hz() <= 148000000);
    pressKey(ImGuiKey_PageDown);
  });
  step(settle, [this] {
    expect("9700: Page Down wraps back", sim_->hz() >= 1240000000);
    typeText("145.5");
    pressKey(ImGuiKey_Enter);
  });
  step(settle, [this] {
    expect("9700: typed 145.5 is MHz", sim_->hz() == 145500000);
    typeText("1296200");
    pressKey(ImGuiKey_Enter);
  });
  step(settle, [this] {
    expect("9700: typed 1296200 is kHz", sim_->hz() == 1296200000);
    typeText("14.074");
    pressKey(ImGuiKey_Enter);
  });
  step(settle, [this] {
    expect("9700: 14.074 is refused", sim_->hz() == 1296200000 && !entry_.empty());
    pressKey(ImGuiKey_Escape);
  });
  step(10, [this] {
    goMarker(stations_.files[0].stations[0]);
    expect("9700: an HF station doesn't send it elsewhere", status_.find("isn't on the IC-9700") != std::string::npos);
    for (const bands::Station& s : stations_.files[0].stations)
      if (s.hz == 145800000) goMarker(s);
  });
  step(settle, [this] {
    expect("9700: a 2 m station tunes it and sets the mode", sim_->hz() == 145800000 && sim_->mode() == civ::FM);
    expect("9700: and has its flag on the scope", rects_.count("station:145800000") == 1);
    typeText("148");
    pressKey(ImGuiKey_Enter);
  });
  step(settle, [this] { setStep(2); pressKey(ImGuiKey_RightArrow); });
  step(settle, [this] {
    expect("9700: tuning stops at the top of 2 m", sim_->hz() == 148000000);
    typeText("145.5");
    pressKey(ImGuiKey_Enter);
    rig_.setOtherFreq(145600000);
  });
  step(settle, [this] { click("swap"); });
  step(settle, [this] {
    expect("9700: A/B swaps the VFOs", sim_->hz() == 145600000 && sim_->otherHz() == 145500000);
    expect("9700: A/B doesn't exchange MAIN and SUB", !sim_->mainSubExchanged());
    click("att");
  });
  step(settle, [this] {
    expect("9700: the attenuator takes its own code", sim_->att() == 0x10 && st_.lastError.find("attenuator") == std::string::npos);
    click("att");
  });
  step(settle, [this] {
    click("mode:DV");
    expect("9700: nothing has been refused", st_.lastError.empty());
  });
  step(settle, [this] { expect("9700: DV is a mode", sim_->mode() == civ::DV); });

  // ---- and now to an IC-R8600, which can't transmit and has a good deal less to control
  step(1, [this] {
    rig_.stop();
    sim_.reset(new SimRig(civ::kRadioR8600));
    rects_.clear();
    rig_.start([this] { return sim_->newTransport(); });
  });
  step(90, [this] {
    expect("R8600: found by asking", st_.connected && st_.radio == &civ::kRadioR8600 && st_.rigId == civ::kIcR8600);
    expect("R8600: the title says so", windowTitle().find("IC-R8600") != std::string::npos);
    needleMeter_ = true;  // the rest of the receiver's run shows the needle with no power scale on it
    expect("R8600: nothing that transmits is shown", !rects_.count("arm") && !rects_.count("ptt") && !rects_.count("tune") && !rects_.count("swap"));
    expect("R8600: no data button", !rects_.count("data"));
    expect("R8600: three antennas, three attenuator steps", rects_.count("ant:1") && rects_.count("ant:2") && rects_.count("ant:3") && rects_.count("att:30"));
    expect("R8600: the bands are on a menu", rects_.count("bandmenu") && !rects_.count("band:20"));
    expect("R8600: the plan has its whole coverage", plan_.bandOf(198000) >= 0 && plan_.bandOf(100000000) >= 0 && plan_.bandOf(1575420000) >= 0 && plan_.bandOf(14074000) >= 0);
    click("ant:2");
  });
  step(settle, [this] {
    expect("R8600: ANT2 below 30 MHz", sim_->antenna() == 1 && st_.ant == 1);
    click("ant:3");
  });
  step(settle, [this] {
    expect("R8600: ANT3 below 30 MHz", sim_->antenna() == 2);
    click("att:20");
  });
  step(settle, [this] {
    expect("R8600: the attenuator takes 20 dB", sim_->att() == 0x20 && st_.attDb == 20);
    click("att:0");
  });
  step(settle, [this] {
    expect("R8600: and off again", sim_->att() == 0);
    click("ipplus");
  });
  step(settle, [this] {
    expect("R8600: IP+", sim_->ipPlus() && st_.ipPlus == 1);
    click("Preamp:ON");
  });
  step(settle, [this] {
    expect("R8600: the preamp is just on or off", st_.sw[civ::S_PREAMP] == 1 && !rects_.count("Preamp:P2"));
    typeText("100");
    pressKey(ImGuiKey_Enter);
  });
  step(settle, [this] {
    expect("R8600: typed 100 is MHz", sim_->hz() == 100000000);
    click("mode:WFM");
  });
  step(settle, [this] {
    expect("R8600: WFM, with no filter byte", sim_->mode() == civ::WFM && sim_->received(0x06).back() == civ::Bytes{civ::WFM});
    click("ant:1");
  });
  step(settle, [this] {
    expect("R8600: above 30 MHz, only ANT1", st_.ant == 0);
    click("ant:2");  // disabled, so nothing should come of it
  });
  step(settle, [this] {
    expect("R8600: ANT2 does nothing above 30 MHz", st_.ant == 0 && st_.lastError.find("antenna") == std::string::npos);
    click("modes:digital");
  });
  step(settle, [this] { click("mode:P25"); });
  step(settle, [this] {
    expect("R8600: a digital mode from its menu", sim_->mode() == civ::P25);
    click("bandmenu");
  });
  step(settle, [this] {
    expect("R8600: the band menu opens on its groups", rects_.count("bandgroup:Amateur") && rects_.count("bandgroup:Aviation"));
    pressKey(ImGuiKey_Escape);
  });
  step(settle, [this] {
    int b = plan_.bandOf(121500000);
    expect("R8600: 121.5 MHz is on a band", b >= 0 && plan_.bands[(size_t)b].group == "Aviation");
    if (b >= 0) goBand(b);
  });
  step(settle, [this] {
    expect("R8600: a band's mode goes with it", sim_->mode() == civ::AM && sim_->hz() >= 118000000 && sim_->hz() <= 137000000);
    bool found = false;
    for (const bands::Station& s : stations_.find("receiver")->stations)
      if (s.hz == 156800000) {
        goMarker(s);
        found = true;
      }
    expect("R8600: marine channel 16 is listed", found);
  });
  step(settle, [this] {
    expect("R8600: a station sets its mode", sim_->hz() == 156800000 && sim_->mode() == civ::FM);
    expect("R8600: and has its flag on the scope", rects_.count("station:156800000") == 1);
    expect("R8600: never asked to transmit", sim_->received(0x1C).empty() && !sim_->tx());
    expect("R8600: never asked for a second VFO", sim_->received(0x0F).empty() && sim_->received(0x21).empty() && sim_->received(0x25).empty() && sim_->received(0x26).empty());
    expect("R8600: nothing has been refused", sim_->refusals() == 0);
  });
  // ---- and back, in case the receiver's bands and stations have stuck
  step(1, [this] {
    rig_.stop();
    sim_.reset(new SimRig(civ::kRadio7300));
    rects_.clear();
    rig_.start([this] { return sim_->newTransport(); });
  });
  step(90, [this] {
    expect("7300 again: its buttons are back", st_.radio == &civ::kRadio7300 && rects_.count("band:20") && rects_.count("arm") && !rects_.count("ant:1"));
    expect("7300 again: the receiver's bands are gone", plan_.bandOf(100000000) < 0 && plan_.bandOf(9700000) < 0);
    expect("7300 again: so are its stations", stations_.find("receiver") == nullptr);
    expect("the clock button is beside the time", rects_.count("clocksync") == 1);
    click("clocksync");
  });
  step(settle, [this] {
    expect("the clock button reports back", status_.find("no time server asked") != std::string::npos && !clockSync_);
  });
  step(5, [this] { testDone_ = true; });
}

void App::runSelfTest() {
  ImGuiIO& io = ImGui::GetIO();
  // Injected input first, one piece a frame, as a person's hand would.
  if (!inject_.empty()) {
    auto fn = inject_.front();
    inject_.erase(inject_.begin());
    fn(io);
    return;
  }
  // Waits are counted at 60 frames a second but measured by the clock: the rig's replies take real time,
  // and a covered window can draw far faster or slower than its display.
  if (stepWait_ > 0) {
    stepUntil_ = glfwGetTime() + stepWait_ / 60.0;
    stepWait_ = 0;
  }
  if (glfwGetTime() < stepUntil_) return;
  if (stepAt_ < steps_.size()) {
    steps_[stepAt_].fn();
    stepAt_++;
    if (stepAt_ < steps_.size()) stepWait_ = steps_[stepAt_].waitFrames;
  }
}

int App::run() {
  loadSettings();
  openSettings_ = opt_.settings;
  if (opt_.demo) {
    seenRadio_ = opt_.demoRadio == 9700 ? &civ::kRadio9700 : opt_.demoRadio == 8600 ? &civ::kRadioR8600 : &civ::kRadio7300;
    sim_.reset(new SimRig(*seenRadio_));
  }

  glfwSetErrorCallback(glfwError);
  if (!glfwInit()) return 1;
#ifdef __APPLE__
  const char* glsl = "#version 150";
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
  glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
#else
  const char* glsl = "#version 130";
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
#endif
  bool automated = !opt_.selfTest.empty() || !opt_.screenshot.empty();
  if (automated) {
    // A scripted run mustn't take the keyboard from whoever's using the machine.
    glfwWindowHint(GLFW_FOCUSED, GLFW_FALSE);
    glfwWindowHint(GLFW_FOCUS_ON_SHOW, GLFW_FALSE);
  }
  // The layout is designed in logical pixels and needs 1280x720 of them. Window sizes are real pixels on
  // Windows and X11 (macOS and Wayland handle scaling themselves). If the display's scaling leaves fewer
  // than that, the whole interface shrinks to fit rather than hanging its controls off the edge.
  float sx = 1, sy = 1;
  int waX = 0, waY = 0, waW = 1920, waH = 1080;
  if (GLFWmonitor* m = glfwGetPrimaryMonitor()) {
    glfwGetMonitorContentScale(m, &sx, &sy);
    glfwGetMonitorWorkarea(m, &waX, &waY, &waW, &waH);
  }
#ifdef __APPLE__
  sx = 1;
#endif
  scale_ = std::max(0.75f, std::min({sx, (float)waW / 1280.f, (float)waH / 720.f}));
  int w = opt_.width ? opt_.width : cfg_.getInt("win_w", (int)(1560 * scale_));
  int h = opt_.height ? opt_.height : cfg_.getInt("win_h", (int)(960 * scale_));
  if (!opt_.width) {
    w = std::min(w, waW);
    h = std::min(h, waH - (int)(40 * scale_));  // room for the title bar
  }
  win_ = glfwCreateWindow(w, h, windowTitle().c_str(), nullptr, nullptr);
  if (!win_) {
    glfwTerminate();
    return 1;
  }
  glfwMakeContextCurrent(win_);
  glfwSwapInterval(1);
  glfwSetWindowSizeLimits(win_, std::min((int)(1280 * scale_), waW), std::min((int)(700 * scale_), waH), GLFW_DONT_CARE, GLFW_DONT_CARE);
#ifndef _WIN32
  setIcon(win_);
#endif

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  setupStyle(scale_);
  // A scripted run gets no real mouse or keyboard at all: only what the script injects.
  ImGui_ImplGlfw_InitForOpenGL(win_, opt_.selfTest.empty());
  ImGui_ImplOpenGL3_Init(glsl);

  wfScope_.init(civ::kScopePoints, kWaterfallRows);
  applyFft();
  audio_.setMonitorGain(monitorGain_);
  connect();
  keepAudioOpen();
  if (!plans_.errors.empty()) say("A band plan has a problem: " + plans_.errors[0]);
  else if (!automated && cfg_.get("band_plan").empty()) say("The band plan is set to " + plan_.name + ". Choose your country in Settings.");
  if (monitor_ && !opt_.demo) {
    std::string err;
    if (!audio_.startMonitor(audioOut_, err)) say(err);
  }
  if (!opt_.selfTest.empty()) {
    buildSelfTest();
    stepWait_ = steps_.empty() ? 0 : steps_[0].waitFrames;
  }

  uint64_t frames = 0;
  double lastFrameAt = glfwGetTime();
  while (!glfwWindowShouldClose(win_)) {
    glfwPollEvents();
    if (glfwGetWindowAttrib(win_, GLFW_ICONIFIED)) std::this_thread::sleep_for(std::chrono::milliseconds(15));
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    if (!opt_.selfTest.empty()) runSelfTest();
    if (opt_.tuneHz && frames == 60) tuneTo(opt_.tuneHz);
    ImGui::NewFrame();
    frame();
    ImGui::Render();
    int fw, fh;
    glfwGetFramebufferSize(win_, &fw, &fh);
    glViewport(0, 0, fw, fh);
    glClearColor(0.06f, 0.07f, 0.09f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    frames++;
    if (!opt_.screenshot.empty() && (int)frames == opt_.screenshotFrames) {
      saveBmp(opt_.screenshot, fw, fh);
      glfwSetWindowShouldClose(win_, 1);
    }
    glfwSwapBuffers(win_);
    // Vsync stops holding us back while the window's covered or minimised, on some drivers. Without this
    // a hidden window would spin a CPU core flat out.
    {
      double now = glfwGetTime();
      if (now - lastFrameAt < 1.0 / 120) std::this_thread::sleep_for(std::chrono::milliseconds(6));
      lastFrameAt = glfwGetTime();
    }
    if (!opt_.selfTest.empty() && testDone_) {
      FILE* f = fopen(opt_.selfTest.c_str(), "w");
      if (f) {
        for (const std::string& r : results_) fprintf(f, "%s\n", r.c_str());
        fprintf(f, "\n%d checks, %d failed\n", (int)results_.size(), testFails_);
        fclose(f);
      }
      glfwSetWindowShouldClose(win_, 1);
    }
  }

  if (!automated) saveSettings();
  rec_.close();
  rig_.stop();
  audio_.stopMonitor();
  audio_.stopCapture();
  int rc = testFails_ ? 1 : 0;
  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext();
  return rc;  // the waterfalls' textures go with App, while the GL context is still alive
}
}  // namespace

int runApp(const AppOptions& opt) {
  int rc;
  {
    App app(opt);
    rc = app.run();
  }
  glfwTerminate();
  return rc;
}
