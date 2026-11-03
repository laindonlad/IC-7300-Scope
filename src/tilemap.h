#pragma once
// A street and satellite map made of web map tiles, in the web Mercator projection that online maps use. Tiles
// come from OpenStreetMap and Esri on threads of their own, are kept on disk in the data folder so places already
// seen need no network, and are held as GL textures while they're on the screen.
#include <stdint.h>

#include <memory>
#include <string>
#include <unordered_map>

#include "imgui.h"

namespace tilemap {

enum Layer { STREET, TOPO, SATELLITE, HYBRID, LAYER_COUNT };
extern const char* const kLayerNames[LAYER_COUNT];
extern const char* const kLayerTips[LAYER_COUNT];

// x and y run 0 to 1 across and down the whole world.
void project(double latDeg, double lonDeg, double& x, double& y);
void unproject(double x, double y, double& latDeg, double& lonDeg);

struct View {
  double cx = 0.5, cy = 0.38;  // what's in the middle; the world wraps round east to west
  double zoom = 1.5;           // the world is 256 << zoom tiles' pixels across, times the interface's scale
};
const double kMaxZoom = 20;

class Map {
 public:
  Map();
  ~Map();  // needs the GL context still current
  Map(const Map&) = delete;
  Map& operator=(const Map&) = delete;

  // Where tiles are kept on disk. Set before the first draw.
  void setCacheDir(const std::string& dir);

  // The view's scale, in screen pixels per world unit.
  static double worldPixels(const View& v, float uiScale);
  static ImVec2 toScreen(const View& v, ImVec2 p0, ImVec2 size, float uiScale, double x, double y);
  // Keeps the world filling the height if it can, and the zoom in range.
  static void clamp(View& v, ImVec2 size, float uiScale);
  static void zoomAt(View& v, ImVec2 p0, ImVec2 size, float uiScale, ImVec2 mouse, double factor);
  static void pan(View& v, ImVec2 size, float uiScale, ImVec2 pixels);

  // Draws what tiles it has, asks for those it hasn't, and puts the attribution in the corner. The caller sets the
  // clip rectangle.
  void draw(ImDrawList* dl, ImVec2 p0, ImVec2 size, const View& v, Layer layer, float uiScale, ImFont* small);
  // How many tiles are still on their way, and the last thing that went wrong fetching one.
  int pending() const;
  std::string lastError() const;
  // The last tile asked for couldn't be had, from the network or the disk.
  bool failing() const;

  struct Shared;  // with the threads that fetch tiles

 private:
  struct Tile {
    unsigned int tex = 0;
    uint64_t lastUsed = 0;
  };
  void upload();
  void evict();
  const Tile* tile(uint64_t key);

  std::shared_ptr<Shared> sh_;
  std::unordered_map<uint64_t, Tile> tiles_;
  uint64_t frame_ = 0;
};

}  // namespace tilemap
