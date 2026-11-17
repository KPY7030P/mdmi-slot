# Stage 36 (D81): checks the window's TV looks against the model of docs/design/tv-look.md, the study's
# make_tv.py at the window's x2. gui_test saves the ROM page three times from one stalled ROM frame:
# <prefix>-tv-off.bmp, -tv-rgb.bmp, -tv-comp.bmp. The frame is read from the OFF shot (x2, left 40, top 54), each
# look worked out here and compared with the shot within 2 levels a channel.
# Run: python -I check_tv.py <prefix of the shots>
import os
import sys

import numpy as np
from PIL import Image

SUB = 16      # subsamples per H40 pixel
CYCLE = 30    # subsamples per subcarrier cycle (1.875 px)
CHROMA = 45   # chroma means, about 5.6 px
LEFT, TOP, W, H = 40, 54, 320, 224
TOLERANCE = 2


def box(a, n):
    """Centred moving average of length n along axis 1 (edges repeated), as make_tv.py's."""
    left = n // 2
    p = np.pad(a, ((0, 0), (left + 1, n - left - 1)), mode="edge")
    c = np.cumsum(p, axis=1, dtype=np.float64)
    return (c[:, n:] - c[:, :-n]) / n


def composite(rgb):
    up = np.repeat(rgb, SUB, axis=1)
    r, g, b = up[..., 0], up[..., 1], up[..., 2]
    y_ = 0.299 * r + 0.587 * g + 0.114 * b
    i_ = 0.596 * r - 0.274 * g - 0.322 * b
    q_ = 0.211 * r - 0.523 * g + 0.312 * b
    ph = 2 * np.pi * np.arange(up.shape[1]) / CYCLE
    s = y_ + i_ * np.sin(ph) + q_ * np.cos(ph)
    y = box(s, CYCLE)
    c = s - y
    i = box(box(2 * c * np.sin(ph), CHROMA), CHROMA)
    q = box(box(2 * c * np.cos(ph), CHROMA), CHROMA)
    return np.stack([y + 0.956 * i + 0.621 * q, y - 0.272 * i - 0.647 * q, y - 1.106 * i + 1.703 * q], axis=-1)


def crt2(sub):
    xs = ((np.arange(W * 2) + 0.5) * SUB / 2).astype(int)
    line = np.clip(sub[:, xs, :], 0, 1) ** (2.5 / 2.2)
    out = np.empty((H * 2, W * 2, 3))
    out[0::2] = line
    out[1::2] = line * (0.25 + 0.50 * line)
    return np.clip(out * 1.15, 0, 1)


def look(rgb, kind):
    sub = np.repeat(rgb, SUB, axis=1) if kind == "rgb" else composite(rgb)
    return (crt2(sub) * 255 + 0.5).astype(np.int32)


def shot(prefix, name):
    im = Image.open(f"{prefix}-{name}").convert("RGB")
    return np.asarray(im, dtype=np.int32)[TOP:TOP + 2 * H, LEFT:LEFT + 2 * W]


def main():
    folder = sys.argv[1]
    off = shot(folder, "tv-off.bmp")
    rgb = off[0::2, 0::2].astype(np.float64) / 255
    failed = 0
    if not rgb.any():
        print("FAIL: the ROM frame is black")
        failed += 1
    for kind in ("rgb", "comp"):
        got = shot(folder, f"tv-{kind}.bmp")
        want = look(rgb, kind)
        diff = np.abs(got - want)
        worst = int(diff.max())
        changed = int((got != off).any(axis=2).sum())
        ok = worst <= TOLERANCE and changed > 0
        print(f"  {'ok  ' if ok else 'FAIL'}: {kind}: within {worst} levels of the model "
              f"({int((diff > 0).any(axis=2).sum())} pixels off by any), {changed} pixels differ from OFF")
        failed += not ok
    return failed


if __name__ == "__main__":
    sys.exit(main())
