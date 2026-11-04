#pragma once
// Where stations are, for the map: found in the log from a contact's locator, or looked up on QRZ.com, and kept in
// locations.csv in the data folder so the next look is instant and needs no network.
#include <map>
#include <string>
#include <vector>

#include "store.h"

namespace locations {

struct Location {
  std::string call;
  double lat = 0, lon = 0;
  std::string locator, name, qth, country;
  std::string source;    // "Log", "QRZ" or "HamQTH"
  std::string accuracy;  // how the place was found: "locator", or QRZ's geoloc: user, geocode, grid, zip, state, dxcc,
                         // none; or "bearing" or "bearing-grid" when worked out from QRZ's bearing and distance
  std::string updated;   // UTC "YYYY-MM-DD HH:MM"
};

// Upper case, trimmed.
std::string normalCall(const std::string& call);
// The callsign without a prefix or suffix for where it's operating: G4ABC for EA8/G4ABC or G4ABC/P.
std::string baseCall(const std::string& call);
// The 6 character Maidenhead locator for a place.
std::string latLonToGrid(double lat, double lon);
// The edges of a 4 or 6 character locator's square (8 and 10 are read as their first 6). False if it isn't one.
bool gridBounds(const std::string& grid, double& south, double& west, double& north, double& east);
// The Ordnance Survey 10 km square a place is in, such as TQ68, and the grid reference of its south west corner in
// metres. False outside the National Grid, which is for Great Britain.
bool osGridSquare(double lat, double lon, std::string& ref, double& east, double& north);
// A place's National Grid reference in metres, and whether it's on the grid.
bool osGridFromLatLon(double lat, double lon, double& east, double& north);
// The letters of the 100 km square a grid reference in metres is in, such as TQ, or with tenKm the 10 km square,
// such as TQ68. Empty off the grid.
std::string osGridRef(double east, double north, bool tenKm);
// Where a National Grid reference in metres is, in WGS84 like the map.
void osGridToLatLon(double east, double north, double& lat, double& lon);
// How far in the map should zoom to show a place, given how exactly it's known.
double suggestedZoom(const Location& l);
// In words, for the map: "street address", "6-character locator" and so on.
std::string accuracyName(const Location& l);
std::string utcNow();

// The kept locations, by callsign.
class Book {
 public:
  void load(const std::string& path);
  bool save() const;
  const Location* find(const std::string& call) const;
  // Adds it or replaces what was kept for that call, and saves. False if it couldn't be saved.
  bool put(const Location& l);
  size_t size() const { return byCall_.size(); }
  const std::map<std::string, Location>& all() const { return byCall_; }

 private:
  std::string path_;
  std::map<std::string, Location> byCall_;
};

// The newest contact in the log with this callsign and a locator that reads, or false if there's none. Falls back
// on the base callsign, so EA8/G4ABC finds G4ABC's contacts.
bool fromLog(const std::vector<store::LogEntry>& log, const std::string& call, Location& out);

// Looks a callsign up with QRZ.com's XML service, logging in first if sessionKey is empty or has run out, and
// leaving the key in it for next time. Waits on the network, so it belongs on its own thread. False with a message
// if there's no place to be had: a QRZ account without an XML subscription gets no locator or position for most
// callsigns. When it finds no place but QRZ.com's page gives a bearing and distance from the operator's station,
// estimate is the place worked out from those, and haveEstimate is set: a last resort, after other services. Where
// the operator's own station is comes from their QRZ.com page, or failing that from homeLocator.
bool qrzLookup(const std::string& user, const std::string& password, const std::string& call, const std::string& homeLocator, std::string& sessionKey, Location& out, Location& estimate, bool& haveEstimate, std::string& error);

// The same with HamQTH.com's free XML service, which needs only a free HamQTH account. sessionId is kept between
// lookups like QRZ's key. Its place is the station's own locator or position, where they've given one.
bool hamqthLookup(const std::string& user, const std::string& password, const std::string& call, std::string& sessionId, Location& out, std::string& error);

// Where a kept place came from, in words: "QRZ.com", "HamQTH" or "your log".
std::string sourceName(const Location& l);

}  // namespace locations
