"""The owner's Live set for the combined check at the end of stages.

    python tools/make_live_check.py [out.als]

Reads ALS_tsts/tst1 Project/tst6.als (the owner's, read only) for its MDMI track, the owner's sound and
DAC sample, and presets/vowels for the CSM snapshots. Writes, by default,
build/live-check/MDMI check Project/MDMI check.als: everything on the arrangement, MIDI tracks on channels
1, 2, 4, 5, 6 into the MDMI track, automation of its knobs, five MDMIs with the vowels embedded.

A stage whose check needs playing or automation adds its part below (section starts, add_clip,
automate, knob ids from src/patch.h) and its lines to GUIDE_RU at the end, then reruns this. The owner's checklist for the stage, docs/owner-check-*.md, names the bars.
"""
import copy, gzip, re, sys
import xml.etree.ElementTree as ET

import os
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
src = os.path.join(REPO, 'ALS_tsts', 'tst1 Project', 'tst6.als')
vowels_dir = os.path.join(REPO, 'presets', 'vowels')
dst = sys.argv[1] if len(sys.argv) > 1 else os.path.join(REPO, 'build', 'live-check', 'MDMI check Project', 'MDMI check.als')
os.makedirs(os.path.dirname(dst), exist_ok=True)
root = ET.fromstring(gzip.open(src).read())
ls = root.find('LiveSet')
tracks = ls.find('Tracks')
all_tracks = list(tracks)
by_id = {t.get('Id'): t for t in all_tracks}
mdmi_t, midi_t = by_id['12'], by_id['13']
returns = [t for t in all_tracks if t.tag == 'ReturnTrack']

next_ptr = int(ls.find('NextPointeeId').get('Value'))
GLOBAL = re.compile(r'.*(Target|Pointee)$|ControllerTargets\.\d+$')

def renumber(el):
    global next_ptr
    for e in el.iter():
        if e.get('Id') is not None and GLOBAL.match(e.tag):
            e.set('Id', str(next_ptr)); next_ptr += 1
        if e.tag == 'LomId' and e.get('Value'):
            e.set('Value', '0')
        if e.get('LomId') is not None:
            e.set('LomId', '0')

def clear(t):
    ev = t.find('DeviceChain/MainSequencer/ClipTimeable/ArrangerAutomation/Events')
    for c in list(ev): ev.remove(c)
    envs = t.find('AutomationEnvelopes/Envelopes')
    for c in list(envs): envs.remove(c)
    for slot in t.findall('DeviceChain/MainSequencer/ClipSlotList/ClipSlot'):
        v = slot.find('ClipSlot/Value')
        for c in list(v): v.remove(c)

def set_name(t, name):
    t.find('Name/EffectiveName').set('Value', name)
    t.find('Name/UserName').set('Value', name)

def route(t, target, upper, lower):
    r = t.find('DeviceChain/MidiOutputRouting')
    r.find('Target').set('Value', target)
    r.find('UpperDisplayString').set('Value', upper)
    r.find('LowerDisplayString').set('Value', lower)

tmpl_clip = copy.deepcopy(midi_t.find('DeviceChain/MainSequencer/ClipTimeable/ArrangerAutomation/Events/MidiClip'))

def add_clip(t, name, start, length, notes, color, program=-1):
    """notes: (pitch, time from clip start, duration); program: the clip's Pgm (0-127), sent at its start."""
    c = copy.deepcopy(tmpl_clip)
    c.find('ProgramChange').set('Value', str(program))
    events = t.find('DeviceChain/MainSequencer/ClipTimeable/ArrangerAutomation/Events')
    c.set('Id', str(len(events))); c.set('Time', str(start))
    c.find('CurrentStart').set('Value', str(start))
    c.find('CurrentEnd').set('Value', str(start + length))
    lp = c.find('Loop')
    for tag, v in (('LoopStart', 0), ('LoopEnd', length), ('OutMarker', length),
                   ('HiddenLoopStart', 0), ('HiddenLoopEnd', length), ('StartRelative', 0)):
        lp.find(tag).set('Value', str(v))
    lp.find('LoopOn').set('Value', 'false')
    c.find('Name').set('Value', name)
    c.find('Color').set('Value', str(color))
    c.find('ScrollerTimePreserver/RightTime').set('Value', str(length))
    c.find('TimeSelection/OtherTime').set('Value', str(length))
    kts = c.find('Notes/KeyTracks')
    for k in list(kts): kts.remove(k)
    nid = 1
    for i, pitch in enumerate(sorted({p for p, _, _ in notes})):
        kt = ET.SubElement(kts, 'KeyTrack', Id=str(i))
        ns = ET.SubElement(kt, 'Notes')
        for p, s, d in sorted(notes, key=lambda n: n[1]):
            if p == pitch:
                ET.SubElement(ns, 'MidiNoteEvent', Time=str(s), Duration=str(d),
                              Velocity='110', OffVelocity='64', NoteId=str(nid))
                nid += 1
        ET.SubElement(kt, 'MidiKey', Value=str(pitch))
    c.find('Notes/NoteIdGenerator/NextId').set('Value', str(nid))
    events.append(c)

def configure_param(t, slot, name, pid, visual):
    p = t.findall('DeviceChain/DeviceChain/Devices/PluginDevice/ParameterList/PluginFloatParameter')[slot]
    p.find('ParameterName').set('Value', name)
    p.find('ParameterId').set('Value', str(pid))
    p.find('VisualIndex').set('Value', str(visual))
    p.find('ParameterValue/Manual').set('Value', '0')
    p.find('LastUserRange/First').set('Value', '0')
    p.find('LastUserRange/Last').set('Value', '1')
    return p.find('ParameterValue/AutomationTarget').get('Id')

def automate(t, pointee, steps):
    """steps: (time, value) switch points; value 0 before the first."""
    envs = t.find('AutomationEnvelopes/Envelopes')
    env = ET.SubElement(envs, 'AutomationEnvelope', Id=str(len(envs)))
    ET.SubElement(ET.SubElement(env, 'EnvelopeTarget'), 'PointeeId', Value=pointee)
    auto = ET.SubElement(env, 'Automation')
    ev = ET.SubElement(auto, 'Events')
    n = 0
    ET.SubElement(ev, 'FloatEvent', Id=str(n), Time='-63072000', Value='0'); n += 1
    prev = 0
    for time, value in steps:
        ET.SubElement(ev, 'FloatEvent', Id=str(n), Time=str(time), Value=str(prev)); n += 1
        ET.SubElement(ev, 'FloatEvent', Id=str(n), Time=str(time), Value=str(value)); n += 1
        prev = value
    tv = ET.SubElement(auto, 'AutomationTransformViewState')
    ET.SubElement(tv, 'IsTransformPending', Value='false')
    ET.SubElement(tv, 'TimeAndValueTransforms')

def set_state(t, data):
    ps = t.find('DeviceChain/DeviceChain/Devices/PluginDevice/PluginDesc/Vst3PluginInfo/Preset/Vst3Preset/ProcessorState')
    h = data.hex().upper()
    ps.text = '\n' + '\n'.join(h[i:i + 80] for i in range(0, len(h), 80)) + '\n'

# --- the main MDMI ---
clear(mdmi_t)
devs = mdmi_t.find('DeviceChain/DeviceChain/Devices')
for d in list(devs):
    if d.tag != 'PluginDevice': devs.remove(d)
for sc in mdmi_t.iter('SideChain'):
    sc.find('OnOff/Manual').set('Value', 'false')
    r = sc.find('RoutedInput/Routable')
    r.find('Target').set('Value', 'AudioIn/None')
    r.find('UpperDisplayString').set('Value', 'No Input')
    r.find('LowerDisplayString').set('Value', '')
vowel_base = copy.deepcopy(mdmi_t)       # before the automation slots are set
set_name(mdmi_t, 'MDMI песня')
dac_ptr = configure_param(mdmi_t, 2, 'DAC Enable', 5, 1)
mono_ptr = configure_param(mdmi_t, 3, 'Mono CH1', 24, 2)

BAR = 4
A0, B0, C0, D0, E0 = 0, 8 * BAR, 12 * BAR, 20 * BAR, 28 * BAR   # section starts
C, E, G = 48, 52, 55
# DAC on/off while notes hold (bars 3, 5, 7), on again for the TV / DAC wave part
# Stage 51: on again for the drums of bars 111-114 (K0 is where they start).
K0 = 110 * BAR
automate(mdmi_t, dac_ptr, [(A0 + 2 * BAR, 1), (A0 + 4 * BAR, 0), (A0 + 6 * BAR, 1), (B0, 0),
                           (D0, 1), (E0, 0), (K0, 1), (K0 + 4 * BAR, 0)])
automate(mdmi_t, mono_ptr, [(C0, 1), (D0, 0)])

new_tracks = [mdmi_t]
lanes = {}
for tid, name, ch, col in (('31', 'К1 FM1', 0, 26), ('32', 'К2 FM2', 1, 17), ('36', 'К3 FM3', 2, 13),
                           ('33', 'К4 FM4', 3, 9),
                           ('34', 'К5 FM5', 4, 3), ('35', 'К6 DAC', 5, 60)):
    t = copy.deepcopy(midi_t); t.set('Id', tid)
    clear(t); renumber(t); set_name(t, name); t.find('Color').set('Value', str(col))
    route(t, f'MidiOut/Track.12/DeviceIn.3.{ch}', 'MDMI песня', f'MDMI Slot | Ch {ch + 1}')
    lanes[ch] = t; new_tracks.append(t)

L = 8 * BAR
add_clip(lanes[0], '1 DAC вкл/выкл: аккорд держится', A0, L, [(C, 0, L), (E, 0, L), (G, 0, L)], 26)
add_clip(lanes[1], '1 нота держится', A0, L, [(36, 0, L)], 17)
add_clip(lanes[2], '1 нота держится', A0, L, [(E + 12, 0, L)], 13)
add_clip(lanes[3], '1 нота держится', A0, L, [(G + 12, 0, L)], 9)
add_clip(lanes[4], '1 нота держится', A0, L, [(C + 24, 0, L)], 3)
add_clip(lanes[0], '2 октава: волна на ALL стоит', B0, 4 * BAR, [(C, 0, 16), (C + 12, 0, 16)], 26)
add_clip(lanes[1], '2 квинта', B0, 4 * BAR, [(C - 12, 0, 16), (G - 12, 0, 16)], 17)
mono = []
for r in range(4):
    o = r * 8
    mono += [(C, o, 4), (E, o + 1, 1), (G, o + 2.5, 1), (C + 12, o + 4, 4), (G, o + 5, 2), (E, o + 6, 0.5)]
add_clip(lanes[0], '4 MONO CH1 включён: внахлёст', C0, 8 * BAR, mono, 26)
arp = [(C + [0, 4, 7, 12, 16, 12, 7, 4][i % 8], i * 0.5, 0.5) for i in range(64)]
add_clip(lanes[0], '3/5 арпеджио: TV на ROM, волна DAC на ALL', D0, 8 * BAR, arp, 26)
dac = [(60 + (i % 2) * 2, i, 0.5) for i in range(32)]
add_clip(lanes[5], '5 DAC играет', D0, 8 * BAR, dac, 60)

# --- five vowel MDMIs, each with its own ch 3 track ---
melody = [(p, i * 2, 2) for i, p in enumerate([36, 40, 43, 48, 52, 55, 60, 67, 72, 84])]
melody += [(p, 20 + i, 1) for i, p in enumerate([36, 38, 40, 43, 45, 43, 40, 38, 36, 36, 31, 36])]
start = E0
tid = 40
for letter, ru in (('A', 'А'), ('O', 'О'), ('U', 'У'), ('I', 'И'), ('E', 'Э')):
    v = copy.deepcopy(vowel_base); v.set('Id', str(tid)); renumber(v)
    set_name(v, f'MDMI гласная {ru}'); v.find('Color').set('Value', '13')
    set_state(v, open(f'{vowels_dir}/CSM vowel {letter}.mdsnap', 'rb').read())
    m = copy.deepcopy(midi_t); m.set('Id', str(tid + 1))
    clear(m); renumber(m); set_name(m, f'К3 → гласная {ru}'); m.find('Color').set('Value', '13')
    route(m, f'MidiOut/Track.{tid}/DeviceIn.3.2', f'MDMI гласная {ru}', 'MDMI Slot | Ch 3')
    add_clip(m, f'6 гласная {ru}: низ → верх, потом низко', start, 8 * BAR, melody, 13)
    new_tracks += [v, m]
    start += 8 * BAR; tid += 2

# --- one MDMI in an Instrument Rack: the vowels on macros and five macro variations ---
# The rack is taken from Live's own lesson set (one chain); its devices give way to an MDMI with vowel A.
# A macro maps to a plugin knob through the knob's KeyMidi (channel 16 = macros, NoteOrController = the
# macro) and its MidiControllerRange (the knob's normalized range the macro sweeps).
LESSON = 'C:/ProgramData/Ableton/Live 12 Suite/Resources/Core Library/Lessons/Sets/Creating Manual Mappings.als'
SEMI_MAX, FINE_MAX = 48, 63                      # src/patch.h: Semi 0..96, Fine 0..126 stored from 0
VOWELS = {  # docs/csm-vowels.md: (Semi, Fine) of operators 1-4
    'А': ((17, 6), (26, 38), (39, 9), (45, -6)), 'О': ((11, 22), (20, 42), (39, 9), (44, 42)),
    'У': ((2, 38), (16, -22), (38, 38), (44, -10)), 'И': ((-1, 22), (37, 27), (42, 25), (45, 40)),
    'Э': ((9, 40), (33, 40), (39, 43), (45, -6))}
# Macros 1-4 the operators' Semi, over the span the five vowels use; 5-8 their Fine, all of it.
semi_span = [(min(v[op][0] for v in VOWELS.values()), max(v[op][0] for v in VOWELS.values())) for op in range(4)]
MACROS = [(f'F{op + 1}' + (' рот' if op == 0 else ' язык' if op == 1 else ''), 12 + 3 * op, 2 * SEMI_MAX,
           semi_span[op][0] + SEMI_MAX, semi_span[op][1] + SEMI_MAX) for op in range(4)]
MACROS += [(f'F{op + 1} точно', 13 + 3 * op, 2 * FINE_MAX, 0, 2 * FINE_MAX) for op in range(4)]

def macro_values(letter):
    """The eight macros' values (0..127) that give the vowel."""
    out = []
    for i, (_, _, top, lo, hi) in enumerate(MACROS):
        semi, fine = VOWELS[letter][i % 4]
        v = semi + SEMI_MAX if i < 4 else fine + FINE_MAX
        out.append(127.0 * (v - lo) / (hi - lo) if hi > lo else 0.0)
    return out

lesson = ET.fromstring(gzip.open(LESSON).read())
rack = copy.deepcopy(next(g for g in lesson.iter('InstrumentGroupDevice')
                          if g.find('UserName').get('Value') == 'Crossover Syn Bass'))
for parent in list(rack.iter()):
    for km in parent.findall('KeyMidi'):
        parent.remove(km)
lpr = rack.find('LastPresetRef/Value')
for c in list(lpr): lpr.remove(c)
rack.find('UserName').set('Value', 'Гласные CSM')
rack.find('Annotation').set('Value', '')
branch = rack.find('Branches/InstrumentBranch')
branch.find('Name/EffectiveName').set('Value', 'MDMI Slot')
bdevs = branch.find('DeviceChain/MidiToAudioDeviceChain/Devices')
for d in list(bdevs): bdevs.remove(d)
plugin = copy.deepcopy(vowel_base.find('DeviceChain/DeviceChain/Devices/PluginDevice'))
bdevs.append(plugin)
a_values = macro_values('А')
plist = plugin.findall('ParameterList/PluginFloatParameter')
for i, (name, pid, top, lo, hi) in enumerate(MACROS):
    p = plist[10 + i]
    p.find('ParameterName').set('Value', f'Ch3 Op{i % 4 + 1} ' + ('Semi' if i < 4 else 'Fine'))
    p.find('ParameterId').set('Value', str(pid))
    p.find('VisualIndex').set('Value', str(10 + i))
    pv = p.find('ParameterValue')
    km = ET.Element('KeyMidi')
    for tag, val in (('PersistentKeyString', ''), ('IsNote', 'false'), ('Channel', '16'), ('NoteOrController', str(i)),
                     ('LowerRangeNote', '-1'), ('UpperRangeNote', '-1'), ('ControllerMapMode', '0')):
        ET.SubElement(km, tag, Value=val)
    pv.insert(1, km)
    pv.find('MidiControllerRange/Min').set('Value', repr(lo / top))
    pv.find('MidiControllerRange/Max').set('Value', repr(hi / top))
    pv.find('Manual').set('Value', repr((lo + (hi - lo) * a_values[i] / 127) / top))
    p.find('LastUserRange/First').set('Value', '0')
    p.find('LastUserRange/Last').set('Value', '1')
rack.find('NumVisibleMacroControls').set('Value', '8')
for i in range(16):
    on = i < len(MACROS)
    rack.find(f'MacroControls.{i}/Manual').set('Value', repr(a_values[i]) if on else '0')
    rack.find(f'MacroDisplayNames.{i}').set('Value', MACROS[i][0] if on else f'Macro {i + 1}')
    rack.find(f'MacroDefaults.{i}').set('Value', repr(a_values[i]) if on else '-1')
snaps = rack.find('MacroVariations/MacroSnapshots')
for c in list(snaps): snaps.remove(c)
for n, letter in enumerate(VOWELS):
    s = ET.SubElement(snaps, 'MacroSnapshot', Id=str(n))
    ET.SubElement(s, 'AutogeneratedNameIndex', Value='0')
    ET.SubElement(s, 'SnapshotName', Value=f'Гласная {letter}')
    vals = macro_values(letter)
    for i in range(16):
        ET.SubElement(s, f'MacroValues.{i}', Value=repr(vals[i]) if i < len(vals) else '-1')
    for i in range(16):
        ET.SubElement(s, f'MacroHasValue.{i}', Value='true' if i < len(vals) else 'false')
rack_t = copy.deepcopy(vowel_base); rack_t.set('Id', str(tid))
rdevs = rack_t.find('DeviceChain/DeviceChain/Devices')
for d in list(rdevs): rdevs.remove(d)
rdevs.append(rack)
renumber(rack_t)
set_name(rack_t, 'MDMI гласные на макросах'); rack_t.find('Color').set('Value', '13')
ps =plugin.find('PluginDesc/Vst3PluginInfo/Preset/Vst3Preset/ProcessorState')
h = open(f'{vowels_dir}/CSM vowel A.mdsnap', 'rb').read().hex().upper()
ps.text = '\n' + '\n'.join(h[i:i + 80] for i in range(0, len(h), 80)) + '\n'

def automate_points(t, pointee, points):
    """points: (time, value); Live draws straight lines between them."""
    envs = t.find('AutomationEnvelopes/Envelopes')
    env = ET.SubElement(envs, 'AutomationEnvelope', Id=str(len(envs)))
    ET.SubElement(ET.SubElement(env, 'EnvelopeTarget'), 'PointeeId', Value=pointee)
    auto = ET.SubElement(env, 'Automation')
    ev = ET.SubElement(auto, 'Events')
    ET.SubElement(ev, 'FloatEvent', Id='0', Time='-63072000', Value=repr(points[0][1]))
    for n, (time, value) in enumerate(points, 1):
        ET.SubElement(ev, 'FloatEvent', Id=str(n), Time=repr(float(time)), Value=repr(value))
    tv = ET.SubElement(auto, 'AutomationTransformViewState')
    ET.SubElement(tv, 'IsTransformPending', Value='false')
    ET.SubElement(tv, 'TimeAndValueTransforms')

# Bars: five vowels a bar and a half each, then a slow glide А → О → У → И → Э → А, two bars a step.
R0 = start
order = list(VOWELS)
glide = order + ['А']
for i in range(len(MACROS)):
    ptr = rack.find(f'MacroControls.{i}/AutomationTarget').get('Id')
    pts = []
    for n, letter in enumerate(order):
        t0 = R0 + n * 6
        pts += [(t0, macro_values(letter)[i]), (t0 + 5.999, macro_values(letter)[i])]
    g0 = R0 + 8 * BAR
    pts += [(g0 + n * 2 * BAR, macro_values(letter)[i]) for n, letter in enumerate(glide)]
    automate_points(rack_t, ptr, pts)
rm = copy.deepcopy(midi_t); rm.set('Id', str(tid + 1))
clear(rm); renumber(rm); set_name(rm, 'К3 → гласные на макросах'); rm.find('Color').set('Value', '13')
route(rm, f'MidiOut/Track.{tid}/DeviceIn.3.2', 'MDMI гласные на макросах', 'MDMI Slot | Ch 3')
add_clip(rm, '7 гласные по очереди (вариации)', R0, 8 * BAR, [(45, 0, 30), (40, 30, 2)], 13)
add_clip(rm, '7 гласные плавно перетекают', R0 + 8 * BAR, 10 * BAR, [(45, 0, 20), (40, 20, 20)], 13)
new_tracks += [rack_t, rm]
start = R0 + 18 * BAR

# --- stages 41, 44, 46: PSG on the main MDMI; Env Speed of channels 8 and 10 automated (ids 31, 33) ---
S0 = start
sp8 = configure_param(mdmi_t, 4, 'Ch8 Env Speed', 31, 3)
sp10 = configure_param(mdmi_t, 5, 'Ch10 Env Speed', 33, 4)
X1 = 64 / 127
# 87-88 x1, 89-90 down to x1/16, 91-92 up to x4, 93-94 back to x1
automate_points(mdmi_t, sp8, [(S0, X1), (S0 + 2 * BAR, X1), (S0 + 4 * BAR, 0.0), (S0 + 6 * BAR, 1.0),
                              (S0 + 8 * BAR, X1)])
# 95-96 x1, 97-100 down to x1/16 and back, 101-102 x1
automate_points(mdmi_t, sp10, [(S0, X1), (S0 + 10 * BAR, X1), (S0 + 12 * BAR, 0.0), (S0 + 14 * BAR, X1)])
psg = {}
for tid, name, ch, col in (('37', 'К7 PSG1', 6, 21), ('38', 'К8 PSG2', 7, 21), ('39', 'К10 шум', 9, 23)):
    t = copy.deepcopy(midi_t); t.set('Id', tid)
    clear(t); renumber(t); set_name(t, name); t.find('Color').set('Value', str(col))
    route(t, f'MidiOut/Track.12/DeviceIn.3.{ch}', 'MDMI песня', f'MDMI Slot | Ch {ch + 1}')
    psg[ch] = t; new_tracks.append(t)
add_clip(psg[7], '8 скорость огибающей К8: x1, медленнее, быстрее', S0, 8 * BAR,
         [(60, 0, 8), (64, 8, 8), (67, 16, 8), (72, 24, 8)], 21)
add_clip(psg[9], '8 скорость огибающей шума', S0 + 8 * BAR, 6 * BAR, [(60, i * 0.5, 0.5) for i in range(48)], 23)
add_clip(psg[6], '9 волна PSG на ROM: A5, C6, D#6', S0 + 14 * BAR, 2 * BAR, [(81, 0, 2), (84, 2, 2), (87, 4, 2), (81, 6, 2)], 21)
start = S0 + 16 * BAR

# --- stages 49, 54: a big chord's voices in ALL's OUT boxes (bars 103-106) ---
# tst6 has other presets on channels 1-4 and 6 than on 5, Ch3 Special off, and the DAC is off here
# (automation from bar 29). On K5 a single note on the beat, then a chord on the third beat: in 103 of 4
# notes (its own voice and 3 free ones), in 105 of 6 notes taking the voices of channels 1-4 and 6. Each
# lent row's OUT box shows its voice's note in green (stage 54); row 5 the whole chord. In 104 and 106
# channels 1-4 and 6 play a short note each, which takes their voices home: their boxes go blue for it.
# The delay measurement of stage 49 is closed (stage 25: not audible).
H0 = start
add_clip(lanes[4], '10 аккорд из 4 на К5', H0, BAR, [(60, 0, 1), (60, 2, 1), (64, 2, 1), (67, 2, 1), (72, 2, 1)], 3)
add_clip(lanes[4], '10 аккорд из 6 на К5', H0 + 2 * BAR, BAR,
         [(60, 0, 1), (48, 2, 1), (55, 2, 1), (60, 2, 1), (64, 2, 1), (67, 2, 1), (72, 2, 1)], 3)
for ch, key in ((0, 48), (1, 36), (2, 64), (3, 67), (5, 43)):
    for bar in (1, 3):
        add_clip(lanes[ch], '10 голос домой', H0 + bar * BAR, BAR, [(key, 0, 0.25)], lanes[ch].find('Color').get('Value'))
start = H0 + 4 * BAR

# --- stage 50: UNDO doesn't jump over a clip's Program Change (bars 107-110) ---
# K1 plays a note a beat. The owner turns the wheel on CH1's Preset in 107-108; the clip of 109 sends
# a Program Change at its start (place 41); in 109-110 the wheel again, then UNDO twice: the first goes
# back to the clip's place, the second to before the wheel of 107.
U0 = start
beats = [(60 + [0, 4, 7, 4][i % 4], i, 0.9) for i in range(8)]
add_clip(lanes[0], '11 UNDO: колесо до Program Change', U0, 2 * BAR, beats, 26)
add_clip(lanes[0], '11 UNDO: клип ставит место 41, колесо после', U0 + 2 * BAR, 2 * BAR, beats, 26, program=40)
start = U0 + 4 * BAR

# --- stages 51, 53: ALL's DAC row, the four voices' squares lit by these drums (bars 111-114) ---
# The ROM's kit on K6 (unless tst6's sample is on these notes): closed hat (41) in eighths, each a
# voice of its own key; a kick (35) on beats 1 and 3 and a snare (37) on 2 and 4, each held 1.5 beats,
# so two and three squares take turns; a crash (49 is HiStudio, so 48 Crsh1) held over bar 113 lights all four.
assert start == K0, (start, K0)
hats = [(41, i * 0.5, 0.4) for i in range(8)]
kit = hats + [(35, 0, 1.5), (37, 1, 1.5), (35, 2, 1.5), (37, 3, 1.5)]
add_clip(lanes[5], '12 DAC: хеты восьмыми поверх бочки и малого', K0, BAR, kit, 60)
add_clip(lanes[5], '12 DAC: хеты восьмыми поверх бочки и малого', K0 + BAR, BAR, kit, 60)
add_clip(lanes[5], '12 DAC: и тарелка, четыре голоса', K0 + 2 * BAR, BAR, kit + [(48, 0, 4)], 60)
add_clip(lanes[5], '12 DAC: снова без тарелки', K0 + 3 * BAR, BAR, kit, 60)
start = K0 + 4 * BAR

for t in all_tracks: tracks.remove(t)
for t in new_tracks + returns: tracks.append(t)
for sc in ls.find('Scenes'):
    sc.find('Name').set('Value', '')
ls.find('NextPointeeId').set('Value', str(next_ptr))

data = ET.tostring(root, encoding='UTF-8', xml_declaration=True)
with gzip.open(dst, 'wb') as f:
    f.write(data)
GUIDE_RU = '''Проект для общей проверки MDMI Slot. Всё на таймлайне (Arrangement, клавиша Tab): нажмите Play и смотрите.
Сделан из tst6: на дорожке «MDMI песня» ваш звук из tst6 с сэмплом DAC. Пункты = пункты docs/owner-check-*.md.

Дорожки:
  MDMI песня            плагин; на ней автоматизация DAC Enable, Mono CH1, Ch8 и Ch10 Env Speed
  К1-К6                 шлют ноты в «MDMI песня» на MIDI-каналы 1-6
  MDMI гласная А…Э      пять плагинов, в каждый уже загружен свой CSM vowel (LOAD не нужен)
  К3 → гласная А…Э      шлют ноты на канал 3 своего плагина

Таймлайн (такты):
  1-8    п.1  аккорд на К1 и ноты на К2, К3, К4, К5 держатся. DAC включается на 3-м такте, выключается на 5-м,
              снова включается на 7-м. Каждый раз ноты должны ударить заново сами, а не замолчать.
  9-12   п.2  октава на К1, квинта на К2, DAC выключен. Окошки с волной на странице ALL не дёргаются.
  13-20  п.4  ноты внахлёст на К1, Mono CH1 включён автоматизацией. Одна нота, новая перебивает,
              после отпускания возвращается прежняя, без скольжения.
  21-28  п.3, п.5  арпеджио на К1 и DAC на К6, DAC включён. Смотрите TV RGB/COMP на странице ROM
              и на странице ALL в строке 6 смотрите живую волну DAC в окошке OUT.
  29-36  п.6  гласная А: ноты снизу вверх, потом низкая мелодия. Внизу слышна «а», вверху сливается.
  37-44       гласная О
  45-52       гласная У
  53-60       гласная И
  61-68       гласная Э
  69-76  п.7  «MDMI гласные на макросах»: один плагин в стойке (Instrument Rack), гласные А, О, У, И, Э
              по очереди, по полтора такта. Их переключает автоматизация макросов.
  77-86       та же стойка: гласные плавно перетекают А → О → У → И → Э → А, по два такта.
  87-94  п.8  К8 PSG держит ноты по два такта. На странице канала 8 горит столбик, который играет.
              Автоматизация Ch8 Env Speed: такты 87-88 обычная скорость, 89-90 огибающая замедляется до x1/16,
              91-92 ускоряется до x4, 93-94 снова x1. Столбик идёт медленнее и быстрее вместе со звуком.
  95-100      К10 шум восьмыми, Ch10 Env Speed: 97-98 замедляется, 99-100 возвращается к x1.
  101-102 п.9 К7 PSG играет A5, C6, D#6: на странице ROM волна канала 7 у каждой ноты своя.
  103-106 этап 54, п.7 волна ноты аккорда в окошках OUT. Откройте страницу ALL. На К5 в 103 и 105 такте:
              на первую долю одна нота, на третью долю аккорд. 103: аккорд из 4 нот, 105: из 6 нот.
              Ноты аккорда звучат на голосах других каналов (зелёные стрелки слева). В строке каждого такого
              голоса в окошке OUT зелёная волна его ноты, в строке 5 синяя волна всего аккорда.
              Наведите мышь на зелёное окошко: внизу «VOICE 1 PLAYS CH5 E-3, HEARD ON CH5 OUT».
              В 104 и 106 каналы 1-4 и 6 коротко играют свою ноту: голос возвращается домой, его окошко на
              эту ноту синее. После отпускания аккорда окошки пустые, хотя хвост ещё звучит: так задумано.
  107-110 п.11 UNDO и Program Change из клипа (этап 50). К1 играет ноту на каждую долю. Включите петлю
              на тактах 107-110 или остановитесь на 110. Покрутите колесо до и после: в 107-108 колесом
              на имени канала 1 (слева) смените звук на несколько мест, в 109 клип сам поставит каналу 1
              место 41, в 109-110 снова покрутите колесо. Остановите Play и нажмите UNDO два раза.
              Первый UNDO вернёт место 41 (то, что поставил клип), второй: звук до колеса в 107.
  111-114 этап 53, п.1 строка DAC на ALL. DAC включён, на К6 закрытый хет восьмыми, бочка на 1 и 3,
              малый на 2 и 4, в 113 такте ещё тарелка. На странице ALL строка 6 слева направо: четыре
              квадратика голосов DAC (горит оранжевым с нотой, пока голос звучит; доиграл: только
              оранжевая нота; свободен: «---»), VOL и PAN, живая волна в окошке OUT, имя сэмпла справа.
              Горят 2-3 квадратика, в 113 такте должны загореться все четыре. Щелчок по окошку OUT:
              картинка последнего сэмпла с линиями голосов, второй щелчок: снова волна.

Стойка «Гласные CSM» (дорожка «MDMI гласные на макросах»):
  F1 рот, F2 язык   две главные крутилки: F1 открывает рот (И, У внизу, А вверху), F2 двигает язык (У внизу,
                    И вверху). Вдвоём они дают любую из пяти гласных и всё, что между ними.
  F3, F4            верхние форманты, слабо влияют.
  F1-F4 точно       подстройка в центах.
  Вариации          кнопки над макросами (Macro Variations): «Гласная А» … «Гласная Э» ставят ровно эту гласную.
Чтобы крутить самому, выключите автоматизацию стойки (кнопка Re-Enable Automation или удалите огибающие).
Если дорожка «К3 → гласные на макросах» молчит, выберите у неё в MIDI To «MDMI гласные на макросах»
и ниже «MDMI Slot | Ch 3», и напишите мне: тогда я поправлю проект.

Проверка TV после сохранения: выберите COMP, сохраните проект под своим именем, закройте и откройте снова.
Шесть плагинов сразу нагружают процессор. Если тяжело, удалите лишние пары «гласная» или отключите их.
В ALS_tsts ничего не записано. Если Live не откроет файл, напишите текст ошибки.
'''
with open(os.path.join(os.path.dirname(dst), 'КАК ПОЛЬЗОВАТЬСЯ.txt'), 'w', encoding='utf-8') as f:
    f.write(GUIDE_RU)
print('ok', len(data), 'end beat', start, 'next pointee', next_ptr)
