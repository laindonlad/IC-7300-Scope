#include "chart_overlay.h"

#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <filesystem>
#include <system_error>

#include "store.h"

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include "builtin_overlays.inc"

namespace chart_overlay {
namespace fs = std::filesystem;

static const char kNotes[] =
    "Map overlays for WEFAX charts\n"
    "\n"
    "Each .png file here appears under Overlay, above the chart in the WEFAX panel. Add your own beside\n"
    "northwood_land_overlay.png, which is an example, and they'll be listed the next time you open the list.\n"
    "\n"
    "An overlay is a picture with a transparent background: the land, say, with everything else see-through.\n"
    "Make one by tracing a whole chart of one product, such as one station's surface analysis, since the same\n"
    "product always covers the same area. It can be any size. It's scaled to the chart's width, with its top\n"
    "left corner on the chart's, so draw it over the whole chart, margins included.\n"
    "\n"
    "X and Y move it, for a chart that was tuned in late. Save a copy puts the chart with its overlay in the\n"
    "received folder as a new PNG. The chart itself is never changed.\n";

void install(const std::string& dir) {
  std::error_code ec;
  fs::path d(dir);
  fs::create_directories(d, ec);
  for (const auto& b : kBuiltinOverlays) {
    fs::path p = d / (std::string(b.id) + ".png");
    if (!fs::exists(p, ec)) store::saveText(p.string(), std::string((const char*)b.data, b.size));
  }
  store::saveText((d / "README.txt").string(), kNotes);
}

std::vector<std::string> list(const std::string& dir) {
  std::vector<std::string> names;
  std::error_code ec;
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    std::string ext = it->path().extension().string();
    for (char& c : ext) c = (char)tolower((unsigned char)c);
    if (ext == ".png") names.push_back(it->path().filename().string());
  }
  std::sort(names.begin(), names.end());
  return names;
}

std::string title(const std::string& file) {
  return file.substr(0, file.find_last_of('.'));
}

bool load(const std::string& path, Image& out) {
  int w, h, channels;
  stbi_uc* px = stbi_load(path.c_str(), &w, &h, &channels, 4);
  if (!px) return false;
  out.w = w;
  out.h = h;
  out.rgba.assign(px, px + (size_t)w * h * 4);
  stbi_image_free(px);
  return true;
}

Image resized(const Image& src, int w, int h) {
  Image out;
  out.w = w;
  out.h = h;
  out.rgba.assign((size_t)w * h * 4, 0);
  for (int y = 0; y < h; y++) {
    int y0 = (int)((long long)y * src.h / h);
    int y1 = std::max(y0 + 1, (int)((long long)(y + 1) * src.h / h));
    for (int x = 0; x < w; x++) {
      int x0 = (int)((long long)x * src.w / w);
      int x1 = std::max(x0 + 1, (int)((long long)(x + 1) * src.w / w));
      unsigned long long r = 0, g = 0, b = 0, a = 0;
      for (int sy = y0; sy < y1; sy++) {
        const uint8_t* s = &src.rgba[((size_t)sy * src.w + x0) * 4];
        for (int sx = x0; sx < x1; sx++, s += 4) {
          r += (unsigned)s[0] * s[3];
          g += (unsigned)s[1] * s[3];
          b += (unsigned)s[2] * s[3];
          a += s[3];
        }
      }
      if (!a) continue;
      uint8_t* d = &out.rgba[((size_t)y * w + x) * 4];
      d[0] = (uint8_t)(r / a);
      d[1] = (uint8_t)(g / a);
      d[2] = (uint8_t)(b / a);
      d[3] = (uint8_t)(a / ((unsigned long long)(x1 - x0) * (y1 - y0)));
    }
  }
  return out;
}

// One of the decoder's charts, as RGB. Fewer rows than the header says is fine: a chart can be cut short.
static bool readChart(const std::string& path, int& w, int& h, std::vector<uint8_t>& rgb) {
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return false;
  uint8_t head[54];
  bool ok = fread(head, 1, sizeof head, f) == sizeof head && head[0] == 'B' && head[1] == 'M';
  auto at = [&](int i) { return (uint32_t)head[i] | (uint32_t)head[i + 1] << 8 | (uint32_t)head[i + 2] << 16 | (uint32_t)head[i + 3] << 24; };
  if (ok) {
    w = (int)at(18);
    int rows = (int)at(22);
    bool topDown = rows < 0;
    h = topDown ? -rows : rows;
    ok = w > 0 && h > 0 && w <= 20000 && h <= 100000 && head[28] == 8 && fseek(f, (long)at(10), SEEK_SET) == 0;
    if (ok) {
      std::vector<uint8_t> row((size_t)((w + 3) & ~3));
      rgb.assign((size_t)w * h * 3, 255);
      int got = 0;
      for (; got < h && fread(row.data(), 1, row.size(), f) == row.size(); got++) {
        uint8_t* d = &rgb[(size_t)(topDown ? got : h - 1 - got) * w * 3];
        for (int x = 0; x < w; x++, d += 3) d[0] = d[1] = d[2] = row[(size_t)x];
      }
      ok = got > 0;
      if (ok && topDown) h = got;
    }
  }
  fclose(f);
  return ok;
}

bool save(const std::string& chartPath, const Image& overlay, const std::string& overlayFile, int x, int y, std::string& saved, std::string& error) {
  int w = 0, h = 0;
  std::vector<uint8_t> rgb;
  if (!readChart(chartPath, w, h, rgb)) return error = "Can't read the chart " + chartPath, false;
  if (overlay.w <= 0 || overlay.h <= 0) return error = "There's no overlay to save", false;

  int oh = std::max(1, (int)(((long long)overlay.h * w + overlay.w / 2) / overlay.w));
  Image o = resized(overlay, w, oh);
  for (int oy = std::max(0, -y); oy < oh && oy + y < h; oy++)
    for (int ox = std::max(0, -x); ox < w && ox + x < w; ox++) {
      const uint8_t* s = &o.rgba[((size_t)oy * w + ox) * 4];
      if (!s[3]) continue;
      uint8_t* d = &rgb[((size_t)(oy + y) * w + ox + x) * 3];
      for (int c = 0; c < 3; c++) d[c] = (uint8_t)((s[c] * s[3] + d[c] * (255 - s[3]) + 127) / 255);
    }

  // Never over anything: not the chart, and not a copy saved earlier with the overlay somewhere else.
  fs::path chart(chartPath);
  std::string base = chart.stem().string() + "-" + title(overlayFile);
  std::error_code ec;
  fs::path out = chart.parent_path() / (base + ".png");
  for (int n = 2; fs::exists(out, ec); n++) out = chart.parent_path() / (base + "-" + std::to_string(n) + ".png");
  if (!stbi_write_png(out.string().c_str(), w, h, 3, rgb.data(), w * 3)) {
    fs::remove(out, ec);
    return error = "Can't write " + out.string(), false;
  }
  saved = out.string();
  return true;
}
}  // namespace chart_overlay
