// IC-7300 / IC9700 / IC-R8600 Receiver Console. See README.md.
#include <string.h>

#include <string>

#include "app.h"
#include "clock_sync.h"

#if defined(_WIN32) && defined(_MSC_VER)
#include <stdlib.h>
#include <windows.h>
int main(int, char**);
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) { return main(__argc, __argv); }
#endif

int main(int argc, char** argv) {
  AppOptions o;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
    if (a == "--sync-clock") return clocksync::runSyncClock();  // no window: see clock_sync.h
    if (a == "--demo") o.demo = true;
    else if (a == "--demo-radio") o.demo = true, o.demoRadio = atoi(next().c_str());
    else if (a == "--port") o.port = next();
    else if (a == "--no-rig") o.noRig = true;
    else if (a == "--screenshot") o.screenshot = next();
    else if (a == "--frames") o.screenshotFrames = atoi(next().c_str());
    else if (a == "--selftest") o.selfTest = next();
    else if (a == "--size") {
      std::string s = next();
      auto x = s.find('x');
      if (x != std::string::npos) o.width = atoi(s.c_str()), o.height = atoi(s.c_str() + x + 1);
    } else if (a == "--tab") o.tab = atoi(next().c_str());
    else if (a == "--settings") o.settings = true;
    else if (a == "--needle") o.needle = true;
    else if (a == "--find") o.find = next();
    else if (a == "--tune") o.tuneHz = atoll(next().c_str());
  }
  return runApp(o);
}
