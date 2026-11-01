#pragma once
// Band plans: where each band begins and ends, how it's divided up, and the frequencies worth marking.
// They live in JSON files the user can edit. A copy of each is built into the program and written to
// the data folder the first time, so there is always a good plan to fall back on. A plan is a guide for
// the eye, not a statement of what your licence allows.
#include <string>
#include <vector>

namespace bands {
enum Use { U_CW, U_DIGI, U_SSB, U_FM, U_BEACON, U_SAT, U_OTHER, U_BROADCAST, U_AIR, U_MARINE, U_MOBILE, U_FIXED, U_NAV };

struct Segment {
  long long lo = 0, hi = 0;
  Use use = U_OTHER;
  std::string label;
};
// With a mode it's also a preset: it appears on the band's menu, and clicking it sets the mode as well.
struct Marker {
  long long hz = 0;
  std::string label, note;
  int mode = -1;
  bool data = false;
  long long dialHz = 0;  // where clicking tunes, if that isn't hz: see Station
  std::string decoder;   // the decoder its signal needs when the mode doesn't say: NAVTEX, WEFAX or RTTY
  bool preset() const { return mode >= 0; }
  long long tuneHz() const { return dialHz > 0 ? dialHz : hz; }
};
// The segments are in order and don't overlap. Between them is out of band, which is how 60 m's separate
// blocks work: lo and hi are simply the outside edges of the first and last segment.
struct Band {
  std::string name;
  std::string unit = "m";  // 70 is 70 cm. Nothing, if the name says it all, as a receiver's bands do
  // Set on what isn't an amateur band: the rest of a receiver's coverage, and a menu to find it in. Only a
  // receiver is offered these, so a transceiver's buttons and its out-of-band warnings stay as they were.
  std::string group;
  std::string title() const { return unit.empty() ? name : name + " " + unit; }
  long long lo = 0, hi = 0;
  long long homeHz = 0;  // where the button goes the first time
  int homeMode = 0;
  std::vector<Segment> segments;
  std::vector<Marker> markers;
  const Segment* segmentAt(long long hz) const;
};
struct Plan {
  std::string id;  // the file name, without .json
  std::string name, revision, source, note;
  std::vector<Band> bands;  // lowest first
  int bandOf(long long hz) const;  // within a band's outside edges, or -1
  const Segment* segmentAt(long long hz) const;  // nullptr: out of band
  Plan forRadio(bool receiver) const;  // without the groups' bands, unless it's for a receiver
};

const char* useName(Use u);

// False with a message, naming the line for a syntax error, if the text isn't a usable plan.
bool parsePlan(const std::string& json, const std::string& id, Plan& out, std::string& error);

struct Library {
  std::vector<Plan> plans;  // by name
  std::vector<std::string> errors;  // files that couldn't be used, and why
  const Plan* find(const std::string& id) const;
};
// The ones built into the program.
Library builtinPlans();
// The built-in ones, each replaced by its file in dir if that's good, plus any other .json files there.
// A file with nothing above 100 MHz, such as one written before VHF plans existed, keeps the built-in
// plan's VHF and UHF bands, so an older copy doesn't leave a 9700 with no bands at all. One with no groups
// keeps the built-in plan's for the same reason, except any that would overlap a band of its own.
Library loadPlans(const std::string& dir);
// Writes any built-in plan that's missing from dir, and the format notes. Never touches an existing plan.
void installPlans(const std::string& dir);
// Puts the built-in copy back. The file it replaces is kept as <id>.json.bak.
bool resetPlan(const std::string& dir, const std::string& id);

// ---- stations: what else is on the air, shown whichever band plan is chosen. For listening, not transmitting.
// New ones go last: settings keep each service's on or off by its number.
enum Service { S_COMMERCIAL, S_GOVERNMENT, S_NAVAL, S_MILITARY, S_BROADCAST, S_AMATEUR, S_AVIATION, S_SPACE, S_LAND, S_MYSTERY, S_COUNT };
const char* serviceName(Service s);

// The flag sits at hz, where the signal is. For a data signal that's above where USB has to be tuned to
// put its tones in the passband, which is dialHz.
struct Station : Marker {
  Service service = S_GOVERNMENT;
  std::string group;
  // When a broadcaster is scheduled, in UTC minutes. It can run past midnight, and days is of its start,
  // with bit 0 for Monday. A station with none is always on.
  struct AirTime {
    int from = 0, to = 0;
    unsigned days = 0x7f;
  };
  std::vector<AirTime> airTimes;
  bool onAir(int weekday, int minute) const;  // weekday 0 is Monday
};
// A stretch of spectrum rather than one frequency, such as a broadcast band.
struct StationRange {
  long long lo = 0, hi = 0;
  Service service = S_BROADCAST;
  std::string label, note;
  int mode = -1;  // what going there sets, if the range says. Otherwise a broadcast band is AM
};
struct StationFile {
  std::string id, name, revision, source, note;
  bool receiverOnly = false;  // beyond what a transceiver would want to see, or could tune
  std::vector<Station> stations;
  std::vector<StationRange> ranges;
};
// One of station or range is set. They point into the Stations they came from.
struct StationHit {
  const Station* station = nullptr;
  const StationRange* range = nullptr;
  long long hz = 0;
  bool named = false;  // the query's first word is in its label
  bool phrase = false;  // the query's words, together and in order, are in its label or note
};
struct Stations {
  std::vector<StationFile> files;  // by name
  std::vector<std::string> errors;
  const StationFile* find(const std::string& id) const;
  Stations forRadio(bool receiver) const;  // without the receiver-only files, unless it's for a receiver
  const StationRange* rangeAt(long long hz, const bool* shown = nullptr) const;  // shown: one per service
  // Every word must appear, in any case, in the name, group, notes, service or frequency in kHz. Those with
  // the words as a phrase come first, then those it names, then by frequency. An empty query finds everything.
  std::vector<StationHit> search(const std::string& query) const;
};
bool parseStations(const std::string& json, const std::string& id, StationFile& out, std::string& error);
Stations builtinStations();
// Works as for plans: built-in files, each replaced by a good file of its name in dir, plus any others there.
// And as for plans, a file with nothing above 100 MHz keeps the built-in file's VHF and UHF stations.
Stations loadStations(const std::string& dir);
void installStations(const std::string& dir);
bool resetStations(const std::string& dir, const std::string& id);

// The ADIF name of the band a frequency is in, such as "20M", or "" if none. This is the worldwide
// definition from the ADIF spec, whichever plan is showing: a contact logged is on its band regardless.
const char* adifBand(long long hz);
}  // namespace bands
