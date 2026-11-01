# IC-7300 / IC9700 / IC-R8600 Receiver Console

A desktop front end for the Icom IC-7300 and IC-9700 transceivers and the IC-R8600 receiver. Plug the radio into your PC with one USB cable and the program works out which it is. You get:

- **Full CAT control**: frequency, mode, filters, levels, noise controls, RIT/XIT, split, meters and the tuner (on the radios that have them), and the IC-R8600's antennas, WFM and digital modes
- **An audio waterfall**, from the radio's built-in USB sound card
- **A band waterfall**, from the radio's own wideband scope
- **Station tools**: memories, a log with ADIF export, a CW keyer with macros, voice memories and WAV recording
- **A demo mode** with a simulated radio, so you can try it all with no hardware

It's written in C++17 with Dear ImGui, GLFW and miniaudio. It builds to a single executable with nothing to install.

## Supported radios

| Radio | Type | Coverage | Default CI-V address | Run on a real radio? |
|---|---|---|---|---|
| Icom IC-7300 | HF/6 m/4 m transceiver | 30 kHz to 74.8 MHz | 94h | Yes |
| Icom IC-9700 | VHF/UHF/23 cm transceiver | 144-148, 430-450 and 1240-1300 MHz, on the US model | A2h | **No**, simulated radio only |
| Icom IC-R8600 | Wideband receiver | 10 kHz to 3 GHz | 96h | **No**, simulated radio only |

The IC-7300 MK2 hasn't been tested. See [What changes on an IC-9700](#what-changes-on-an-ic-9700), [What changes on an IC-R8600](#what-changes-on-an-ic-r8600) and [Tested on](#tested-on).

## Contents

- [Supported radios](#supported-radios)
- [Download](#download)
- [Tested on](#tested-on)
- [Building](#building)
- [Setting up the radio](#setting-up-the-radio)
- [Running it](#running-it)
- [Using it](#using-it)
- [Transmit safety](#transmit-safety)
- [Your data](#your-data)
- [How it works](#how-it-works)
- [Testing](#testing)
- [Troubleshooting](#troubleshooting)
- [Third-party code](#third-party-code)

## Download

You don't need to compile anything. Ready-to-run programs are in the [`bin`](bin) folder:

| System | Folder | Needs |
|---|---|---|
| Windows 10 or 11, x64 | [`bin/windows-x64`](bin/windows-x64) | Nothing else |
| Linux, x64 | [`bin/linux-x64`](bin/linux-x64) | glibc 2.34 or newer (Ubuntu 22.04, Debian 12, Fedora 35 or later) and a graphics driver with OpenGL |
| macOS 11 or newer, Apple Silicon or Intel | [`bin/macos`](bin/macos) | Nothing else. If this folder isn't there, the macOS build hasn't been published yet, so [build it from source](#linux-and-macos) |

**Reminder for band plans and station lists:** the standard ones are built in, so you don't need to copy anything to use them. To add or replace one, copy its `.json` file into the `bandplans` or `stations` folder inside your data folder, then press **Reload** in **Rig > Settings**. The data folder is `%APPDATA%\IC7300Console` on Windows, `~/.config/ic7300console` on Linux and `~/Library/Application Support/IC7300Console` on macOS. [More about these files](#band-plans-and-station-lists).

To get them, [download the project as a ZIP](https://github.com/laindonlad/IC-7300-Scope/archive/refs/heads/main.zip) and unzip it, or clone it with Git. Each folder holds the same three programs, and only `ic7300console` is needed to use the radio:

| File | What it is |
|---|---|
| `ic7300console` | The application |
| `ic7300probe` | A console tool that checks the protocol against a real radio ([details](#testing)) |
| `test_core` | The core test suite |

Start with `--demo` to try it with a simulated radio. When you're ready to use a real radio, [set it up](#setting-up-the-radio), then run it without `--demo`. It finds the serial port and sound card itself.

The programs aren't code-signed, so your system may warn you the first time. Check how far each system has been tested in [Tested on](#tested-on).

### Windows

In PowerShell, from the unzipped folder:

```powershell
.\bin\windows-x64\ic7300console.exe --demo
```

To use a real radio, double-click `ic7300console.exe`. If SmartScreen stops it, choose **More info**, then **Run anyway**.

### Linux

```sh
cd bin/linux-x64
chmod +x ic7300console ic7300probe test_core
./ic7300console --demo
```

To open the radio's serial port, your user must be in the `dialout` group:

```sh
sudo usermod -aG dialout "$USER"     # then log out and back in
```

### macOS

```sh
cd bin/macos
chmod +x ic7300console ic7300probe test_core
xattr -d com.apple.quarantine ic7300console ic7300probe test_core   # stops Gatekeeper blocking a downloaded file
./ic7300console --demo
```

The `xattr` command reports an error for a file that isn't marked as downloaded, which is harmless.

### Band plans and station lists

**There's nothing to install.** The band plans and the station list are built into `ic7300console`, so you only need that one program. You don't need the `bandplans` and `stations` folders from the download, and you don't need to copy them anywhere.

The first time it runs, the program writes editable copies into two folders inside [your data folder](#your-data), creating them if they aren't there:

| System | Band plans | Station lists |
|---|---|---|
| Windows | `%APPDATA%\IC7300Console\bandplans` | `%APPDATA%\IC7300Console\stations` |
| Linux | `~/.config/ic7300console/bandplans` | `~/.config/ic7300console/stations` |
| macOS | `~/Library/Application Support/IC7300Console/bandplans` | `~/Library/Application Support/IC7300Console/stations` |

You only need these folders to change a list or add your own. The buttons in **Rig > Settings** open them:

- **Files beside the program are ignored.** A `.json` file only counts if it's directly in one of the folders above, not beside `ic7300console` or in a subfolder.
- **To add one**: put a `.json` file in the right folder under a new name and press **Reload** in Settings, or restart. A band plan then appears under **Country or region**, and a station list is added to the ones already showing. Each folder's `README.txt` explains the format.
- **To change one**: edit the copy in the folder and press **Reload**. A copy with the same name as a built-in file is used instead of it. If it has a mistake, Settings names the file and the line, and the built-in version is used until it's fixed.
- **To undo your changes**: use **Reset this plan** or **Reset station list** in Settings, which keeps your file as `<name>.json.bak`. You can also delete the file, and the program writes a fresh copy the next time it starts.

**After installing a new version**, the program doesn't overwrite the copies in these folders, so you keep your edits but also keep the old data. To get the new data, use the reset buttons, or delete the files before starting it. Settings tells you when a newer copy of the chosen band plan is available, but it doesn't do this for the station list.

## Tested on

| | |
|---|---|
| **Operating system** | Windows 11 Pro (build 26300), x64 |
| **Compilers** | MinGW-w64 GCC 16.2 (WinLibs UCRT) with CMake 4.4 and Ninja 1.13; Visual Studio 2026 (MSVC, x64) |
| **Radio** | Icom IC-7300 over USB, with the Silicon Labs CP210x driver 6.6.1.0 |

Other setups:

- **Other versions of Windows**: not tested.
- **Linux**: the binaries in `bin/linux-x64` were built on Ubuntu 22.04 with GCC 11. They pass the core tests and the GUI self-test in a container, on a virtual X11 display with software OpenGL and the simulated radio. **They have never been run on a real desktop, on Wayland, or with a real radio or sound card.**
- **macOS**: **not tested.** The binaries come from an automated build, and nobody has tried them on a real Mac desktop or with a real radio.
- **IC-7300 MK2**: not tested.
- **IC-9700**: **never run on a real radio.** Its support is written from Icom's CI-V Reference Guide and Hamlib, and tested against the simulated radio only. See [What's still unproven on an IC-9700](#whats-still-unproven-on-an-ic-9700).
- **IC-R8600**: **never run on a real radio.** Its support is written from Icom's CI-V Reference Guide and Hamlib, and tested against the simulated radio only. See [What's still unproven on an IC-R8600](#whats-still-unproven-on-an-ic-r8600).

## Building

You only need this if you want to change the code, or if there's no [ready-made program](#download) for your system.

You need CMake 3.16 or newer and a C++17 compiler. All the libraries are already in `third_party/` at pinned versions, so there's nothing else to download and there are no submodules:

| Library | Version |
|---|---|
| Dear ImGui | 1.91.9b |
| GLFW | 3.4 |
| miniaudio | 0.11.22 |

### Windows with MinGW-w64

```powershell
winget install BrechtSanders.WinLibs.POSIX.UCRT   # GCC, CMake and Ninja; open a new terminal afterwards
winget install Git.Git                            # if you don't already have Git

git clone https://github.com/laindonlad/IC-7300-Scope.git
cd IC-7300-Scope
cmake -S . -B build -G Ninja
cmake --build build
.\build\test_core
.\build\ic7300console.exe --demo
```

The executables are statically linked, so `ic7300console.exe` can be copied to another PC and run on its own.

### Windows with Visual Studio

Install the **Desktop development with C++** workload in the Visual Studio Installer, then:

```powershell
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release
.\build\Release\test_core
```

The programs are in `build\Release`, so use that path in the commands below. These builds use the shared Microsoft C++ runtime, so another PC may need the Visual C++ Redistributable. Use the MinGW-w64 build if you want a single file to hand to someone.

For an older Visual Studio, change the generator name. `cmake --help` lists them. Only 2026 has been tested.

### Build outputs

A build makes the same three programs as the [download](#download), in `build`. The default build type is Release. To build only the core library, the tests and the probe, without the window, add `-DBUILD_GUI=OFF` to the first `cmake` command.

### Linux and macOS

Linux needs these packages, and your user must be in the `dialout` group to open the serial port:

```sh
sudo apt install build-essential cmake ninja-build pkg-config libgl1-mesa-dev xorg-dev libwayland-dev libxkbcommon-dev wayland-protocols
sudo usermod -aG dialout "$USER"     # then log out and back in
```

macOS needs Xcode's command line tools and CMake. On both, the build commands are the same as above, using `./build/ic7300console` to run it.

### Updating `bin`

The programs in `bin` are copied in by hand, so they only change when someone rebuilds them. After changing the code, update each one:

- **Windows**: do the MinGW-w64 build, copy the three `.exe` files to `bin/windows-x64`, and run `strip` on them.
- **Linux**: do the build on Ubuntu 22.04, so the programs run on as many distributions as possible. Add `-DCMAKE_EXE_LINKER_FLAGS="-static-libstdc++ -static-libgcc"` to the first `cmake` command, and copy the three programs to `bin/linux-x64`. Docker can provide Ubuntu 22.04 on any system. If you commit from Windows, add new files with `git add --chmod=+x` to keep them executable.
- **macOS**: run **Build macOS binaries** from the repository's Actions tab. It builds for Apple Silicon and Intel on a Mac that GitHub provides, runs the tests and commits the result to `bin/macos`.

## Setting up the radio

Connect the radio's USB port to the PC. It appears as a serial port (a Silicon Labs CP210x bridge) and a sound card (**USB Audio CODEC**). If no COM port appears, install the CP210x driver from Silicon Labs or Icom's USB driver. An IC-9700's bridge is expected to show two COM ports: CI-V is on the one called **Enhanced**, which the program prefers. If it picks the wrong one, choose it in **Rig > Settings**.

Then, on the radio, go to **MENU > SET > Connectors > CI-V**:

| Setting | Value | Why |
|---|---|---|
| CI-V USB Port | Unlink from [REMOTE] | The scope stream needs this |
| CI-V USB Baud Rate | 115200 | The scope stream needs this too |
| CI-V Address | Leave it at the default: 94h on an IC-7300, A2h on an IC-9700, 96h on an IC-R8600 | How the app recognises the radio |
| CI-V USB Echo Back | OFF | ON also works |

And under **Connectors**:

| Setting | Value | Why |
|---|---|---|
| USB AF Output Level | Raise it until the level bar in the AUDIO toolbar sits below red | Sets the level of the audio waterfall and recordings |
| USB SEND and USB Keying | OFF | Otherwise a serial line left high could key the radio |

The app can't change these for you, by design. Menu writes use addresses that differ between the IC-7300 and the MK2, and a wrong address silently changes some other setting, so the app never sends them.

Only one program can own a COM port, so close any other CAT software first. The sound card can be shared. To run this app alongside something that needs the COM port, such as WSJT-X, use `--no-rig` for the audio waterfall alone.

### Which radio it is

The program asks with CI-V's "read the transceiver ID" (`19 00`): first at the address in **Rig > Settings**, then at 94h, A2h, 96h and the broadcast address. Nothing else is sent until a radio has answered. It asks again whenever the link has been lost, so you can swap radios on the same cable. Changing radio turns ARM TX off.

If you've changed the radio's CI-V address, put the new one in Settings. **Radio** in Settings can also say outright which it is, if detection gets it wrong. A radio that isn't one of these three is only spoken to if you've entered its address, and is then treated as an IC-7300, as it always was.

### What changes on an IC-9700

| | IC-7300 | IC-9700 |
|---|---|---|
| **Bands** | 30 kHz to 74.8 MHz | 144-148, 430-450 and 1240-1300 MHz, on the US model. Models sold elsewhere have less, and refuse the rest |
| **Full power** | 100 W | 100 W on 2 m, 75 W on 70 cm, 10 W on 23 cm. The power slider and Po meter follow the band |
| **Tuner, XIT** | Yes | The radio has neither, so the buttons aren't shown |
| **Attenuator** | 20 dB | 10 dB |
| **Preamp** | Off, P1, P2 | Off, On, External, Both |
| **Modes** | | DV as well, and DD on 23 cm |
| **A/B** | Swaps the VFOs with the radio's command | Writes each VFO with the other's settings, as the radio's command would exchange its MAIN and SUB bands instead |
| **Typing a number** | `145` is 145 kHz | `145` is 145 MHz. `145500` is kHz |

The program works the **main band only**. It selects it on connecting, and its scope follows that band. It has no controls for the sub band, dualwatch or satellite mode, and the radio refuses the other-VFO commands in satellite mode. Stations and memories that aren't on the connected radio's bands can't be tuned: the program says so rather than jumping somewhere else.

### What changes on an IC-R8600

It's a receiver with one VFO, so everything to do with transmitting or a second VFO is gone: ARM TX, PTT, TUNE, split, A/B, A=B, RIT, data mode, the CW / Voice tab and the transmit meters. The program never sends those commands to it, and it can't be armed.

| | What you get |
|---|---|
| **Coverage** | 10 kHz to 3 GHz. A US model has the cellular bands blocked, and refuses them |
| **Antennas** | ANT1, ANT2 and ANT3 where a transceiver has its transmit buttons. ANT1 is Type-N, for the whole range. ANT2 (SO-239) and ANT3 (RCA, 500 ohms) only work below 30 MHz, so above that they're greyed out |
| **Modes** | WFM has its own button. S-AM and its lower and upper sideband forms are on one menu, and DV, P25, dPMR, NXDN and DCR on another. RTTY is what the radio calls FSK |
| **Bands** | A menu by what they're for, as there are about 140 of them. Page Up and Page Down still go through them in order |
| **Attenuator** | Off, 10, 20 or 30 dB |
| **Preamp** | Off or on, with IP+ as a switch of its own |
| **Meters** | The S meter, whether the squelch is open, and whether the receiver is overloaded |
| **Scope** | Spans up to +/-2.5 MHz |
| **Typing a number** | As on an IC-9700: `100` is 100 MHz and `145500` is kHz. Long wave needs a `k`, as in `198k` |

On the rear USB port, the scope needs CI-V USB Port set to Unlink from [REMOTE], as well as the baud rate at 115200. The front USB port only needs the baud rate.

## Running it

| Option | What it does |
|---|---|
| *(none)* | Finds the radio and connects |
| `--demo` | Uses a simulated radio and test audio. Touches no hardware, and keeps its own settings, memories and log |
| `--demo-radio 9700` | The same, simulating an IC-9700 |
| `--demo-radio 8600` | The same, simulating an IC-R8600 |
| `--port COM11` | Uses this serial port instead of the saved or detected one |
| `--no-rig` | Audio waterfall only: leaves the serial port alone |
| `--size 1600x900` | Sets the window size |
| `--tab N` | Opens on a right-hand tab: 0 Keypad, 1 Memories, 2 Log, 3 CW / Voice |
| `--settings` | Opens with the Settings dialog showing |
| `--tune HZ` | Tunes to this frequency soon after starting |
| `--find TEXT` | Opens Find a station with this search |
| `--sync-clock` | Sets the computer's clock from internet time and exits, with no window ([details](#setting-the-clock)). Needs administrator rights or root |
| `--selftest out.txt` | Runs the scripted GUI test ([details](#testing)). Use it with `--demo` |
| `--screenshot out.bmp` | Saves a screenshot and exits |
| `--frames N` | Which frame `--screenshot` captures |

Port, baud rate, CI-V address, sound devices, your callsign and the PTT time limit are all in **Rig > Settings**. Keys and the radio setup are also listed in **Help**.

## Using it

### The window

| Area | What's there |
|---|---|
| **Top** | Frequency and mode of both VFOs; S, SWR, ALC and Vd meters (click them to swap the bars for a backlit needle meter, and click again to go back); ARM TX, PTT, TUNE, ATU, SPLIT, A/B, A=B |
| **Below that** | Band, mode, data mode, filter and tuning step |
| **Left** | Receive, filter and noise controls, including AF, RF gain, preamp, attenuator, AGC, Twin PBT, NB, NR and notch |
| **Centre** | The band scope and the audio waterfall. Drag the bar between them to resize. Either can be hidden from **View** |
| **Right** | Tabs for Keypad (with the tuning dial), Memories, Log and CW / Voice |
| **Bottom** | Connection status, UTC and the **Sync** button for [setting the clock](#setting-the-clock). The dot is green when connected, amber when the port is open but the radio isn't answering, and red when the port can't be opened |

### Setting the clock

FT8, FT4 and similar modes only decode if the computer's clock is within about a second of everyone else's. **Sync**, next to the time at the bottom of the window, sets it from internet time and says in the status bar how far out it was.

- The program asks NTP servers (`pool.ntp.org`, `time.cloudflare.com`, then `time.google.com`) itself, over UDP port 123, and only when you press the button. It uses the first two that answer.
- It leaves the clock alone if it's within 0.02 s, if it's more than an hour out (check your date and time settings instead), or if the servers disagree by more than half a second.
- **Windows asks for permission each time**, as setting the clock needs administrator rights. To stop it asking, give your account the right: in `secpol.msc`, under Local Policies > User Rights Assignment, add yourself to **Change the system time**, then sign out and back in. Windows Home doesn't have `secpol.msc`.
- On Linux and macOS, it tells you how far out the clock is, and setting it needs `sudo ic7300console --sync-clock`.
- In `--demo`, it only reports how far out the clock is.

### The tuning dial

The knob at the top of the Keypad tab works like a radio's main tuning control. Click it anywhere and spin it, and it follows your hand even if the pointer leaves the knob. Clockwise tunes up.

- **Turn it slowly to fine tune.** One full turn is ten tuning steps, as the label below it shows: at the 100 Hz step, one turn is 1 kHz, in 10 Hz clicks.
- **Spin it fast to move further.** Above 1.5 turns a second the rate builds up, reaching eight times at 4 turns a second.
- **Let go while spinning fast and it runs on** for a moment, like a weighted knob. Click it to stop it. After a slow turn it stops dead, and it never runs on while transmitting.
- **Hold Shift** for ten times the rate.
- **The mouse wheel** moves it one tuning step at a time.

### Entering a frequency

Just start typing, anywhere. Enter goes there and Esc cancels. You can also use the Keypad tab.

| Typed | Means |
|---|---|
| `14.074` | 14.074 MHz |
| `14074` | 14074 kHz |
| `14074.5` | 14074.5 kHz |
| `7` | 7 MHz (75 or below is MHz) |
| `198` | 198 kHz |
| `198k`, `14.074m` | Says so outright |
| `14.074.123` | As the radio displays it |
| `+5`, `-1.5` | kHz up or down from the dial |
| `145.5`, `1296`, `145500` | On an IC-9700 or IC-R8600: MHz, MHz and kHz |

### Keys

| Key | Action |
|---|---|
| Left, Right | Tune by the step (Shift: ten times the step) |
| Up, Down | Change the step |
| Page Up, Page Down | Next or previous band |
| Ctrl+F | Find a station by name |
| F1 to F8 | LSB, USB, CW, CW-R, AM, FM, RTTY, RTTY-R |
| F9, F10 | Data mode, next filter |
| F11, F12 | Split, swap VFOs |
| Ctrl+Space | PTT on or off, once armed |

### Mouse

- **Digits**: the wheel tunes that digit. Click the top half to go up, the bottom half to go down.
- **Waterfalls and the tuning dial**: the wheel tunes by the step.
- **Sliders**: the wheel moves them.
- **Band scope**: click to tune there. Right-click for split and memories. Click a flag to go to that frequency, in its mode if it has one.
- **Band map**: click a portion to tune there.
- **Audio waterfall**: click to move that signal to the pitch, which is 1500 Hz on SSB and your CW pitch on CW. In data modes the dial stays put, so a digital program keeps its place.
- **Band buttons**: click to return to where you last were on that band. Right-click for CW, FT8, FT4, WSPR and SSB frequencies.
- **SPLIT**: right-click for a quick offset.

### Band plan

The scope shows you where you are on the band, using the plan for your country. Choose it in **Rig > Settings**. A band plan is a guide and **not a statement of what your licence allows**. **View > Band plan on the scope** turns all of this off.

- **Band map**: a bar above the scope with the whole of the current band, from where it begins to where it ends. Each portion is named and the part the scope is showing is outlined.
- **Strip under the spectrum**: the same portions for the part of the band on screen.
- **Colours**: blue for CW, purple for data, green for voice, orange for FM, yellow for beacons and teal for satellites. Hold the mouse over a portion in the map for the plan's own wording and its frequencies.
- **Band edges**: a red line, with the band's first and last frequency named. Everything outside is shaded and marked "Out of band". This includes the gaps in bands that come in separate blocks, such as 60 m in most countries.
- **Flags**: common frequencies such as FT8, WSPR, QRP calling and emergency centres of activity. Hold the mouse over one for what it is.

| Plan | Source | In force |
|---|---|---|
| Australia | WIA Band Plan, Revision 2026-1 | February 2026 |
| Canada | RAC 0-30 MHz Band Plan and 60 metres page. 6 m from IARU Region 2 | June 2023 |
| IARU Region 1 | IARU Region 1 HF Band Plan. 6 m from the VHF Handbook v7.51 | October 2020 |
| IARU Region 2 | IARU Region 2 Band Plan | September 2020 |
| IARU Region 3 | IARU Region 3 Band Plan, R3-004 Rev. 2 | November 2024 |
| Japan | JARL Amateur Band Plan | July 2025 |
| New Zealand | NZART Band Plans and HF usage guide | June 2026 |
| United Kingdom | RSGB Band Plan 2026 | January 2026 |
| United States | ARRL band plan. 60 m from FCC Report and Order 25-60 | February 2026 |

Countries without their own plan can use their IARU region's.

Every plan also has 2 m, 70 cm and 23 cm, and the band buttons show only what the connected radio has. These come from:

| Plan | Source for 2 m, 70 cm and 23 cm |
|---|---|
| Australia | WIA Band Plan, Revision 2026-1, February 2026 |
| Canada | RAC plans: 2 m approved October 2020, 70 cm September 1995, 23 cm October 1997 |
| IARU Region 1 | VHF Handbook v10.03, February 2026 |
| IARU Region 2 | IARU Region 2 Band Plan, September 2020 |
| IARU Region 3 | R3-004 Rev. 2, November 2024 |
| Japan | JARL Amateur Band Plan, July 2025 |
| New Zealand | NZART Band Plans, 2026 |
| United Kingdom | RSGB Band Plan 2026 |
| United States | ARRL band plan page, undated, with limits from FCC Part 97 |

FT8 and similar presets that a plan doesn't list come from WSJT-X's default frequencies, and the ISS downlink from its operators, and their notes say so. Some 70 cm and 23 cm plans leave out FT8 where the segment doesn't list data. In the US and Region 2, 420-430 MHz is on the plan but below what the radio tunes.

Each plan is a JSON file you can edit, in the `bandplans` folder inside [your data folder](#your-data). **Open plans folder** in Settings takes you there and `README.txt` explains the format. After editing, press **Reload**. To add a country, copy a file to a new name and change its `name`. If a file has a mistake, Settings names the file and the line, and the built-in plan is used until it's fixed. **Reset this plan** puts the built-in copy back and keeps yours as `<name>.json.bak`.

A plan leaves out any band its country doesn't have, so that band gets no button. The 4 m button only works on radios that have that band. Others refuse the command and the status bar says so. A copy of a plan with no band above 100 MHz, such as one from an earlier version of this program, keeps the built-in 2 m, 70 cm and 23 cm, so upgrading doesn't leave an IC-9700 without bands.

Every plan also covers the rest of an IC-R8600's range, from 10 kHz to 3 GHz: broadcasting, aviation, maritime, land mobile, satellites and mobile phones, fitted round that plan's amateur bands. These bands have a `group` in the file, which is the heading they go under on the receiver's Bands menu, and only a receiver is offered them. They're broad, summarised from the ITU table of allocations and general knowledge of each region, and not checked against each country's own table, so treat them as a guide to what's where. A plan with no groups, such as a copy from an earlier version of this program, keeps the built-in ones, except any that would overlap a band of its own. On a receiver, nothing is shaded as out of band, since that's about where you may transmit.

### Stations

As well as the amateur bands, the scope flags what else is on the air, and this list stays the same whichever band plan you choose. Few other services work inside the IC-9700's bands, so its flags are mostly satellites and beacons, and a satellite is only there during a pass. It's for **listening only**. Some countries, including the UK, make it an offence to listen to messages not meant for you or to pass them on, so check what applies where you are.

| Service | What's listed |
|---|---|
| Commercial | HFDL aircraft data, airline long-distance voice, Kyodo press fax, and the licence-free PMR446 and LPD433 bands |
| Government | Time signals, VOLMET, North Atlantic air traffic control |
| Naval and maritime | NAVTEX, WEFAX, GMDSS distress channels, coast guard and marine weather, French Navy STANAG 4285 |
| Military | US HFGCS voice and ALE, RAF Volmet, Trenton, Russian stations such as The Buzzer |
| Broadcast | About 1,800 shortwave and medium wave broadcasters from 66 countries with their schedules, such as the Voice of America, BBC World Service and China Radio International, plus the long wave stations, US clear channel AM stations, and the medium wave and shortwave bands |
| Amateur satellites and beacons | The ISS, FM and linear satellites with their beacons, and a few long-running 2 m, 70 cm and 23 cm beacons |

With an IC-R8600 connected there's a second list, `stations/receiver.json`, of about 930 more:

| Service | What's listed |
|---|---|
| Aviation | HF air routes by region, common VHF and UHF voice, ACARS, VDL Mode 2, ADS-B and radar |
| Naval and maritime | Marine VHF channels, AIS, and the HF marine voice channels |
| Military | VLF and LF naval stations |
| Government | NOAA Weather Radio and the US interoperability channels |
| Satellites and navigation | GPS and the other navigation systems, weather satellites, Inmarsat, distress beacons, the hydrogen line |
| Land mobile and licence-free | CB, PMR446, LPD433, FRS and GMRS, MURS, Australian UHF CB, and short-range sensors and LoRa |
| Broadcast | DAB channels, UHF TV channels, and the FM bands |

Local things such as your own airport's frequencies aren't listed. Many of these are digital or encrypted, so a flag says what's there, not that you can hear it. The file names its sources, and says which entries come from general knowledge rather than a source checked when it was written.

- **Flags**: station flags are solid, in a colour for their service, and the plan's are outlined. Hold the mouse over one for what it is. Clicking tunes there and sets the mode. A data signal's flag sits on the signal itself and the click tunes USB below it, ready for a decoder.
- **Broadcast bands**: a bar along the bottom of the spectrum.
- **Broadcasters**: `stations/broadcast.json`, by home country. A broadcaster has a flag only during its scheduled hours, so the scope shows what should be on now. **Stations > Only what's scheduled now** turns that off. Hold the mouse over one for its times in UTC, languages, target area and transmitter. Scheduled doesn't mean you'll hear it: that depends on the hour, the season and where the signal is aimed. Schedules change at the end of March and October, when the list needs regenerating with `tools/make_broadcast_stations.py`. It covers stations that broadcast internationally or reach far, not every local medium wave station: outside the US clear channels, most domestic AM stations aren't in it.
- **Find a station**: **Stations > Find a station** or Ctrl+F searches names, types, places and frequencies in kHz, such as `northwood`, `wefax japan` or `voice of america`. Click a result, or press Enter for the first.
- **Stations menu**: every station by service and group. It also turns each service's flags on and off.

The list is `stations/worldwide.json` in [your data folder](#your-data), and you can edit it or add your own files beside it. `README.txt` there explains the format. **Open stations folder**, **Reload** and **Reset station list** are in Settings. Its sources are named in the file. They are public and hobby lists of varying age, so a flag doesn't promise a signal. A copy with nothing above 100 MHz, such as one from an earlier version of this program, keeps the built-in VHF and UHF stations.

## Transmit safety

These rules are enforced in `src/rig.cpp`, below the window, so no button or menu can get round them:

- **Nothing transmits until ARM TX is pressed**, and it's off at every start. That covers PTT, TUNE, CW and voice memories. Disarming also releases PTT.
- **PTT has a time limit.** It lets go by itself after 180 seconds by default (5 to 600 seconds, in Settings).
- **Un-key is confirmed.** It's resent every 250 ms until the radio itself reports it has stopped, including across a lost link. Closing the program un-keys too.
- **Listen won't play into the radio.** It refuses to play to any device named USB Audio CODEC, as the radio's sound card is its modulation input.
- **Opening the port doesn't key the radio.** RTS and DTR are held low, as the radio can be set to key on either.

> [!WARNING]
> A hard crash or a pulled USB cable can't send an un-key. The radio's own Time-Out Timer is the backstop, so keep it on.

## Your data

Everything is stored as plain text files in a per-user folder. **Rig > Open data folder** takes you there.

| System | Location |
|---|---|
| Windows | `%APPDATA%\IC7300Console` |
| Linux | `~/.config/ic7300console` |
| macOS | `~/Library/Application Support/IC7300Console` |

| File | Contents |
|---|---|
| `settings.ini` | Settings, as `key=value` |
| `memories.csv` | Memories |
| `log.csv` | The log |
| `log.adi` | The log exported as ADIF, for other logging software |
| `bandplans/*.json` | Band plans, which you can edit ([details](#band-plan)). Written the first time the program runs, and never overwritten |
| `stations/*.json` | Station lists, which you can edit ([details](#stations)). Written the first time the program runs, and never overwritten |
| `rx-<date>-<time>-<hz>.wav` | Recordings: 16-bit mono, 48 kHz |

Demo mode uses `demo-settings.ini`, `demo-memories.csv` and `demo-log.csv`, so trying it can't disturb a real setup.

ADIF has no single mode for USB-D and similar, as they could be any digital mode. Those records leave out the MODE field and note what the radio showed in the comment, so you may want to set the mode in your logging software.

## How it works

```
                 ┌───── IC-7300, IC-9700 or IC-R8600 ──────┐
                 │  CP210x serial port    USB Audio CODEC  │
                 └───────┬───────────────────────┬─────────┘
                    CI-V │ 115200 baud           │ 48 kHz audio
                         │                       │
 serial.cpp / sim_rig ── Transport           miniaudio ── demo_audio.h
                         │                       │
 rig thread              Rig (rig.cpp)           │        audio callback
 civ.h encodes/decodes   │                       │
                         │ RigState, ScopeLine   │ samples
                         │                       │
 UI thread (app.cpp)     │ window ── setters     │ FFT (dsp.h)
                         ▼                       ▼
                  band waterfall ──────── audio waterfall
                         └──── waterfall.cpp ────┘
```

The window never talks to the radio directly. One thread owns the serial port and everything else goes through it, so a slow radio can't stall the display.

### CI-V, the radio's protocol

`src/civ.h` builds and parses CI-V frames: `FE FE <to> <from> <cmd> [data] FD`. An IC-7300 is at address `94h`, an IC-9700 at `A2h`, an IC-R8600 at `96h`, and the PC at `E0h`. What differs between them is in one `civ::Radio` description of each. Frequencies are little-endian BCD and levels are big-endian BCD. It also holds the meter calibration tables, which turn raw readings into S units, watts, SWR, volts and amps. It's plain C++ with no OS or threading code, so the tests can check it byte by byte.

### The rig thread

`Rig` in `src/rig.cpp` runs a loop on its own thread: read whatever arrived, service transmit safety, then send one thing. It sends only when the last command has been answered, or after a 250 ms timeout, because the radio can ignore a command sent before it has replied to the previous one.

- **Commands** from the window go in a queue. A newer command of the same kind replaces a queued one, so spinning the dial can't build a backlog.
- **Polling** fills the idle time, always choosing the most overdue reading:

  | Reading | Every |
  |---|---|
  | S meter (receive), or power, SWR, ALC, compression and current (transmit) | 100 to 150 ms |
  | TX state | 200 ms |
  | Frequency | 250 ms |
  | Mode, data mode, filter width, other VFO | 500 ms to 1.2 s |
  | Split, tuner, attenuator, RIT/XIT | 1 to 1.5 s |
  | All levels and switches, which a knob on the radio might have changed | 2.5 s |
  | Scope settings, supply voltage, radio ID | 3 to 5 s |

- **Setters return at once** and update `RigState` immediately, so the window never waits on the radio. Replies to reads sent before a change would make the display jump back, so readings of that value are ignored for 400 ms after a write.
- **The link is watched.** If no frame arrives for 2.5 seconds the radio counts as gone, and queued commands are dropped so a stale one can't move the dial when it returns. A lost port is retried every 2 seconds. On reconnecting, everything is read again and the scope stream is requested again, as a power cycle forgets it.
- **Power on** sends about 150 `FE` bytes first, as the radio needs them to wake its CI-V at 115200 baud.

### The band waterfall

The radio streams its scope over CI-V using command `27 00`. Each sweep comes in 11 divisions. The first carries the mode and the frequency edges, and together they hold 475 points valued 0 to 160. `Rig` assembles the divisions into a `ScopeLine` and the window scales it to 0 to 255 for the waterfall.

The frame's own edges are used for click-to-tune, never the dial, because in Center mode the sweep may be centred on the filter rather than the dial. Waterfall history is cleared when the span changes or the view moves by more than a quarter of the span, since it would only mislead.

### The audio waterfall

`src/audio.cpp` uses miniaudio to capture the radio's USB sound card in shared mode, as mono 48 kHz floats. `src/dsp.h` applies a Hann window and an FFT, then converts to dBFS and maps -140 to 0 dB onto 0 to 255.

The FFT size can be 2048, 4096, 8192 (the default, about 6 Hz per bin) or 16384. The rate defaults to 25 lines a second, and the capture reopens by itself if the radio is switched off.

`src/tuning.h` maps an audio tone to its RF frequency. It allows for sideband, CW pitch and the RTTY mark tone, which is how clicking the waterfall knows where to tune. AM and FM don't map a tone to one RF frequency, so clicking doesn't retune in those modes.

### The tuning dial

`tuning::Dial` in `src/tuning.h` measures how far the pointer has moved round the knob's centre, so grabbing it anywhere doesn't make it jump. It converts that into clicks, 100 to the turn, and applies the speed-up and the run-on. It ignores the pointer very close to the centre, where a tiny movement would swing the angle wildly. It has no GUI code, so `test_core` can check it directly. `App::drawDial` draws the knob, with the light fixed while the flutes and dimple turn.

### Drawing

`src/waterfall.cpp` keeps each waterfall in one OpenGL texture, used as a ring. A new line overwrites the oldest and only that row is uploaded, however large the window. Raw levels are kept beside the colours, so changing brightness, contrast or palette also applies to the history.

`src/app.cpp` draws the whole interface every frame with Dear ImGui. It embeds its fonts in the executable at build time. The layout needs 1280x720 logical pixels and shrinks to fit if display scaling leaves fewer than that.

### The simulated radio

`src/sim_rig.cpp` implements `Transport`, the same interface as the serial port, so `Rig` can't tell the difference. It answers CI-V, generates scope sweeps and can inject faults: refused commands, echo, silence, lost un-keys and a pulled cable. It drives both `--demo` and the tests.

### What the real radio taught us

- It sends about 4 scope sweeps a second at Fast (some runs measured 8), whatever the polling load. That's the radio's limit, so the band waterfall's rows are drawn tall to compensate.
- It can ignore a command sent before it has answered the last one, so `Rig` always waits for the reply.
- In Center mode, the sweep may be centred on the filter rather than the dial (a radio setting), so tuning uses the frame's own edges.

### Source layout

| Path | What |
|---|---|
| `src/civ.h` | The CI-V protocol: frames, commands, meter calibration |
| `src/transport.h` | The byte-pipe interface shared by the serial port and the simulator |
| `src/rig.h`, `src/rig.cpp` | The rig thread: command queue, polling, state, transmit safety |
| `src/serial.cpp` | Serial ports for Win32 and POSIX, and finding the radio's port |
| `src/sim_rig.h`, `src/sim_rig.cpp` | The simulated IC-7300, IC-9700 and IC-R8600 |
| `src/audio.h`, `src/audio.cpp` | Sound in and out, through miniaudio |
| `src/demo_audio.h` | The demo's generated audio |
| `src/dsp.h` | The FFT and dB scaling |
| `src/waterfall.h`, `src/waterfall.cpp` | The waterfall texture and palettes |
| `src/tuning.h` | Frequency entry, audio-to-RF mapping and the tuning dial's behaviour |
| `src/bands.h`, `src/bands.cpp` | Band plans: reading the JSON files, and which band a frequency is in |
| `bandplans/*.json` | The built-in band plans, compiled into the program |
| `stations/*.json` | The built-in station lists, compiled into the program |
| `src/store.h`, `src/store.cpp` | Settings, memories, log, ADIF, WAV |
| `src/clock_sync.h`, `src/clock_sync.cpp` | Asking NTP servers the time and setting the computer's clock |
| `src/app.h`, `src/app.cpp` | The window and the self-test script |
| `src/main.cpp` | Command-line options |
| `tests/test_core.cpp` | The core test suite |
| `tools/probe.cpp` | The check against a real radio |
| `res/` | The Windows icon and the resource file that embeds it |
| `tools/make_icon.py` | Draws the icon. Only needed to change it |
| `bin/` | The ready-to-run programs for each system |
| `.github/workflows/build-macos.yml` | The automated build behind `bin/macos` |

## Testing

The commands below use the `build` folder. If you downloaded the programs, use your system's folder in `bin` instead.

| Test | Needs | What it covers |
|---|---|---|
| `test_core` | Nothing | About 30,000 checks: CI-V bytes, frequency entry, the tuning dial, band plans, stations and their search, FFT and log files, plus the rig thread against the simulated radios, including finding which radio it is, refused commands, echo, stale readings, lost links and every transmit guard |
| `ic7300console --demo --selftest out.txt` | A display | 120 checks: real clicks and keystrokes through the window, against the simulated IC-7300, then IC-9700, then IC-R8600 |
| `ic7300probe` | A real radio | Reads everything from the radio and listens to the scope |

### Core tests

```powershell
.\build\test_core            # or: ctest --test-dir build
```

This takes about 30 seconds and ends with `0 failed`. The number of checks varies slightly from run to run, as some loop over what the simulated radio received.

### GUI self-test

`ic7300console` is a Windows GUI program, so PowerShell doesn't wait for it and it prints nothing. Use `Start-Process -Wait` and read the results file:

```powershell
Start-Process .\build\ic7300console.exe -ArgumentList '--demo','--selftest','selftest.txt' -Wait
Get-Content selftest.txt -Tail 1
```

It takes about a minute, opens a window without taking focus, and exits with code 1 if anything fails.

**Don't click on its window during the run.** If it gets focus, the real cursor position overrides the scripted one and the mouse checks fail, while the keyboard checks still pass. Windows can also hand it focus when you close another program. If you see many mouse-related failures, run it again and leave it alone.

### Checking a real radio

```powershell
.\build\ic7300probe          # finds the port
.\build\ic7300probe COM11    # or name it
```

This is safe on a live station. It **never keys the transmitter or starts the tuner**. Its only writes are turning the scope data on, and off again on the way out. Add `--write-same` to also write back the frequency, mode, AF level and span the radio already has, which proves writes are accepted without changing anything.

It prints `ok` or `FAIL` for each reading and ends with `ALL CHECKS PASSED` or `SOME CHECKS FAILED`.

### What the tests can't show

The simulated radio shares this code's reading of the protocol, so passing against it shows the app is self-consistent and handles faults. **Only `ic7300probe` shows the protocol is right, and since it never keys, it doesn't exercise transmit on a real radio.** The transmit guards are tested against the simulator.

### What's still unproven on an IC-9700

`ic7300probe` hasn't been run on one yet, and it would settle most of these. Until it has, treat the IC-9700 as untested:

- **What `19 00` reports if the CI-V address has been changed.** Handled either way, but if it reports the new address the radio can't be told from any other, and **Radio** in Settings has to say.
- **Whether a broadcast `19 00` is answered.** Radios at their default addresses don't need it.
- **Changing band with `05` when the SUB band is already on that band.** The status bar asks about this if the radio refuses. The program doesn't exchange the bands for you.
- **Which COM port carries CI-V**, and its name in Windows, which comes from general knowledge of Icom's dual bridge rather than the guide.
- **Whether the A/B workaround and `26 01` are accepted** exactly as the guide describes.

### What's still unproven on an IC-R8600

`ic7300probe` hasn't been run on one yet either. Until it has, treat the IC-R8600 as untested:

- **What `12` does at and above 30 MHz.** The guide says antenna selection is only for 10 kHz to 29.999999 MHz, but not what a read returns up there. The program shows ANT1 and sends nothing else.
- **Which modes take a filter.** WFM and the digital modes are sent with no filter byte, which the guide allows. The filter buttons are greyed out for them on the assumption that they have no choice of filter.
- **S-AM's filter widths.** The guide only gives AM's, and S-AM is assumed to be the same.
- **The width shown for WFM and the digital modes** is the width of the channel, for shading the scope, and not read from the radio.

## Troubleshooting

| What you see | What to do |
|---|---|
| `COMx is in use by another program` | Close the other CAT program. A port can only have one owner |
| `COMx isn't there (radio off or unplugged?)` | Turn the radio on and check the cable. If the COM number has changed, choose the new one in **Rig > Settings** |
| `COMx open, but no answer` | Check that the radio is on, with CI-V USB Baud Rate at 115200. On an IC-9700, make sure it's the Enhanced COM port |
| `A radio answered at ...` | It isn't an IC-7300, IC-9700 or IC-R8600, or its CI-V address has been changed. Put that address in **Rig > Settings**, and choose **Radio** if it's one of those |
| `The rig wouldn't set that frequency. Is the SUB band on that band?` | On an IC-9700, main and sub can't share a band. Move the sub band on the radio, then try again |
| No COM port at all | Install the CP210x driver (see [Setting up the radio](#setting-up-the-radio)) |
| It connects to the wrong device | It picks the first port that looks like a CP210x. Choose the right one in **Rig > Settings** or use `--port` |
| Band waterfall is empty, the rest works | Set CI-V USB Port to Unlink from [REMOTE] and the baud rate to 115200. `ic7300probe` checks this too |
| `The rig's USB audio isn't there` | Turn the radio on, or choose the device in **Rig > Settings**. The app looks for one named USB Audio CODEC |
| Audio waterfall is dim, or the level bar is red | Adjust USB AF Output Level on the radio |
| Listen is refused | The chosen output is named USB Audio CODEC. Choose your speakers in **Rig > Settings** |
| PTT, TUNE or CW does nothing | Press **ARM TX** first. It's off at every start |
| `The rig wouldn't ...` in the status bar | The radio refused that command, often because of its current mode or band |
| The window doesn't open | The app needs a graphics driver with OpenGL 3.0 or newer, which some remote desktop sessions and virtual machines don't provide |

## Third-party code

The command bytes were checked against [Hamlib](https://hamlib.github.io/)'s IC-7300, IC-9700 and IC-R8600 backends, and the IC-9700's and IC-R8600's against Icom's CI-V Reference Guides.

These are bundled in `third_party/`, each under its own licence:

| Component | Licence |
|---|---|
| [Dear ImGui](https://github.com/ocornut/imgui) | [MIT](third_party/imgui/LICENSE.txt) |
| [GLFW](https://www.glfw.org/) | [zlib/libpng](third_party/glfw/LICENSE.md) |
| [miniaudio](https://miniaud.io/) | [Public domain or MIT-0](third_party/miniaudio/LICENSE) |
| Roboto font, by Christian Robertson | [Apache 2.0](third_party/imgui/fonts/LICENSE-Roboto.txt) |
| Cousine font, by Steve Matteson | [SIL Open Font License 1.1](third_party/imgui/fonts/LICENSE-Cousine.txt) |

All of these are compiled into the programs in `bin`, so keep these licence files with them if you pass the programs on.

This project is released under the [MIT License](LICENSE).
