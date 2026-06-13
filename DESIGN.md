# Yamaha DM2000 + REAPER csurf DLL - Design Document

## Project overview

A native REAPER control surface DLL (`reaper_csurf_dm2000`) that gives full
bidirectional integration between the Yamaha DM2000 digital console and REAPER.
The DLL speaks HUI on USB ports 1–4 (24 faders, transport, mute/solo/rec-arm,
automation modes) and Yamaha native SysEx on USB port 8 (scribble strips,
meter bridge, scene recall). No existing tool does this; this is the first
open-source DM2000 csurf for REAPER.

Version: v0.1
Homepage: bmroz.eu/projects/dm2000-csurf
Repository: `git.pg.edu.pl/p829296` / `github.com/mormegil6`
License: LGPL v3
Build target: Windows x64 DLL, REAPER 6+

---

## Hardware context

- Console: Yamaha DM2000 V2, firmware V2.40 (final, no further Yamaha updates)
- Connection: USB (8 virtual MIDI ports), "Pro Tools" Remote Layer target
- DAW port assignment on DM2000: SETUP → MIDI/HOST SETUP → DAW = USB 1–4
  (groups are 1–4, 2–5, 3–6, … four consecutive ports; earlier documentation
  said "3 ports" but hardware testing shows 4)
- Port 8: always carries Yamaha native SysEx regardless of Remote Layer setting
- "DAW Off-line" on DM2000 display = normal; means controls are routed to MIDI

---

## Protocol reference

### HUI (ports 1–4)

HUI is a Mackie/Digidesign 1997 protocol. Each port handles 8 channels.
Port 1 = channels 1–8, Port 2 = channels 9–16, Port 3 = channels 17–24,
Port 4 = channels 25–32 (hardware-verified active; channel-strip purpose on
port 4 not yet mapped - likely master/CR/effects return section).

**Keepalive ping (critical):**
- DM2000 sends: `90 00 7F` on each port every ~1 second
- Host must echo: `90 00 7F` back on the same port output
- If host does not respond within ~2 seconds, DM2000 shows "DAW Off-line"

**Fader position (DM2000 → host):**
Two sequential CC messages, channel 0–7 on that port:
```
B0  ch    vv  - fader MSB (controller 0x00–0x07 = channels 0–7)
B0  20+ch vv  - fader LSB (controller 0x20–0x27); triggers the volume update
```
Value reconstruction: `(MSB << 7) | LSB`, full scale 0–16383 (MSB reaches 0x7F;
LSB only carries bits 5–6, so the ~9-bit effective resolution sits in the top
bits). The dB mapping follows the console's **printed scale**, NOT REAPER's
slider taper - calibrated mark-by-mark (MIDI-OX, 2026-06-12):
| printed mark | wire value |
|---|---|
| +5 (= console max; saturates above) | 16352 |
| 0 | 14112 |
| −30 | 3200 |
| −40 | 2304 |
| −50 | 1056 |
| −∞ (bottom) | 0 |
0→−30 is near-linear (≈364 units/dB); both calibration captures agree within
~0.5 dB. Conversion is piecewise-linear between anchors (`g_taper_db[]` /
`g_taper_val[]` in csurf_dm2000.cpp), used by both directions.

**Fader position (host → DM2000):**
Same controller pair, sent to the correct port (`channel / 8`) and channel within port:
```
B0  ch    msb  - MSB first  (controller 0x00–0x07)
B0  20+ch lsb  - LSB second (controller 0x20–0x27)
```
Both directions use the same calibrated taper table (`volToInt14` is the
inverse of `int14ToVol`). REAPER volumes above +5 dB clamp to 16352 - the
console's value range physically tops out at the printed +5 mark, so REAPER
+5..+12 all park the motor there.

**Echo requirement (hardware-verified 2026-06-12):** the DM2000 keeps an
internal model of the DAW's fader positions and springs the motor back to that
model on touch release. Every fader move received from the console must
therefore be echoed back to it (Pro Tools behaves the same way) - the surface
passes `ignoresurf=NULL` so its own `SetSurfaceVolume` runs for its own moves.

**Shutdown:** HUI has no "goodbye" message - the console flags DAW Off-line by
itself ~2 s after the ping echo stops. On close the DLL drives all faders to
−∞ (wire value 0), then clears meters, LEDs, pan rings, and scribble strips.

**Switch matrix (DM2000 → host):**
All button activity arrives as a two-message pair:
```
B0  0F  zone  - zone select (sets current zone for this port)
B0  2F  vv    - value: bits 0–3 = switch number, bit 6 = press (0x40), 0 = release
```

Zone assignments:
| Zone | Meaning | Switches used |
|------|---------|---------------|
| 0–7  | Channel strip (zone = channel 0–7 on this port) | 0=fader touch/release, 1=SELECT, 2=MUTE, 3=SOLO, 5=pan knob press → center pan, 7=REC/RDY |
| 0x0A | Bank/channel navigation | 0=ch◄, 1=bank◄, 2=ch►, 3=bank► |
| 0x0D | Cursor cluster + wheel modes (hardware-verified) | 0=down, 1=left, 2=INC → next marker, 3=right, 4=up, 5=SCRUB, 6=SHUTTLE |
| 0x0E | Transport | 1=REW, 2=FFWD, 3=STOP, 4=PLAY, 5=REC |
| 0x14 | ENTER (hardware-verified) | 0=press → toggle scroll/zoom |
| 0x1B | DEC (hardware-verified) | 7=press → previous marker |

**Pan v-pots (DM2000 → host):**
Relative deltas, NOT switch-matrix messages:
```
B0  40+ch vv  - bits 0–5 = amount, bit 6 set = clockwise (pan right)
```

**Pan feedback (host → DM2000):**
```
B0  10+ch vv  - LED ring position 1–11
```

**Jog/scrub wheel (DM2000 → host):**
Direct CC, NOT a switch-matrix zone:
```
B0  0D  vv    - bits 0–5 = speed (1–6 observed), bit 6 SET = forward (hardware-verified)
```
Three modes, selected by the SCRUB/SHUTTLE keys (zone 0x0D sw 5/6, LEDs driven,
mutually exclusive): default jog = `MoveEditCursor(speed * ±0.1s)`; SHUTTLE =
coarse `MoveEditCursor(speed * ±1s)`; SCRUB = `CSurf_ScrubAmt(speed * ±0.05)`.
Plain `CSurf_ScrubAmt()` as the only handler did nothing in normal operation
(hardware-verified), hence the edit-cursor default. ENTER (zone 0x14 sw 0)
toggles the cursor arrows between scroll and zoom (`CSurf_OnArrow`).

**Switch/LED feedback (host → DM2000):**
LEDs use a different CC pair to avoid confusion with the incoming zone-select:
```
B0  0C  target  - target select (0–7 = channel strip, 0x0E = transport row)
B0  2C  vv      - value: bits 0–3 = switch number, bit 6 = on (0x40), 0 = off
```

Channel LEDs (switch numbers): 1=SELECT, 2=MUTE, 3=SOLO, 7=REC/RDY
Transport LEDs (target 0x0E, switch numbers): 3=STOP, 4=PLAY, 5=REC

**HUI meter (host → DM2000):**
Polyphonic key pressure, one message per meter side:
```
A0  ch  vv    - ch = strip 0–7; vv: high nibble = side (0=L, 1=R), low nibble = level
```
Signal levels use segments 0x00–0x0B (≈ –60..0 dB). **The DM2000's red OVER
segment is level 0x0C** and it is sent only when the peak is **strictly above**
0 dBFS, matching REAPER's red. Both facts are hardware-verified: sending 0x0C
for hot-but-legal signals lit the red ~2.5 dB early, and the HUI docs' "0x0E =
clip" code is **ignored by the DM2000** (red never lit while clipping). Polled
every 100ms in `Run()`, with a 3-cycle peak hold (max of the last 3 polls) to
keep steady signals from flickering.
(An earlier draft described a `B0 0D / B0 2D` CC pair - refuted: 0x0D is the jog
wheel CC, and HUI metering is poly pressure. If hardware testing shows no meter
movement, capture what Pro Tools sends to the console and adjust.)

### Yamaha native SysEx (port 8)

Port 8 always active regardless of Remote Layer. Used for features HUI cannot reach.

**Parameter change structure** (verified: Owner's Manual MIDI Data Format, 13.4.1, p. 379):
```
F0 43 1n 3E 06  tt ee pp cc  DD...DD  F7
       ^^ ^^    ^^^^^^^^^^^  ^^^^^^^
       |  model=DM2000       data bytes (size depends on parameter)
       device# n (0–15, use 0)
```
- `43` = Yamaha manufacturer ID, `3E` = digital mixer group, `06` = DM2000 model ID
- `tt` = data type (01=edit buffer, 02=patch, 03=setup, 04=backup, 10=function call,
  20=key remote, 21=remote meter, 22=remote time counter, 23=automix status)
- `ee` = element no. (expands to two bytes when 0), `pp` = parameter no., `cc` = channel no.
- **13.4.5 (p. 379): "Consult your dealer for parameter address details."** - the
  edit-buffer element/parameter numbers are NOT published by Yamaha.

**Keepalive observed on port 8:**
`F0 43 17 3E 06 7F F7` - DM2000 sends this ~1/sec. No response required.

**Channel names via SysEx: CAPTURED 2026-06-12 via loopMIDI + Studio Manager.**
Studio Manager sends one 14-byte message per character:
```
F0 43 17 3E 06 02 04 [pos] [ch] 00 00 00 [ASCII] F7
```
- type=0x02, element=0x04 = channel input name
- pos = character index (0–3 confirmed; 0–7 tried in implementation)
- ch = channel 0-indexed (0=ch1 .. 23=ch24)
- data = 4-byte Yamaha 7-bit encoding; last byte is the ASCII code (others 0x00)

Studio Manager used pos=0..3 (4-char names matching HUI); whether the DM2000
scribble strip displays more than 4 characters when pos=4..7 is sent requires
hardware testing - the implementation sends all 8 positions.

Also captured: Studio Manager's Remote Meter subscription request (sent to port 8):
`F0 43 37 3E 06 21 00 [sub] 00 00 18 F7` (sub=0x00 current, sub=0x05 peak; 0x18=24 channels)

**Meter bridge:**
Bulk Dump types 32–34 = Key Remote, Remote Meter, Remote Counter.
Send meter data via parameter-change messages to address block for meters.

**Scene recall:**
`F0 43 10 3E 06 [scene_addr] [scene_number] F7`
Allows REAPER marker positions to trigger DM2000 scene changes.

---

## Implementation layers

### Layer 1 - HUI core

**Status: complete**

Files: `Source\jmde\csurf\csurf_dm2000.cpp`

Tasks:
- [x] Skeleton: class registration, 4-port MIDI open/close, Run() loop
- [x] HUI ping response (`90 00 7F` echo)
- [x] Config dialog: "starting port" dropdown (4 consecutive ports) + separate SysEx output port selector
- [x] Fader touch detect: switch-matrix switch 0 sets touch state; fader CC pair (`B0 ch` / `B0 20+ch`) calls `CSurf_OnVolumeChange()`
- [x] Fader feedback: implement `SetSurfaceVolume()` to send fader position to surface
- [x] Mute buttons: channel zone switch 2, call `CSurf_OnMuteChange()`
- [x] Mute LEDs: implement `SetSurfaceMute()` to update surface LEDs
- [x] Solo buttons: channel zone switch 3
- [x] Rec-arm buttons: channel zone switch 7
- [x] Transport: play/stop/record/rewind/forward
- [x] Bank switching: left/right arrows to shift which 24 tracks are visible
- [x] Automation modes: AUTOMIX section (zone 0x18, 7 buttons) mapped to REAPER global automation override; LED feedback on all 3 HUI ports

**Test criteria for Layer 1:**
- Move fader on DM2000 → REAPER track volume changes
- Move fader in REAPER → DM2000 fader moves
- Press mute on DM2000 → track mutes in REAPER
- Mute in REAPER → LED lights on DM2000
- Play/stop from DM2000 transport → REAPER responds
- All 24 channels accessible via bank switching

### Layer 2 - Channel strip feedback

Tasks:
- [x] VU meters on channel strips via HUI meter messages (100ms poll of `Track_GetPeakInfo()`, –60..0 dB → 0x00–0x0C, 0x0E clip)
- [x] Pan position feedback: `SetSurfacePan()` → HUI pan ring messages (`B0 10+ch`); knob input on `B0 40+ch` relative deltas
- [x] Selected channel highlight
- [x] Track name truncated to 4 chars on scribble strip via HUI (`F0 00 00 66 05 00 10 <ch> <4 chars> F7`, per port)
- [x] Jog/scrub wheel: CC 0x0D, bits 0–5 = speed, bit 6 set = forward; jog/SHUTTLE/SCRUB modes (see protocol section)
- [x] Cursor arrows (scroll/zoom via ENTER toggle), DEC/INC = prev/next marker

**Test criteria for Layer 2:**
- Play audio → DM2000 channel strip meters move
- Pan knob on DM2000 → REAPER pan changes and vice versa

### Layer 3 - Native SysEx extensions

Tasks:
- [x] Full track names via Yamaha SysEx to scribble strips
  - Format captured 2026-06-12 (see "Channel names via SysEx" in protocol section)
  - Implemented in `SendTrackTitle()`: sends pos=0..7 on port 8 alongside HUI 4-char names
  - Hardware test result: display shows 4 chars only - scribble strip is physically 4-char wide in DAW mode; native SysEx updates console memory but HUI controls the visible strip
- [x] Meter bridge - already driven by HUI `A0 ch (side<<4)|level` messages (Layer 2); the DM2000 has no separate channel-strip VU display, so HUI meters ARE the meter bridge. No native SysEx needed.
- [ ] Scene recall: map REAPER markers → DM2000 scenes via SysEx

**Test criteria for Layer 3:**
- Track named "Bass Guitar" shows "Bass Gui" on DM2000 display
- DM2000 scene button triggers REAPER marker jump and vice versa
- Meter bridge shows signal during playback

### Layer 4 - Extended features (future)

- EQ parameter control via selected channel knobs
- Dynamics (compressor/gate) parameter control
- Sends/aux routing
- [ ] Surround panner joystick: requires native Yamaha SysEx on USB port 4 (not HUI); V2 firmware only; implement after Layer 3 is complete
- Talkback button integration
- USER DEFINED KEYS mapping

---

## REAPER API reference (key calls)

```cpp
// Called by REAPER when track volume changes - update surface
void SetSurfaceVolume(MediaTrack *trackid, double volume);

// Called by REAPER when track pan changes
void SetSurfacePan(MediaTrack *trackid, double pan);

// Called by REAPER when mute state changes
void SetSurfaceMute(MediaTrack *trackid, bool mute);

// Called by REAPER when track name changes
void SetTrackTitle(MediaTrack *trackid, const char *title);

// Call these from DM2000 → REAPER direction:
CSurf_OnVolumeChange(track, volume, relative);
CSurf_OnPanChange(track, pan, relative);
CSurf_OnMuteChange(track, mute);
CSurf_OnSoloChange(track, solo);
CSurf_OnRecArmChange(track, recarm);
CSurf_OnPlay();
CSurf_OnStop();
CSurf_OnRecord();
CSurf_OnFwd(seekplay);
CSurf_OnRew(seekplay);

// Track lookup:
CSurf_TrackFromID(id, false);   // id=1 is first track, 0 is master
CSurf_TrackToID(track, false);
CSurf_NumTracks(false);
```

Volume conversion (HUI 14-bit wire value ↔ REAPER linear), as implemented by
`int14ToVol()` / `volToInt14()` in csurf_dm2000.cpp:
```cpp
// wire (0–16383) → REAPER volume
double vol = DB2VAL(SLIDER2DB((double)val14 * 1000.0 / 16383.0));

// REAPER volume → wire (0–16383, clamped)
int val14 = (int)(DB2SLIDER(VAL2DB(vol)) * 16383.0 / 1000.0 + 0.5);
```

---

## Config string format

Stored as: `in1 out1 in2 out2 in3 out3 in4 out4` (8 integers, MIDI device indices, -1=none)

The UI has one control:
- "Starting port" dropdown: selects 4 consecutive HUI port pairs. If user selects port N: in1=N, out1=N, in2=N+1, out2=N+1, in3=N+2, out3=N+2, in4=N+3, out4=N+3

Note: earlier versions stored a 9th value (sysex_out); it is now ignored on load. The SysEx port (m_midiout8) is reserved for scene recall (Layer 3) and will be re-exposed in the UI when that feature is implemented.

---

## Known issues and caveats

- DM2000 uses 4 consecutive HUI ports (hardware-verified; documentation previously said 3); port 4 channels (25–32) are opened and ping-echoed but their purpose on this console is not yet mapped
- Fader taper is CALIBRATED and applied (see protocol section): the console's
  wire value saturates at the printed +5 mark, so REAPER volumes above +5 dB
  all park the motor at +5 - a hardware limit of the DM2000's HUI value range,
  not a bug. (The first calibration capture's provisional table was superseded
  by the explicit mark-by-mark capture of 2026-06-12.)
- HUI fader resolution: ~9-bit (512 steps), coarse below -20dB - by design, not a bug
- Pro Tools target locks USB port 1 on the DM2000 - if port 1 is unavailable, check DM2000 SETUP
- REAPER must not have the DM2000 ports open as regular MIDI devices (disable in REAPER MIDI prefs)
- DLL is locked by REAPER while running - always close REAPER before rebuilding

---

## Known limitations

- **Channels 25–32 (port 4) not mapped**: port 4 is opened and ping-echoed (keeps DM2000 online), but no channel-strip controls on port 4 are wired to REAPER tracks. The DM2000's physical layout for channels 25–32 is not yet determined (likely master bus / effects returns).
- **macOS not supported**: the csurf DLL is Windows x64 only. A macOS port is planned.
- **8-char scribble strip names not achievable**: hardware test confirms the display is 4-char wide in DAW mode. Native SysEx pos=4..7 updates console memory but is not visible.
- **Scene recall not implemented**: SysEx format is a guess; requires a Studio Manager or front-panel capture to confirm addresses before implementing.
- **Joystick / display knobs / dynamics knobs not implemented**: queued for Layer 4.

---

## Build instructions

```powershell
cd C:\dev\reaper_csurf_vs201x
# Close REAPER first
msbuild Builds\VisualStudio2019\reaper_csurf.sln /p:Configuration=Debug /p:Platform=x64
# DLL auto-copies to %APPDATA%\REAPER\UserPlugins\
# Restart REAPER to load new DLL
```

Prerequisites:
- Visual Studio 2019+ Community with "Desktop development with C++" workload
- REAPER Extension SDK at `C:\reaper_extension_sdk\` with env var `REAPER_EXTENSION_SDK`
- msbuild in PATH: `C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\`

---

## How to use this document with Claude Code

At the start of every Claude Code session, paste this prompt:

```
Read DESIGN.md in the project root. This is the authoritative design 
document for the DM2000 csurf project. Before writing any code, confirm 
which Layer 1 tasks are marked [ ] (not done) and tell me what you plan 
to implement next. Then implement the next incomplete task, build, and 
confirm 0 errors. Update the checkbox in DESIGN.md when a task is complete.
```

This ensures every session picks up exactly where the last one left off,
without re-explaining the project from scratch.

---

## Community resources

- HUI protocol PDF: `github.com/openCS18/HUI-to-Mackie-Control-Wrapper`
- DM2000 SysEx: Owner's Manual Appendix C (pp. 369–385)
  `https://jp.yamaha.com/files/download/other_assets/7/334227/dm2000v2_en_om_g0.pdf`
- REAPER SDK: `github.com/justinfrankel/reaper-sdk`
- Scaffold: `github.com/Erriez/reaper_csurf_vs201x`
- CheckCheckOneTwo Yamaha forum: `discourse.checkcheckonetwo.com/c/software/programming-for-the-yamaha-consoles/13`
- REAPER forum DM2000 threads: `forum.cockos.com/showthread.php?t=40080`
