"""MDMI's test set for ableton-probe (D:/ALL/MUSIC/TECH/MYPLUGINS/ableton-probe, the owner's tool), D96.

    python tools/make_probe_set.py

Builds the owner's check set again with tools/make_live_check.py, but into
build/probe/MDMI Test Project/MDMI Test.als (the owner's set in build/live-check is not touched), and adds,
before the return tracks:
- "Pulse": a MIDI track with one clip, a short note on every beat of bars 1-120, output nowhere;
- "Rec Out": the tool's record track, input = Pulse (Post FX). A hole in the recorded pulse means Live stood;
- "Rec Audio": an audio track, input = "MDMI песня" (Post FX), monitoring off, armed. The tool arms only its
  record track and leaves other arms alone, so this track records MDMI's sound in the same take.

tools/live_probe_check.py calls this before every run; probe.py then plays a copy of the set.
"""
import copy, gzip, os, re, subprocess, sys
import xml.etree.ElementTree as ET

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DST = os.path.join(REPO, 'build', 'probe', 'MDMI Test Project', 'MDMI Test.als')
# Live's own lesson set: the only audio track template at hand in Live 12.3's format.
LESSON = 'C:/ProgramData/Ableton/Live 12 Suite/Resources/Core Library/Lessons/Sets/Recording Audio.als'
PULSE, REC, AUDIO = 'Pulse', 'Rec Out', 'Rec Audio'
SONG = '12'          # track id of "MDMI песня"
TEMPLATE = '31'      # a clip track (channel 1) to copy the MIDI tracks from
PULSE_BEATS = 120 * 4
TAP = 'PostFxOut'    # Live's input target for "From: <track>, Post FX"
GLOBAL = re.compile(r'.*(Target|Pointee)$|ControllerTargets\.\d+$')


def main():
    subprocess.run([sys.executable, os.path.join(REPO, 'tools', 'make_live_check.py'), DST], check=True)
    root = ET.fromstring(gzip.open(DST).read())
    ls = root.find('LiveSet')
    tracks = ls.find('Tracks')
    by_id = {t.get('Id'): t for t in tracks}
    nid = ls.find('NextPointeeId')
    state = {'next': int(nid.get('Value')), 'track': max(int(t.get('Id')) for t in tracks if t.get('Id')) + 1}

    def fresh(t, name, color):
        """Give a copied track its own ids and name; empty its clips and automation."""
        t.set('Id', str(state['track'])); state['track'] += 1
        for e in t.iter():
            if e.get('Id') is not None and GLOBAL.match(e.tag):
                e.set('Id', str(state['next'])); state['next'] += 1
            if e.tag == 'LomId' and e.get('Value'):
                e.set('Value', '0')
        for n in ('EffectiveName', 'UserName'):
            t.find('Name/' + n).set('Value', name)
        t.find('Color').set('Value', str(color))
        for p in ('DeviceChain/MainSequencer/ClipTimeable/ArrangerAutomation/Events', 'AutomationEnvelopes/Envelopes'):
            e = t.find(p)
            if e is not None:
                for c in list(e): e.remove(c)
        for slot in t.findall('DeviceChain/MainSequencer/ClipSlotList/ClipSlot'):
            v = slot.find('ClipSlot/Value')
            for c in list(v): v.remove(c)
        return t

    def route(t, kind, target, upper, lower):
        r = t.find(f'DeviceChain/{kind}')
        r.find('Target').set('Value', target)
        r.find('UpperDisplayString').set('Value', upper)
        r.find('LowerDisplayString').set('Value', lower)

    src = by_id[TEMPLATE]
    pulse_clip = copy.deepcopy(src.find('DeviceChain/MainSequencer/ClipTimeable/ArrangerAutomation/Events')[0])
    pulse = fresh(copy.deepcopy(src), PULSE, 0)
    route(pulse, 'MidiOutputRouting', 'MidiOut/None', 'None', '')
    c = pulse_clip
    for k, v in (('CurrentStart', 0), ('CurrentEnd', PULSE_BEATS), ('Loop/LoopStart', 0), ('Loop/LoopEnd', PULSE_BEATS),
                 ('Loop/StartRelative', 0), ('Loop/OutMarker', PULSE_BEATS), ('Loop/HiddenLoopStart', 0),
                 ('Loop/HiddenLoopEnd', PULSE_BEATS)):
        c.find(k).set('Value', str(v))
    c.find('Loop/LoopOn').set('Value', 'false')
    c.find('Name').set('Value', 'pulse: one note a beat')
    for e in (c.find('Envelopes/Envelopes'), c.find('Notes/KeyTracks')):
        for x in list(e): e.remove(x)
    kt = ET.SubElement(c.find('Notes/KeyTracks'), 'KeyTrack', Id='0')
    notes = ET.SubElement(kt, 'Notes')
    for i in range(PULSE_BEATS):
        ET.SubElement(notes, 'MidiNoteEvent', Time=str(i), Duration='0.25', Velocity='100', OffVelocity='64',
                      NoteId=str(i + 1))
    ET.SubElement(kt, 'MidiKey', Value='60')
    c.find('Notes/NoteIdGenerator/NextId').set('Value', str(PULSE_BEATS + 1))
    pulse.find('DeviceChain/MainSequencer/ClipTimeable/ArrangerAutomation/Events').append(c)

    rec = fresh(copy.deepcopy(src), REC, 0)
    route(rec, 'MidiInputRouting', f'MidiIn/Track.{pulse.get("Id")}/{TAP}', PULSE, 'Post FX')
    route(rec, 'MidiOutputRouting', 'MidiOut/None', 'None', '')

    lesson = ET.fromstring(gzip.open(LESSON).read())
    audio = copy.deepcopy(lesson.find('LiveSet/Tracks/AudioTrack'))
    # Sends and clip slots must match this set's return tracks and scenes: take them from our own track.
    # Both sequencers hold clip slots; a count unlike the scenes' makes Live call the set corrupt ("Slot count mismatch").
    for p in ('DeviceChain/Mixer/Sends', 'DeviceChain/MainSequencer/ClipSlotList',
              'DeviceChain/FreezeSequencer/ClipSlotList'):
        parent = audio.find(p.rsplit('/', 1)[0])
        old = audio.find(p)
        parent.insert(list(parent).index(old), copy.deepcopy(src.find(p)))
        parent.remove(old)
    fresh(audio, AUDIO, 0)
    song = by_id[SONG].find('Name/EffectiveName').get('Value')
    route(audio, 'AudioInputRouting', f'AudioIn/Track.{SONG}/{TAP}', song, 'Post FX')
    route(audio, 'AudioOutputRouting', 'AudioOut/Main', 'Master', '')
    audio.find('DeviceChain/MainSequencer/MonitoringEnum').set('Value', '2')   # Off
    audio.find('DeviceChain/MainSequencer/Recorder/IsArmed').set('Value', 'true')
    nid.set('Value', str(state['next']))

    # Return tracks must stay last: Live calls a set with a track after them corrupt ("more send knobs than return tracks").
    kids = list(tracks)
    first_return = next((i for i, t in enumerate(kids) if t.tag == 'ReturnTrack'), len(kids))
    for i, t in enumerate((pulse, rec, audio)):
        tracks.insert(first_return + i, t)
    with gzip.open(DST, 'wb') as f:
        f.write(ET.tostring(root, encoding='UTF-8', xml_declaration=True))
    print('probe set:', DST, '|', PULSE, '->', REC, '|', song, '->', AUDIO)


if __name__ == '__main__':
    main()
