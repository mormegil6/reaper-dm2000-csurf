#!/usr/bin/env python3
"""
hui_send.py - interactive sender: drive ANY DM2000 surface element by hand.

Keeps the desk online itself (answers the keepalive on every port) and gives you
a small command language, organised by the desk's physical sections, to light
LEDs, move faders, drive the meter bridge, and write the scribbles / counter /
displays. It builds the MIDI for you - you never hand-encode a byte, and you ask
for *results* (a colour, an on/off, a 0..1 level), not DAW functions.

Every lit button on the desk is just a (zone, switch) pair, so `led <zone> <sw>`
can drive *any* of them; the named sections below are convenience aliases for the
common ones. `all_on` lights everything; `clear` wipes everything.

REQUIREMENT: the DM2000 ports must be free - close REAPER / Pro Tools / MIDI-OX
first (this tool holds the ports and keeps the desk online on its own).

Nothing is cleared on exit - whatever you set stays while this runs; the desk only
reverts because it goes offline when the tool quits. `clear` is the explicit wipe.

Run:   py hui_send.py        (type `help`; `quit` or Ctrl-C exits)

Examples:
    channel 1 sel on            channel 1 SELECT lights
    channel 1 on off            channel 1 ON button dark (= muted)
    channel 1 ringled on        channel 1 V-pot ring blinks
    channel 1 ring 0.5          channel 1 pan ring to center
    channel 3 auto red          channel 3 AUTO indicator -> red
    channel 1 meter 0.5 1       channel 1 meter: L = half, R = clip
    automix rec on              AUTOMIX REC (Write) button lights
    effects bypass on           EFFECTS/PLUG-INS BYPASS box lights
    selassign Rvb               SELECT ASSIGN readout
    counter 1:23.456            LED counter
    all_on  /  clear            light everything / wipe everything
"""
import sys
import shlex
import threading
import rtmidi
from hui_common import find_port, dm2000_daw_ports

HUI = [0xF0, 0x00, 0x00, 0x66, 0x05, 0x00]   # display-SysEx header

# Channel-LED switch numbers (zone = the channel itself, 0-7 within its port).
# The ON button (sw2) is *inverted* (lit = unmuted) so it is handled separately.
# The arm/REC LED (sw7) is driven by the `trackarm` command - it physically lives in the
# TRACK ARMING row, not on the strip above the fader.
CHAN_LED = {"sel": 1, "solo": 3, "ringled": 5, "ins": 6}

# AUTO indicator colour byte (a result, not a DAW function): green/orange/red/off
AUTO_COLOR = {"off": 0x04, "green": 0x34, "orange": 0x64, "red": 0x44}

# Counter-mode indicator LEDs (zone 0x16)
COUNTER_MODE = {"timecode": 0, "feet": 1, "beats": 2}

# Global-button sections: name -> (broadcast_ports, {button-label: (zone, sw)}).
# Hardware-captured zone/sw. AUTOMIX broadcasts on ports 1-3 like the plugin; the
# rest are driven on port 1 (if a "global" button won't light on P1 alone, broadcast
# it like AUTOMIX - some of these report their presses on P1-3).
SECTIONS = {  # listed in the desk's physical panel order
    "matrix":    ([0], {"1": (0x0C, 1), "2": (0x0C, 4), "4": (0x0C, 5)}),   # MATRIX SELECT (3 silent; 4 LED dark)
    "aux":       ([0], {"1": (0x0B, 7), "2": (0x0B, 6), "3": (0x0B, 5),
                        "4": (0x0B, 4), "5": (0x0B, 3)}),   # AUX SELECT 1-5 (ENCODER MODE). aux1/sw7
    #   IS the AUX/MTRX key (identical MIDI); the AUX/MTRX label-LED auto-lights with any aux (desk-driven)
    "encoder":   ([0], {"pan": (0x0B, 2), "assign1": (0x0B, 1), "assign2": (0x0B, 0),
                        "assign3": (0x0C, 0)}),   # ENCODER MODE (assign3 = 0x0C sw0; LED is console-local)
    "effects":   ([0], {"param": (0x1C, 0), "insert": (0x1C, 0),   # same box: PARAM lit / INSERT dark
                        "assign": (0x1C, 1), "compare": (0x1C, 7), "bypass": (0x1C, 6),
                        "sel1": (0x1C, 2), "sel2": (0x1C, 3), "sel3": (0x1C, 4), "sel4": (0x1C, 5),
                        # PANEL EFFECTS/PLUG-INS button labels 5-8 = the F1-F4 soft keys (same MIDI):
                        "5": (0x1C, 1), "6": (0x1C, 7), "7": (0x1C, 6), "8": (0x1C, 0)}),  # 5=F1..8=F4
    "automix":   ([0, 1, 2], {"relative": (0x18, 0), "autorec": (0x18, 1), "return": (0x18, 2),
                              "touch": (0x18, 3), "rec": (0x18, 4), "abort": (0x18, 5)}),
    "overwrite": ([0], {"eq": (0x17, 0), "pan": (0x17, 1), "fader": (0x17, 2),
                        "auxon": (0x17, 3), "aux": (0x17, 4), "on": (0x17, 5)}),   # AUTOMIX OVERWRITE
    "locate":    ([0], {"rtz": (0x0F, 0), "end": (0x0F, 1), "online": (0x0F, 2),
                        "loop": (0x0F, 3), "qpunch": (0x0F, 4), "audition": (0x10, 0),
                        "pre": (0x10, 1), "in": (0x10, 2), "out": (0x10, 3), "post": (0x10, 4)}),
    "control":   ([0], {"rew": (0x0E, 1), "ff": (0x0E, 2), "stop": (0x0E, 3),
                        "play": (0x0E, 4), "rec": (0x0E, 5),
                        "shuttle": (0x0D, 5), "scrub": (0x0D, 6)}),   # transport + shuttle/scrub
    # BACK (0x08 sw2) / FORWARD (0x08 sw6) transmit but have NO LED. FADER MODE is a command
    # (cmd_fadermode) - FADER<->AUX/MTRX is an exclusive pair on one switch.
}

# USER DEFINED KEYS: number -> (zone, sw). PROVISIONAL - the ini-derived map is off
# (e.g. UDK9 = 0x09 sw1, not sw0). Capture UDK 1..16 in order to finalise; `udk <n>` below.
UDK_KEYS = {1: (0x09, 2), 2: (0x0A, 1), 3: (0x0A, 3), 4: (0x08, 1),
            5: (0x08, 5), 6: (0x19, 5), 7: (0x19, 3), 8: (0x19, 4),
            9: (0x09, 1), 10: (0x0A, 0), 11: (0x0A, 2), 12: (0x08, 0),
            13: (0x08, 4), 14: (0x19, 1), 15: (0x08, 3), 16: (0x08, 7)}

# LOCATE MEMORY 1-8: number -> (zone, sw).  NOTE: these LEDs may be console-local
# (they did not light from HUI output in testing) - kept for completeness / future capture.
LM_KEYS = {1: (0x13, 1), 2: (0x13, 3), 3: (0x13, 6), 4: (0x13, 2),
           5: (0x13, 4), 6: (0x13, 7), 7: (0x15, 0), 8: (0x15, 1)}

# MCS PANNER ROUTING buttons 1-8 live on PORT 4 with their own encoding (not zone/sw):
# lit = BE 00 N, dark = BE 01 N, where N follows this 2-column wire order. The console
# powers them on by default, so "off" is an explicit BE 01.
ROUTING_WIRE = [0, 4, 1, 5, 2, 6, 3, 7]

HELP = """\
commands  (channels 1-based; results not DAW functions;  [a|b]=pick one,  <x>=a value)

CHANNEL STRIPS   (top of the strip down to the fader)
  channel <ch> meter <L 0..1> <R 0..1>            meter bridge at the top (1.0 = clip)
  channel <ch> auto [off|green|orange|red]        AUTO indicator colour
  channel <ch> [sel|solo|on] [on|off]             button LEDs (on = the ON button; lit = unmuted)
  channel <ch> [ins|ringled] [on|off]             strip display: INS icon / V-pot ring blink
  channel <ch> ring <0..1>                        V-pot ring position (0.5 = center)
  channel <ch> name <text>                        4-char name just above the fader
  channel <ch> fader <0..1>                       fader at the bottom (0.5 = middle)
   (the arm/REC LED isn't on the strip - it lives in TRACK ARMING; see `trackarm`)

PANEL BUTTONS   (each: a button or number, then [on|off])
  matrix     [1|2|4]                                MATRIX SELECT (3 silent; 4 LED is console-local)
  aux        [1|2|3|4|5]                            AUX SELECT (AUX/MTRX label-LED auto-lights with aux)
  encoder    [pan|assign1|assign2|assign3]          ENCODER MODE (assign3 LED is console-local)
  fadermode  [fader|auxmtrx]                        FADER MODE - one exclusive pair (fader OR aux/mtrx)
  effects    [5|6|7|8]                              EFFECTS/PLUG-INS panel buttons (1-4 send no MIDI;
             5-8 = the F1-F4 soft keys, SAME MIDI as INSERT ASSIGN/EDIT on the display)
  routing    <1-8|direct>                           MCS PANNER ROUTING (port 4)
  trackarm   <1-24|master> [on|off]                 TRACK ARMING: per-channel arm LEDs + master
  automix    [relative|autorec|return|touch|rec|abort]
  overwrite  [eq|pan|fader|auxon|aux|on]            AUTOMIX-OVERWRITE
  udk <1-16>                                        USER DEFINED KEYS (provisional; udk9 LED not assignable)
  lm <1-8>                                          LOCATE MEMORY (LEDs are console-local)
  locate     [rtz|end|online|loop|qpunch|audition|pre|in|out|post]

TRANSPORT / JOG
  control    [rew|ff|stop|play|rec|shuttle|scrub] [on|off]

DISPLAY   (the one big LCD and the readouts around it)
  selassign <text>                                SELECT ASSIGN readout (4 chars)
  counter <0-9 . : ,>                             8-digit LED counter, right-aligned; e.g. 1:23.456
  cursormode [nav|zoom|select]                    CURSOR MODE (select flashes ~1s, settles to zoom)
  countermode [timecode|feet|beats] [on|off]      counter-mode indicator
  INSERT ASSIGN/EDIT (on-screen FX area; its buttons are EFFECTS/PLUG-INS - either listing works):
    effects [param|insert|assign|compare|bypass|sel1|sel2|sel3|sel4] [on|off]   ASSIGN/COMPARE/BYPASS/SEL
    remotelcd <line 1-8> <text>                   the big message space (8 lines x 10 chars)
    selring <1-4> <0..1> [dot|boostcut|fill|spread]  the SEL rings (mode = ring shape; 0.5 = mid)

LOW-LEVEL / UTILITY
  led <zone> <sw> [on|off]                        raw global LED - drives ANY button, e.g. led 0x17 0 on
  raw [port 1-4] <hex...>                         arbitrary bytes (echo shows port for replay)
  all_on                                          light every LED / meter / fader / strip / display
  clear                                           blank every LED / meter / fader / strip / display
  help / quit
"""


class Sender:
    """Holds one MidiOut per DAW port, keeps the desk online, and sends messages."""

    def __init__(self):
        self.outs = []
        self.ins = []
        self.lock = threading.Lock()

    def open(self):
        for name in dm2000_daw_ports(rtmidi.MidiIn()):
            midi_out, midi_in = rtmidi.MidiOut(), rtmidi.MidiIn()
            out_idx, out_label = find_port(midi_out, name)
            in_idx, _ = find_port(midi_in, name)
            if out_idx is None or in_idx is None:
                print(f"port '{name}' not found - skipping")
                self.outs.append(None)
                continue
            midi_out.open_port(out_idx)
            midi_in.open_port(in_idx)
            midi_in.set_callback(self._keepalive(midi_out))
            self.outs.append(midi_out)
            self.ins.append(midi_in)
            print(f"open: {out_label}")
        return any(o is not None for o in self.outs)

    def _keepalive(self, out):
        def callback(event, _data=None):
            msg = event[0]
            if len(msg) >= 3 and msg[0] == 0x90 and msg[1] == 0x00 and msg[2] == 0x7F:
                with self.lock:
                    out.send_message([0x90, 0x00, 0x7F])
        return callback

    def close(self):
        for midi_in in self.ins:
            midi_in.close_port()
        for midi_out in self.outs:
            if midi_out:
                midi_out.close_port()

    def send(self, port, *data):
        out = self.outs[port] if 0 <= port < len(self.outs) else None
        if out is None:
            print(f"port {port + 1} not available")
            return
        with self.lock:
            out.send_message(list(data))
        print("  -> P%d  %s" % (port + 1, " ".join("%02X" % b for b in data)))  # show the bytes

    # ---- text encoders ------------------------------------------------------
    @staticmethod
    def _chars(text, n):
        out = [(ord(c) & 0x7F) if 0x20 <= ord(c) < 0x7F else 0x20 for c in text[:n]]
        return out + [0x20] * (n - len(out))

    def scribble(self, port, cell, text):            # zone 0x10, 4 chars
        self.send(port, *HUI, 0x10, cell, *self._chars(text, 4), 0xF7)

    def remote(self, line0, text):                   # zone 0x12, 10 chars, port 1
        self.send(0, *HUI, 0x12, line0, *self._chars(text, 10), 0xF7)

    def counter(self, text):                         # zone 0x11, 8 positions, port 1
        # Build right-aligned, right-to-left, exactly like the plugin's SendCounter:
        # rightmost digit -> position 7; leading positions stay 0x20 (the desk blanks
        # leading 0x20, but renders a *trailing* 0x20 as "0"). Stream goes out pos 7 first.
        disp = [0x20] * 8
        dpos, sep = 7, False
        for c in reversed(text):
            if c.isdigit():
                if dpos < 0:
                    break
                disp[dpos] = (0x10 if sep else 0x00) | int(c)   # high nibble = separator after this digit
                dpos -= 1
                sep = False
            elif c in ".:,":
                sep = True
        self.send(0, *HUI, 0x11, *reversed(disp), 0xF7)

    def led(self, port, zone, sw, on):               # any zone/sw button LED
        self.send(port, 0xB0, 0x0C, zone)
        self.send(port, 0xB0, 0x2C, (0x40 if on else 0x00) | sw)


# ---- value parsers -----------------------------------------------------------
def _ch_port(ch1):
    """Channels are 1-based for the user (fader 1 = first strip) -> (port, cell)."""
    if not 1 <= ch1 <= 24:
        raise ValueError("channel must be 1..24")
    ch = ch1 - 1
    return ch // 8, ch & 7


def _unit(text):
    """Parse a 0.0..1.0 result value, clamped."""
    return max(0.0, min(1.0, float(text)))


def _onoff(text):
    return text.lower() == "on"


# ---- command handlers --------------------------------------------------------
def cmd_channel(s, a):
    port, cell = _ch_port(int(a[0]))
    elem, rest = a[1].lower(), a[2:]
    if elem == "on":                                 # ON button is inverted: lit = unmuted
        s.led(port, cell, 2, not _onoff(rest[0]))
    elif elem in CHAN_LED:
        s.led(port, cell, CHAN_LED[elem], _onoff(rest[0]))
    elif elem == "auto":
        s.send(port, 0xB0, 0x0C, cell)
        s.send(port, 0xB0, 0x2C, AUTO_COLOR[rest[0].lower()])
    elif elem == "fader":
        v = round(_unit(rest[0]) * 16383)
        s.send(port, 0xB0, cell, (v >> 7) & 0x7F)
        s.send(port, 0xB0, 0x20 + cell, v & 0x7F)
    elif elem == "meter":
        s.send(port, 0xA0, cell, round(_unit(rest[0]) * 12))
        s.send(port, 0xA0, cell, 0x10 | round(_unit(rest[1]) * 12))
    elif elem == "ring":
        s.send(port, 0xB0, 0x10 + cell, 1 + round(_unit(rest[0]) * 10))   # 1..11, 6=center
    elif elem == "name":
        s.scribble(port, cell, " ".join(rest))
    else:
        raise ValueError("channel elements: sel on solo ins ringled auto fader meter ring name")


def cmd_section(s, section, a):
    ports, buttons = SECTIONS[section]
    btn = a[0].lower()
    if btn not in buttons:
        raise ValueError(f"{section} buttons: {', '.join(buttons)}")
    zone, sw = buttons[btn]
    on = _onoff(a[1])
    for p in ports:
        s.led(p, zone, sw, on)


def cmd_selassign(s, a):                              # SELECT ASSIGN = scribble cell 8
    s.scribble(0, 8, " ".join(a))


def cmd_remotelcd(s, a):                              # big LCD, one of 8 lines x 10 chars
    line = int(a[0])
    if not 1 <= line <= 8:
        raise ValueError("remotelcd line must be 1..8")
    s.remote(line - 1, " ".join(a[1:]))


def cmd_counter(s, a):                                # 8-digit LED counter
    s.counter(" ".join(a))


def cmd_cursormode(s, a):                             # CURSOR MODE: nav / zoom / select
    mode = a[0].lower()
    if mode == "select":
        # SELECT has no steady code: the desk flashes it (~1s, then settles to ZOOM) when it
        # sees a fast NAV->ZOOM transition, so send both states back-to-back (hw 2026-06-18).
        for vv in (0x02, 0x42):
            s.send(0, 0xB0, 0x0C, 0x0D)
            s.send(0, 0xB0, 0x2C, vv)
        return
    s.send(0, 0xB0, 0x0C, 0x0D)
    s.send(0, 0xB0, 0x2C, 0x42 if mode == "zoom" else 0x02)   # zoom = sw2 on, nav = sw2 off


def cmd_countermode(s, a):                            # TIME CODE / FEET / BEATS indicator
    if a[0].lower() not in COUNTER_MODE:
        raise ValueError("countermode: timecode feet beats")
    s.led(0, 0x16, COUNTER_MODE[a[0].lower()], _onoff(a[1]))


def cmd_udk(s, a):                                    # USER DEFINED KEY n (1..16)
    n = int(a[0])
    if n not in UDK_KEYS:
        raise ValueError("udk number must be 1..16")
    zone, sw = UDK_KEYS[n]
    s.led(0, zone, sw, _onoff(a[1]))


def cmd_lm(s, a):                                     # LOCATE MEMORY n (1..8)
    n = int(a[0])
    if n not in LM_KEYS:
        raise ValueError("lm number must be 1..8")
    zone, sw = LM_KEYS[n]
    s.led(0, zone, sw, _onoff(a[1]))


def cmd_routing(s, a):                                # ROUTING button 1..8 or DIRECT (port 4 / MCS PANNER)
    arg = a[0].lower()
    if arg == "direct":
        wire = 8
    else:
        n = int(arg)
        if not 1 <= n <= 8:
            raise ValueError("routing button must be 1..8 or direct")
        wire = ROUTING_WIRE[n - 1]
    s.send(3, 0xBE, 0x00 if _onoff(a[1]) else 0x01, wire)


def cmd_fadermode(s, a):                              # FADER MODE: ONLY the FADER<->AUX/MTRX pair (sw3),
    btn = a[0].lower()                                # mutually exclusive - one switch, two labels
    if btn == "fader":                      s.led(0, 0x0C, 3, False)   # FADER label lit
    elif btn in ("auxmtrx", "aux", "mtrx"): s.led(0, 0x0C, 3, True)    # AUX/MTRX label lit
    else:
        raise ValueError("fadermode: fader, auxmtrx (exclusive pair)")


# HUI V-pot ring display modes (high nibble of the ring value byte), hw-confirmed on the DM2000:
#   dot = one segment at the position, sweeps left->right with value;
#   boostcut = bar from centre (right above 0.5, left below 0.5);
#   fill = fills L->R (full at value 1.0);  spread = grows both ways from centre, full at value 0.5
#   (above 0.5 looks the same - saturated).  Add 0x40 for the centre LED.
SELRING_MODE = {"dot": 0x00, "boostcut": 0x10, "fill": 0x20, "spread": 0x30}


def cmd_selring(s, a):                                # EFFECTS/PLUG-INS SEL ring 1-4 (CC 0x18-0x1B)
    n = int(a[0])
    if not 1 <= n <= 4:
        raise ValueError("selring number must be 1..4")
    pos = 1 + round(_unit(a[1]) * 10)                 # position 1..11 (6 = center)
    if len(a) > 2 and a[2].lower() not in SELRING_MODE:
        raise ValueError("selring mode: dot, boostcut, fill, spread")
    mode = SELRING_MODE[a[2].lower()] if len(a) > 2 else 0x00   # ring shape (high nibble)
    s.send(0, 0xB0, 0x17 + n, mode | pos)             # val = (mode<<4) | position


def cmd_trackarm(s, a):                               # TRACK ARMING: per-channel arm LEDs (1-24) + master.
    target = a[0].lower()                             # the arm LEDs live in this row (= channel sw7), not the strip
    if target == "master":
        s.led(0, 0x0C, 6, _onoff(a[1]))
    else:
        port, cell = _ch_port(int(target))
        s.led(port, cell, 7, _onoff(a[1]))


def cmd_led(s, a):
    s.led(0, int(a[0], 0), int(a[1], 0), _onoff(a[2]))


def cmd_raw(s, a):
    port = 0
    if a and len(a[0]) == 1 and a[0].isdigit():
        port, a = int(a[0]) - 1, a[1:]
    data = [int(b, 16) for b in a]
    i = 0
    while i < len(data):                              # split into one message per MIDI status
        if data[i] == 0xF0:
            j = i
            while j < len(data) and data[j] != 0xF7:
                j += 1
            s.send(port, *data[i:j + 1])
            i = j + 1
        else:
            j = i + 1
            while j < len(data) and data[j] < 0x80:
                j += 1
            s.send(port, *data[i:j])
            i = j


def _all_section_leds(s, on):
    for ports, buttons in SECTIONS.values():
        for zone, sw in set(buttons.values()):        # set() dedups e.g. param/insert
            for p in ports:
                s.led(p, zone, sw, on)


def cmd_allon(s, _a):                                 # light everything (show-off / lamp test)
    for ch in range(1, 25):
        port, cell = _ch_port(ch)
        for sw in CHAN_LED.values():                  # sel/solo/ringled/ins lit
            s.led(port, cell, sw, True)
        s.led(port, cell, 7, True)                    # arm LED (TRACK ARMING row)
        s.led(port, cell, 2, False)                   # ON LED lit (inverted: False = lit)
        s.send(port, 0xB0, 0x0C, cell)                # AUTO -> red (its highest state, like fader/meter)
        s.send(port, 0xB0, 0x2C, AUTO_COLOR["red"])
        s.send(port, 0xA0, cell, 12)                  # meter L -> clip
        s.send(port, 0xA0, cell, 0x10 | 12)           # meter R -> clip
        s.send(port, 0xB0, cell, 0x7F)                # fader -> top
        s.send(port, 0xB0, 0x20 + cell, 0x7F)
        s.send(port, 0xB0, 0x10 + cell, 11)           # pan ring full
        s.scribble(port, cell, "8888")                # strip name display
    _all_section_leds(s, True)
    for zone, sw in list(UDK_KEYS.values()) + list(LM_KEYS.values()):  # UDK + LOCATE MEMORY
        s.led(0, zone, sw, True)
    for n in range(8):                                # MCS PANNER routing LEDs on (port 4)
        s.send(3, 0xBE, 0x00, ROUTING_WIRE[n])
    s.send(3, 0xBE, 0x00, 8)                          # ROUTING DIRECT
    s.led(0, 0x0C, 3, True)                           # FADER MODE -> AUX/MTRX (not FADER)
    s.led(0, 0x0C, 6, True)                           # TRACK ARMING master
    for sw in COUNTER_MODE.values():                  # TIME CODE / FEET / BEATS on
        s.led(0, 0x16, sw, True)
    for vv in (0x02, 0x42):                           # CURSOR MODE: flash SELECT, settle on ZOOM
        s.send(0, 0xB0, 0x0C, 0x0D)
        s.send(0, 0xB0, 0x2C, vv)
    s.scribble(0, 8, "8888")                          # SELECT ASSIGN
    for line in range(8):                             # REMOTE LCD: all 8 lines
        s.remote(line, "8888888888")
    s.counter("88888888")                             # LED counter: all segments
    for n in range(4):                                # EFFECTS SEL parameter rings -> full ring
        s.send(0, 0xB0, 0x18 + n, 0x20 | 11)          # 0x2B = fill mode at max (whole arc lit)


def cmd_clear(s, _a):                                 # blank everything
    for ch in range(1, 25):
        port, cell = _ch_port(ch)
        for sw in CHAN_LED.values():                  # sel/solo/ringled/ins off
            s.led(port, cell, sw, False)
        s.led(port, cell, 7, False)                   # arm LED off (TRACK ARMING row)
        s.led(port, cell, 2, True)                    # ON LED dark (inverted: True = dark)
        s.send(port, 0xB0, 0x0C, cell)                # AUTO -> off
        s.send(port, 0xB0, 0x2C, AUTO_COLOR["off"])
        s.send(port, 0xA0, cell, 0x00)                # meter L off
        s.send(port, 0xA0, cell, 0x10)                # meter R off
        s.send(port, 0xB0, cell, 0x00)                # fader -> bottom
        s.send(port, 0xB0, 0x20 + cell, 0x00)
        s.send(port, 0xB0, 0x10 + cell, 0x00)         # pan ring off
        s.scribble(port, cell, "")
    _all_section_leds(s, False)
    for zone, sw in list(UDK_KEYS.values()) + list(LM_KEYS.values()):  # UDK + LOCATE MEMORY
        s.led(0, zone, sw, False)
    for n in range(8):                                # MCS PANNER routing LEDs off (port 4)
        s.send(3, 0xBE, 0x01, ROUTING_WIRE[n])
    s.send(3, 0xBE, 0x01, 8)                          # ROUTING DIRECT off
    s.led(0, 0x0C, 3, False)                          # FADER MODE -> FADER (default)
    s.led(0, 0x0C, 6, False)                          # TRACK ARMING master off
    for sw in COUNTER_MODE.values():                  # TIME CODE / FEET / BEATS indicator off
        s.led(0, 0x16, sw, False)
    s.send(0, 0xB0, 0x0C, 0x0D)                       # CURSOR MODE -> NAVIGATION (default)
    s.send(0, 0xB0, 0x2C, 0x02)
    for n in range(4):                                # EFFECTS SEL parameter rings off
        s.send(0, 0xB0, 0x18 + n, 0)
    s.counter("")                                     # blank LED counter
    s.scribble(0, 8, "")                              # blank SELECT ASSIGN
    for line in range(8):                             # blank the 8 REMOTE display lines
        s.remote(line, "")


# flat top-level verbs -> handler (panel button sections are dispatched via SECTIONS)
COMMANDS = {
    "channel": cmd_channel,
    "selassign": cmd_selassign, "remotelcd": cmd_remotelcd, "counter": cmd_counter,
    "cursormode": cmd_cursormode, "countermode": cmd_countermode,
    "udk": cmd_udk, "lm": cmd_lm, "routing": cmd_routing, "fadermode": cmd_fadermode,
    "selring": cmd_selring, "trackarm": cmd_trackarm,
    "led": cmd_led, "raw": cmd_raw, "all_on": cmd_allon, "clear": cmd_clear,
}


def main():
    sender = Sender()
    if not sender.open():
        print("no DM2000 ports found - is REAPER / Pro Tools still holding them?")
        return 1
    print("\ndesk online. type `help` for commands, `quit` to exit.\n")
    try:
        while True:
            try:
                line = input("dm2000> ").strip()
            except EOFError:
                break
            if not line:
                continue
            try:
                parts = shlex.split(line)          # honours "quoted text"; bare spaces still work
            except ValueError:
                parts = line.split()
            if not parts:
                continue
            verb, args = parts[0].lower(), parts[1:]
            if verb in ("quit", "exit"):
                break
            if verb == "help":
                print(HELP)
                continue
            try:
                if verb in SECTIONS:
                    cmd_section(sender, verb, args)
                elif verb in COMMANDS:
                    COMMANDS[verb](sender, args)
                else:
                    print(f"unknown command '{verb}' - try `help`")
            except (ValueError, IndexError, KeyError) as err:
                print(f"bad arguments for '{verb}': {err} - try `help`")
    except KeyboardInterrupt:
        pass
    finally:
        print("\nclosing (desk reverts to offline; nothing wiped)")
        sender.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
