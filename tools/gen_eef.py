"""Writes the EEF envelopes in tools/test/files: the owner tries stage 7 with them, and the tests read them.

usage: gen_eef.py <tools/test/files>

A byte is one frame: the low nibble is attenuation, 0 loudest and F silent; the high nibble shifts
the pitch as the MDMI ROM does (1-4 up 5, 10, 20, 50 cents, 5-7 up 1, 2, 5 semitones, 8-E the same
down). FE starts the loop, FD ends it and the steps after it play on release, FF ends the envelope.
"""
import os
import sys

LOOP, RELEASE, END = 0xFE, 0xFD, 0xFF

ENVELOPES = {
    # Dies away over half a second, release or not.
    "PSG Pluck": [level for level in range(16) for _ in range(2)] + [END],
    # Holds while the key is down, then fades.
    "PSG Organ": [0x04, 0x02, 0x00, LOOP, 0x00, RELEASE, 0x02, 0x04, 0x06, 0x08, 0x0A, 0x0C, 0x0E, END],
    # A pitch wobble of up to 20 cents while held.
    "PSG Vibrato": [0x00, 0x00, LOOP, 0x00, 0x00, 0x10, 0x10, 0x20, 0x20, 0x10, 0x10, 0x00, 0x00, 0x80, 0x80,
                    0xA0, 0xA0, 0x80, 0x80, RELEASE, 0x03, 0x06, 0x09, 0x0C, END],
    # Note and a whole tone above, in turn.
    "PSG Trill": [LOOP, 0x00, 0x00, 0x00, 0x60, 0x60, 0x60, RELEASE, 0x04, 0x08, 0x0C, END],
    # Short, for the noise channel.
    "Noise Hit": [0x00, 0x01, 0x03, 0x05, 0x07, 0x09, 0x0B, 0x0D, 0x0F, END],
    # One step over the plugin's limit of 124, so loading it is refused.
    "Too Long": [0x00] * 125 + [END],
}


def main():
    folder = sys.argv[1]
    for name, steps in ENVELOPES.items():
        with open(os.path.join(folder, name + ".eef"), "wb") as f:
            f.write(bytes(steps))


main()
