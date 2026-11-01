#include "dxcluster.h"

#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

namespace dx {
namespace {
std::string upper(std::string s) {
  for (char& c : s) c = (char)toupper((unsigned char)c);
  return s;
}
std::string trim(const std::string& s) {
  size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
  return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}
}  // namespace

// ---------------------------------------------------------------- reading spots

bool parseSpot(const std::string& lineIn, Spot& out) {
  std::string line = trim(lineIn);
  if (line.size() < 8 || upper(line.substr(0, 6)) != "DX DE ") return false;
  size_t colon = line.find(':', 6);
  if (colon == std::string::npos) return false;
  Spot s;
  s.spotter = upper(trim(line.substr(6, colon - 6)));
  if (s.spotter.empty()) return false;
  s.skimmer = s.spotter.size() > 2 && s.spotter.compare(s.spotter.size() - 2, 2, "-#") == 0;

  const char* p = line.c_str() + colon + 1;
  char* end = nullptr;
  double khz = strtod(p, &end);
  if (end == p || !(khz > 0) || khz > 3e6) return false;
  s.hz = llround(khz * 1000);

  size_t at = (size_t)(end - line.c_str());
  at = line.find_first_not_of(" \t", at);
  if (at == std::string::npos) return false;
  size_t callEnd = line.find_first_of(" \t", at);
  s.call = upper(line.substr(at, callEnd == std::string::npos ? std::string::npos : callEnd - at));
  if (s.call.size() < 3 || s.call.size() > 16) return false;
  std::string rest = callEnd == std::string::npos ? "" : line.substr(callEnd);

  // The time is the last thing but for an optional locator: four digits and a Z, on its own.
  size_t cut = rest.size();
  for (size_t i = rest.size(); i-- > 0;) {
    if (rest[i] != 'Z' && rest[i] != 'z') continue;
    if (i < 4 || (i + 1 < rest.size() && rest[i + 1] != ' ' && rest[i + 1] != '\t')) continue;
    bool digits = true;
    for (size_t k = i - 4; k < i; k++) digits = digits && isdigit((unsigned char)rest[k]);
    if (!digits || (i > 4 && rest[i - 5] != ' ' && rest[i - 5] != '\t')) continue;
    s.utc = rest.substr(i - 4, 4);
    cut = i - 4;
    break;
  }
  s.comment = trim(rest.substr(0, cut));
  out = s;
  return true;
}

ModeHint modeHint(const std::string& comment) {
  std::string word;
  std::string c = upper(comment) + " ";
  ModeHint found = NO_HINT;
  for (char ch : c) {
    if (isalnum((unsigned char)ch)) {
      word += ch;
      continue;
    }
    if (found == NO_HINT && !word.empty()) {
      if (word == "FT8") found = HINT_FT8;
      else if (word == "FT4") found = HINT_FT4;
      else if (word == "CW") found = HINT_CW;
      else if (word == "SSB" || word == "USB" || word == "LSB") found = HINT_SSB;
      else if (word == "FM") found = HINT_FM;
      else if (word == "RTTY") found = HINT_RTTY;
      else if (word.compare(0, 3, "PSK") == 0 || word == "JS8" || word == "JT65" || word == "JT9" || word == "MSK144" || word == "WSPR" || word == "SSTV" ||
               word == "OLIVIA" || word == "DIGI" || word == "DIGITAL" || word == "VARAC" || word == "Q65")
        found = HINT_DATA;
    }
    word.clear();
  }
  return found;
}

void SpotList::add(Spot s) {
  for (size_t i = 0; i < spots_.size(); i++)
    if (spots_[i].call == s.call && llabs(spots_[i].hz - s.hz) <= 1000) {
      s.count = spots_[i].count + 1;
      if (s.comment.empty()) s.comment = spots_[i].comment;
      spots_.erase(spots_.begin() + (long)i);
      break;
    }
  spots_.insert(spots_.begin(), std::move(s));
  if (spots_.size() > 1000) spots_.pop_back();
}

bool SpotList::prune(long long now, long long maxAge) {
  size_t before = spots_.size();
  spots_.erase(std::remove_if(spots_.begin(), spots_.end(), [&](const Spot& s) { return now - s.gotAt > maxAge; }), spots_.end());
  return spots_.size() != before;
}

// ---------------------------------------------------------------- countries

namespace {
// Each token is a prefix, or a run of them such as "DA-DR". Where two match, the longest wins.
const struct {
  const char* country;
  const char* prefixes;
} kPrefixes[] = {
    // Britain and Ireland
    {"England", "G M 2E"}, {"Scotland", "GM MM 2M GS MS"}, {"Wales", "GW MW 2W GC MC"}, {"Northern Ireland", "GI MI 2I GN MN"},
    {"Isle of Man", "GD MD 2D GT MT"}, {"Jersey", "GJ MJ 2J GH MH"}, {"Guernsey", "GU MU 2U GP MP"}, {"Ireland", "EI EJ"},
    // Western Europe
    {"France", "F TM TO-TQ"}, {"Corsica", "TK"}, {"Germany", "DA-DR Y2-Y9"}, {"Italy", "I"}, {"Sardinia", "IS0 IM0"}, {"Sicily", "IT9"},
    {"Spain", "EA-EH AM-AO"}, {"Balearic Islands", "EA6 EB6 EC6 ED6 EE6 EF6 EG6 EH6"}, {"Canary Islands", "EA8 EB8 EC8 ED8 EE8 EF8 EG8 EH8"},
    {"Ceuta and Melilla", "EA9 EB9 EC9 ED9 EE9 EF9 EG9 EH9"}, {"Portugal", "CT CQ CR CS"}, {"Madeira", "CT3 CQ3 CR3 CS3"}, {"Azores", "CU CT2"},
    {"Belgium", "ON-OT"}, {"Netherlands", "PA-PI"}, {"Luxembourg", "LX"}, {"Switzerland", "HB HE"}, {"Liechtenstein", "HB0 HE0"}, {"Austria", "OE"},
    {"Monaco", "3A"}, {"Andorra", "C3"}, {"San Marino", "T7"}, {"Vatican", "HV"}, {"Malta", "9H"},
    // Northern Europe
    {"Iceland", "TF"}, {"Faroe Islands", "OY"}, {"Greenland", "OX XP"}, {"Denmark", "OU-OW OZ 5P-5Q"}, {"Norway", "LA-LN"}, {"Svalbard", "JW"}, {"Jan Mayen", "JX"},
    {"Sweden", "SA-SM 7S 8S"}, {"Finland", "OF-OI"}, {"Aland Islands", "OH0 OF0 OG0 OI0"}, {"Market Reef", "OJ0"}, {"Estonia", "ES"}, {"Latvia", "YL"}, {"Lithuania", "LY"},
    // Central and eastern Europe
    {"Poland", "SN-SR HF 3Z"}, {"Czech Republic", "OK OL"}, {"Slovakia", "OM"}, {"Hungary", "HA HG"}, {"Romania", "YO-YR"}, {"Bulgaria", "LZ"}, {"Greece", "SV-SZ J4"},
    {"Dodecanese", "SV5 J45"}, {"Crete", "SV9 J49"}, {"Serbia", "YT YU"}, {"Croatia", "9A"}, {"Slovenia", "S5"}, {"Bosnia and Herzegovina", "E7"}, {"North Macedonia", "Z3"},
    {"Montenegro", "4O"}, {"Albania", "ZA"}, {"Kosovo", "Z6"}, {"Belarus", "EU-EW"}, {"Ukraine", "UR-UZ EM-EO"}, {"Moldova", "ER"}, {"Russia", "R UA-UI"},
    {"Kaliningrad", "UA2 RA2 R2F R2K"}, {"Georgia", "4L"}, {"Armenia", "EK"}, {"Azerbaijan", "4J 4K"}, {"Turkey", "TA-TC YM"}, {"Cyprus", "5B C4 H2 P3"},
    // Middle East
    {"Israel", "4X 4Z"}, {"Lebanon", "OD"}, {"Syria", "YK"}, {"Jordan", "JY"}, {"Iraq", "YI"}, {"Iran", "EP EQ"}, {"Saudi Arabia", "HZ 7Z 8Z"}, {"Kuwait", "9K"},
    {"Bahrain", "A9"}, {"Qatar", "A7"}, {"United Arab Emirates", "A6"}, {"Oman", "A4"}, {"Yemen", "7O"},
    // Asia
    {"Japan", "JA-JS 7J-7N 8J-8N"}, {"South Korea", "HL DS DT 6K-6N D7-D9"}, {"North Korea", "P5"}, {"China", "B"}, {"Taiwan", "BM-BQ BU-BX"}, {"Hong Kong", "VR"}, {"Macao", "XX9"},
    {"Mongolia", "JT-JV"}, {"India", "VU AT-AW 8T-8Y"}, {"Pakistan", "AP-AS 6P-6S"}, {"Bangladesh", "S2 S3"}, {"Sri Lanka", "4P-4S"}, {"Nepal", "9N"}, {"Bhutan", "A5"},
    {"Maldives", "8Q"}, {"Afghanistan", "YA T6"}, {"Thailand", "HS E2"}, {"Vietnam", "XV 3W"}, {"Laos", "XW"}, {"Cambodia", "XU"}, {"Myanmar", "XY XZ"},
    {"Malaysia", "9M 9W"}, {"Singapore", "9V S6"}, {"Indonesia", "YB-YH 7A-7I 8A-8I"}, {"Philippines", "DU-DZ 4D-4I"}, {"Brunei", "V8"}, {"Timor-Leste", "4W"},
    {"Kazakhstan", "UN-UQ"}, {"Uzbekistan", "UJ-UM"}, {"Kyrgyzstan", "EX"}, {"Tajikistan", "EY"}, {"Turkmenistan", "EZ"},
    // Oceania
    {"Australia", "VK AX VH-VN"}, {"New Zealand", "ZL ZK ZM"}, {"Papua New Guinea", "P2"}, {"Solomon Islands", "H4"}, {"Vanuatu", "YJ"}, {"New Caledonia", "FK"},
    {"French Polynesia", "FO"}, {"Wallis and Futuna", "FW"}, {"Fiji", "3D2"}, {"Tonga", "A3"}, {"Samoa", "5W"}, {"Tuvalu", "T2"}, {"Kiribati", "T3"}, {"Palau", "T8"},
    {"Micronesia", "V6"}, {"Marshall Islands", "V7"}, {"Nauru", "C2"}, {"Cook Islands", "E5"}, {"Niue", "E6"}, {"Pitcairn", "VP6"},
    // North America
    {"United States", "K N W AA-AK"}, {"Alaska", "KL AL NL WL"}, {"Hawaii", "KH6 KH7 AH6 AH7 NH6 NH7 WH6 WH7"}, {"Guam", "KH2 AH2 NH2 WH2"}, {"Northern Marianas", "KH0 AH0 NH0 WH0"},
    {"American Samoa", "KH8 AH8 NH8 WH8"}, {"Puerto Rico", "KP3 KP4 NP3 NP4 WP3 WP4"}, {"US Virgin Islands", "KP2 NP2 WP2"}, {"Canada", "VA-VG VO VX VY CF-CK CY CZ XJ-XO"},
    {"Mexico", "XA-XI 4A-4C 6D-6J"}, {"Bermuda", "VP9"}, {"Guatemala", "TG TD"}, {"Belize", "V3"}, {"Honduras", "HQ HR"}, {"El Salvador", "YS HU"}, {"Nicaragua", "YN H6 H7 HT"},
    {"Costa Rica", "TI TE"}, {"Panama", "HO HP H3 H8 H9 3E 3F"},
    // The Caribbean
    {"Cuba", "CL CM CO T4"}, {"Bahamas", "C6"}, {"Jamaica", "6Y"}, {"Haiti", "HH 4V"}, {"Dominican Republic", "HI"}, {"Cayman Islands", "ZF"}, {"Turks and Caicos", "VP5"},
    {"Anguilla", "VP2E"}, {"Montserrat", "VP2M"}, {"British Virgin Islands", "VP2V"}, {"St Kitts and Nevis", "V4"}, {"Antigua and Barbuda", "V2"}, {"Dominica", "J7"},
    {"St Lucia", "J6"}, {"St Vincent", "J8"}, {"Grenada", "J3"}, {"Barbados", "8P"}, {"Trinidad and Tobago", "9Y 9Z"}, {"Guadeloupe", "FG"}, {"Martinique", "FM"},
    {"St Martin", "FS"}, {"St Barthelemy", "FJ"}, {"Netherlands Antilles", "PJ"}, {"Aruba", "P4"},
    // South America
    {"Argentina", "LO-LW AY AZ L2-L9"}, {"Brazil", "PP-PY ZV-ZZ"}, {"Chile", "CA-CE XQ XR 3G"}, {"Uruguay", "CV-CX"}, {"Paraguay", "ZP"}, {"Bolivia", "CP"}, {"Peru", "OA-OC 4T"},
    {"Ecuador", "HC HD"}, {"Colombia", "HJ HK 5J 5K"}, {"Venezuela", "YV-YY 4M"}, {"Guyana", "8R"}, {"Suriname", "PZ"}, {"French Guiana", "FY"}, {"Falkland Islands", "VP8"},
    // Africa
    {"South Africa", "ZR-ZU"}, {"Namibia", "V5"}, {"Botswana", "A2 8O"}, {"Lesotho", "7P"}, {"Eswatini", "3DA"}, {"Mozambique", "C8 C9"}, {"Zimbabwe", "Z2"}, {"Zambia", "9I 9J"},
    {"Malawi", "7Q"}, {"Tanzania", "5H 5I"}, {"Kenya", "5Y 5Z"}, {"Uganda", "5X"}, {"Rwanda", "9X"}, {"Burundi", "9U"}, {"Ethiopia", "ET"}, {"Eritrea", "E3"}, {"Somalia", "T5 6O"},
    {"Djibouti", "J2"}, {"Sudan", "ST"}, {"South Sudan", "Z8"}, {"Egypt", "SU"}, {"Libya", "5A"}, {"Tunisia", "3V TS"}, {"Algeria", "7R 7T-7Y"}, {"Morocco", "CN 5C-5G"},
    {"Mauritania", "5T"}, {"Senegal", "6V 6W"}, {"Gambia", "C5"}, {"Guinea-Bissau", "J5"}, {"Guinea", "3X"}, {"Sierra Leone", "9L"}, {"Liberia", "EL 5L 5M 6Z"}, {"Ivory Coast", "TU"},
    {"Ghana", "9G"}, {"Togo", "5V"}, {"Benin", "TY"}, {"Nigeria", "5N 5O"}, {"Niger", "5U"}, {"Mali", "TZ"}, {"Burkina Faso", "XT"}, {"Cameroon", "TJ"}, {"Central African Republic", "TL"},
    {"Chad", "TT"}, {"Gabon", "TR"}, {"Republic of the Congo", "TN"}, {"DR Congo", "9O-9T"}, {"Angola", "D2 D3"}, {"Equatorial Guinea", "3C"}, {"Sao Tome and Principe", "S9"},
    {"Cape Verde", "D4"}, {"Madagascar", "5R 5S 6X"}, {"Mauritius", "3B8"}, {"Seychelles", "S7"}, {"Comoros", "D6"}, {"Reunion", "FR"}, {"Mayotte", "FH"},
    {"St Helena", "ZD7"}, {"Ascension Island", "ZD8"}, {"Tristan da Cunha", "ZD9"},
};

const std::map<std::string, const char*>& prefixTable() {
  static const std::map<std::string, const char*> table = [] {
    std::map<std::string, const char*> t;
    for (const auto& e : kPrefixes) {
      const char* p = e.prefixes;
      while (*p) {
        const char* sp = strchr(p, ' ');
        std::string token = sp ? std::string(p, (size_t)(sp - p)) : std::string(p);
        p = sp ? sp + 1 : p + token.size();
        size_t dash = token.find('-');
        if (dash == std::string::npos) {
          t[token] = e.country;
        } else {
          std::string from = token.substr(0, dash), to = token.substr(dash + 1);
          for (char c = from.back(); c <= to.back(); c++) t[from.substr(0, from.size() - 1) + c] = e.country;
        }
      }
    }
    return t;
  }();
  return table;
}

bool isSuffix(const std::string& p) {
  static const char* const k[] = {"P", "M", "MM", "AM", "QRP", "QRPP", "A", "R", "B", "LH", "T", "N", "G", "E", "J", "AG", "AE", "KT", "IMD"};
  for (const char* s : k)
    if (p == s) return true;
  return p.size() == 1 && isdigit((unsigned char)p[0]);
}
}  // namespace

std::string countryOf(const std::string& callIn) {
  std::string call = upper(trim(callIn));
  // The part of the call that says where it is. In a call with two parts, that's the shorter.
  std::string best;
  size_t at = 0;
  while (at <= call.size()) {
    size_t slash = call.find('/', at);
    std::string part = call.substr(at, slash == std::string::npos ? std::string::npos : slash - at);
    if (!part.empty() && !isSuffix(part) && (best.empty() || part.size() < best.size())) best = part;
    if (slash == std::string::npos) break;
    at = slash + 1;
  }
  const auto& table = prefixTable();
  for (size_t n = std::min<size_t>(4, best.size()); n > 0; n--) {
    auto it = table.find(best.substr(0, n));
    if (it != table.end()) return it->second;
  }
  return "";
}

// ---------------------------------------------------------------- distance

bool gridToLatLon(const std::string& gridIn, double& lat, double& lon) {
  std::string g = upper(trim(gridIn));
  if (g.size() == 8 || g.size() == 10) g.resize(6);  // a few hundred metres either way, which a distance in km can't show
  if (g.size() != 4 && g.size() != 6) return false;
  if (g[0] < 'A' || g[0] > 'R' || g[1] < 'A' || g[1] > 'R' || !isdigit((unsigned char)g[2]) || !isdigit((unsigned char)g[3])) return false;
  lon = (g[0] - 'A') * 20.0 - 180 + (g[2] - '0') * 2.0;
  lat = (g[1] - 'A') * 10.0 - 90 + (g[3] - '0') * 1.0;
  if (g.size() == 6) {
    if (g[4] < 'A' || g[4] > 'X' || g[5] < 'A' || g[5] > 'X') return false;
    lon += (g[4] - 'A' + 0.5) * (2.0 / 24);
    lat += (g[5] - 'A' + 0.5) * (1.0 / 24);
  } else {  // the middle of the square
    lon += 1;
    lat += 0.5;
  }
  return true;
}

bool pathBetween(const std::string& from, const std::string& to, double& km, double& bearingDeg) {
  double lat1, lon1, lat2, lon2;
  if (!gridToLatLon(from, lat1, lon1) || !gridToLatLon(to, lat2, lon2)) return false;
  const double rad = M_PI / 180;
  double p1 = lat1 * rad, p2 = lat2 * rad, dl = (lon2 - lon1) * rad;
  double a = pow(sin((p2 - p1) / 2), 2) + cos(p1) * cos(p2) * pow(sin(dl / 2), 2);
  km = 2 * 6371.0 * atan2(sqrt(a), sqrt(1 - a));
  bearingDeg = fmod(atan2(sin(dl) * cos(p2), cos(p1) * sin(p2) - sin(p1) * cos(p2) * cos(dl)) / rad + 360, 360);
  return true;
}

// ---------------------------------------------------------------- the connection

struct Client::Shared {
  std::atomic<bool> quit{false};
  mutable std::mutex m;
  std::deque<Spot> spots;
  Status status;
  void set(State s, const std::string& message) {
    std::lock_guard<std::mutex> g(m);
    status.state = s;
    status.message = message;
  }
  void addLine(const std::string& line) {
    Spot s;
    if (!parseSpot(line, s)) return;
    s.gotAt = (long long)time(nullptr);
    std::lock_guard<std::mutex> g(m);
    spots.push_back(std::move(s));
    if (spots.size() > 2000) spots.pop_front();  // nobody's taking them: a minimised window, say
  }
};

namespace {
#ifdef _WIN32
typedef SOCKET Sock;
const Sock kNoSock = INVALID_SOCKET;
void closeSock(Sock s) { closesocket(s); }
void startSockets() {
  static const bool started = [] {
    WSADATA d;
    return WSAStartup(MAKEWORD(2, 2), &d) == 0;
  }();
  (void)started;
}
void setNonBlocking(Sock s) {
  u_long on = 1;
  ioctlsocket(s, FIONBIO, &on);
}
#else
typedef int Sock;
const Sock kNoSock = -1;
void closeSock(Sock s) { close(s); }
void startSockets() {}
void setNonBlocking(Sock s) { fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK); }
#endif

double nowSeconds() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

bool waitOn(Sock s, bool forWrite, int ms) {
  fd_set set, bad;
  FD_ZERO(&set);
  FD_ZERO(&bad);
  FD_SET(s, &set);
  FD_SET(s, &bad);
  struct timeval t;
  t.tv_sec = ms / 1000, t.tv_usec = (ms % 1000) * 1000;
  return select((int)s + 1, forWrite ? nullptr : &set, forWrite ? &set : nullptr, &bad, &t) > 0;
}

// Tries each of the host's addresses for a while each, and can be interrupted between waits.
Sock connectTo(const std::string& host, int port, const std::atomic<bool>& quit) {
  struct addrinfo hints;
  memset(&hints, 0, sizeof hints);
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  struct addrinfo* found = nullptr;
  if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &found) != 0) return kNoSock;
  Sock result = kNoSock;
  for (struct addrinfo* a = found; a && result == kNoSock && !quit; a = a->ai_next) {
    Sock s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
    if (s == kNoSock) continue;
    setNonBlocking(s);
    connect(s, a->ai_addr, (int)a->ai_addrlen);
    bool ok = false;
    for (int waited = 0; waited < 8000 && !quit && !ok; waited += 250) {
      if (!waitOn(s, true, 250)) continue;
      int err = 1;
      socklen_t len = sizeof err;
      if (getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&err, &len) == 0 && err == 0) ok = true;
      else break;
    }
    if (ok) result = s;
    else closeSock(s);
  }
  freeaddrinfo(found);
  return result;
}

bool sendLine(Sock s, const std::string& text) {
  std::string line = text + "\r\n";
  return send(s, line.c_str(), (int)line.size(), 0) == (int)line.size();
}

// Takes telnet's option haggling out of what's arrived. A cluster doesn't care if it goes unanswered.
class TelnetFilter {
 public:
  void feed(const char* data, int n, std::string& out) {
    for (int i = 0; i < n; i++) {
      unsigned char c = (unsigned char)data[i];
      switch (state_) {
        case TEXT:
          if (c == 255) state_ = IAC;
          else out += (char)c;
          break;
        case IAC:
          if (c == 255) out += (char)c, state_ = TEXT;
          else if (c >= 251 && c <= 254) state_ = OPTION;
          else if (c == 250) state_ = SUB;
          else state_ = TEXT;
          break;
        case OPTION: state_ = TEXT; break;
        case SUB:
          if (c == 255) state_ = SUB_IAC;
          break;
        case SUB_IAC: state_ = c == 240 ? TEXT : SUB; break;
      }
    }
  }

 private:
  enum { TEXT, IAC, OPTION, SUB, SUB_IAC } state_ = TEXT;
};

bool looksLikeLoginPrompt(const std::string& partial) {
  std::string p = upper(partial);
  return p.find("LOGIN") != std::string::npos || p.find("CALL") != std::string::npos;
}

// One connection, from logging in until it ends. Returns the last thing the server said, which is often why.
std::string talk(Client::Shared& sh, Sock s, const std::string& host, const std::string& call, const std::string& commands) {
  TelnetFilter telnet;
  std::string text, lastLine;
  double connectedAt = nowSeconds(), lastHeard = connectedAt, lastPoke = connectedAt, loggedInAt = 0;
  bool commandsSent = commands.empty();
  auto logIn = [&] {
    if (loggedInAt > 0) return true;
    if (!sendLine(s, call)) return false;
    loggedInAt = nowSeconds();
    sh.set(Client::CONNECTED, "Connected to " + host);
    return true;
  };
  while (!sh.quit) {
    double now = nowSeconds();
    // A prompt has no line ending. One that isn't recognised gets the callsign anyway after a few seconds.
    if (loggedInAt == 0 && now - connectedAt > 5 && !logIn()) return lastLine;
    if (loggedInAt > 0 && !commandsSent && now - loggedInAt > 2) {
      size_t at = 0;
      while (at < commands.size()) {
        size_t semi = commands.find(';', at);
        std::string one = trim(commands.substr(at, semi == std::string::npos ? std::string::npos : semi - at));
        if (!one.empty()) sendLine(s, one);
        if (semi == std::string::npos) break;
        at = semi + 1;
      }
      commandsSent = true;
    }
    // Some nodes drop a connection that's gone quiet, and a dead one looks just the same from here.
    if (now - lastHeard > 300 && now - lastPoke > 300) {
      lastPoke = now;
      if (!sendLine(s, "")) return lastLine;
    }
    if (now - lastHeard > 600) return "Nothing heard for ten minutes";

    if (!waitOn(s, false, 250)) continue;
    char buf[4096];
    int n = (int)recv(s, buf, sizeof buf, 0);
    if (n <= 0) return lastLine;
    lastHeard = nowSeconds();
    telnet.feed(buf, n, text);
    size_t nl;
    while ((nl = text.find('\n')) != std::string::npos) {
      std::string line = trim(text.substr(0, nl));
      text.erase(0, nl + 1);
      if (!line.empty()) lastLine = line;
      sh.addLine(line);
    }
    if (loggedInAt == 0 && looksLikeLoginPrompt(text)) {
      text.clear();
      if (!logIn()) return lastLine;
    }
    if (text.size() > 8192) text.clear();  // not lines at all, so nothing of use
  }
  return lastLine;
}

void runClient(std::shared_ptr<Client::Shared> sh, std::string host, int port, std::string call, std::string commands) {
  startSockets();
  auto wait = [&](int seconds) {
    for (int i = 0; i < seconds * 4 && !sh->quit; i++) std::this_thread::sleep_for(std::chrono::milliseconds(250));
  };
  std::string where = host + ":" + std::to_string(port);
  while (!sh->quit) {
    sh->set(Client::CONNECTING, "Connecting to " + where);
    Sock s = connectTo(host, port, sh->quit);
    if (s == kNoSock) {
      if (!sh->quit) sh->set(Client::WAITING, "Couldn't reach " + where + ". Trying again in a minute.");
      wait(60);
      continue;
    }
    std::string last = talk(*sh, s, host, call, commands);
    closeSock(s);
    if (sh->quit) break;
    sh->set(Client::WAITING, "Lost " + where + (last.empty() ? "" : " (it said: " + last + ")") + ". Trying again in a minute.");
    wait(60);
  }
}

void runDemo(std::shared_ptr<Client::Shared> sh) {
  static const char* const kLines[] = {
      "DX de G4DEMO:    14205.0  JA1XYZ       SSB, loud into Europe",
      "DX de DL1DEM:    14025.5  PY2ABC       CW up 2",
      "DX de K1DEMO:    14074.0  VK3XYZ       FT8 -12 dB",
      "DX de F5DEM:      7155.0  EA8ABC       LSB",
      "DX de W3DEMO-#:   7012.3  ZL2ABC       CW 18 dB 24 WPM CQ",
      "DX de ON4DEM:    21295.0  W1XYZ        ",
      "DX de PA3DEM:    18100.0  LU1ABC       FT8",
      "DX de EI2DEM:     3780.0  GM3ABC       ",
      "DX de VE3DEM:    28480.0  ZS6XYZ       USB",
      "DX de I2DEMO:    10136.0  HL2ABC       FT8",
      "DX de OH2DEM:     1830.0  VP9ABC       CW",
      "DX de G3DEMO:    50313.0  CT1XYZ       FT8",
      "DX de SM5DEM:   144300.0  OZ1ABC       SSB tropo",
      "DX de PA1DEM:   432200.0  G8ABC        SSB",
  };
  const int count = (int)(sizeof kLines / sizeof kLines[0]);
  sh->set(Client::CONNECTED, "Made-up spots, as this is the demo. Nothing is connected.");
  for (int i = 0; !sh->quit; i = (i + 1) % count) {
    char when[16];
    time_t t = time(nullptr);
    struct tm u;
#ifdef _WIN32
    gmtime_s(&u, &t);
#else
    gmtime_r(&t, &u);
#endif
    strftime(when, sizeof when, " %H%MZ", &u);
    sh->addLine(std::string(kLines[i]) + when);
    for (int w = 0; w < 8 && !sh->quit; w++) std::this_thread::sleep_for(std::chrono::milliseconds(250));
  }
}
}  // namespace

void Client::start(const std::string& host, int port, const std::string& call, const std::string& commands) {
  stop();
  shared_ = std::make_shared<Shared>();
  shared_->set(CONNECTING, "Connecting to " + host);
  std::thread(runClient, shared_, host, port, call, commands).detach();
}

void Client::startDemo() {
  stop();
  shared_ = std::make_shared<Shared>();
  std::thread(runDemo, shared_).detach();
}

void Client::stop() {
  if (shared_) shared_->quit = true;
  shared_.reset();
}

Client::Status Client::status() const {
  if (!shared_) return Status();
  std::lock_guard<std::mutex> g(shared_->m);
  return shared_->status;
}

void Client::take(std::vector<Spot>& out) {
  if (!shared_) return;
  std::lock_guard<std::mutex> g(shared_->m);
  while (!shared_->spots.empty()) {
    out.push_back(std::move(shared_->spots.front()));
    shared_->spots.pop_front();
  }
}
}  // namespace dx
