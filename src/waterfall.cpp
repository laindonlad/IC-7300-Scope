#include "waterfall.h"

#include <GLFW/glfw3.h>
#include <math.h>
#include <string.h>

#include <algorithm>

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

const char* const kPaletteNames[PAL_COUNT] = {"Classic", "Thermal", "Viridis", "Ice", "Grey"};

namespace {
struct Stop {
  float at;
  uint8_t r, g, b;
};
const Stop kClassic[] = {{0, 0, 0, 16}, {.22f, 0, 16, 96}, {.42f, 0, 110, 190}, {.6f, 0, 210, 140}, {.76f, 240, 230, 40}, {.9f, 250, 110, 10}, {1, 255, 255, 255}};
const Stop kThermal[] = {{0, 0, 0, 4}, {.25f, 60, 10, 100}, {.5f, 170, 40, 90}, {.72f, 240, 110, 30}, {.9f, 250, 210, 70}, {1, 255, 255, 220}};
const Stop kViridis[] = {{0, 68, 1, 84}, {.25f, 59, 82, 139}, {.5f, 33, 145, 140}, {.75f, 94, 201, 98}, {1, 253, 231, 37}};
const Stop kIce[] = {{0, 2, 4, 20}, {.4f, 14, 60, 120}, {.7f, 70, 170, 220}, {1, 240, 255, 255}};
const Stop kGrey[] = {{0, 0, 0, 0}, {1, 255, 255, 255}};

uint32_t rgba(uint8_t r, uint8_t g, uint8_t b) {
  // GL_RGBA with GL_UNSIGNED_BYTE is bytes r,g,b,a in memory, which is this on a little-endian machine.
  return (uint32_t)r | (uint32_t)g << 8 | (uint32_t)b << 16 | 0xFF000000u;
}

void paletteAt(int palette, float t, uint8_t& r, uint8_t& g, uint8_t& b) {
  const Stop* s = kClassic;
  int n = 7;
  switch (palette) {
    case PAL_THERMAL: s = kThermal, n = 6; break;
    case PAL_VIRIDIS: s = kViridis, n = 5; break;
    case PAL_ICE: s = kIce, n = 4; break;
    case PAL_GREY: s = kGrey, n = 2; break;
  }
  for (int i = 1; i < n; i++)
    if (t <= s[i].at || i == n - 1) {
      float f = (t - s[i - 1].at) / (s[i].at - s[i - 1].at);
      f = f < 0 ? 0 : f > 1 ? 1 : f;
      r = (uint8_t)(s[i - 1].r + (s[i].r - s[i - 1].r) * f);
      g = (uint8_t)(s[i - 1].g + (s[i].g - s[i - 1].g) * f);
      b = (uint8_t)(s[i - 1].b + (s[i].b - s[i - 1].b) * f);
      return;
    }
}
}  // namespace

Waterfall::~Waterfall() {
  // The GL context can already have gone by now, if the window closed first, so this is App's to order.
  if (tex_ && glfwGetCurrentContext()) glDeleteTextures(1, &tex_);
}

void Waterfall::init(int cols, int rows) {
  GLint maxSize = 4096;
  glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxSize);
  cols = std::min(cols, (int)maxSize);
  rows = std::min(rows, (int)maxSize);
  cols_ = cols, rows_ = rows, head_ = 0, filled_ = 0;
  levels_.assign((size_t)cols * rows, 0);
  rgba_.assign((size_t)cols * rows, lut_[0]);
  dirtyRows_.clear();
  if (!tex_) glGenTextures(1, &tex_);
  glBindTexture(GL_TEXTURE_2D, tex_);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, cols, rows, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  colourDirty_ = true;
}

void Waterfall::clear() {
  if (!cols_) return;
  std::fill(levels_.begin(), levels_.end(), (uint8_t)0);
  head_ = filled_ = 0;
  colourDirty_ = true;
}

void Waterfall::pushRow(const uint8_t* in, int n) {
  if (!cols_ || n <= 0) return;
  uint8_t* row = &levels_[(size_t)head_ * cols_];
  if (n == cols_) {
    memcpy(row, in, (size_t)cols_);
  } else if (n > cols_) {
    for (int x = 0; x < cols_; x++) {
      int a = (int)((long long)x * n / cols_), b = std::max(a + 1, (int)((long long)(x + 1) * n / cols_));
      uint8_t m = 0;
      for (int i = a; i < b && i < n; i++) m = std::max(m, in[i]);
      row[x] = m;
    }
  } else {
    for (int x = 0; x < cols_; x++) {
      float p = (float)x * (n - 1) / (float)std::max(1, cols_ - 1);
      int i = (int)p;
      float f = p - (float)i;
      row[x] = (uint8_t)(in[i] + (in[std::min(i + 1, n - 1)] - in[i]) * f);
    }
  }
  dirtyRows_.push_back(head_);
  head_ = (head_ + 1) % rows_;
  filled_ = std::min(filled_ + 1, rows_);
}

void Waterfall::setDisplay(float lo, float hi, int palette) {
  if (lo == lo_ && hi == hi_ && palette == palette_ && lut_[255]) return;
  lo_ = lo, hi_ = hi, palette_ = palette;
  float span = std::max(1.f, hi - lo);
  for (int i = 0; i < 256; i++) {
    float t = ((float)i - lo) / span;
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    uint8_t r, g, b;
    paletteAt(palette, t, r, g, b);
    lut_[i] = rgba(r, g, b);
  }
  colourDirty_ = true;
}

void Waterfall::colourRow(int row) {
  const uint8_t* l = &levels_[(size_t)row * cols_];
  uint32_t* c = &rgba_[(size_t)row * cols_];
  for (int x = 0; x < cols_; x++) c[x] = lut_[l[x]];
}

void Waterfall::flush() {
  glBindTexture(GL_TEXTURE_2D, tex_);
  if (colourDirty_) {
    for (int r = 0; r < rows_; r++) colourRow(r);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, cols_, rows_, GL_RGBA, GL_UNSIGNED_BYTE, rgba_.data());
    colourDirty_ = false;
  } else {
    for (int r : dirtyRows_) {
      colourRow(r);
      glTexSubImage2D(GL_TEXTURE_2D, 0, 0, r, cols_, 1, GL_RGBA, GL_UNSIGNED_BYTE, &rgba_[(size_t)r * cols_]);
    }
  }
  dirtyRows_.clear();
}

void Waterfall::draw(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float u0, float u1, float pxPerRow) {
  if (!cols_) return;
  flush();
  ImTextureID id = (ImTextureID)(intptr_t)tex_;
  float h = p1.y - p0.y;
  // A fixed height a row, so the scroll speed doesn't change with the window. The last row may hang off
  // the bottom, where the caller's clip rect cuts it.
  int visible = std::min((int)ceilf(h / pxPerRow), rows_);
  float R = (float)rows_;
  int atTop = std::min(visible, head_);  // rows head-1 downward, before the ring wraps
  float y = p0.y;
  if (atTop > 0) {
    float y2 = y + atTop * pxPerRow;
    dl->AddImage(id, ImVec2(p0.x, y), ImVec2(p1.x, y2), ImVec2(u0, (float)head_ / R), ImVec2(u1, (float)(head_ - atTop) / R));
    y = y2;
  }
  int rest = visible - atTop;
  if (rest > 0)
    dl->AddImage(id, ImVec2(p0.x, y), ImVec2(p1.x, y + rest * pxPerRow), ImVec2(u0, 1.f), ImVec2(u1, (float)(rows_ - rest) / R));
}
