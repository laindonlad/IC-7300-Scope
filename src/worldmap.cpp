#include "worldmap.h"

#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <string>
#include <vector>

#include "worldmap_data.inc"

namespace worldmap {
namespace {

const double kRad = M_PI / 180;
const double kMaxX = M_PI;
const double kMaxY = 1.25 * log(tan(M_PI / 4 + 0.4 * M_PI / 2));  // Miller's projection of the pole
const double kMaxZoom = 40;
const double kFineFromPxPerDegree = 10;  // above this the detailed level takes over from the whole-world one

const ImU32 C_OUTSIDE = IM_COL32(9, 14, 20, 255);
const ImU32 C_SEA = IM_COL32(16, 42, 67, 255);
const ImU32 C_LAND = IM_COL32(72, 92, 68, 255);
const ImU32 C_COAST = IM_COL32(176, 196, 176, 255);
const ImU32 C_BORDER = IM_COL32(150, 162, 146, 200);
const ImU32 C_GRID = IM_COL32(255, 255, 255, 26);
const ImU32 C_GRID_TEXT = IM_COL32(190, 215, 240, 90);
const ImU32 C_SHADOW = IM_COL32(0, 0, 0, 190);
const ImU32 C_COUNTRY = IM_COL32(240, 240, 225, 235);
const ImU32 C_TOWN = IM_COL32(225, 225, 215, 215);

struct Box {
  float x0 = FLT_MAX, y0 = FLT_MAX, x1 = -FLT_MAX, y1 = -FLT_MAX;
  void add(float x, float y) {
    x0 = std::min(x0, x), x1 = std::max(x1, x);
    y0 = std::min(y0, y), y1 = std::max(y1, y);
  }
  bool meets(const Box& o) const { return x0 <= o.x1 && o.x0 <= x1 && y0 <= o.y1 && o.y0 <= y1; }
};
struct Batch {
  uint32_t firstVert, verts, firstIndex, indices;
  Box box;
};
struct Mesh {
  std::vector<float> xy;
  std::vector<uint16_t> index;
  std::vector<Batch> batches;
};
struct Run {
  uint32_t first, points;
  Box box;
};
struct Runs {
  std::vector<float> xy;
  std::vector<Run> runs;
};
struct Level {
  Mesh land, lakes;
  Runs coast, borders;
};
struct Label {
  float x, y;
  uint8_t kind, rank;  // kind 0 is a country, 1 a town
  std::string name;
};
struct Data {
  Level levels[2];
  std::vector<Label> labels;
};

struct Reader {
  const unsigned char* at;
  const unsigned char* end;
  template <typename T>
  T get() {
    T v{};
    if (at + sizeof v <= end) memcpy(&v, at, sizeof v), at += sizeof v;
    return v;
  }
  // Reads n longitude and latitude pairs, as map units.
  void points(uint32_t n, std::vector<float>& xy, Box& box) {
    for (uint32_t i = 0; i < n; i++) {
      double lon = get<int16_t>() * 180.0 / 32767, lat = get<int16_t>() * 90.0 / 32767;
      float x, y;
      project(lon, lat, x, y);
      xy.push_back(x), xy.push_back(y);
      box.add(x, y);
    }
  }
};

void readMesh(Reader& r, Mesh& m) {
  uint32_t count = r.get<uint32_t>();
  m.batches.reserve(count);
  for (uint32_t b = 0; b < count; b++) {
    Batch batch{};
    batch.verts = r.get<uint16_t>(), batch.indices = r.get<uint32_t>();
    batch.firstVert = (uint32_t)(m.xy.size() / 2), batch.firstIndex = (uint32_t)m.index.size();
    r.points(batch.verts, m.xy, batch.box);
    for (uint32_t i = 0; i < batch.indices; i++) m.index.push_back(r.get<uint16_t>());
    m.batches.push_back(batch);
  }
}

void readRuns(Reader& r, Runs& l) {
  uint32_t count = r.get<uint32_t>();
  l.runs.reserve(count);
  for (uint32_t i = 0; i < count; i++) {
    Run run{};
    run.points = r.get<uint16_t>();
    run.first = (uint32_t)(l.xy.size() / 2);
    r.points(run.points, l.xy, run.box);
    l.runs.push_back(run);
  }
}

// Read the first time the map is shown, as most sessions never open it.
const Data& data() {
  static Data* d = [] {
    Data* out = new Data;
    Reader r{kWorldMap, kWorldMap + sizeof kWorldMap};
    if (sizeof kWorldMap < 4 || memcmp(r.at, "WMP1", 4) != 0) return out;
    r.at += 4;
    for (Level& l : out->levels) {
      readMesh(r, l.land), readMesh(r, l.lakes);
      readRuns(r, l.coast), readRuns(r, l.borders);
    }
    uint32_t count = r.get<uint32_t>();
    out->labels.reserve(count);
    for (uint32_t i = 0; i < count; i++) {
      Label label{};
      double lon = r.get<int16_t>() * 180.0 / 32767, lat = r.get<int16_t>() * 90.0 / 32767;
      project(lon, lat, label.x, label.y);
      label.kind = r.get<uint8_t>(), label.rank = r.get<uint8_t>();
      uint8_t len = r.get<uint8_t>();
      if (r.at + len > r.end) break;
      // The file has Latin-1 and ImGui wants UTF-8.
      for (uint8_t c = 0; c < len; c++) {
        unsigned char ch = r.at[c];
        if (ch < 0x80) label.name += (char)ch;
        else label.name += (char)(0xC0 | (ch >> 6)), label.name += (char)(0x80 | (ch & 0x3F));
      }
      r.at += len;
      out->labels.push_back(std::move(label));
    }
    return out;
  }();
  return *d;
}

double baseScale(ImVec2 size) { return std::min(size.x / (2 * kMaxX), size.y / (2 * kMaxY)); }

// What it takes to turn map units into pixels this frame.
struct Xform {
  double s, ox, oy, cx, cy;
  ImVec2 at(float x, float y) const { return ImVec2((float)(ox + (x - cx) * s), (float)(oy - (y - cy) * s)); }
};

void drawMesh(ImDrawList* dl, const Mesh& m, const Xform& t, const Box& seen, ImU32 col, bool largeMeshes) {
  const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
  for (const Batch& b : m.batches) {
    if (!b.box.meets(seen)) continue;
    // A very old OpenGL can't address more than 64k vertices at once, so there it stops short instead of scrambling.
    if (!largeMeshes && dl->_VtxCurrentIdx + b.verts >= 65535) return;
    dl->PrimReserve((int)b.indices, (int)b.verts);
    const unsigned int base = dl->_VtxCurrentIdx;  // only now, as making room can start it again from zero
    for (uint32_t i = 0; i < b.indices; i++) dl->PrimWriteIdx((ImDrawIdx)(base + m.index[b.firstIndex + i]));
    const float* p = &m.xy[b.firstVert * 2];
    for (uint32_t i = 0; i < b.verts; i++) dl->PrimWriteVtx(t.at(p[i * 2], p[i * 2 + 1]), uv, col);
  }
}

void drawRuns(ImDrawList* dl, const Runs& l, const Xform& t, const Box& seen, ImU32 col, float width, bool largeMeshes) {
  static std::vector<ImVec2> pts;
  for (const Run& r : l.runs) {
    if (!r.box.meets(seen)) continue;
    if (!largeMeshes && dl->_VtxCurrentIdx + r.points * 4 >= 65535) return;
    pts.clear();
    const float* p = &l.xy[r.first * 2];
    for (uint32_t i = 0; i < r.points; i++) {
      ImVec2 q = t.at(p[i * 2], p[i * 2 + 1]);
      // Points closer than a pixel only cost time, except that a run has to reach its end.
      if (!pts.empty() && i + 1 < r.points) {
        float dx = q.x - pts.back().x, dy = q.y - pts.back().y;
        if (dx * dx + dy * dy < 1.0f) continue;
      }
      pts.push_back(q);
    }
    if (pts.size() >= 2) dl->AddPolyline(pts.data(), (int)pts.size(), col, ImDrawFlags_None, width);
  }
}

void drawGrid(ImDrawList* dl, ImVec2 p0, ImVec2 size, const Xform& t, const Box& seen, const Box& world, double pxPerDeg, float ui, ImFont* font) {
  const ImVec2 lo = t.at(world.x0, world.y1), hi = t.at(world.x1, world.y0);
  const float top = std::max(lo.y, p0.y), bottom = std::min(hi.y, p0.y + size.y), left = std::max(lo.x, p0.x), right = std::min(hi.x, p0.x + size.x);
  auto lines = [&](double lonStep, double latStep) {
    for (double lon = std::max(-180.0, ceil((seen.x0 / kRad + 180) / lonStep) * lonStep - 180); lon * kRad <= seen.x1 && lon <= 180; lon += lonStep) {
      float x, y;
      project(lon, 0, x, y);
      float sx = t.at(x, 0).x;
      dl->AddLine(ImVec2(sx, top), ImVec2(sx, bottom), C_GRID);
    }
    for (double lat = -90; lat <= 90; lat += latStep) {
      float x, y;
      project(0, lat, x, y);
      if (y < seen.y0 || y > seen.y1) continue;
      float sy = t.at(0, y).y;
      dl->AddLine(ImVec2(left, sy), ImVec2(right, sy), C_GRID);
    }
  };
  const double squarePx = 2 * pxPerDeg;
  lines(20, 10);
  if (squarePx >= 24 * ui) lines(2, 1);

  // Names go in the bottom left corner of each, which is the corner a locator counts from.
  const bool nameSquares = squarePx >= 44 * ui;
  if (!nameSquares && 20 * pxPerDeg < 44 * ui) return;
  const double lonStep = nameSquares ? 2 : 20, latStep = nameSquares ? 1 : 10;
  const float fontSize = font->FontSize, pad = 3 * ui;
  for (double lon = std::max(-180.0, floor((seen.x0 / kRad + 180) / lonStep) * lonStep - 180); lon * kRad <= seen.x1 && lon < 180; lon += lonStep) {
    for (double lat = -90; lat < 90; lat += latStep) {
      float x, y, topY, unused;
      project(lon, lat, x, y);
      project(lon, lat + latStep, unused, topY);
      if (topY < seen.y0 || y > seen.y1) continue;
      char name[5] = {(char)('A' + (int)((lon + 180) / 20)), (char)('A' + (int)((lat + 90) / 10)), 0, 0, 0};
      if (nameSquares) {
        name[2] = (char)('0' + (int)fmod((lon + 180) / 2, 10));
        name[3] = (char)('0' + (int)fmod(lat + 90, 10));
      }
      ImVec2 at = t.at(x, y);
      dl->AddText(font, fontSize, ImVec2(at.x + pad, at.y - fontSize - pad / 2), C_GRID_TEXT, name);
    }
  }
}

void drawLabels(ImDrawList* dl, const Data& d, const Xform& t, const Box& seen, double pxPerDeg, float ui, ImFont* countryFont, ImFont* townFont) {
  // How much there's room for, judged on the interface's own scale. A label that would still overlap one placed
  // before it is left out, and the biggest are placed first.
  const double room = pxPerDeg / ui;
  const int countries = room < 3 ? 3 : room < 5 ? 4 : room < 9 ? 5 : room < 16 ? 6 : 10;
  const int towns = room < 7 ? -1 : room < 12 ? 1 : room < 20 ? 3 : room < 35 ? 4 : room < 60 ? 5 : room < 110 ? 6 : 7;
  struct Placed {
    float x0, y0, x1, y1;
  };
  static std::vector<Placed> placed;
  placed.clear();
  auto free = [&](const Placed& p) {
    for (const Placed& o : placed)
      if (p.x0 < o.x1 && o.x0 < p.x1 && p.y0 < o.y1 && o.y0 < p.y1) return false;
    return true;
  };
  const float gap = 2 * ui, dot = 2 * ui;
  for (const Label& l : d.labels) {
    if ((int)l.rank > (l.kind == 0 ? countries : towns)) continue;
    if (l.x < seen.x0 || l.x > seen.x1 || l.y < seen.y0 || l.y > seen.y1) continue;
    ImFont* font = l.kind == 0 ? countryFont : townFont;
    const float fs = font->FontSize;
    const char* text = l.name.c_str();
    ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0, text);
    ImVec2 at = t.at(l.x, l.y), pos;
    if (l.kind == 0) pos = ImVec2(at.x - ts.x / 2, at.y - ts.y / 2);
    else pos = ImVec2(at.x + dot + gap, at.y - ts.y / 2);
    Placed p{(l.kind == 0 ? pos.x : at.x - dot) - gap, pos.y - gap, pos.x + ts.x + gap, pos.y + ts.y + gap};
    if (!free(p)) continue;
    placed.push_back(p);
    if (l.kind != 0) {
      dl->AddCircleFilled(at, dot + 1, C_SHADOW, 10);
      dl->AddCircleFilled(at, dot, C_TOWN, 10);
    }
    dl->AddText(font, fs, ImVec2(pos.x + 1, pos.y + 1), C_SHADOW, text);
    dl->AddText(font, fs, pos, l.kind == 0 ? C_COUNTRY : C_TOWN, text);
  }
}

}  // namespace

void project(double lonDeg, double latDeg, float& x, float& y) {
  x = (float)(lonDeg * kRad);
  y = (float)(1.25 * log(tan(M_PI / 4 + 0.4 * latDeg * kRad)));
}

ImVec2 toScreen(const View& v, ImVec2 p0, ImVec2 size, float x, float y) {
  double s = baseScale(size) * v.zoom;
  return ImVec2((float)(p0.x + size.x / 2 + (x - v.cx) * s), (float)(p0.y + size.y / 2 - (y - v.cy) * s));
}

void clamp(View& v, ImVec2 size) {
  v.zoom = std::max(1.0, std::min(kMaxZoom, v.zoom));
  double s = baseScale(size) * v.zoom;
  double roomX = kMaxX - size.x / 2 / s, roomY = kMaxY - size.y / 2 / s;
  v.cx = roomX > 0 ? std::max(-roomX, std::min(roomX, v.cx)) : 0;
  v.cy = roomY > 0 ? std::max(-roomY, std::min(roomY, v.cy)) : 0;
}

void zoomAt(View& v, ImVec2 p0, ImVec2 size, ImVec2 mouse, double factor) {
  double mx = mouse.x - p0.x - size.x / 2, my = mouse.y - p0.y - size.y / 2;
  double before = baseScale(size) * v.zoom;
  double wx = v.cx + mx / before, wy = v.cy - my / before;
  v.zoom = std::max(1.0, std::min(kMaxZoom, v.zoom * factor));
  double after = baseScale(size) * v.zoom;
  v.cx = wx - mx / after, v.cy = wy + my / after;
  clamp(v, size);
}

void pan(View& v, ImVec2 size, ImVec2 pixels) {
  double s = baseScale(size) * v.zoom;
  v.cx -= pixels.x / s, v.cy += pixels.y / s;
  clamp(v, size);
}

void draw(ImDrawList* dl, ImVec2 p0, ImVec2 size, const View& v, float uiScale, ImFont* countryFont, ImFont* townFont) {
  const Data& d = data();
  const double s = baseScale(size) * v.zoom;
  const Xform t{s, p0.x + size.x / 2.0, p0.y + size.y / 2.0, v.cx, v.cy};
  const ImVec2 p1(p0.x + size.x, p0.y + size.y);

  Box world, seen;
  world.add((float)-kMaxX, (float)-kMaxY), world.add((float)kMaxX, (float)kMaxY);
  seen.add((float)(v.cx - size.x / 2 / s), (float)(v.cy - size.y / 2 / s));
  seen.add((float)(v.cx + size.x / 2 / s), (float)(v.cy + size.y / 2 / s));

  dl->AddRectFilled(p0, p1, C_OUTSIDE);
  dl->AddRectFilled(t.at(world.x0, world.y1), t.at(world.x1, world.y0), C_SEA);

  const double pxPerDeg = s * kRad;
  const Level& level = d.levels[pxPerDeg >= kFineFromPxPerDegree ? 1 : 0];
  const bool large = (ImGui::GetIO().BackendFlags & ImGuiBackendFlags_RendererHasVtxOffset) != 0;
  drawMesh(dl, level.land, t, seen, C_LAND, large);
  drawMesh(dl, level.lakes, t, seen, C_SEA, large);
  drawGrid(dl, p0, size, t, seen, world, pxPerDeg, uiScale, townFont);
  drawRuns(dl, level.borders, t, seen, C_BORDER, 1.0f * uiScale, large);
  drawRuns(dl, level.coast, t, seen, C_COAST, 1.0f * uiScale, large);
  drawLabels(dl, d, t, seen, pxPerDeg, uiScale, countryFont, townFont);
}

}  // namespace worldmap
