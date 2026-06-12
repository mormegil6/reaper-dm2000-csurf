# reaper_csurf_dm2000 — Yamaha DM2000 control surface for REAPER

A native REAPER control surface DLL giving bidirectional integration between
the Yamaha DM2000 digital console and REAPER: 24 touch-sensitive faders, pan
knobs, mute/solo/rec-arm/select buttons with LED feedback, transport, jog
wheel scrub, bank switching, and channel-name scribble strips. The DLL speaks
HUI on the console's USB MIDI ports 1–3, with native Yamaha SysEx on port 8
planned for features HUI cannot reach (8-char names, meter bridge, scene
recall).

To our knowledge this is the first open-source DM2000 control surface for
REAPER.

**License:** The Unlicense (public domain).
**Design document:** see [DESIGN.md](DESIGN.md) for the full protocol
reference, layer plan, and task tracking.

## Hardware requirements

- Yamaha DM2000 **V2** (firmware V2.40) — V1 uses a different DAW port layout
  and is not supported
- USB connection to the PC with the Yamaha USB-MIDI driver (exposes 8 virtual
  MIDI port pairs)
- Console setup:
  - Remote Layer target: **Pro Tools**
  - `SETUP → MIDI/HOST SETUP → DAW = USB 1–3`
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
# Close REAPER first — it locks the DLL and the post-build copy will fail
msbuild Builds\VisualStudio2019\reaper_csurf.sln /p:Configuration=Debug /p:Platform=x64
```

The post-build step copies the DLL to `%APPDATA%\REAPER\UserPlugins\`.
Restart REAPER to load it.

Only the **VisualStudio2019** solution includes `csurf_dm2000.cpp`; the
2013/2015/2017 solutions are left over from the scaffold and will not link.

This project is built on the [Erriez/reaper_csurf_vs201x](https://github.com/Erriez/reaper_csurf_vs201x)
scaffold — see its README/wiki for a step-by-step Visual Studio debugging
setup (attaching to reaper.exe, breakpoints, etc.).

## REAPER configuration

1. Disable the DM2000's MIDI ports as regular MIDI devices in
   `Preferences → Audio → MIDI Devices` (the surface opens them itself;
   conflicts cause erratic behavior).
2. `Preferences → Control/OSC/web → Add → "Yamaha DM2000"`.
3. In the surface settings, pick the **starting port** group — with the
   default console setup that is the entry reading
   `Yamaha DM2000-1 ... Yamaha DM2000-3`. The DLL uses the selected port plus
   the next two (matching `DAW = USB 1–3` on the console), and locates the
   matching output ports by name.
4. OK, then restart REAPER if the DLL was just rebuilt.

## Feature status

### Layer 1 — HUI core: implemented

- 3-port MIDI open/close, keepalive ping echo, config dialog
- Faders both directions, touch detect (touch automation works)
- Mute / solo / rec-arm / select buttons with LED feedback
- Transport (play/stop/record/rewind/forward) with LEDs
- Bank switching (channel ±1, bank ±24)

### Layer 2 — Channel strip feedback: complete (hardware-tested)

- [x] Pan: knob input (relative v-pot deltas), ring-LED feedback, knob press centers pan
- [x] Selected channel highlight
- [x] Jog wheel moves the edit cursor (speed-scaled, 1–6)
- [x] 4-char track names on scribble strips (HUI SysEx)
- [x] VU meters on channel strips (100ms peak polling, 3-cycle peak hold,
      red OVER only above 0 dBFS — matches REAPER's clip indication)

### DM2000-specific behavior (hardware-verified — details in DESIGN.md)

- **Fader taper is calibrated to the console's printed scale**: REAPER dB and
  the printed marks agree along the throw (piecewise-linear table from
  mark-by-mark MIDI captures).
- **The console's fader value range tops out at the printed +5 mark** — REAPER
  volumes from +5 to +12 dB all park the motor at +5. Hardware limit, not a bug.
- The console keeps an internal model of DAW fader positions and springs motors
  back to it on release; the DLL echoes every received fader move to keep that
  model in sync.
- On close, the DLL drives all faders to −∞, then clears meters, LEDs, pan rings, and scribbles.

### Layer 3 — Native SysEx (port 8): scaffolding only

- Port 8 output is opened and a `SendSysEx()` helper exists
- [ ] 8-char track names — **blocked**: Yamaha does not publish the channel-name
      SysEx addresses (manual 13.4.5: "Consult your dealer"); requires capturing
      Studio Manager rename traffic first (see DESIGN.md)
- [ ] Meter bridge
- [ ] Scene recall ↔ REAPER markers

### Layer 4 — future

EQ/dynamics parameter control, sends, surround panner joystick, talkback,
USER DEFINED KEYS. See [DESIGN.md](DESIGN.md).
