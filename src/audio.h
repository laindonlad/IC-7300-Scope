#pragma once
// Sound through miniaudio (WASAPI, CoreAudio, or ALSA/PulseAudio): the rig's USB audio in, mono at 48kHz,
// for the waterfall and recorder, and optionally out to the PC's speakers so you can listen.
//
// The rig's USB sound card is shared, so other programs can use it at the same time. Its playback side
// is the rig's modulation input, so the monitor refuses to play to it: see isRigCodec.
#include <stdint.h>

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

class AudioEngine {
 public:
  static constexpr int kRate = 48000;
  AudioEngine();
  ~AudioEngine();

  std::vector<std::string> captureDevices();
  std::vector<std::string> playbackDevices();  // never lists the rig's own codec
  // The device that's probably the rig's receive audio, or "".
  std::string guessRigInput();
  static bool isRigCodec(const std::string& deviceName);

  bool startCapture(const std::string& name, std::string& err);
  // Instead of a device: gen(out, n) is called in real time from a thread of its own.
  void startGenerated(std::function<void(float*, int)> gen);
  void stopCapture();
  bool capturing() const;
  // Unplugged, or the rig switched off. Reopen with startCapture.
  bool lost() const { return lost_; }
  const std::string& captureName() const { return captureName_; }

  // name "" is the system default. Refused if that turns out to be the rig's codec.
  bool startMonitor(const std::string& name, std::string& err);
  void stopMonitor();
  bool monitoring() const;
  void setMonitorGain(float g) { monitorGain_ = g; }
  void setMonitorMuted(bool m) { monitorMuted_ = m; }

  // New samples for the waterfall, since the last read. Only one caller.
  size_t read(float* out, size_t max);
  // The loudest sample since the last call, 0-1.
  float takePeak();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  std::atomic<bool> lost_{false};
  std::atomic<float> monitorGain_{1.f};
  std::atomic<bool> monitorMuted_{false};
  std::string captureName_;
  std::thread genThread_;
  std::atomic<bool> genRun_{false};
  friend struct AudioCallbacks;
};
