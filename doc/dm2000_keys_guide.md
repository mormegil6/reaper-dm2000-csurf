# dm2000_keys.ini guide

This guide explains how to configure the `dm2000_keys.ini` file that controls
which REAPER actions the DM2000 Locate Memory and User Defined Key buttons
trigger.

## Where the file lives

The build/install step places `dm2000_keys.ini.example` in REAPER's resource
folder as `dm2000_keys.ini` if the file does not already exist:

- Windows: `%APPDATA%\REAPER\dm2000_keys.ini`
- macOS: `~/Library/Application Support/REAPER/dm2000_keys.ini`

The plugin looks there by default. You can override the path in the DM2000
config dialog (REAPER Preferences > Control Surfaces > DM2000 > Configure).

## Finding REAPER action IDs

1. In REAPER: **Actions > Show action list**
2. Search for the action by name
3. Right-click the action > **Copy selected action command ID**
4. Paste the number into the ini file

The same list includes SWS/ReaPack actions once those extensions are installed.
Action IDs are integers; the ini file uses decimal format.

## File format

```ini
[locate]
key = action_id

[udk]
key = action_id
```

- `0` disables the button (silently ignored)
- Omitting a key uses the compiled-in default (RETURN TO ZERO/END use the API;
  all others default to 0)
- Lines starting with `;` are comments

## [locate] section - button reference

The **Default** column below shows the value shipped in `dm2000_keys.ini.example`.
The plugin's compiled-in default for every button is `0` (no action; RETURN TO
ZERO and END fall back to the go-to-start/end API) - the meaningful action IDs
come from the example ini, not the binary.

### Row 2 (above transport)

| Key | Button | Default | Notes |
|-----|--------|---------|-------|
| `rtz` | RETURN TO ZERO | 0 (GoStart API) | Returns playhead to project start |
| `end` | END | 0 (GoEnd API) | Moves playhead to project end |
| `online` | ONLINE | 0 | No standard REAPER equivalent; repurpose freely |
| `loop` | LOOP | 1068 | Toggles repeat mode |
| `qpunch` | QUICK PUNCH | 40157 | Insert marker (default); see alternatives below |

### Row 1

| Key | Button | Default | Notes |
|-----|--------|---------|-------|
| `audition` | AUDITION | 0 | No standard REAPER equivalent; repurpose freely |
| `pre` | PRE | 0 | No standard REAPER equivalent; repurpose freely |
| `in` | IN | 40222 | Set loop start point |
| `out` | OUT | 40223 | Set loop end point |
| `post` | POST | 40174 | Insert region from time selection |

### Locate Memory buttons

| Key | Button | Default |
|-----|--------|---------|
| `lm1` - `lm8` | LM1 - LM8 | 40161 - 40168 (Go to markers 1-8) |

## Useful action IDs

Verify these in your REAPER version via the action list - IDs are stable across
versions but worth confirming.

### Transport and navigation

| ID | Name |
|----|------|
| 40042 | Transport: Go to start of project |
| 40043 | Transport: Go to end of project |
| 40044 | Transport: Play/stop |
| 40046 | Transport: Play (with pre-roll) |
| 1068 | Transport: Toggle repeat |
| 40172 | Transport: Toggle auto-punch record mode |
| 40364 | Transport: Toggle loop points with time selection |

### Loop points

| ID | Name |
|----|------|
| 40222 | Transport: Set loop start point |
| 40223 | Transport: Set loop end point |

### Markers

| ID | Name |
|----|------|
| 40157 | Insert marker at current position |
| 40171 | Insert time signature marker at current position |
| 40174 | Insert region from time selection |
| 40161 | Go to marker 1 (40162-40168 for markers 2-8) |

### Editing

| ID | Name |
|----|------|
| 40029 | Edit: Undo |
| 40030 | Edit: Redo |
| 40022 | File: Save project |

### Track

| ID | Name |
|----|------|
| 40182 | Track: Toggle mute for selected tracks |

## Example configurations

### REAPER-native (default in dm2000_keys.ini.example)

Locate buttons follow REAPER conventions: IN/OUT set loop points, LM1-8 jump
to markers, LOOP toggles repeat, QPUNCH inserts a marker.

### Pro Tools-style

Buttons match what they do in Pro Tools:

```ini
[locate]
rtz     = 0       ; go to start (same as default)
end     = 0       ; go to end (same as default)
loop    = 1068    ; toggle loop (same as default)
qpunch  = 40172   ; toggle auto-punch instead of insert marker
in      = 40222   ; set loop start (same as default)
out     = 40223   ; set loop end (same as default)
post    = 0       ; disable POST (Pro Tools uses it for post-roll, no equivalent)
lm1     = 40161   ; go to marker 1 (same as default)
; ... lm2-lm8 same
```

### Minimal - only essential buttons active

```ini
[locate]
rtz    = 0      ; go to start
end    = 0      ; go to end
loop   = 1068   ; toggle repeat
in     = 40222  ; set loop start
out    = 40223  ; set loop end
; everything else = 0 (disabled)
```

## [udk] section - User Defined Keys

The UDK dispatcher is implemented. Each `keyN = action_id` maps User Defined Key
N (1-16) to a REAPER action fired via `Main_OnCommand` on press; `0` or an omitted
key does nothing.

| Key | UDK | Zone/sw | | Key | UDK | Zone/sw |
|-----|-----|---------|---|-----|-----|---------|
| `key1` | 1 | 0x09 sw2 | | `key9` | 9 | 0x09 sw0 |
| `key2` | 2 | 0x0A sw1 (BANK ◄) | | `key10` | 10 | 0x0A sw0 (CH ◄) |
| `key3` | 3 | 0x0A sw3 (BANK ►) | | `key11` | 11 | 0x0A sw2 (CH ►) |
| `key4` | 4 | 0x08 sw1 | | `key12` | 12 | 0x08 sw0 |
| `key5` | 5 | 0x08 sw5 | | `key13` | 13 | 0x08 sw4 |
| `key6` | 6 | 0x19 sw5 | | `key14` | 14 | 0x19 sw1 |
| `key7` | 7 | 0x19 sw3 | | `key15` | 15 | 0x08 sw3 |
| `key8` | 8 | 0x19 sw4 | | `key16` | 16 | 0x08 sw7 |

All 16 buttons are hardware-verified (2026-06-16 full in-order capture) and
dispatchable.

**Navigation fallback:** UDK 2/3/10/11 are physically the **BANK ◄/►** and **CH
◄/►** buttons, which move the fader bank by default. They keep that behavior
*unless* you set the matching key here - then your action overrides navigation on
that one button. Leave them unset to keep banking.

Zone/switch assignments are also documented in `dm2000_keys.ini.example` and
`DESIGN.md`.

## [counter] section - LED timecode refresh

`refresh_ms` sets how often the 8-digit LED timecode display repaints (20-1000 ms,
default 33 ≈ 30 Hz). The default gives smooth SMPTE frame counting; only digits
that changed are transmitted, so a fast refresh costs little. Raise it (e.g. 100)
to cut MIDI traffic if you only show minutes:seconds.

```ini
[counter]
refresh_ms = 33
```

## [surround] section - MCS PANNER surround pan

In Pro Tools mode the DM2000 **DYNAMICS** knobs and the **surround joystick**
(USB port 4) drive a surround plugin on the *selected* track - they never insert
one. Configure which plugin and which of its parameters each control moves:

| Key | Control | Default | ReaSurroundPan param |
|-----|---------|---------|----------------------|
| `plugin` | FX name to match (`TrackFX_GetByName`) | `ReaSurroundPan` | - |
| `param_front` | THRESHOLD knob + joystick Y | 8 | in 1 Y (front/back) |
| `param_rear` | ATTACK knob | 9 | in 1 Z (height) |
| `param_lr` | DECAY knob + joystick X | 7 | in 1 X (left/right) |
| `param_lfe` | RANGE knob | 10 | in 1 LFE |
| `param_vol` | HOLD knob | 6 | in 1 gain |

The compiled-in default is **none** - without a `[surround]` section the MCS
PANNER controls do nothing (same convention as `[locate]`/`[udk]`). The values
below ship in `dm2000_keys.ini.example` and target **input 1** of a stock
ReaSurroundPan. The `param_*` values are plain parameter indices - change them
for a different plugin, a different input, or a different feel. To discover
indices, select a track that has the plugin and press **UDK 16** with no `key16`
mapping: the plugin prints every parameter index and name to the REAPER console
(`View > Console`).

```ini
[surround]
plugin = ReaSurroundPan
param_front = 8
param_rear  = 9
param_lr    = 7
param_lfe   = 10
param_vol   = 6
stride  = 9   ; params per object (see below)
objects = 0   ; max object count, 0 = no limit
```

### Switching the controlled object (ROUTING buttons + Direct)

The `param_*` above are **object 1**. The **ROUTING buttons 1-8** select object 1-8
within the current bank - each adds `(object-1) x stride` to every `param_*` index.
The **Direct** button shifts the bank of 8: **single press = up** (objects 9-16,
17-24...), **double press = down** (no wrapping, so it scales to Atmos-size sessions).

- `stride` = the panner's parameters-per-object. ReaSurroundPan is **9** (gain, X, Y,
  Z, LFE, divergence, delay, mute, solo). `0` disables object switching.
- `objects` = how many objects to stop at when banking up (`0` = no top limit).

The selected object is printed to the REAPER console as you switch. (Lighting the
routing buttons to show the selection is a planned addition.)

## SWS extension actions

If you have the [SWS extension](https://www.sws-extension.org/) installed, its
actions appear in the same REAPER action list and can be used here. Examples
worth exploring: cycle actions, auto-coloring, region/marker operations. Find
IDs the same way - action list > right-click > copy ID.
