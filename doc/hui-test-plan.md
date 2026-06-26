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

- [ ] **A1 — TALKBACK** (canonical zone `0x0e` p0). We already handle this zone's
      p1–p5 (REW / FFWD / STOP / PLAY / REC); **p0 is the one port we don't use.**
      Press the desk's TALKBACK control and watch for `0x0e` sw0 (`b0 0f 0e` +
      `b0 2f 40`). **Highest-value item on this list** — if it transmits, it's an
      immediately implementable hardware talkback hook for recording sessions.
      _Result:_ …  → if yes, wire to a REAPER talkback action / send.

- [ ] **A2 — Footswitch input** (canonical zone `0x1d` p0/p1). Plug a pedal into the
      DM2000's foot-switch jack(s) and press. Watch for `0x1d` / footswitch
      (`b0 0f 1d` + `b0 2f 40/41`).
      _Result:_ …  → if it transmits, expose as an assignable `[foot]` action.

- [ ] **A3 — Edit operations** (canonical zone `0x1a` = paste/cut/capture/delete/
      copy/separate). Press the desk's edit-ish buttons that we don't decode yet.
      Watch for any `0x1a` press; note which physical button maps to which switch.
      _Result:_ …  → if present, free cut/copy/paste/delete action hooks.

- [ ] **A4 — Monitor / control-room section** (canonical switch zones `0x11`
      monitor-input, `0x12` monitor-output). Operate the monitor/control-room
      buttons; watch for `0x11` / `0x12` switch presses.
      ⚠️ This switch/LED **zone** `0x12` is unrelated to the display SysEx
      **sub-command** `0x12` in test B2 — two different namespaces that happen to
      share the number `0x12`.
      _Result:_ …  → could map to REAPER monitoring.

- [ ] **A5 — Power-cycle / system reset** (`0xff`). With deskmon running, power the
      DM2000 off and on and confirm one or more `0xff` (MIDI System Reset) arrive.
      Then repeat with REAPER + csurf running and confirm the plugin recovers
      gracefully (re-onlines, no stuck LEDs/faders).
      _Result:_ …

- [ ] **A6 — Completeness sweep.** With deskmon running, press every remaining
      unmapped button / section once. Note any zone/switch that transmits but we
      don't currently handle.
      _Result:_ …

---

## B. "Does the desk render / light it?" — run `hui_send.py`

- [ ] **B1 — RUDE SOLO LED** (canonical zone `0x16` p3; we already drive p0–p2 =
      TIME CODE / FEET / BEATS). Send `led 0x16 3 on`, then `led 0x16 3 off`.
      Does a global "something is soloed" indicator light anywhere on the desk?
      _Result:_ …  → if yes, drive it whenever any track is soloed (cheap win).

- [ ] **B2 — Full REMOTE / 2×40 main display** (display SysEx **sub-command** `0x12`:
      `F0 00 00 66 05 00 12 …` — *not* the switch zone `0x12` of test A4). This is
      the REMOTE / INSERT display, which we already drive for the FX editor. Use the
      tool's display/`remote` command (see `help`) to write **all eight cells / both
      lines** and confirm how wide and how many lines the desk actually renders.
      _Result:_ …  → decides whether a general 2-line status/parameter readout
      (beyond the FX editor's current use) is worthwhile.

- [ ] **B3 — Non-ASCII scribble characters.** REAPER track names can contain
      non-ASCII (e.g. Polish ł / ó / ż). The HUI small-display set
      (`hui-spec/HUI_CSET.txt`) is its own ~7-bit set, not Latin-1 / UTF-8. Send a
      scribble with accented / Polish characters and see what the strip renders;
      decide how the plugin should map or fold unsupported characters. (Partly a
      code question — the UTF-8 → HUI-charset mapping — but needs the desk to
      confirm what actually displays.)
      _Result:_ …

- [ ] **B4 — Timecode decimal points / separators.** Send a known counter value
      with separators, e.g. `counter 1:23:45.67`, and confirm the digits, the
      decimal point, and the field separators all land in the right positions.
      _Result:_ …

- [ ] **B5 — Click / Beep** (canonical zone `0x1d` p2 = click, p3 = beep). Send
      `led 0x1d 2 on` (click) and `led 0x1d 3 on` (beep). Any audible response?
      _Result:_ …  (low priority — only if curious.)

---

## When done

For each item: record the result + a decision (implement / drop / needs more
capture). Move anything actionable into `TODO.md`, update the status flags in
`hui-canonical-coverage.md`, and delete this file.
