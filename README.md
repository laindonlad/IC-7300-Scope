# IC-7300 Console

A desktop front end for the Icom IC-7300: full CAT control over the rig's USB port, an audio waterfall
from its USB sound card, and the rig's own wideband scope as a second waterfall.

    build\ic7300console.exe          run it (finds COM port and sound card itself)
    build\ic7300console.exe --demo   a simulated radio: touches no hardware and no saved settings

Only one program can own the rig's COM port, so close Station or MorseRx first. The sound card is shared.

## Setting up the rig

MENU > SET > Connectors > CI-V:

| Setting | Value |
|---|---|
| CI-V USB Port | Unlink from [REMOTE] (the scope stream needs this) |
| CI-V USB Baud Rate | 115200 |
| CI-V Address | 94h |
| CI-V USB Echo Back | OFF (ON also works) |

Also: raise USB AF Output Level until the level bar in the AUDIO toolbar sits below red, and leave
USB SEND and USB Keying OFF, since a serial line left high would otherwise key the rig.

## Using it

**Punch in a frequency**: just start typing, anywhere. Enter goes there, Esc cancels. Or use the keypad tab.

| Typed | Means |
|---|---|
| `14.074` | 14.074 MHz |
| `14074` | 14074 kHz |
| `14074.5` | 14074.5 kHz |
| `7` | 7 MHz (75 or below is MHz) |
| `198` | 198 kHz |
| `198k`, `14.074m` | says so outright |
| `14.074.123` | as the rig displays it |
| `+5`, `-1.5` | kHz from the dial |

**Keys**

| Key | Action |
|---|---|
| Left, Right | Tune by the step (Shift: ten times) |
| Up, Down | Change the step |
| Page Up, Page Down | Next or previous band |
| F1 to F8 | LSB, USB, CW, CW-R, AM, FM, RTTY, RTTY-R |
| F9, F10 | Data mode, next filter |
| F11, F12 | Split, swap VFOs |
| Ctrl+Space | PTT on or off, once armed |

**Mouse**

- Wheel on a digit tunes that digit; click its top half for up, bottom half for down.
- Wheel on a waterfall tunes by the step. Wheel on a slider moves it.
- Click the band scope to tune there. Right-click it for split and memories.
- Click the audio waterfall to move that signal to the pitch: 1500 Hz on SSB, your CW pitch on CW. In data
  modes the dial stays put, so a digital program keeps its place.
- A band button returns to where you last were on that band. Right-click it for CW, FT8, FT4, WSPR and SSB.
- Right-click SPLIT for a quick offset.

**Also here**: RX and TX levels, filter width and shape, Twin PBT, NB, NR, notch, RIT/XIT, meters, ATU, A/B,
memories, a log with ADIF export, a CW keyer with macros, voice memories, WAV recording, listening through
the PC's speakers, and the band plan as a coloured strip under the scope (blue CW, purple digital, green
SSB, orange FM). It's a simplified guide, not a statement of what your licence allows.

Settings, memories, the log and recordings are plain files in `%APPDATA%\IC7300Console` (Rig > Open data folder).

## Transmit safety

Enforced in `src/rig.cpp`, so no part of the window can get round it:

- Nothing transmits until **ARM TX** is pressed, and it's off at every start. That covers PTT, TUNE, CW and voice
  memories. Disarming also releases PTT.
- PTT lets go by itself after a time limit (180 s by default, in Settings).
- Un-key is resent until the rig itself reports it has stopped, including across a lost link. Closing the
  program un-keys too.
- Listen won't play to the rig's USB sound card, as that's its modulation input.

A hard crash or a pulled USB cable can't send an un-key. The rig's own Time-Out Timer setting is the backstop,
so keep it on.

## Building

Needs CMake and a C++17 compiler. Everything else is in `third_party/` at pinned versions: Dear ImGui 1.91.9b,
GLFW 3.4, miniaudio 0.11.22.

    cmake -S . -B build -G Ninja
    cmake --build build
    build\test_core

Linux first needs `build-essential cmake ninja-build libgl1-mesa-dev` and GLFW's X11 and Wayland packages
(`xorg-dev libwayland-dev libxkbcommon-dev`), and your user in the `dialout` group. macOS needs Xcode's
command line tools and CMake.

The Linux and macOS code is written (POSIX serial, GLFW, miniaudio, per-user data folders) but **has never
been compiled or run**. Expect some small fixes on first build.

## How it's tested

| Test | What it covers |
|---|---|
| `test_core` | CI-V bytes, frequency entry, FFT, log files, and the rig thread against a simulated 7300, including refused commands, echo, stale readings, lost links and every transmit guard |
| `ic7300console --demo --selftest out.txt` | Real clicks and keystrokes through the window, against the simulated 7300 |
| `ic7300probe COM11` | Reads everything from the real radio and listens to the scope. Never keys. Add `--write-same` to also write the values it already has |

The simulated rig shares this code's reading of the protocol, so only `ic7300probe` shows the protocol is right.

## Things the real radio taught us

- It sends about 4 scope sweeps a second at Fast (some runs measured 8), whatever the polling load. That's the
  rig's limit, so the band waterfall's rows are drawn tall to compensate.
- It can ignore a command sent before it has answered the last one. `Rig` always waits for the reply.
- In Center mode, the sweep may be centred on the filter rather than the dial (a rig setting), so the frame's
  own edges are used for tuning, never the dial.

## Third-party code

Bundled in `third_party/`, each under its own licence:

| Component | Licence |
|---|---|
| [Dear ImGui](https://github.com/ocornut/imgui) | MIT |
| [GLFW](https://www.glfw.org/) | zlib/libpng |
| [miniaudio](https://miniaud.io/) | Public domain or MIT-0 |
| Roboto font, by Christian Robertson | Apache 2.0 |
| Cousine font, by Steve Matteson | SIL Open Font License 1.1 |

The CI-V command bytes were checked against [Hamlib](https://hamlib.github.io/)'s IC-7300 backend.

## Layout

| Path | What |
|---|---|
| `src/civ.h` | The CI-V protocol: frames, commands, meter calibration. No OS, no threads |
| `src/rig.*` | The rig thread: command queue, polling, state, transmit safety |
| `src/serial.cpp` | Serial ports: Win32 and POSIX |
| `src/sim_rig.*` | The simulated IC-7300 |
| `src/audio.*` | Sound in and out, through miniaudio |
| `src/waterfall.*`, `src/dsp.h` | The waterfall texture and the FFT |
| `src/tuning.h`, `src/bands.h` | Frequency entry, audio-to-RF mapping, the band table |
| `src/store.*` | Settings, memories, log, ADIF, WAV |
| `src/app.cpp` | The window, and the self-test script |
| `tools/probe.cpp` | The read-only check of a real radio |
