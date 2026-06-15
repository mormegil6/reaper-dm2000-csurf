[![C++](https://img.shields.io/badge/C++-14-blue.svg)]() [![Windows](https://img.shields.io/badge/Windows-x64-0078D4.svg?logo=windows&logoColor=white)]() [![macOS](https://img.shields.io/badge/macOS-arm64%2Fx86__64-000000.svg?logo=apple&logoColor=white)]() [![REAPER](https://img.shields.io/badge/REAPER-6+-darkgreen.svg)]() [![v0.5](https://img.shields.io/badge/version-v0.5-lightgrey.svg)](https://bmroz.eu/projects/dm2000-csurf) [![License: LGPL v3](https://img.shields.io/badge/License-LGPL%20v3-blue.svg)](https://www.gnu.org/licenses/lgpl-3.0)

# reaper_csurf_dm2000 - Yamaha DM2000 control surface for REAPER

A native REAPER control surface plugin (DLL/dylib) giving bidirectional
integration between the Yamaha DM2000 digital console and REAPER: 24
touch-sensitive faders, pan knobs, mute/solo/rec-arm/select buttons with LED
feedback, transport controls, automation mode switching, jog wheel, bank
switching, and channel-name scribble strips. The plugin speaks HUI on the
console's USB MIDI ports 1-4, with native Yamaha SysEx on a configurable
port 8 output for channel names.

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
- Windows x64 or macOS (arm64/x86_64), REAPER 6 or later
- Note: "DAW Off-line" on the DM2000 display is normal until REAPER is running
  with the surface configured (the plugin answers the console's keepalive ping)

**DM2000 Owner's Manual (V2):** [jp.yamaha.com - DM2000V2 EN OM](https://jp.yamaha.com/files/download/other_assets/7/334227/dm2000v2_en_om_g0.pdf) - MIDI/HUI chapter 18, SysEx appendix C.

## Installation (pre-built)

Download the latest release from the [Releases](../../releases) page.

**Windows:** copy `reaper_csurf_dm2000_x64.dll` to `%APPDATA%\REAPER\UserPlugins\`.

**macOS:** copy `reaper_csurf_dm2000.dylib` to `~/Library/Application Support/REAPER/UserPlugins/`.

Restart REAPER after copying.

> macOS note: the dylib is a universal binary (arm64 + x86_64). Hardware-verified
> with a connected DM2000 on 2026-06-15 (v0.3); all features work on macOS.

## Building from source

### Windows

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

Only the **VisualStudio2019** solution includes `csurf_dm2000.cpp`; the
2013/2015/2017 solutions are left over from the scaffold and will not link.

This project is built on the [Erriez/reaper_csurf_vs201x](https://github.com/Erriez/reaper_csurf_vs201x)
scaffold - see its README/wiki for a step-by-step Visual Studio debugging
setup (attaching to reaper.exe, breakpoints, etc.).

### macOS

See [MACOS_BUILD.md](MACOS_BUILD.md) for the full procedure (prerequisites,
WDL clone, `make`, binary verification, install).

## REAPER configuration

1. `Preferences → Control/OSC/web → Add → "Yamaha DM2000"`.
2. In the surface settings, pick the **port group** - with the default console
   setup that is the entry reading `Yamaha DM2000-1 ... Yamaha DM2000-4`.
   The plugin opens 4 consecutive HUI ports from that selection.
3. OK.

<p align="center"><img src="doc/config-dialog.png" alt="Config dialog"></p>

## Feature status

### Layer 1 - HUI core: complete

- 4-port MIDI open/close, keepalive ping echo, config dialog
- Faders both directions, touch detect (touch automation works)
- Mute / solo / rec-arm / select buttons with LED feedback
- Transport (play/stop/record/rewind/forward, RTZ, END, LOOP) with LEDs; all switch positions
  hardware-verified 2026-06-15 (RTZ/END are zone 0x0F sw0/1; LOOP is zone 0x0F sw3 - Locate row, not transport zone 0x0E)
- Bank switching (channel +-1, bank +-24)
- Automation modes: AUTOMIX section buttons control REAPER's global automation override (read/touch/write/latch/latch preview/bypass), with LED feedback; all sw assignments hardware-verified 2026-06-15

### Layer 2 - Channel strip feedback: complete (hardware-tested)

- [x] Pan: knob input (relative v-pot deltas), ring-LED feedback, knob press centers pan
- [x] Selected channel highlight
- [x] Jog wheel moves the edit cursor (speed-scaled, 1-6)
- [x] 4-char track names on scribble strips (HUI SysEx)
- [x] VU meters on channel strips (100ms peak polling, 3-cycle peak hold,
      red OVER only above 0 dBFS - matches REAPER's clip indication)
- [x] Locate section buttons (all hw-verified 2026-06-15, fully configurable via `[locate]` in `dm2000_keys.ini`):
      - Row 2 (zone 0x0F): RTZ (sw0, go to project start), END (sw1, go to project end),
        LOOP (sw3, toggle repeat with LED feedback), QUICK PUNCH (sw4, insert marker).
        ONLINE (sw2), SET/REHEARSAL/MTR/MASTER: no HUI output - DM2000 internal only.
      - Row 1 (zone 0x10): IN (sw2, set loop in-point), OUT (sw3, set loop out-point),
        POST (sw4, insert region from time selection). AUDITION/PRE: no default action.
      - LOCATE MEMORY 1-8 (zone 0x13 sw1/sw3/sw6/sw2/sw4/sw7 for LM1-6; zone 0x15
        sw0/sw1 for LM7-8): jump to REAPER markers 1-8. Configurable via dm2000_keys.ini.
- [x] BACK button (zone 0x08 sw2) -> undo; FORWARD (zone 0x08 sw6) -> redo.
- [x] ENTER button (zone 0x14 sw0) cycles cursor arrows through three modes: scroll,
      zoom, and bank-scroll (left/right shift the fader bank offset by one channel).
- [x] REW/FF (zone 0x0E sw1/sw2) support auto-repeat when held (400 ms delay, 80 ms interval), same as cursor arrows.
- [x] AUTO button (per-channel, zone 0-7 sw4) resets that channel's fader to 0 dB.
- [x] Scrub wheel speed increased (10x); jog wheel unchanged.
- [x] Scene recall: DM2000 scenes 1-99 jump to REAPER markers 1-99 via Program
      Change on the GENERAL port (0-indexed wire format per manual p.370, unverified)
- [x] LED counter display: position sent every 100ms as HUI SysEx. Protocol decoded
      2026-06-15 from Pro Tools loopMIDI capture - delta BCD update, bytes right-to-left,
      each byte `(sep_flag<<4)|digit`. Follows REAPER's transport display format setting
      (right-click transport). Hardware-verified 2026-06-15.

Full button/zone/MIDI reference: [DESIGN.md - Button / function / MIDI reference table](DESIGN.md#button--function--midi-reference-table).

### DM2000-specific behavior (hardware-verified - details in DESIGN.md)

- **Fader taper is calibrated to the console's printed scale**: REAPER dB and
  the printed marks agree along the throw (11-point piecewise-linear table from
  DM2000 Editor captures, post-fader-calibration, 2026-06-15).
- **The console's fader physical maximum is the printed +10 mark** (wire value
  16383). REAPER volumes above +10 dB clamp to that position.
- The console keeps an internal model of DAW fader positions and springs motors
  back to it on release; the plugin echoes every received fader move to keep that
  model in sync.
- On close, the plugin drives all faders to -inf, then clears meters, LEDs, pan rings, and scribbles.

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

- **Port 4 (MCS PANNER) not implemented** - port 4 is opened and keepalive-echoed
  but the MCS PANNER protocol (surround joystick) is not yet implemented.
- **macOS hardware-verified** - universal (arm64 + x86_64) build verified with a
  connected DM2000 on 2026-06-15 (v0.3). See [MACOS_BUILD.md](MACOS_BUILD.md)
  for build instructions.
- **Scribble strips show 4 characters only** - hardware-verified; the display is
  4-char wide in DAW mode regardless of what native SysEx sends.
- **Scene recall partial** - PC receive is implemented (DM2000 scene recall sends PC on the
  GENERAL port → REAPER jumps to the matching marker); user must set GENERAL to a DAW USB
  port. PC send and SysEx scene dump not yet implemented.

## License

LGPL v3. See [LICENSE](LICENSE).

## Contact

Bartłomiej Mróz · bartlomiej.mroz@pg.edu.pl · Department of Multimedia Systems, Gdańsk University of Technology · [bmroz.eu](https://bmroz.eu)
