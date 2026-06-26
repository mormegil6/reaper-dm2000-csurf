# TODO

## Hardware / on-site

- [x] **Fader recalibration** - DM2000 built-in fader calibration utility run
      2026-06-15, then DM2000 Editor used to confirm 11 dB marks via MIDI-OX
      (doc/fader-calibration-2026-06-15.txt). `g_taper_db[]` / `g_taper_val[]`
      updated to 11-point post-calibration table (-inf, -50..+10). Previous table
      (2026-06-12, 6 points) had systematic offset in -20 to -5 dB range; fixed.

- [x] **Scene recall — GENERAL port verified 2026-06-16** - captured the wire format on the
      GENERAL port with MIDI-OX: scroll/display `F0 43 10 3E 06 04 0A 00*5 <scene+1> F7` (fires on
      every dial turn, even empty scenes) vs. recall `C0 <scene-1>` (only on RECALL). The console
      recalls on an incoming `C0 <scene-1>`, confirmed on hardware. The GENERAL port is a separate,
      unused USB port (the console refuses to share a DAW port number) and is selected in the config
      dialog, not derived from the HUI ports.

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

- **Remote Meter receive - NOT PURSUING (parked 2026-06-16).** The idea was to read SysEx type 0x21
      (Remote Meter) on port 8 and drive the meter bridge by push instead of polling. Finding: the
      console does not talk back unsolicited - a counter value pushed via Bome SendSX updated the
      display, but the DM2000 returned nothing (the display path is host->console only). A push feed
      would require sending the Studio Manager SUBSCRIPTION SysEx first
      (`F0 43 37 3E 06 21 00 <sub> 00 00 18 F7`, sub=0x05 peak) - untested, and pointless anyway since
      HUI meter polling already drives the bridge fine. Dropped unless polling ever proves too coarse.

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

- [x] **Scene recall PC indexing — verified 2026-06-16** - receive maps `C0 <scene-1>` → scene N
      → REAPER marker *numbered* N (matched via EnumProjectMarkers, not the "go to marker NN" actions
      which only exist for 1-10 and would fire a stray edit). Default on; the scroll SysEx is ignored
      so a recall can't double-fire. Hardware-confirmed: recalling scene 3/4 on the desk jumps REAPER
      to markers 3/4.

- [x] **Scene recall PC send — implemented + hw-verified 2026-06-16** - `!SCENE n` markers recall
      scene n on the desk (scroll SysEx + `C0 n-1`), `#SCENE n` scrolls the display only; number is
      the first token, trailing text is a free label. Forward crossings during playback; owning-scene
      resync on play-start / loop wrap / backward seek; optional `follow_cursor` while stopped (0 off /
      1 recall-on-settle / 2 scroll-only). GENERAL port chosen in the config dialog (10-int config
      string, backward compatible). Default off. `[scene] send/receive/follow_cursor/marker_prefix/
      marker_recall` in dm2000_keys.ini.

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

- **EQ via the console's EQ knobs - NOT DOABLE (confirmed 2026-06-16).** The SELECTED CHANNEL EQ
      section is wired to the DM2000's own internal EQ, NOT to the DAW remote layer. Confirmed by
      testing Pro Tools itself: the EQ knobs do nothing for the DAW there either - they only touch the
      console's onboard EQ (which needs audio passing through the desk, and the remote layer uses just
      the 4 HUI ports). So the physical EQ controls transmit nothing usable on ports 1-4 or GENERAL,
      and there is no path to drive a REAPER EQ from them. Use the below-display **FX parameter editor**
      instead (assign ReaEQ, edit via the 4 knobs + page arrows). The GENERAL-port CC table still lists
      EQ ATT / faders / pans, but those are for remoting the console's *own* parameters, not a feed
      from the EQ knobs. **Re-confirmed 2026-06-26** by an all-8-port sniff (`hui_deskmon.py all`):
      EQ / dynamics / selected-channel emit nothing on *any* port. (The control-room / monitor section,
      however, DOES transmit native SysEx - see the new item below.)

- [ ] **Control-room / talkback via native SysEx (GENERAL port)** - hardware-captured 2026-06-26
      ([doc/dm2000-native-sysex.md](doc/dm2000-native-sysex.md)). The DM2000's talkback, slate, dim,
      mono, small, surround-monitor, solo knob, and 14-bit stereo-master strip (auto/sel/on/fader) all
      transmit native Yamaha SysEx on ports 5 & 8 - silent on HUI, but the plugin already opens the
      GENERAL port for scene recall, so they're sniffable there with no extra wiring. Candidate
      Layer-4/5 feature: map talkback / slate / dim / mono / surround-monitor -> REAPER actions, and the
      stereo-master fader/ON <-> REAPER master. Opt-in config like `[scene]`.

- [x] **macOS port** - cross-platform SWELL build; every release is hardware-verified on
      macOS (universal arm64+x86_64) as well as Windows.

- [x] **FLIP (FADER MODE button)** - implemented 2026-06-26 (reserved for v0.9). The FADER MODE
      button (zone 0x0C sw3) toggles FLIP: while the encoders ride a send (an AUX SELECT active),
      the motor faders ride the selected send (calibrated taper, absolute set), and the encoders +
      rings ride volume (1 dB/detent, ring shows volume). The send-level poll drives the faders
      (no REAPER send callback); SetSurfaceVolume drives the ring in flip. The FADER/AUX-MTRX LED
      (B0 2C 03/43) tracks state. Pan mode = no-op (nothing to swap). `[fader] flip` (default on).
      Sends-only first cut; pan-on-fader swap is a possible later extension. **Pending hardware test.**

- [x] **Scribble non-ASCII transliteration** - implemented 2026-06-26. The 4-char scribble
      strips drop any byte >= 0x80 (hardware-confirmed: "Łóżko" was showing as just "k"), so
      track names with Polish/accented characters were vanishing. `scribbleAsciiFold` now folds
      UTF-8 diacritics to base ASCII (ł->l, ó->o, ż->z, é->e, ß->s, ñ->n, …) before sending, on
      both the HUI scribble and the port-8 native-name paths. ASCII names unchanged. Logic
      unit-checked; pending a hardware confirm on the strips.
