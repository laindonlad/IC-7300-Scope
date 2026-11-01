#pragma once
// Decoding what the receiver hears: CW, RTTY and NAVTEX to text, WEFAX to a picture. The decoders themselves
// are the Morse project's (third_party/morse), run here on a thread of their own, so the window can stall
// without the audio's timing suffering, which a chart in progress couldn't recover from.
//
// Everything decoded goes in one folder:
//   YYYY-MM-DD-cw.txt, -rtty.txt, -navtex.txt   text, one file a UTC day, tagged by signal and appended to
//   YYYY-MM-DD-HHMMSS.bmp                       a chart, 8-bit grey, named for when it started in UTC
#include <stddef.h>
#include <stdint.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

class Decoder {
 public:
  enum Mode { OFF, CW, RTTY, NAVTEX, WEFAX, MODE_COUNT };
  static const char* modeName(int mode);

  enum FaxState { FAX_IDLE, FAX_STARTING, FAX_PHASING, FAX_IMAGE };  // as WefaxDemod's
  static constexpr int kFaxWidth = 1809;

  struct FaxLine {
    uint32_t chart = 0;  // changes with each new chart
    std::vector<uint8_t> px;
  };
  struct Signal {
    float hz = 0;  // in the audio
    std::string label;
  };
  struct Status {
    int mode = OFF;
    std::string heading;           // what it's locked to, or that it's listening
    std::string live;              // CW: what's being heard, before the decoder settles on it and it goes in the log
    std::vector<Signal> signals;   // every signal it's found
    std::string file;              // the log, or the chart being written or last written
    std::string error;
    int faxState = FAX_IDLE;
    int faxIoc = 576, faxPhasing = 0;
    uint32_t faxLines = 0;
    float faxPpm = 0, faxCentre = 1900;
    bool faxMeasured = false;
  };

  // Gets up to n samples of 48 kHz mono, returning how many it got. Called from the decoder's own thread until
  // the Decoder is destroyed. An empty folder decodes without saving anything.
  using Source = std::function<size_t(float*, size_t)>;
  Decoder(const std::string& folder, Source source);
  ~Decoder();

  void setMode(int mode);
  void setMuted(bool muted);  // transmitting: it hears silence, so our own signal isn't logged as received
  void setDial(const std::string& dial);  // written to the log above what's heard there
  void faxStart();  // a chart from here, without waiting for its start tone
  void faxStop();
  void faxCentre(float hz);
  void faxSlant(float ppm);

  // What's new since the last call, and the status now.
  void take(std::string& text, std::vector<FaxLine>& fax, Status& status);

 private:
  struct Impl;
  std::unique_ptr<Impl> p_;
};
