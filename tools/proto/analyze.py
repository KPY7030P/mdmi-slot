"""Checks the probe output: per-event onset latency, pitch and silence between notes.

usage: analyze.py <run.log> <out.wav> [out.bmp out.png]
"""
import re
import struct
import sys
import wave
import zlib

import numpy as np

THRESHOLD = 300


def note_hz(note):
    return 440.0 * 2 ** ((note - 69) / 12)


def dominant_hz(mono, rate):
    window = mono * np.hanning(len(mono))
    spectrum = np.abs(np.fft.rfft(window, n=len(mono) * 4))
    freqs = np.fft.rfftfreq(len(mono) * 4, 1 / rate)
    band = (freqs > 50) & (freqs < 5000)
    return freqs[band][np.argmax(spectrum[band])]


def bmp_to_png(bmp_path, png_path):
    data = open(bmp_path, "rb").read()
    offset, _, width, height = struct.unpack_from("<IIii", data, 10)
    rows = bytearray()
    for y in range(height - 1, -1, -1):
        row = data[offset + y * width * 4 : offset + (y + 1) * width * 4]
        rows.append(0)
        for x in range(width):
            rows += bytes((row[x * 4 + 2], row[x * 4 + 1], row[x * 4]))

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(rows))) + chunk(b"IEND", b"")
    open(png_path, "wb").write(png)


def main():
    log, wav = sys.argv[1:3]
    with wave.open(wav) as w:
        rate = w.getframerate()
        raw = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2").reshape(-1, 2)
    mono = raw.mean(axis=1)
    print(f"rate={rate} seconds={len(mono) / rate:.2f} peak={np.abs(raw).max()} L={np.abs(raw[:, 0]).max()} R={np.abs(raw[:, 1]).max()}")

    events = re.findall(r"EVENT (.+?) sample=(\d+)", open(log).read())
    boot_end = int(events[0][1])
    idle = mono[boot_end - rate // 2 : boot_end]
    dc = np.median(idle)
    print(f"idle before first note: dc={dc:.1f} ripple={idle.max() - idle.min():.1f} boot_peak={np.abs(mono[:boot_end]).max():.0f}")
    mono = mono - dc

    expected = {"C4": 60, "E4": 64, "C5": 72}
    for i, (label, start) in enumerate(events):
        start = int(start)
        end = int(events[i + 1][1]) if i + 1 < len(events) else len(mono)
        segment = mono[start:end]
        if label.endswith(" on"):
            hits = np.nonzero(np.abs(segment) > THRESHOLD)[0]
            if not len(hits):
                print(f"{label}: NO SOUND, peak={np.abs(segment).max():.0f}")
                continue
            onset = hits[0]
            body = segment[onset + rate // 50 : onset + rate // 50 + rate // 2]
            hz = dominant_hz(body, rate)
            target = note_hz(expected[label.split()[2]])
            # Timbres have strong harmonics, so compare as an interval against the nearest octave multiple.
            ratio = hz / target
            cents = 1200 * np.log2(ratio / max(round(ratio), 1)) if ratio >= 0.75 else float("nan")
            print(f"{label}: onset={onset / rate * 1000:.1f}ms from queue, peak={np.abs(segment).max():.0f}, dominant={hz:.1f}Hz, target={target:.1f}Hz (harmonic {max(round(ratio), 1)}, {cents:+.0f} cents)")
        else:
            tail = segment[int(rate * 0.5) :]
            print(f"{label}: peak 0.5s after off={np.abs(tail).max():.0f}")

    if len(sys.argv) == 5:
        bmp_to_png(sys.argv[3], sys.argv[4])


main()
