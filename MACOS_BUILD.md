# Building reaper_csurf_dm2000 on macOS

This is the handoff for finishing the macOS port on a Mac. The Windows source was
prepared on Windows (which cannot compile a `.dylib`), so the remaining work is:
clone WDL, run `make`, fix any compile residuals, and verify in REAPER. Everything
platform-specific is already guarded behind `#ifdef _WIN32`, so none of this affects
the Visual Studio Windows build.

You can drive this with a Claude Code session on the Mac - the steps below are written
to be followed top to bottom.

---

## How the macOS build works (one paragraph)

REAPER hosts SWELL (its Win32-compatibility layer). The extension compiles against
SWELL's Win32-shaped headers (`swell.h`, pulled in automatically by
`Source/jmde/reaper_plugin.h` on non-Windows) and links exactly **one** SWELL source
file - `WDL/swell/swell-modstub.mm`, built with `-DSWELL_PROVIDED_BY_APP` - which binds
every SWELL call to REAPER's own implementation at load time. The full SWELL library is
**not** built. The Win32 dialog (`res.rc`) is transpiled by `swell_resgen.php` into
`res.rc_mac_dlg` / `res.rc_mac_menu`, which `csurf_main.cpp` already `#include`s under
`#ifndef _WIN32`.

---

## Prerequisites

```sh
xcode-select --install        # clang++ (skip if already installed)
brew install php              # runs the SWELL resource generator
clang++ --version             # confirm it runs
php --version                 # confirm it runs
```

---

## Step 1 - obtain WDL (provides SWELL)

WDL is not vendored in this repo. The existing `#include "../WDL/swell/swell.h"` in
`Source/jmde/reaper_plugin.h` (and `../../WDL/swell/...` in `csurf_main.cpp`) resolve to
`Source/WDL`, so clone WDL there:

```sh
cd <repo root>
git clone --depth 1 https://github.com/justinfrankel/WDL.git /tmp/WDL
cp -R /tmp/WDL/WDL Source/WDL          # note: inner WDL dir -> Source/WDL
```

Verify these all exist:

```sh
ls Source/WDL/swell/swell.h \
   Source/WDL/swell/swell-modstub.mm \
   Source/WDL/swell/swell_resgen.php \
   Source/WDL/db2val.h Source/WDL/wdlstring.h \
   Source/WDL/mutex.h Source/WDL/ptrlist.h
```

(If `swell_resgen.php` is absent but `mac_resgen.php` exists, that's an older WDL - the
Makefile auto-detects either name. Prefer current WDL.)

`Source/WDL` is referenced only by the Makefile and by includes already inside
`#ifndef _WIN32`; it is not in the `.vcxproj`, so the Windows build never sees it.
Consider adding `Source/WDL/` and `res.rc_mac_*` to `.gitignore` rather than committing
the whole library.

---

## Step 2 - sanity-check the resource generator

```sh
php Source/WDL/swell/swell_resgen.php --quiet Source/jmde/csurf/res.rc
```

This writes `Source/jmde/csurf/res.rc_mac_dlg` and `res.rc_mac_menu`. Open
`res.rc_mac_dlg` and find the `IDD_SURFACEEDIT_DM2000` block. Confirm it contains the
port-group combo, the edit field, the "Open folder" button, the etched divider, and the
`SS_NOTIFY` static for the footer link - and **not** a `SysLink` line.

> If resgen mishandled the `#ifdef _WIN32 / #else / #endif` around the footer (e.g. the
> `SysLink` line leaked through, or both lines appeared), just edit `res.rc` so the
> footer is only the `SS_NOTIFY` static (the `SysLink` branch matters only to the
> Windows resource compiler, and `res.rc_mac_dlg` is Mac-only). The Makefile regenerates
> it on the next build.

---

## Step 3 - build

```sh
cd Builds/Make
make
```

**Compile fixes already applied (2026-06-14):**

Two issues were found and fixed during the first Mac compilation pass — both are already
committed, so a clean clone will not hit them:

1. **Include path for `WDL/db2val.h`** — Clang could not resolve `#include "WDL/db2val.h"`
   because the Makefile's `$(WDL)` variable points to `Source/WDL` itself (not its parent).
   Fixed by adding `-I../../Source` to `INCS` in `Builds/Make/Makefile`.

2. **C++11 narrowing in scaffold files** — Clang (`-std=c++14`) rejects implicit int-to-`unsigned char`
   narrowing inside brace-initialiser lists for `MIDI_event_t`. MSVC silently allows this;
   Clang treats it as a hard error. Fixed with explicit `(unsigned char)` casts in five
   scaffold files that are not DM2000-specific (`csurf_mcu.cpp`, `csurf_faderport.cpp`,
   `csurf_babyhui.cpp`, `csurf_alphatrack.cpp`, `csurf_01X.cpp`).

Expected remaining culprits if it doesn't link cleanly:

- A Windows symbol used outside an `#ifdef _WIN32` that SWELL doesn't provide - wrap it
  or use the SWELL equivalent.
- An undefined `csurf_*_reg` at link time - a surface `.cpp` is missing from `CPPSRCS`
  in the Makefile (the set must match the externs in `csurf_main.cpp`).
- `clang` rejecting `-mmacosx-version-min=10.9` for the arm64 slice. If so, switch to a
  two-pass build and `lipo`:

  ```sh
  clang++ -arch x86_64 -mmacosx-version-min=10.9 ... -o reaper_csurf_dm2000-x86_64.dylib
  clang++ -arch arm64  -mmacosx-version-min=11.0 ... -o reaper_csurf_dm2000-arm64.dylib
  lipo -create -output reaper_csurf_dm2000.dylib \
       reaper_csurf_dm2000-x86_64.dylib reaper_csurf_dm2000-arm64.dylib
  ```

Re-run `make` until it links `reaper_csurf_dm2000.dylib`.

---

## Step 4 - verify the binary

```sh
lipo -archs Builds/Make/reaper_csurf_dm2000.dylib        # -> "x86_64 arm64"
nm -gU Builds/Make/reaper_csurf_dm2000.dylib | grep ReaperPluginEntry   # entrypoint exported
otool -L Builds/Make/reaper_csurf_dm2000.dylib          # AppKit/Cocoa, NO swell dylib
```

---

## Step 5 - install and smoke-test in REAPER

```sh
cd Builds/Make && make install     # -> ~/Library/Application Support/REAPER/UserPlugins/
```

Launch REAPER, then:

1. Preferences -> Control/OSC/web -> Add. Confirm **Yamaha DM2000** is in the list.
   *(Verified 2026-06-14: plugin loads and appears in the list.)*
2. Add it; confirm the config dialog renders: port-group combo populated, User Keys
   edit field, "Open folder" button, etched divider, and the clickable bmroz.eu footer.
3. Click the footer static -> default browser opens `https://bmroz.eu/projects/dm2000-csurf`.
4. Click "Open folder" -> Finder opens the keys-file directory.
5. Confirm the keys-file path resolves under `~/Library/Application Support/REAPER`
   (via `GetResourcePath`).
6. With the DM2000 connected over USB, confirm the surface itself works (faders,
   transport, meters, scribble strips) - the surface logic is platform-independent, so
   once it loads it should behave exactly as on Windows.
   *(Verified 2026-06-15 with DM2000 connected: all features work on macOS.)*

---

## Step 6 - confirm the Windows build is untouched

```sh
git status
```

Should show only: `Builds/Make/Makefile`, `Source/jmde/csurf/dm2000_compat.h`,
`MACOS_BUILD.md`, untracked `Source/WDL/` and `res.rc_mac_*`, and edits that live inside
`#ifdef` guards (`csurf_dm2000.cpp`, `csurf.h`, `csurf_main.cpp`, `res.rc`, `resource.h`).
No `.vcxproj` / `.sln` changes.

---

## What was changed on the Windows side (all guarded, Windows behavior unchanged)

- `Builds/Make/Makefile` - new; the macOS build.
- `Source/jmde/csurf/dm2000_compat.h` - new; `sprintf_s`/`strcpy_s`/`SetDlgItemTextA`/
  `GetDlgItemTextA` shims + `DM2000_PATHSEP`. Empty on Windows.
- `csurf_dm2000.cpp` - Windows-only includes guarded; `GetIniPath` gains a macOS branch
  using `GetResourcePath`; folder open and the footer-link click use SWELL `ShellExecute`
  on macOS; `WM_CTLCOLOREDIT` and the `WM_NOTIFY`/`NMLINK` handler guarded under
  `#ifdef _WIN32`.
- `csurf.h` / `csurf_main.cpp` - declare and non-fatally resolve `GetResourcePath` via
  `rec->GetFunc` (does not affect the IMPAPI errcnt, so a REAPER lacking it still loads).
- `res.rc` - footer is the Windows `SysLink` under `#ifdef _WIN32`, an `SS_NOTIFY` static
  otherwise; `resource.h` adds `IDC_DM2000_LINK`.

## Known open risks (see DESIGN.md)

- The `res.rc` `#ifdef` around the footer relies on resgen passing preprocessor lines
  through; verify in Step 2 and fall back to the note there if needed.
- The single-fat `-mmacosx-version-min=10.9` build; fall back to per-arch + `lipo` if
  clang objects (Step 3).
- Only the slice matching the build Mac's CPU is runtime-verified; `lipo -archs` confirms
  both slices are present but the other is statically verified only.
