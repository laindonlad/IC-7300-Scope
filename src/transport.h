#pragma once
// A byte pipe to the rig. The rig thread owns it and is its only user.
#include <stdint.h>

#include <string>
#include <vector>

class Transport {
 public:
  virtual ~Transport() = default;
  // False with a reason in err if it can't be opened.
  virtual bool open(std::string& err) = 0;
  virtual void close() = 0;
  virtual bool isOpen() const = 0;
  // Returns as soon as anything has arrived, or after timeoutMs with 0. -1: the link has gone.
  virtual int read(uint8_t* buf, int max, int timeoutMs) = 0;
  virtual bool write(const uint8_t* buf, int n) = 0;
};

struct SerialPortInfo {
  std::string name;  // "COM11", "/dev/ttyUSB0", "/dev/cu.SLAB_USBtoUART"
  std::string desc;  // what the OS calls the device, or ""
};

// serial.cpp: Win32 or POSIX.
std::vector<SerialPortInfo> listSerialPorts();
// Opening never raises RTS or DTR for long, as the 7300 can be set to key on either.
Transport* newSerialTransport(const std::string& port, int baud);
// The port most likely to be an IC-7300's (its USB bridge is a Silicon Labs CP210x), or "".
std::string guessRigPort(const std::vector<SerialPortInfo>& ports);
