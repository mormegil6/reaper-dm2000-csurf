# TODO

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

- [x] **Port 4 MCS PANNER** - surround pan implemented 2026-06-16 (see "Surround pan
      control" in the Software section). Joystick BE 02 (X) / BE 03 (Y, absolute), dynamics
      knobs BE 10-14 (relative), and the ROUTING buttons (BE 00/01 N) for object select drive
      the `[surround]` plugin on the selected track; the selected routing button's LED lights via
      port 4 (`BE 00 N` on / `BE 01 N` off). Display/Stereo routing buttons don't transmit.

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

- [x] **USER DEFINED KEYS dispatch** - `[udk]` dispatcher implemented 2026-06-16. key1..key16
      -> Main_OnCommand(id,0) on press. ALL 16 zones hw-verified 2026-06-16 (full in-order
      capture, corrected an earlier off-by-one): UDK 1=0x09 sw2, 2=0x0A sw1, 3=0x0A sw3,
      4=0x08 sw1, 5=0x08 sw5, 6=0x19 sw5, 7=0x19 sw3, 8=0x19 sw4, 9=0x09 sw0(+sw1 companion),
      10=0x0A sw0, 11=0x0A sw2, 12=0x08 sw0, 13=0x08 sw4, 14=0x19 sw1, 15=0x08 sw3, 16=0x08 sw7.
      UDK 2/3/10/11 are the BANK ◄/► and CH ◄/► buttons: they keep navigation unless their ini
      key is set (per-button override fallback). zone 0x19 UDK deduped via port 0 (same proven
      path as AUTOMIX ENABLE). Hardware-verified working 2026-06-16.

- [x] **HUI counter refresh rate** - LED timecode decoupled from the 100ms meter poll onto its
      own timer; `[counter] refresh_ms` in dm2000_keys.ini (default 33 ms ≈ 30 Hz, clamped
      20-1000) for smooth SMPTE frames. Delta-encoding means only changed digits transmit.

- [x] **Surround pan control (port 4 MCS PANNER)** - implemented + hw-verified 2026-06-16.
      DYNAMICS knobs (`BE 10..14`, relative) + joystick (`BE 02`=X / `BE 03`=Y, absolute 0-127,
      Y inverted) drive the `[surround]` plugin on the selected track. ROUTING buttons 1-8 select
      object/input (param += (obj-1)*stride); Direct shifts the bank of 8 (single up, double down,
      no wrap). Compiled default none; example ini targets ReaSurroundPan input 1 (front=8/rear=9/
      lr=7/lfe=10/vol=6). stride/objects/plugin/indices ini-configurable. Remaining: pick the final
      dynamics-knob->param feel.

- [x] **Routing-button LED feedback** - implemented 2026-06-16. The selected object's ROUTING
      button lights via **port 4** (already open): `BE 00 N` lights, `BE 01 N` clears (N in the
      2-column wire order). LEDs update on object/bank change and clear on close. (The earlier
      port-8 native-SysEx method `F0 43 10 3E 7F 01 22 03 ... NN F7` is abandoned - its NN=00
      "off" did not clear on the Remote layer; port 4 works cleanly with no port 8 needed.)

- [x] **Generic FX parameter editor** - implemented + hw-tested 2026-06-16, always-on (no edit
      mode). Zone 0x1C F-buttons: F4=sw0 (home slot0/page0), F1=sw1 (next FX slot), F2=sw7 (prev
      FX slot), F3=sw6 (bypass), sw2-sw5=knob 1-4 press (reset that param to 0). Param knobs 1-4
      (`B0 48..4B`, port 1, step 0.001) nudge the current 4-param page; up/down page arrows
      (`B0 4C`, up=next/down=prev, wrapping) scroll pages with a 250ms debounce (arrows fire twice
      per press). Slot/page console-logged.

- [ ] **EQ / parameter control via CC** - manual appendix confirms EQ ATT (input
      attenuation), EQ ON/OFF, and all faders/pans are accessible as MIDI CC on
      the GENERAL port (channels 1-16, CC table on pp. 353-368). Layer 4 feature;
      requires GENERAL port open and a selected-channel UI concept.

- [x] **macOS port** - cross-platform SWELL build; every release is hardware-verified on
      macOS (universal arm64+x86_64) as well as Windows.

- [ ] **Scene recall full** - basic PC path implemented above; blocked on
      SysEx for full scene dump/restore and on GENERAL port config for send direction.
