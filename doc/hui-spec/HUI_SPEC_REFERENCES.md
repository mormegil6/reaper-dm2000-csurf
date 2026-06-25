# HUI Protocol Specification — References

The canonical reverse-engineered **Mackie HUI** MIDI protocol, by
**theageman ("AgeMan")**, 2010. Compiled document © 2011 **SSEI** —
[ssei-online.de](http://www.ssei-online.de).

This is the reference the DM2000's Pro Tools / HUI emulation is built on, and the
basis for our protocol work. All credit for the reverse-engineering belongs to
the original author. See [../hui-canonical-coverage.md](../hui-canonical-coverage.md)
for how this plugin maps onto it.

## Original sources

| File | What it covers | Original link |
|---|---|---|
| `HUI.pdf` | Full compiled document (all of the below + hardware-layout diagram) | <https://stash.reaper.fm/12332/HUI.pdf> |
| `HUIZONES.txt` | Zone / port map for LEDs, switches, faders | <https://stash.reaper.fm/12337/HUIZONES.txt> |
| `HUIREFTX.txt` | Command reference — transmitting (host → surface) | <https://stash.reaper.fm/12336/HUIREFTX.txt> |
| `HUIREFRX.txt` | Command reference — receiving (surface → host) | <https://stash.reaper.fm/12334/HUIREFRX.txt> |
| `HUI_CSET.txt` | Small-display character set | <https://stash.reaper.fm/12333/HUI_CSET.txt> |
| `HUICSET2.txt` | Main / large-display character set | (host offline) [Internet Archive snapshot](https://web.archive.org/web/20120623005534/http://www.ssei-online.de:80/HUICSET2.txt) |

**Original announcement / discussion:**
<https://forum.cockos.com/showthread.php?t=101328>

## What is mirrored here, and why

The five **`.txt`** files are mirrored into this folder (`doc/hui-spec/`) as the
original text (the four UTF-16 files transcoded to UTF-8 for readability, content
unchanged); provenance and credit live in this file. They carry author attribution
but no copyright notice, and the author explicitly invited redistribution and
extension of the text ("…please feel free to extend this document with your
knowledge"). `HUICSET2.txt` in particular is mirrored because its original host is
offline and it survives only on the Internet Archive.

**`HUI.pdf` is *not* mirrored here.** It is the one file carrying an explicit
"© 2011 by SSEI" notice, and it remains durably hosted on the REAPER stash, so it
is linked above rather than copied.

These mirrors are kept for archival / educational purposes with full credit to the
original author, and will be removed on request from the author or rights-holder.
