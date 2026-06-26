#!/usr/bin/env python3
"""
hui_deskmon.py - monitor what the DM2000 transmits, with no DAW running.

The desk only sends DAW-layer MIDI while it is "online", so this script answers
the console's keepalive itself (echoes the 90 00 7F heartbeat on each port) to
bring it online, then decodes and logs everything the desk transmits across all
four DAW ports - button presses, fader / encoder moves, fader-touch, anything.

Use it to find out whether a given control sends MIDI at all, and to capture the
zone / switch of any button - without needing a DAW or loopMIDI.

REQUIREMENT: the DM2000 ports must be free - close REAPER / Pro Tools / MIDI-OX
(anything that holds them) before running.

Usage:
    python hui_deskmon.py [dm2000-name-fragment | all]
With no argument it auto-discovers the four DAW ports (1-4), regardless of how the
OS names them - macOS "YAMAHA DM2000 Port1", Windows "Yamaha DM2000-1". Pass "all"
to watch all eight ports - useful for spotting traffic on the non-DAW ports 5-8,
where anything that appears is outside our HUI mapping.   Ctrl-C to stop.

To watch a DAW's output instead (LED / display feedback), use hui_bridge.py.
"""
import sys
import time
import rtmidi
from hui_common import find_port, decode, dm2000_daw_ports

SHOW_KEEPALIVE = False


def _is_device_pong(msg):
    """The desk's 90 00 7F heartbeat - we echo it to keep the desk online."""
    return len(msg) >= 3 and msg[0] == 0x90 and msg[1] == 0x00 and msg[2] == 0x7F


def main(argv):
    arg = argv[1] if len(argv) > 1 else None
    if arg and arg.lower() in ("all", "8"):
        names = dm2000_daw_ports(rtmidi.MidiIn(), count=8)   # all 8 ports (incl. non-DAW 5-8)
    elif arg:
        names = [arg]
    else:
        names = dm2000_daw_ports(rtmidi.MidiIn())            # the 4 DAW HUI ports

    ports = []  # list of (midi_in, midi_out, label, zone_state)
    for name in names:
        midi_in, midi_out = rtmidi.MidiIn(), rtmidi.MidiOut()
        in_idx, in_label = find_port(midi_in, name)
        out_idx, _ = find_port(midi_out, name)
        if in_idx is None or out_idx is None:
            print(f"port '{name}' not found - skipping")
            continue
        midi_in.open_port(in_idx)
        midi_in.ignore_types(sysex=False, timing=True, active_sense=True)
        midi_out.open_port(out_idx)
        ports.append((midi_in, midi_out, in_label, {"zone": None}))
        print(f"open: {in_label}")

    if not ports:
        print("no DM2000 ports found - is REAPER / Pro Tools still holding them?")
        return 1
    print("\nkeeping the desk online; operate any control to see its MIDI. Ctrl-C to stop.\n")

    try:
        while True:
            idle = True
            for slot, (midi_in, midi_out, label, state) in enumerate(ports):
                event = midi_in.get_message()
                if event:
                    idle = False
                    msg, _ = event
                    if _is_device_pong(msg):
                        midi_out.send_message([0x90, 0x00, 0x7F])  # echo -> stay online
                    line = decode(msg, state, SHOW_KEEPALIVE)
                    if line:
                        print(f"{time.strftime('%H:%M:%S')} P{slot + 1}  {line}")
            if idle:
                time.sleep(0.001)
    except KeyboardInterrupt:
        print("\nstopped")
    finally:
        for midi_in, midi_out, _, _ in ports:
            midi_in.close_port()
            midi_out.close_port()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
