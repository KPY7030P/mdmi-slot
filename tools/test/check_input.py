"""Checks a vst3_host run with MDMI_TEST_INPUT (stage 23, D74): the 1 kHz sine given to the VST3's
input bus comes out of the plugin through the DAC, before the script's first note at 3 s.

usage: check_input.py <out-N.wav>
"""
import struct
import sys

import numpy as np


def main():
    data = open(sys.argv[1], "rb").read()
    rate = struct.unpack_from("<I", data, 24)[0]
    size = struct.unpack_from("<I", data, 40)[0]
    samples = np.frombuffer(data, dtype="<f4", count=size // 4, offset=44).reshape(-1, 2)
    part = samples[int(1.0 * rate):int(2.8 * rate), 0].astype(np.float64)
    n = np.arange(len(part))
    magnitude = abs(np.dot(part, np.exp(-2j * np.pi * 1000.0 * n / rate))) / len(part)
    print("input through the VST3: 1 kHz magnitude %.5f" % magnitude)
    # A half-scale sine in the DAC comes out at about 0.04 (one chip channel's level); none without.
    if magnitude < 0.01:
        print("FAIL: the VST3 input bus does not reach the DAC")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
