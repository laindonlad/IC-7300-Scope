#include "store.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fstream>
#include <sstream>

#include "bands.h"

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#include <shellapi.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

namespace store {
void makeDirs(const std::string& path) {
  std::string cur;
  for (size_t i = 0; i < path.size(); i++) {
    cur += path[i];
    if (path[i] == '/' || path[i] == '\\' || i + 1 == path.size()) {
#ifdef _WIN32
      _mkdir(cur.c_str());
#else
      mkdir(cur.c_str(), 0755);
#endif
    }
  }
}

bool saveText(const std::string& path, const std::string& text) {
  std::string tmp = path + ".tmp";
  {
    std::ofstream f(tmp, std::ios::binary);
    if (!f) return false;
    f << text;
    if (!f.good()) return false;
  }
#ifdef _WIN32
  return MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
#else
  return rename(tmp.c_str(), path.c_str()) == 0;
#endif
}

std::string dataDir() {
  std::string d;
#ifdef _WIN32
  const char* a = getenv("APPDATA");
  d = std::string(a ? a : ".") + "\\IC7300Console";
#elif defined(__APPLE__)
  const char* h = getenv("HOME");
  d = std::string(h ? h : ".") + "/Library/Application Support/IC7300Console";
#else
  const char* c = getenv("XDG_CONFIG_HOME");
  const char* h = getenv("HOME");
  d = (c && *c ? std::string(c) : std::string(h ? h : ".") + "/.config") + "/ic7300console";
#endif
  makeDirs(d);
#ifdef _WIN32
  return d + "\\";
#else
  return d + "/";
#endif
}

std::string exeDir() {
  char b[4096] = "";
#ifdef _WIN32
  DWORD n = GetModuleFileNameA(nullptr, b, sizeof b);
  std::string s(b, n < sizeof b ? n : 0);
#elif defined(__APPLE__)
  uint32_t n = sizeof b;
  std::string s(_NSGetExecutablePath(b, &n) == 0 ? b : "");
#else
  ssize_t n = readlink("/proc/self/exe", b, sizeof b - 1);
  std::string s(b, n > 0 ? (size_t)n : 0);
#endif
  size_t cut = s.find_last_of("\\/");
  if (cut == std::string::npos) return std::string(".") + (s.find('\\') != std::string::npos ? "\\" : "/");
  return s.substr(0, cut + 1);
}

// A folder under the program's, or under the data folder if the program's can't be written to.
static std::string writableFolder(const std::string& name) {
  std::string d = exeDir() + name;
  makeDirs(d);
  std::string probe = d + ".write-test";
  FILE* f = fopen(probe.c_str(), "wb");
  if (!f) return dataDir() + name;
  fclose(f);
  remove(probe.c_str());
  return d;
}

std::string receivedDir() {
#ifdef _WIN32
  static const std::string chosen = writableFolder("received\\");
#else
  static const std::string chosen = writableFolder("received/");
#endif
  makeDirs(chosen);
  return chosen;
}

std::string overlaysDir() {
#ifdef _WIN32
  static const std::string chosen = writableFolder("overlays\\");
#else
  static const std::string chosen = writableFolder("overlays/");
#endif
  makeDirs(chosen);
  return chosen;
}

void openFolder(const std::string& path) {
#ifdef _WIN32
  ShellExecuteA(nullptr, "open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#elif defined(__APPLE__)
  std::string cmd = "open \"" + path + "\"";
  if (system(cmd.c_str()) != 0) {}
#else
  std::string cmd = "xdg-open \"" + path + "\" &";
  if (system(cmd.c_str()) != 0) {}
#endif
}

void Settings::load(const std::string& path) {
  kv_.clear();
  std::ifstream f(path);
  std::string line;
  while (std::getline(f, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    auto eq = line.find('=');
    if (eq != std::string::npos) kv_.push_back({line.substr(0, eq), line.substr(eq + 1)});
  }
}
bool Settings::save(const std::string& path) const {
  std::string t;
  for (const auto& p : kv_) t += p.first + "=" + p.second + "\n";
  return saveText(path, t);
}
std::string Settings::get(const std::string& k, const std::string& def) const {
  for (const auto& p : kv_)
    if (p.first == k) return p.second;
  return def;
}
int Settings::getInt(const std::string& k, int def) const {
  std::string v = get(k);
  return v.empty() ? def : atoi(v.c_str());
}
double Settings::getDouble(const std::string& k, double def) const {
  std::string v = get(k);
  return v.empty() ? def : atof(v.c_str());
}
void Settings::set(const std::string& k, const std::string& v) {
  for (auto& p : kv_)
    if (p.first == k) {
      p.second = v;
      return;
    }
  kv_.push_back({k, v});
}
void Settings::setInt(const std::string& k, int v) { set(k, std::to_string(v)); }
void Settings::setDouble(const std::string& k, double v) {
  char b[32];
  snprintf(b, sizeof b, "%.4f", v);
  set(k, b);
}

std::string joinCsv(const std::vector<std::string>& fields) {
  std::string out;
  for (size_t i = 0; i < fields.size(); i++) {
    if (i) out += ',';
    const std::string& s = fields[i];
    if (s.find_first_of(",\"\n\r") == std::string::npos) {
      out += s;
      continue;
    }
    out += '"';
    for (char c : s) {
      if (c == '"') out += '"';
      out += (c == '\n' || c == '\r') ? ' ' : c;
    }
    out += '"';
  }
  return out;
}

std::vector<std::string> splitCsv(const std::string& line) {
  std::vector<std::string> out;
  std::string cur;
  bool quoted = false;
  for (size_t i = 0; i < line.size(); i++) {
    char c = line[i];
    if (quoted) {
      if (c == '"' && i + 1 < line.size() && line[i + 1] == '"') cur += '"', i++;
      else if (c == '"') quoted = false;
      else cur += c;
    } else if (c == '"') {
      quoted = true;
    } else if (c == ',') {
      out.push_back(cur);
      cur.clear();
    } else if (c != '\r') {
      cur += c;
    }
  }
  out.push_back(cur);
  return out;
}

std::vector<Memory> loadMemories(const std::string& path) {
  std::vector<Memory> m;
  std::ifstream f(path);
  std::string line;
  while (std::getline(f, line)) {
    auto p = splitCsv(line);
    if (p.size() < 6) continue;
    Memory e;
    e.name = p[0], e.group = p[1];
    e.hz = atoll(p[2].c_str());
    e.mode = atoi(p[3].c_str()), e.data = p[4] == "1", e.filter = atoi(p[5].c_str());
    if (e.hz > 0) m.push_back(e);
  }
  return m;
}
bool saveMemories(const std::string& path, const std::vector<Memory>& m) {
  std::string t;
  for (const Memory& e : m)
    t += joinCsv({e.name, e.group, std::to_string(e.hz), std::to_string(e.mode), e.data ? "1" : "0", std::to_string(e.filter)}) + "\n";
  return saveText(path, t);
}

std::vector<LogEntry> loadLog(const std::string& path) {
  std::vector<LogEntry> l;
  std::ifstream f(path);
  std::string line;
  while (std::getline(f, line)) {
    auto p = splitCsv(line);
    if (p.size() < 8) continue;
    LogEntry e;
    e.utc = p[0], e.call = p[1], e.hz = atoll(p[2].c_str()), e.mode = p[3];
    e.rstSent = p[4], e.rstRcvd = p[5], e.name = p[6], e.notes = p[7];
    l.push_back(e);
  }
  return l;
}
static std::string logLine(const LogEntry& e) {
  return joinCsv({e.utc, e.call, std::to_string(e.hz), e.mode, e.rstSent, e.rstRcvd, e.name, e.notes}) + "\n";
}
bool appendLog(const std::string& path, const LogEntry& e) {
  std::ofstream f(path, std::ios::app | std::ios::binary);
  if (!f) return false;
  f << logLine(e);
  return f.good();
}
bool saveLog(const std::string& path, const std::vector<LogEntry>& log) {
  std::string t;
  for (const LogEntry& e : log) t += logLine(e);
  return saveText(path, t);
}

static void adifField(std::string& out, const char* name, const std::string& v) {
  if (v.empty()) return;
  out += "<" + std::string(name) + ":" + std::to_string(v.size()) + ">" + v + " ";
}

std::string adifRecord(const LogEntry& e, const std::string& stationCall) {
  std::string out;
  adifField(out, "CALL", e.call);
  // "2026-10-31 19:05" to 20261031 and 1905
  if (e.utc.size() >= 16) {
    adifField(out, "QSO_DATE", e.utc.substr(0, 4) + e.utc.substr(5, 2) + e.utc.substr(8, 2));
    adifField(out, "TIME_ON", e.utc.substr(11, 2) + e.utc.substr(14, 2));
  }
  char mhz[32];
  snprintf(mhz, sizeof mhz, "%.6f", e.hz / 1e6);
  adifField(out, "FREQ", mhz);
  adifField(out, "BAND", bands::adifBand(e.hz));
  // ADIF has no USB or USB-D mode. SSB, CW, AM, FM and RTTY are real names; a data mode could be any
  // digital mode, so it's left out rather than guessed, and the notes keep what the rig showed.
  std::string notes = e.notes;
  std::string mode;
  if (e.mode == "USB" || e.mode == "LSB") mode = "SSB";
  else if (e.mode == "CW" || e.mode == "CW-R") mode = "CW";
  else if (e.mode == "RTTY" || e.mode == "RTTY-R") mode = "RTTY";
  else if (e.mode == "AM") mode = "AM";
  else if (e.mode == "FM") mode = "FM";
  else if (e.mode == "DV") mode = "DIGITALVOICE", adifField(out, "SUBMODE", "DSTAR");
  else if (!e.mode.empty()) notes = notes.empty() ? "Rig mode " + e.mode : notes + " (rig mode " + e.mode + ")";
  adifField(out, "MODE", mode);
  adifField(out, "RST_SENT", e.rstSent);
  adifField(out, "RST_RCVD", e.rstRcvd);
  adifField(out, "NAME", e.name);
  adifField(out, "COMMENT", notes);
  adifField(out, "STATION_CALLSIGN", stationCall);
  out += "<EOR>\n";
  return out;
}

bool exportAdif(const std::string& path, const std::vector<LogEntry>& log, const std::string& stationCall) {
  std::string t = APP_NAME " log export\n<ADIF_VER:5>3.1.4 <PROGRAMID:" + std::to_string(sizeof(APP_NAME) - 1) + ">" APP_NAME " <EOH>\n";
  for (const LogEntry& e : log) t += adifRecord(e, stationCall);
  return saveText(path, t);
}

// ---- WAV ----
static void put32(FILE* f, uint32_t v) {
  uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
  fwrite(b, 1, 4, f);
}
static void put16(FILE* f, uint16_t v) {
  uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)};
  fwrite(b, 1, 2, f);
}

bool WavWriter::open(const std::string& path, int rate) {
  close();
  FILE* f = fopen(path.c_str(), "wb");
  if (!f) return false;
  f_ = f, rate_ = rate, frames_ = 0, path_ = path;
  fwrite("RIFF", 1, 4, f);
  put32(f, 36);
  fwrite("WAVEfmt ", 1, 8, f);
  put32(f, 16);
  put16(f, 1);
  put16(f, 1);
  put32(f, (uint32_t)rate);
  put32(f, (uint32_t)rate * 2);
  put16(f, 2);
  put16(f, 16);
  fwrite("data", 1, 4, f);
  put32(f, 0);
  return true;
}

void WavWriter::write(const float* s, size_t n) {
  if (!f_) return;
  FILE* f = (FILE*)f_;
  for (size_t i = 0; i < n; i++) {
    float v = s[i] * 32767.f;
    v = v > 32767.f ? 32767.f : v < -32768.f ? -32768.f : v;
    put16(f, (uint16_t)(int16_t)v);
  }
  frames_ += n;
}

void WavWriter::close() {
  if (!f_) return;
  FILE* f = (FILE*)f_;
  uint32_t bytes = (uint32_t)(frames_ * 2);
  fseek(f, 4, SEEK_SET);
  put32(f, 36 + bytes);
  fseek(f, 40, SEEK_SET);
  put32(f, bytes);
  fclose(f);
  f_ = nullptr;
}
}  // namespace store
