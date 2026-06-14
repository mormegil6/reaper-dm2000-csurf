[![C++](https://img.shields.io/badge/C++-17-blue.svg)]() [![Windows](https://img.shields.io/badge/Windows-x64-0078D4.svg?logo=windows&logoColor=white)]() [![REAPER](https://img.shields.io/badge/REAPER-6+-darkgreen.svg)]() [![v0.2](https://img.shields.io/badge/version-v0.2-lightgrey.svg)](https://bmroz.eu/projects/dm2000-csurf) [![License: LGPL v3](https://img.shields.io/badge/License-LGPL%20v3-blue.svg)](https://www.gnu.org/licenses/lgpl-3.0)

# reaper_csurf_dm2000 - Yamaha DM2000 control surface for REAPER

A native REAPER control surface DLL giving bidirectional integration between
the Yamaha DM2000 digital console and REAPER: 24 touch-sensitive faders, pan
knobs, mute/solo/rec-arm/select buttons with LED feedback, transport controls,
automation mode switching, jog wheel, bank switching, and channel-name scribble
strips. The DLL speaks HUI on the console's USB MIDI ports 1-4, with native
Yamaha SysEx on a configurable port 8 output for channel names.

To our knowledge this is the first open-source DM2000 control surface for
REAPER.

**Design document:** see [DESIGN.md](DESIGN.md) for the full protocol
reference, layer plan, and task tracking.

## Hardware requirements

- Yamaha DM2000 **V2** (firmware V2.40) - V1 uses a different DAW port layout
  and is not supported
- USB connection to the PC with the Yamaha USB-MIDI driver (exposes 8 virtual
  MIDI port pairs)
- Console setup:
  - Remote Layer target: **Pro Tools**
  - `SETUP → MIDI/HOST SETUP → DAW = USB 1-4`
- Windows x64, REAPER 6 or later
- Note: "DAW Off-line" on the DM2000 display is normal until REAPER is running
  with the surface configured (the DLL answers the console's keepalive ping)

## Building

Prerequisites:

- Visual Studio 2019 Community or later with the
  "Desktop development with C++" workload
- REAPER Extension SDK extracted to `C:\reaper_extension_sdk\` with the
  environment variable `REAPER_EXTENSION_SDK` pointing at it
- `msbuild` on `PATH`

```powershell
cd C:\dev\reaper_csurf_vs201x
# Close REAPER first - it locks the DLL and the post-build copy will fail
msbuild Builds\VisualStudio2019\reaper_csurf.sln /p:Configuration=Debug /p:Platform=x64
```

The post-build step copies the DLL to `%APPDATA%\REAPER\UserPlugins\`.
Restart REAPER to load it.

Only the **VisualStudio2019** solution includes `csurf_dm2000.cpp`; the
2013/2015/2017 solutions are left over from the scaffold and will not link.

This project is built on the [Erriez/reaper_csurf_vs201x](https://github.com/Erriez/reaper_csurf_vs201x)
scaffold - see its README/wiki for a step-by-step Visual Studio debugging
setup (attaching to reaper.exe, breakpoints, etc.).

## REAPER configuration

1. `Preferences → Control/OSC/web → Add → "Yamaha DM2000"`.
2. In the surface settings, pick the **port group** - with the default console
   setup that is the entry reading `Yamaha DM2000-1 ... Yamaha DM2000-4`.
   The DLL opens 4 consecutive HUI ports from that selection.
3. OK, then restart REAPER if the DLL was just rebuilt.

<p align="center"><img src="doc/config-dialog.png" alt="Config dialog"></p>

## Feature status

### Layer 1 - HUI core: complete

- 4-port MIDI open/close, keepalive ping echo, config dialog
- Faders both directions, touch detect (touch automation works)
- Mute / solo / rec-arm / select buttons with LED feedback
- Transport (play/stop/record/rewind/forward, RTZ, END, LOOP) with LEDs; RTZ/END/LOOP switch
  positions are unverified - verify with MIDI-OX before relying on them
- Bank switching (channel +-1, bank +-24)
- Automation modes: AUTOMIX section buttons control REAPER's global automation override (read/touch/write/latch/latch preview/bypass), with LED feedback

### Layer 2 - Channel strip feedback: complete (hardware-tested)

- [x] Pan: knob input (relative v-pot deltas), ring-LED feedback, knob press centers pan
- [x] Selected channel highlight
- [x] Jog wheel moves the edit cursor (speed-scaled, 1-6)
- [x] 4-char track names on scribble strips (HUI SysEx)
- [x] VU meters on channel strips (100ms peak polling, 3-cycle peak hold,
      red OVER only above 0 dBFS - matches REAPER's clip indication)
- [~] Transport extensions: RTZ, END, LOOP buttons wired; LOOP LED feedback via
      `SetRepeatState`. Switch positions are software guesses - verify with MIDI-OX
      before relying on RTZ/END/LOOP in a session.

### DM2000-specific behavior (hardware-verified - details in DESIGN.md)

- **Fader taper is calibrated to the console's printed scale**: REAPER dB and
  the printed marks agree along the throw (piecewise-linear table from
  mark-by-mark MIDI captures).
- **The console's fader value range tops out at the printed +5 mark** - REAPER
  volumes from +5 to +12 dB all park the motor at +5. Hardware limit, not a bug.
- The console keeps an internal model of DAW fader positions and springs motors
  back to it on release; the DLL echoes every received fader move to keep that
  model in sync.
- On close, the DLL drives all faders to -inf, then clears meters, LEDs, pan rings, and scribbles.

### Layer 3 - Native SysEx (port 8): partial

- SysEx output port is user-configured (config dialog SysEx dropdown)
- [x] Channel names via native SysEx - format captured 2026-06-12; sends pos=0..7 alongside HUI 4-char names; hardware-tested: scribble strip remains 4-char wide in DAW mode (physical limit)
- [x] Meter bridge - driven by existing HUI meter messages (Layer 2); no native SysEx needed
- [~] Scene recall: PC receive implemented (DM2000 scene change → REAPER marker jump for
      scenes 1-9); set GENERAL port to a DAW USB port on the console. PC send and SysEx
      scene dump not yet implemented.

### Layer 4 - future

EQ/dynamics parameter control, sends, surround panner joystick, talkback,
USER DEFINED KEYS. See [DESIGN.md](DESIGN.md).

## Known limitations

- **Channels 25-32 (port 4) not mapped** - port 4 is active but no track controls
  are wired there; the physical purpose of those strips on the DM2000 is not yet
  determined.
- **macOS port in progress** - a universal (arm64 + x86_64) build via a SWELL
  Makefile (`Builds/Make/`) compiles and loads in REAPER; hardware verification
  with a connected DM2000 is still pending. Windows x64 remains the only
  hardware-tested target. See [MACOS_BUILD.md](MACOS_BUILD.md).
- **Scribble strips show 4 characters only** - hardware-verified; the display is
  4-char wide in DAW mode regardless of what native SysEx sends.
- **Scene recall partial** - PC receive is implemented (DM2000 scene recall sends PC on the
  GENERAL port → REAPER jumps to the matching marker); user must set GENERAL to a DAW USB
  port. PC send and SysEx scene dump not yet implemented.

## License

LGPL v3 (Cockos csurf sources are LGPL; user contributions are additionally
released under The Unlicense). See [LICENSE](LICENSE).

## Contact

Bartłomiej Mróz · bartlomiej.mroz@pg.edu.pl · Department of Multimedia Systems, Gdańsk University of Technology · [bmroz.eu](https://bmroz.eu)
