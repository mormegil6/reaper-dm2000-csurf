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

- [x] **Transport sw verification** - RTZ, END are zone 0x0F sw0/1; LOOP is zone 0x0F sw3
      (Locate row 2, not transport zone 0x0E). All hw-verified 2026-06-15, code fixed.

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
      Zone/sw table updated in DESIGN.md. All locate-section switches identified:
      zone 0x10 sw4 = POST, zone 0x0F sw4 = QUICK PUNCH. BACK/FORWARD wired 2026-06-15
      (zone 0x08 sw2=undo, sw6=redo). QUICK PUNCH (zone 0x0F sw4) defaults to insert marker
      via dm2000_keys.ini.

---

## Software

- [x] **Fader taper** - 11-point post-calibration table implemented 2026-06-15
      (see Hardware section above). Previous 6-point hand-matched table superseded.

- [x] **Transport: play, stop, rec, rew, fwd** - implemented and hardware-verified.
      REW/FF (zone 0x0E sw1/sw2) also support auto-repeat when held (400 ms delay,
      80 ms interval), matching the cursor arrow key behavior.

- [x] **ENTER third mode** - ENTER now cycles three modes: scroll (CSurf_OnArrow
      zoom=false), zoom (zoom=true), bank-scroll (left/right = AdjustBankOffset +-1,
      with auto-repeat). Mode cycles: 0 -> 1 -> 2 -> 0.

- [x] **LOCATE MEMORY 7-8** - zone 0x15 sw0=LM7, sw1=LM8 added (hw-captured 2026-06-15).
      All 8 LOCATE MEMORY buttons now wired.

- [x] **AUTO button fader reset** - zone 0-7 sw4 now resets that channel's fader to
      0 dB (CSurf_OnVolumeChange to 1.0 = unity gain).

- [x] **Scrub speed** - CSurf_ScrubAmt multiplier increased from 0.05 to 0.5 (10x).
      Jog and shuttle unchanged.

- [x] **GUI ini example link** - config dialog (IDD_SURFACEEDIT_DM2000) height increased
      from 100 to 118 dialog units; second SysLink added linking to
      doc/dm2000_keys.ini.example on GitHub.

- [x] **HUI counter display** - protocol decoded 2026-06-15 from Pro Tools loopMIDI capture.
      `F0 00 00 66 05 00 11 [N bytes right-to-left] F7`; each byte `(sep_flag<<4)|digit`,
      delta updates only. Follows REAPER transport display format via `timemode2`/`timemode`
      project config vars (same as csurf_mcu). Hardware-verified 2026-06-15.

- [x] **Transport extensions: RTZ, END, LOOP** - zone 0x0F sw0/1/3 (Locate row 2,
      hw-verified 2026-06-15). LOOP LED targets zone 0x0F sw3 via SetRepeatState.
      AUTOMIX section sw assignments also corrected (all were wrong previously).

- [x] **Locate section INI-configurable** - all buttons in the Locate section (RTZ, END,
      ONLINE, LOOP, QPUNCH, AUDITION, PRE, IN, OUT, POST, LM1-LM8) read their REAPER
      action IDs from the `[locate]` section of dm2000_keys.ini on plugin load.
      0 = no action (RTZ/END: 0 = use CSurf_GoStart/GoEnd API). All constructor defaults
      are 0; meaningful defaults (LOOP=1068, IN=40222, LM1-8=40161-40168, etc.) come from
      dm2000_keys.ini.example which is auto-installed to REAPER's resource folder on first
      build/install. doc/dm2000_keys_guide.md added with full action ID reference and
      example configurations.

- [ ] **Scene recall PC indexing verify** - implemented for all 99 scenes using
      0-indexed wire format (byte 0x00 = Scene 1 → marker 1). Per manual p.370
      ("Program number 0-127"). UNVERIFIED: test by recalling scenes 1 and 2 and
      checking which REAPER markers are jumped to; if off by one, flip to 1-indexed.

- [ ] **Scene recall PC send** - REAPER → DM2000: when playback crosses a marker,
      send PC to DM2000 to recall the matching scene. Requires knowing which port
      is GENERAL; plan to add as optional 5th port in config, or reuse port 4.

- [ ] **USER DEFINED KEYS dispatch** - config dialog has ini path field; the `[locate]`
      section is now implemented (see above), but the `[udk]` dispatcher that maps UDK
      button N to a REAPER action ID is not yet written. Format documented in
      doc/dm2000_keys.ini.example under `[udk]`. Confirmed UDK zones: 4/5/13/14=zone 0x08,
      6/7/8=zone 0x19. Keys 2/3 duplicate bank navigation (zone 0x0A sw1/sw3) and 10/11
      duplicate ch navigation (zone 0x0A sw0/sw2) per manual ch.19; zones for
      1/9/12/15/16 unconfirmed.

- [ ] **EQ / parameter control via CC** - manual appendix confirms EQ ATT (input
      attenuation), EQ ON/OFF, and all faders/pans are accessible as MIDI CC on
      the GENERAL port (channels 1-16, CC table on pp. 353-368). Layer 4 feature;
      requires GENERAL port open and a selected-channel UI concept.

- [x] **macOS port** - hardware-verified with DM2000 connected on 2026-06-15 (v0.3).
      All features work on macOS.

- [ ] **Scene recall full** - basic PC path implemented above; blocked on
      SysEx for full scene dump/restore and on GENERAL port config for send direction.
