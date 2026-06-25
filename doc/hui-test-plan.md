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
  - **`py tools/hui_deskmon.py`** — decodes everything the desk *transmits*
    (find out whether a control sends MIDI, and capture its zone/switch).
  - **`py tools/hui_send.py`** — drives any surface element by hand
    (`led <zone> <sw> [on|off]`, plus display/counter/meter aliases; type `help`).

---

## A. "Does the desk send it?" — run `hui_deskmon.py`, then operate controls

- [ ] **A1 — Footswitch input** (canonical zone `0x1d` p0/p1). Plug a pedal into the
      DM2000's foot-switch jack(s) and press. Watch for `0x1d` / footswitch
      (`b0 0f 1d` + `b0 2f 40/41`).
      _Result:_ …  → if it transmits, expose as an assignable `[foot]` action.

- [ ] **A2 — Edit operations** (canonical zone `0x1a` = paste/cut/capture/delete/
      copy/separate). Press the desk's edit-ish buttons that we don't decode yet.
      Watch for any `0x1a` press; note which physical button maps to which switch.
      _Result:_ …  → if present, free cut/copy/paste/delete action hooks.

- [ ] **A3 — Monitor / control-room section** (canonical `0x11` monitor-input,
      `0x12` monitor-output). Operate the monitor/control-room buttons; watch for
      `0x11` / `0x12` switch presses. (Note: `0x12` is also the REMOTE-display *TX*
      zone — here we're looking at switch *RX*.)
      _Result:_ …  → could map to REAPER monitoring.

- [ ] **A4 — Completeness sweep.** With deskmon running, press every remaining
      unmapped button / section once. Note any zone/switch that transmits but we
      don't currently handle.
      _Result:_ …

---

## B. "Does the desk render / light it?" — run `hui_send.py`

- [ ] **B1 — RUDE SOLO LED** (canonical zone `0x16` p3; we already drive p0–p2 =
      TIME CODE / FEET / BEATS). Send `led 0x16 3 on`, then `led 0x16 3 off`.
      Does a global "something is soloed" indicator light anywhere on the desk?
      _Result:_ …  → if yes, drive it whenever any track is soloed (cheap win).

- [ ] **B2 — Full REMOTE / 2×40 main display** (canonical `<hdr> 12 …`; this *is*
      our zone `0x12` REMOTE display, already used by the FX editor as 8×10). Use
      the tool's display/`remote` command (see `help`) to write **all eight cells /
      both lines** and confirm how wide and how many lines the desk actually
      renders.
      _Result:_ …  → decides whether a general 2-line status/parameter readout
      (beyond the FX editor's current use) is worthwhile.

- [ ] **B3 — Click / Beep** (canonical zone `0x1d` p2 = click, p3 = beep). Send
      `led 0x1d 2 on` (click) and `led 0x1d 3 on` (beep). Any audible response?
      _Result:_ …  (low priority — only if curious.)

---

## When done

For each item: record the result + a decision (implement / drop / needs more
capture). Move anything actionable into `TODO.md`, update the status flags in
`hui-canonical-coverage.md`, and delete this file.
