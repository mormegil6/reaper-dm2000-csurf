#!/usr/bin/env python3
"""
hui_bridge.py - MITM HUI sniffer: tap a DAW's HUI output on its way to the DM2000.

Sits in the path between a HUI-speaking DAW and the desk, so every message the
DAW transmits (LED feedback, scribble / counter / display SysEx) is forwarded on
*and* printed in decoded form:

    DAW  --(Send To)-->  loopMIDI port  --[this script]-->  DM2000

The DAW's input is wired straight from the desk (that low-latency path keeps the
DAW online); this script only carries - and decodes - the DAW -> desk direction.

REQUIREMENTS (this tool needs a DAW and a loopMIDI port):
  * A running DAW with a HUI controller configured:
      Receive From = Yamaha DM2000-1  (direct, so the desk stays online)
      Send To      = the loopMIDI port below
  * loopMIDI with that virtual port created.
  * Nothing else holding the loopMIDI / DM2000 ports (close MIDI-OX etc.).

Usage:
    python hui_bridge.py [loopmidi-port-fragment] [dm2000-port-fragment]
Defaults: "loopMIDI IN1"  ->  "Yamaha DM2000-1".   Ctrl-C to stop.

For sniffing the desk on its own (no DAW), use hui_deskmon.py instead.
"""
import sys
import time
import rtmidi
from hui_common import find_port, decode

DEFAULT_IN = "loopmidi in1"      # where the DAW's HUI output arrives
DEFAULT_OUT = "yamaha dm2000-1"  # forwarded on to the desk
SHOW_KEEPALIVE = False           # set True to also log the 90 00 .. heartbeat


def _list_ports(midi, label):
    print(f"available {label} ports:")
    for i, n in enumerate(midi.get_ports()):
        print(f"  {i}: {n}")


def main(argv):
    in_name = argv[1] if len(argv) > 1 else DEFAULT_IN
    out_name = argv[2] if len(argv) > 2 else DEFAULT_OUT

    midi_in, midi_out = rtmidi.MidiIn(), rtmidi.MidiOut()
    in_idx, in_label = find_port(midi_in, in_name)
    out_idx, out_label = find_port(midi_out, out_name)
    if in_idx is None:
        print(f"input port matching '{in_name}' not found.")
        _list_ports(midi_in, "input")
        return 1
    if out_idx is None:
        print(f"output port matching '{out_name}' not found.")
        _list_ports(midi_out, "output")
        return 1

    midi_in.open_port(in_idx)
    midi_in.ignore_types(sysex=False, timing=True, active_sense=True)  # we want SysEx
    midi_out.open_port(out_idx)
    print(f"bridging DAW output  '{in_label}'  -->  '{out_label}'  (decoding as it passes)")
    print("Ctrl-C to stop.\n")

    state = {"zone": None}
    try:
        while True:
            event = midi_in.get_message()
            if event:
                msg, _ = event
                midi_out.send_message(msg)            # pass through to the desk
                line = decode(msg, state, SHOW_KEEPALIVE)
                if line:
                    print(f"{time.strftime('%H:%M:%S')}  {line}")
            else:
                time.sleep(0.001)
    except KeyboardInterrupt:
        print("\nstopped")
    finally:
        midi_in.close_port()
        midi_out.close_port()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
