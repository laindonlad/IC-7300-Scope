#include "transport.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>

std::string guessRigPort(const std::vector<SerialPortInfo>& ports) {
  auto lower = [](std::string s) {
    for (char& c : s)
      if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
    return s;
  };
  // A 9700's dual bridge shows two ports. CI-V is on the one called Enhanced; Standard carries other data.
  for (const char* want : {"ic-7300", "ic7300", "ic-9700", "ic9700", "ic-r8600", "icr8600", "enhanced com", "cp210", "silicon labs", "slab_usb"})
    for (const auto& p : ports)
      if (lower(p.desc + " " + p.name).find(want) != std::string::npos) return p.name;
  return "";
}

#ifdef _WIN32
// ---------------------------------------------------------------- Windows
#include <windows.h>
#include <devguid.h>
#include <setupapi.h>

namespace {
std::string narrow(const wchar_t* w) {
  int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  if (n <= 1) return "";
  std::string s((size_t)n - 1, 0);
  WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
  return s;
}

class WinSerial : public Transport {
 public:
  WinSerial(std::string port, int baud) : port_(std::move(port)), baud_(baud) {}
  ~WinSerial() override { close(); }
  bool isOpen() const override { return h_ != INVALID_HANDLE_VALUE; }

  bool open(std::string& err) override {
    close();
    if (port_.empty()) {
      err = "No serial port chosen";
      return false;
    }
    std::string name = "\\\\.\\" + port_;
    h_ = CreateFileA(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h_ == INVALID_HANDLE_VALUE) {
      DWORD e = GetLastError();
      err = port_ + (e == ERROR_ACCESS_DENIED ? " is in use by another program"
                     : e == ERROR_FILE_NOT_FOUND ? " isn't there (radio off or unplugged?)"
                                                 : " can't be opened");
      return false;
    }
    DCB d = {};
    d.DCBlength = sizeof d;
    GetCommState(h_, &d);
    d.BaudRate = (DWORD)baud_, d.ByteSize = 8, d.Parity = NOPARITY, d.StopBits = ONESTOPBIT;
    d.fBinary = TRUE, d.fParity = FALSE, d.fOutxCtsFlow = d.fOutxDsrFlow = FALSE, d.fOutX = d.fInX = FALSE;
    d.fDtrControl = DTR_CONTROL_DISABLE, d.fRtsControl = RTS_CONTROL_DISABLE;  // the 7300 can key on either
    SetCommState(h_, &d);
    PurgeComm(h_, PURGE_RXCLEAR | PURGE_TXCLEAR);
    lastTimeout_ = -1;
    return true;
  }

  void close() override {
    if (h_ == INVALID_HANDLE_VALUE) return;
    EscapeCommFunction(h_, CLRRTS);
    EscapeCommFunction(h_, CLRDTR);
    CloseHandle(h_);
    h_ = INVALID_HANDLE_VALUE;
  }

  int read(uint8_t* buf, int max, int timeoutMs) override {
    if (timeoutMs != lastTimeout_) {
      // this combination: return as soon as anything's in, or after the constant with nothing
      COMMTIMEOUTS t = {MAXDWORD, MAXDWORD, (DWORD)timeoutMs, 0, 500};
      SetCommTimeouts(h_, &t);
      lastTimeout_ = timeoutMs;
    }
    DWORD n = 0;
    if (!ReadFile(h_, buf, (DWORD)max, &n, nullptr)) return -1;
    return (int)n;
  }

  bool write(const uint8_t* buf, int n) override {
    DWORD done = 0;
    return WriteFile(h_, buf, (DWORD)n, &done, nullptr) && (int)done == n;
  }

 private:
  std::string port_;
  int baud_;
  HANDLE h_ = INVALID_HANDLE_VALUE;
  int lastTimeout_ = -1;
};
}  // namespace

Transport* newSerialTransport(const std::string& port, int baud) { return new WinSerial(port, baud); }

std::vector<SerialPortInfo> listSerialPorts() {
  std::vector<SerialPortInfo> ports;
  auto have = [&](const std::string& n) {
    return std::any_of(ports.begin(), ports.end(), [&](const SerialPortInfo& p) { return p.name == n; });
  };
  // The Ports class gives each port's name and the device's; SERIALCOMM adds virtual ports outside it.
  HDEVINFO set = SetupDiGetClassDevsW(&GUID_DEVCLASS_PORTS, nullptr, nullptr, DIGCF_PRESENT);
  if (set != INVALID_HANDLE_VALUE) {
    SP_DEVINFO_DATA d = {};
    d.cbSize = sizeof d;
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &d); i++) {
      HKEY k = SetupDiOpenDevRegKey(set, &d, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
      if (k == INVALID_HANDLE_VALUE) continue;
      wchar_t name[32] = {};
      DWORD n = sizeof name - sizeof(wchar_t), type = 0;
      LONG r = RegQueryValueExW(k, L"PortName", nullptr, &type, (BYTE*)name, &n);
      RegCloseKey(k);
      if (r != ERROR_SUCCESS || type != REG_SZ || wcsncmp(name, L"COM", 3)) continue;
      wchar_t desc[256] = {};
      SetupDiGetDeviceRegistryPropertyW(set, &d, SPDRP_FRIENDLYNAME, nullptr, (BYTE*)desc, sizeof desc - sizeof(wchar_t), nullptr);
      SerialPortInfo p{narrow(name), narrow(desc)};
      auto at = p.desc.rfind(" (" + p.name + ")");
      if (at != std::string::npos) p.desc.resize(at);
      if (!have(p.name)) ports.push_back(p);
    }
    SetupDiDestroyDeviceInfoList(set);
  }
  HKEY k;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DEVICEMAP\\SERIALCOMM", 0, KEY_READ, &k) == ERROR_SUCCESS) {
    for (DWORD i = 0;; i++) {
      wchar_t value[256], data[32] = {};
      DWORD vn = 256, dn = sizeof data - sizeof(wchar_t), type = 0;
      if (RegEnumValueW(k, i, value, &vn, nullptr, &type, (BYTE*)data, &dn) != ERROR_SUCCESS) break;
      if (type == REG_SZ && !have(narrow(data))) ports.push_back({narrow(data), ""});
    }
    RegCloseKey(k);
  }
  std::sort(ports.begin(), ports.end(), [](const SerialPortInfo& a, const SerialPortInfo& b) {
    return atoi(a.name.c_str() + 3) < atoi(b.name.c_str() + 3);
  });
  return ports;
}

#else
// ---------------------------------------------------------------- Linux and macOS
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

namespace {
speed_t baudConstant(int baud) {
  switch (baud) {
    case 4800: return B4800;
    case 9600: return B9600;
    case 19200: return B19200;
    case 38400: return B38400;
    case 57600: return B57600;
    default: return B115200;
  }
}

class PosixSerial : public Transport {
 public:
  PosixSerial(std::string port, int baud) : port_(std::move(port)), baud_(baud) {}
  ~PosixSerial() override { close(); }
  bool isOpen() const override { return fd_ >= 0; }

  bool open(std::string& err) override {
    close();
    if (port_.empty()) {
      err = "No serial port chosen";
      return false;
    }
    // O_NONBLOCK so opening can't hang on carrier detect. O_EXCL-style locking keeps a second program out.
    fd_ = ::open(port_.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd_ < 0) {
      int e = errno;
      err = port_ + (e == EACCES ? " permission denied (on Linux, join the dialout group)"
                     : e == EBUSY ? " is in use by another program"
                     : e == ENOENT ? " isn't there (radio off or unplugged?)"
                                   : std::string(" can't be opened: ") + strerror(e));
      return false;
    }
    if (ioctl(fd_, TIOCEXCL) != 0) { /* not fatal: only stops a second opener */ }
    // POSIX raises DTR and RTS on open and we can't prevent that first instant: drop them at once,
    // before anything else, as the 7300 can be set to key on either.
    clearLines();
    termios t = {};
    tcgetattr(fd_, &t);
    cfmakeraw(&t);
    cfsetispeed(&t, baudConstant(baud_));
    cfsetospeed(&t, baudConstant(baud_));
    t.c_cflag |= CLOCAL | CREAD;
    t.c_cflag &= ~(CRTSCTS | HUPCL);
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    tcsetattr(fd_, TCSANOW, &t);
    clearLines();
    tcflush(fd_, TCIOFLUSH);
    return true;
  }

  void close() override {
    if (fd_ < 0) return;
    clearLines();
    ::close(fd_);
    fd_ = -1;
  }

  int read(uint8_t* buf, int max, int timeoutMs) override {
    pollfd p = {fd_, POLLIN, 0};
    int r = ::poll(&p, 1, timeoutMs);
    if (r == 0) return 0;
    if (r < 0) return errno == EINTR ? 0 : -1;
    if (p.revents & (POLLERR | POLLHUP | POLLNVAL)) return -1;
    ssize_t n = ::read(fd_, buf, (size_t)max);
    if (n < 0) return (errno == EAGAIN || errno == EINTR) ? 0 : -1;
    return (int)n;
  }

  bool write(const uint8_t* buf, int n) override {
    int done = 0;
    for (int tries = 0; done < n && tries < 50; tries++) {
      ssize_t w = ::write(fd_, buf + done, (size_t)(n - done));
      if (w > 0) {
        done += (int)w;
        continue;
      }
      if (w < 0 && errno != EAGAIN && errno != EINTR) return false;
      pollfd p = {fd_, POLLOUT, 0};
      ::poll(&p, 1, 20);
    }
    return done == n;
  }

 private:
  void clearLines() {
    int lines = TIOCM_DTR | TIOCM_RTS;
    ioctl(fd_, TIOCMBIC, &lines);
  }
  std::string port_;
  int baud_;
  int fd_ = -1;
};

std::vector<std::string> listDir(const char* dir, const char* prefix) {
  std::vector<std::string> out;
  DIR* d = opendir(dir);
  if (!d) return out;
  while (dirent* e = readdir(d))
    if (!strncmp(e->d_name, prefix, strlen(prefix))) out.push_back(std::string(dir) + "/" + e->d_name);
  closedir(d);
  std::sort(out.begin(), out.end());
  return out;
}
}  // namespace

Transport* newSerialTransport(const std::string& port, int baud) { return new PosixSerial(port, baud); }

std::vector<SerialPortInfo> listSerialPorts() {
  std::vector<SerialPortInfo> ports;
#ifdef __APPLE__
  for (auto& n : listDir("/dev", "cu.")) {
    if (n.find("Bluetooth") != std::string::npos) continue;
    ports.push_back({n, n.find("SLAB") != std::string::npos ? "Silicon Labs CP210x" : ""});
  }
#else
  // by-id names carry the device's name ("usb-Silicon_Labs_CP2102_..._IC-7300_...") and survive replugging.
  for (auto& n : listDir("/dev/serial/by-id", "")) {
    if (n.back() == '.') continue;
    ports.push_back({n, n.substr(n.rfind('/') + 1)});
  }
  if (ports.empty()) {
    for (auto& n : listDir("/dev", "ttyUSB")) ports.push_back({n, ""});
    for (auto& n : listDir("/dev", "ttyACM")) ports.push_back({n, ""});
  }
#endif
  return ports;
}
#endif
