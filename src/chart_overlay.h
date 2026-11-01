#pragma once
// Maps laid over a WEFAX chart: transparent PNGs in the overlays folder, each drawn for one station's product.
// An overlay is scaled to the chart's width, as they're made to cover a whole chart. Saving makes a new picture
// beside the chart and never touches the chart itself.
#include <stdint.h>

#include <string>
#include <vector>

namespace chart_overlay {
struct Image {
  int w = 0, h = 0;
  std::vector<uint8_t> rgba;  // straight alpha, top row first
};

// Writes any built-in overlay that's missing from dir, and a note on how they work. Never touches an existing file.
void install(const std::string& dir);
// The PNG files in dir, by name.
std::vector<std::string> list(const std::string& dir);
// A file's name as shown: without its extension.
std::string title(const std::string& file);
bool load(const std::string& path, Image& out);
// Averaged when shrinking, so thin coastlines don't break up. Weighted by alpha, so they don't pick up a dark edge
// from whatever colour the transparent pixels happen to hold.
Image resized(const Image& src, int w, int h);
// The chart at chartPath (one of the decoder's 8-bit grey BMPs) with the overlay on it, x and y along and down in
// the chart's own pixels. Saved as CHART-OVERLAY.png beside the chart, or CHART-OVERLAY-2.png and so on if that's
// taken. False with a message if it can't be done, and then there's no new file.
bool save(const std::string& chartPath, const Image& overlay, const std::string& overlayFile, int x, int y, std::string& saved, std::string& error);
}  // namespace chart_overlay
