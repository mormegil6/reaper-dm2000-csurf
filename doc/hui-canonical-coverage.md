# Canonical HUI protocol — coverage notes

How this plugin's DM2000 implementation lines up against the canonical
reverse-engineered **Mackie HUI** MIDI protocol. Useful for spotting features the
HUI defines that we don't use yet, and for confirming that the DM2000's
hardware-captured behaviour matches "standard" HUI.

## Source & credit

The canonical reference is **"Mackie HUI MIDI protocol"** — a 2-day
reverse-engineering session by **theageman ("AgeMan")**, dated 2010-05-01,
compiled and © 2011 by **SSEI** (ssei-online.de). It circulates as a PDF plus
the text files `HUIZONES.txt`, `HUIREFTX.txt`, `HUIREFRX.txt`, `HUI_CSET.txt`,
`HUICSET2.txt` (announced on a [REAPER forum thread](https://forum.cockos.com/showthread.php?t=101328)).

This file is **our own derived analysis**, not a copy of that document. The five
text files are mirrored — with credit — under [hui-spec/](hui-spec/); the
copyright-noticed PDF is linked, not re-hosted. See
[hui-spec/HUI_SPEC_REFERENCES.md](hui-spec/HUI_SPEC_REFERENCES.md) for sources and
credits.

> Conventions below: ✅ implemented · ❌ not implemented · 🔬 needs a hardware
> test on a DM2000 · ⚪ intentionally ignored / not applicable.

## What the canonical HUI defines

**Host → surface ("transmitting"):** ping, 4-char channel + SELECT-ASSIGN text,
a 2×40 main display, VU meters, timecode display, V-Pot rings, LEDs (zone/port
model), motor faders, relays, click, beep.

**Surface → host ("receiving"):** ping reply, switches (zone-select + port),
V-Pots (delta), jog wheel (delta), faders (touch / move / release), footswitches,
system reset (`ff`).

LEDs/switches use a 29-zone (`0x00`–`0x1d`) × 8-port model; a button and its LED
share the same zone/port. Host lights an LED with `b0 0c <zone>` then
`b0 2c 4<port>`; the surface reports a press with `b0 0f <zone>` then
`b0 2f 4<port>`.

## Coverage matrix

### Host → surface (feedback)

| HUI feature | Wire | Status | Notes |
|---|---|---|---|
| Ping / keep-alive | `90 00 00` → `90 00 7f` | ✅ | echo implemented |
| 4-char channel text | `<hdr> 10 ...` | ✅ | scribble strips |
| SELECT-ASSIGN text | `<hdr> 10 08 ...` | ✅ | encoder-mode readout |
| 2×40 main display | `<hdr> 12 ...` (display SysEx **sub-command** `0x12`) | ✅ 🔬 | this is the REMOTE / INSERT display; we already drive it for the FX editor. Open question is full 2-line / 8-cell addressing & width. Note: unrelated to switch/LED **zone** `0x12` (monitor output) |
| VU meters | `a0 0y sv` | ✅ | our scale matches the doc (−60..0 dB → `0x00`..`0x0b`, `0x0c` = clip) |
| Timecode display | `<hdr> 11 ...` | ✅ | same delta-BCD digit encoding |
| V-Pot rings | `b0 1y vv` | ✅ | pan + send rings |
| LEDs | `b0 0c`/`b0 2c` | ✅ | full zone/port model |
| Motor faders | `b0 0z hi` / `b0 2z lo` | ✅ | 14-bit, calibrated taper |
| Relays | zone `0x1d` p0/p1 | ⚪ | no relay hardware on a DM2000 |
| Click | zone `0x1d` p2 | ❌ 🔬 | low value; test if the desk reacts |
| Beep | zone `0x1d` p3 | ❌ 🔬 | low value; test if the desk reacts |

### Surface → host (control)

| HUI feature | Wire | Status | Notes |
|---|---|---|---|
| Ping reply | `90 00 7f` | ✅ | |
| Switches | `b0 0f`/`b0 2f` | ✅ | all decoded buttons |
| V-Pots (delta) | `b0 4p vv` | ✅ | pan / encoder, `vv>0x40` = +, `<0x40` = − |
| Jog wheel (delta) | `b0 0d vv` | ✅ | same delta encoding |
| Faders touch/move/release | `b0 0f 0z`+`b0 2f 40/00`, `b0 0z hi` | ✅ | touch automation works |
| **Footswitches** | zone `0x1d` p0/p1 | ❌ 🔬 | **test whether the DM2000's foot-switch jacks emit HUI footswitch** — if so, expose as assignable |
| System reset | `ff` | ⚪ | sent on power on/off; harmless to ignore |

## Notable matches & Yamaha deviations

- **Strong validation.** Our independently hardware-captured zones line up with
  the canonical map: encoder-assign row `0x0b` (pan + Sends A–E), `0x0c`
  (suspend/default/mute/bypass/arm), transport `0x0e`, automation modes `0x18`,
  numeric/locate rows, scrub/shuttle `0x0d` p5/p6, counter-mode LEDs `0x16`. The
  VU scale and the timecode digit format match the doc exactly.
- **FADER MODE = canonical "shift" port.** The DM2000's FADER MODE button is
  `0x0c` sw3, which the canonical map labels **shift**. Yamaha repurposed that
  port as the Flip toggle — relevant to the planned FLIP feature, which keys off
  exactly this button.
- **Encoder ASSIGN 1/2** (`0x0b` p0/p1) are the canonical **output/input**
  encoder modes; we reuse them for the scribble "peek" overlays.

## To test next to a DM2000

Worked up as a runnable checklist (using the `tools/` helpers) in
[hui-test-plan.md](hui-test-plan.md). Rough priority order (most useful first):

1. **TALKBACK** (switch zone `0x0e` p0). We handle p1–p5 (REW/FFWD/STOP/PLAY/REC);
   p0 is the one port in that zone we don't use. Does the desk's TALKBACK control
   transmit it? **Highest value** — an immediate hardware talkback hook for
   recording sessions.
2. **2×40 main display** (display SysEx sub-command `0x12`). We already drive this
   for the FX editor — open question is the full 2-line / 8-cell width, i.e. whether
   a general status/parameter readout is worthwhile. (Not the same `0x12` as the
   monitor-output switch zone.)
3. **RUDE SOLO LED** (`0x16` p3). We already drive `0x16` p0–p2 (TIME CODE / FEET /
   BEATS). Does the desk have/light a "rude solo" indicator we can drive whenever
   any track is soloed? Easy win if present.
4. **Footswitch input** (`0x1d` p0/p1). Plug a pedal into the DM2000's foot-switch
   jack(s) and watch for `b0 0f 1d` / `b0 2f 40`. If it transmits, expose as an
   assignable action (like `[udk]`).
5. **Edit zone `0x1a`** (paste/cut/capture/delete/copy/separate in canonical HUI).
   Does any DM2000 button emit `0x1a`? If so, free edit-action hooks.
6. **Monitor sections `0x11` / `0x12`** (input/output monitor rows). Do the desk's
   monitor buttons emit these? Could map to REAPER monitoring.
7. **Non-ASCII scribble characters & timecode separators**, and **power-cycle /
   `0xff` handling** — display-fidelity and resilience checks; see the test plan.
8. **Click / Beep** (`0x1d` p2/p3). Low value — only worth a quick poke.

## Archiving these docs

Resolved: the five **`.txt`** files (which carry author attribution but no
copyright notice, and whose author invited redistribution) are mirrored under
[hui-spec/](hui-spec/), with credit recorded in
[hui-spec/HUI_SPEC_REFERENCES.md](hui-spec/HUI_SPEC_REFERENCES.md). The
**`HUI.pdf`** — the one file with an explicit "© 2011 SSEI" notice, and durably
hosted on the REAPER stash — is linked, not re-hosted. `HUICSET2.txt` is the most
important mirror: its original host is offline and it survives only on the
Internet Archive. Full sources and credits:
[hui-spec/HUI_SPEC_REFERENCES.md](hui-spec/HUI_SPEC_REFERENCES.md).
