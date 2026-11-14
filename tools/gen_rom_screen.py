"""Generates the tiles, palette and tables of the ROM's status screen (stage 24a, D63) as C, into
the MDMI fork: third_party/mdmi/src/ui_tiles.c and ui_tiles.h. Pictures can't live in the fork:
patches/ is a text diff (docs/design/rom-screen.md 10.1). Glyphs come from the ROM's res/font.png
and the brackets from res/sprite/, so a change of those needs a run of this script; the output is
committed through patches/0003 and the ROM build needs no Python.

The layout, colours and tile ranges are docs/design/rom-screen.md, sections 1-10.

usage: python tools/gen_rom_screen.py
"""
import os

from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MDMI = os.path.join(ROOT, "third_party", "mdmi")
RES = os.path.join(MDMI, "res")

# rom-screen.md 10.2: PAL0 FM, PAL1 PSG, PAL2 DAC, PAL3 Ch3 Special; indices 1-10 the same use in
# every palette. Live note cells 6-8 start in their chip's bright colour; 24a2 moves them.
FM_BRIGHT, PSG_BRIGHT, SP_BRIGHT = 0xE88, 0xEEA, 0xC8E
PALETTE = [
    # PAL0, FM
    [0x000, 0xEEE, 0xE24, 0x822, 0x222, 0x000, FM_BRIGHT, FM_BRIGHT, FM_BRIGHT, 0xEC2, 0xEC2, 0xE88, 0xE86, 0xE4A, 0xECC, 0x00E],
    # PAL1, PSG
    [0x000, 0x444, 0xEC2, 0x640, 0x222, 0x000, FM_BRIGHT, FM_BRIGHT, FM_BRIGHT, 0xEC2, 0xEEE, 0x000, 0xEEA, 0xA80, 0xCA6, 0x000],
    # PAL2, DAC
    [0x000, 0x026, 0x08E, 0x026, 0x222, 0x000, PSG_BRIGHT, PSG_BRIGHT, PSG_BRIGHT, 0xEEE, 0x640, 0xEEE, 0x0E0, 0x0EE, 0x00E, 0x000],
    # PAL3, Ch3 Special; 12-15 the noise's cells (24a2)
    [0x000, 0x888, 0xA2E, 0x206, 0x222, 0x000, PSG_BRIGHT, 0x000, 0x000, 0x640, 0x640, 0xA2E, 0x444, 0x000, 0x000, 0x000],
]
I_TEXT, I_CHIP, I_DIM, I_GREY, I_MASK = 1, 2, 3, 4, 5
I_LIVE = (6, 7, 8)
I_DAC_INK = 11
I_GREEN, I_YELLOW, I_RED = 12, 13, 14
OWNER_INKS = (I_CHIP, 12, 13, 14)

NOTE_GLYPHS = "0123456789ABCDEFG#-?"
NAMES = ["C-", "C#", "D-", "D#", "E-", "F-", "F#", "G-", "G#", "A-", "A#", "B-"]
LOWEST_NAMED = 0  # every note since stage 30 (rom-screen.md 12); octave -1 shows '-' for its digit

# The PSG wave (rom-screen.md 8, mockup final_psgwave): one period per 3 semitones from A2, the PSG's lowest note, up.
WAVE_PERIODS = [32, 27, 23, 19, 16, 13, 11, 10, 8, 7, 6, 5, 4]
WAVE_WIDTH = 32
PSG_CLOCK = 3579545

DIGITS_3X5 = {
    "0": ["111", "101", "101", "101", "111"], "1": ["010", "110", "010", "010", "111"],
    "2": ["111", "001", "111", "100", "111"], "3": ["111", "001", "111", "001", "111"],
    "4": ["101", "101", "111", "001", "001"], "5": ["111", "100", "111", "001", "111"],
    "6": ["111", "100", "111", "101", "111"],
}


def blank(fill=0):
    return [[fill] * 8 for _ in range(8)]


def font():
    im = Image.open(os.path.join(RES, "font.png"))
    px = im.load()
    glyphs = {}
    for code in range(32, 128):
        t = code - 32
        x0, y0 = (t % 16) * 8, (t // 16) * 8
        glyphs[chr(code)] = [[px[x0 + x, y0 + y] for x in range(8)] for y in range(8)]
    return glyphs


def ink(glyph, colour, bg=0):
    """The font's ink (index 1) in a colour, its shadow (2) and paper as the background."""
    return [[colour if p == 1 else bg for p in row] for row in glyph]


def sheet_tiles(path, remap):
    im = Image.open(path)
    px = im.load()
    w, h = im.size
    tiles = []
    for ty in range(h // 8):
        for tx in range(w // 8):
            tiles.append([[remap(px[tx * 8 + x, ty * 8 + y]) for x in range(8)] for y in range(8)])
    return tiles


def pack(tile):
    words = []
    for row in tile:
        v = 0
        for p in row:
            v = v << 4 | p
        words.append(v)
    return words


class Tiles:
    def __init__(self):
        self.tiles = []
        self.marks = []

    def mark(self, name):
        self.marks.append((name, len(self.tiles)))

    def add(self, tile):
        self.tiles.append(tile)
        return len(self.tiles) - 1


def stripes(lit, peak, on, off):
    """Four stripes of a 16-stripe bar in one tile: 1 point wide, 2 apart, rows 1-6."""
    t = blank()
    for i in range(4):
        c = on if i < lit or i == peak else off
        for y in range(1, 7):
            t[y][i * 2] = c
    return t


def level_patterns():
    """Every (lit, peak) a tile of a bar can show: lit 0-4, the peak one stripe past them or none."""
    pats = []
    for lit in range(5):
        pats.append((lit, -1))
    for lit in range(4):
        for peak in range(lit, 4):
            pats.append((lit, peak))
    return pats


def square(fill, free):
    t = blank()
    frame = I_GREY if free else I_DIM
    for i in range(1, 7):
        t[1][i] = t[6][i] = t[i][1] = t[i][6] = frame
    for y in range(6 - fill, 6):
        if not free:
            for x in range(2, 6):
                t[y][x] = I_CHIP
    return t


def wave_tiles(period, amp):
    """The mockup's square wave over 32 points, centred on row 4 of the tile row; 4 tiles."""
    a = [[0] * WAVE_WIDTH for _ in range(8)]
    if period is None:
        for x in range(WAVE_WIDTH):
            a[4][x] = I_DIM
    else:
        yt = 4 - amp // 2
        yb = yt + amp - 1
        x, hi = 0, True
        while x < WAVE_WIDTH:
            half = max(1, period // 2) if hi else period - max(1, period // 2)
            for i in range(x, min(x + half, WAVE_WIDTH)):
                a[yt if hi else yb][i] = I_CHIP
            x += half
            if x < WAVE_WIDTH:
                for y in range(yt, yb + 1):
                    a[y][x] = I_CHIP
            hi = not hi
    return [[row[t * 8:(t + 1) * 8] for row in a] for t in range(4)]


def tone_period(key):
    f = 440.0 * 2 ** ((key - 69) / 12.0)
    return PSG_CLOCK / (32.0 * f)


# Stage 24a2. The ribbon's tiles start at 384 (rom-screen.md 10.3): pattern n is tile 384 + n, its
# bits the tile's columns that hold a hit; then the hold tile and the noise's 8.
# Stage 24a3: the YM2612's envelope times in NTSC frames for the screen's estimate (hardware-specialist
# from BlastEm's ym2612.c and ym_common.c, 2026-11-13). KS is not counted. A decay at effective rate r
# takes DECAY over the whole 0..1023 range, linear in dB; an attack from silence takes ATTACK[AR].
EG_TICKS_PER_FRAME = 53693175 / 1008 / 3 / (53693175 / (3420 * 262))
ATTACK = [None, 1002.2, 501.1, 335.2, 250.6, 167.6, 125.3, 83.8, 62.6, 41.9, 31.3, 20.95, 15.66, 10.48, 7.83,
          5.24, 3.92, 2.62, 1.96, 1.31, 0.98, 0.66, 0.49, 0.33, 0.25, 0.18, 0.13, 0.09, 0.07, 0.04, 0.03, 0.0]


def decay_frames(r):
    if r < 2:
        return None
    if r < 6:
        a = 0.5
    elif r < 8:
        a = 0.75
    elif r < 48:
        a = (4 + (r & 3)) / 8.0
    elif r < 60:
        a = (4 + (r & 3)) / 4.0 * 2 ** ((r - 48) >> 2)
    else:
        a = 8.0
    shift = (47 - r) >> 2 if r < 48 else 0
    return 1023.0 * 2 ** shift / a / EG_TICKS_PER_FRAME


RIBBON_BASE = 384
ALGO_BASE = 1285
ALGO_ROWS = 4
LINE_Y = 1
BAR_ROWS = 4


def ribbon_pattern(n):
    """The line on the tile's 2nd row in dark grey; a hit is a bar 1x4 of the chip's colour hanging
    down from it, the line's own point included (as rom-screen.png)."""
    t = blank()
    for x in range(8):
        t[LINE_Y][x] = I_GREY
    for x in range(8):
        if n >> x & 1:
            for y in range(LINE_Y, LINE_Y + BAR_ROWS):
                t[y][x] = I_CHIP
    return t


def ribbon_hold():
    t = blank()
    for x in range(8):
        t[LINE_Y][x] = I_DIM
    return t


# Stage 31 (rom-screen.md 13, art-director): the ribbon's strikes in the owner's hue k = 1-3, in VRAM left
# free (10.3), a set of HUE_TILES each: the strike patterns with 1-3 strikes, then one strike + the hold
# line after it (bit 0-7), then the two combs. A cell of 4 or more strikes draws a comb of 4, every other
# column from its first strike's; a held cell with 2 or more strikes shows its strikes alone. The line
# stays grey and the hold the chip's dim (index 3): the empty and hold tiles are the chip's. k = 0, the
# DAC and Ch3 Special keep the chip's sets.
HUE_BASES = (905, 1092, 1336)
HUE_STRIKES = [n for n in range(1, 256) if bin(n).count("1") <= 3]
HUE_COMBS = (0x55, 0xAA)
HUE_TILES = len(HUE_STRIKES) + 8 + len(HUE_COMBS)


def hue_tile(n, ink, hold_ink=None):
    """The line in dark grey, strikes of ink hanging from it; with hold_ink the line after the last strike
    (all of it when there is none) in hold_ink."""
    t = blank()
    last = n.bit_length() - 1
    for x in range(8):
        t[LINE_Y][x] = hold_ink if hold_ink is not None and x > last else I_GREY
    for x in range(8):
        if n >> x & 1:
            for y in range(LINE_Y, LINE_Y + BAR_ROWS):
                t[y][x] = ink
    return t


def hue_set(k):
    ink = OWNER_INKS[k]
    tiles = [hue_tile(n, ink) for n in HUE_STRIKES]
    tiles += [hue_tile(1 << b, ink, I_DIM) for b in range(8)]
    tiles += [hue_tile(n, ink) for n in HUE_COMBS]
    return tiles


def hue_cell(held, bits):
    """A cell's tile within a hue set, by whether its striking key is down and its strike columns."""
    if bin(bits).count("1") > 3:
        first = (bits & -bits).bit_length() - 1
        return len(HUE_STRIKES) + 8 + (first & 1)
    if held and bin(bits).count("1") == 1:
        return len(HUE_STRIKES) + bits.bit_length() - 1
    return HUE_STRIKES.index(bits)


def ribbon_pattern_held(n):
    """Stage 30: pattern n while the key that struck in this cell is still down: the hold line from the
    last hit's column on, so it joins the strike (pattern 0 is the hold tile itself)."""
    t = ribbon_pattern(n)
    last = n.bit_length() - 1
    for x in range(last + 1, 8):
        t[LINE_Y][x] = I_DIM
    if n == 0:
        t = ribbon_hold()
    return t


I_NOISE = (12, 13, 14, 15)


def noise_tiles(periodic):
    """W19 over 32 points (the mockup's motion.py w19_texture): periodic stripes or white grain, each
    point one of the 4 cycling cells PAL3 12-15."""
    import random
    rnd = random.Random(19)
    a = [[0] * WAVE_WIDTH for _ in range(8)]
    for y in range(8):
        for x in range(WAVE_WIDTH):
            if periodic:
                k = (x // 2) % 4
                a[y][x] = I_NOISE[k] if (y % 2 == 0 or k < 2) else 0
            else:
                r = rnd.random()
                a[y][x] = I_NOISE[rnd.randrange(4)] if r < 0.55 else 0
    return [[row[t * 8:(t + 1) * 8] for row in a] for t in range(4)]


# The console's 8 levels of a colour component, and an RGB colour as the VDP's 0BGR.
LEVELS = (0, 52, 87, 116, 144, 172, 206, 255)


def vdp(rgb):
    out = 0
    for i, c in enumerate(rgb):
        step = min(range(8), key=lambda k: abs(LEVELS[k] - c))
        out |= (step * 2) << (4 * i)
    return out


def mix(a, b, f):
    return tuple(a[i] + (b[i] - a[i]) * f for i in range(3))


WHITE = (255, 255, 255)
# The live cells' chips (rom-screen.md 3): FM, PSG, channel 3 in Ch3 Special; held and dim.
LIVE_CHIPS = (((144, 144, 255), (52, 52, 144)), ((172, 255, 255), (0, 87, 116)), ((255, 144, 206), (116, 0, 52)))
# The mockup's live_colour (motion.py): white 3 frames, then over 18 to the target; a release mixes
# held to dim over 14.
FLASH_FRAMES = 21
RELEASE_FRAMES = 14


def flash(target, h):
    return WHITE if h < 3 else mix(WHITE, target, (h - 3) / 18.0)


# W19's colours (motion.py palette_for): 4 loudness steps; periodic runs a ramp every 2 frames,
# white shuffles 4 greys every frame (8 fixed shuffles here).
NOISE_RAMP = [(0, 255, 206), (0, 172, 144), (0, 87, 87), (0, 52, 52)]
NOISE_GREYS = [(255, 255, 255), (172, 172, 172), (87, 87, 87), (0, 0, 0)]
NOISE_SHUFFLES = 8


def noise_colours():
    import random
    rnd = random.Random(24)
    periodic = []
    white = []
    shuffles = []
    for _ in range(NOISE_SHUFFLES):
        s = list(NOISE_GREYS)
        rnd.shuffle(s)
        shuffles.append(s)
    for step in range(4):
        lv = (step + 1) / 4.0
        periodic.append([[vdp(mix((0, 0, 0), NOISE_RAMP[(k + p) % 4], 0.25 + 0.75 * lv)) for k in range(4)] for p in range(4)])
        white.append([[vdp(mix((0, 0, 0), s[k], 0.2 + 0.8 * lv)) for k in range(4)] for s in shuffles])
    silent = [[vdp((0, 52, 52)), 0, 0, 0], [vdp((52, 52, 52)), 0, 0, 0]]
    return periodic, white, silent


def main():
    glyphs = font()
    T = Tiles()

    T.mark("UI_T_NOTE")  # 3 copies of 20 glyphs, ink 6, 7, 8
    for idx in I_LIVE:
        for g in NOTE_GLYPHS:
            T.add(ink(glyphs[g], idx))
    T.mark("UI_T_DAC_NOTE")  # 20 glyphs, ink 11 on fill 3
    for g in NOTE_GLYPHS:
        T.add(ink(glyphs[g], I_DAC_INK, I_DIM))
    T.mark("UI_T_DAC_DIM")  # 20 glyphs in the font's index 1, which PAL2 makes dim orange
    for g in NOTE_GLYPHS:
        T.add(ink(glyphs[g], I_TEXT))
    T.mark("UI_T_DASH")
    T.add(ink(glyphs["-"], I_GREY))

    T.mark("UI_T_OWNER")  # 4 inks x 16 channels, then 4 stripes
    for colour in OWNER_INKS:
        for m in range(1, 17):
            s = str(m)
            if len(s) == 1:
                T.add(ink(glyphs[s], colour))
            else:
                t = blank()
                for d, ch in enumerate(s):
                    for y, row in enumerate(DIGITS_3X5[ch]):
                        for x, bit in enumerate(row):
                            if bit == "1":
                                t[1 + y][d * 4 + x] = colour
                T.add(t)
    # Dots 3-5, two dots clear of the owner's digit (art-director, 24a1).
    T.mark("UI_T_STRIPE")
    for colour in OWNER_INKS:
        t = blank()
        for y in range(8):
            for x in range(3, 6):
                t[y][x] = colour
        T.add(t)

    T.mark("UI_T_BRACKET")  # start, line, end; ink 2 on the mask
    for t in sheet_tiles(os.path.join(RES, "sprite", "borders.png"), lambda p: I_CHIP if p else I_MASK):
        T.add(t)
    T.mark("UI_T_LABEL_FM")  # YM2612 bottom to top, the font's index 1 on the mask
    fm_label = sheet_tiles(os.path.join(RES, "sprite", "device_fm.png"), lambda p: I_TEXT if p == 1 else I_MASK)
    for t in fm_label:
        T.add(t)
    T.mark("UI_T_LABEL_PSG")
    psg_label = sheet_tiles(os.path.join(RES, "sprite", "device_psg.png"), lambda p: I_TEXT if p == 1 else I_MASK)
    for t in psg_label:
        T.add(t)

    pats = level_patterns()
    T.mark("UI_T_LEVEL")  # green, yellow, red x patterns
    for colour in (I_GREEN, I_YELLOW, I_RED):
        for lit, peak in pats:
            T.add(stripes(lit, peak, colour, I_GREY))
    T.mark("UI_T_BAR")  # the CPU bar and the load lamps: the font's index 1 (grey in PAL3)
    for lit, peak in pats:
        T.add(stripes(lit, peak, I_TEXT, I_GREY))

    T.mark("UI_T_SQUARE")  # fill 0-4, then free
    for fill in range(5):
        T.add(square(fill, False))
    T.add(square(0, True))

    T.mark("UI_T_WAVE")
    seen = {}
    wave_map = []
    flat = None
    for t in wave_tiles(None, 0):
        key = tuple(map(tuple, t))
        if key not in seen:
            seen[key] = len(seen)
            T.add(t)
        flat = seen[key]
    for period in WAVE_PERIODS:
        per_amp = []
        for amp in (3, 5, 7):
            idxs = []
            for t in wave_tiles(period, amp):
                key = tuple(map(tuple, t))
                if key not in seen:
                    seen[key] = len(seen)
                    T.add(t)
                idxs.append(seen[key])
            per_amp.append(idxs)
        wave_map.append(per_amp)
    T.mark("UI_T_END")
    assert 16 + len(T.tiles) <= RIBBON_BASE

    B = Tiles()
    B.mark("UI_T_RIBBON")
    for n in range(256):
        B.add(ribbon_pattern(n))
    B.mark("UI_T_HOLD")
    B.add(ribbon_hold())
    B.mark("UI_T_NOISE")  # periodic 4, then white 4
    for periodic in (True, False):
        for t in noise_tiles(periodic):
            B.add(t)
    B.mark("UI_T_RIBBON_HOLD")
    for n in range(256):
        B.add(ribbon_pattern_held(n))
    B.mark("UI_T_B_END")
    noise_periodic, noise_white, noise_silent = noise_colours()

    # Stage 24a3 (D64): the CC 83 view's algorithm pictures as plane A tiles from 1285 (rom-screen.md
    # 10.3), from res/sprite/algor_N.bmp; index 3 becomes 15 (red) and 14 becomes 1 (white, 10.1).
    C = Tiles()
    C.mark("UI_T_ALGO")
    algo_seen = {}
    algo_map = []
    for n in range(8):
        tiles = sheet_tiles(os.path.join(RES, "sprite", "algor_%d.bmp" % n),
                            lambda px: {3: 15, 14: 1}.get(px, px))
        rows = Image.open(os.path.join(RES, "sprite", "algor_%d.bmp" % n)).size[1] // 8
        cells = []
        for t in tiles:
            key = tuple(map(tuple, t))
            if not any(any(row) for row in t):
                cells.append(0)
                continue
            if key not in algo_seen:
                algo_seen[key] = C.add(t)
            cells.append(algo_seen[key] + 1)
        cells += [0] * (6 * ALGO_ROWS - len(cells))
        algo_map.append(cells)
    C.mark("UI_T_C_END")
    # Stage 31's hue sets in the free ranges (rom-screen.md 10.3): after the ribbon's, before the FM curves
    # (1020); between the curves' 1092 and the PSG dot (1236); after the algorithm pictures, before the font (1440).
    assert RIBBON_BASE + len(B.tiles) <= HUE_BASES[0] and HUE_BASES[0] + HUE_TILES <= 1020
    assert HUE_BASES[1] >= 1092 and HUE_BASES[1] + HUE_TILES <= 1236
    assert HUE_BASES[2] >= ALGO_BASE + len(C.tiles) and HUE_BASES[2] + HUE_TILES <= 1440

    # Wave classes by the tone period the ROM wrote: class c holds keys 45+3c to 47+3c (A2 up).
    thresholds = [int(round(tone_period(45 + 3 * c - 0.5))) for c in range(1, len(WAVE_PERIODS))]

    note_table = []
    for p in range(128):
        if p < LOWEST_NAMED:
            s = "???"
        else:
            s = NAMES[p % 12] + (str(p // 12 - 1) if p >= 12 else "-")
        note_table.append([NOTE_GLYPHS.index(c) for c in s])

    h = []
    h.append("// Generated by tools/gen_rom_screen.py (MDMI plugin, stage 24a). Do not edit.")
    h.append("#pragma once")
    h.append("")
    h.append("// Where the screen's own tiles start in VRAM (rom-screen.md 10.3).")
    h.append("#define UI_TILE_BASE 16")
    for name, offset in T.marks:
        h.append(f"#define {name} (UI_TILE_BASE + {offset})")
    h.append(f"#define UI_TILE_COUNT {len(T.tiles)}")
    h.append("// Stage 24a2: the ribbon's patterns from 384, the hold tile and the noise's (rom-screen.md 10.3).")
    h.append(f"#define UI_TILE_B_BASE {RIBBON_BASE}")
    for name, offset in B.marks:
        h.append(f"#define {name} (UI_TILE_B_BASE + {offset})")
    h.append(f"#define UI_TILE_B_COUNT {len(B.tiles)}")
    h.append("// Stage 24a3: the CC 83 view's algorithm pictures from 1285 (rom-screen.md 10.3).")
    h.append(f"#define UI_TILE_C_BASE {ALGO_BASE}")
    for name, offset in C.marks:
        h.append(f"#define {name} (UI_TILE_C_BASE + {offset})")
    h.append(f"#define UI_TILE_C_COUNT {len(C.tiles)}")
    h.append(f"#define UI_HUE_TILES {HUE_TILES}")
    for k, base in enumerate(HUE_BASES, 1):
        h.append(f"#define UI_T_HUE_{k} {base}")
    h.append(f"#define UI_ALGO_COLS 6")
    h.append(f"#define UI_ALGO_ROWS {ALGO_ROWS}")
    h.append(f"#define UI_FLASH_FRAMES {FLASH_FRAMES}")
    h.append(f"#define UI_RELEASE_FRAMES {RELEASE_FRAMES}")
    h.append(f"#define UI_NOISE_SHUFFLES {NOISE_SHUFFLES}")
    h.append("#define UI_NOTE_GLYPHS 20")
    h.append(f"#define UI_GLYPH_QUESTION {NOTE_GLYPHS.index('?')}")
    h.append(f"#define UI_GLYPH_DASH {NOTE_GLYPHS.index('-')}")
    h.append(f"#define UI_LEVEL_PATTERNS {len(pats)}")
    h.append(f"#define UI_WAVE_CLASSES {len(WAVE_PERIODS)}")
    h.append(f"#define UI_WAVE_FLAT {flat}")
    h.append("")
    h.append("// The plugin's tests read the tile numbers alone.")
    h.append("#ifndef UI_TILE_NUMBERS_ONLY")
    h.append('#include "genesis.h"')
    h.append("extern const u32 UI_TILES[UI_TILE_COUNT * 8];")
    h.append("extern const u16 UI_PALETTE[64];")
    h.append("// A key's name as 3 note glyphs: C-4, C#4, C-- for octave -1 (stage 30).")
    h.append("extern const u8 UI_NOTE[128][3];")
    h.append("// A tile of a 16-stripe bar: its pattern by how many of its 4 stripes are lit and where")
    h.append("// the peak is, +1 (0 none).")
    h.append("extern const s8 UI_LEVEL_PATTERN[5][5];")
    h.append("// The tone period below which the wave has the next class, from A2's class 0 up.")
    h.append("extern const u16 UI_WAVE_PERIOD[UI_WAVE_CLASSES - 1];")
    h.append("// The wave's 4 tiles from UI_T_WAVE by class and height (3, 5, 7 points).")
    h.append("extern const u8 UI_WAVE[UI_WAVE_CLASSES][3][4];")
    h.append("extern const u32 UI_TILES_B[UI_TILE_B_COUNT * 8];")
    h.append("extern const u32 UI_TILES_C[UI_TILE_C_COUNT * 8];")
    h.append("// Stage 31: the ribbon's sets in the owner's hue k = 1-3, each at UI_HUE_BASE[k - 1].")
    h.append("extern const u32 UI_TILES_HUE[3][UI_HUE_TILES * 8];")
    h.append("extern const u16 UI_HUE_BASE[3];")
    h.append("// A strike cell's tile in a hue set: [its striking key is down][strike columns], 0 unused.")
    h.append("extern const u8 UI_HUE_CELL[2][256];")
    h.append("// The YM2612's envelope for the screen's estimate (24a3): an attack's frames and its step, a")
    h.append("// decay's frames over 0..1023 and its step by effective rate (D1R, D2R 2n; RR 4n + 2); steps")
    h.append("// are attenuation a frame in 10.6 fixed point. 65536 / n for fractions.")
    h.append("extern const u8 UI_ATTACK_FRAMES[32];")
    h.append("extern const u16 UI_ATTACK_STEP[32];")
    h.append("extern const u16 UI_DECAY_FRAMES[64];")
    h.append("extern const u16 UI_DECAY_STEP[64];")
    h.append("extern const u16 UI_RECIP[251];")
    h.append("extern const u32 UI_NIBBLES[256];")
    h.append("// An algorithm's picture, 6 x 4 cells by row: 0 empty, else the tile from UI_T_ALGO, + 1.")
    h.append("extern const u8 UI_ALGO[8][UI_ALGO_COLS * UI_ALGO_ROWS];")
    h.append("// A live note cell's colour by chip (FM, PSG, Ch3 Special): frames since the hit toward the held")
    h.append("// colour [0] or the dim one [1]; frames since the release from held to dim; held and dim.")
    h.append("extern const u16 UI_FLASH[3][2][UI_FLASH_FRAMES];")
    h.append("extern const u16 UI_RELEASE[3][UI_RELEASE_FRAMES];")
    h.append("extern const u16 UI_LIVE_HELD[3];")
    h.append("extern const u16 UI_LIVE_DIM[3];")
    h.append("// The noise's 4 cells PAL3 12-15 by loudness step: periodic by phase, white by shuffle; silent.")
    h.append("extern const u16 UI_NOISE_PERIODIC[4][4][4];")
    h.append("extern const u16 UI_NOISE_WHITE[4][UI_NOISE_SHUFFLES][4];")
    h.append("extern const u16 UI_NOISE_SILENT[2][4];")
    h.append("#endif")
    h.append(f"#define UI_LABEL_FM_TILES {len(fm_label)}")
    h.append(f"#define UI_LABEL_PSG_TILES {len(psg_label)}")
    h.append("")

    c = []
    c.append("// Generated by tools/gen_rom_screen.py (MDMI plugin, stage 24a). Do not edit.")
    c.append('#include "ui_tiles.h"')
    c.append("")
    c.append("const u32 UI_TILES[UI_TILE_COUNT * 8] = {")
    for tile in T.tiles:
        c.append("    " + ", ".join(f"0x{w:08X}" for w in pack(tile)) + ",")
    c.append("};")
    c.append("")
    c.append("const u16 UI_PALETTE[64] = {")
    for pal in PALETTE:
        c.append("    " + ", ".join(f"0x{v:03X}" for v in pal) + ",")
    c.append("};")
    c.append("")
    c.append("const u8 UI_NOTE[128][3] = {")
    for i in range(0, 128, 8):
        c.append("    " + " ".join("{ %d, %d, %d }," % tuple(n) for n in note_table[i:i + 8]))
    c.append("};")
    c.append("")
    table = [[-1] * 5 for _ in range(5)]
    for i, (lit, peak) in enumerate(pats):
        table[lit][peak + 1] = i
    c.append("const s8 UI_LEVEL_PATTERN[5][5] = {")
    for row in table:
        c.append("    { " + ", ".join(str(v) for v in row) + " },")
    c.append("};")
    c.append("")
    c.append("const u16 UI_WAVE_PERIOD[UI_WAVE_CLASSES - 1] = { " + ", ".join(str(t) for t in thresholds) + " };")
    c.append("")
    c.append("const u8 UI_WAVE[UI_WAVE_CLASSES][3][4] = {")
    for per_amp in wave_map:
        c.append("    { " + ", ".join("{ %d, %d, %d, %d }" % tuple(i) for i in per_amp) + " },")
    c.append("};")
    c.append("")

    # Envelope tables (24a3): frames capped to 250 (the screen's u8 counters), and the attenuation
    # (0..1023) a frame moves, in 10.6 fixed point; 0 is no move, 65535 a full move in one frame.
    def q(frames):
        if frames is None:
            return 0
        return 65535 if frames <= 1.0 else min(65535, int(round(1023 * 64 / frames)))

    def cap(frames):
        if frames is None:
            return 250
        return max(1, min(250, int(round(frames))))
    # An attack longer than the counters (250 frames) is 255: it never ends on the screen.
    c.append("const u8 UI_ATTACK_FRAMES[32] = { " + ", ".join(str(0 if a is None else 255 if a > 250 else cap(a) if a >= 0.5 else 0) for a in ATTACK) + " };")
    c.append("const u16 UI_ATTACK_STEP[32] = { " + ", ".join(str(0 if a is None else q(a)) for a in ATTACK) + " };")
    c.append("const u16 UI_DECAY_FRAMES[64] = { " + ", ".join(str(65535 if decay_frames(r) is None else min(65535, int(round(decay_frames(r))))) for r in range(64)) + " };")
    c.append("const u16 UI_DECAY_STEP[64] = { " + ", ".join(str(q(decay_frames(r))) for r in range(64)) + " };")
    c.append("// A byte's 8 bits as 8 nibbles (bit 7 the first point), for tile rows without a loop a point.")
    c.append("const u32 UI_NIBBLES[256] = { " + ", ".join("0x%08X" % sum(1 << (4 * (7 - i)) for i in range(8) if n >> (7 - i) & 1) for n in range(256)) + " };")
    c.append("// 65536 / n for n frames 1..250, for fractions without division.")
    c.append("const u16 UI_RECIP[251] = { 0, " + ", ".join(str(min(65535, 65536 // n)) for n in range(1, 251)) + " };")
    c.append("")
    c.append("const u32 UI_TILES_C[UI_TILE_C_COUNT * 8] = {")
    for tile in C.tiles:
        c.append("    " + ", ".join(f"0x{w:08X}" for w in pack(tile)) + ",")
    c.append("};")
    c.append("const u8 UI_ALGO[8][UI_ALGO_COLS * UI_ALGO_ROWS] = {")
    for cells in algo_map:
        c.append("    { " + ", ".join(str(v) for v in cells) + " },")
    c.append("};")
    c.append("")
    c.append("const u32 UI_TILES_B[UI_TILE_B_COUNT * 8] = {")
    for tile in B.tiles:
        c.append("    " + ", ".join(f"0x{w:08X}" for w in pack(tile)) + ",")
    c.append("};")
    c.append("")
    c.append("const u32 UI_TILES_HUE[3][UI_HUE_TILES * 8] = {")
    for k in (1, 2, 3):
        c.append("    {")
        for tile in hue_set(k):
            c.append("        " + ", ".join(f"0x{w:08X}" for w in pack(tile)) + ",")
        c.append("    },")
    c.append("};")
    c.append("const u16 UI_HUE_BASE[3] = { " + ", ".join(str(b) for b in HUE_BASES) + " };")
    c.append("const u8 UI_HUE_CELL[2][256] = {")
    for held in (0, 1):
        c.append("    { 0, " + ", ".join(str(hue_cell(held, n)) for n in range(1, 256)) + " },")
    c.append("};")
    c.append("")

    def row(values):
        return "{ " + ", ".join(f"0x{v:03X}" for v in values) + " }"

    c.append("const u16 UI_FLASH[3][2][UI_FLASH_FRAMES] = {")
    for held, dim in LIVE_CHIPS:
        c.append("    { " + ", ".join(row(vdp(flash(t, h)) for h in range(FLASH_FRAMES)) for t in (held, dim)) + " },")
    c.append("};")
    c.append("const u16 UI_RELEASE[3][UI_RELEASE_FRAMES] = {")
    for held, dim in LIVE_CHIPS:
        c.append("    " + row(vdp(mix(held, dim, s_ / float(RELEASE_FRAMES))) for s_ in range(RELEASE_FRAMES)) + ",")
    c.append("};")
    c.append("const u16 UI_LIVE_HELD[3] = " + row(vdp(h_) for h_, d_ in LIVE_CHIPS) + ";")
    c.append("const u16 UI_LIVE_DIM[3] = " + row(vdp(d_) for h_, d_ in LIVE_CHIPS) + ";")
    c.append("")
    c.append("const u16 UI_NOISE_PERIODIC[4][4][4] = {")
    for step in noise_periodic:
        c.append("    { " + ", ".join(row(p) for p in step) + " },")
    c.append("};")
    c.append("const u16 UI_NOISE_WHITE[4][UI_NOISE_SHUFFLES][4] = {")
    for step in noise_white:
        c.append("    { " + ", ".join(row(p) for p in step) + " },")
    c.append("};")
    c.append("const u16 UI_NOISE_SILENT[2][4] = { " + ", ".join(row(p) for p in noise_silent) + " };")
    c.append("")

    src = os.path.join(MDMI, "src")
    with open(os.path.join(src, "ui_tiles.h"), "w", newline="\n") as f:
        f.write("\n".join(h))
    with open(os.path.join(src, "ui_tiles.c"), "w", newline="\n") as f:
        f.write("\n".join(c))
    print(f"{len(T.tiles)} + {len(B.tiles)} + {len(C.tiles)} tiles, wave {len(seen)} distinct, thresholds {thresholds}")


if __name__ == "__main__":
    main()
