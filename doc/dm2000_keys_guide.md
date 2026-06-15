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
- Omitting a key uses the compiled-in default (RTZ/END use the API; all
  others default to 0)
- Lines starting with `;` are comments

## [locate] section - button reference

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

The UDK dispatcher is not yet implemented. The `[udk]` section is reserved for
a future release. Zone/switch assignments for the confirmed UDK buttons are
documented in `dm2000_keys.ini.example` and in `DESIGN.md`.

## SWS extension actions

If you have the [SWS extension](https://www.sws-extension.org/) installed, its
actions appear in the same REAPER action list and can be used here. Examples
worth exploring: cycle actions, auto-coloring, region/marker operations. Find
IDs the same way - action list > right-click > copy ID.
