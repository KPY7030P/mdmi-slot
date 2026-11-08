"""Checks a clap_host run: note timing against the reported latency, pitch and silence.

usage: check_wav.py <run.log> <out-N.wav>
"""
import re
import struct
import sys

import numpy as np

THRESHOLD = 0.01


def read_float_wav(path):
    data = open(path, "rb").read()
    rate = struct.unpack_from("<I", data, 24)[0]
    size = struct.unpack_from("<I", data, 40)[0]
    samples = np.frombuffer(data, dtype="<f4", count=size // 4, offset=44).reshape(-1, 2)
    return rate, samples


def note_hz(note):
    return 440.0 * 2 ** ((note - 69) / 12)


def dominant_hz(mono, rate):
    n = len(mono)
    spectrum = np.abs(np.fft.rfft(mono * np.hanning(n), n=n * 4))
    freqs = np.fft.rfftfreq(n * 4, 1 / rate)
    band = (freqs > 50) & (freqs < 5000)
    return freqs[band][np.argmax(spectrum[band])]


def main():
    log, wav = sys.argv[1:3]
    text = open(log).read()
    latency = int(re.search(r"latency=(\d+)", text).group(1))
    rate, samples = read_float_wav(wav)
    mono = samples.mean(axis=1)
    print(f"rate={rate} latency={latency} ({latency / rate * 1000:.1f}ms) peak={np.abs(samples).max():.3f} dc={mono.mean():+.5f}")

    failed = False
    events = [(s, int(n), int(a)) for s, n, a in re.findall(r"EVENT (\w+) (\d+) sample=(\d+)", text)]
    for status, note, at in events:
        if status[0] == "9" and ("80", note, at) in events:
            # A note of no length: by 250 ms its release is over, a hung note would still sound.
            tail = np.abs(mono[at + rate // 4 : at + rate * 3 // 10]).max()
            print(f"note {note}: no length, level 250 ms later {tail:.4f}")
            if tail > THRESHOLD:
                print(f"note {note}: HANGS")
                failed = True
        elif status[0] == "9":
            segment = mono[at : at + rate // 2]
            hits = np.nonzero(np.abs(segment) > THRESHOLD)[0]
            if not len(hits):
                print(f"note {note}: NO SOUND peak={np.abs(segment).max():.4f}")
                failed = True
                continue
            onset = hits[0]
            body = segment[onset + rate // 50 :]
            hz = dominant_hz(body, rate)
            cents = 1200 * np.log2(hz / note_hz(note))
            early = np.abs(mono[at - rate // 10 : at]).max()
            print(f"note {note}: onset={onset} samples after event = {onset / rate * 1000:.2f}ms (minus latency {(onset - latency) / rate * 1000:+.2f}ms) pitch={hz:.1f}Hz {cents:+.0f}c peak={np.abs(segment).max():.3f} before_note_peak={early:.4f}")
            if abs(cents) > 25:
                failed = True
    print("FAILED" if failed else "OK")
    sys.exit(1 if failed else 0)


main()
