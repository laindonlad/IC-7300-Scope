#pragma once
// Live DX spots from a DX cluster, over telnet, and what goes with them: reading spot lines, keeping a list,
// working out a country from a callsign and a distance from two locators. No GUI code.
//
// The connection has its own thread, which can wait on the network for as long as it likes. It owns its half of
// the state and is left to finish alone when stopped, so closing the window never waits for a slow server.
// Nothing connects until the operator turns spots on.
#include <memory>
#include <string>
#include <vector>

namespace dx {
struct Spot {
  long long hz = 0;
  std::string call, spotter, comment;
  std::string utc;        // "1205", as the spotter's node gave it
  long long gotAt = 0;    // when it arrived, in Unix seconds
  int count = 1;          // how many times it's been spotted
  bool skimmer = false;   // from a robot, such as the Reverse Beacon Network: the spotter's call ends in -#
};

// False if the line isn't a spot: "DX de W3LPL:     14025.0  JA1XYZ       CW 12 dB 25 WPM     1205Z"
bool parseSpot(const std::string& line, Spot& out);

// What a spot's comment says about its mode. NO_HINT is most of them, and then the band plan has to say.
// AM isn't looked for: comments are full of "I am", and it would take the rig out of sideband for nothing.
enum ModeHint { NO_HINT, HINT_CW, HINT_SSB, HINT_FM, HINT_RTTY, HINT_FT8, HINT_FT4, HINT_DATA };
ModeHint modeHint(const std::string& comment);

// Newest first. A station spotted again within a kHz or so replaces its earlier spot and keeps the count.
class SpotList {
 public:
  void add(Spot s);
  bool prune(long long now, long long maxAgeSeconds);  // true if any went
  void clear() { spots_.clear(); }
  const std::vector<Spot>& spots() const { return spots_; }

 private:
  std::vector<Spot> spots_;
};

// The country or island a callsign's prefix belongs to, or "" if it isn't known. A guide from the ITU's
// allocations and the usual DXCC prefixes: special calls and some islands will be wrong. HB9/G3ABC is in
// Switzerland, and /P, /M and the like are ignored.
std::string countryOf(const std::string& call);

// The middle of a Maidenhead locator of 4 or 6 characters, or 8 or 10 read as their first 6. False if it isn't one.
bool gridToLatLon(const std::string& grid, double& lat, double& lon);
// The same locators. False if either isn't one.
bool pathBetween(const std::string& fromGrid, const std::string& toGrid, double& km, double& bearingDeg);

class Client {
 public:
  enum State { OFF, CONNECTING, CONNECTED, WAITING };  // WAITING: lost it, and trying again soon
  struct Status {
    State state = OFF;
    std::string message;
  };
  ~Client() { stop(); }
  void start(const std::string& host, int port, const std::string& call, const std::string& commands);
  // Made-up spots, with no network at all, for --demo.
  void startDemo();
  void stop();
  bool running() const { return (bool)shared_; }
  Status status() const;
  void take(std::vector<Spot>& out);  // adds what has arrived since last time

  struct Shared;

 private:
  std::shared_ptr<Shared> shared_;
};
}  // namespace dx
