# TODO

Tasks tracked here to avoid scrolling chat history.

---

## Hardware / on-site

- [x] **Fader recalibration** - DM2000 built-in fader calibration utility run
      2026-06-15, then DM2000 Editor used to confirm 11 dB marks via MIDI-OX
      (doc/fader-calibration-2026-06-15.txt). `g_taper_db[]` / `g_taper_val[]`
      updated to 11-point post-calibration table (-inf, -50..+10). Previous table
      (2026-06-12, 6 points) had systematic offset in -20 to -5 dB range; fixed.

- [ ] **Scene recall — verify GENERAL port** - manual ch.18 confirms Program Change
      on the GENERAL port recalls DM2000 scenes bidirectionally. On the console:
      `SETUP → MIDI/HOST SETUP → GENERAL`, set to one of the 4 DAW USB ports
      (whichever the csurf already opens). PC receive is implemented in code;
      test by recalling a DM2000 scene and confirming REAPER jumps to the
      matching marker.

- [x] **Transport sw verification** - RTZ, END, LOOP are at zone 0x10 sw 0/1/2
      (not zone 0x0E as previously guessed). Verified 2026-06-15, code fixed.

- [x] **ENTER zone hardware re-verify** - zone 0x14 sw 0 confirmed by full
      capture 2026-06-15. ENTER now toggles cursor arrow keys between scroll
      mode and zoom mode (CSurf_OnArrow zoom flag). Previously mapped to insert
      marker; that function moved to QUICK PUNCH (zone 0x10 sw3).

- [x] **LOCATE MEMORY zone** - was wrongly coded at zone 0x0F sw 0-7.
      Correct: zone 0x13, non-sequential sw: LM1=sw1, LM2=sw3, LM3=sw6,
      LM4=sw2, LM5=sw4, LM6=sw7; sw5 is a companion event that fires
      alongside every press (ignore it). Code fixed 2026-06-15.

- [ ] **Port 4 MCS PANNER** - protocol captured 2026-06-15: surround joystick
      uses BE 02 (X-axis) and BE 03 (Y-axis) CC on MIDI channel 15 (port 4).
      Routing buttons use BE 00/01 pairs; dynamics knobs use BE 10-14.
      Implementation pending; Layer 4 feature.

- [ ] **Remote Meter receive** - manual appendix shows SysEx type 0x21 (Remote
      Meter) is rx/tx on port 8. Capture what the DM2000 sends on port 8 during
      playback to see if it pushes peak data to the host (would enable meter
      bridge without polling).

- [x] **Full button zone map** - complete MIDI-OX surface capture done
      2026-06-15 (every button, fader, knob). Raw data: doc/midi-capture-2026-06-15.txt.
      Zone/sw table updated in DESIGN.md. Remaining gaps: zone 0x10 sw4,
      zone 0x0F sw4 not identified. BACK/FORWARD wired 2026-06-15 (zone 0x08
      sw2=undo, sw6=redo). QUICK PUNCH (zone 0x10 sw3) = insert marker.

---

## Software

- [x] **Fader taper** - 11-point post-calibration table implemented 2026-06-15
      (see Hardware section above). Previous 6-point hand-matched table superseded.

- [x] **Transport: play, stop, rec, rew, fwd** - implemented and hardware-verified.
      REW/FF (zone 0x0E sw1/sw2) also support auto-repeat when held (400 ms delay,
      80 ms interval), matching the cursor arrow key behavior.

- [~] **HUI counter display** - `F0 00 00 66 05 00 11 <8 ASCII chars> F7` sent every 100ms
      in `Run()`. Command byte 0x11 and ASCII encoding are UNVERIFIED on the DM2000. Verify
      by checking the LED counter during playback; if blank/garbled, capture what Pro Tools
      sends to port 1 with MIDI-OX and adjust format.

- [x] **Transport extensions: RTZ, END, LOOP** - moved to zone 0x10 sw=0/1/2
      (hardware-verified 2026-06-15). LOOP LED now targets zone 0x10 sw 2.
      AUTOMIX section sw assignments also corrected (all were wrong previously).

- [ ] **Scene recall PC indexing verify** - implemented for all 99 scenes using
      0-indexed wire format (byte 0x00 = Scene 1 → marker 1). Per manual p.370
      ("Program number 0-127"). UNVERIFIED: test by recalling scenes 1 and 2 and
      checking which REAPER markers are jumped to; if off by one, flip to 1-indexed.

- [ ] **Scene recall PC send** - REAPER → DM2000: when playback crosses a marker,
      send PC to DM2000 to recall the matching scene. Requires knowing which port
      is GENERAL; plan to add as optional 5th port in config, or reuse port 4.

- [ ] **USER DEFINED KEYS dispatch** - config dialog has ini path field; the actual
      ini-file reader that maps key N to a REAPER action ID is not written.
      Format and suggested actions documented in doc/dm2000_keys.ini.example.
      Confirmed UDK zones: 4/5/13/14=zone 0x08, 6/7/8=zone 0x19. Keys 2/3 duplicate
      bank navigation (zone 0x0A sw1/sw3) and 10/11 duplicate ch navigation
      (zone 0x0A sw0/sw2) per manual ch.19; zones for 1/3/9/11/12/15/16 unconfirmed.

- [ ] **EQ / parameter control via CC** - manual appendix confirms EQ ATT (input
      attenuation), EQ ON/OFF, and all faders/pans are accessible as MIDI CC on
      the GENERAL port (channels 1-16, CC table on pp. 353-368). Layer 4 feature;
      requires GENERAL port open and a selected-channel UI concept.

- [~] **macOS port** - SWELL Makefile + portability shims written; compiled on Mac
      (2026-06-14, two fixes needed — see MACOS_BUILD.md). Plugin loads in REAPER
      and appears in the control surface list. Remaining: hardware verification
      with DM2000 connected (faders, transport, meters).

- [ ] **Scene recall full** - basic PC path implemented above; blocked on
      SysEx for full scene dump/restore and on GENERAL port config for send direction.
