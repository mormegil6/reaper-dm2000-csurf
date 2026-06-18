"""
Shared HUI / DM2000 MIDI helpers for the diagnostic tools in this folder.

The DM2000's Pro Tools "DAW Remote" layer speaks the HUI protocol. Both tools
here (the DAW-output bridge and the standalone desk monitor) need the same two
things, centralised below:

  * find_port()  - locate a MIDI port by a case-insensitive name fragment.
  * decode()     - turn one raw HUI message into a readable line.

Requires: python-rtmidi   (pip install python-rtmidi)
"""

# HUI display-SysEx header:  F0 00 00 66 05 00 <zone> <payload...> F7
HUI_SYSEX_HEADER = bytes([0xF0, 0x00, 0x00, 0x66, 0x05, 0x00])

# Display zones carried in a HUI display SysEx (the byte after the header).
DISPLAY_ZONES = {
    0x10: "scribble",        # 4-char channel strips (cell 8 = SELECT ASSIGN)
    0x11: "counter",         # 8-digit LED timecode counter
    0x12: "remote-display",  # 8x10 REMOTE / INSERT editor
}


def find_port(midi, name_fragment):
    """Return (index, full_name) of the first port whose name contains
    name_fragment (case-insensitive), else (None, None)."""
    want = name_fragment.lower()
    for index, name in enumerate(midi.get_ports()):
        if want in name.lower():
            return index, name
    return None, None


def is_keepalive(msg):
    """True for the HUI keepalive note: 90 00 00 (host ping) or 90 00 7F (device pong)."""
    return len(msg) >= 3 and msg[0] == 0x90 and msg[1] == 0x00 and msg[2] in (0x00, 0x7F)


def decode(msg, state, show_keepalive=False):
    """Decode one MIDI message to a readable string, or None to suppress it.

    `state` is a caller-owned dict that remembers the last HUI zone-select: HUI
    sends the zone in one CC (0x0C host->desk / 0x0F desk->host) and the LED or
    switch in the next, so the pair must be tracked across calls.
    """
    status = msg[0]

    if is_keepalive(msg):
        if not show_keepalive:
            return None
        return "PING 90 00 00" if msg[2] == 0x00 else "PONG 90 00 7F"

    # Display SysEx: scribble / counter / remote-display text
    if status == 0xF0:
        data = bytes(msg)
        if data[:6] == HUI_SYSEX_HEADER and data[-1] == 0xF7:
            zone = data[6]
            payload = data[7:-1]
            text = "".join(chr(c) if 0x20 <= c < 0x7F else "." for c in payload)
            hexs = " ".join("%02X" % c for c in payload)
            return f"SYSX zone=0x{zone:02X} ({DISPLAY_ZONES.get(zone, '?'):14}) [{hexs}]  '{text}'"
        return "SYSX " + " ".join("%02X" % c for c in data)

    # Control Change: zone-select, LED/switch feedback, faders, rings, encoders
    if status == 0xB0 and len(msg) >= 3:
        cc, value = msg[1], msg[2]
        if cc in (0x0C, 0x0F):                       # zone select - remember for the next CC
            state["zone"] = value
            return None
        zone = state.get("zone")
        ztxt = "0x%02X" % zone if zone is not None else "0x??"
        if cc == 0x2C:                               # LED feedback (host -> desk)
            return f"LED   zone={ztxt} sw={value & 0x0F} {'ON ' if value & 0x40 else 'off'} (2C={value:02X})"
        if cc == 0x2F:                               # switch (desk -> host)
            return f"SW    zone={ztxt} sw={value & 0x0F} {'PRESS' if value & 0x40 else 'rel'} (2F={value:02X})"
        if cc < 0x08:
            return f"FADER ch={cc} msb={value:02X}"
        if 0x10 <= cc < 0x18:                        # channel V-pot pan rings (8)
            return f"RING  ch={cc - 0x10} val={value:02X}"
        if 0x18 <= cc < 0x1C:                        # EFFECTS/PLUG-INS SEL parameter rings (4)
            return f"SELRING fx={cc - 0x18 + 1} val={value:02X}"
        if 0x20 <= cc < 0x28:
            return f"FADER ch={cc - 0x20} lsb={value:02X}"
        if 0x40 <= cc < 0x48:
            return f"ENC   ch={cc - 0x40} delta={value:02X}"
        return f"CC    {cc:02X} {value:02X}"         # encoders / other controls

    if 0xA0 <= status <= 0xAF and len(msg) >= 3:     # poly aftertouch = meter levels
        return f"METER note={msg[1]:02X} val={msg[2]:02X}"

    return "MSG  " + " ".join("%02X" % c for c in msg)
