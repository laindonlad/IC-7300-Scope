#include "bands.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "civ.h"
#include "store.h"

#include "builtin_bandplans.inc"
#include "builtin_stations.inc"

namespace bands {
namespace {
// ---- just enough JSON, with line numbers in the errors, as people edit these by hand
struct Json {
  enum Type { Null, Bool, Number, String, Array, Object } type = Null;
  bool b = false;
  double num = 0;
  std::string str;
  std::vector<Json> arr;
  std::vector<std::pair<std::string, Json>> obj;
  const Json* get(const char* key) const {
    for (const auto& p : obj)
      if (p.first == key) return &p.second;
    return nullptr;
  }
};

class Parser {
 public:
  explicit Parser(const std::string& t) : s_(t) {}
  bool parse(Json& out, std::string& error) {
    // A byte order mark, as Notepad can leave.
    if (s_.compare(0, 3, "\xEF\xBB\xBF") == 0) p_ = 3;
    bool ok = value(out, 0);
    if (ok) {
      ws();
      if (p_ < s_.size()) ok = fail("unexpected text after the end");
    }
    if (!ok) error = "line " + std::to_string(line()) + ": " + err_;
    return ok;
  }

 private:
  int line() const { return 1 + (int)std::count(s_.begin(), s_.begin() + (long)std::min(p_, s_.size()), '\n'); }
  bool fail(const std::string& m) {
    if (err_.empty()) err_ = m;
    return false;
  }
  void ws() {
    while (p_ < s_.size() && strchr(" \t\r\n", s_[p_])) p_++;
  }
  bool value(Json& v, int depth) {
    if (depth > 16) return fail("nested too deeply");
    ws();
    if (p_ >= s_.size()) return fail("the file ends too soon");
    char c = s_[p_];
    if (c == '{') return object(v, depth);
    if (c == '[') return array(v, depth);
    if (c == '"') {
      v.type = Json::String;
      return string(v.str);
    }
    if (word("true")) {
      v.type = Json::Bool, v.b = true;
      return true;
    }
    if (word("false")) {
      v.type = Json::Bool, v.b = false;
      return true;
    }
    if (word("null")) return true;
    if (c == '-' || (c >= '0' && c <= '9')) {
      const char* start = s_.c_str() + p_;
      char* end = nullptr;
      v.type = Json::Number, v.num = strtod(start, &end);
      if (end == start) return fail("bad number");
      p_ += (size_t)(end - start);
      return true;
    }
    return fail(std::string("didn't expect '") + c + "' here");
  }
  bool word(const char* w) {
    size_t n = strlen(w);
    if (s_.compare(p_, n, w) != 0) return false;
    p_ += n;
    return true;
  }
  bool string(std::string& out) {
    p_++;
    while (p_ < s_.size()) {
      char c = s_[p_++];
      if (c == '"') return true;
      if (c == '\n') return fail("a string isn't closed on its line");
      if (c != '\\') {
        out += c;
        continue;
      }
      if (p_ >= s_.size()) break;
      char e = s_[p_++];
      switch (e) {
        case 'n': out += '\n'; break;
        case 't': out += '\t'; break;
        case 'r': out += '\r'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case '"': case '\\': case '/': out += e; break;
        case 'u': {
          if (p_ + 4 > s_.size()) return fail("bad \\u escape");
          unsigned cp = (unsigned)strtoul(s_.substr(p_, 4).c_str(), nullptr, 16);
          p_ += 4;
          if (cp < 0x80) out += (char)cp;
          else if (cp < 0x800) out += (char)(0xC0 | cp >> 6), out += (char)(0x80 | (cp & 63));
          else out += (char)(0xE0 | cp >> 12), out += (char)(0x80 | ((cp >> 6) & 63)), out += (char)(0x80 | (cp & 63));
          break;
        }
        default: return fail("bad escape in a string");
      }
    }
    return fail("the file ends inside a string");
  }
  bool array(Json& v, int depth) {
    v.type = Json::Array;
    p_++;
    ws();
    if (p_ < s_.size() && s_[p_] == ']') return p_++, true;
    while (true) {
      v.arr.emplace_back();
      if (!value(v.arr.back(), depth + 1)) return false;
      ws();
      if (p_ >= s_.size()) return fail("the file ends inside a list");
      char c = s_[p_++];
      if (c == ']') return true;
      if (c != ',') {
        p_--;
        return fail("expected a comma or ] in a list");
      }
    }
  }
  bool object(Json& v, int depth) {
    v.type = Json::Object;
    p_++;
    ws();
    if (p_ < s_.size() && s_[p_] == '}') return p_++, true;
    while (true) {
      ws();
      if (p_ >= s_.size() || s_[p_] != '"') return fail("expected a \"name\" here (is there a comma too many?)");
      std::string key;
      if (!string(key)) return false;
      ws();
      if (p_ >= s_.size() || s_[p_] != ':') return fail("expected a colon after \"" + key + "\"");
      p_++;
      v.obj.push_back({key, Json()});
      if (!value(v.obj.back().second, depth + 1)) return false;
      ws();
      if (p_ >= s_.size()) return fail("the file ends inside an object");
      char c = s_[p_++];
      if (c == '}') return true;
      if (c != ',') {
        p_--;
        return fail("expected a comma or } after \"" + key + "\"");
      }
    }
  }
  const std::string& s_;
  size_t p_ = 0;
  std::string err_;
};

// ---- from JSON to a plan
bool hzField(const Json& o, const char* key, long long& hz) {
  const Json* v = o.get(key);
  if (!v || v->type != Json::Number || !(v->num > 0) || v->num > 1e7) return false;
  hz = llround(v->num * 1000);
  return true;
}
std::string strField(const Json& o, const char* key) {
  const Json* v = o.get(key);
  return v && v->type == Json::String ? v->str : "";
}
std::string khzText(long long hz) {
  char b[32];
  snprintf(b, sizeof b, "%.3f", (double)hz / 1000);
  std::string s = b;
  while (s.back() == '0') s.pop_back();
  if (s.back() == '.') s.pop_back();
  return s;
}
bool parseUse(const std::string& s, Use& u) {
  static const struct {
    const char* name;
    Use use;
  } t[] = {{"cw", U_CW}, {"digi", U_DIGI}, {"ssb", U_SSB}, {"fm", U_FM}, {"beacon", U_BEACON}, {"sat", U_SAT}, {"other", U_OTHER}};
  for (const auto& e : t)
    if (s == e.name) {
      u = e.use;
      return true;
    }
  return false;
}
// LSB, USB, AM, FM, CW, CW-R, RTTY, RTTY-R, and -D on those that have a data mode.
bool parseMode(std::string s, int& mode, bool& data) {
  data = s.size() > 2 && s.compare(s.size() - 2, 2, "-D") == 0;
  if (data) s.resize(s.size() - 2);
  for (int m : {civ::LSB, civ::USB, civ::AM, civ::CW, civ::RTTY, civ::FM, civ::CW_R, civ::RTTY_R})
    if (s == civ::modeName(m)) {
      mode = m;
      return !data || civ::hasData(m);
    }
  return false;
}

bool parseBand(const Json& j, Band& b, std::string& error) {
  b.name = strField(j, "name");
  if (b.name.empty()) return error = "a band has no \"name\"", false;
  std::string where = "band " + b.name + ": ";
  const Json* segs = j.get("segments");
  if (!segs || segs->type != Json::Array || segs->arr.empty()) return error = where + "needs a list of \"segments\"", false;
  for (const Json& e : segs->arr) {
    Segment s;
    if (!hzField(e, "from", s.lo) || !hzField(e, "to", s.hi)) return error = where + "each segment needs \"from\" and \"to\", in kHz", false;
    if (s.lo >= s.hi) return error = where + "segment " + khzText(s.lo) + "-" + khzText(s.hi) + " ends before it begins", false;
    std::string use = strField(e, "use");
    if (!parseUse(use, s.use)) return error = where + "segment " + khzText(s.lo) + " has unknown \"use\" \"" + use + "\" (cw, digi, ssb, fm, beacon, sat or other)", false;
    s.label = strField(e, "label");
    if (s.label.empty()) s.label = useName(s.use);
    b.segments.push_back(s);
  }
  std::sort(b.segments.begin(), b.segments.end(), [](const Segment& x, const Segment& y) { return x.lo < y.lo; });
  for (size_t i = 1; i < b.segments.size(); i++)
    if (b.segments[i].lo < b.segments[i - 1].hi)
      return error = where + "segments overlap at " + khzText(b.segments[i].lo) + " kHz", false;
  b.lo = b.segments.front().lo, b.hi = b.segments.back().hi;

  bool data = false;
  if (!hzField(j, "home_khz", b.homeHz)) return error = where + "needs \"home_khz\": where its button goes first", false;
  if (!b.segmentAt(b.homeHz)) return error = where + "home_khz " + khzText(b.homeHz) + " isn't in any of its segments", false;
  std::string homeMode = strField(j, "home_mode");
  if (!parseMode(homeMode, b.homeMode, data)) return error = where + "unknown home_mode \"" + homeMode + "\"", false;

  if (const Json* marks = j.get("markers")) {
    if (marks->type != Json::Array) return error = where + "\"markers\" should be a list", false;
    for (const Json& e : marks->arr) {
      Marker m;
      if (!hzField(e, "khz", m.hz)) return error = where + "each marker needs \"khz\"", false;
      if (m.hz < b.lo || m.hz > b.hi) return error = where + "marker at " + khzText(m.hz) + " kHz is outside the band", false;
      m.label = strField(e, "label");
      if (m.label.empty()) return error = where + "marker at " + khzText(m.hz) + " kHz has no \"label\"", false;
      m.note = strField(e, "note");
      if (e.get("mode")) {
        std::string mode = strField(e, "mode");
        if (!parseMode(mode, m.mode, m.data)) return error = where + "marker at " + khzText(m.hz) + " kHz has unknown mode \"" + mode + "\"", false;
      }
      b.markers.push_back(m);
    }
  }
  return true;
}

std::string readFile(const std::filesystem::path& p, bool& ok) {
  std::ifstream f(p, std::ios::binary);
  ok = (bool)f;
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}
void sortByName(Library& l) {
  std::sort(l.plans.begin(), l.plans.end(), [](const Plan& a, const Plan& b) { return a.name < b.name; });
}
const char kFormatNotes[] =
    "IC-7300 Console band plans\n"
    "\n"
    "Each .json file here is one band plan. Edit one, press Reload in Settings, and the scope shows it.\n"
    "To add a country, copy a file to a new name, change its \"name\", and edit it. A file that can't be\n"
    "read is ignored, and Settings says which line is wrong. Reset there puts a built-in plan back, and\n"
    "keeps what it replaced as <name>.json.bak.\n"
    "\n"
    "All frequencies are in kHz.\n"
    "\n"
    "  name, revision, source, note   Shown in Settings. A newer revision in the program is pointed out.\n"
    "  bands                          One entry for each band. A band left out has no button.\n"
    "\n"
    "In a band:\n"
    "  name                  What the button says, such as \"20\". Memory of where you were goes by this.\n"
    "  home_khz, home_mode   Where the button goes the first time.\n"
    "  segments              from, to, use, and optionally a label.\n"
    "                        use is one of: cw digi ssb fm beacon sat other. It sets the colour and\n"
    "                        the plain name shown on the band (CW, Data, Voice, FM, Beacons, Satellite).\n"
    "                        The label is the detail shown under the mouse. With use \"other\", the\n"
    "                        label is shown on the band as well.\n"
    "                        Anywhere not in a segment shows as out of band, so leave gaps where a band\n"
    "                        is in separate blocks.\n"
    "  markers               khz, label, and optionally note and mode.\n"
    "                        A marker with a mode also goes on the band's menu, and clicking it sets\n"
    "                        that mode: LSB USB AM FM CW CW-R RTTY RTTY-R, or LSB-D USB-D AM-D FM-D.\n"
    "\n"
    "A band plan is a guide. Your licence is what says where you may transmit.\n";

// ---- from JSON to a list of stations
const long long kRigLowHz = 30000, kRigHighHz = 74800000;  // what the IC-7300 can receive

bool parseService(const std::string& s, Service& out) {
  static const struct {
    const char* name;
    Service service;
  } t[] = {{"commercial", S_COMMERCIAL}, {"government", S_GOVERNMENT}, {"naval", S_NAVAL}, {"military", S_MILITARY}, {"broadcast", S_BROADCAST}};
  for (const auto& e : t)
    if (s == e.name) {
      out = e.service;
      return true;
    }
  return false;
}
const char kServiceNames[] = "commercial, government, naval, military or broadcast";

bool parseStationGroup(const Json& j, StationFile& f, std::string& error) {
  Station base;
  base.group = strField(j, "name");
  if (base.group.empty()) return error = "a group has no \"name\"", false;
  std::string where = "group " + base.group + ": ";
  std::string service = strField(j, "service");
  if (!parseService(service, base.service)) return error = where + "unknown \"service\" \"" + service + "\" (" + kServiceNames + ")", false;
  std::string groupMode = strField(j, "mode"), groupNote = strField(j, "note");
  const Json* off = j.get("dial_offset_khz");
  if (off && off->type != Json::Number) return error = where + "\"dial_offset_khz\" should be a number", false;
  long long offsetHz = off ? llround(off->num * 1000) : 0;
  const Json* list = j.get("stations");
  if (!list || list->type != Json::Array || list->arr.empty()) return error = where + "needs a list of \"stations\"", false;
  for (const Json& e : list->arr) {
    Station s = base;
    if (!hzField(e, "khz", s.hz)) return error = where + "each station needs \"khz\"", false;
    std::string at = where + "station at " + khzText(s.hz) + " kHz ";
    s.label = strField(e, "label");
    if (s.label.empty()) return error = at + "has no \"label\"", false;
    std::string mode = e.get("mode") ? strField(e, "mode") : groupMode;
    if (!parseMode(mode, s.mode, s.data)) return error = at + "has unknown mode \"" + mode + "\"", false;
    if (e.get("dial_khz")) {
      if (!hzField(e, "dial_khz", s.dialHz)) return error = at + "has a bad \"dial_khz\"", false;
    } else if (offsetHz) {
      s.dialHz = s.hz + offsetHz;
    }
    if (s.hz < kRigLowHz || s.hz > kRigHighHz || s.tuneHz() < kRigLowHz || s.tuneHz() > kRigHighHz)
      return error = at + "is outside what the rig can receive (30 kHz to 74.8 MHz)", false;
    s.note = strField(e, "note");
    if (!groupNote.empty()) s.note += (s.note.empty() ? "" : "\n") + groupNote;
    f.stations.push_back(std::move(s));
  }
  return true;
}
const char kStationNotes[] =
    "IC-7300 Console stations\n"
    "\n"
    "Each .json file here is a list of stations, shown on the scope whichever band plan is chosen. Edit\n"
    "one, or add your own file beside it, and press Reload in Settings. They're for listening: nothing in\n"
    "them is a frequency to transmit on. Frequencies are in kHz.\n"
    "\n"
    "  name, revision, source, note   Shown in Settings.\n"
    "  groups                         Stations that belong together. Each makes a submenu.\n"
    "  ranges                         Stretches of spectrum, such as a broadcast band.\n"
    "\n"
    "In a group:\n"
    "  service           commercial, government, naval, military or broadcast. Sets the colour and the menu.\n"
    "  name, note        The note is shown under the mouse for every station in the group.\n"
    "  mode              LSB USB AM FM CW CW-R RTTY RTTY-R, or LSB-D USB-D AM-D FM-D.\n"
    "  dial_offset_khz   Optional, and usually negative: where to tune, relative to the frequency given.\n"
    "                    A radiofax signal listed at 8040 needs USB at 8038.1, so that's -1.9.\n"
    "  stations          khz, label, and optionally note, mode and dial_khz (which overrides the offset).\n"
    "\n"
    "In a range: from, to, service, label, and optionally note.\n";
void sortByName(Stations& s) {
  std::sort(s.files.begin(), s.files.end(), [](const StationFile& a, const StationFile& b) { return a.name < b.name; });
}
}  // namespace

const char* serviceName(Service s) {
  switch (s) {
    case S_COMMERCIAL: return "Commercial";
    case S_GOVERNMENT: return "Government";
    case S_NAVAL: return "Naval and maritime";
    case S_MILITARY: return "Military";
    case S_BROADCAST: return "Broadcast";
    default: return "";
  }
}

const StationFile* Stations::find(const std::string& id) const {
  for (const StationFile& f : files)
    if (f.id == id) return &f;
  return nullptr;
}

const StationRange* Stations::rangeAt(long long hz, const bool* shown) const {
  for (const StationFile& f : files)
    for (const StationRange& r : f.ranges)
      if (hz >= r.lo && hz <= r.hi && (!shown || shown[r.service])) return &r;
  return nullptr;
}

bool parseStations(const std::string& text, const std::string& id, StationFile& out, std::string& error) {
  Json j;
  Parser parser(text);
  if (!parser.parse(j, error)) return false;
  if (j.type != Json::Object) return error = "a station list should be one { } object", false;
  StationFile f;
  f.id = id;
  f.name = strField(j, "name");
  if (f.name.empty()) return error = "the list has no \"name\"", false;
  f.revision = strField(j, "revision"), f.source = strField(j, "source"), f.note = strField(j, "note");
  if (const Json* groups = j.get("groups")) {
    if (groups->type != Json::Array) return error = "\"groups\" should be a list", false;
    for (const Json& e : groups->arr)
      if (!parseStationGroup(e, f, error)) return false;
  }
  if (const Json* ranges = j.get("ranges")) {
    if (ranges->type != Json::Array) return error = "\"ranges\" should be a list", false;
    for (const Json& e : ranges->arr) {
      StationRange r;
      if (!hzField(e, "from", r.lo) || !hzField(e, "to", r.hi)) return error = "each range needs \"from\" and \"to\", in kHz", false;
      std::string at = "range at " + khzText(r.lo) + " kHz ";
      if (r.lo >= r.hi) return error = at + "ends before it begins", false;
      std::string service = strField(e, "service");
      if (!parseService(service, r.service)) return error = at + "has unknown \"service\" \"" + service + "\" (" + kServiceNames + ")", false;
      r.label = strField(e, "label");
      if (r.label.empty()) return error = at + "has no \"label\"", false;
      r.note = strField(e, "note");
      f.ranges.push_back(std::move(r));
    }
  }
  if (f.stations.empty() && f.ranges.empty()) return error = "the list has no \"groups\" or \"ranges\"", false;
  out = std::move(f);
  return true;
}

Stations builtinStations() {
  Stations l;
  for (const auto& b : kBuiltinStations) {
    StationFile f;
    std::string error;
    if (parseStations(std::string((const char*)b.data, b.size), b.id, f, error)) l.files.push_back(std::move(f));
    else l.errors.push_back(std::string(b.id) + " (built in): " + error);  // test_core sees this never happens
  }
  sortByName(l);
  return l;
}

Stations loadStations(const std::string& dir) {
  namespace fs = std::filesystem;
  Stations l = builtinStations();
  std::error_code ec;
  for (fs::directory_iterator it(fs::path(dir), ec), end; !ec && it != end; it.increment(ec)) {
    const fs::path& path = it->path();
    if (path.extension() != ".json") continue;
    std::string id = path.stem().string(), name = path.filename().string(), error;
    bool ok;
    std::string text = readFile(path, ok);
    StationFile f;
    if (!ok) {
      l.errors.push_back(name + ": can't be read");
    } else if (!parseStations(text, id, f, error)) {
      l.errors.push_back(name + ": " + error);
    } else {
      auto same = std::find_if(l.files.begin(), l.files.end(), [&](const StationFile& e) { return e.id == id; });
      if (same != l.files.end()) *same = std::move(f);
      else l.files.push_back(std::move(f));
    }
  }
  sortByName(l);
  return l;
}

void installStations(const std::string& dir) {
  namespace fs = std::filesystem;
  std::error_code ec;
  fs::path d(dir);
  fs::create_directories(d, ec);
  for (const auto& b : kBuiltinStations) {
    fs::path p = d / (std::string(b.id) + ".json");
    if (!fs::exists(p, ec)) store::saveText(p.string(), std::string((const char*)b.data, b.size));
  }
  store::saveText((d / "README.txt").string(), kStationNotes);
}

bool resetStations(const std::string& dir, const std::string& id) {
  namespace fs = std::filesystem;
  for (const auto& b : kBuiltinStations) {
    if (id != b.id) continue;
    std::error_code ec;
    fs::path p = fs::path(dir) / (id + ".json");
    fs::path bak = p;
    bak += ".bak";
    if (fs::exists(p, ec)) fs::rename(p, bak, ec);
    return store::saveText(p.string(), std::string((const char*)b.data, b.size));
  }
  return false;
}

std::vector<StationHit> Stations::search(const std::string& query) const {
  auto lower = [](std::string s) {
    for (char& c : s)
      if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
  };
  std::vector<std::string> words;
  std::stringstream ss(lower(query));
  for (std::string w; ss >> w;) words.push_back(w);
  auto matches = [&](const std::string& text) {
    for (const std::string& w : words)
      if (text.find(w) == std::string::npos) return false;
    return true;
  };
  // Whatever's named by the query comes before whatever only mentions it.
  auto named = [&](const std::string& label) { return !words.empty() && lower(label).find(words[0]) != std::string::npos; };
  std::vector<StationHit> hits;
  for (const StationFile& f : files) {
    for (const Station& s : f.stations)
      if (matches(lower(s.label + " " + s.group + " " + s.note + " " + serviceName(s.service) + " " + khzText(s.hz)))) hits.push_back({&s, nullptr, s.hz, named(s.label)});
    for (const StationRange& r : f.ranges)
      if (matches(lower(r.label + " " + r.note + " " + serviceName(r.service)))) hits.push_back({nullptr, &r, r.lo, named(r.label)});
  }
  std::stable_sort(hits.begin(), hits.end(), [](const StationHit& a, const StationHit& b) {
    return a.named != b.named ? a.named : a.hz < b.hz;
  });
  return hits;
}

const char* useName(Use u) {
  switch (u) {
    case U_CW: return "CW";
    case U_DIGI: return "Data";
    case U_SSB: return "Voice";
    case U_FM: return "FM";
    case U_BEACON: return "Beacons";
    case U_SAT: return "Satellite";
    default: return "Other";
  }
}

const Segment* Band::segmentAt(long long hz) const {
  for (const Segment& s : segments)
    if (hz >= s.lo && hz <= s.hi) return &s;
  return nullptr;
}

int Plan::bandOf(long long hz) const {
  for (size_t i = 0; i < bands.size(); i++)
    if (hz >= bands[i].lo && hz <= bands[i].hi) return (int)i;
  return -1;
}

const Segment* Plan::segmentAt(long long hz) const {
  int b = bandOf(hz);
  return b < 0 ? nullptr : bands[(size_t)b].segmentAt(hz);
}

const Plan* Library::find(const std::string& id) const {
  for (const Plan& p : plans)
    if (p.id == id) return &p;
  return nullptr;
}

bool parsePlan(const std::string& text, const std::string& id, Plan& out, std::string& error) {
  Json j;
  Parser parser(text);
  if (!parser.parse(j, error)) return false;
  if (j.type != Json::Object) return error = "a plan should be one { } object", false;
  Plan p;
  p.id = id;
  p.name = strField(j, "name");
  if (p.name.empty()) return error = "the plan has no \"name\"", false;
  p.revision = strField(j, "revision"), p.source = strField(j, "source"), p.note = strField(j, "note");
  const Json* bs = j.get("bands");
  if (!bs || bs->type != Json::Array || bs->arr.empty()) return error = "the plan needs a list of \"bands\"", false;
  for (const Json& e : bs->arr) {
    Band b;
    if (!parseBand(e, b, error)) return false;
    for (const Band& o : p.bands)
      if (o.name == b.name) return error = "band " + b.name + " appears twice", false;
    p.bands.push_back(std::move(b));
  }
  // Lowest first, as Page Up and Page Down expect, and no band inside another: the two would fight over
  // which button lights up.
  std::sort(p.bands.begin(), p.bands.end(), [](const Band& a, const Band& b) { return a.lo < b.lo; });
  for (size_t i = 1; i < p.bands.size(); i++)
    if (p.bands[i].lo <= p.bands[i - 1].hi) return error = "bands " + p.bands[i - 1].name + " and " + p.bands[i].name + " overlap", false;
  out = std::move(p);
  return true;
}

Library builtinPlans() {
  Library l;
  for (const auto& b : kBuiltinPlans) {
    Plan p;
    std::string error;
    if (parsePlan(std::string((const char*)b.data, b.size), b.id, p, error)) l.plans.push_back(std::move(p));
    else l.errors.push_back(std::string(b.id) + " (built in): " + error);  // test_core sees this never happens
  }
  sortByName(l);
  return l;
}

Library loadPlans(const std::string& dir) {
  namespace fs = std::filesystem;
  Library l = builtinPlans();
  std::error_code ec;
  for (fs::directory_iterator it(fs::path(dir), ec), end; !ec && it != end; it.increment(ec)) {
    const fs::path& path = it->path();
    if (path.extension() != ".json") continue;
    std::string id = path.stem().string(), name = path.filename().string(), error;
    bool ok;
    std::string text = readFile(path, ok);
    Plan p;
    if (!ok) {
      l.errors.push_back(name + ": can't be read");
    } else if (!parsePlan(text, id, p, error)) {
      l.errors.push_back(name + ": " + error);
    } else {
      auto same = std::find_if(l.plans.begin(), l.plans.end(), [&](const Plan& e) { return e.id == id; });
      if (same != l.plans.end()) *same = std::move(p);
      else l.plans.push_back(std::move(p));
    }
  }
  sortByName(l);
  return l;
}

void installPlans(const std::string& dir) {
  namespace fs = std::filesystem;
  std::error_code ec;
  fs::path d(dir);
  fs::create_directories(d, ec);
  for (const auto& b : kBuiltinPlans) {
    fs::path p = d / (std::string(b.id) + ".json");
    if (!fs::exists(p, ec)) store::saveText(p.string(), std::string((const char*)b.data, b.size));
  }
  store::saveText((d / "README.txt").string(), kFormatNotes);
}

bool resetPlan(const std::string& dir, const std::string& id) {
  namespace fs = std::filesystem;
  for (const auto& b : kBuiltinPlans) {
    if (id != b.id) continue;
    std::error_code ec;
    fs::path p = fs::path(dir) / (id + ".json");
    fs::path bak = p;
    bak += ".bak";
    if (fs::exists(p, ec)) fs::rename(p, bak, ec);
    return store::saveText(p.string(), std::string((const char*)b.data, b.size));
  }
  return false;
}

const char* adifBand(long long hz) {
  static const struct {
    const char* name;
    long long lo, hi;
  } t[] = {{"2190M", 135700, 137800},     {"630M", 472000, 479000},       {"160M", 1800000, 2000000},
           {"80M", 3500000, 4000000},     {"60M", 5060000, 5450000},      {"40M", 7000000, 7300000},
           {"30M", 10100000, 10150000},   {"20M", 14000000, 14350000},    {"17M", 18068000, 18168000},
           {"15M", 21000000, 21450000},   {"12M", 24890000, 24990000},    {"10M", 28000000, 29700000},
           {"6M", 50000000, 54000000},    {"4M", 70000000, 71000000}};
  for (const auto& b : t)
    if (hz >= b.lo && hz <= b.hi) return b.name;
  return "";
}
}  // namespace bands
