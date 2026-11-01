#pragma once
#include <string>

struct AppOptions {
  bool demo = false;      // a simulated rig and audio; touches no hardware and no saved settings
  bool noRig = false;     // audio only: leaves the serial port alone
  std::string port;       // overrides the saved port for this run
  std::string screenshot; // saves the window as a BMP after screenshotFrames, then quits
  int screenshotFrames = 240;
  std::string selfTest;   // with demo: clicks and types through the window, writes results here, quits
  int width = 0, height = 0;
  int tab = 0;            // which right-hand tab starts open, for screenshots
  bool settings = false;  // starts with the Settings dialog open, for screenshots
  long long tuneHz = 0;   // tunes here soon after starting, for screenshots
  std::string find;       // starts with Find a station showing this search, for screenshots
};

int runApp(const AppOptions& opt);
