# TODO

Tasks tracked here to avoid scrolling chat history.

---

## Hardware / on-site

- [ ] **Fader recalibration** - use DM2000 Editor to set exact dB values, capture
      HUI wire values in MIDI-OX. Replace the 6 hand-matched printed-mark points
      in `g_taper_db[]` / `g_taper_val[]` with a dense, precise table.
      Target: -60, -50, -40, -30, -20, -10, -6, -3, 0, +3, +5 at minimum.

- [ ] **Scene recall — verify GENERAL port** - manual ch.18 confirms Program Change
      on the GENERAL port recalls DM2000 scenes bidirectionally. On the console:
      `SETUP → MIDI/HOST SETUP → GENERAL`, set to one of the 4 DAW USB ports
      (whichever the csurf already opens). PC receive is implemented in code;
      test by recalling a DM2000 scene and confirming REAPER jumps to the
      matching marker.

- [ ] **Transport sw verification** - RTZ (sw=0), END (sw=6), LOOP (sw=7) have
      been added to zone 0x0E but switch numbers are guesses. Verify by pressing
      those buttons while MIDI-OX monitors port 1, or confirm the zone 0x0E
      switch positions match implementation.

- [ ] **LOCATE MEMORY 1-8 zone** - TODO says zone 0x10 / 0x0F was captured;
      needs hardware re-verification. Once confirmed, wire to
      `SendMessage(g_hwnd, WM_COMMAND, ID_GOTO_MARKER1 + n, 0)`.

- [ ] **Port 4 channels 25-32** - identify what those strips physically represent
      on the DM2000 (master bus? effects returns? CR?). Determines whether and
      how to wire them to REAPER tracks.

- [ ] **Remote Meter receive** - manual appendix shows SysEx type 0x21 (Remote
      Meter) is rx/tx on port 8. Capture what the DM2000 sends on port 8 during
      playback to see if it pushes peak data to the host (would enable meter
      bridge without polling).

---

## Software

- [x] **Fader taper** - hardware-calibrated table implemented in `g_taper_db[]` /
      `g_taper_val[]` from printed-mark captures 2026-06-12. Revisit after full
      recalibration run above.

- [x] **Transport: play, stop, rec, rew, fwd** - implemented and hardware-verified.

- [ ] **Transport extensions: RTZ, END, LOOP** - added to zone 0x0E sw=0/6/7;
      switch numbers unverified. `SetRepeatState` wired to LOOP LED.

- [ ] **Scene recall PC receive** - implemented: PC 1-9 on any DAW port jump to
      REAPER markers 1-9 (1-indexed per manual p.218). Requires GENERAL port to be
      set to a DAW USB port on the console. PC 10+ not yet handled (needs a
      scene-to-marker table).

- [ ] **Scene recall PC send** - REAPER → DM2000: when playback crosses a marker,
      send PC to DM2000 to recall the matching scene. Requires knowing which port
      is GENERAL; plan to add as optional 5th port in config, or reuse port 4.

- [ ] **LOCATE MEMORY 1-8** - zone map noted as 0x10 / 0x0F; wiring to
      REAPER marker jump not yet written. Needs hardware re-verification first.

- [ ] **USER DEFINED KEYS dispatch** - config dialog has ini path field; the actual
      ini-file reader that maps `zone_sw = action_id` to REAPER actions is not
      written. From manual ch.19: keys 2/3 = bank ±24 (duplicate of zone 0x0A),
      10/11 = channel ±1 (duplicate). Other keys (1,4-9,12-16) have no defined
      mapping — worth assigning to REAPER actions via the ini file.

- [ ] **EQ / parameter control via CC** - manual appendix confirms EQ ATT (input
      attenuation), EQ ON/OFF, and all faders/pans are accessible as MIDI CC on
      the GENERAL port (channels 1-16, CC table on pp. 353-368). Layer 4 feature;
      requires GENERAL port open and a selected-channel UI concept.

- [~] **macOS port** - SWELL Makefile + portability shims written; compiled on Mac
      (2026-06-14, two fixes needed — see MACOS_BUILD.md "Compile fixes" section).
      Remaining: verify dialog + surface in REAPER (Step 5 of MACOS_BUILD.md).

- [ ] **Scene recall full** - basic PC path implemented above; blocked on
      SysEx for full scene dump/restore and on GENERAL port config for send direction.
