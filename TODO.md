# TODO

Tasks tracked here to avoid scrolling chat history.

---

## Hardware / on-site

- [ ] **Fader recalibration** - use DM2000 Editor to set exact dB values, capture
      HUI wire values in MIDI-OX. Replace the 6 hand-matched printed-mark points
      in `g_taper_db[]` / `g_taper_val[]` with a dense, precise table.
      Target: -60, -50, -40, -30, -20, -10, -6, -3, 0, +3, +5 at minimum.

- [ ] **Scene recall SysEx capture** - the address in DESIGN.md is a guess.
      Use Studio Manager or front-panel to trigger a scene change while MIDI-OX
      monitors port 8. Capture the exact SysEx message before implementation.

- [ ] **Port 4 channels 25-32** - identify what those strips physically represent
      on the DM2000 (master bus? effects returns? CR?). Determines whether and
      how to wire them to REAPER tracks.

---

## Software

- [ ] **LOCATE MEMORY 1-8** - zone map captured (0x10 / 0x0F), wiring to
      REAPER marker jump not yet written.

- [ ] **Transport extensions** - RTZ, end, loop toggle, quick punch, roll back,
      rehearsal. Zone mapping partially in DESIGN.md, none wired up.

- [ ] **USER DEFINED KEYS dispatch** - config dialog has path field; the actual
      ini-file reader that maps `zone_sw = action_id` to REAPER actions is not
      written yet.

- [ ] **macOS port** - REAPER csurf API is cross-platform. Plan: CMake build
      targeting `.dylib`, SWELL for the dialog (ships with REAPER SDK, replaces
      Win32 resource/dialog API with minimal code changes), replace ShellExecuteA
      with `open` verb. No code rewrite needed, mostly a build system task.

- [ ] **Scene recall** - blocked on SysEx capture above.

- [ ] **Fader taper** - blocked on recalibration capture above.
