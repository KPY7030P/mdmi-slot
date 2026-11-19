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

def add_clip(t, name, start, length, notes, color):
    """notes: (pitch, time from clip start, duration)."""
    c = copy.deepcopy(tmpl_clip)
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
automate(mdmi_t, dac_ptr, [(A0 + 2 * BAR, 1), (A0 + 4 * BAR, 0), (A0 + 6 * BAR, 1), (B0, 0),
                           (D0, 1), (E0, 0)])
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
  MDMI песня            плагин; на ней автоматизация DAC Enable и Mono CH1 (видно в списке огибающих дорожки)
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
              и щёлкните по картинке DAC на странице ALL: живая волна DAC.
  29-36  п.6  гласная А: ноты снизу вверх, потом низкая мелодия. Внизу слышна «а», вверху сливается.
  37-44       гласная О
  45-52       гласная У
  53-60       гласная И
  61-68       гласная Э

Проверка TV после сохранения: выберите COMP, сохраните проект под своим именем, закройте и откройте снова.
Шесть плагинов сразу нагружают процессор. Если тяжело, удалите лишние пары «гласная» или отключите их.
В ALS_tsts ничего не записано. Если Live не откроет файл, напишите текст ошибки.
'''
with open(os.path.join(os.path.dirname(dst), 'КАК ПОЛЬЗОВАТЬСЯ.txt'), 'w', encoding='utf-8') as f:
    f.write(GUIDE_RU)
print('ok', len(data), 'end beat', start, 'next pointee', next_ptr)
