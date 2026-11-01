#pragma once
// Band plans: where each band begins and ends, how it's divided up, and the frequencies worth marking.
// They live in JSON files the user can edit. A copy of each is built into the program and written to
// the data folder the first time, so there is always a good plan to fall back on. A plan is a guide for
// the eye, not a statement of what your licence allows.
#include <string>
#include <vector>

namespace bands {
enum Use { U_CW, U_DIGI, U_SSB, U_FM, U_BEACON, U_SAT, U_OTHER };

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
  bool preset() const { return mode >= 0; }
  long long tuneHz() const { return dialHz > 0 ? dialHz : hz; }
};
// The segments are in order and don't overlap. Between them is out of band, which is how 60 m's separate
// blocks work: lo and hi are simply the outside edges of the first and last segment.
struct Band {
  std::string name;
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
Library loadPlans(const std::string& dir);
// Writes any built-in plan that's missing from dir, and the format notes. Never touches an existing plan.
void installPlans(const std::string& dir);
// Puts the built-in copy back. The file it replaces is kept as <id>.json.bak.
bool resetPlan(const std::string& dir, const std::string& id);

// ---- stations: what else is on the air, shown whichever band plan is chosen. For listening, not transmitting.
enum Service { S_COMMERCIAL, S_GOVERNMENT, S_NAVAL, S_MILITARY, S_BROADCAST, S_COUNT };
const char* serviceName(Service s);

// The flag sits at hz, where the signal is. For a data signal that's above where USB has to be tuned to
// put its tones in the passband, which is dialHz.
struct Station : Marker {
  Service service = S_GOVERNMENT;
  std::string group;
};
// A stretch of spectrum rather than one frequency, such as a broadcast band.
struct StationRange {
  long long lo = 0, hi = 0;
  Service service = S_BROADCAST;
  std::string label, note;
};
struct StationFile {
  std::string id, name, revision, source, note;
  std::vector<Station> stations;
  std::vector<StationRange> ranges;
};
// One of station or range is set. They point into the Stations they came from.
struct StationHit {
  const Station* station = nullptr;
  const StationRange* range = nullptr;
  long long hz = 0;
  bool named = false;  // the query's first word is in its label
};
struct Stations {
  std::vector<StationFile> files;  // by name
  std::vector<std::string> errors;
  const StationFile* find(const std::string& id) const;
  const StationRange* rangeAt(long long hz, const bool* shown = nullptr) const;  // shown: one per service
  // Every word must appear, in any case, in the name, group, notes, service or frequency in kHz. Those it
  // names come first, then by frequency. An empty query finds everything.
  std::vector<StationHit> search(const std::string& query) const;
};
bool parseStations(const std::string& json, const std::string& id, StationFile& out, std::string& error);
Stations builtinStations();
// Works as for plans: built-in files, each replaced by a good file of its name in dir, plus any others there.
Stations loadStations(const std::string& dir);
void installStations(const std::string& dir);
bool resetStations(const std::string& dir, const std::string& id);

// The ADIF name of the band a frequency is in, such as "20M", or "" if none. This is the worldwide
// definition from the ADIF spec, whichever plan is showing: a contact logged is on its band regardless.
const char* adifBand(long long hz);
}  // namespace bands
