// dm2000_compat.h - small portability shims for the macOS (SWELL) build.
//
// Included by csurf_dm2000.cpp right after the platform headers. On Windows the
// whole file is empty, so the Win32 build is byte-for-byte unchanged. On macOS
// it maps the few MSVC/Win32-only helpers the DM2000 code uses onto SWELL/libc
// equivalents. SWELL itself (swell.h) is already pulled in via
// reaper_plugin.h on non-Windows, so it provides ShellExecute, HWND, RGB,
// MAX_PATH, lstrcpyn, the dialog/combo messages, etc.

#ifndef DM2000_COMPAT_H
#define DM2000_COMPAT_H

// Path separator used when composing/splitting the keys-file path.
#ifdef _WIN32
#define DM2000_PATHSEP '\\'
#else
#define DM2000_PATHSEP '/'
#endif

#ifndef _WIN32

#include <cstdio>
#include <cstring>
#include <cstddef>
#include <cstdlib>   // atoi (scene-marker / config parsing) - transitive on Win32, explicit here

// MSVC secure-CRT helpers used by the DM2000 code. Every call site passes the
// buffer size explicitly as the 2nd argument, so the snprintf/bounded-copy
// mappings are exact. (lstrcpyn is NOT redefined - SWELL provides it.)
#define sprintf_s(buf, sz, ...) snprintf((buf), (sz), __VA_ARGS__)

static inline void strcpy_s(char *dst, size_t sz, const char *src)
{
    if (!sz) return;
    size_t n = strlen(src);
    if (n >= sz) n = sz - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

// SWELL exposes the narrow names without the A suffix.
#define SetDlgItemTextA SetDlgItemText
#define GetDlgItemTextA GetDlgItemText

#endif // !_WIN32

#endif // DM2000_COMPAT_H
