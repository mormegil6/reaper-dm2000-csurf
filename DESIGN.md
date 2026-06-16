# Yamaha DM2000 + REAPER csurf DLL - Design Document

## Project overview

A native REAPER control surface DLL (`reaper_csurf_dm2000`) that gives full
bidirectional integration between the Yamaha DM2000 digital console and REAPER.
The DLL speaks HUI on USB ports 1–4 (24 faders, transport, mute/solo/rec-arm,
automation modes). Port 8 native SysEx (extended names, meter bridge, scene
recall send) is partly stubbed and planned for Layer 3 - not yet active.
No existing tool does this; this is the first open-source DM2000 csurf for REAPER.

Version: v0.6
Homepage: bmroz.eu/projects/dm2000-csurf
Repository: `git.pg.edu.pl/p829296` / `github.com/mormegil6`
License: LGPL v3
Build target: Windows x64 DLL + macOS universal dylib (arm64 + x86_64), REAPER 6+

---

## Hardware context

- Console: Yamaha DM2000 V2, firmware V2.40 (final, no further Yamaha updates)
- Connection: USB (8 virtual MIDI ports), "Pro Tools" Remote Layer target
- DAW port assignment on DM2000: SETUP → MIDI/HOST SETUP → DAW = USB 1–4
  (groups are 1–4, 2–5, 3–6, … four consecutive ports; ports 1–3 carry HUI
  for channels 1–24, port 4 carries MCS PANNER protocol for the surround joystick
  - manual ch.19 p.223)
- Port 8: always carries Yamaha native SysEx regardless of Remote Layer setting
- "DAW Off-line" on DM2000 display = normal; means controls are routed to MIDI

---

## Protocol reference

### HUI (ports 1–4)

HUI is a Mackie/Digidesign 1997 protocol. Each port handles 8 channels.
Port 1 = channels 1–8, Port 2 = channels 9–16, Port 3 = channels 17–24.
Port 4 = MCS PANNER (surround joystick protocol, not HUI channel strips;
confirmed by manual ch.19 p.223 - Pro Tools configures controllers #1–#3 as
HUI and controller #4 as MCS PANNER).

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
slider taper - calibrated via DM2000 Editor after running the console's built-in
fader calibration utility (MIDI-OX, 2026-06-15; doc/fader-calibration-2026-06-15.txt):
| printed mark | wire value |
|---|---|
| +10 (= console max) | 16383 |
| +5 | 14768 |
| 0 | 13168 |
| −5 | 11568 |
| −10 | 9968 |
| −15 | 8368 |
| −20 | 6768 |
| −30 | 5168 |
| −40 | 3568 |
| −50 | 2768 |
| −∞ (bottom) | 0 |
Taper is linear from −30 to +10 dB (1600 wire units per 5 dB); compressed below
−30 dB. Conversion is piecewise-linear between anchors (`g_taper_db[]` /
`g_taper_val[]` in csurf_dm2000.cpp), used by both directions.

**Fader position (host → DM2000):**
Same controller pair, sent to the correct port (`channel / 8`) and channel within port:
```
B0  ch    msb  - MSB first  (controller 0x00–0x07)
B0  20+ch lsb  - LSB second (controller 0x20–0x27)
```
Both directions use the same calibrated taper table (`volToInt14` is the
inverse of `int14ToVol`). REAPER volumes above +10 dB clamp to 16383 - the
console's physical maximum.

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

Zone assignments (all hw-verified from full-surface MIDI-OX capture 2026-06-15; see doc/midi-capture-2026-06-15.txt). Notation: `N=name` means switch number N; `press` means the action fires on the press event (bit 6 of the switch byte set); release events are only handled where noted.

**Channel strips**
| Zone | Meaning | Switches used |
|------|---------|---------------|
| 0-7 | Channel strip (zone = ch 0-7 on this port) | 0=fader touch, 1=SELECT, 2=MUTE, 3=SOLO, 4=AUTOMIX per-ch, 5=pan knob press, 7=REC/RDY |

**Display, navigation & user-defined keys**
| Zone | Meaning | Switches used |
|------|---------|---------------|
| 0x08 | Display History + UDK 4/5/12/13/15/16 | sw2=BACK (undo), sw6=FORWARD (redo); UDK: sw1=UDK4, sw5=UDK5, sw0=UDK12, sw4=UDK13, sw3=UDK15, sw7=UDK16 (dispatch via [udk]; port 0) |
| 0x09 | Display buttons + UDK 1/9 | sw2=UDK1, sw0=UDK9 (sw1 fires alongside UDK9 - ignored); dispatch via [udk], port 0. Remaining sw = DM2000 display selects |
| 0x0A | BANK ◄/►, CH ◄/► = UDK 2/3/10/11 (manual ch.19) | 0=CH◄/UDK10, 1=BANK◄/UDK2, 2=CH►/UDK11, 3=BANK►/UDK3. Navigation by default; an [udk] entry (key2/3/10/11) overrides that button |

**EQ & routing control**
| Zone | Meaning | Switches used |
|------|---------|---------------|
| 0x0B | Aux select + encoder assign | sw7=AUX1, sw6=AUX2, sw5=AUX3, sw4=AUX4, sw3=AUX5; sw2=ENC PAN, sw1=ENC ASSIGN1, sw0=ENC ASSIGN2 |
| 0x0C | Fader mode + AUTOMIX REC + matrix select | sw3=FADER, sw0=FAD ASSIGN2; sw2=AUTOMIX REC; sw1=MATRIX1, sw4=MATRIX2 |

**Cursor, jog & transport**
| Zone | Meaning | Switches used |
|------|---------|---------------|
| 0x0D | Cursor cluster + wheel modes | 0=down, 1=left, 2=INC, 3=right, 4=up, 5=SCRUB, 6=SHUTTLE |
| 0x0E | Transport | 1=REW, 2=FFWD, 3=STOP, 4=PLAY, 5=REC |
| 0x10 | Locate row 1 (hw-verified 2026-06-15) | 0=AUDITION (no action), 1=PRE (no action), 2=IN (set loop in-point), 3=OUT (set loop out-point), 4=POST (insert region from time selection) |

**Locate & markers**
| Zone | Meaning | Switches used |
|------|---------|---------------|
| 0x0F | Locate row 2 (hw-verified 2026-06-15) | 0=RTZ, 1=END, 2=ONLINE (no action), 3=LOOP (toggle repeat), 4=QUICK PUNCH (insert marker); SET/REHEARSAL/MTR/MASTER do not transmit HUI - DM2000 internal only |
| 0x13 | LOCATE MEMORY | sw1=LM1, sw3=LM2, sw6=LM3, sw2=LM4, sw4=LM5, sw7=LM6; sw5=companion event (always fires alongside, ignore) |
| 0x14 | ENTER | 0=press -> cycle cursor arrows: scroll -> zoom -> bank-scroll |
| 0x15 | LOCATE MEMORY 7-8 | sw0=LM7, sw1=LM8 (hw-captured 2026-06-15; fires alongside zone 0x13 sw5) |

**AUTOMIX & overwrite**
| Zone | Meaning | Switches used |
|------|---------|---------------|
| 0x17 | OVERWRITE section | 0=AUX ON, 1=PAN, 2=FADER, 3=AUX, 4=SURROUND, 5=ON |
| 0x18 | AUTOMIX mode buttons | 0=TOUCH SENSE, 1=RETURN/READ, 2=RELATIVE, 4=ABORT/UNDO, 5=AUTO-REC/LATCH |
| 0x19 | AUTOMIX ENABLE + UDK 6/7/8/14 | sw2=ENABLE (toggle bypass/read); sw5=UDK6, sw3=UDK7, sw4=UDK8, sw1=UDK14 (broadcast on all 3 ports, deduped on port 0) |
| 0x1C | EFFECTS/PLUG-INS (F1-F4 below display) - FX parameter editor | F4=sw0 (home: FX slot 0 / page 0), F1=sw1 (next FX slot), F2=sw7 (prev FX slot), F3=sw6 (bypass slot), sw2-sw5=knob 1-4 press (reset that param to 0). The 4 param knobs + 0x4C page arrows are always live (no edit mode) |

**DEC button (separate zone from INC)**
| Zone | Meaning | Switches used |
|------|---------|---------------|
| 0x1B | DEC | 7=press -> previous marker (INC is in zone 0x0D sw2 with the cursor cluster) |

<details>
<summary><strong>Button / function / MIDI reference table</strong> (click to expand)</summary>

"Verified" = confirmed on hardware with MIDI-OX or direct test. "Inferred" = protocol-consistent assignment not yet hardware-confirmed. "Unverified" = software guess; switch number may be wrong.

| DM2000 physical control | Direction | HUI message | REAPER function | Status |
|---|---|---|---|---|
| Channel fader (ch N, port P) | DM2000→host | `B0+(P) 0+(N%8)` MSB then `B0+(P) 20+(N%8)` LSB | `CSurf_OnVolumeChange()` | Verified |
| Channel fader (ch N, port P) | host→DM2000 | same CC pair | `SetSurfaceVolume()` | Verified |
| Fader touch on/off | DM2000→host | zone N%8, sw 0, press/release | sets touch state for automation | Verified |
| MUTE button (ch N) | DM2000→host | zone N%8, sw 2 | `CSurf_OnMuteChange()` | Verified |
| MUTE LED | host→DM2000 | target N%8, sw 2 | `SetSurfaceMute()` | Verified |
| SOLO button (ch N) | DM2000→host | zone N%8, sw 3 | `CSurf_OnSoloChange()` | Verified |
| SOLO LED | host→DM2000 | target N%8, sw 3 | `SetSurfaceSolo()` | Verified |
| REC/RDY button (ch N) | DM2000→host | zone N%8, sw 7 | `CSurf_OnRecArmChange()` | Verified |
| REC/RDY LED | host→DM2000 | target N%8, sw 7 | `SetSurfaceRecArm()` | Verified |
| SELECT button (ch N) | DM2000→host | zone N%8, sw 1 | `SetSurfaceSelected()` | Verified |
| SELECT LED | host→DM2000 | target N%8, sw 1 | `SetSurfaceSelected()` | Verified |
| Pan knob turn (ch N) | DM2000→host | `B0+(P) 40+(N%8) vv` | `CSurf_OnPanChange()` | Verified |
| Pan knob press (ch N) | DM2000→host | zone N%8, sw 5 | center pan | Verified |
| Pan ring LED (ch N) | host→DM2000 | `B0+(P) 10+(N%8) vv` | `SetSurfacePan()` | Verified |
| VU meter (ch N, L/R) | host→DM2000 | `A0+(P) N%8 (side<<4)\|level` | `Track_GetPeakInfo()` poll | Verified |
| CHANNEL ◄ / ► | DM2000→host | zone 0x0A, sw 0/2 | `AdjustBankOffset(∓1)` (shift view by 1 channel) | Verified |
| BANK ◄ / ► | DM2000→host | zone 0x0A, sw 1/3 | bank offset ±24 | Verified |
| PLAY | DM2000→host | zone 0x0E, sw 4 | `CSurf_OnPlay()` | Verified |
| STOP | DM2000→host | zone 0x0E, sw 3 | `CSurf_OnStop()` | Verified |
| REC | DM2000→host | zone 0x0E, sw 5 | `CSurf_OnRecord()` | Verified |
| REW | DM2000→host | zone 0x0E, sw 1 | `CSurf_OnRew()` with auto-repeat (400ms delay, 80ms interval) | Verified |
| FFWD | DM2000→host | zone 0x0E, sw 2 | `CSurf_OnFwd()` with auto-repeat (400ms delay, 80ms interval) | Verified |
| AUDITION | DM2000→host | zone 0x10, sw 0 | no action (ini: `audition`) | Verified |
| PRE | DM2000→host | zone 0x10, sw 1 | no action (ini: `pre`) | Verified |
| IN | DM2000→host | zone 0x10, sw 2 | set loop in-point, default 40222 (ini: `in`) | Verified |
| OUT | DM2000→host | zone 0x10, sw 3 | set loop out-point, default 40223 (ini: `out`) | Verified |
| POST | DM2000→host | zone 0x10, sw 4 | insert region, default 40174 (ini: `post`) | Verified |
| RTZ (RETURN TO ZERO) | DM2000→host | zone 0x0F, sw 0 | `CSurf_GoStart()` (ini: `rtz`, 0=API) | Verified |
| END | DM2000→host | zone 0x0F, sw 1 | `CSurf_GoEnd()` (ini: `end`, 0=API) | Verified |
| ONLINE | DM2000→host | zone 0x0F, sw 2 | no action (ini: `online`) | Verified |
| LOOP | DM2000→host | zone 0x0F, sw 3 | toggle repeat, default 1068 (ini: `loop`) | Verified |
| LOOP LED | host→DM2000 | target 0x0F, sw 3 | `SetRepeatState()` | Verified |
| QUICK PUNCH | DM2000→host | zone 0x0F, sw 4 | insert marker, default 40157 (ini: `qpunch`) | Verified |
| SET | DM2000→host | — | does not transmit HUI - DM2000 internal only | Not mappable |
| REHEARSAL | DM2000→host | — | does not transmit HUI | Not mappable |
| MTR | DM2000→host | — | does not transmit HUI | Not mappable |
| MASTER | DM2000→host | — | does not transmit HUI | Not mappable |
| PLAY LED | host→DM2000 | target 0x0E, sw 4 | `SetPlayState()` | Verified |
| STOP LED | host→DM2000 | target 0x0E, sw 3 | `SetPlayState()` | Verified |
| REC LED | host→DM2000 | target 0x0E, sw 5 | `SetPlayState()` | Verified |
| Jog wheel | DM2000→host | `B0 0D vv` | `MoveEditCursor()` (default jog) | Verified |
| SCRUB key | DM2000→host | zone 0x0D, sw 5 | switch to scrub mode | Verified |
| SHUTTLE key | DM2000→host | zone 0x0D, sw 6 | switch to shuttle mode | Verified |
| Cursor UP/DOWN/LEFT/RIGHT | DM2000→host | zone 0x0D, sw 4/0/1/3 | `CSurf_OnArrow()` scroll | Verified |
| INC | DM2000→host | zone 0x0D, sw 2 | next marker | Verified |
| DEC | DM2000→host | zone 0x1B, sw 7 | previous marker | Verified |
| ENTER | DM2000→host | zone 0x14, sw 0 | cycle cursor arrows: scroll -> zoom -> bank-scroll (m_arrow_mode) | Verified |
| Display Hist BACK | DM2000→host | zone 0x08, sw 2 | undo (action 40029) | Verified |
| Display Hist FORWARD | DM2000→host | zone 0x08, sw 6 | redo (action 40030) | Verified |
| LOCATE MEMORY 1 | DM2000→host | zone 0x13, sw 1 (+sw5 companion) | jump to REAPER marker 1 | Verified |
| LOCATE MEMORY 2 | DM2000→host | zone 0x13, sw 3 (+sw5 companion) | jump to REAPER marker 2 | Verified |
| LOCATE MEMORY 3 | DM2000→host | zone 0x13, sw 6 (+sw5 companion) | jump to REAPER marker 3 | Verified |
| LOCATE MEMORY 4 | DM2000→host | zone 0x13, sw 2 (+sw5 companion) | jump to REAPER marker 4 | Verified |
| LOCATE MEMORY 5 | DM2000→host | zone 0x13, sw 4 (+sw5 companion) | jump to REAPER marker 5 | Verified |
| LOCATE MEMORY 6 | DM2000→host | zone 0x13, sw 7 (+sw5 companion) | jump to REAPER marker 6 | Verified |
| AUTOMIX ENABLE | DM2000→host | zone 0x19, sw 2 (all 3 ports) | toggle bypass/read | Verified |
| AUTOMIX RETURN | DM2000→host | zone 0x18, sw 1 (all 3 ports) | global automation = Read | Verified |
| AUTOMIX TOUCH SENSE | DM2000→host | zone 0x18, sw 0 (all 3 ports) | global automation = Touch | Verified |
| AUTOMIX REC (WRITE) | DM2000→host | zone 0x0C, sw 2 (port 1 only) | global automation = Write | Verified |
| AUTOMIX AUTO-REC | DM2000→host | zone 0x18, sw 5 (all 3 ports) | global automation = Latch | Verified |
| AUTOMIX RELATIVE | DM2000→host | zone 0x18, sw 2 (all 3 ports) | global automation = Latch Preview | Verified |
| AUTOMIX ABORT/UNDO | DM2000→host | zone 0x18, sw 4 (all 3 ports) | undo (`IDC_EDIT_UNDO`) | Verified |
| Scribble strip (ch N) | host→DM2000 | `F0 00 00 66 05 00 10 N <4 chars> F7` (port P) | `SetTrackTitle()` 4-char | Verified |
| LED counter display | host→DM2000 | see **HUI counter display protocol** section below | position from `format_timestr_pos()` following REAPER transport format | Verified 2026-06-15 |
| USER DEFINED KEYS (UDK 1-16) | DM2000→host | all 16 zones hw-verified 2026-06-16 (full in-order capture) - see zone table | custom action via `[udk]` in dm2000_keys.ini (`Main_OnCommand`) | Verified |
| Dynamics knobs (THRESHOLD/ATTACK/DECAY/RANGE/HOLD) | DM2000→host (port 4) | `BE 10..14 vv` (relative signed) | `[surround]` plugin param on selected track (relative nudge) | Verified |
| Surround joystick X / Y | DM2000→host (port 4) | `BE 02 / BE 03 vv` (absolute 0-127) | `[surround]` param_lr / param_front (absolute; Y inverted) | Verified |
| ROUTING buttons 1-8 / Direct | DM2000→host (port 4) | `BE 00 N` + `BE 01 N` (N in 2-column order; Direct = 8) | `[surround]` object select (1-8 within the current bank); Direct shifts the bank of 8 (single press up, double down) | Verified |
| Parameter knobs 1-4 (below display) | DM2000→host | `B0 48..4B vv` (relative signed) | nudge current FX-slot param, always-on (`TrackFX_SetParam`, step 0.001); press = reset param to 0 | Verified |
| Page arrows (below display) | DM2000→host | `B0 4C vv` (up=`0x0A` / down=`0x4A`) | FX param page: up=next, down=prev, wraps; fires twice/press (250ms debounce) | Verified |
| EFFECTS/PLUG-INS (F1-F4) | DM2000→host | zone 0x1C sw0/1/6/7 | home / next FX slot / bypass / prev FX slot | Verified |

**Pan ring LED values:** 1=hard left, 6=centre, 11=hard right. `B0+(P) 10+(N%8) 0` = ring off.

**Zone numbering:** zone = channel number within the port (0–7); port P = 0–3 (ports 1–4). Channel strip controls on port P channel C address global channel `P*8 + C` (0–31).

</details>

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
coarse `MoveEditCursor(speed * ±1s)`; SCRUB = `CSurf_ScrubAmt(speed * ±0.5)`.
Plain `CSurf_ScrubAmt()` as the only handler did nothing in normal operation
(hardware-verified), hence the edit-cursor default. ENTER (zone 0x14 sw 0) cycles the cursor arrow keys: scroll -> zoom -> bank-scroll
(mode 2 calls `AdjustBankOffset` + `SetMixerScroll` to keep the REAPER mixer view
in sync with the DM2000 bank). REW/FF (zone 0x0E sw1/sw2) support auto-repeat:
first repeat after 400 ms, then every 80 ms. Locate row 1 (zone 0x10) and row 2
(zone 0x0F) buttons are fully configurable via dm2000_keys.ini; see doc/dm2000_keys_guide.md.

**MCS PANNER surround control (DM2000 → host, port 4, status `0xBE` = ch 15):**
In Pro Tools mode the DYNAMICS section and the surround joystick send MCS PANNER CC
on port 4. They drive the `[surround]` plugin on the selected track (compiled default: none;
the example ini targets ReaSurroundPan); nothing happens if that plugin is not present (it
is never auto-inserted).
- **Dynamics knobs** `BE 10..14 vv` - **7-bit signed relative** (`0x01..0x3F = +n`,
  `0x40..0x7F = -(0x80-n)`, so `0x7F = -1`). THRESHOLD/ATTACK/DECAY/RANGE/HOLD →
  `param_front`/`param_rear`/`param_lr`/`param_lfe`/`param_vol` (one step = 0.01 of range).
- **Joystick** `BE 02 vv` (X) and `BE 03 vv` (Y) - **absolute** 0-127 → param_lr / param_front.
- **ROUTING buttons 1-8** `BE 00 N` + `BE 01 N` (N arrives in a 2-column order:
  buttons 1/3/5/7 -> N 0/1/2/3, buttons 2/4/6/8 -> N 4/5/6/7) select **object 1-8**
  within the current bank. Each object adds `(object-1) * stride` to every `param_*`.
- **Direct** (`N = 8`) shifts the bank of 8: a single press = up (objects 9-16, 17-24...),
  a double press (within 400ms) = down. No wrap (sized for Atmos-scale object counts).
The plugin name, the 5 param indices, and `stride` (params per object) are ini-configurable;
the object-count top clamp auto-detects at runtime from the plugin's `in N` inputs (so it
adapts as you add objects), with `objects` as an optional manual override. UDK 16 with no
`[udk]` mapping dumps the selected FX's
full parameter list to the REAPER console.
- **Routing LEDs** (host → DM2000, port 4): the selected object's button lights via
  `BE 00 N` (on) / `BE 01 N` (off), N in the same 2-column wire order. Updated on every
  object/bank change and cleared on close. No port 8 needed.

**Generic FX parameter editor (DM2000 → host, port 1):**
The EFFECTS/PLUG-INS section (zone 0x1C) plus the four parameter knobs and the up/down
page arrows below the display edit any FX on the selected track:
- **Parameter knobs 1-4** `B0 48..4B vv` (7-bit signed relative) - nudge params
  `page*4 + 0..3` of the current FX slot (one step = 0.001 of the normalized range);
  pressing a knob (zone 0x1C sw2-sw5) resets its param to 0.
- **Page arrows** `B0 4C vv` (up = `0x0A`, down = `0x4A`) - the up/down arrows below the
  display scroll the 4-param page (up = next, down = previous, **wrapping** at both ends).
  Each press emits the CC **twice** (~145 ms apart, hw-verified), so a 250 ms debounce
  collapses a burst into one page step.
The knobs and arrows are **always live** (no edit mode to enter) - they act on the selected
track's current FX slot. The F1-F4 buttons (zone 0x1C) just steer which FX: **F4** (sw0) homes
to slot 0 / page 0 and reports; **F1** (sw1) next FX slot; **F2** (sw7) previous FX slot;
**F3** (sw6) toggles
`TrackFX_SetEnabled`; the knob presses (sw2-sw5) reset their param to 0. Current slot/page
are logged to the console while mapping.

**Switch/LED feedback (host → DM2000):**
LEDs use a different CC pair to avoid confusion with the incoming zone-select:
```
B0  0C  target  - target select (0–7 = channel strip, 0x0E = transport row)
B0  2C  vv      - value: bits 0–3 = switch number, bit 6 = on (0x40), 0 = off
```

Channel LEDs (switch numbers): 1=SELECT, 2=MUTE, 3=SOLO, 7=REC/RDY
Transport LEDs (target 0x0E, switch numbers): 3=STOP, 4=PLAY, 5=REC. LOOP LED is at target 0x0F sw3 (wired via SetRepeatState), not on transport row.

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

**HUI counter display protocol (host → DM2000, port 1):**

Decoded 2026-06-15 from Pro Tools via loopMIDI sniff on port 1. Hardware-verified on DM2000.

The display has 8 digit positions (0 = leftmost). Separators (`:` and `.`) are encoded as
a flag on the digit to their left.

**Byte encoding:** `(sep_flag << 4) | bcd_digit`
- `bcd_digit` = 0x00–0x09 (the digit value 0–9)
- `sep_flag` = 1 if a separator (`:` or `.`) appears after this digit in the display, 0 otherwise

**Clear / blank all 8 positions:**
```
F0 00 00 66 05 00 11  20 20 20 20 20 20 20 20  F7   (16 bytes, 8 x 0x20)
```
Sent once at plugin load and on close. 0x20 is a special "blank" code distinct from digit 0 (0x00).

**Position update (delta, during playback):**
```
F0 00 00 66 05 00 11  [N bytes]  F7   (N = 1–8)
```
Bytes cover only the positions that changed since the last message, sent **right-to-left**
(position 7 first, then 6, ..., down to the leftmost changed position).

**Example - display showing `1:23.626`:**
Positions 2–7: `1[:]`, `2`, `3[.]`, `6`, `2`, `6`
Encoded: `0x11 0x02 0x13 0x06 0x02 0x06`
Sent right-to-left (pos 7→2): `06 02 06 13 02 11`
Full message: `F0 00 00 66 05 00 11 06 02 06 13 02 11 F7`

If only the frame/sub-second digits changed (positions 5–7), only 3 bytes follow the header.

**Implementation note:** the display format (SMPTE, minutes:seconds, measures:beats, etc.)
is determined by `projectconfig_var_addr(NULL, __g_projectconfig_timemode2)`, with fallback
to `__g_projectconfig_timemode` - matching csurf_mcu.cpp behaviour. Refreshed on its own timer,
decoupled from the 100ms meter poll: `[counter] refresh_ms` in dm2000_keys.ini (default 33 ms
≈ 30 Hz, clamped 20-1000) drives smooth SMPTE frame counting. Only digit positions that changed
since the last message are transmitted, so a fast refresh adds negligible MIDI traffic and is
silent when the transport is stopped.

**Verified captures at known playback positions:**
| Pro Tools display | Data bytes (after `F0 00 00 66 05 00 11`) |
|---|---|
| `0:12.458` | `08 05 04 12 01` (positions 7→3; `0:` in pos 2 unchanged) |
| `0:51.882` | `02 08 08 11 05` |
| `1:23.626` | `06 02 06 13 02 11` (6 bytes; minute digit changed) |

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

### GENERAL MIDI port (Program Change / CC)

The DM2000 has a separate GENERAL MIDI port (configurable: `SETUP → MIDI/HOST SETUP → GENERAL`).
It carries Program Change, CC (EQ ATT, faders, pans), and NRPN independently of the DAW/HUI ports.

**Program Change - scene recall (Owner's Manual ch.18, p.215):**
Bidirectional: DM2000 sends PC on scene change, and responds to incoming PC to recall a scene.
Default table (manual p.218): scene N sends PC N (1-indexed - PC 1 = scene 1, PC 2 = scene 2, etc.).
Both directions use the same mapping.

Current implementation: PC receive on any of the 4 DAW HUI ports is handled (scenes 1-99 jump to
REAPER markers 1-99). The user must set GENERAL to one of the 4 DAW USB ports on the console
for this to work. PC send (REAPER → DM2000) and full scene dump via SysEx are not yet implemented.

**CC table (Owner's Manual Appendix C, pp.353-368):**
16 MIDI channels, CC 0-119 per channel. Ch1-4 = input faders/mute/pan for inputs 1-96;
Ch5-8 = EQ ATT/EQ ON; Ch9-16 = surround parameters. Controllable via GENERAL port.
NRPN: parameter number on 62h/63h, data on 06h/26h.

**Remote Meter SysEx type 0x21 (Owner's Manual Appendix C):**
The DM2000 can push peak meter data to the host on port 8. Studio Manager subscription message
captured: `F0 43 37 3E 06 21 00 [sub] 00 00 18 F7` (sub=0x00 current, sub=0x05 peak; 0x18=24 ch).
Not yet implemented on the receive side.

---

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
- [x] Config dialog: "starting port" dropdown (4 consecutive ports) + ini path field + folder/edit buttons
- [x] Fader touch detect: switch-matrix switch 0 sets touch state; fader CC pair (`B0 ch` / `B0 20+ch`) calls `CSurf_OnVolumeChange()`
- [x] Fader feedback: implement `SetSurfaceVolume()` to send fader position to surface
- [x] Mute buttons: channel zone switch 2, call `CSurf_OnMuteChange()`
- [x] Mute LEDs: implement `SetSurfaceMute()` to update surface LEDs
- [x] Solo buttons: channel zone switch 3
- [x] Rec-arm buttons: channel zone switch 7
- [x] Transport: play/stop/record/rewind/forward with LEDs
- [x] Transport extensions: RTZ (zone 0x0F sw0), END (zone 0x0F sw1), LOOP (zone 0x0F sw3);
      all hw-verified 2026-06-15. `SetRepeatState` wired to LOOP LED (target 0x0F sw3).
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
- [x] VU meters on channel strips via HUI meter messages (100ms poll of `Track_GetPeakInfo()`, –60..0 dB → 0x00–0x0B; 0x0C = red OVER, sent only above 0 dBFS; the HUI 0x0E clip code is ignored by the DM2000)
- [x] Pan position feedback: `SetSurfacePan()` → HUI pan ring messages (`B0 10+ch`); knob input on `B0 40+ch` relative deltas
- [x] Selected channel highlight
- [x] Track name truncated to 4 chars on scribble strip via HUI (`F0 00 00 66 05 00 10 <ch> <4 chars> F7`, per port)
- [x] Jog/scrub wheel: CC 0x0D, bits 0–5 = speed, bit 6 set = forward; jog/SHUTTLE/SCRUB modes (see protocol section)
- [x] Cursor arrows (3-mode ENTER: scroll/zoom/bank-scroll+mixer-scroll), DEC/INC = prev/next marker, IN = set loop in-point, OUT = set loop out-point
- [x] LOCATE MEMORY [1-8]: zones 0x13 (LM1-6) and 0x15 (LM7-8); hw-verified 2026-06-15.
      Default action (from dm2000_keys.ini): jump to REAPER markers 1-8. Fully configurable.
- [x] HUI counter (LED timecode) display: delta BCD protocol decoded 2026-06-15 from Pro Tools
      loopMIDI capture (see "HUI counter display protocol" in protocol section). Follows REAPER
      transport display format via `projectconfig_var_addr` timemode2/timemode. Hw-verified.

**Test criteria for Layer 2:**
- Play audio → DM2000 channel strip meters move
- Pan knob on DM2000 → REAPER pan changes and vice versa
- Press play → DM2000 LED counter counts up matching REAPER position

### Layer 3 - Native SysEx extensions

Tasks:
- [x] Full track names via Yamaha SysEx to scribble strips
  - Format captured 2026-06-12 (see "Channel names via SysEx" in protocol section)
  - Implemented in `SendTrackTitle()`: sends pos=0..7 on port 8 alongside HUI 4-char names
  - Hardware test result: display shows 4 chars only - scribble strip is physically 4-char wide in DAW mode; native SysEx updates console memory but HUI controls the visible strip
- [x] Meter bridge - already driven by HUI `A0 ch (side<<4)|level` messages (Layer 2); the DM2000 has no separate channel-strip VU display, so HUI meters ARE the meter bridge. No native SysEx needed.
- [ ] (in progress) Scene recall:
  - [x] PC receive: all 99 scenes (bytes 0x00-0x62) jump to REAPER markers 1-99.
        Wire format is 0-indexed per manual p.370 ("Program number 0-127"):
        byte 0x00 = PC 1 = Scene 1 → marker 1. UNVERIFIED: some Yamaha devices
        use 1-indexed bytes; verify with MIDI-OX on the GENERAL port.
        Requires GENERAL port on the console set to one of the 4 DAW USB ports.
  - [ ] PC send: REAPER marker → send PC to DM2000 to recall matching scene. Needs a 5th
        port or reusing port 4; not yet implemented.
  - [ ] Full SysEx scene dump/restore: address block unconfirmed; needs capture.

**Test criteria for Layer 3:**
- Track named "Bass Guitar" shows "Bass Gui" on DM2000 display
- DM2000 scene recall (via PC on GENERAL port) triggers REAPER marker jump
- Meter bridge shows signal during playback

### Layer 4 - Extended features

- [x] USER DEFINED KEYS dispatch: all 16 UDK buttons (zones hw-verified 2026-06-16 full
      in-order capture) fire `[udk]` action IDs via `Main_OnCommand`. Zone 0x0A buttons
      (UDK 2/3/10/11 = BANK ◄/► and CH ◄/►) keep bank/channel navigation unless overridden
      by an ini entry.
- [x] Surround pan control: USB port 4 MCS PANNER (manual ch.19 p.232), all messages
      hw-verified 2026-06-15/16. The DYNAMICS knobs (`BE 10..14`, relative) and the surround
      joystick (`BE 02/03`, absolute 0-127) drive the `[surround]` plugin on the selected track
      via `TrackFX_SetParam`; no-op when absent (never auto-inserts). The compiled-in default is
      none (like `[locate]`/`[udk]`): the working ReaSurroundPan map (input 1: X=7/Y=8/Z=9/LFE=10/
      gain=6) ships in dm2000_keys.ini.example. The ROUTING buttons 1-8 select which object/input
      the knobs+joystick drive (each adds `(object-1)*stride`), and Direct shifts the bank of 8
      (single press up, double down, no wrap). The object-count top clamp auto-detects from the
      plugin's `in N` inputs (adapts as objects are added; `objects` is an optional override);
      plugin name, 5 param indices, and `stride` are ini-configurable. UDK 16 (unmapped) dumps the
      selected FX's param list to the console. The selected object's ROUTING button LED lights via port 4 (`BE 00 N` on / `BE 01 N`
      off) - no port 8 needed (the port-8 native-SysEx method could not clear on the Remote layer).
- [x] Generic FX parameter editor (EFFECTS/PLUG-INS zone 0x1C + parameter knobs CC 0x48-0x4B
      + page arrows CC 0x4C, port 1): always-on - the 4 knobs (step 0.001) nudge the selected
      track's current FX-slot page and the up/down arrows scroll pages (up=next, wrapping), no edit
      mode. F1 next FX slot, F2 prev, F3 bypass, F4 home to slot 0, knob press resets its param to
      0. Slot/page logged to console. Hardware-tested 2026-06-16.
- [ ] EQ parameter control via selected-channel knobs; sends/aux routing; talkback.

### macOS port (complete, hardware-verified each release)

Build system: a hand-written SWELL Makefile (`Builds/Make/Makefile`), the convention
Cockos uses for its own REAPER extensions (SWS, ReaPack). NOT CMake - the earlier
DESIGN draft said CMake, but the Makefile is lower-risk, matches the upstream
reaper_csurf scaffold this descends from, and leaves the Visual Studio Windows
build untouched. See `MACOS_BUILD.md` for the full build/verify procedure.

Model: REAPER hosts SWELL. The extension compiles against SWELL's Win32-shaped
headers (`swell.h`, pulled in by `reaper_plugin.h` on non-Windows) and links exactly
ONE SWELL source - `swell-modstub.mm` with `-DSWELL_PROVIDED_BY_APP` - which binds
every SWELL call to REAPER's own implementation at load time. The full SWELL library
is NOT built.

Universal binary: `clang++ -arch x86_64 -arch arm64 -mmacosx-version-min=10.9`
(single fat dylib; the arm64 11.0 floor is enforced only at runtime on Apple Silicon,
which is always >= 11.0). For oldest-Intel coverage the two arches can instead be
built separately (x86_64 @ 10.9, arm64 @ 11.0) and joined with `lipo -create`.

Resource generation: `php swell_resgen.php res.rc` transpiles the Win32 `.rc` into
`res.rc_mac_dlg` / `res.rc_mac_menu`, already `#include`d in csurf_main.cpp under
`#ifndef _WIN32`. PHP is a build prerequisite on the Mac.

Windows-only code made portable (all behind `#ifdef _WIN32`, Windows path unchanged):
- `<shellapi.h>/<shlobj.h>/<commctrl.h>` includes guarded; SWELL supplies the rest
- ini path: `GetResourcePath` (resolved via `rec->GetFunc`, non-fatal) replaces the
  `GetModuleHandle("reaper.exe")` trick on macOS (SWELL has no GetModuleHandle)
- folder/URL open: SWELL's own `ShellExecute` (it routes to NSWorkspace)
- `WM_CTLCOLOREDIT`/`WHITE_BRUSH` whitening guarded out (undefined in SWELL; Mac
  edit fields are white by default)
- SysLink hyperlink: SWELL has no SysLink, so the footer becomes an `SS_NOTIFY`
  clickable static on macOS, routed through `WM_COMMAND` to `ShellExecute`; the
  genuine SysLink + `NMLINK` handler stays under `#ifdef _WIN32`
- `sprintf_s`/`strcpy_s`/`SetDlgItemTextA`/`GetDlgItemTextA` shimmed in
  `dm2000_compat.h` (empty on Windows); `lstrcpyn` is provided by SWELL

WDL is not vendored; the Mac build expects it cloned to `Source/WDL` so the existing
relative includes resolve. Not referenced by the VS project, so Windows is unaffected.

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
CSurf_GoStart();   // RTZ
CSurf_GoEnd();     // go to end of project
// ID_GOTO_MARKER1 = 40161: SendMessage(g_hwnd, WM_COMMAND, ID_GOTO_MARKER1 + n, 0)  (n=0 → marker 1)
// IDC_REPEAT = 1068: SendMessage(g_hwnd, WM_COMMAND, IDC_REPEAT, 0)  (toggle loop)

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

- Port 4 is MCS PANNER (surround joystick protocol), not a 4th HUI channel block - surround pan (joystick + dynamics knobs + ROUTING [6]) is implemented (Layer 4); the remaining MCS PANNER controls are not
- Fader taper is CALIBRATED and applied (see protocol section): physical maximum
  is the printed +10 mark (wire 16383); REAPER volumes above +10 dB clamp there.
  Table recalibrated 2026-06-15 after running the DM2000's built-in fader
  calibration utility, which fixed a systematic offset in the −20 to −5 dB range.
- HUI fader resolution: ~9-bit (512 steps), coarse below -20dB - by design, not a bug
- Pro Tools target locks USB port 1 on the DM2000 - if port 1 is unavailable, check DM2000 SETUP
- REAPER must not have the DM2000 ports open as regular MIDI devices (disable in REAPER MIDI prefs)
- DLL is locked by REAPER while running - always close REAPER before rebuilding

---

## Known limitations

- **Port 4 (MCS PANNER) partial**: surround pan (joystick `BE 02/03`, dynamics knobs `BE 10..14`) drives the `[surround]` plugin on the selected track; ROUTING buttons 1-8 (`BE 00/01 N`) select the object (with LED feedback on the selected button), Direct shifts the bank (Layer 4). The remaining MCS PANNER routing/assign modes are not implemented.
- **macOS port**: cross-platform SWELL build, hardware-verified on macOS every release
  alongside Windows. Universal arm64+x86_64 dylib via SWELL Makefile (`Builds/Make/Makefile`).
  See `MACOS_BUILD.md`.
- **8-char scribble strip names not achievable**: hardware test confirms the display is 4-char wide in DAW mode. Native SysEx pos=4..7 updates console memory but is not visible.
- **Scene recall partial**: PC receive implemented (scenes 1-99 → REAPER markers 1-99); wire
  indexing (byte 0x00 → marker 1) is 0-indexed per the code, still unverified on hardware (see
  TODO). User must set GENERAL port to a DAW USB port on the console. PC send and SysEx
  scene dump not yet implemented; SysEx address unconfirmed.
- **EQ / send / talkback control not implemented**: queued for Layer 4. (Surround joystick,
  display parameter knobs, and dynamics knobs are implemented in v0.6.)

---

## Build instructions

```powershell
cd C:\dev\reaper_csurf_vs201x
# Close REAPER first
msbuild Builds\VisualStudio2019\reaper_csurf.sln /p:Configuration=Debug /p:Platform=x64
# DLL auto-copies to %APPDATA%\REAPER\UserPlugins\reaper_csurf_dm2000_x64.dll
# dm2000_keys.ini.example copied to %APPDATA%\REAPER\dm2000_keys.ini if not already present
# Restart REAPER to load new DLL
```

Prerequisites:
- Visual Studio 2019+ Community with "Desktop development with C++" workload
- REAPER Extension SDK at `C:\reaper_extension_sdk\` with env var `REAPER_EXTENSION_SDK`
- msbuild in PATH: `C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\`

---

## Community resources

- HUI protocol PDF: `github.com/openCS18/HUI-to-Mackie-Control-Wrapper`
- DM2000 Owner's Manual V2: [jp.yamaha.com - DM2000V2 EN OM](https://jp.yamaha.com/files/download/other_assets/7/334227/dm2000v2_en_om_g0.pdf) - HUI/MIDI ch.18, SysEx appendix C (pp. 369–385)
- REAPER SDK: `github.com/justinfrankel/reaper-sdk`
- Scaffold: `github.com/Erriez/reaper_csurf_vs201x`
- CheckCheckOneTwo Yamaha forum: `discourse.checkcheckonetwo.com/c/software/programming-for-the-yamaha-consoles/13`
- REAPER forum DM2000 threads: `forum.cockos.com/showthread.php?t=40080`
