"""Compares the second note of vst3_host runs that moved the Preset knob to different values.

usage: compare_programs.py <out_prefix> <value> [<value> ...]
The first value is the reference.
"""
import sys

import numpy as np

RATE = 48000


def note(prefix, value):
    data = open(f"{prefix}{value}-0.wav", "rb").read()
    samples = np.frombuffer(data, dtype="<f4", offset=44).reshape(-1, 2).mean(axis=1)
    return samples[int(4.05 * RATE) : int(4.45 * RATE)]


def spectrum(samples):
    magnitude = np.abs(np.fft.rfft(samples * np.hanning(len(samples))))
    return magnitude / np.linalg.norm(magnitude)


def main():
    prefix = sys.argv[1]
    values = sys.argv[2:]
    reference = spectrum(note(prefix, values[0]))
    for value in values:
        samples = note(prefix, value)
        print(f"preset knob {value}: peak={np.abs(samples).max():.3f} spectrum_vs_{values[0]}={float(spectrum(samples) @ reference):.3f}")


main()
