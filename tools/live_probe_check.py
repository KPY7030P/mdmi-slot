"""Live plays the check set: MDMI in real Ableton Live through the owner's ableton-probe (stage 57, D96).

    python tools/live_probe_check.py [RUN ...]      (RUN: а б в г д е, the plan's names; all when none;
                                                     a part writes summary-part.txt, the full summaries stay)
    python tools/live_probe_check.py --notes        (stage 58: bars 110-114 of the check set's events for
                                                     reg_test --dac-loop, build/stage58/notes-110-114.txt)

Runs the plan below one run at a time. Each run: build the probe set (tools/make_probe_set.py), play it with
probe.py, then check what came back: Live opened, played to the end and closed by itself; the pulse track has a
note on every beat (no hole: Live never stood); MDMI's recorded sound (the "Rec Audio" track) sounds on every beat
where the set has a note for "MDMI песня" and is silent where it has none; after off, delete and in the loop as
the plan says. One line per run goes to build/live-probe/summary.txt, with the PNG and WAV it was judged on.
The previous summary is kept as summary-prev.txt and the verdicts are compared.

Shared Live (ableton-probe README): Live is touched only through probe.py. Exit 3 (another session's run) or
"Live already running" with a free lock (the owner's) means wait: 2 minutes between tries, at most an hour, then
the run is "не сыгран", not failed. --keep is never used.
"""
import glob, gzip, json, math, os, re, shutil, subprocess, sys, time
import xml.etree.ElementTree as ET


import numpy as np

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PROBE = r'D:\ALL\MUSIC\TECH\MYPLUGINS\ableton-probe\probe.py'   # the owner's tool, the same from any worktree
CFG = os.path.join(REPO, 'ableton-probe.json')
PROJECT = os.path.join(REPO, 'build', 'probe', 'MDMI Test Project')
COPY = os.path.join(PROJECT, 'MDMI Auto.als')
TOOL_OUT = os.path.join(REPO, 'build', 'ableton-probe')
OUT = os.path.join(REPO, 'build', 'live-probe')
CRASH = os.path.expandvars(r'%APPDATA%\Ableton\Live 12.3\Preferences\Crash')
TRACK, DEVICE, AUDIO, SONG_OUT = 'MDMI песня', 'MDMI Slot', 'Rec Audio', 'MidiOut/Track.12/'
BB = 4                       # beats in a bar
LATE = 0.4                   # play from the middle of a set starts ~0.4 beat late; delete acts ~0.4 beat late
WAIT_STEP, WAIT_MAX = 120, 3600

# The plan (stage 57, item 4). Order matters: the whole set first as the base, then the actions. Since stage 59
# (D101) the off/on of the old runs б (held chord) and в (DAC drums) go inside а: each launch costs ~60 s of Live
# for 14-20 s of play. Deletes stay runs of their own: the recording ends with MDMI (D96).
PLAN = [
    dict(name='а', what='весь набор, выкл/вкл на аккорде и на DAC', bars=(1, 118),
         at=['2.3:off', '3.3:on', '4.3:off', '6.3:on', '112.1:off', '112.3:on', '113.2:off', '113.4:on']),
    dict(name='г', what='удаление на аккорде при DAC', bars=(1, 6), at=['3.3:delete']),
    dict(name='д', what='удаление посреди барабанов DAC', bars=(110, 114), at=['113.2:delete']),
    dict(name='е', what='луп 111-114 x8', bars=(110, 114), loop=(111, 114), passes=8),
]

# RMS of one beat of MDMI's sound, dBFS. Set once from run а (stage 57, item 5) and not moved for the others.
# Run а on 0.48.0 (2026-11-22): beats with a note -51 dB and louder, beats without notes digital zero (-200)
# or below -108 dB. A silent beat is measured without its last 0.1 beat: a note on the next beat may start a
# sample early (-0.5 ms at 87.1).
SOUND_DB, SILENCE_DB = -70, -90
QUIET_TAIL = 0.01            # beats (5 ms) left off the end of a silent window: a note starts a sample early
# Run е prints the kick of 113.3 in every pass. Until stage 58 a lost kick there was a third outcome (D97); its
# cause was MDMI's (D99), fixed in 0.49.0, so run е only passes or fails. A summary written before that may still
# hold the old outcome, read as a pass when the verdicts are compared.
KICK_BEAT = (113 - 1) * BB + 2
OLD_KNOWN = 'ИЗВЕСТНЫЙ ДЕФЕКТ (этап 58)'
MIN_WINDOW = 0.1            # beats; a shorter silent window after off is not measured
EDGE = 0.05                  # beats (25 ms) left off each side of a beat when loop passes are compared
ACT_LATE = 0.25              # beats; probe.py sends an action 20-40 ms (<0.1 beat at 120) after the point
# The device answers a delete up to ~0.45 beat after probe.py sent it (run д: sent 113.2.035, recording ended
# 113.2.13, /acted 113.2.455), so the deletion lies between the two.
# Beats with a note where silence is right, 'bar.beat': reason. No reason, no exception.
PSG_DECAY = 'К7 PSG: огибающая звука канала гаснет за ~0,4 с, нота в две доли длиннее звука (первая доля проверяется)'
EXCEPTIONS = {'101.2': PSG_DECAY, '101.4': PSG_DECAY, '102.2': PSG_DECAY, '102.4': PSG_DECAY}


def log(*a):
    print(*a, flush=True)


# ---------- Live through probe.py ----------

def probe(*args):
    p = subprocess.run([sys.executable, PROBE] + list(args), capture_output=True, text=True, encoding='utf-8',
                       errors='replace', cwd=REPO)
    return p.returncode, p.stdout + p.stderr


def live_state():
    _, out = probe('status')
    return ('занято' in out.split('\n')[0]), ('Live запущен' in out)


def play(run):
    """probe.py run for one plan item, waiting while Live is someone else's. (code, log) or None (not played)."""
    a, b = run['bars']
    args = ['run', '--config', CFG, '--bars', f'{a}-{b}']
    for x in run.get('at', []):
        args += ['--at', x]
    if run.get('at'):
        args += ['--device-track', TRACK, '--device', DEVICE]
    if run.get('loop'):
        args += ['--loop', '%d-%d' % run['loop'], '--passes', str(run['passes'])]
    t0 = time.time()
    while True:
        busy, running = live_state()
        if not busy and not running:
            code, out = probe(*args)
            if code != 3 and 'Live уже запущен' not in out:
                return code, out
        if time.time() - t0 > WAIT_MAX:
            return None
        log(f'  Live занят ({"чужой прогон" if busy else "владелец"}), жду {WAIT_STEP} с')
        time.sleep(WAIT_STEP)


def check_set(path):
    """Problems Live would reject the set for, before Live sees it: [] when none. Live leaves a set it calls
    corrupt open on a dialog that probe.py cannot close, so this runs before every run."""
    ls = ET.fromstring(gzip.open(path).read()).find('LiveSet')
    scenes = len(list(ls.find('Scenes')))
    returns = sum(1 for t in ls.find('Tracks') if t.tag == 'ReturnTrack')
    bad = []
    for t in ls.find('Tracks'):
        if t.tag == 'ReturnTrack':
            continue
        nm = t.find('Name/EffectiveName').get('Value')
        for seq in ('MainSequencer', 'FreezeSequencer'):
            n = len(t.findall(f'DeviceChain/{seq}/ClipSlotList/ClipSlot'))
            if n != scenes:
                bad.append(f'«{nm}» {seq}: {n} слотов при {scenes} сценах')
        n = len(list(t.find('DeviceChain/Mixer/Sends')))
        if n != returns:
            bad.append(f'«{nm}»: {n} посылов при {returns} возвратных')
    return bad


# ---------- the set: what should sound ----------

def song_events(ls):
    """Every Note On and Note Off the set sends to "MDMI песня" (stage 58): (beat, 'on'|'off', channel 1-16,
    key, velocity, note number), sorted by beat; a note's off is at its end, cut at its clip's end. The channel
    is the track's output, DeviceIn.3.{ch} -> ch + 1. The note number pairs an on with its off."""
    res = []
    for t in ls.find('Tracks'):
        o = t.find('DeviceChain/MidiOutputRouting/Target')
        if o is None or not o.get('Value', '').startswith(SONG_OUT):
            continue
        m = re.search(r'DeviceIn\.\d+\.(\d+)$', o.get('Value'))
        ch = int(m.group(1)) + 1 if m else 0
        for c in t.find('DeviceChain/MainSequencer/ClipTimeable/ArrangerAutomation/Events').iter('MidiClip'):
            if c.find('Disabled').get('Value') == 'true':
                continue
            st, en = float(c.find('CurrentStart').get('Value')), float(c.find('CurrentEnd').get('Value'))
            ls0 = float(c.find('Loop/LoopStart').get('Value'))
            for kt in c.iter('KeyTrack'):
                key = int(kt.find('MidiKey').get('Value'))
                for n in kt.iter('MidiNoteEvent'):
                    if n.get('IsEnabled') == 'false':
                        continue
                    s = st + float(n.get('Time')) - ls0
                    if st <= s < en:
                        i = len(res) // 2
                        res.append((s, 'on', ch, key, int(float(n.get('Velocity'))), i))
                        res.append((min(en, s + float(n.get('Duration'))), 'off', ch, key,
                                    int(float(n.get('OffVelocity', '64'))), i))
    # at one beat offs first, as a sequencer ends a note before the next one on the same key
    return sorted(res, key=lambda e: (e[0], e[1] == 'on', e[2], e[3]))


def song_notes(ls):
    """(start, end) beats of every note the set sends to "MDMI песня"."""
    ev = song_events(ls)
    on = {e[5]: e[0] for e in ev if e[1] == 'on'}
    return sorted((on[e[5]], e[0]) for e in ev if e[1] == 'off')


def tempo_of(ls):
    return float(ls.find('MainTrack/DeviceChain/Mixer/Tempo/Manual').get('Value'))


# ---------- the notes for reg_test --dac-loop (stage 58) ----------

CHECK_SET = os.path.join(REPO, 'build', 'live-check', 'MDMI check Project', 'MDMI check.als')
STAGE58 = os.path.join(REPO, 'build', 'stage58')
DAC_KEYS = (35, 37, 41, 48)                 # K6's kit in bars 111-114 (tools/make_live_check.py)


def state_dac_block(ls):
    """The DAC block (src/state.c, BLOCK_DAC 9) of "MDMI песня"'s state, and its notes: [(key, name, mode)]
    with mode as dac_block_write writes it (0 cuts, 1 one-shot, 2 loop with release, 4 loop stopped)."""
    t = next(t for t in ls.find('Tracks') if t.find('Name/EffectiveName').get('Value') == TRACK)
    ps = next(t.iter('ProcessorState'))
    raw = bytes.fromhex(''.join((ps.text or '').split()))
    at = 4                                   # the state's version, then blocks of (type, length)
    while at + 8 <= len(raw):
        typ, n = int.from_bytes(raw[at:at + 4], 'little'), int.from_bytes(raw[at + 4:at + 8], 'little')
        if typ == 9:
            block, notes, i = raw[at + 8:at + 8 + n], [], 0
            while i + 3 <= len(block):
                hl = int.from_bytes(block[i + 1:i + 3], 'little')
                h = block[i + 3:i + 3 + hl]
                frames, length = int.from_bytes(h[36:40], 'little'), int.from_bytes(h[57:61], 'little')
                notes.append((block[i], h[:32].split(b'\0')[0].decode('latin1'), h[54]))
                i += 3 + hl + frames * 2 + length
            return block, notes
        at += 8 + n
    return None, []


def write_notes(bars=(110, 114), loop=(111, 114)):
    """build/stage58/notes-110-114.txt: the check set's events of the bars, one a line, «beat on|off channel
    key velocity»; notes held past the loop's end get their off there, as Live sends at the wrap. The
    state's DAC block goes next to it when it has samples on the kit's keys (the header says which)."""
    ls = ET.fromstring(gzip.open(CHECK_SET).read()).find('LiveSet')
    a, b = (bars[0] - 1) * BB, bars[1] * BB
    l0, l1 = (loop[0] - 1) * BB, loop[1] * BB
    ev = song_events(ls)
    on = {e[5]: e[0] for e in ev if e[1] == 'on'}
    lines = []
    for e in ev:
        start = on[e[5]]
        if not a <= start < b:
            continue
        beat = min(e[0], l1) if l0 <= start < l1 else e[0]
        lines.append((beat, e[1] == 'on', e[2], e[3], f'{beat:g} {e[1]} {e[2]} {e[3]} {e[4]}'))
    lines.sort(key=lambda x: x[:4])
    block, samples = state_dac_block(ls)
    own = [s for s in samples if s[0] in DAC_KEYS]
    os.makedirs(STAGE58, exist_ok=True)
    head = [f'# {os.path.relpath(CHECK_SET, REPO).replace(os.sep, '/')}, bars {bars[0]}-{bars[1]}, events to «{TRACK}»',
            f'tempo {tempo_of(ls):g}',
            f'loop {l0:g} {l1:g}',
            f'# loop = bars {loop[0]}-{loop[1]}; beats from the set\'s start, 4 a bar']
    for k in DAC_KEYS:
        s = next((s for s in own if s[0] == k), None)
        head.append(f'# key {k}: ' + (f'tst6 sample «{s[1]}», mode {s[2]} ({ {0: "Note Off cuts", 1: "one-shot", 2: "loop with release", 4: "loop, Note Off stops"}.get(s[2], "?")})'
                                       if s else 'the ROM\'s sample'))
    if own:
        open(os.path.join(STAGE58, 'dac-block.bin'), 'wb').write(block)
        head.append('dac dac-block.bin')
        head.append('# dac-block.bin: the state\'s whole DAC block (' + ', '.join(f'{k} {n}' for k, n, _ in samples) + ')')
    out = os.path.join(STAGE58, f'notes-{bars[0]}-{bars[1]}.txt')
    open(out, 'w', encoding='utf-8', newline='\n').write('\n'.join(head + [x[4] for x in lines]) + '\n')
    log(f'{out}: {len(lines)} events; ' + '; '.join(h[2:] for h in head if h.startswith('# key')))
    return 0


# ---------- the recording ----------

class Take:
    """One recorded audio clip: arrangement beats -> samples of its WAV."""

    def __init__(self, clip, tempo, project):
        self.start = float(clip.find('CurrentStart').get('Value'))
        self.end = float(clip.find('CurrentEnd').get('Value'))
        self.loop_start = float(clip.find('Loop/LoopStart').get('Value'))
        fr = clip.find('SampleRef/FileRef')
        path = fr.find('Path').get('Value')
        if not os.path.exists(path):
            path = os.path.join(project, fr.find('RelativePath').get('Value'))
        self.path = os.path.normpath(path)
        self.marks = sorted((float(m.get('BeatTime')), float(m.get('SecTime'))) for m in clip.iter('WarpMarker'))
        self.warped = clip.find('IsWarped').get('Value') == 'true'
        self.tempo = tempo
        with open(self.path, 'rb') as f:
            self.rate, self.power = read_wav(f.read())

    def sec(self, beat):
        b = self.loop_start + beat - self.start
        m = self.marks
        if not self.warped or len(m) < 2:
            return b * 60 / self.tempo
        i = 0
        while i < len(m) - 2 and b > m[i + 1][0]:
            i += 1
        (b0, s0), (b1, s1) = m[i], m[i + 1]
        return s0 + (b - b0) * (s1 - s0) / (b1 - b0)

    def rms_db(self, x0, x1):
        """RMS of beats [x0, x1) in dBFS; None outside the recording."""
        if x0 < self.start - 1e-6 or x1 > self.end + 1e-6:
            return None
        i0, i1 = int(round(self.sec(x0) * self.rate)), int(round(self.sec(x1) * self.rate))
        if i0 < 0 or i1 > len(self.power) or i1 <= i0:
            return None
        r = float(np.sqrt(np.mean(self.power[i0:i1])))
        return 20 * math.log10(r) if r > 1e-10 else -200.0


def read_wav(raw):
    """(rate, float samples, all channels flat) from a RIFF WAV: PCM 16/24/32 or float 32."""
    assert raw[:4] == b'RIFF' and raw[8:12] == b'WAVE', 'not a WAV'
    i, fmt, data = 12, None, None
    while i + 8 <= len(raw):
        cid, n = raw[i:i + 4], int.from_bytes(raw[i + 4:i + 8], 'little')
        body = raw[i + 8:i + 8 + n]
        if cid == b'fmt ':
            fmt = body
        elif cid == b'data':
            data = body
        i += 8 + n + (n & 1)
    tag, ch, rate = int.from_bytes(fmt[0:2], 'little'), int.from_bytes(fmt[2:4], 'little'), \
        int.from_bytes(fmt[4:8], 'little')
    bits = int.from_bytes(fmt[14:16], 'little')
    if tag == 0xFFFE:
        tag = int.from_bytes(fmt[24:26], 'little')
    if tag == 3:
        x = np.frombuffer(data[:len(data) // 4 * 4], '<f4').astype(np.float64)
    elif bits == 16:
        x = np.frombuffer(data[:len(data) // 2 * 2], '<i2') / 32768.0
    elif bits == 24:
        b = np.frombuffer(data[:len(data) // 3 * 3], np.uint8).reshape(-1, 3).astype(np.int32)
        v = b[:, 0] | (b[:, 1] << 8) | (b[:, 2] << 16)
        x = np.where(v >= 1 << 23, v - (1 << 24), v) / float(1 << 23)
    else:
        x = np.frombuffer(data[:len(data) // 4 * 4], '<i4') / float(1 << 31)
    x = x[:len(x) // ch * ch].reshape(-1, ch)
    return rate, np.mean(x * x, axis=1)          # power of each frame, channels averaged


def takes_of(ls, project):
    """Main lane clips and take-lane clips of the audio record track."""
    t = next(t for t in ls.find('Tracks') if t.find('Name/EffectiveName').get('Value') == AUDIO)
    tempo = tempo_of(ls)
    main = [Take(c, tempo, project) for c in
            t.find('DeviceChain/MainSequencer/Sample/ArrangerAutomation/Events').iter('AudioClip')]
    lanes = []
    tl = t.find('TakeLanes/TakeLanes')
    for lane in (tl if tl is not None else []):
        ev = lane.find('ClipAutomation/Events')
        lanes.append([Take(c, tempo, project) for c in ev.iter('AudioClip')] if ev is not None else [])
    return main, lanes


def rms_at(takes, x0, x1):
    for t in takes:
        r = t.rms_db(x0, x1)
        if r is not None:
            return r
    return None


# ---------- one run ----------

def judge(run, code, out, crashes_before):
    """Verdict of one run: (passed, [problems], [facts], [observations], files). files['tool'] lists what the
    ableton-probe side did not do (D100): the run is «не в счёт» when that is all that went wrong."""
    a, b = run['bars']
    bad, facts, seen, tool = [], [], [], []
    m = re.search(r'Live открыл набор за (\d+) с', out)
    opened = int(m.group(1)) if m else None
    if opened is None:
        bad.append('Live не открыл копию')
    if 'стоп по таймеру' in out:
        tool.append('стоп по таймеру (позиция не дошла до конца)')
    if 'Live не закрылся' in out:
        bad.append('Live не закрылся сам за 30 с')
        probe('close', '--config', CFG)
    if 'ОШИБКА' in out:
        bad.append('probe.py: ' + out.split('ОШИБКА:', 1)[1].split('\n')[0].strip())
    if live_state()[1]:
        bad.append('Live остался запущенным')
    crashes = set(os.listdir(CRASH)) if os.path.isdir(CRASH) else set()
    if crashes - crashes_before:
        bad.append('Live записал отчёт о падении: ' + ', '.join(sorted(crashes - crashes_before)))
    m = re.search(r'сохранено; снимок: (.*)', out)
    pngs = [p.strip() for p in m.group(1).split(',')] if m else []
    if len(pngs) != 1:
        bad.append(f'снимок конца прогона: {len(pngs)} окон Live (диалог?)')
    files = {'png': pngs[0] if pngs else None, 'tool': tool}
    m = re.search(r'play с (\d+)\.(\d+\.\d+)', out)
    p0 = (int(m.group(1)) - 1) * BB + float(m.group(2)) - 1 if m else (a - 1) * BB
    if code != 0:
        bad.append(f'probe.py код {code}')
    if code is None or not os.path.exists(os.path.join(TOOL_OUT, 'notes.json')) or opened is None:
        return False, bad, facts, seen, files
    notes = json.load(open(os.path.join(TOOL_OUT, 'notes.json'), encoding='utf-8'))
    shutil.copy(os.path.join(TOOL_OUT, 'notes.json'), os.path.join(OUT, run['name'] + '-notes.json'))
    acts = notes.get('actions') or []
    for x in acts:
        if x.get('acted_beat') is None:
            bad.append(f'{x["action"]} на {x["at"]}: устройство не ответило')
        elif x['sent_beat'] - x['beat'] > ACT_LATE:
            # probe.py fires an action on the first position the device reports past the point: a late one
            # means positions stopped coming, and the run did not test what it should (not MDMI's fault)
            tool.append(f'{x["action"]} задан на {x["at"]}, послан на {bar_beat(x["sent_beat"])}: позиция от устройства '
                        f'ableton-probe пришла поздно, прогон не проверил своё')

    # pulse: one note per beat, from where play really started
    if run.get('loop'):
        loop_pulse(run, notes, out, bad, tool, facts)
    else:
        want = list(range((a - 1) * BB, b * BB))
        ts = [n['time_beats'] for n in notes['notes'] if n['key'] == 60]
        miss = [k for k in want if not any(abs(t - k) < 0.1 for t in ts)]
        facts.append(f'пульс {len(want) - len(miss)}/{len(want)}')
        if miss and miss != [want[0]] or (miss and p0 < want[0] + 0.05):   # the first beat may go with a late start
            bad.append('дыры в пульсе на долях ' + ', '.join(bar_beat(k) for k in miss[:8]))

    # sound
    root = ET.fromstring(gzip.open(COPY).read())
    ls = root.find('LiveSet')
    try:
        main, lanes = takes_of(ls, PROJECT)
    except (OSError, StopIteration, AssertionError, AttributeError) as e:
        bad.append(f'звук не записан ({e})')
        return False, bad, facts, seen, files
    if not main and not any(lanes):
        bad.append('звук не записан: на «Rec Audio» нет клипа')
        return False, bad, facts, seen, files
    wav = (main or [t for l in lanes for t in l])[0]
    dst = os.path.join(OUT, run['name'] + '.wav')
    shutil.copy(wav.path, dst)
    files['wav'] = dst
    facts.append(f'WAV {wav.rate} Гц')
    files['rate'] = wav.rate
    files['rec_start'] = min(t.start for t in (main or [t for l in lanes for t in l]))
    rec_end = max(t.end for t in (main or [t for l in lanes for t in l]))
    gone = [(x['sent_beat'], x['acted_beat']) for x in acts
            if x['action'] == 'delete' and x.get('acted_beat') is not None]
    if gone:
        # Live stops recording "Rec Audio" when its source track loses its instrument: the sound ends with MDMI.
        if not gone[0][0] - 0.05 <= rec_end <= gone[0][1] + LATE + 1:
            bad.append(f'запись звука кончилась на {bar_beat(rec_end)}, удаление послано на {bar_beat(gone[0][0])}, '
                       f'ответ на {bar_beat(gone[0][1])}')
        else:
            facts.append(f'запись звука кончилась вместе с MDMI ({bar_beat(rec_end)})')
    elif not run.get('loop') and rec_end < b * BB + 2 * BB - 1:
        bad.append(f'запись оборвалась на {bar_beat(rec_end)} (ждали до {bar_beat(b * BB + 2 * BB)})')
    # What should sound comes from the set as built: Live drops the clip tracks' routing to a deleted device.
    sn = song_notes(ET.fromstring(gzip.open(os.path.join(PROJECT, 'MDMI Test.als')).read()).find('LiveSet'))
    if SOUND_DB is None:
        files['beats'] = beat_table(main, sn, (a - 1) * BB, b * BB + 2 * BB)
        return not bad, bad, facts, seen, files
    if run.get('loop'):
        loop_sound(run, lanes, sn, bad, tool, facts)
    else:
        line_sound(run, main, sn, acts, bad, facts, seen, files)
    return not bad, bad, facts, seen, files


def bar_beat(x):
    return f'{int(x // BB) + 1}.{round(x % BB + 1, 2):g}'


def beat_table(takes, sn, x0, x1):
    """[(beat, rms dB, cover)] for the threshold choice on run а."""
    res = []
    for k in range(int(x0), int(x1)):
        r = rms_at(takes, k, k + 1)
        if r is not None:
            res.append((k, round(r, 1), cover(sn, k, k + 1), cover(sn, k - 1, k + 1)))
    return res


def cover(sn, x0, x1, before=None):
    """Beats of [x0, x1) that some note sounds (notes started before `before` left out)."""
    segs = sorted((max(s, x0), min(e, x1)) for s, e in sn if s < x1 and e > x0 and (before is None or s >= before))
    tot, cur = 0.0, x0
    for s, e in segs:
        s = max(s, cur)
        if e > s:
            tot += e - s
            cur = e
    return round(tot, 3)


def line_sound(run, takes, sn, acts, bad, facts, seen, files):
    a, b = run['bars']
    x0, end = max((a - 1) * BB, files['rec_start']), b * BB
    tail_end = end + 2 * BB
    set_end = max(e for s, e in sn)
    quiet = []                                 # [from, to): MDMI off or gone, silence after one beat
    # an action the device did not answer has already failed the run (judge); there is no beat to measure from
    acts = [x for x in acts if x.get('acted_beat') is not None]

    on_at = []
    for i, x in enumerate(acts):
        t = x['acted_beat']
        if x['action'] == 'off':
            nxt = next((y['acted_beat'] for y in acts[i + 1:] if y['action'] == 'on'), tail_end)
            quiet.append((t + 1, nxt))
        elif x['action'] == 'delete':
            quiet.append((t + LATE + 1, tail_end))
        else:
            on_at.append(t)
    n_sound = n_quiet = n_off = 0
    wrong = []
    # MDMI off or deleted: from a beat after the act to the next on, in beat-long windows from the act, the last
    # one shorter (off-on in run в is about one beat and never on the grid). Past the end of a recording (Live
    # stops recording at a delete) nothing is measured.
    for q0, q1 in quiet:
        w = q0
        while q1 - w >= MIN_WINDOW:
            w1 = min(w + 1, q1)
            r = rms_at(takes, w, w1 - QUIET_TAIL)
            if r is not None:
                n_off += 1
                if r > SILENCE_DB:
                    wrong.append(f'{bar_beat(w)}-{bar_beat(w1)} звук {r:.0f} дБ после выкл')
            w = w1
    for k in range(math.ceil(x0), int(tail_end)):
        r = rms_at(takes, k, k + 1)
        if r is None:
            continue
        rq = rms_at(takes, k, k + 1 - QUIET_TAIL)
        if any(q0 - 1 < k + 1 and k < q1 for q0, q1 in quiet):
            continue                           # MDMI off or gone: judged above
        last_on = max([t for t in on_at if t <= k], default=None)
        c = cover(sn, k, k + 1, before=last_on)
        if c >= 0.1:
            if bar_beat(k) in EXCEPTIONS:
                continue
            n_sound += 1
            if r < SOUND_DB:
                wrong.append(f'{bar_beat(k)} тишина {r:.0f} дБ при ноте')
        elif cover(sn, k - 1, k + 1) == 0:
            if last_on is not None and cover(sn, k, k + 1) > 0:
                continue                       # only notes held through off: an observation, below
            n_quiet += 1
            r = rq
            if r > SILENCE_DB:
                wrong.append(f'{bar_beat(k)} звук {r:.0f} дБ без нот' + (' (хвост после конца набора)'
                                                                     if k >= set_end else ''))
    for t in on_at:
        nxt = next((s for s, e in sn if s >= t and s < tail_end), None)
        stop = min([q0 - 1 for q0, q1 in quiet if q0 - 1 > t] + [tail_end])
        if nxt is not None and nxt + 1 <= stop:
            r = rms_at(takes, nxt, nxt + 1)
            if r is not None and r < SOUND_DB:
                wrong.append(f'после вкл на {bar_beat(t)}: первая нота {bar_beat(nxt)} молчит ({r:.0f} дБ)')
        held = [(s, e) for s, e in sn if s < t < e]
        if held:
            # from just after on to the next new note: only the held notes can sound there
            w1 = min(stop, nxt if nxt is not None else stop, max(e for s, e in held), t + 1.25)
            r = rms_at(takes, t + 0.1, w1) if w1 - t >= 0.25 else None
            seen.append(f'после вкл на {bar_beat(t)} ноты, взятые до выкл, ' +
                        ('не измерить: новая нота сразу' if r is None else
                         ('звучат' if r >= SOUND_DB else 'молчат') + f' ({r:.0f} дБ, '
                         f'{bar_beat(t + 0.1)}-{bar_beat(w1)})'))
    for x in acts:
        if x['action'] == 'delete':
            seen.append(f'удалено на {x["acted"]}')
    facts.append(f'долей со звуком {n_sound}, тишины {n_quiet}' + (f', тишины после выкл {n_off}' if n_off else '')
                 + f', мимо {len(wrong)}')
    if wrong:
        bad.append('звук: ' + '; '.join(wrong[:6]) + (f' (ещё {len(wrong) - 6})' if len(wrong) > 6 else ''))


def loop_pulse(run, notes, out, bad, tool, facts):
    """The pulse of a loop run: a hole in a take is «упал»; wraps or whole takes other than `passes` are the
    tool's (D100)."""
    l0, l1 = (run['loop'][0] - 1) * BB, run['loop'][1] * BB
    full = [k for k in range(l0, l1)]
    by_take = {}
    for n in notes['take_notes']:
        by_take.setdefault(n['take'], []).append(n['time_beats'])
    holes = 0
    whole = 0
    for tk, ts in sorted(by_take.items()):
        got = [k for k in full if any(abs(t - k) < 0.1 for t in ts)]
        if len(got) == len(full):
            whole += 1
        elif tk not in (min(by_take), max(by_take)):
            holes += len(full) - len(got)
    wraps = len(re.findall(r'луп: возврат', out))
    facts.append(f'возвратов лупа {wraps}, дублей пульса {len(by_take)}, целых {whole}')
    if wraps != run['passes']:
        tool.append(f'возвратов лупа {wraps}, ждали {run["passes"]}')
    if holes:
        bad.append(f'пульс в дублях с дырами ({holes} долей), целых проходов {whole}')
    if whole != run['passes']:
        tool.append(f'целых дублей пульса {whole}, ждали {run["passes"]}')


def loop_sound(run, lanes, sn, bad, tool, facts):
    """Every whole take against pass 1, the extra ones too (D100)."""
    l0, l1 = (run['loop'][0] - 1) * BB, run['loop'][1] * BB
    full = [l for l in lanes if any(t.start <= l0 + 1e-6 and t.end >= l1 - 1e-6 for t in l)]
    # one whole take per pass: --passes N returns give the first pass and N - 1 more, the last return only a tail
    if len(full) != run['passes']:
        tool.append(f'звук лупа: целых дублей {len(full)}, ждали {run["passes"]}')
    if not full:
        tool.append('звук лупа: ни одного целого дубля, сравнивать не с чем')
        return None
    if len(full) < 2:
        # pass 1 is still held to the set's notes below: «не в счёт» never comes with no sound checked (D100)
        tool.append(f'звук лупа: целых дублей {len(full)}, меньше двух, проходы не сравнить')
    # Beats without their edges: an onset lands up to ~25 ms off the grid, differently on each pass (run е,
    # kick at 112.3: 8 ms early in pass 1, 15 ms late in pass 2), and 8 ms of a kick moved a beat's RMS by 3 dB.
    passes = [[rms_at(l, k + EDGE, k + 1 - EDGE) for k in range(l0, l1)] for l in full]
    first = passes[0]
    # the first pass is held to the set's notes like a straight run, the others to the first pass
    for k in range(l0, l1):
        r = rms_at(full[0], k, k + 1)
        if r is None:
            bad.append(f'луп: проход 1 без записи на доле {bar_beat(k)}')
        elif cover(sn, k, k + 1) >= 0.1 and bar_beat(k) not in EXCEPTIONS:
            if r < SOUND_DB:
                bad.append(f'луп: проход 1 доля {bar_beat(k)} тишина {r:.0f} дБ при ноте')
        elif cover(sn, k - 1, k + 1) == 0:
            r = rms_at(full[0], k, k + 1 - QUIET_TAIL)
            if r > SILENCE_DB:
                bad.append(f'луп: проход 1 доля {bar_beat(k)} звук {r:.0f} дБ без нот')
    worst = 0.0
    off = []                                   # (pass, beat, text) more than 3 dB from pass 1
    for p, pr in enumerate(passes[1:], 2):
        for k, (r0, r) in enumerate(zip(first, pr)):
            if r0 is None or r is None:
                bad.append(f'луп: проход {1 if r0 is None else p} без записи на доле {bar_beat(l0 + k)}')
                continue
            if r0 <= SILENCE_DB and r <= SILENCE_DB:
                continue
            d = abs(r - r0)
            worst = max(worst, d)
            if d > 3:
                off.append((p, l0 + k, r < r0, f'проход {p} доля {bar_beat(l0 + k)}: {r:.0f} против {r0:.0f} дБ'))
    kb = KICK_BEAT - l0
    facts.append(f'целых проходов {len(full)}, худшая разница с первым {worst:.1f} дБ; доля {bar_beat(KICK_BEAT)} '
                 'по проходам: ' + ' '.join('?' if pr[kb] is None else f'{pr[kb]:.0f}' for pr in passes))
    if not off:
        return None
    bad.append('луп: ' + '; '.join(x[3] for x in off[:6]) + (f' (ещё {len(off) - 6})' if len(off) > 6 else ''))
    return None


# ---------- the plan ----------

def main():
    if sys.argv[1:] == ['--notes']:
        return write_notes()
    os.makedirs(OUT, exist_ok=True)
    only = sys.argv[1:]                        # run names to play, all when none
    names = [r['name'] for r in PLAN]
    if any(n not in names for n in only):
        log(f'нет такого прогона: {" ".join(n for n in only if n not in names)}; в плане: {" ".join(names)}'
            ' (б и в с этапа 59 внутри а)')
        return 2
    full = os.path.join(OUT, 'summary.txt')
    # a part of the plan writes its own file and is compared with the same runs of the last full summary
    summary = os.path.join(OUT, 'summary-part.txt') if only else full
    prev = open(full, encoding='utf-8').read() if os.path.exists(full) else None
    lines = [f'MDMI в Live (ableton-probe), {time.strftime("%Y-%m-%d %H:%M")}, '
             f'установлено: {installed()}' + (f', только {" ".join(only)}' if only else ''), '']
    verdicts = []
    for run in PLAN:
        if only and run['name'] not in only:
            continue
        log(f'прогон {run["name"]}: {run["what"]}, такты {run["bars"][0]}-{run["bars"][1]}')
        subprocess.run([sys.executable, os.path.join(REPO, 'tools', 'make_probe_set.py')], check=True,
                       capture_output=True)
        broken = check_set(os.path.join(PROJECT, 'MDMI Test.als'))
        if broken:
            log('набор испорчен, Live не запускаю: ' + '; '.join(broken))
            return 2
        shutil.rmtree(os.path.join(PROJECT, 'Samples', 'Recorded'), ignore_errors=True)
        if os.path.exists(os.path.join(TOOL_OUT, 'notes.json')):
            os.remove(os.path.join(TOOL_OUT, 'notes.json'))   # a run that writes none must not pass on the last one
        crashes = set(os.listdir(CRASH)) if os.path.isdir(CRASH) else set()
        res = play(run)
        if res is None:
            v = f'{run["name"]} {run["what"]}: НЕ СЫГРАН (Live был занят больше часа)'
            verdicts.append(v); lines += [v, '']
            continue
        code, out = res
        open(os.path.join(OUT, run['name'] + '.log'), 'w', encoding='utf-8').write(out)
        ok, bad, facts, seen, files = judge(run, code, out, crashes)
        tool = files.get('tool') or []
        if not ok or tool:                     # the next run overwrites <name>.wav: keep what failed
            stamp = time.strftime('%Y%m%d-%H%M%S')
            for ext in ('.wav', '.log', '-notes.json'):
                src = os.path.join(OUT, run['name'] + ext)
                if os.path.exists(src):
                    shutil.copy(src, os.path.join(OUT, f'{run["name"]}-{stamp}{ext}'))
            if files.get('wav'):
                files['wav'] = os.path.join(OUT, f'{run["name"]}-{stamp}.wav')
        v = f'{run["name"]} {run["what"]} ({run["bars"][0]}-{run["bars"][1]}): ' + \
            (('ДЕФЕКТ: ' + '; '.join(bad)) if not ok else
             ('НЕ В СЧЁТ: ' + '; '.join(tool)) if tool else 'ПРОШЁЛ')
        verdicts.append(v)
        m = re.search(r'Live открыл набор за (\d+) с', out)
        lines += [v, '   ' + '; '.join(facts)]
        lines += ['   наблюдение: ' + s for s in seen]
        lines += [f'   открыл за {m.group(1) if m else "?"} с; PNG {files.get("png")}; WAV {files.get("wav")}', '']
        log('  ' + v)
        if 'beats' in files:
            json.dump(files['beats'], open(os.path.join(OUT, run['name'] + '-beats.json'), 'w'), indent=0)
            log('  пороги не выставлены: доли в', run['name'] + '-beats.json')
    # D100: «не в счёт» is a run the tool did not carry through, the sound clean on every whole take.
    counts = [sum(1 for v in verdicts if f': {w}' in v) for w in ('ПРОШЁЛ', 'НЕ В СЧЁТ', 'ДЕФЕКТ', 'НЕ СЫГРАН')]
    lines.append(f'Итого: прошёл {counts[0]}, не в счёт {counts[1]}, упал {counts[2]}' +
                 (f', не сыгран {counts[3]}' if counts[3] else ''))
    if prev is not None:
        old = [l for l in prev.split('\n') if re.match(r'^\S+ .*: (ПРОШЁЛ|ДЕФЕКТ|НЕ СЫГРАН|НЕ В СЧЁТ|ИЗВЕСТНЫЙ ДЕФЕКТ)', l)
               and (not only or l.split(' ', 1)[0] in only)]
        same = [outcome(v) for v in old] == [outcome(v) for v in verdicts]
        # the runs themselves (name, what, bars: the text before the first ': ') must match, or there is nothing
        # to compare with (stage 59 changed the plan)
        plan_same = [v.split(': ', 1)[0] for v in old] == [v.split(': ', 1)[0] for v in verdicts]
        lines.append('С прошлым полным прогоном: ' + ('план другой, сравнения нет' if not plan_same else
                     'то же' if same else 'ИНАЧЕ, сравни с ' + ('summary.txt' if only else 'summary-prev.txt')))
        if not only:
            open(os.path.join(OUT, 'summary-prev.txt'), 'w', encoding='utf-8').write(prev)
    open(summary, 'w', encoding='utf-8').write('\n'.join(lines) + '\n')
    log('\n'.join(lines))
    # 0 all passed, 1 a run failed or was not played, 4 only «не в счёт» besides passes (D100).
    if all(outcome(v).endswith(': ПРОШЁЛ') for v in verdicts):
        return 0
    return 1 if counts[2] or counts[3] else 4


def outcome(verdict):
    """A verdict line for the repeat comparison: the old known defect of a summary before 0.49.0 counts as a pass."""
    i = verdict.find(': ' + OLD_KNOWN)
    return verdict[:i] + ': ПРОШЁЛ' if i >= 0 else verdict


def installed():
    try:
        return open(r'D:\ALL\VST\CHIPTUNE\MDMI\BUILD.txt', encoding='utf-8').read().split('\n')[0].strip()
    except OSError:
        return '?'


if __name__ == '__main__':
    sys.exit(main())
