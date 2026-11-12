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
    [0x000, 0x888, 0xA2E, 0x206, 0x222, 0x000, PSG_BRIGHT, 0x000, 0x000, 0x640, 0x640, 0xA2E, 0x222, 0x222, 0x222, 0x222],
]
I_TEXT, I_CHIP, I_DIM, I_GREY, I_MASK = 1, 2, 3, 4, 5
I_LIVE = (6, 7, 8)
I_DAC_INK = 11
I_GREEN, I_YELLOW, I_RED = 12, 13, 14
OWNER_INKS = (I_CHIP, 12, 13, 14)

NOTE_GLYPHS = "0123456789ABCDEFG#-?"
NAMES = ["C-", "C#", "D-", "D#", "E-", "F-", "F#", "G-", "G#", "A-", "A#", "B-"]
LOWEST_NAMED = 21  # A0, as the old screen

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

    # Wave classes by the tone period the ROM wrote: class c holds keys 45+3c to 47+3c (A2 up).
    thresholds = [int(round(tone_period(45 + 3 * c - 0.5))) for c in range(1, len(WAVE_PERIODS))]

    note_table = []
    for p in range(128):
        if p < LOWEST_NAMED:
            s = "???"
        else:
            s = NAMES[p % 12] + str(p // 12 - 1)
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
    h.append("// A key's name as 3 note glyphs: C-4, C#4, ??? below A0.")
    h.append("extern const u8 UI_NOTE[128][3];")
    h.append("// A tile of a 16-stripe bar: its pattern by how many of its 4 stripes are lit and where")
    h.append("// the peak is, +1 (0 none).")
    h.append("extern const s8 UI_LEVEL_PATTERN[5][5];")
    h.append("// The tone period below which the wave has the next class, from A2's class 0 up.")
    h.append("extern const u16 UI_WAVE_PERIOD[UI_WAVE_CLASSES - 1];")
    h.append("// The wave's 4 tiles from UI_T_WAVE by class and height (3, 5, 7 points).")
    h.append("extern const u8 UI_WAVE[UI_WAVE_CLASSES][3][4];")
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

    src = os.path.join(MDMI, "src")
    with open(os.path.join(src, "ui_tiles.h"), "w", newline="\n") as f:
        f.write("\n".join(h))
    with open(os.path.join(src, "ui_tiles.c"), "w", newline="\n") as f:
        f.write("\n".join(c))
    print(f"{len(T.tiles)} tiles, wave {len(seen)} distinct, thresholds {thresholds}")


if __name__ == "__main__":
    main()
