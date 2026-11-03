#include "tilemap.h"

#include <GLFW/glfw3.h>
#include <math.h>
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include "net.h"
#include "store.h"

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#include "stb_image.h"

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

namespace tilemap {

const char* const kLayerNames[LAYER_COUNT] = {"Street", "Topo", "Satellite", "Hybrid"};
const char* const kLayerTips[LAYER_COUNT] = {
    "OpenStreetMap: streets, footpaths and buildings, for finding your way on foot",
    "Esri's topographic map: terrain, contours and trails",
    "Esri's satellite and aerial photographs",
    "The photographs with roads and place names over them",
};

namespace {

struct Source {
  const char* folder;  // under the cache folder
  const char* url;     // printf'd with z, then x and y or y and x
  bool yx;             // y before x in the URL, as Esri has them
  int maxZ;
  bool osm;            // OpenStreetMap asks that no program fetches more than two tiles at once
};
const Source kSources[] = {
    {"osm", "https://tile.openstreetmap.org/%d/%d/%d.png", false, 19, true},
    {"esri-imagery", "https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/%d/%d/%d", true, 19, false},
    {"esri-roads", "https://server.arcgisonline.com/ArcGIS/rest/services/Reference/World_Transportation/MapServer/tile/%d/%d/%d", true, 19, false},
    {"esri-places", "https://server.arcgisonline.com/ArcGIS/rest/services/Reference/World_Boundaries_and_Places/MapServer/tile/%d/%d/%d", true, 19, false},
    {"esri-topo", "https://server.arcgisonline.com/ArcGIS/rest/services/World_Topo_Map/MapServer/tile/%d/%d/%d", true, 19, false},
};
const std::vector<int> kLayerSources[LAYER_COUNT] = {{0}, {4}, {1}, {1, 2, 3}};
const char* const kAttribution[LAYER_COUNT] = {
    "\xC2\xA9 OpenStreetMap contributors",
    "Tiles \xC2\xA9 Esri: Esri, HERE, Garmin, USGS, NGA, EPA, USDA, NPS",
    "Imagery \xC2\xA9 Esri, Maxar, Earthstar Geographics",
    "Imagery \xC2\xA9 Esri, Maxar, Earthstar Geographics; labels \xC2\xA9 Esri, HERE, Garmin",
};

const double kTilePx = 256;
const int kWorkers = 4, kOsmAtOnce = 2;
const size_t kKeepTextures = 300, kMaxTextures = 420;
const int kUploadsPerFrame = 24;
const double kStaleSeconds = 30.0 * 24 * 3600;  // a tile on disk older than this is fetched again
const double kRetrySeconds = 30;

uint64_t keyOf(int src, int z, int x, int y) { return (uint64_t)src << 56 | (uint64_t)z << 48 | (uint64_t)x << 24 | (uint64_t)y; }
int keySrc(uint64_t k) { return (int)(k >> 56); }
int keyZ(uint64_t k) { return (int)(k >> 48) & 0xFF; }
int keyX(uint64_t k) { return (int)(k >> 24) & 0xFFFFFF; }
int keyY(uint64_t k) { return (int)k & 0xFFFFFF; }

double nowSeconds() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

bool readFile(const std::string& path, std::string& out) {
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return false;
  out.clear();
  char buf[16384];
  size_t got;
  while ((got = fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, got);
  fclose(f);
  return !out.empty();
}

void writeFile(const std::string& path, const std::string& data) {
  std::string tmp = path + ".tmp";
  FILE* f = fopen(tmp.c_str(), "wb");
  if (!f) return;
  bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
  ok = fclose(f) == 0 && ok;
  remove(path.c_str());
  if (!ok || rename(tmp.c_str(), path.c_str()) != 0) remove(tmp.c_str());
}

}  // namespace

struct Map::Shared {
  std::mutex m;
  std::condition_variable cv;
  bool quit = false;
  std::string cacheDir;
  std::vector<uint64_t> wanted;  // nearest the middle of the view first; replaced every frame
  std::set<uint64_t> busy;
  int osmBusy = 0;
  std::unordered_map<uint64_t, double> failedUntil;
  struct Ready {
    uint64_t key;
    int w, h;
    std::vector<unsigned char> rgba;
  };
  std::vector<Ready> ready;
  std::set<uint64_t> readyKeys;
  std::string lastError;
  bool failing = false;

  // Under the lock: the first wanted tile nobody is fetching, or -1.
  int pick(double now) const {
    for (size_t i = 0; i < wanted.size(); i++) {
      uint64_t k = wanted[i];
      if (busy.count(k) || readyKeys.count(k)) continue;
      auto f = failedUntil.find(k);
      if (f != failedUntil.end() && f->second > now) continue;
      if (kSources[keySrc(k)].osm && osmBusy >= kOsmAtOnce) continue;
      return (int)i;
    }
    return -1;
  }
};

// Fetches tiles until the map goes. Holds the shared part, so the map can go without waiting for the network.
static void work(std::shared_ptr<Map::Shared> sh) {
  for (;;) {
    uint64_t key;
    std::string dir;
    {
      std::unique_lock<std::mutex> lk(sh->m);
      int i;
      while (!sh->quit && (i = sh->pick(nowSeconds())) < 0) sh->cv.wait_for(lk, std::chrono::seconds(1));
      if (sh->quit) return;
      key = sh->wanted[(size_t)i];
      sh->wanted.erase(sh->wanted.begin() + i);
      sh->busy.insert(key);
      if (kSources[keySrc(key)].osm) sh->osmBusy++;
      dir = sh->cacheDir;
    }
    const Source& src = kSources[keySrc(key)];
    const int z = keyZ(key), x = keyX(key), y = keyY(key);
    char rel[96];
    snprintf(rel, sizeof rel, "%s/%d/%d/%d.img", src.folder, z, x, y);
    std::string path = dir + rel;

    std::string bytes, error;
    bool have = readFile(path, bytes);
    struct stat info;
    bool fresh = have && stat(path.c_str(), &info) == 0 && difftime(time(nullptr), info.st_mtime) < kStaleSeconds;
    if (!fresh) {
      char url[256];
      snprintf(url, sizeof url, src.url, z, src.yx ? y : x, src.yx ? x : y);
      std::string body;
      if (net::httpGet(url, body, error, 2u << 20)) {
        bytes.swap(body);
        have = true;
        store::makeDirs(path.substr(0, path.find_last_of('/')));
        writeFile(path, bytes);
      }
      // If it can't be had, an old copy on disk does instead.
    }
    Map::Shared::Ready r{key, 0, 0, {}};
    bool ok = false;
    if (have) {
      int n = 0;
      unsigned char* px = stbi_load_from_memory((const unsigned char*)bytes.data(), (int)bytes.size(), &r.w, &r.h, &n, 4);
      if (px) {
        r.rgba.assign(px, px + (size_t)r.w * r.h * 4);
        stbi_image_free(px);
        ok = true;
      } else {
        error = "a tile that wouldn't decode";
        remove(path.c_str());
      }
    }
    std::lock_guard<std::mutex> g(sh->m);
    sh->busy.erase(key);
    if (src.osm) sh->osmBusy--;
    sh->failing = !ok;
    if (ok) {
      sh->readyKeys.insert(key);
      sh->ready.push_back(std::move(r));
    } else {
      sh->failedUntil[key] = nowSeconds() + kRetrySeconds;
      sh->lastError = error;
    }
    sh->cv.notify_all();
  }
}

void project(double latDeg, double lonDeg, double& x, double& y) {
  const double lat = std::max(-85.0511, std::min(85.0511, latDeg)) * M_PI / 180;
  x = (lonDeg + 180) / 360;
  y = (1 - log(tan(lat) + 1 / cos(lat)) / M_PI) / 2;
}

void unproject(double x, double y, double& latDeg, double& lonDeg) {
  lonDeg = x * 360 - 180;
  latDeg = atan(sinh(M_PI * (1 - 2 * y))) * 180 / M_PI;
}

Map::Map() : sh_(std::make_shared<Shared>()) {
  for (int i = 0; i < kWorkers; i++) std::thread(work, sh_).detach();
}

Map::~Map() {
  {
    std::lock_guard<std::mutex> g(sh_->m);
    sh_->quit = true;
  }
  sh_->cv.notify_all();
  for (auto& kv : tiles_)
    if (kv.second.tex) glDeleteTextures(1, &kv.second.tex);
}

void Map::setCacheDir(const std::string& dir) {
  std::lock_guard<std::mutex> g(sh_->m);
  sh_->cacheDir = dir;
}

double Map::worldPixels(const View& v, float uiScale) { return kTilePx * uiScale * pow(2.0, v.zoom); }

ImVec2 Map::toScreen(const View& v, ImVec2 p0, ImVec2 size, float uiScale, double x, double y) {
  const double w = worldPixels(v, uiScale);
  return ImVec2((float)(p0.x + size.x / 2 + (x - v.cx) * w), (float)(p0.y + size.y / 2 + (y - v.cy) * w));
}

void Map::clamp(View& v, ImVec2 size, float uiScale) {
  const double least = log2(std::max(1.0, (double)size.y) / (kTilePx * uiScale));
  v.zoom = std::max(least, std::min(kMaxZoom, v.zoom));
  const double half = size.y / 2 / worldPixels(v, uiScale);
  v.cy = half >= 0.5 ? 0.5 : std::max(half, std::min(1 - half, v.cy));
  v.cx -= floor(v.cx);
}

void Map::zoomAt(View& v, ImVec2 p0, ImVec2 size, float uiScale, ImVec2 mouse, double factor) {
  const double before = worldPixels(v, uiScale);
  const double mx = v.cx + (mouse.x - p0.x - size.x / 2) / before, my = v.cy + (mouse.y - p0.y - size.y / 2) / before;
  v.zoom = std::min(kMaxZoom, v.zoom + log2(factor));
  clamp(v, size, uiScale);
  const double after = worldPixels(v, uiScale);
  v.cx = mx - (mouse.x - p0.x - size.x / 2) / after;
  v.cy = my - (mouse.y - p0.y - size.y / 2) / after;
  clamp(v, size, uiScale);
}

void Map::pan(View& v, ImVec2 size, float uiScale, ImVec2 pixels) {
  const double w = worldPixels(v, uiScale);
  v.cx -= pixels.x / w;
  v.cy -= pixels.y / w;
  clamp(v, size, uiScale);
}

void Map::upload() {
  std::vector<Shared::Ready> ready;
  {
    std::lock_guard<std::mutex> g(sh_->m);
    if (sh_->ready.empty()) return;
    size_t n = std::min(sh_->ready.size(), (size_t)kUploadsPerFrame);
    ready.assign(std::make_move_iterator(sh_->ready.begin()), std::make_move_iterator(sh_->ready.begin() + (long)n));
    sh_->ready.erase(sh_->ready.begin(), sh_->ready.begin() + (long)n);
    for (const Shared::Ready& r : ready) sh_->readyKeys.erase(r.key);
  }
  for (const Shared::Ready& r : ready) {
    Tile& t = tiles_[r.key];
    if (!t.tex) glGenTextures(1, &t.tex);
    glBindTexture(GL_TEXTURE_2D, t.tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, r.w, r.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, r.rgba.data());
    t.lastUsed = frame_;
  }
}

void Map::evict() {
  if (tiles_.size() <= kMaxTextures) return;
  std::vector<std::pair<uint64_t, uint64_t>> old;  // last used, key
  for (const auto& kv : tiles_)
    if (kv.second.lastUsed < frame_) old.emplace_back(kv.second.lastUsed, kv.first);
  std::sort(old.begin(), old.end());
  for (size_t i = 0; i < old.size() && tiles_.size() > kKeepTextures; i++) {
    auto it = tiles_.find(old[i].second);
    if (it->second.tex) glDeleteTextures(1, &it->second.tex);
    tiles_.erase(it);
  }
}

const Map::Tile* Map::tile(uint64_t key) {
  auto it = tiles_.find(key);
  if (it == tiles_.end()) return nullptr;
  it->second.lastUsed = frame_;
  return &it->second;
}

void Map::draw(ImDrawList* dl, ImVec2 p0, ImVec2 size, const View& v, Layer layer, float uiScale, ImFont* small) {
  frame_++;
  upload();
  const ImVec2 p1(p0.x + size.x, p0.y + size.y);
  dl->AddRectFilled(p0, p1, layer == SATELLITE || layer == HYBRID ? IM_COL32(8, 12, 20, 255) : IM_COL32(170, 211, 223, 255));

  const std::vector<int>& sources = kLayerSources[layer];
  const int maxZ = kSources[sources[0]].maxZ;
  const int z = std::max(0, std::min(maxZ, (int)floor(v.zoom + 0.5)));
  const int n = 1 << z;
  const double w = worldPixels(v, uiScale);
  const double left = v.cx - size.x / 2 / w, top = v.cy - size.y / 2 / w;
  const int tx0 = (int)floor(left * n), tx1 = (int)floor((left + size.x / w) * n);
  const int ty0 = std::max(0, (int)floor(top * n)), ty1 = std::min(n - 1, (int)floor((top + size.y / w) * n));
  // Whole pixels, so neighbouring tiles meet without a seam.
  auto sx = [&](int tx) { return floorf((float)(p0.x + size.x / 2 + ((double)tx / n - v.cx) * w) + 0.5f); };
  auto sy = [&](int ty) { return floorf((float)(p0.y + size.y / 2 + ((double)ty / n - v.cy) * w) + 0.5f); };

  struct Want {
    double d;
    uint64_t key;
  };
  std::vector<Want> want;
  const double midX = v.cx * n, midY = v.cy * n;
  for (size_t s = 0; s < sources.size(); s++) {
    const int src = sources[s];
    for (int ty = ty0; ty <= ty1; ty++) {
      for (int tx = tx0; tx <= tx1; tx++) {
        const int wx = ((tx % n) + n) % n;
        const uint64_t key = keyOf(src, z, wx, ty);
        const ImVec2 a(sx(tx), sy(ty)), b(sx(tx + 1), sy(ty + 1));
        if (const Tile* t = tile(key)) {
          dl->AddImage((ImTextureID)(intptr_t)t->tex, a, b);
          continue;
        }
        // The base layer stands in with a larger tile already here, stretched, until this one comes.
        if (s == 0) {
          for (int k = 1; k <= std::min(z, 10); k++) {
            const int px = wx >> k, py = ty >> k;
            if (const Tile* t = tile(keyOf(src, z - k, px, py))) {
              const float part = 1.f / (float)(1 << k);
              const ImVec2 uv0((float)(wx - (px << k)) * part, (float)(ty - (py << k)) * part);
              dl->AddImage((ImTextureID)(intptr_t)t->tex, a, b, uv0, ImVec2(uv0.x + part, uv0.y + part));
              break;
            }
          }
        }
        const double dx = tx + 0.5 - midX, dy = ty + 0.5 - midY;
        // The base layer first, then nearest the middle.
        want.push_back({dx * dx + dy * dy + (double)s * 1e6, key});
      }
    }
  }
  std::sort(want.begin(), want.end(), [](const Want& a, const Want& b) { return a.d < b.d; });
  {
    std::lock_guard<std::mutex> g(sh_->m);
    sh_->wanted.clear();
    for (const Want& x : want) sh_->wanted.push_back(x.key);
  }
  sh_->cv.notify_all();
  evict();

  // Whose map it is, as the tiles' terms ask.
  const char* credit = kAttribution[layer];
  const float fs = small->FontSize;
  const ImVec2 ts = small->CalcTextSizeA(fs, FLT_MAX, 0, credit);
  const float pad = 3 * uiScale;
  const ImVec2 c(p1.x - ts.x - pad * 2, p1.y - ts.y - pad);
  dl->AddRectFilled(ImVec2(c.x - pad, c.y - pad / 2), p1, IM_COL32(255, 255, 255, 170));
  dl->AddText(small, fs, c, IM_COL32(40, 40, 40, 255), credit);
}

int Map::pending() const {
  std::lock_guard<std::mutex> g(sh_->m);
  const double now = nowSeconds();
  int n = (int)sh_->busy.size() + (int)sh_->ready.size();
  for (uint64_t k : sh_->wanted) {
    auto f = sh_->failedUntil.find(k);
    if (f == sh_->failedUntil.end() || f->second <= now) n++;
  }
  return n;
}

std::string Map::lastError() const {
  std::lock_guard<std::mutex> g(sh_->m);
  return sh_->lastError;
}

bool Map::failing() const {
  std::lock_guard<std::mutex> g(sh_->m);
  return sh_->failing;
}

}  // namespace tilemap
