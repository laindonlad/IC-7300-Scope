#pragma once
// A scrolling waterfall in one GL texture, used as a ring: a new line overwrites the oldest and only the
// texture row changes, however large the window. The raw levels are kept beside it, so brightness,
// contrast and palette apply to the history as well as what comes next.
#include <stdint.h>

#include <vector>

#include "imgui.h"

enum Palette { PAL_CLASSIC, PAL_THERMAL, PAL_VIRIDIS, PAL_ICE, PAL_GREY, PAL_COUNT };
extern const char* const kPaletteNames[PAL_COUNT];

class Waterfall {
 public:
  ~Waterfall();
  // Needs a current GL context. Clears any history.
  void init(int cols, int rows);
  int cols() const { return cols_; }
  int rows() const { return rows_; }
  // n levels, 0-255. If n isn't cols, it's fitted: wider lines keep each pixel's strongest value, as a
  // weak signal between two pixels is what the waterfall is for.
  void pushRow(const uint8_t* levels, int n);
  // Levels below lo are the palette's bottom; above hi, its top.
  void setDisplay(float lo, float hi, int palette);
  void clear();
  // Draws the newest row at the top. u0 and u1 pick the part of the width to show, 0-1.
  void draw(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float u0, float u1, float pxPerRow);

 private:
  void recolourAll();
  void flush();
  void colourRow(int row);
  int cols_ = 0, rows_ = 0, head_ = 0, filled_ = 0;
  unsigned int tex_ = 0;
  std::vector<uint8_t> levels_;
  std::vector<uint32_t> rgba_;
  std::vector<int> dirtyRows_;
  bool allDirty_ = false, colourDirty_ = false;
  float lo_ = 0, hi_ = 255;
  int palette_ = 0;
  uint32_t lut_[256] = {};
};
