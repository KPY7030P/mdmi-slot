"""E7: judge numbers of the screen section of several reg_test logs, and where the logs first differ."""
import re, sys

def lines(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        return [l.rstrip("\n") for l in f]

def screen(ls):
    out, on = [], False
    for l in ls:
        if l.startswith("screen (stage 24a)"):
            on = True
        elif on and l and not l.startswith(" "):
            if not l.startswith("  FAIL"):
                break
        if on:
            out.append(l)
    return out

KEYS = [
    ("chord of 6", r"chord of 6\s+update\s+(\d+)"),
    ("all ten", r"all ten\s+update\s+(\d+)"),
    ("program change", r"program change on all\s+update\s+(\d+)"),
    ("all notes off", r"all notes off\s+update\s+(\d+)"),
    ("DAC on", r"DAC on\s+update\s+(\d+)"),
    ("4 DAC voices", r"4 DAC voices\s+update\s+(\d+)"),
    ("DAC off", r"DAC off\s+update\s+(\d+)"),
    ("Ch3 Special on", r"Ch3 Special on\s+update\s+(\d+)"),
    ("Ch3 OPS notes", r"Ch3 OPS notes\s+update\s+(\d+)"),
    ("Ch3 Special off", r"Ch3 Special off\s+update\s+(\d+)"),
    ("PSG portamento", r"PSG portamento C3-C6\s+update\s+(\d+)"),
    ("idle drawing", r"idle drawing ([\d.]+)"),
    ("one note", r"one note's frames draw in (\d+) and (\d+)"),
    ("steady chord", r"steady chord of 6: ([\d.]+) lines a frame on average, (\d+) at worst"),
]

def judges(ls):
    text = "\n".join(screen(ls))
    row = {}
    for name, rx in KEYS:
        m = re.search(rx, text)
        row[name] = "/".join(m.groups()) if m else "-"
    row["fails"] = str(sum(1 for l in ls if "FAIL" in l and "FAILED" not in l))
    return row

NOISE = re.compile(r"\bms\b|MB|took \d+ lines|\d+ ms|private memory|worst run|seconds|_ms")

def first_difference(a, b):
    i = j = 0
    for k in range(min(len(a), len(b))):
        if a[k] != b[k] and not (NOISE.search(a[k]) and NOISE.search(b[k])):
            return k, a[k], b[k]
    return None

if __name__ == "__main__":
    paths = sys.argv[1:]
    rows = [judges(lines(p)) for p in paths]
    names = [k for k, _ in KEYS] + ["fails"]
    print("judge".ljust(18) + "".join(p.split("/")[-1].replace(".log", "").rjust(12) for p in paths))
    for n in names:
        print(n.ljust(18) + "".join(r[n].rjust(12) for r in rows))
    base = lines(paths[0])
    for p in paths[1:]:
        d = first_difference(base, lines(p))
        print(p.split("/")[-1], "first difference vs", paths[0].split("/")[-1], ":", "none" if not d else "line %d\n   %s\n   %s" % d)
