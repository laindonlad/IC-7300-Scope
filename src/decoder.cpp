#include "decoder.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

// A band can have more signals on it than the Morse board has room for. Only this file includes the engine.
#define DIGI_MAX_CH 8
#include "digi/bmp.h"
#include "digi/streams.h"
#include "digi/wefax.h"
#include "store.h"

static_assert(WefaxDemod::MAX_W == Decoder::kFaxWidth, "the window sizes its picture from this");

namespace {
const int kInRate = 48000, kOutRate = 16000;  // the sound card's, and the decoders'

std::string utcStamp(const char* fmt) {
  time_t t = time(nullptr);
  struct tm u;
#ifdef _WIN32
  gmtime_s(&u, &t);
#else
  gmtime_r(&t, &u);
#endif
  char b[64];
  strftime(b, sizeof b, fmt, &u);
  return b;
}

void clockNow(char* b, size_t n) { snprintf(b, n, "%s ", utcStamp("%H:%MZ").c_str()); }

// 48 kHz to 16 kHz, with a filter that keeps what's above 8 kHz from folding down onto the signals.
class Downsampler {
 public:
  Downsampler() {
    const double cutoff = 7000.0 / kInRate;
    double sum = 0;
    for (int i = 0; i < kTaps; i++) {
      double x = i - (kTaps - 1) / 2.0;
      double sinc = x == 0 ? 2 * cutoff : sin(2 * M_PI * cutoff * x) / (M_PI * x);
      double hamming = 0.54 - 0.46 * cos(2 * M_PI * i / (kTaps - 1));
      taps_[i] = (float)(sinc * hamming);
      sum += taps_[i];
    }
    for (float& t : taps_) t = (float)(t / sum);
  }
  void run(const float* in, size_t n, std::vector<int16_t>& out) {
    const int factor = kInRate / kOutRate;
    for (size_t k = 0; k < n; k++) {
      hist_[pos_] = in[k];
      pos_ = (pos_ + 1) % kTaps;
      if (++count_ < factor) continue;
      count_ = 0;
      float y = 0;
      for (int i = 0, at = pos_; i < kTaps; i++, at = at + 1 == kTaps ? 0 : at + 1) y += taps_[i] * hist_[at];
      y *= 32767.f;
      out.push_back((int16_t)(y > 32767.f ? 32767.f : y < -32767.f ? -32767.f : y));
    }
  }

 private:
  static const int kTaps = 63;
  float taps_[kTaps], hist_[kTaps] = {};
  int pos_ = 0, count_ = 0;
};

// A chart as an 8-bit grey BMP, its header's height put right every 32 lines, so one cut short is still a picture.
struct ChartFile {
  FILE* f = nullptr;
  int width = 0;
  uint32_t lines = 0;
  bool begin(const std::string& path, int w) {
    end();
    f = fopen(path.c_str(), "wb");
    if (!f) return false;
    width = w;
    lines = 0;
    uint8_t h[Bmp::HEADER];
    Bmp::header(h, width, 0);
    fwrite(h, 1, sizeof h, f);
    return true;
  }
  void fixHeader() {
    uint8_t h[Bmp::HEADER];
    Bmp::header(h, width, (int)lines);
    fseek(f, 0, SEEK_SET);
    fwrite(h, 1, sizeof h, f);
    fseek(f, 0, SEEK_END);
    fflush(f);
  }
  void line(const uint8_t* px) {
    if (!f) return;
    static const uint8_t pad[4] = {0, 0, 0, 0};
    fwrite(px, 1, (size_t)width, f);
    fwrite(pad, 1, (size_t)(Bmp::stride(width) - width), f);
    if (++lines % 32 == 0) fixHeader();
  }
  void end() {
    if (!f) return;
    fixHeader();
    fclose(f);
    f = nullptr;
  }
};
}  // namespace

const char* Decoder::modeName(int mode) {
  static const char* const kNames[MODE_COUNT] = {"Off", "CW", "RTTY", "NAVTEX", "WEFAX"};
  return mode >= 0 && mode < MODE_COUNT ? kNames[mode] : "?";
}

struct Decoder::Impl {
  // shared with the window
  std::mutex m;
  struct Cmd {
    enum Kind { MODE, FAX_START, FAX_STOP, FAX_CENTRE, FAX_SLANT } kind;
    float value;
  };
  std::vector<Cmd> cmds;
  std::string dial;
  std::string textOut;
  std::vector<FaxLine> faxOut;
  Status st;
  std::atomic<bool> quit{false}, muted{false};

  // the thread's own
  std::string folder;
  Source source;
  std::thread thread;
  Downsampler down;
  std::unique_ptr<Digi::Engine> engine;
  std::unique_ptr<Digi::Streams> streams;
  std::unique_ptr<WefaxDemod> wefax;
  int mode = OFF;
  uint64_t fed = 0;  // 16 kHz samples, which are the streams' clock
  int sinceUpdate = 0;
  float centre = WefaxDemod::CENTRE, ppm = 0;
  FILE* log = nullptr;
  std::string logPath, dialWritten;
  bool needHeading = true, atLineStart = true;
  ChartFile chart;
  uint32_t chartNum = 0;
  uint8_t faxWas = WefaxDemod::IDLE;
  uint8_t line[WefaxDemod::MAX_W];

  void setError(const std::string& e) {
    std::lock_guard<std::mutex> g(m);
    st.error = e;
  }

  // ---- text ----

  void put(const char* s) {
    if (!*s) return;
    if (log) {
      fputs(s, log);
      fflush(log);
    }
    atLineStart = s[strlen(s) - 1] == '\n';
    std::lock_guard<std::mutex> g(m);
    for (; *s; s++)
      if (*s != '\r') textOut += *s;
  }

  // Today's file for this mode, whenever the date in its name changes.
  void openLog() {
    if (folder.empty()) return;
    const char* name = mode == CW ? "cw" : mode == RTTY ? "rtty" : "navtex";
    std::string want = folder + utcStamp("%Y-%m-%d-") + name + ".txt";
    if (want == logPath) return;
    closeLog();
    logPath = want;
    needHeading = true;
    store::makeDirs(folder);
    log = fopen(want.c_str(), "ab");
    setError(log ? "" : "Can't write " + want);
    std::lock_guard<std::mutex> g(m);
    st.file = want;
  }

  void closeLog() {
    if (log) fclose(log);
    log = nullptr;
    logPath.clear();
  }

  // Everything the streams say, whichever signal it's from.
  void text(const char* s) {
    openLog();
    std::string now;
    {
      std::lock_guard<std::mutex> g(m);
      now = dial;
    }
    if (needHeading || now != dialWritten) {
      needHeading = false;
      dialWritten = now;
      if (!atLineStart) put("\r\n");
      put(("--- " + utcStamp("%Y-%m-%d %H:%MZ") + (now.empty() ? "" : "  " + now) + " ---\r\n").c_str());
    }
    put(s);
  }

  static void streamOut(void* ctx, const char* s) { ((Impl*)ctx)->text(s); }

  uint32_t ms() const { return (uint32_t)(fed / 16); }
  bool textMode() const { return mode == CW || mode == RTTY || mode == NAVTEX; }

  // ---- charts ----

  void takeFax() {
    uint32_t c;
    int n;
    while ((n = wefax->takeLine(line, c)) > 0) {
      if (c != chartNum) {
        chartNum = c;
        chart.end();
        if (!folder.empty()) {
          store::makeDirs(folder);
          std::string path = folder + utcStamp("%Y-%m-%d-%H%M%S") + ".bmp";
          bool ok = chart.begin(path, n);
          setError(ok ? "" : "Can't write " + path);
          std::lock_guard<std::mutex> g(m);
          st.file = path;
        }
      }
      chart.line(line);
      FaxLine l;
      l.chart = c;
      l.px.assign(line, line + n);
      std::lock_guard<std::mutex> g(m);
      faxOut.push_back(std::move(l));
    }
    uint8_t s = wefax->state();
    if (faxWas == WefaxDemod::IMAGE && s != WefaxDemod::IMAGE) chart.end();
    faxWas = s;
  }

  // ---- changing what it does ----

  void setModeNow(int to) {
    if (to == mode) return;
    if (textMode()) {
      streams->finish(*engine, ms());
      closeLog();
    }
    if (mode == WEFAX) {
      wefax->stopNow();
      takeFax();
      chart.end();
    }
    mode = to;
    needHeading = true;
    // The engine's channels go with a change of family, and WEFAX is the family that has none.
    engine->setFamily(mode == CW ? Digi::CW : mode == RTTY ? Digi::RTTY : mode == NAVTEX ? Digi::NAVTEX : Digi::WEFAX);
    if (mode == WEFAX) {
      wefax->reset();
      wefax->tune(centre);
      wefax->setSlant(ppm);
      faxWas = WefaxDemod::IDLE;
    }
    {
      std::lock_guard<std::mutex> g(m);
      textOut.clear();
      faxOut.clear();
      st.file.clear();
      st.error.clear();
    }
    update();
  }

  void apply(const Cmd& c) {
    switch (c.kind) {
      case Cmd::MODE: setModeNow((int)c.value); break;
      case Cmd::FAX_START:
        if (mode == WEFAX) wefax->startNow();
        break;
      case Cmd::FAX_STOP:
        if (mode == WEFAX) wefax->stopNow();
        break;
      case Cmd::FAX_CENTRE:
        centre = c.value;
        if (mode == WEFAX) wefax->tune(centre);
        break;
      case Cmd::FAX_SLANT:
        ppm = c.value;
        if (mode == WEFAX) wefax->setSlant(ppm);
        break;
    }
  }

  // ---- the status, ten times a second ----

  void update() {
    std::string heading, live;
    std::vector<Signal> signals;
    char b[96], lb[24];
    if (textMode()) {
      streams->update(*engine, ms());
      const Digi::Option* sig;
      int n = engine->signals(sig);
      for (int i = 0; i < n; i++) {
        if (sig[i].fam == Digi::AUTO) continue;
        Signal s;
        s.hz = sig[i].hz;
        Digi::label(sig[i], lb, sizeof lb);
        s.label = lb;
        signals.push_back(s);
      }
      if (engine->state() == Digi::Engine::LOCKED) {
        const Digi::Option& o = engine->locked();
        Digi::label(o, lb, sizeof lb);
        int at = snprintf(b, sizeof b, "%s at %.0f Hz, %.0f dB", lb, o.hz, o.snr);
        float wpm = engine->lockedWpm();
        if (wpm > 0) snprintf(b + at, sizeof b - (size_t)at, ", %.0f WPM", wpm);
        heading = b;
      } else {
        heading = std::string("Listening for ") + modeName(mode);
      }
      if (mode == CW && engine->state() == Digi::Engine::LOCKED) {
        const char* t = engine->text().text;
        size_t len = strlen(t), from = len > 48 ? len - 48 : 0;
        while (from < len && ((unsigned char)t[from] & 0xC0) == 0x80) from++;  // not in the middle of a character
        live.assign(t + from, len - from);
        for (char& c : live)
          if (c == '\n' || c == '\r') c = ' ';
      }
    }
    std::lock_guard<std::mutex> g(m);
    st.mode = mode;
    st.heading = std::move(heading);
    st.live = std::move(live);
    st.signals = std::move(signals);
    if (mode == WEFAX) {
      st.faxState = wefax->state();
      st.faxIoc = wefax->ioc();
      st.faxPhasing = wefax->phasingLines();
      st.faxLines = wefax->state() == WefaxDemod::IMAGE ? wefax->lines() : 0;
      st.faxPpm = wefax->ppm();
      st.faxCentre = wefax->centre();
      st.faxMeasured = wefax->slantMeasured();
    } else {
      st.faxState = FAX_IDLE;
    }
  }

  void run() {
    std::vector<float> in(4096);
    std::vector<int16_t> pcm;
    while (!quit) {
      std::vector<Cmd> todo;
      {
        std::lock_guard<std::mutex> g(m);
        todo.swap(cmds);
      }
      for (const Cmd& c : todo) apply(c);
      size_t n = source(in.data(), in.size());
      if (!n) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        continue;
      }
      if (mode == OFF) continue;  // keeps the queue empty, so a mode starts with what's on air now
      // Silence rather than nothing, so a chart's lines stay in time.
      if (muted) memset(in.data(), 0, n * sizeof(float));
      pcm.clear();
      down.run(in.data(), n, pcm);
      if (pcm.empty()) continue;
      if (mode == WEFAX) {
        wefax->feed(pcm.data(), (int)pcm.size());
        takeFax();
      } else {
        engine->feed(pcm.data(), (int)pcm.size());
      }
      fed += pcm.size();
      sinceUpdate += (int)pcm.size();
      if (sinceUpdate >= kOutRate / 10) {
        sinceUpdate = 0;
        update();
      }
    }
    setModeNow(OFF);  // writes out what's still held back
  }
};

Decoder::Decoder(const std::string& folder, Source source) : p_(new Impl) {
  p_->folder = folder;
  p_->source = std::move(source);
  p_->engine.reset(new Digi::Engine());
  p_->streams.reset(new Digi::Streams(Impl::streamOut, p_.get(), clockNow, false));
  p_->wefax.reset(new WefaxDemod());
  p_->engine->setFamily(Digi::WEFAX);
  p_->thread = std::thread([this] { p_->run(); });
}

Decoder::~Decoder() {
  p_->quit = true;
  p_->thread.join();
}

void Decoder::setMode(int mode) {
  std::lock_guard<std::mutex> g(p_->m);
  p_->cmds.push_back({Impl::Cmd::MODE, (float)mode});
}
void Decoder::setMuted(bool muted) { p_->muted = muted; }
void Decoder::setDial(const std::string& dial) {
  std::lock_guard<std::mutex> g(p_->m);
  p_->dial = dial;
}
void Decoder::faxStart() {
  std::lock_guard<std::mutex> g(p_->m);
  p_->cmds.push_back({Impl::Cmd::FAX_START, 0});
}
void Decoder::faxStop() {
  std::lock_guard<std::mutex> g(p_->m);
  p_->cmds.push_back({Impl::Cmd::FAX_STOP, 0});
}
void Decoder::faxCentre(float hz) {
  std::lock_guard<std::mutex> g(p_->m);
  p_->cmds.push_back({Impl::Cmd::FAX_CENTRE, hz});
}
void Decoder::faxSlant(float ppm) {
  std::lock_guard<std::mutex> g(p_->m);
  p_->cmds.push_back({Impl::Cmd::FAX_SLANT, ppm});
}

void Decoder::take(std::string& text, std::vector<FaxLine>& fax, Status& status) {
  std::lock_guard<std::mutex> g(p_->m);
  text.swap(p_->textOut);
  p_->textOut.clear();
  fax.swap(p_->faxOut);
  p_->faxOut.clear();
  status = p_->st;
}
