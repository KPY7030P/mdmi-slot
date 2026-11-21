"""MDMI's test set for ableton-probe (D:/ALL/MUSIC/TECH/MYPLUGINS/ableton-probe, the owner's tool).

    python tools/make_probe_set.py [--from TRACK]

Builds the owner's check set again with tools/make_live_check.py, but into
build/probe/MDMI Test Project/MDMI Test.als (the owner's set in build/live-check is not touched), and adds the
record track "Rec Out": a MIDI track whose input is the MIDI out of one clip track (default: the channel 1
track, id 31). probe.py records what it plays, so a run shows the set opens, plays through without a crash,
and what the windows look like. The sound is checked with our own hosts, not here.

Run (Live must be closed; if it is open, the owner is using it):
    python ../../../MYPLUGINS/ableton-probe/probe.py run --config ableton-probe.json --bars 115-118
"""
import argparse, copy, gzip, os, re, subprocess, sys
import xml.etree.ElementTree as ET

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DST = os.path.join(REPO, 'build', 'probe', 'MDMI Test Project', 'MDMI Test.als')
REC = 'Rec Out'
# Live's input target for "MIDI From: <track>, Post FX"; check it in the window if a run records nothing.
TAP = 'PostFxOut'

ap = argparse.ArgumentParser()
ap.add_argument('--from', dest='src', default='31', help='Id of the clip track to record (31 = channel 1)')
args = ap.parse_args()

subprocess.run([sys.executable, os.path.join(REPO, 'tools', 'make_live_check.py'), DST], check=True)

root = ET.fromstring(gzip.open(DST).read())
ls = root.find('LiveSet')
tracks = ls.find('Tracks')
by_id = {t.get('Id'): t for t in tracks}
src = by_id[args.src]
nid = ls.find('NextPointeeId')
nxt = int(nid.get('Value'))
GLOBAL = re.compile(r'.*(Target|Pointee)$|ControllerTargets\.\d+$')

rec = copy.deepcopy(src)
rec.set('Id', str(max(int(t.get('Id')) for t in tracks if t.get('Id')) + 1))
for e in rec.iter():
    if e.get('Id') is not None and GLOBAL.match(e.tag):
        e.set('Id', str(nxt)); nxt += 1
    if e.tag == 'LomId' and e.get('Value'):
        e.set('Value', '0')
nid.set('Value', str(nxt))
for n in ('EffectiveName', 'UserName'):
    rec.find('Name/' + n).set('Value', REC)
rec.find('Color').set('Value', '0')
ev = rec.find('DeviceChain/MainSequencer/ClipTimeable/ArrangerAutomation/Events')
for c in list(ev): ev.remove(c)
envs = rec.find('AutomationEnvelopes/Envelopes')
for c in list(envs): envs.remove(c)
for slot in rec.findall('DeviceChain/MainSequencer/ClipSlotList/ClipSlot'):
    v = slot.find('ClipSlot/Value')
    for c in list(v): v.remove(c)
src_name = src.find('Name/EffectiveName').get('Value')
r = rec.find('DeviceChain/MidiInputRouting')
r.find('Target').set('Value', f'MidiIn/Track.{args.src}/{TAP}')
r.find('UpperDisplayString').set('Value', src_name)
r.find('LowerDisplayString').set('Value', 'Post FX')
o = rec.find('DeviceChain/MidiOutputRouting')
o.find('Target').set('Value', 'MidiOut/None')
o.find('UpperDisplayString').set('Value', 'None')
o.find('LowerDisplayString').set('Value', '')
# Return tracks must stay last: Live calls a set with a track after them corrupt ("more send knobs than return tracks").
kids = list(tracks)
first_return = next((i for i, t in enumerate(kids) if t.tag == 'ReturnTrack'), len(kids))
tracks.insert(first_return, rec)

with gzip.open(DST, 'wb') as f:
    f.write(ET.tostring(root, encoding='UTF-8', xml_declaration=True))
print('probe set:', DST, '| record track', REC, '<-', args.src)
