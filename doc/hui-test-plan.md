# DM2000 HUI coverage — hardware test plan

A short, working checklist of the open questions the canonical-HUI analysis
([hui-canonical-coverage.md](hui-canonical-coverage.md)) raised — things that can
only be settled at the desk. Run it next time you're at a DM2000, jot the result
under each item, then fold the findings into `hui-canonical-coverage.md` /
`DESIGN.md` / `TODO.md` and trim this file. **This file is temporary scaffolding.**

## Setup

- Close REAPER / Pro Tools / MIDI-OX so the four `Yamaha DM2000-1..4` ports are free.
- Tools need `python-rtmidi` (`pip install python-rtmidi`); they keep the desk
  online on their own (echo the keepalive), so no DAW is required.
- Two tools do all of this:
  - **`python tools/hui_deskmon.py`** — decodes everything the desk *transmits*
    (find out whether a control sends MIDI, and capture its zone/switch).
  - **`python tools/hui_send.py`** — drives any surface element by hand
    (`led <zone> <sw> [on|off]`, plus display/counter/meter aliases; type `help`).

---

## A. "Does the desk send it?" — run `hui_deskmon.py`, then operate controls

- [x] **A1 — TALKBACK** (canonical zone `0x0e` p0). We already handle this zone's
      p1–p5 (REW / FFWD / STOP / PLAY / REC); **p0 is the one port we don't use.**
      Press the desk's TALKBACK control and watch for `0x0e` sw0 (`b0 0f 0e` +
      `b0 2f 40`).
      _Result (2026-06-26):_ **No HUI output — but they DO transmit as native Yamaha
      SysEx on ports 5 & 8** (found via the all-8 sweep, `hui_deskmon.py all`):
      - TALKBACK = `F0 43 10 3E 06 04 15 11 00 00 00 00 VV F7` (VV: 01 press / 00 release)
      - SLATE    = `F0 43 10 3E 06 04 15 12 00 00 00 00 VV F7`
      - a companion `…06 04 15 0D …VV…` fires alongside both
      - (P8 also spams a `F0 43 10 3E 06 7F F7` native heartbeat — ignore.)
      → **Reopened: implementable.** Not via HUI, but the plugin already opens the
      GENERAL port (port 5) for scene recall, and these arrive there too — so it can
      sniff `06 04 15 11/12` and fire a configurable REAPER talkback/slate action.
      (Supersedes the earlier "drop"; that was only true for the DAW layer.)

- [ ] **A2 — Footswitch input** (canonical zone `0x1d` p0/p1). Plug a pedal into the
      DM2000's foot-switch jack(s) and press. Watch for `0x1d` / footswitch
      (`b0 0f 1d` + `b0 2f 40/41`).
      _Result (2026-06-26):_ **Not tested** — no footswitch/pedal on hand; deferred
      until one is available. → if it transmits, expose as an assignable `[foot]` action.

- [x] **A3 — Edit operations** (canonical zone `0x1a` = paste/cut/capture/delete/
      copy/separate). Press the desk's edit-ish buttons that we don't decode yet.
      Watch for any `0x1a` press; note which physical button maps to which switch.
      _Result (2026-06-26):_ **No HUI output.** The only edit buttons on the desk are
      **COPY** and **PASTE** (under the **CHANNEL** label); neither transmits MIDI.
      → **Drop** — no `0x1a` edit-op hooks available from the surface.

- [x] **A4 — Monitor / control-room section** (canonical switch zones `0x11`
      monitor-input, `0x12` monitor-output). Operate the monitor/control-room
      buttons; watch for `0x11` / `0x12` switch presses.
      ⚠️ This switch/LED **zone** `0x12` is unrelated to the display SysEx
      **sub-command** `0x12` in test B2 — two different namespaces that happen to
      share the number `0x12`.
      _Result (2026-06-26):_ **No HUI output.** This is the same rightmost section as
      talkback (A1); no button there reports MIDI. → **Drop.**

- [x] **A5 — Power-cycle / system reset** (`0xff`). With deskmon running, power the
      DM2000 off and on and confirm one or more `0xff` (MIDI System Reset) arrive.
      Then repeat with REAPER + csurf running and confirm the plugin recovers
      gracefully (re-onlines, no stuck LEDs/faders).
      _Result (2026-06-26):_ **No `0xff` on power-down or power-up.** Expected for a
      USB-MIDI console: a power cycle disconnects and re-enumerates the device at the
      USB level, so the monitor's already-open ports go stale rather than receiving a
      clean HUI `0xff` (which is meant for an always-connected HUI controller, not a
      USB re-enumeration). Not a port/channel issue — `0xff` is a channel-less system
      message, and the device itself drops off the bus. → **N/A** for FF detection.
      The meaningful resilience check is power-cycling with REAPER + csurf running:
      the desk shows "DAW Off-line" then re-onlines once csurf answers the keepalive
      (already handled by the keepalive path).

- [ ] **A6 — Completeness sweep + cross-port check.** The DAW HUI layer (ports 1-4)
      is already fully mapped from earlier captures, so a blind sweep just re-shows
      known controls (a full "known-message filter" wasn't worth building). The
      productive check is `python tools/hui_deskmon.py all`, which watches **all 8
      ports** — any traffic on the non-DAW ports 5-8 is outside our mapping. Re-press
      the A1-A4 buttons (talkback / edit / monitor) during this pass: if ports 5-8
      stay silent, that confirms those controls emit nothing on *any* port.
      _Result (2026-06-26):_ all-8 sweep done — see the bonus finding below. The
      rightmost / control-room section is **not** silent: it transmits native Yamaha
      SysEx on ports 5 & 8 (just not HUI). No *unmapped HUI* traffic appeared.

---

## Bonus finding — native control-room SysEx (ports 5 & 8, 2026-06-26)

The all-8 sweep showed the DM2000's **control-room / monitor section** — silent on
the HUI/DAW layer (ports 1-4) — transmitting **Yamaha native parameter-change SysEx**
on ports 5 (GENERAL) and 8. The plugin already opens the GENERAL port for scene
recall, so these are sniffable there with no extra wiring.

Format: `F0 43 10 3E 06 EE AA PP 00 00 00 00 VV F7`
(`43`=Yamaha, `3E`=DM2000, `10`=param-change; `EE AA PP` address the control; `VV`=value.)
Most monitor controls live in element `06 04 15` (so `PP` is the parameter); the SOLO
knob is element `06 03 2D 03`. Source selects within a group are **exclusive radio-buttons**
(selecting one clears the others, hence the paired `01`/`00`). Confirmed on ports 5 & 8.

**STUDIO monitor source** — `06 04 15 PP`, exclusive (`01`=selected / `00`=cleared)

| `PP` | Source |
|------|--------|
| `00` | Control Room |
| `01` | Stereo |
| `02` | Aux 11 |
| `03` | Aux 12 |

**CONTROL ROOM monitor source** — `06 04 15 PP`

| `PP` | Source | Value |
|------|--------|-------|
| `08` | 2TR select | `04`=2TR A1, `05`=2TR A2 (2TR D1/D2/D3 transmit nothing) |
| `09` | Stereo | `01` / `00` |
| `0A` | Assign 1 | `01` / `00` |
| `0B` | Assign 2 | `01` / `00` |

**Monitor switches** — `06 04 15 PP` (`01` on / `00` off)

| `PP` | Control |
|------|---------|
| `0C` | Mono |
| `0D` | Dimmer (also auto-fires when Talkback/Slate engage — talkback auto-dims) |
| `0F` | Small |

**Surround monitor** — `06 04 15 PP`

| `PP` | Control | Value |
|------|---------|-------|
| `10` | Surround group (Assign1 / Assign2 / Bus) | `01` / `02` / `04` |
| `13` | Surround Monitor Level knob | `00`–`7F` continuous |

**Talkback** — `06 04 15 PP` (`01` press / `00` release)

| `PP` | Control |
|------|---------|
| `11` | Talkback |
| `12` | Slate |

**Solo** — element `06 03 2D 03`

| Address | Control | Value |
|---------|---------|-------|
| `06 03 2D 03` | Solo contrast / dim knob | `00`–`7F` continuous |

**Stereo master strip** — channel-strip controls (mixed encodings)

| Control | Message(s) | Value |
|---------|-----------|-------|
| AUTO | `F0 43 10 3E 06 04 54 00 0n 00 00 00 VV F7` (sub-indices `00`/`01` both fire) | `VV` 01/00 |
| SEL | `F0 43 10 3E 06 04 5E 00 0n 00 00 00 VV F7` — also `… 7F 01 4D …` SysEx + CC `B4 77 VV` (ch 5) | 01/00 |
| ON | `F0 43 10 3E 06 04 09 21 00 00 00 00 VV F7` (coupled `09 18`=level) | 01 on / 00 off |
| Fader | **14-bit CC on ch 4**: `B3 1D <msb>` + `B3 3D <lsb>`; also `F0 43 10 3E 7F 01 4F 00 01 00 00 HH LL F7` | position 0 … `07 7F` (~0–1023) |

→ The stereo-master **fader** is a clean 14-bit value — it could drive (and, with the
SysEx-set form, be driven by) the REAPER master fader. AUTO/SEL/ON are addressable too.

Silent / not captured: `15 0E` (gap), the SOLO **CLEAR** button, and 2TR **D1/D2/D3**
all transmit nothing. Port 8 also spams a constant `F0 43 10 3E 06 7F F7` heartbeat — ignore.

**EQ / DYNAMICS / SELECTED CHANNEL: emit NO MIDI on any of the 8 ports** (tested
2026-06-26 via `hui_deskmon.py all`). So EQ is not controllable from the desk *even
on the native layer* — the native SysEx covers the monitor/master sections only, not
channel DSP. This confirms DESIGN.md's "EQ not doable", now on every port, not just HUI.

→ Opens a potential **control-room feature** (talkback / slate / dim / mono / small /
surround-monitor → REAPER monitoring actions), driven from the desk via the GENERAL
port. Layer-4; record-session-friendly. Supersedes the A1/A4 "drop" calls (those held
only for the DAW/HUI layer).

---

## B. "Does the desk render / light it?" — run `hui_send.py`

- [x] **B1 — RUDE SOLO LED** (canonical zone `0x16` p3; we already drive p0–p2 =
      TIME CODE / FEET / BEATS). Send `led 0x16 3 on`, then `led 0x16 3 off`.
      _Result (2026-06-26):_ **Nothing lights** → the DM2000 has no rude-solo
      indicator (a Mackie-HUI hardware feature it doesn't implement). **Absent.**
      (Other LEDs/counter from the same tool do light, so the desk is reachable.)

- [x] **B2 — Full REMOTE / 2×40 main display** (display SysEx **sub-command** `0x12`:
      `F0 00 00 66 05 00 12 …` — *not* the switch zone `0x12` of test A4). This is
      the REMOTE / INSERT display, which we already drive for the FX editor. Use the
      tool's display/`remote` command (see `help`) to write **all eight cells / both
      lines** and confirm how wide and how many lines the desk actually renders.
      _Result (2026-06-26):_ all 8 lines × 10 chars render — but this only confirms what
      the FX editor already uses; nothing new. → **No action.**

- [x] **B3 — Non-ASCII scribble characters.** REAPER track names can contain
      non-ASCII (e.g. Polish ł / ó / ż). The HUI small-display set
      (`hui-spec/HUI_CSET.txt`) is its own ~7-bit set, not Latin-1 / UTF-8. Send a
      scribble with accented / Polish characters and see what the strip renders;
      decide how the plugin should map or fold unsupported characters. (Partly a
      code question — the UTF-8 → HUI-charset mapping — but needs the desk to
      confirm what actually displays.)
      _Result (2026-06-26):_ **Non-ASCII is dropped.** "łóżk" → "k", "café" → "caf" —
      only 7-bit ASCII survives; accented / Polish chars vanish entirely. → **Action:**
      transliterate non-ASCII track names to ASCII before sending (ł→l, ó→o, ż→z, é→e,
      …) so e.g. "Łóżko" shows as "Lozk" instead of "k". Small, worthwhile fix.

- [x] **B4 — Timecode decimal points / separators.** Send a known counter value
      with separators, e.g. `counter 1:23:45.67`, and confirm the digits, the
      decimal point, and the field separators all land in the right positions.
      _Result (2026-06-26):_ The LED counter has **dots only** — no colon segment, so
      `:` renders as `.` (`1:23:45.67` → `01.23.45.67`). Matches the canonical HUI
      timecode display (per-digit decimal points) and our hardware-verified counter.
      → **No action** — works as designed; the hardware simply has no colons.

- [x] **B5 — Click / Beep** (canonical zone `0x1d` p2 = click, p3 = beep). Send
      `led 0x1d 2 on` (click) and `led 0x1d 3 on` (beep).
      _Result (2026-06-26):_ **Nothing** → click/beep are Mackie-HUI hardware features
      the DM2000 doesn't implement. **Absent.**

---

## When done

For each item: record the result + a decision (implement / drop / needs more
capture). Move anything actionable into `TODO.md`, update the status flags in
`hui-canonical-coverage.md`, and delete this file.
