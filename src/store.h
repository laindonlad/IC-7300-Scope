#pragma once
// What the app keeps between runs, all as plain text files in the per-user folder: settings (key=value),
// memories and the log (CSV). The log exports as ADIF for anything else that keeps logs.
#include <string>
#include <vector>

namespace store {
// Per-user folder: %APPDATA%\IC7300Console, ~/Library/Application Support/IC7300Console, or
// ~/.config/ic7300console. Created if needed.
std::string dataDir();
void openFolder(const std::string& path);

class Settings {
 public:
  void load(const std::string& path);
  bool save(const std::string& path) const;
  std::string get(const std::string& k, const std::string& def = "") const;
  int getInt(const std::string& k, int def) const;
  double getDouble(const std::string& k, double def) const;
  void set(const std::string& k, const std::string& v);
  void setInt(const std::string& k, int v);
  void setDouble(const std::string& k, double v);

 private:
  std::vector<std::pair<std::string, std::string>> kv_;
};

std::string joinCsv(const std::vector<std::string>& fields);
std::vector<std::string> splitCsv(const std::string& line);

struct Memory {
  std::string name, group;
  long long hz = 0;
  int mode = 1;
  bool data = false;
  int filter = 1;
};
std::vector<Memory> loadMemories(const std::string& path);
bool saveMemories(const std::string& path, const std::vector<Memory>& m);

struct LogEntry {
  std::string utc;  // "YYYY-MM-DD HH:MM"
  std::string call;
  long long hz = 0;
  std::string mode;  // as the rig shows it: USB, LSB, CW, USB-D...
  std::string rstSent, rstRcvd, name, notes;
};
std::vector<LogEntry> loadLog(const std::string& path);
bool appendLog(const std::string& path, const LogEntry& e);
bool saveLog(const std::string& path, const std::vector<LogEntry>& log);
// Band and mode use ADIF's own names. USB-D and the like could be any digital mode, so the MODE field
// is left out for those and the comment records what the rig showed.
std::string adifRecord(const LogEntry& e, const std::string& stationCall);
bool exportAdif(const std::string& path, const std::vector<LogEntry>& log, const std::string& stationCall);

// A rough WAV writer for recording the receiver: 16-bit mono.
class WavWriter {
 public:
  ~WavWriter() { close(); }
  bool open(const std::string& path, int rate);
  void write(const float* samples, size_t n);
  void close();
  bool isOpen() const { return f_ != nullptr; }
  double seconds() const { return rate_ ? (double)frames_ / rate_ : 0; }
  const std::string& path() const { return path_; }

 private:
  void* f_ = nullptr;
  int rate_ = 0;
  unsigned long long frames_ = 0;
  std::string path_;
};
}  // namespace store
