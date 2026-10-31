#include "audio.h"

#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_ENCODING
#define MA_NO_DECODING
#define MA_NO_GENERATION
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE
#include "miniaudio.h"

#include <math.h>
#include <string.h>

#include <algorithm>
#include <chrono>

namespace {
// One writer, one reader, no locks: the writer is the audio callback, which must never wait.
class Ring {
 public:
  explicit Ring(size_t size) : buf_(size), mask_(size - 1) {}
  // A full ring drops what's new: the reader's stalled, and whatever it comes back to is old anyway.
  void push(const float* p, size_t n) {
    size_t w = w_.load(std::memory_order_relaxed), r = r_.load(std::memory_order_acquire);
    size_t space = buf_.size() - (w - r);
    if (n > space) n = space;
    for (size_t i = 0; i < n; i++) buf_[(w + i) & mask_] = p[i];
    w_.store(w + n, std::memory_order_release);
  }
  size_t pop(float* p, size_t n) {
    size_t r = r_.load(std::memory_order_relaxed), w = w_.load(std::memory_order_acquire);
    size_t have = w - r;
    if (n > have) n = have;
    for (size_t i = 0; i < n; i++) p[i] = buf_[(r + i) & mask_];
    r_.store(r + n, std::memory_order_release);
    return n;
  }
  size_t size() const { return w_.load(std::memory_order_acquire) - r_.load(std::memory_order_acquire); }
  void skip(size_t n) {
    size_t r = r_.load(std::memory_order_relaxed);
    r_.store(r + n, std::memory_order_release);
  }
  void clear() { r_.store(w_.load(std::memory_order_acquire), std::memory_order_release); }

 private:
  std::vector<float> buf_;
  size_t mask_;
  std::atomic<size_t> w_{0}, r_{0};
};

bool contains(std::string s, const char* what) {
  for (char& c : s)
    if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
  return s.find(what) != std::string::npos;
}
}  // namespace

bool AudioEngine::isRigCodec(const std::string& n) { return contains(n, "usb audio codec"); }

struct AudioEngine::Impl {
  ma_context ctx{};
  bool ctxOk = false;
  ma_device cap{}, play{};
  bool capOpen = false, playOpen = false;
  Ring forDsp{1u << 18};  // 5 seconds
  Ring forMonitor{1u << 15};
  std::atomic<float> peak{0.f};
};

struct AudioCallbacks {
  static void input(ma_device* d, void*, const void* in, ma_uint32 frames) {
    auto* e = (AudioEngine*)d->pUserData;
    deliver(e, (const float*)in, frames);
  }
  static void deliver(AudioEngine* e, const float* s, ma_uint32 frames) {
    float pk = 0;
    for (ma_uint32 i = 0; i < frames; i++) pk = std::max(pk, fabsf(s[i]));
    float cur = e->impl_->peak.load(std::memory_order_relaxed);
    if (pk > cur) e->impl_->peak.store(pk, std::memory_order_relaxed);
    e->impl_->forDsp.push(s, frames);
    if (e->impl_->playOpen) e->impl_->forMonitor.push(s, frames);
  }
  static void output(ma_device* d, void* out, const void*, ma_uint32 frames) {
    auto* e = (AudioEngine*)d->pUserData;
    float* o = (float*)out;
    Ring& r = e->impl_->forMonitor;
    // Two clocks never quite agree. Rather than let the delay creep up, skip ahead past 150ms.
    size_t over = AudioEngine::kRate * 150 / 1000;
    if (r.size() > over + frames) r.skip(r.size() - over);
    size_t got = r.pop(o, frames);
    for (size_t i = got; i < frames; i++) o[i] = 0;
    float g = e->monitorMuted_ ? 0.f : e->monitorGain_.load();
    for (size_t i = 0; i < frames; i++) o[i] *= g;
  }
  static void notify(const ma_device_notification* n) {
    if (n->type != ma_device_notification_type_stopped) return;
    auto* e = (AudioEngine*)n->pDevice->pUserData;
    e->lost_ = true;
  }
};

AudioEngine::AudioEngine() : impl_(new Impl) {
  impl_->ctxOk = ma_context_init(nullptr, 0, nullptr, &impl_->ctx) == MA_SUCCESS;
}

AudioEngine::~AudioEngine() {
  stopMonitor();
  stopCapture();
  if (impl_->ctxOk) ma_context_uninit(&impl_->ctx);
}

std::vector<std::string> AudioEngine::captureDevices() {
  std::vector<std::string> out;
  ma_device_info* info;
  ma_uint32 n;
  if (impl_->ctxOk && ma_context_get_devices(&impl_->ctx, nullptr, nullptr, &info, &n) == MA_SUCCESS)
    for (ma_uint32 i = 0; i < n; i++) out.push_back(info[i].name);
  return out;
}

std::vector<std::string> AudioEngine::playbackDevices() {
  std::vector<std::string> out;
  ma_device_info* info;
  ma_uint32 n;
  if (impl_->ctxOk && ma_context_get_devices(&impl_->ctx, &info, &n, nullptr, nullptr) == MA_SUCCESS)
    for (ma_uint32 i = 0; i < n; i++)
      if (!isRigCodec(info[i].name)) out.push_back(info[i].name);
  return out;
}

std::string AudioEngine::guessRigInput() {
  for (const std::string& n : captureDevices())
    if (isRigCodec(n)) return n;
  return "";
}

bool AudioEngine::startCapture(const std::string& name, std::string& err) {
  stopCapture();
  if (!impl_->ctxOk) {
    err = "The audio system didn't start";
    return false;
  }
  ma_device_info* info;
  ma_uint32 n;
  if (ma_context_get_devices(&impl_->ctx, nullptr, nullptr, &info, &n) != MA_SUCCESS) {
    err = "Can't list the sound devices";
    return false;
  }
  const ma_device_id* id = nullptr;
  for (ma_uint32 i = 0; i < n; i++)
    if (name == info[i].name) id = &info[i].id;
  if (!id) {
    err = "\"" + name + "\" isn't there (radio off or unplugged?)";
    return false;
  }
  ma_device_config c = ma_device_config_init(ma_device_type_capture);
  c.capture.pDeviceID = id;
  c.capture.format = ma_format_f32;
  c.capture.channels = 1;  // the rig's two channels are the same receiver: mixed down, not doubled
  c.capture.shareMode = ma_share_mode_shared;  // so other programs can listen too
  c.sampleRate = kRate;
  c.dataCallback = AudioCallbacks::input;
  c.notificationCallback = AudioCallbacks::notify;
  c.pUserData = this;
  if (ma_device_init(&impl_->ctx, &c, &impl_->cap) != MA_SUCCESS) {
    err = "Can't open \"" + name + "\"";
    return false;
  }
  lost_ = false;
  impl_->forDsp.clear();
  if (ma_device_start(&impl_->cap) != MA_SUCCESS) {
    ma_device_uninit(&impl_->cap);
    err = "Can't start \"" + name + "\"";
    return false;
  }
  impl_->capOpen = true;
  captureName_ = name;
  return true;
}

void AudioEngine::startGenerated(std::function<void(float*, int)> gen) {
  stopCapture();
  lost_ = false;
  captureName_ = "Simulated audio";
  genRun_ = true;
  genThread_ = std::thread([this, gen] {
    using namespace std::chrono;
    const int block = 1024;
    std::vector<float> b(block);
    auto next = steady_clock::now();
    while (genRun_) {
      gen(b.data(), block);
      AudioCallbacks::deliver(this, b.data(), block);
      next += microseconds(1000000LL * block / kRate);
      std::this_thread::sleep_until(next);
    }
  });
}

void AudioEngine::stopCapture() {
  if (genThread_.joinable()) {
    genRun_ = false;
    genThread_.join();
  }
  if (impl_->capOpen) {
    ma_device_uninit(&impl_->cap);
    impl_->capOpen = false;
  }
  captureName_.clear();
}

bool AudioEngine::capturing() const { return impl_->capOpen || genThread_.joinable(); }

bool AudioEngine::startMonitor(const std::string& name, std::string& err) {
  stopMonitor();
  if (!impl_->ctxOk) {
    err = "The audio system didn't start";
    return false;
  }
  ma_device_info* info;
  ma_uint32 n;
  if (ma_context_get_devices(&impl_->ctx, &info, &n, nullptr, nullptr) != MA_SUCCESS) {
    err = "Can't list the sound devices";
    return false;
  }
  const ma_device_id* id = nullptr;
  for (ma_uint32 i = 0; i < n; i++) {
    bool pick = name.empty() ? info[i].isDefault != 0 : name == info[i].name;
    if (!pick) continue;
    if (isRigCodec(info[i].name)) {
      // Playing to it feeds the rig's modulation input: it goes on the air if the rig transmits.
      err = "The rig's USB sound card is the system default output, and the monitor won't feed it. Choose your speakers in Settings.";
      return false;
    }
    id = &info[i].id;
  }
  if (!id) {
    err = name.empty() ? "No default output device" : "\"" + name + "\" isn't there";
    return false;
  }
  ma_device_config c = ma_device_config_init(ma_device_type_playback);
  c.playback.pDeviceID = id;
  c.playback.format = ma_format_f32;
  c.playback.channels = 1;
  c.sampleRate = kRate;
  c.performanceProfile = ma_performance_profile_low_latency;
  c.dataCallback = AudioCallbacks::output;
  c.pUserData = this;
  if (ma_device_init(&impl_->ctx, &c, &impl_->play) != MA_SUCCESS) {
    err = "Can't open the output device";
    return false;
  }
  impl_->forMonitor.clear();
  impl_->playOpen = true;
  if (ma_device_start(&impl_->play) != MA_SUCCESS) {
    impl_->playOpen = false;
    ma_device_uninit(&impl_->play);
    err = "Can't start the output device";
    return false;
  }
  return true;
}

void AudioEngine::stopMonitor() {
  if (!impl_->playOpen) return;
  impl_->playOpen = false;
  ma_device_uninit(&impl_->play);
}

bool AudioEngine::monitoring() const { return impl_->playOpen; }

size_t AudioEngine::read(float* out, size_t max) { return impl_->forDsp.pop(out, max); }

float AudioEngine::takePeak() { return impl_->peak.exchange(0.f); }
