#include "locations.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include <algorithm>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>

#include "dxcluster.h"
#include "net.h"

namespace locations {

std::string normalCall(const std::string& call) {
  std::string s;
  for (char c : call)
    if (!isspace((unsigned char)c)) s += (char)toupper((unsigned char)c);
  return s;
}

std::string baseCall(const std::string& callIn) {
  const std::string call = normalCall(callIn);
  // The part with a digit and the most letters around it is the callsign; the rest are prefixes and suffixes.
  std::string best;
  size_t start = 0;
  while (start <= call.size()) {
    size_t end = call.find('/', start);
    if (end == std::string::npos) end = call.size();
    std::string part = call.substr(start, end - start);
    bool digit = false;
    for (char c : part) digit |= isdigit((unsigned char)c) != 0;
    if (digit && part.size() > best.size() && part.size() >= 3) best = part;
    start = end + 1;
  }
  return best.empty() ? call : best;
}

std::string latLonToGrid(double lat, double lon) {
  lon = fmod(lon + 180, 360);
  if (lon < 0) lon += 360;
  lat = std::max(0.0, std::min(179.9999, lat + 90));
  char g[7];
  g[0] = (char)('A' + (int)(lon / 20));
  g[1] = (char)('A' + (int)(lat / 10));
  g[2] = (char)('0' + (int)fmod(lon / 2, 10));
  g[3] = (char)('0' + (int)fmod(lat, 10));
  g[4] = (char)('a' + (int)fmod(lon * 12, 24));
  g[5] = (char)('a' + (int)fmod(lat * 24, 24));
  g[6] = 0;
  return g;
}

bool gridBounds(const std::string& grid, double& south, double& west, double& north, double& east) {
  std::string g = normalCall(grid);
  if (g.size() == 8 || g.size() == 10) g.resize(6);
  double lat, lon;
  if (!dx::gridToLatLon(g, lat, lon)) return false;
  const double w = g.size() == 6 ? 2.0 / 24 : 2.0, h = g.size() == 6 ? 1.0 / 24 : 1.0;
  west = lon - w / 2, east = lon + w / 2, south = lat - h / 2, north = lat + h / 2;
  return true;
}

// ---------------------------------------------------------------- the Ordnance Survey's National Grid

namespace {

const double kRad = M_PI / 180;

struct Ellipsoid {
  double a, b;
};
const Ellipsoid kGRS80 = {6378137.0, 6356752.314140};
const Ellipsoid kAiry = {6377563.396, 6356256.909};

// WGS84 to OSGB36; the other way is the same with every sign turned round. Good to a few metres.
struct Helmert {
  double tx, ty, tz, s, rx, ry, rz;  // metres, parts per million, arc seconds
};
const Helmert kToOSGB = {-446.448, 125.157, -542.060, 20.4894, -0.1502, -0.2470, -0.8421};

void toCartesian(double lat, double lon, const Ellipsoid& el, double& x, double& y, double& z) {
  const double e2 = 1 - el.b * el.b / (el.a * el.a), sp = sin(lat * kRad), cp = cos(lat * kRad);
  const double nu = el.a / sqrt(1 - e2 * sp * sp);
  x = nu * cp * cos(lon * kRad), y = nu * cp * sin(lon * kRad), z = nu * (1 - e2) * sp;
}

void fromCartesian(double x, double y, double z, const Ellipsoid& el, double& lat, double& lon) {
  const double e2 = 1 - el.b * el.b / (el.a * el.a), p = sqrt(x * x + y * y);
  double phi = atan2(z, p * (1 - e2));
  for (int i = 0; i < 10; i++) {
    const double nu = el.a / sqrt(1 - e2 * sin(phi) * sin(phi));
    phi = atan2(z + e2 * nu * sin(phi), p);
  }
  lat = phi / kRad, lon = atan2(y, x) / kRad;
}

void helmert(double& x, double& y, double& z, const Helmert& h, double sign) {
  const double sec = kRad / 3600, s = 1 + sign * h.s * 1e-6;
  const double rx = sign * h.rx * sec, ry = sign * h.ry * sec, rz = sign * h.rz * sec;
  const double nx = sign * h.tx + s * x - rz * y + ry * z;
  const double ny = sign * h.ty + rz * x + s * y - rx * z;
  const double nz = sign * h.tz - ry * x + rx * y + s * z;
  x = nx, y = ny, z = nz;
}

// The National Grid's transverse Mercator, on the Airy ellipsoid.
const double kF0 = 0.9996012717, kLat0 = 49 * kRad, kLon0 = -2 * kRad, kE0 = 400000, kN0 = -100000;

double meridianArc(double phi) {
  const double a = kAiry.a, b = kAiry.b, n = (a - b) / (a + b), n2 = n * n, n3 = n2 * n;
  const double d = phi - kLat0, s = phi + kLat0;
  return b * kF0 *
         ((1 + n + 1.25 * n2 + 1.25 * n3) * d - (3 * n + 3 * n2 + 21.0 / 8 * n3) * sin(d) * cos(s) +
          (15.0 / 8 * n2 + 15.0 / 8 * n3) * sin(2 * d) * cos(2 * s) - 35.0 / 24 * n3 * sin(3 * d) * cos(3 * s));
}

void osgbLatLonToGrid(double latDeg, double lonDeg, double& E, double& N) {
  const double a = kAiry.a, b = kAiry.b, e2 = 1 - b * b / (a * a);
  const double phi = latDeg * kRad, lam = lonDeg * kRad, sp = sin(phi), cp = cos(phi), tp = tan(phi);
  const double nu = a * kF0 / sqrt(1 - e2 * sp * sp), rho = a * kF0 * (1 - e2) / pow(1 - e2 * sp * sp, 1.5);
  const double eta2 = nu / rho - 1, M = meridianArc(phi);
  const double I = M + kN0, II = nu / 2 * sp * cp, III = nu / 24 * sp * pow(cp, 3) * (5 - tp * tp + 9 * eta2);
  const double IIIA = nu / 720 * sp * pow(cp, 5) * (61 - 58 * tp * tp + pow(tp, 4));
  const double IV = nu * cp, V = nu / 6 * pow(cp, 3) * (nu / rho - tp * tp);
  const double VI = nu / 120 * pow(cp, 5) * (5 - 18 * tp * tp + pow(tp, 4) + 14 * eta2 - 58 * tp * tp * eta2);
  const double dl = lam - kLon0;
  N = I + II * dl * dl + III * pow(dl, 4) + IIIA * pow(dl, 6);
  E = kE0 + IV * dl + V * pow(dl, 3) + VI * pow(dl, 5);
}

void osgbGridToLatLon(double E, double N, double& latDeg, double& lonDeg) {
  const double a = kAiry.a, b = kAiry.b, e2 = 1 - b * b / (a * a);
  double phi = kLat0, M = 0;
  do {
    phi = (N - kN0 - M) / (a * kF0) + phi;
    M = meridianArc(phi);
  } while (fabs(N - kN0 - M) >= 0.00001);
  const double sp = sin(phi), cp = cos(phi), tp = tan(phi), sec = 1 / cp;
  const double nu = a * kF0 / sqrt(1 - e2 * sp * sp), rho = a * kF0 * (1 - e2) / pow(1 - e2 * sp * sp, 1.5);
  const double eta2 = nu / rho - 1, t2 = tp * tp, t4 = t2 * t2, t6 = t4 * t2;
  const double VII = tp / (2 * rho * nu), VIII = tp / (24 * rho * pow(nu, 3)) * (5 + 3 * t2 + eta2 - 9 * t2 * eta2);
  const double IX = tp / (720 * rho * pow(nu, 5)) * (61 + 90 * t2 + 45 * t4);
  const double X = sec / nu, XI = sec / (6 * pow(nu, 3)) * (nu / rho + 2 * t2);
  const double XII = sec / (120 * pow(nu, 5)) * (5 + 28 * t2 + 24 * t4), XIIA = sec / (5040 * pow(nu, 7)) * (61 + 662 * t2 + 1320 * t4 + 720 * t6);
  const double dE = E - kE0;
  latDeg = (phi - VII * dE * dE + VIII * pow(dE, 4) - IX * pow(dE, 6)) / kRad;
  lonDeg = (kLon0 + X * dE - XI * pow(dE, 3) + XII * pow(dE, 5) - XIIA * pow(dE, 7)) / kRad;
}

}  // namespace

bool osGridFromLatLon(double lat, double lon, double& east, double& north) {
  double x, y, z, la, lo;
  toCartesian(lat, lon, kGRS80, x, y, z);
  helmert(x, y, z, kToOSGB, 1);
  fromCartesian(x, y, z, kAiry, la, lo);
  osgbLatLonToGrid(la, lo, east, north);
  return east >= 0 && east < 700000 && north >= 0 && north < 1300000;
}

std::string osGridRef(double E, double N, bool tenKm) {
  if (E < 0 || E >= 700000 || N < 0 || N >= 1300000) return "";
  const int e100 = (int)(E / 100000), n100 = (int)(N / 100000);
  int l1 = (19 - n100) - (19 - n100) % 5 + (e100 + 10) / 5;
  int l2 = (19 - n100) * 5 % 25 + e100 % 5;
  if (l1 > 7) l1++;  // there's no I
  if (l2 > 7) l2++;
  char b[8];
  if (tenKm) snprintf(b, sizeof b, "%c%c%d%d", 'A' + l1, 'A' + l2, (int)fmod(E, 100000) / 10000, (int)fmod(N, 100000) / 10000);
  else snprintf(b, sizeof b, "%c%c", 'A' + l1, 'A' + l2);
  return b;
}

bool osGridSquare(double lat, double lon, std::string& ref, double& east, double& north) {
  double E, N;
  if (!osGridFromLatLon(lat, lon, E, N)) return false;
  ref = osGridRef(E, N, true);
  east = floor(E / 10000) * 10000, north = floor(N / 10000) * 10000;
  return true;
}

void osGridToLatLon(double east, double north, double& lat, double& lon) {
  double la, lo, x, y, z;
  osgbGridToLatLon(east, north, la, lo);
  toCartesian(la, lo, kAiry, x, y, z);
  helmert(x, y, z, kToOSGB, -1);
  fromCartesian(x, y, z, kGRS80, lat, lon);
}

static size_t gridLength(const std::string& grid) {
  std::string g = normalCall(grid);
  return g.size() >= 6 ? 6 : g.size();
}

double suggestedZoom(const Location& l) {
  const std::string& a = l.accuracy;
  if (a == "user" || a == "geocode") return 15;
  if (a == "zip") return 11;
  if (a == "state") return 6;
  if (a == "dxcc" || a == "none") return 4;
  return gridLength(l.locator) == 6 ? 11.5 : 7.5;
}

std::string accuracyName(const Location& l) {
  const std::string& a = l.accuracy;
  if (a == "user") return "position set by the operator on QRZ";
  if (a == "geocode") return "street address";
  if (a == "zip") return "post code";
  if (a == "state") return "state or county";
  if (a == "dxcc" || a == "none") return "country only";
  return gridLength(l.locator) == 6 ? "6-character locator, about 5 km by 4" : "4-character locator, about 150 km by 110";
}

std::string utcNow() {
  time_t t = time(nullptr);
  struct tm u;
#ifdef _WIN32
  gmtime_s(&u, &t);
#else
  gmtime_r(&t, &u);
#endif
  char b[20];
  strftime(b, sizeof b, "%Y-%m-%d %H:%M", &u);
  return b;
}

// ---------------------------------------------------------------- the file

static const char kHeader[] = "call,lat,lon,locator,name,qth,country,source,accuracy,updated";

void Book::load(const std::string& path) {
  path_ = path;
  byCall_.clear();
  std::ifstream f(path, std::ios::binary);
  std::string line;
  while (std::getline(f, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line.compare(0, 5, "call,") == 0) continue;
    std::vector<std::string> v = store::splitCsv(line);
    if (v.size() < 3) continue;
    v.resize(10);
    Location l;
    l.call = normalCall(v[0]);
    char* end = nullptr;
    l.lat = strtod(v[1].c_str(), &end);
    if (end == v[1].c_str()) continue;
    l.lon = strtod(v[2].c_str(), &end);
    if (end == v[2].c_str() || l.call.empty()) continue;
    l.locator = v[3], l.name = v[4], l.qth = v[5], l.country = v[6], l.source = v[7], l.accuracy = v[8], l.updated = v[9];
    byCall_[l.call] = l;
  }
}

bool Book::save() const {
  std::string t = std::string(kHeader) + "\n";
  for (const auto& kv : byCall_) {
    const Location& l = kv.second;
    char lat[32], lon[32];
    snprintf(lat, sizeof lat, "%.6f", l.lat);
    snprintf(lon, sizeof lon, "%.6f", l.lon);
    t += store::joinCsv({l.call, lat, lon, l.locator, l.name, l.qth, l.country, l.source, l.accuracy, l.updated}) + "\n";
  }
  return store::saveText(path_, t);
}

const Location* Book::find(const std::string& call) const {
  auto it = byCall_.find(normalCall(call));
  return it == byCall_.end() ? nullptr : &it->second;
}

bool Book::put(const Location& l) {
  Location k = l;
  k.call = normalCall(l.call);
  if (k.call.empty()) return false;
  byCall_[k.call] = k;
  return save();
}

// ---------------------------------------------------------------- the log

static bool searchLog(const std::vector<store::LogEntry>& log, const std::string& call, Location& out) {
  const store::LogEntry* best = nullptr;
  double lat = 0, lon = 0;
  for (const store::LogEntry& e : log) {
    if (normalCall(e.call) != call) continue;
    double la, lo;
    if (!dx::gridToLatLon(e.locator, la, lo)) continue;
    if (best && e.utc < best->utc) continue;
    best = &e, lat = la, lon = lo;
  }
  if (!best) return false;
  out = Location();
  out.call = call;
  out.lat = lat, out.lon = lon;
  out.locator = normalCall(best->locator);
  if (out.locator.size() >= 6) {
    for (size_t i = 4; i < out.locator.size(); i++) out.locator[i] = (char)tolower((unsigned char)out.locator[i]);
  }
  out.name = best->name, out.qth = best->qth;
  out.country = best->country.empty() ? dx::countryOf(call) : best->country;
  out.source = "Log";
  out.accuracy = "locator";
  out.updated = utcNow();
  return true;
}

bool fromLog(const std::vector<store::LogEntry>& log, const std::string& callIn, Location& out) {
  const std::string call = normalCall(callIn);
  if (searchLog(log, call, out)) return true;
  const std::string base = baseCall(call);
  if (base != call && searchLog(log, base, out)) {
    out.call = call;
    return true;
  }
  return false;
}

// ---------------------------------------------------------------- QRZ.com

static std::string unescapeXml(const std::string& s) {
  std::string out;
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] != '&') {
      out += s[i];
      continue;
    }
    size_t semi = s.find(';', i);
    if (semi == std::string::npos || semi - i > 10) {
      out += s[i];
      continue;
    }
    std::string ent = s.substr(i + 1, semi - i - 1);
    if (ent == "amp") out += '&';
    else if (ent == "lt") out += '<';
    else if (ent == "gt") out += '>';
    else if (ent == "quot") out += '"';
    else if (ent == "apos") out += '\'';
    else if (!ent.empty() && ent[0] == '#') {
      long c = ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X') ? strtol(ent.c_str() + 2, nullptr, 16) : strtol(ent.c_str() + 1, nullptr, 10);
      // As UTF-8.
      if (c < 0x80) out += (char)c;
      else if (c < 0x800) out += (char)(0xC0 | (c >> 6)), out += (char)(0x80 | (c & 0x3F));
      else out += (char)(0xE0 | (c >> 12)), out += (char)(0x80 | ((c >> 6) & 0x3F)), out += (char)(0x80 | (c & 0x3F));
    } else {
      out += s.substr(i, semi - i + 1);
    }
    i = semi;
  }
  return out;
}

// The text of the first <tag> in xml, after from. Empty if there's none.
static std::string tag(const std::string& xml, const char* name, size_t from = 0) {
  const std::string open = std::string("<") + name + ">", close = std::string("</") + name + ">";
  size_t a = xml.find(open, from);
  if (a == std::string::npos) return "";
  a += open.size();
  size_t b = xml.find(close, a);
  if (b == std::string::npos) return "";
  return unescapeXml(xml.substr(a, b - a));
}

static const char kQrzUrl[] = "https://xmldata.qrz.com/xml/current/";

static bool qrzLogin(const std::string& user, const std::string& password, std::string& key, std::string& error) {
  std::string body;
  if (!net::httpGet(std::string(kQrzUrl) + "?username=" + net::urlEscape(user) + "&password=" + net::urlEscape(password) + "&agent=IC7300Console", body, error, 1 << 16)) {
    error = "QRZ.com: " + error;
    return false;
  }
  size_t session = body.find("<Session>");
  key = session == std::string::npos ? "" : tag(body, "Key", session);
  if (key.empty()) {
    std::string why = session == std::string::npos ? "" : tag(body, "Error", session);
    error = "QRZ.com wouldn't log in" + (why.empty() ? std::string(".") : ": " + why);
    return false;
  }
  return true;
}

enum Found { FOUND, FOUND_NO_PLACE, NOT_FOUND, STALE, FAILED };

// One lookup with a session key. FOUND_NO_PLACE fills in everything but where, which a free account often gets.
static Found qrzFind(const std::string& key, const std::string& call, Location& out, std::string& error) {
  std::string body;
  if (!net::httpGet(std::string(kQrzUrl) + "?s=" + net::urlEscape(key) + "&callsign=" + net::urlEscape(call), body, error, 1 << 18)) {
    error = "QRZ.com: " + error;
    return FAILED;
  }
  size_t session = body.find("<Session>");
  std::string sessionError = session == std::string::npos ? "" : tag(body, "Error", session);
  if (session == std::string::npos || tag(body, "Key", session).empty()) {
    error = "QRZ.com: " + (sessionError.empty() ? std::string("the session ended") : sessionError);
    return STALE;
  }
  size_t rec = body.find("<Callsign>");
  if (rec == std::string::npos) {
    error = sessionError.empty() ? "QRZ.com has no " + call : "QRZ.com: " + sessionError;
    return NOT_FOUND;
  }
  out = Location();
  out.call = normalCall(call);
  std::string fname = tag(body, "fname", rec), name = tag(body, "name", rec), nick = tag(body, "nickname", rec);
  out.name = fname.empty() ? name : name.empty() ? fname : fname + " " + name;
  if (out.name.empty()) out.name = nick;
  std::string town = tag(body, "addr2", rec), state = tag(body, "state", rec), county = tag(body, "county", rec);
  out.qth = town;
  if (!state.empty()) out.qth += (out.qth.empty() ? "" : ", ") + state;
  else if (!county.empty()) out.qth += (out.qth.empty() ? "" : ", ") + county;
  out.country = tag(body, "country", rec);
  out.locator = tag(body, "grid", rec);
  out.accuracy = tag(body, "geoloc", rec);
  out.source = "QRZ";
  out.updated = utcNow();
  std::string lat = tag(body, "lat", rec), lon = tag(body, "lon", rec);
  char* endLat = nullptr;
  char* endLon = nullptr;
  double la = strtod(lat.c_str(), &endLat), lo = strtod(lon.c_str(), &endLon);
  if (!lat.empty() && !lon.empty() && endLat != lat.c_str() && endLon != lon.c_str() && fabs(la) <= 90 && fabs(lo) <= 180 && !(la == 0 && lo == 0)) {
    out.lat = la, out.lon = lo;
    if (out.locator.empty()) out.locator = latLonToGrid(la, lo);
  } else if (dx::gridToLatLon(out.locator, la, lo)) {
    out.lat = la, out.lon = lo;
    out.accuracy = "grid";
  } else {
    std::string message = tag(body, "Message", session);
    error = "QRZ.com has " + out.call + " but gives no locator or position" + (message.empty() ? std::string(".") : " (" + message + ")");
    return FOUND_NO_PLACE;
  }
  if (out.accuracy.empty()) out.accuracy = "grid";
  return FOUND;
}

// ---------------------------------------------------------------- QRZ.com's own page for a callsign

// The Detail tab of a callsign's page has its locator for anyone logged in, subscriber or not. This logs in as the
// operator would in a browser, once for the program's run, and reads that tab: one page for each callsign looked
// up, and only for those the XML service gave no place for, which are then kept in locations.csv.
namespace web {

std::mutex m;
std::unique_ptr<net::Session> session;
std::string loggedInAs;  // empty until logged in
const char kSite[] = "https://www.qrz.com";
const char kAjax[] = "X-Requested-With: XMLHttpRequest\r\nReferer: https://www.qrz.com/login\r\nOrigin: https://www.qrz.com\r\n";

std::string stripTags(const std::string& html) {
  std::string out;
  bool inTag = false;
  for (char c : html) {
    if (c == '<') inTag = true;
    else if (c == '>') inTag = false, out += ' ';
    else if (!inTag) out += c;
  }
  out = unescapeXml(out);
  for (char& c : out)
    if (c == '\r' || c == '\n' || c == '\t' || (unsigned char)c == 0xA0) c = ' ';
  size_t a = out.find_first_not_of(' '), b = out.find_last_not_of(' ');
  if (a == std::string::npos) return "";
  out = out.substr(a, b - a + 1);
  // &nbsp; comes through as UTF-8's two bytes.
  std::string tidy;
  for (size_t i = 0; i < out.size(); i++) {
    if ((unsigned char)out[i] == 0xC2 && i + 1 < out.size() && (unsigned char)out[i + 1] == 0xA0) {
      tidy += ' ';
      i++;
    } else {
      tidy += out[i];
    }
  }
  return tidy;
}

// The text of the table cell after the one labelled label, or empty.
std::string cellAfter(const std::string& html, size_t from, const char* label) {
  const std::string want = std::string(">") + label;
  size_t at = from;
  while ((at = html.find(want, at)) != std::string::npos) {
    size_t end = at + want.size();
    // The label is the whole of its cell, give or take a colon or spaces.
    size_t close = html.find('<', end);
    if (close == std::string::npos) return "";
    std::string rest = html.substr(end, close - end);
    if (rest.find_first_not_of(" :\t\r\n") != std::string::npos) {
      at = end;
      continue;
    }
    size_t cell = html.find("<td", close);
    size_t cellEnd = cell == std::string::npos ? cell : html.find("</td>", cell);
    if (cellEnd == std::string::npos) return "";
    size_t open = html.find('>', cell);
    return stripTags(html.substr(open + 1, cellEnd - open - 1));
  }
  return "";
}

// "51.47 N", "-0.12", "0.12 W" and the like.
bool degrees(const std::string& s, double limit, double& v) {
  const char* p = s.c_str();
  char* end = nullptr;
  v = strtod(p, &end);
  if (end == p || fabs(v) > limit) return false;
  for (const char* q = end; *q; q++)
    if (*q == 'S' || *q == 's' || *q == 'W' || *q == 'w') v = -fabs(v);
  return true;
}

// A JSON answer from the login handshake: true unless it says error. message is any message it gives.
bool handshakeOk(const std::string& json, std::string& message, bool& twoFactor) {
  auto value = [&](const char* key) {
    const std::string k = std::string("\"") + key + "\"";
    size_t at = json.find(k);
    if (at == std::string::npos) return std::string();
    at = json.find(':', at + k.size());
    if (at == std::string::npos) return std::string();
    at = json.find_first_not_of(" \t", at + 1);
    if (at == std::string::npos) return std::string();
    if (json[at] == '"') {
      std::string out;
      for (size_t i = at + 1; i < json.size() && json[i] != '"'; i++) {
        if (json[i] == '\\' && i + 1 < json.size()) i++;
        out += json[i];
      }
      return out;
    }
    size_t end = json.find_first_of(",}", at);
    return json.substr(at, end - at);
  };
  message = stripTags(value("message"));
  twoFactor = value("twofactor") == "true";
  return value("error") != "true";
}

bool login(const std::string& user, const std::string& password, std::string& error) {
  session.reset(new net::Session);
  loggedInAs.clear();
  std::string page;
  if (!session->get(std::string(kSite) + "/login", page, error)) {
    error = "QRZ.com's sign-in page: " + error;
    return false;
  }
  size_t t = page.find("'loginTicket'");
  t = t == std::string::npos ? t : page.find('\'', page.find(':', t) + 1);
  size_t te = t == std::string::npos ? t : page.find('\'', t + 1);
  if (te == std::string::npos) {
    error = "QRZ.com's sign-in page has changed, so this can't sign in any more";
    return false;
  }
  const std::string ticket = page.substr(t + 1, te - t - 1);
  const std::string who = "loginTicket=" + net::urlEscape(ticket) + "&username=" + net::urlEscape(user);
  std::string reply, message;
  bool twoFactor = false;
  if (!session->post(std::string(kSite) + "/login-handshake", who + "&step=1", reply, error, kAjax)) {
    error = "Signing in to QRZ.com: " + error;
    return false;
  }
  if (!handshakeOk(reply, message, twoFactor)) {
    error = "QRZ.com wouldn't sign in: " + (message.empty() ? std::string("it didn't know the username") : message);
    return false;
  }
  if (!session->post(std::string(kSite) + "/login-handshake", who + "&password=" + net::urlEscape(password) + "&step=2", reply, error, kAjax)) {
    error = "Signing in to QRZ.com: " + error;
    return false;
  }
  if (!handshakeOk(reply, message, twoFactor)) {
    error = "QRZ.com wouldn't sign in: " + (message.empty() ? std::string("check the password in Settings") : message);
    return false;
  }
  if (twoFactor) {
    error = "Your QRZ.com account asks for a two-factor code to sign in, which this can't give";
    return false;
  }
  const std::string form = "login_ref=&username=" + net::urlEscape(user) + "&password=" + net::urlEscape(password) + "&target=%2F&flush=1";
  if (!session->post(std::string(kSite) + "/login", form, reply, error, "Referer: https://www.qrz.com/login\r\nOrigin: https://www.qrz.com\r\n")) {
    error = "Signing in to QRZ.com: " + error;
    return false;
  }
  loggedInAs = user;
  return true;
}

// Where the page for call says the station is.
bool lookup(const std::string& user, const std::string& password, const std::string& call, Location& out, std::string& error) {
  std::lock_guard<std::mutex> g(m);
  for (int attempt = 0; attempt < 2; attempt++) {
    if ((loggedInAs != user || !session) && !login(user, password, error)) return false;
    std::string page;
    if (!session->get(std::string(kSite) + "/db/" + net::urlEscape(call), page, error)) {
      error = "QRZ.com's page for " + call + ": " + error;
      return false;
    }
    if (page.find("Login is required for additional detail") != std::string::npos) {
      loggedInAs.clear();  // the sign-in has lapsed, or didn't take: once more
      if (attempt == 0) continue;
      store::saveText(store::dataDir() + "qrz-page.html", page);
      error = "QRZ.com didn't keep the sign-in, so the locator wasn't shown. The page is in qrz-page.html in the data folder.";
      return false;
    }
    size_t detail = page.find("id=\"t_detail\"");
    if (detail == std::string::npos) detail = 0;
    const std::string grid = cellAfter(page, detail, "Grid Square");
    const std::string latText = cellAfter(page, detail, "Latitude"), lonText = cellAfter(page, detail, "Longitude");
    std::string how = cellAfter(page, detail, "Geo Source");
    for (char& c : how) c = (char)tolower((unsigned char)c);
    double lat, lon;
    const bool haveGrid = dx::gridToLatLon(grid, lat, lon);
    double la, lo;
    const bool haveLatLon = degrees(latText, 90, la) && degrees(lonText, 180, lo) && !(la == 0 && lo == 0);
    if (!haveGrid && !haveLatLon) {
      if (page.find("produced no results") != std::string::npos || page.find("not found") != std::string::npos) {
        error = "QRZ.com has no " + call;
      } else {
        store::saveText(store::dataDir() + "qrz-page.html", page);
        error = "QRZ.com's page for " + call + " gives no locator. It's in qrz-page.html in the data folder.";
      }
      return false;
    }
    out.call = normalCall(call);
    out.source = "QRZ";
    out.updated = utcNow();
    if (haveLatLon) {
      out.lat = la, out.lon = lo;
      out.locator = haveGrid ? grid : latLonToGrid(la, lo);
    } else {
      out.lat = lat, out.lon = lon;
      out.locator = grid;
    }
    if (how.find("grid") != std::string::npos || !haveLatLon) out.accuracy = "grid";
    else if (how.find("user") != std::string::npos) out.accuracy = "user";
    else if (how.find("geocod") != std::string::npos || how.find("address") != std::string::npos) out.accuracy = "geocode";
    else if (how.find("zip") != std::string::npos || how.find("post") != std::string::npos) out.accuracy = "zip";
    else if (how.find("state") != std::string::npos || how.find("county") != std::string::npos) out.accuracy = "state";
    else if (how.find("dxcc") != std::string::npos || how.find("country") != std::string::npos) out.accuracy = "dxcc";
    else out.accuracy = "grid";
    if (out.country.empty()) out.country = dx::countryOf(out.call);
    return true;
  }
  return false;
}

}  // namespace web

bool qrzLookup(const std::string& user, const std::string& password, const std::string& callIn, std::string& sessionKey, Location& out, std::string& error) {
  if (user.empty() || password.empty()) {
    error = "Put your QRZ.com username and password in Settings to look callsigns up there";
    return false;
  }
  const std::string call = normalCall(callIn);
  std::string asked = call;  // the callsign QRZ knows it by, which may be without a prefix or suffix
  Found found = FAILED;
  Location record;
  for (int attempt = 0; attempt < 2; attempt++) {
    if (sessionKey.empty() && !qrzLogin(user, password, sessionKey, error)) break;
    found = qrzFind(sessionKey, call, record, error);
    if (found == NOT_FOUND) {
      // A callsign with a prefix or suffix that QRZ doesn't know may be there without it.
      const std::string base = baseCall(call);
      if (base != call) {
        found = qrzFind(sessionKey, base, record, error);
        if (found != NOT_FOUND) asked = base;
      }
    }
    if (found != STALE) break;
    sessionKey.clear();
  }
  if (found == FOUND) {
    out = record;
    out.call = call;
    return true;
  }
  if (found == NOT_FOUND) return false;
  // The XML service had no place for it, or couldn't be used: its page on the website may say.
  const std::string xmlError = error;
  Location place;
  if (!web::lookup(user, password, asked, place, error)) {
    if (found == FOUND_NO_PLACE) error = xmlError + " " + error;
    return false;
  }
  if (found == FOUND_NO_PLACE) {
    out = record;
    out.lat = place.lat, out.lon = place.lon, out.locator = place.locator, out.accuracy = place.accuracy;
  } else {
    out = place;
  }
  out.call = call;
  return true;
}

}  // namespace locations
