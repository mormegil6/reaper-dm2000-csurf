# Yamaha DM2000 + REAPER csurf DLL — Design Document

## Project overview

A native REAPER control surface DLL (`reaper_csurf_dm2000`) that gives full
bidirectional integration between the Yamaha DM2000 digital console and REAPER.
The DLL speaks HUI on USB ports 1–3 (24 faders, transport, mute/solo/rec-arm)
and Yamaha native SysEx on USB port 8 (scribble strips, meter bridge, scene
recall). No existing tool does this; this is the first open-source DM2000
csurf for REAPER.

Repository: `git.pg.edu.pl/p829296` / `github.com/mormegil6`
License: The Unlicense (public domain)
Build target: Windows x64 DLL, REAPER 6+

---

## Hardware context

- Console: Yamaha DM2000 V2, firmware V2.40 (final, no further Yamaha updates)
- Connection: USB (8 virtual MIDI ports), "Pro Tools" Remote Layer target
- DAW port assignment on DM2000: SETUP → MIDI/HOST SETUP → DAW = USB 1–3
  (V2 firmware uses 3 ports; V1 used 4 — not relevant here)
- Port 8: always carries Yamaha native SysEx regardless of Remote Layer setting
- "DAW Off-line" on DM2000 display = normal; means controls are routed to MIDI

---

## Protocol reference

### HUI (ports 1–3)

HUI is a Mackie/Digidesign 1997 protocol. Each port handles 8 channels.
Port 1 = channels 1–8, Port 2 = channels 9–16, Port 3 = channels 17–24.

**Keepalive ping (critical):**
- DM2000 sends: `90 00 7F` on each port every ~1 second
- Host must echo: `90 00 7F` back on the same port output
- If host does not respond within ~2 seconds, DM2000 shows "DAW Off-line"

**Fader position (DM2000 → host):**
Two sequential CC messages, channel 0–7 on that port:
```
B0  ch    vv  — fader MSB (controller 0x00–0x07 = channels 0–7)
B0  20+ch vv  — fader LSB (controller 0x20–0x27); triggers the volume update
```
14-bit value reconstruction: `(MSB << 7) | LSB`, range 0–16383.
Volume conversion (matches REAPER's 0–1000 slider scale):
```cpp
double int14ToVol(unsigned char msb, unsigned char lsb) {
    int val = lsb | (msb << 7);
    return DB2VAL(SLIDER2DB((double)val * 1000.0 / 16383.0));
}
```

**Fader position (host → DM2000):**
Same controller pair, sent to the correct port (`channel / 8`) and channel within port:
```
B0  ch    msb  — MSB first  (controller 0x00–0x07)
B0  20+ch lsb  — LSB second (controller 0x20–0x27)
```

**Switch matrix (DM2000 → host):**
All button activity arrives as a two-message pair:
```
B0  0F  zone  — zone select (sets current zone for this port)
B0  2F  vv    — value: bits 0–3 = switch number, bit 6 = press (0x40), 0 = release
```

Zone assignments:
| Zone | Meaning | Switches used |
|------|---------|---------------|
| 0–7  | Channel strip (zone = channel 0–7 on this port) | 0=fader touch/release, 1=SELECT, 2=MUTE, 3=SOLO, 7=REC/RDY |
| 0x0A | Bank/channel navigation | 0=ch◄, 1=bank◄, 2=ch►, 3=bank► |
| 0x0E | Transport | 1=REW, 2=FFWD, 3=STOP, 4=PLAY, 5=REC |

**Pan v-pots (DM2000 → host):**
Relative deltas, NOT switch-matrix messages:
```
B0  40+ch vv  — bits 0–5 = amount, bit 6 set = clockwise (pan right)
```

**Pan feedback (host → DM2000):**
```
B0  10+ch vv  — LED ring position 1–11
```

**Jog/scrub wheel (DM2000 → host):**
Direct CC, NOT a switch-matrix zone:
```
B0  0D  vv    — bits 0–5 = speed (1–6 observed), bit 6 set = counter-clockwise
```
Handled via `CSurf_ScrubAmt(speed * ±0.05)`.

**Switch/LED feedback (host → DM2000):**
LEDs use a different CC pair to avoid confusion with the incoming zone-select:
```
B0  0C  target  — target select (0–7 = channel strip, 0x0E = transport row)
B0  2C  vv      — value: bits 0–3 = switch number, bit 6 = on (0x40), 0 = off
```

Channel LEDs (switch numbers): 1=SELECT, 2=MUTE, 3=SOLO, 7=REC/RDY
Transport LEDs (target 0x0E, switch numbers): 3=STOP, 4=PLAY, 5=REC

**HUI meter (host → DM2000, optional):**
Sends signal level to channel strip LEDs:
```
B0  0D  cc    — channel select
B0  2D  vv    — level (0x00–0x0C = signal levels, 0x0E = clip)
```

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
- **13.4.5 (p. 379): "Consult your dealer for parameter address details."** — the
  edit-buffer element/parameter numbers are NOT published by Yamaha.

**Keepalive observed on port 8:**
`F0 43 17 3E 06 7F F7` — DM2000 sends this ~1/sec. No response required.

**Channel names via SysEx: NOT documented (verified 2026-06-12).**
The manual's Appendix C (pp. 369–385) contains **no channel-name message**. The only
title-setting SysEx is the Function-call "title" message (13.4.15, p. 381,
`F0 43 1n 3E 7F 10 4f mh ml <16 bytes> F7`), which renames LIBRARY entries
(scene/EQ/channel-library slots), not mixing channels. Channel names presumably
live at unpublished edit-buffer addresses (see 13.4.5 above) or inside opaque
bulk-dump payloads. **Path forward:** capture what Studio Manager sends when
renaming a channel (MIDI-OX on the DM2000 USB ports), then replay that format.
Manual: `https://jp.yamaha.com/files/download/other_assets/7/334227/dm2000v2_en_om_g0.pdf`

**Meter bridge:**
Bulk Dump types 32–34 = Key Remote, Remote Meter, Remote Counter.
Send meter data via parameter-change messages to address block for meters.

**Scene recall:**
`F0 43 10 3E 06 [scene_addr] [scene_number] F7`
Allows REAPER marker positions to trigger DM2000 scene changes.

---

## Implementation layers

### Layer 1 — HUI core (current focus)

**Status: in progress**

Files: `Source\jmde\csurf\csurf_dm2000.cpp`

Tasks:
- [x] Skeleton: class registration, 3-port MIDI open/close, Run() loop
- [x] HUI ping response (`90 00 7F` echo)
- [x] Config dialog: single "starting port" dropdown (consecutive ports)
- [x] Fader touch detect: switch-matrix switch 0 sets touch state; fader CC pair (`B0 ch` / `B0 20+ch`) calls `CSurf_OnVolumeChange()`
- [x] Fader feedback: implement `SetSurfaceVolume()` to send fader position to surface
- [x] Mute buttons: channel zone switch 2, call `CSurf_OnMuteChange()`
- [x] Mute LEDs: implement `SetSurfaceMute()` to update surface LEDs
- [x] Solo buttons: channel zone switch 3
- [x] Rec-arm buttons: channel zone switch 7
- [x] Transport: play/stop/record/rewind/forward
- [x] Bank switching: left/right arrows to shift which 24 tracks are visible

**Test criteria for Layer 1:**
- Move fader on DM2000 → REAPER track volume changes
- Move fader in REAPER → DM2000 fader moves
- Press mute on DM2000 → track mutes in REAPER
- Mute in REAPER → LED lights on DM2000
- Play/stop from DM2000 transport → REAPER responds
- All 24 channels accessible via bank switching

### Layer 2 — Channel strip feedback

Tasks:
- [x] VU meters on channel strips via HUI meter messages (100ms poll of `Track_GetPeakInfo()`, –60..0 dB → 0x00–0x0C, 0x0E clip)
- [x] Pan position feedback: `SetSurfacePan()` → HUI pan ring messages (`B0 10+ch`); knob input on `B0 40+ch` relative deltas
- [x] Selected channel highlight
- [x] Track name truncated to 4 chars on scribble strip via HUI (`F0 00 00 66 05 00 10 <ch> <4 chars> F7`, per port)
- [x] Jog/scrub wheel: CC 0x0D, value 0x01–0x06=clockwise / 0x41–0x46=counter-clockwise (bits 0–5 = speed), calls `CSurf_ScrubAmt()` scaled by speed

**Test criteria for Layer 2:**
- Play audio → DM2000 channel strip meters move
- Pan knob on DM2000 → REAPER pan changes and vice versa

### Layer 3 — Native SysEx extensions

Tasks:
- [ ] Full track names (up to 8 chars) via Yamaha SysEx to scribble strips
  - **BLOCKED:** channel-name addresses are unpublished (manual 13.4.5: "consult
    your dealer"); requires sniffing Studio Manager rename traffic first — see
    "Channel names via SysEx" in the protocol section. Port-8 output and
    `SendSysEx()` are already in place.
- [ ] Meter bridge (top LED array) via SysEx bulk meter messages
- [ ] Scene recall: map REAPER markers → DM2000 scenes via SysEx

**Test criteria for Layer 3:**
- Track named "Bass Guitar" shows "Bass Gui" on DM2000 display
- DM2000 scene button triggers REAPER marker jump and vice versa
- Meter bridge shows signal during playback

### Layer 4 — Extended features (future)

- EQ parameter control via selected channel knobs
- Dynamics (compressor/gate) parameter control
- Sends/aux routing
- [ ] Surround panner joystick: requires native Yamaha SysEx on USB port 4 (not HUI); V2 firmware only; implement after Layer 3 is complete
- Talkback button integration
- USER DEFINED KEYS mapping

---

## REAPER API reference (key calls)

```cpp
// Called by REAPER when track volume changes — update surface
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

Stored as: `in1 out1 in2 out2 in3 out3` (6 integers, MIDI device indices, -1=none)

The UI simplification: one "starting port" dropdown. If user selects port N:
- in1=N, out1=N, in2=N+1, out2=N+1, in3=N+2, out3=N+2

---

## Known issues and caveats

- DM2000 V2 firmware: DAW uses 3 ports (V1 used 4) — only 3-port mode implemented
- HUI fader resolution: ~9-bit (512 steps), coarse below -20dB — by design, not a bug
- Pro Tools target locks USB port 1 on the DM2000 — if port 1 is unavailable, check DM2000 SETUP
- REAPER must not have the DM2000 ports open as regular MIDI devices (disable in REAPER MIDI prefs)
- DLL is locked by REAPER while running — always close REAPER before rebuilding

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

## Community resources

- HUI protocol PDF: `github.com/openCS18/HUI-to-Mackie-Control-Wrapper`
- DM2000 SysEx: Owner's Manual Appendix C (pp. 369–385)
  `https://jp.yamaha.com/files/download/other_assets/7/334227/dm2000v2_en_om_g0.pdf`
- REAPER SDK: `github.com/justinfrankel/reaper-sdk`
- Scaffold: `github.com/Erriez/reaper_csurf_vs201x`
- CheckCheckOneTwo Yamaha forum: `discourse.checkcheckonetwo.com/c/software/programming-for-the-yamaha-consoles/13`
- REAPER forum DM2000 threads: `forum.cockos.com/showthread.php?t=40080`
