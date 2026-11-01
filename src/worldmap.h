#pragma once
// The world map behind the log book: land, lakes, coasts, borders, country and town names and the Maidenhead grid,
// from Natural Earth data built into the program by tools/make_world_map.py. Drawn as shapes, not a picture, so it
// stays sharp however far in it's zoomed. Uses Miller's projection, which shows the whole world including the poles
// and keeps countries looking as people expect.
#include "imgui.h"

namespace worldmap {
// Map units are radians of longitude across, and Miller's projected latitude up.
struct View {
  double cx = 0, cy = 0;  // what's in the middle
  double zoom = 1;        // 1 fits the whole world in
};

void project(double lonDeg, double latDeg, float& x, float& y);
ImVec2 toScreen(const View& v, ImVec2 p0, ImVec2 size, float x, float y);
// Keeps the world on the screen, after anything that moves the view or changes its size.
void clamp(View& v, ImVec2 size);
// Zooms by a factor, keeping what's under the mouse where it is.
void zoomAt(View& v, ImVec2 p0, ImVec2 size, ImVec2 mouse, double factor);
void pan(View& v, ImVec2 size, ImVec2 pixels);

// The caller sets the clip rectangle. uiScale is the interface's scale, for line widths and how much text fits.
void draw(ImDrawList* dl, ImVec2 p0, ImVec2 size, const View& v, float uiScale, ImFont* countryFont, ImFont* townFont);
}  // namespace worldmap
