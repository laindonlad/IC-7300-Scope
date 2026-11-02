#include "store.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <ctype.h>
#include <filesystem>
#include <fstream>
#include <map>
#include <math.h>
#include <sstream>

#include "bands.h"

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#include <commdlg.h>
#include <objbase.h>
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

void* activeWindow() {
#ifdef _WIN32
  return (void*)GetActiveWindow();
#else
  return nullptr;
#endif
}

#ifndef _WIN32
// The first line a command prints, and whether it could be run and said yes.
static bool commandLine(const char* cmd, std::string& line) {
  FILE* p = popen(cmd, "r");
  if (!p) return false;
  char b[4096];
  if (fgets(b, sizeof b, p)) line = b;
  bool ok = pclose(p) == 0;
  while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
  if (!ok) line.clear();
  return ok;
}
#endif

bool pickAdifFile(void* owner, std::string& path) {
  path.clear();
#ifdef _WIN32
  // The dialog's shell extensions want a single-threaded apartment on the thread that shows it.
  HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  static wchar_t name[32768];
  name[0] = 0;
  OPENFILENAMEW o;
  memset(&o, 0, sizeof o);
  o.lStructSize = sizeof o;
  o.hwndOwner = (HWND)owner;
  o.lpstrFilter = L"ADIF logs (*.adi, *.adif)\0*.adi;*.adif\0All files\0*.*\0";
  o.lpstrFile = name;
  o.nMaxFile = sizeof name / sizeof name[0];
  o.lpstrTitle = L"Choose an ADIF log to import";
  o.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
  if (GetOpenFileNameW(&o)) {
    int n = WideCharToMultiByte(CP_UTF8, 0, name, -1, nullptr, 0, nullptr, nullptr);
    if (n > 1) {
      path.resize((size_t)n - 1);
      WideCharToMultiByte(CP_UTF8, 0, name, -1, &path[0], n, nullptr, nullptr);
    }
  }
  if (SUCCEEDED(com)) CoUninitialize();
  return true;
#elif defined(__APPLE__)
  (void)owner;
  commandLine("osascript -e 'POSIX path of (choose file with prompt \"Choose an ADIF log to import\")' 2>/dev/null", path);
  return true;
#else
  (void)owner;
  std::string found;
  if (commandLine("command -v zenity", found)) {
    commandLine("zenity --file-selection --title='Choose an ADIF log to import' --file-filter='ADIF logs | *.adi *.adif *.ADI *.ADIF' --file-filter='All files | *' 2>/dev/null", path);
    return true;
  }
  if (commandLine("command -v kdialog", found)) {
    commandLine("kdialog --title 'Choose an ADIF log to import' --getopenfilename . '*.adi *.adif *.ADI *.ADIF|ADIF logs' 2>/dev/null", path);
    return true;
  }
  return false;
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
    std::string* later[] = {&e.utcOff, &e.power, &e.qth, &e.locator, &e.country, &e.contest, &e.exchSent, &e.exchRcvd};
    for (size_t i = 0; i < 8 && 8 + i < p.size(); i++) *later[i] = p[8 + i];
    std::string* qsl[] = {&e.station, &e.qslSent, &e.qslRcvd, &e.qslSentDate, &e.qslRcvdDate, &e.lotwSent, &e.lotwRcvd, &e.lotwSentDate, &e.lotwRcvdDate, &e.eqslSent, &e.eqslRcvd, &e.eqslSentDate, &e.eqslRcvdDate};
    for (size_t i = 0; i < 13 && 16 + i < p.size(); i++) *qsl[i] = p[16 + i];
    if (p.size() > 29) e.distKm = p[29];
    if (p.size() > 30) e.distMiles = p[30];
    l.push_back(e);
  }
  return l;
}
static std::string logLine(const LogEntry& e) {
  return joinCsv({e.utc, e.call, std::to_string(e.hz), e.mode, e.rstSent, e.rstRcvd, e.name, e.notes, e.utcOff, e.power, e.qth, e.locator, e.country, e.contest, e.exchSent, e.exchRcvd,
                  e.station, e.qslSent, e.qslRcvd, e.qslSentDate, e.qslRcvdDate, e.lotwSent, e.lotwRcvd, e.lotwSentDate, e.lotwRcvdDate, e.eqslSent, e.eqslRcvd, e.eqslSentDate, e.eqslRcvdDate, e.distKm, e.distMiles}) + "\n";
}
// V is what older ADIF used for a card checked for an award: confirmed all the same.
static bool isConfirmed(const std::string& status) { return status == "Y" || status == "V"; }
std::string confirmedBy(const LogEntry& e) {
  std::string out;
  auto add = [&](bool yes, const char* who) {
    if (yes) out += (out.empty() ? "" : ", ") + std::string(who);
  };
  add(isConfirmed(e.lotwRcvd), "LoTW");
  add(isConfirmed(e.eqslRcvd), "eQSL");
  add(isConfirmed(e.qslRcvd), "QSL");
  return out;
}
static const struct {
  const char* name;
  bool dayFirst, yearFirst;
  char separator;
} kDateFormats[kDateFormatCount] = {
    {"DD-MM-YYYY", true, false, '-'}, {"DD/MM/YYYY", true, false, '/'}, {"DD.MM.YYYY", true, false, '.'},
    {"MM-DD-YYYY", false, false, '-'}, {"MM/DD/YYYY", false, false, '/'}, {"YYYY-MM-DD", false, true, '-'},
};
const char* dateFormatName(int format) { return format >= 0 && format < kDateFormatCount ? kDateFormats[format].name : ""; }
std::string displayDate(const std::string& utc, int format, bool withYear) {
  if (format < 0 || format >= kDateFormatCount || utc.size() < 10 || utc[4] != '-' || utc[7] != '-') return utc;
  const auto& f = kDateFormats[format];
  const std::string year = utc.substr(0, 4), month = utc.substr(5, 2), day = utc.substr(8, 2), time = utc.substr(10);
  const std::string dayMonth = f.dayFirst ? day + f.separator + month : month + f.separator + day;
  if (!withYear) return dayMonth + time;
  return (f.yearFirst ? year + f.separator + dayMonth : dayMonth + f.separator + year) + time;
}
bool backupFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return true;
  std::stringstream all;
  all << f.rdbuf();
  return saveText(path + ".bak", all.str());
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

std::string adifRecord(const LogEntry& e, const std::string& stationCall, const std::string& stationGrid) {
  std::string out;
  adifField(out, "CALL", e.call);
  // "2026-10-31 19:05" to 20261031 and 1905
  if (e.utc.size() >= 16) {
    adifField(out, "QSO_DATE", e.utc.substr(0, 4) + e.utc.substr(5, 2) + e.utc.substr(8, 2));
    adifField(out, "TIME_ON", e.utc.substr(11, 2) + e.utc.substr(14, 2));
  }
  if (e.utcOff.size() >= 16) {
    adifField(out, "QSO_DATE_OFF", e.utcOff.substr(0, 4) + e.utcOff.substr(5, 2) + e.utcOff.substr(8, 2));
    adifField(out, "TIME_OFF", e.utcOff.substr(11, 2) + e.utcOff.substr(14, 2));
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
  else if (e.mode == "FT4" || e.mode == "JS8") mode = "MFSK", adifField(out, "SUBMODE", e.mode);  // how ADIF files these
  else if (e.mode == "FT8" || e.mode == "PSK31" || e.mode == "PSK" || e.mode == "JT65" || e.mode == "JT9" || e.mode == "WSPR" || e.mode == "SSTV" || e.mode == "OLIVIA" || e.mode == "MFSK")
    mode = e.mode == "PSK31" ? "PSK" : e.mode;  // typed in the log book, or imported: the operator knows what it was
  else if (!e.mode.empty()) notes = notes.empty() ? "Rig mode " + e.mode : notes + " (rig mode " + e.mode + ")";
  adifField(out, "MODE", mode);
  adifField(out, "RST_SENT", e.rstSent);
  adifField(out, "RST_RCVD", e.rstRcvd);
  adifField(out, "NAME", e.name);
  adifField(out, "QTH", e.qth);
  adifField(out, "GRIDSQUARE", e.locator);
  adifField(out, "COUNTRY", e.country);
  adifField(out, "TX_PWR", e.power);
  adifField(out, "CONTEST_ID", e.contest);
  adifField(out, "STX_STRING", e.exchSent);
  adifField(out, "SRX_STRING", e.exchRcvd);
  adifField(out, "COMMENT", notes);
  adifField(out, "QSL_SENT", e.qslSent);
  adifField(out, "QSLSDATE", e.qslSentDate);
  adifField(out, "QSL_RCVD", e.qslRcvd);
  adifField(out, "QSLRDATE", e.qslRcvdDate);
  adifField(out, "LOTW_QSL_SENT", e.lotwSent);
  adifField(out, "LOTW_QSLSDATE", e.lotwSentDate);
  adifField(out, "LOTW_QSL_RCVD", e.lotwRcvd);
  adifField(out, "LOTW_QSLRDATE", e.lotwRcvdDate);
  adifField(out, "EQSL_QSL_SENT", e.eqslSent);
  adifField(out, "EQSL_QSLSDATE", e.eqslSentDate);
  adifField(out, "EQSL_QSL_RCVD", e.eqslRcvd);
  adifField(out, "EQSL_QSLRDATE", e.eqslRcvdDate);
  adifField(out, "STATION_CALLSIGN", e.station.empty() ? stationCall : e.station);
  adifField(out, "MY_GRIDSQUARE", stationGrid);
  out += "<EOR>\n";
  return out;
}

bool exportAdif(const std::string& path, const std::vector<LogEntry>& log, const std::string& stationCall, const std::string& stationGrid) {
  std::string t = APP_NAME " log export\n<ADIF_VER:5>3.1.4 <PROGRAMID:" + std::to_string(sizeof(APP_NAME) - 1) + ">" APP_NAME " <EOH>\n";
  for (const LogEntry& e : log) t += adifRecord(e, stationCall, stationGrid);
  return saveText(path, t);
}

// "20261031" and "1905" or "190533" to "2026-10-31 19:05"
static std::string adifTime(const std::string& date, const std::string& t) {
  if (date.size() < 8 || t.size() < 4) return "";
  return date.substr(0, 4) + "-" + date.substr(4, 2) + "-" + date.substr(6, 2) + " " + t.substr(0, 2) + ":" + t.substr(2, 2);
}

// Somewhere in a band, for a record that gives only the band's name.
static long long adifBandHz(const std::string& band) {
  static const struct {
    const char* name;
    long long hz;
  } t[] = {{"2190M", 136000},    {"630M", 475000},    {"160M", 1850000},   {"80M", 3600000},   {"60M", 5350000},    {"40M", 7100000},
           {"30M", 10120000},    {"20M", 14100000},   {"17M", 18100000},   {"15M", 21100000},  {"12M", 24900000},   {"10M", 28500000},
           {"6M", 50100000},     {"4M", 70100000},    {"2M", 144300000},   {"70CM", 432100000}, {"23CM", 1296100000}};
  for (const auto& b : t)
    if (band == b.name) return b.hz;
  return 0;
}

// Everything before <EOH>, in capitals, or "" if there's no header.
static std::string adifHeader(const std::string& text) {
  std::string all = text;
  for (char& c : all) c = (char)toupper((unsigned char)c);
  size_t eoh = all.find("<EOH>");
  return eoh == std::string::npos ? "" : all.substr(0, eoh);
}

// The header's PROGRAMID, from a header adifHeader gave.
static std::string adifProgram(const std::string& header) {
  size_t tag = header.find("<PROGRAMID:");
  if (tag == std::string::npos) return "";
  size_t gt = header.find('>', tag);
  if (gt == std::string::npos) return "";
  return header.substr(gt + 1, (size_t)atol(header.c_str() + tag + 11));
}

static bool isEqslInbox(const std::string& header) {
  return adifProgram(header).find("DOWNLOADINBOX") != std::string::npos || header.find("RECEIVED EQSLS") != std::string::npos;
}

std::vector<LogEntry> parseAdif(const std::string& text) {
  std::vector<LogEntry> out;
  const std::string header = adifHeader(text);
  const bool fromLotw = adifProgram(header) == "LOTW", fromEqslInbox = isEqslInbox(header);
  std::vector<std::pair<std::string, std::string>> fields;
  size_t at = 0;
  // A file with a header has <EOH> after it. One with none starts straight in on its first record.
  auto upperOf = [](std::string s) {
    for (char& c : s) c = (char)toupper((unsigned char)c);
    return s;
  };
  std::string all = upperOf(text);
  size_t eoh = all.find("<EOH>");
  if (eoh != std::string::npos) at = eoh + 5;
  while (true) {
    size_t lt = text.find('<', at);
    if (lt == std::string::npos) break;
    size_t gt = text.find('>', lt);
    if (gt == std::string::npos) break;
    std::string tag = upperOf(text.substr(lt + 1, gt - lt - 1));
    at = gt + 1;
    if (tag == "EOR") {
      LogEntry e;
      std::string date, on, dateOff, off, band, freq, mode, submode, comment, notes;
      for (const auto& f : fields) {
        const std::string& k = f.first;
        const std::string& v = f.second;
        if (k == "CALL") e.call = upperOf(v);
        else if (k == "QSO_DATE") date = v;
        else if (k == "TIME_ON") on = v;
        else if (k == "QSO_DATE_OFF") dateOff = v;
        else if (k == "TIME_OFF") off = v;
        else if (k == "FREQ") freq = v;
        else if (k == "BAND") band = upperOf(v);
        else if (k == "MODE") mode = upperOf(v);
        else if (k == "SUBMODE") submode = upperOf(v);
        else if (k == "RST_SENT") e.rstSent = v;
        else if (k == "RST_RCVD") e.rstRcvd = v;
        else if (k == "NAME") e.name = v;
        else if (k == "QTH") e.qth = v;
        else if (k == "GRIDSQUARE") e.locator = v;
        else if (k == "COUNTRY") e.country = v;
        else if (k == "TX_PWR") e.power = v;
        else if (k == "CONTEST_ID") e.contest = v;
        else if (k == "STX_STRING") e.exchSent = v;
        else if (k == "SRX_STRING") e.exchRcvd = v;
        else if (k == "STX" && e.exchSent.empty()) e.exchSent = v;
        else if (k == "SRX" && e.exchRcvd.empty()) e.exchRcvd = v;
        else if (k == "COMMENT") comment = v;
        else if (k == "NOTES") notes = v;
        else if (k == "STATION_CALLSIGN") e.station = upperOf(v);
        else if (k == "QSL_SENT") e.qslSent = upperOf(v);
        else if (k == "QSL_RCVD") e.qslRcvd = upperOf(v);
        else if (k == "QSLSDATE") e.qslSentDate = v;
        else if (k == "QSLRDATE") e.qslRcvdDate = v;
        else if (k == "LOTW_QSL_SENT") e.lotwSent = upperOf(v);
        else if (k == "LOTW_QSL_RCVD") e.lotwRcvd = upperOf(v);
        else if (k == "LOTW_QSLSDATE") e.lotwSentDate = v;
        else if (k == "LOTW_QSLRDATE") e.lotwRcvdDate = v;
        else if (k == "EQSL_QSL_SENT") e.eqslSent = upperOf(v);
        else if (k == "EQSL_QSL_RCVD") e.eqslRcvd = upperOf(v);
        else if (k == "EQSL_QSLSDATE") e.eqslSentDate = v;
        else if (k == "EQSL_QSLRDATE") e.eqslRcvdDate = v;
      }
      fields.clear();
      // A LoTW report's QSL_RCVD means LoTW has matched it. An eQSL inbox is written from the other station's
      // side, so its QSL_SENT is that station's card arriving here. Neither is a paper card.
      if (fromLotw) {
        if (e.lotwRcvd.empty()) e.lotwRcvd = e.qslRcvd, e.lotwRcvdDate = e.qslRcvdDate;
        e.qslRcvd.clear(), e.qslRcvdDate.clear();
      } else if (fromEqslInbox) {
        if (e.eqslRcvd.empty() && isConfirmed(e.qslSent)) e.eqslRcvd = "Y", e.eqslRcvdDate = e.qslSentDate;
        e.qslSent.clear(), e.qslSentDate.clear(), e.qslRcvd.clear(), e.qslRcvdDate.clear();
      }
      e.utc = adifTime(date, on);
      e.utcOff = adifTime(dateOff.empty() ? date : dateOff, off);
      e.hz = !freq.empty() ? llround(atof(freq.c_str()) * 1e6) : adifBandHz(band);
      e.notes = comment.empty() ? notes : comment;
      // The log keeps the mode as a rig shows it where that's possible.
      if (mode == "SSB") e.mode = submode == "LSB" || submode == "USB" ? submode : (e.hz > 0 && e.hz < 10000000 && !(e.hz >= 5000000 && e.hz < 5500000) ? "LSB" : "USB");
      else if (mode == "DIGITALVOICE" && submode == "DSTAR") e.mode = "DV";
      else e.mode = !submode.empty() ? submode : mode;
      if (!e.call.empty() && !e.utc.empty()) out.push_back(e);
      continue;
    }
    size_t c1 = tag.find(':');
    if (c1 == std::string::npos) continue;
    size_t len = (size_t)atol(tag.c_str() + c1 + 1);
    std::string value = text.substr(at, len);
    at = at + len < text.size() ? at + len : text.size();
    fields.push_back({tag.substr(0, c1), value});
  }
  return out;
}

// Minutes since 1970 of "YYYY-MM-DD HH:MM", or -1 if it isn't one. Counted rather than compared as text so that
// a contact either side of midnight is still a minute apart.
static long long utcMinutes(const std::string& s) {
  int y, mo, d, h, mi;
  if (sscanf(s.c_str(), "%d-%d-%d %d:%d", &y, &mo, &d, &h, &mi) != 5 || mo < 1 || mo > 12 || d < 1 || d > 31) return -1;
  y -= mo <= 2;
  long long era = (y >= 0 ? y : y - 399) / 400, yoe = y - era * 400;
  long long doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1, doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return (era * 146097 + doe - 719468) * 1440 + h * 60 + mi;
}

// Brings what a record adds to the contact it matched. Whatever the contact already says stays, as it may have
// been corrected here, except that a QSL status can improve. Once confirmed stays confirmed, so reloading an old
// file can't take it back. True if anything changed.
static bool mergeRecord(LogEntry& have, const LogEntry& in) {
  bool changed = false;
  auto fill = [&](std::string& a, const std::string& b) {
    if (a.empty() && !b.empty()) a = b, changed = true;
  };
  fill(have.station, in.station);
  fill(have.utcOff, in.utcOff), fill(have.mode, in.mode), fill(have.rstSent, in.rstSent), fill(have.rstRcvd, in.rstRcvd);
  fill(have.name, in.name), fill(have.qth, in.qth), fill(have.locator, in.locator), fill(have.country, in.country), fill(have.power, in.power);
  fill(have.contest, in.contest), fill(have.exchSent, in.exchSent), fill(have.exchRcvd, in.exchRcvd);
  // Not the note an export adds about a mode ADIF can't name: that isn't something the operator wrote.
  if (in.notes.compare(0, 9, "Rig mode ") != 0) fill(have.notes, in.notes);
  auto status = [&](std::string& s, std::string& date, const std::string& inS, const std::string& inDate) {
    if (inS.empty()) return;
    if (!isConfirmed(s) && s != inS) {
      s = inS, date = inDate, changed = true;
    } else if (isConfirmed(s) == isConfirmed(inS)) {
      fill(date, inDate);
    }
  };
  status(have.qslSent, have.qslSentDate, in.qslSent, in.qslSentDate);
  status(have.qslRcvd, have.qslRcvdDate, in.qslRcvd, in.qslRcvdDate);
  status(have.lotwSent, have.lotwSentDate, in.lotwSent, in.lotwSentDate);
  status(have.lotwRcvd, have.lotwRcvdDate, in.lotwRcvd, in.lotwRcvdDate);
  status(have.eqslSent, have.eqslSentDate, in.eqslSent, in.eqslSentDate);
  status(have.eqslRcvd, have.eqslRcvdDate, in.eqslRcvd, in.eqslRcvdDate);
  return changed;
}

bool importAdif(const std::string& path, std::vector<LogEntry>& log, const std::string& defaultStation, int& added, int& updated, int& skipped, std::string& error) {
  added = updated = skipped = 0;
  // By way of a path, so a name with accents opens on Windows too.
  std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
  if (!f) {
    error = "Couldn't open " + path;
    return false;
  }
  std::stringstream all;
  all << f.rdbuf();
  const std::string text = all.str();
  std::vector<LogEntry> records = parseAdif(text);
  if (records.empty()) {
    error = "There are no ADIF records in " + path;
    return false;
  }
  const long long window = isEqslInbox(adifHeader(text)) ? 30 : 1;
  auto keyOf = [](const LogEntry& e) {
    std::string call;
    for (char c : e.call)
      if (!isspace((unsigned char)c)) call += (char)toupper((unsigned char)c);
    return call + "|" + bands::adifBand(e.hz);
  };
  std::map<std::string, std::vector<size_t>> have;  // positions in the log, by call and band
  for (size_t i = 0; i < log.size(); i++) have[keyOf(log[i])].push_back(i);
  for (LogEntry& e : records) {
    std::vector<size_t>& same = have[keyOf(e)];
    const long long t = utcMinutes(e.utc);
    long long bestGap = window + 1;
    size_t best = 0;
    for (size_t i : same) {
      const LogEntry& h = log[i];
      if (!e.station.empty() && !h.station.empty() && e.station != h.station) continue;  // another of their callsigns
      const long long u = utcMinutes(h.utc);
      const long long gap = t >= 0 && u >= 0 ? (t > u ? t - u : u - t) : (e.utc == h.utc ? 0 : window + 1);
      if (gap < bestGap) bestGap = gap, best = i;
    }
    if (bestGap <= window) {
      if (mergeRecord(log[best], e)) updated++;
      else skipped++;
    } else {
      if (e.station.empty()) e.station = defaultStation;
      same.push_back(log.size());
      log.push_back(e);
      added++;
    }
  }
  std::stable_sort(log.begin(), log.end(), [](const LogEntry& a, const LogEntry& b) { return a.utc < b.utc; });
  return true;
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
