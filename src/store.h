#pragma once
// What the app keeps between runs, all as plain text files in the per-user folder: settings (key=value),
// memories and the log (CSV). The log exports as ADIF for anything else that keeps logs.
#include <string>
#include <vector>

// The program's name, wherever people read it. A literal, so it can join other literals.
// Not the executables' or the data folder's names: those can't hold a slash, and renaming the folder would
// strand everyone's settings, memories and log.
#define APP_NAME "IC-7300 / IC9700 / IC-R8600 Receiver Console"

namespace store {
// Per-user folder: %APPDATA%\IC7300Console, ~/Library/Application Support/IC7300Console, or
// ~/.config/ic7300console. Created if needed.
std::string dataDir();
// The folder the program itself is in, ending in a separator.
std::string exeDir();
// Where decoded text and charts go: "received" under the program's folder, or under the data folder if the
// program's folder can't be written to (Program Files, say). Created if needed.
std::string receivedDir();
// Where the map overlays for charts are, chosen the same way, as "overlays".
std::string overlaysDir();
void makeDirs(const std::string& path);
void openFolder(const std::string& path);
// Written beside the target and moved over it, so a crash mid-save can't lose what was there.
bool saveText(const std::string& path, const std::string& text);

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
  // Added later, after the fields above in the file, so a log written before these existed still reads.
  std::string utcOff;  // "YYYY-MM-DD HH:MM", or empty
  std::string power;   // watts, as typed
  std::string qth, locator, country;
  std::string contest, exchSent, exchRcvd;
  // Which of the operator's own callsigns made the contact. Empty only if none was set at the time.
  std::string station;
  // QSL status for paper cards, Logbook of the World and eQSL, in ADIF's terms: statuses are Y, N, R and so on,
  // dates are YYYYMMDD.
  std::string qslSent, qslRcvd, qslSentDate, qslRcvdDate;
  std::string lotwSent, lotwRcvd, lotwSentDate, lotwRcvdDate;
  std::string eqslSent, eqslRcvd, eqslSentDate, eqslRcvdDate;
  // How far away the station is, in whole km and miles, worked out from the two locators. Empty if that can't be done.
  std::string distKm, distMiles;
};
// Who has confirmed a contact, such as "LoTW, eQSL", or "" if nobody has.
std::string confirmedBy(const LogEntry& e);
std::vector<LogEntry> loadLog(const std::string& path);
bool appendLog(const std::string& path, const LogEntry& e);
bool saveLog(const std::string& path, const std::vector<LogEntry>& log);
// Copies a file to <path>.bak, before something that can't be undone. True if there was nothing to copy.
bool backupFile(const std::string& path);
// Band and mode use ADIF's own names. USB-D and the like could be any digital mode, so the MODE field
// is left out for those and the comment records what the rig showed. The contact's own station callsign is
// used if it has one, and stationCall if not.
std::string adifRecord(const LogEntry& e, const std::string& stationCall, const std::string& stationGrid = "");
bool exportAdif(const std::string& path, const std::vector<LogEntry>& log, const std::string& stationCall, const std::string& stationGrid = "");
// Reads ADIF text. Fields this log has no place for are ignored. In a Logbook of the World report or an eQSL
// inbox the plain QSL fields describe that service's confirmation, so they're filed under that service.
std::vector<LogEntry> parseAdif(const std::string& text);
// Merges the records of an ADIF file into the log, in time order. A record matches a contact with the same
// call and band, the same station callsign if both give one, and a start within a minute (half an hour for an
// eQSL inbox, which carries the other station's clock). A match is never added again: it brings in any better
// QSL status and fills empty fields, and doesn't overwrite anything else. A record with no station callsign
// that isn't matched goes in under defaultStation.
// False with a message if the file can't be read or has no records in it. The path is UTF-8.
bool importAdif(const std::string& path, std::vector<LogEntry>& log, const std::string& defaultStation, int& added, int& updated, int& skipped, std::string& error);
// The window that's in front on this thread, to own a file dialog. Null where that means nothing.
void* activeWindow();
// Asks for an ADIF file with the system's own dialog, and blocks until it's closed, so it belongs on its own
// thread. path is UTF-8, and empty if they cancelled. False if this system has no dialog to show.
bool pickAdifFile(void* owner, std::string& path);

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
