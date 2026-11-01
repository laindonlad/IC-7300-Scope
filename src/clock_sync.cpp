#include "clock_sync.h"

#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0602  // GetSystemTimePreciseAsFileTime
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#else
#include <errno.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <random>

namespace clocksync {

namespace {
const char* const kServers[] = {"pool.ntp.org", "time.cloudflare.com", "time.google.com"};
const int kServersWanted = 2;    // two that agree are enough, and spare the third
const int kAsksPerServer = 2;    // the quicker round trip of the two is the more accurate
const int kAddressesPerServer = 2;  // a server's IPv6 address may not be reachable from here
const int kTimeoutMs = 1000;
const uint64_t kNtpToUnixSeconds = 2208988800ULL;

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
double nowUnix() {
  FILETIME f;
  GetSystemTimePreciseAsFileTime(&f);
  ULARGE_INTEGER u;
  u.LowPart = f.dwLowDateTime, u.HighPart = f.dwHighDateTime;
  return (double)(u.QuadPart - 116444736000000000ULL) / 1e7;
}
#else
typedef int Sock;
const Sock kNoSock = -1;
void closeSock(Sock s) { close(s); }
void startSockets() {}
double nowUnix() {
  struct timeval t;
  gettimeofday(&t, nullptr);
  return (double)t.tv_sec + (double)t.tv_usec / 1e6;
}
#endif

uint64_t get64(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; i++) v = (v << 8) | p[i];
  return v;
}

uint64_t randomTag() {
  std::random_device r;
  uint64_t t = ((uint64_t)r() << 32) | r();
  return t ? t : 1;
}

// One question on an open socket. A reply that doesn't quote the tag counts as no reply.
bool ask(Sock s, Sample& out) {
  uint64_t tag = randomTag();
  uint8_t req[kPacketSize], reply[kPacketSize + 16];
  makeRequest(req, tag);
  double sentAt = nowUnix();
  if (send(s, (const char*)req, kPacketSize, 0) != kPacketSize) return false;
  fd_set in;
  FD_ZERO(&in);
  FD_SET(s, &in);
  struct timeval wait;
  wait.tv_sec = kTimeoutMs / 1000, wait.tv_usec = (kTimeoutMs % 1000) * 1000;
  if (select((int)s + 1, &in, nullptr, nullptr, &wait) <= 0) return false;
  int n = (int)recv(s, (char*)reply, sizeof reply, 0);
  double gotAt = nowUnix();
  return n > 0 && readReply(reply, (size_t)n, tag, sentAt, gotAt, out);
}

// The best answer from one server, trying its next address if the first says nothing.
bool askServer(const char* host, Sample& out) {
  struct addrinfo hints;
  memset(&hints, 0, sizeof hints);
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_DGRAM;
  struct addrinfo* found = nullptr;
  if (getaddrinfo(host, "123", &hints, &found) != 0) return false;
  bool any = false;
  int tried = 0;
  for (struct addrinfo* a = found; a && !any && tried < kAddressesPerServer; a = a->ai_next, tried++) {
    Sock s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
    if (s == kNoSock) continue;
    // Connected, so the system only hands over what comes from the address that was asked.
    if (connect(s, a->ai_addr, (int)a->ai_addrlen) == 0) {
      for (int i = 0; i < kAsksPerServer; i++) {
        Sample one;
        if (!ask(s, one)) continue;
        if (!any || one.delay < out.delay) out = one;
        any = true;
      }
    }
    closeSock(s);
  }
  freeaddrinfo(found);
  return any;
}

bool measure(Sample& best, std::string& why) {
  startSockets();
  std::vector<Sample> answers;
  for (const char* host : kServers) {
    if ((int)answers.size() == kServersWanted) break;
    Sample s;
    if (askServer(host, s)) answers.push_back(s);
  }
  if (answers.empty()) {
    why = "No time server answered. Is this computer online, and is UDP port 123 open?";
    return false;
  }
  if (!choose(answers, best)) {
    why = "The time servers disagree, so the clock was left alone. Try again in a minute.";
    return false;
  }
  return true;
}

enum SetResult { SET_OK, SET_NOT_ALLOWED, SET_FAILED };

SetResult setClock(double offset) {
#ifdef _WIN32
  FILETIME f;
  GetSystemTimePreciseAsFileTime(&f);
  ULARGE_INTEGER u;
  u.LowPart = f.dwLowDateTime, u.HighPart = f.dwHighDateTime;
  // SetSystemTime only takes whole milliseconds, and would cut the rest off. Half a one rounds it instead.
  u.QuadPart += (unsigned long long)(long long)llround(offset * 1e7) + 5000;
  f.dwLowDateTime = u.LowPart, f.dwHighDateTime = u.HighPart;
  SYSTEMTIME t;
  if (!FileTimeToSystemTime(&f, &t)) return SET_FAILED;
  if (SetSystemTime(&t)) return SET_OK;
  return GetLastError() == ERROR_PRIVILEGE_NOT_HELD ? SET_NOT_ALLOWED : SET_FAILED;
#else
  struct timeval t;
  gettimeofday(&t, nullptr);
  long long us = (long long)t.tv_sec * 1000000 + t.tv_usec + llround(offset * 1e6);
  t.tv_sec = (time_t)(us / 1000000), t.tv_usec = (suseconds_t)(us % 1000000);
  if (settimeofday(&t, nullptr) == 0) return SET_OK;
  return errno == EPERM ? SET_NOT_ALLOWED : SET_FAILED;
#endif
}

#ifdef _WIN32
enum AskResult { ASK_DONE, ASK_DECLINED, ASK_FAILED };

// Runs this program again with --sync-clock, which Windows asks permission for.
AskResult askWindowsToSet() {
  wchar_t self[32768];
  DWORD len = GetModuleFileNameW(nullptr, self, sizeof self / sizeof self[0]);
  if (len == 0 || len >= sizeof self / sizeof self[0]) return ASK_FAILED;
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  SHELLEXECUTEINFOW e;
  memset(&e, 0, sizeof e);
  e.cbSize = sizeof e;
  e.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
  e.hwnd = GetForegroundWindow();  // without an owner the prompt can start behind the window
  e.lpVerb = L"runas";
  e.lpFile = self;
  e.lpParameters = L"--sync-clock";
  e.nShow = SW_HIDE;
  BOOL started = ShellExecuteExW(&e);
  DWORD err = GetLastError();
  CoUninitialize();
  if (!started) return err == ERROR_CANCELLED ? ASK_DECLINED : ASK_FAILED;
  if (!e.hProcess) return ASK_FAILED;
  DWORD waited = WaitForSingleObject(e.hProcess, 30000);
  DWORD code = 1;
  bool done = waited == WAIT_OBJECT_0 && GetExitCodeProcess(e.hProcess, &code) && code == 0;
  CloseHandle(e.hProcess);
  return done ? ASK_DONE : ASK_FAILED;
}
#endif

Report said(bool ok, const std::string& message) {
  Report r;
  r.ok = ok;
  r.message = message;
  return r;
}

std::string within(const Sample& s) {
  char b[48];
  snprintf(b, sizeof b, "%.3f s", s.delay / 2);
  return b;
}
}  // namespace

void makeRequest(uint8_t out[kPacketSize], uint64_t tag) {
  memset(out, 0, kPacketSize);
  out[0] = 0x23;  // no leap warning, version 4, a client
  for (int i = 0; i < 8; i++) out[40 + i] = (uint8_t)(tag >> (56 - 8 * i));
}

double ntpToUnix(uint64_t ntp) {
  uint64_t sec = ntp >> 32;
  // Seconds wrap in 2036. With the top bit clear, this is the next time round.
  if (sec < 0x80000000ULL) sec += 0x100000000ULL;
  return (double)((long long)sec - (long long)kNtpToUnixSeconds) + (double)(ntp & 0xFFFFFFFFULL) / 4294967296.0;
}

bool readReply(const uint8_t* p, size_t n, uint64_t tag, double sentAt, double gotAt, Sample& out) {
  if (n < (size_t)kPacketSize) return false;
  if ((p[0] & 7) != 4) return false;     // not from a server
  if ((p[0] >> 6) == 3) return false;    // the server says its own clock isn't set
  if (p[1] == 0 || p[1] > 15) return false;  // stratum 0 is a server telling us to go away
  if (get64(p + 24) != tag) return false;    // not an answer to what we asked
  uint64_t rx = get64(p + 32), tx = get64(p + 40);
  if (rx == 0 || tx == 0) return false;
  double t2 = ntpToUnix(rx), t3 = ntpToUnix(tx);
  double delay = (gotAt - sentAt) - (t3 - t2);
  if (delay < -0.01) return false;  // the server held it for longer than it took: nonsense
  out.offset = ((t2 - sentAt) + (t3 - gotAt)) / 2;
  out.delay = delay < 0 ? 0 : delay;
  return true;
}

bool choose(const std::vector<Sample>& fromEachServer, Sample& best) {
  if (fromEachServer.empty()) return false;
  best = fromEachServer[0];
  for (const Sample& s : fromEachServer) {
    if (fabs(s.offset - fromEachServer[0].offset) > kMostDisagreement) return false;
    if (s.delay < best.delay) best = s;
  }
  return true;
}

Verdict judge(double offset) {
  double size = fabs(offset);
  if (size > kMostToStep) return V_TOO_FAR;
  return size <= kCloseEnough ? V_CLOSE_ENOUGH : V_STEP;
}

std::string describe(double offset) {
  char b[64];
  snprintf(b, sizeof b, "%.3f s %s", fabs(offset), offset >= 0 ? "slow" : "fast");
  return b;
}

Report jog(bool change) {
  Sample before;
  std::string why;
  if (!measure(before, why)) return said(false, why);
  switch (judge(before.offset)) {
    case V_TOO_FAR:
      return said(false, "The clock is " + describe(before.offset) + ", too far out to trust. Check its date and time in your system settings.");
    case V_CLOSE_ENOUGH:
      return said(true, "The clock is right: " + describe(before.offset) + ", to within " + within(before) + ". Left it alone.");
    case V_STEP:
      break;
  }
  if (!change) return said(true, "The clock is " + describe(before.offset) + ". Demo: left it alone.");

  SetResult set = setClock(before.offset);
  if (set == SET_NOT_ALLOWED) {
#ifdef _WIN32
    AskResult asked = askWindowsToSet();
    if (asked == ASK_DECLINED) return said(false, "The clock is still " + describe(before.offset) + ": Windows wasn't given permission to set it.");
    set = asked == ASK_DONE ? SET_OK : SET_FAILED;
#else
    return said(false, "The clock is " + describe(before.offset) + ". Setting it needs root: run this program once with sudo and --sync-clock.");
#endif
  }
  if (set != SET_OK) return said(false, "The clock is still " + describe(before.offset) + ": it couldn't be set.");

  // Say what actually happened, from a fresh look at the clock rather than from what was asked for.
  Sample after;
  if (!measure(after, why)) return said(true, "The clock was " + describe(before.offset) + " and has been set, but the servers didn't answer to confirm it.");
  if (judge(after.offset) != V_CLOSE_ENOUGH) return said(false, "The clock was " + describe(before.offset) + " and was set, but is now " + describe(after.offset) + ".");
  return said(true, "The clock was " + describe(before.offset) + ". It's now " + describe(after.offset) + ", to within " + within(after) + ".");
}

int runSyncClock() {
  Sample s;
  std::string why;
  if (!measure(s, why)) {
    fprintf(stderr, "%s\n", why.c_str());
    return 1;
  }
  Verdict v = judge(s.offset);
  printf("The clock is %s\n", describe(s.offset).c_str());
  if (v == V_CLOSE_ENOUGH) return 0;
  if (v == V_TOO_FAR) {
    fprintf(stderr, "That's too far out to trust. Check the date and time in your system settings.\n");
    return 2;
  }
  SetResult set = setClock(s.offset);
  if (set != SET_OK) fprintf(stderr, set == SET_NOT_ALLOWED ? "Not allowed to set the clock.\n" : "Couldn't set the clock.\n");
  return set == SET_OK ? 0 : 3;
}

}  // namespace clocksync
