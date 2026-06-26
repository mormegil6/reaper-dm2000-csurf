# DM2000 native control-room / master SysEx (ports 5 & 8)

Hardware-captured 2026-06-26. The DM2000's **control-room, monitor, solo and
stereo-master** sections are **silent on the HUI / DAW layer** (ports 1-4), but they
**do** transmit **Yamaha native parameter-change SysEx** on ports 5 (GENERAL) and 8.

This matters because the plugin **already opens the GENERAL port** (port 5) for scene
recall — so all of this is sniffable there with no extra wiring. It is effectively a
second addressable layer beyond HUI, and the basis for a future control-room feature
(talkback / slate / dim / mono / surround-monitor → REAPER actions, and the 14-bit
stereo-master fader ↔ the REAPER master).

Captured with `tools/hui_deskmon.py all` (watches all 8 ports). See also the
[HUI coverage notes](hui-canonical-coverage.md) and the
[hardware test plan](hui-test-plan.md) this came out of.

## Message format

```
F0 43 10 3E 06 EE AA PP 00 00 00 00 VV F7
```

- `43` = Yamaha manufacturer ID, `10` = parameter change (device 0), `3E` = DM2000 model.
- `EE AA PP` address the control; `VV` is the value.
- Most monitor controls live in element `06 04 15` (so `PP` is the parameter); the SOLO
  knob is element `06 03 2D 03`.
- Source selects within a group are **exclusive radio-buttons** — selecting one clears
  the others, which is why a press emits a paired `…01` (set) and `…00` (clear).
- All confirmed mirrored on **ports 5 and 8**.
- Port 8 also spams a constant `F0 43 10 3E 06 7F F7` heartbeat — ignore it.

## STUDIO monitor source — `06 04 15 PP`, exclusive (`01`=selected / `00`=cleared)

| `PP` | Source |
|------|--------|
| `00` | Control Room |
| `01` | Stereo |
| `02` | Aux 11 |
| `03` | Aux 12 |

## CONTROL ROOM monitor source — `06 04 15 PP`

| `PP` | Source | Value |
|------|--------|-------|
| `08` | 2TR select | `04`=2TR A1, `05`=2TR A2 (2TR D1/D2/D3 transmit nothing) |
| `09` | Stereo | `01` / `00` |
| `0A` | Assign 1 | `01` / `00` |
| `0B` | Assign 2 | `01` / `00` |

## Monitor switches — `06 04 15 PP` (`01` on / `00` off)

| `PP` | Control |
|------|---------|
| `0C` | Mono |
| `0D` | Dimmer (also auto-fires when Talkback / Slate engage — talkback auto-dims) |
| `0F` | Small |

## Surround monitor — `06 04 15 PP`

| `PP` | Control | Value |
|------|---------|-------|
| `10` | Surround group (Assign1 / Assign2 / Bus) | `01` / `02` / `04` |
| `13` | Surround Monitor Level knob | `00`–`7F` continuous |

## Talkback — `06 04 15 PP` (`01` press / `00` release)

| `PP` | Control |
|------|---------|
| `11` | Talkback |
| `12` | Slate |

## Solo — element `06 03 2D 03`

| Address | Control | Value |
|---------|---------|-------|
| `06 03 2D 03` | Solo contrast / dim knob | `00`–`7F` continuous |

## Stereo master strip — channel-strip controls (mixed encodings)

| Control | Message(s) | Value |
|---------|-----------|-------|
| AUTO | `F0 43 10 3E 06 04 54 00 0n 00 00 00 VV F7` (sub-indices `00`/`01` both fire) | `VV` 01/00 |
| SEL | `F0 43 10 3E 06 04 5E 00 0n 00 00 00 VV F7` — also `… 7F 01 4D …` SysEx + CC `B4 77 VV` (ch 5) | 01/00 |
| ON | `F0 43 10 3E 06 04 09 21 00 00 00 00 VV F7` (coupled `09 18` = level) | 01 on / 00 off |
| Fader | **14-bit CC on ch 4**: `B3 1D <msb>` + `B3 3D <lsb>`; also `F0 43 10 3E 7F 01 4F 00 01 00 00 HH LL F7` | position 0 … `07 7F` (~0–1023) |

The stereo-master **fader** is a clean 14-bit value — it could both drive and (via the
SysEx set-form) be driven by the REAPER master fader. AUTO / SEL / ON are addressable too.

## Not captured / silent

- `15 0E` (gap in the monitor block), the SOLO **CLEAR** button, and 2TR **D1/D2/D3**
  all transmit nothing.
- **EQ / DYNAMICS / SELECTED CHANNEL** emit **no MIDI on any of the 8 ports** (tested
  2026-06-26). So EQ is not controllable from the desk even on this native layer — it
  covers the monitor / master sections only, not channel DSP. This confirms the
  long-standing "EQ not doable" finding, now on every port rather than just HUI.

## Implementation notes (future "control room" layer)

- **Receive path already exists in spirit:** the plugin opens the GENERAL port for scene
  recall (`OnMIDIEvent` already sees port-5 SysEx). Parsing `F0 43 10 3E 06 04 15 PP …`
  there would let TALKBACK / SLATE / DIM / MONO / SMALL / surround-monitor fire
  configurable REAPER actions (e.g. a talkback send, monitor dim, mono-check).
- **Stereo master:** the 14-bit fader (`B3 1D/3D`) and ON (`06 04 09 21`) map naturally to
  REAPER's master volume / mute; feedback is possible via the SysEx set-form.
- All of this would be opt-in config (like `[scene]`), since it rides the GENERAL port.
