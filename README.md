# MDMI Mega Drive

CLAP and VST3 instrument for Windows x64: the [Mega Drive MIDI Interface](https://github.com/rhargreaves/mega-drive-midi-interface)
ROM running inside the [BlastEm](https://www.retrodev.com/repos/blastem/) emulator. MIDI goes straight into the
emulated cartridge's EverDrive USB registers, with no network or virtual MIDI cable.

## How it works

- `third_party/blastem` is built as `libblastem.dll` with `patches/0001-mdmi-everdrive-bridge.patch`.
- The ROM is MDMI v1.34 built from `third_party/mdmi` with `patches/0003-mdmi-psg-envelopes.patch` (D28), in
  the ROM author's SGDK image, into `build/rom/mdmi-everdrive.bin`. The same file is meant for the console.
  Its additions: SysEx `0A 01 <program> <nibbles>` stores a PSG envelope under a program number, and
  after switching the DAC the ROM writes its PSG tones and volumes again (D35).
  Unchanged, the tag builds the released `roms/mdmi-everdrive-v1.34.bin` byte for byte.
- `tools/gen_rom_map.py` reads where the DAC sample table is from the ROM build's `symbol.txt`, so the
  plugin can patch the image (D36). `src/rom_image.c` patches it, `src/dac.c` turns WAV files into samples.
- `src/embed.c` bundles that DLL and the ROM into the plugin. Each instance unpacks its own copy to
  `%TEMP%\MDMI-Plugin`, because BlastEm keeps its state in globals.
- `src/engine.c` runs the emulator on its own MMCSS thread and feeds a ring buffer. MIDI is applied at
  a fixed distance ahead of the audio, and that distance is reported to the host as latency.
- `src/plugin.c` is the CLAP layer. clap-wrapper builds the VST3 from it.

- `src/patch.c` holds the knobs. It tracks what they show and what the ROM has, and sends the difference as MIDI.
  `src/presets_gm.h` is generated from the ROM's source by `tools/gen_presets.py`, so knobs can show preset values.

## Knobs

Every MIDI channel is an instrument of its own: its knobs always mean that channel, so lanes for several channels
can run at once. There are 359 of them, too many for Live to fill its panel by itself: use Configure and touch a
cell in the window. Choosing a channel in the window only changes the view, no knob or lane.

- Global: Filter (3392 Hz matches the original console, top is off), LFO On, LFO Rate,
  Ch3 Special Mode, DAC Enable
- Every channel: Preset, Volume, Fine Tune, Bend Range, Porta On, Porta Time
- FM channels 1-6 also: Pan, Algorithm, Feedback, AMS, FMS, and for each of 4 operators TL, MUL, DT, RS, AR, DR,
  SR, SL, RR, AM, SSG

Knob ids never change, because hosts store automation by id: a global's id is its place in `src/patch.h` (0 held Polyphony and stays unused), a channel's knob is
1000 + channel * 100 + its place in `src/patch.h`, so 1411 is Op1 TL of channel 4. New knobs take the next free
number. A project stores pairs of id and value, so a knob added later starts at its default in an older project.

- Voices (D20, D21): each FM channel plays on its own voice, and a chord borrows the voices of channels
  that are silent, nothing to set up. A channel always plays on its own voice: when a chord holds it,
  the chord's note there moves to a free voice and starts again, and is cut only if none is free (D22). A cut note that is still held starts again as soon as a voice comes free. Notes beyond the free
  voices don't sound, and the window marks them as CUT. A green bracket in the window joins a chord's
  channel to the rows whose voices it plays. A borrowed voice
  first gets the channel's sound: notes of one moment wait until all their voices have it, a few ms, up to
  about 20 ms for a chord on voices that held other presets. Channels with Porta On, channel 3 in Ch3
  Special Mode and channel 6 with the DAC play only on their own voice and lend none.
- Bank (D23): one bank of 128 places for the plugin, at first the ROM's GM presets. Place N is
  program N: Preset, CC 3 and a clip's Pgm choose from it on any FM channel. The BANK page lists it.
- Files (D25): a TFI, VGI or TYI dropped from Explorer on an FM channel's row gives that channel its
  voice and name, like tweaks: the Preset stays, and the next preset step wipes it. Dropped on a place
  of the BANK page, it replaces that place. LOAD opens an instrument for the selected channel, a
  `.mdbank` for the bank or a `.mdsnap` for everything; SAVE writes the channel as VGI, the bank or a
  snapshot, which is the project's own state. UNDO puts back what the last file replaced, and a second
  UNDO brings the file back. `src/instrument.c` reads the formats, `src/files.c` and `src/state.c`
  the rest.
- A voice that differs from what the ROM holds in more than 12 values goes as one SysEx `0A`, which
  takes as long as 12 controllers (D24).
- Ch3 Special Mode: notes on MIDI channels 11-13 set the frequencies of channel 3's operators 1-3.
- DAC Enable: MIDI channel 6 plays drum samples, chosen by note, with no FM voice of its own.
- DAC pages (stage 12, D36, D37, `docs/design/dac-page.md`): EDITOR of channel 6 while the DAC is on is a
  grid of the 128 notes and the selected note's wave with REPLACE and CLEAR; BANK lists the same notes.
  Each note plays the ROM's sample (notes 26-86, one note below GM, kept as the ROM has
  it), a WAV dropped on it, or nothing. WAV files dropped on a row go to that note and the next ones; a
  file is turned into the driver's format (mono, 8 bits, the lowest of its six rates that keeps the
  file's, at most 32 kHz) and kept in the project, up to the image's free room, about 1.9 MB. CLEAR
  gives a note the ROM's sample back; on a note of the ROM's it removes the sample and RESTORE brings
  it back. A sample dragged to another note swaps with it, Ctrl copies; the ROM's go as copies of their
  bytes. UNDO puts back the samples before the last change. A change goes straight into the running
  core's cartridge and RAM (D39): no restart, other channels play on; a note sounding at that moment may
  play stray bytes to its old length. Only the first boot, or an image that differs outside the samples,
  restarts the core: 0.3-0.6 s without sound, the header says LOADING. TUNE is the page's only cell: off centre, as bend, a note plays
  its neighbour's sample. The rates are those BlastEm plays, 2-3% below what SGDK names them.
- Any sample is prepared on EDITOR (D38, D39; one of the ROM's becomes the project's own, a copy of
  its bytes, at its first change): START and END (the tabs of a strip over the wave, as the PSG loop's), PITCH and
  CT by resampling (length changes with it), GAIN and NORMALIZE, RATE (AUTO or one of six), FADE (the
  last 4 ms) and ONE-SHOT (Note Off doesn't cut it; a flag our ROM reads). Nothing changes while the
  mouse is down; on mouse up the sample is prepared once and goes into the running core. A double click puts a
  value back. The project keeps the file's sound (mono, 16 bits, up to 16 MB in all), the settings and
  the ROM's bytes, so an opened project plays the same bytes and the trim can still go back.
- PSG envelopes (D28): the bank's second list, 128 places, at first the ROM's two envelopes (Square
  Lead on 81, 90 and 100, Plain elsewhere). On a PSG channel Preset, CC 3 and a clip's Pgm choose from
  it, and the BANK page lists it while a channel 7-10 is selected. An EEF works there as a TFI does on
  FM: on a channel's row it becomes that channel's envelope until the next preset step, on a place it
  replaces the place; SAVE writes the channel as EEF. An envelope holds at most 124 steps with its loop
  marks. The ROM picks the envelope at Note On, so a change is heard from the next note. One the ROM
  doesn't hold goes as SysEx `0A 01` under one of the channel's two service numbers 120-127, used in
  turn, so going back to one of the last two is free; the ROM copies the envelope at Note On, so a
  sounding note keeps its own. The longest delays a note of the same moment by 16-18 ms in the
  emulator (`reg_test`). EEF files of Echo shift
  pitch by other steps and load as they are. SysEx `06` and `0A 01` from a clip are dropped.
- Envelope editor (stage 8): the EDITOR page of a channel 7-10 draws its envelope, one column per
  frame (60 Hz): level bars on top, pitch below ordered from -5 to +5 semitones, the loop's strip
  above with its two tabs. A click on the loop's body turns it off; the window keeps a shadow of it
  where it was, and a click on the shadow turns it on there again. An EEF has no switched-off
  loop, so the shadow (`state_loop`) is part of the view and the project's block 7 (D30); it goes
  when the editor sees the channel's envelope under another name. Without a shadow the switch at the strip's start
  loops the whole envelope. LOOP and RELEASE name the loop and the tail after it. The number at the axis' end is the length,
  set by a vertical drag, 124 bytes at most with the loop marks (amber MAX at the limit),
  and the columns share the lane, which reaches the page's right edge at every length: 3 or 4 px at 124 frames, wider when shorter (0.8.1). A drawing is the channel's
  own envelope, as a dropped EEF: the Preset and the bank stay, the next preset step wipes it, SAVE
  writes it as EEF, UNDO puts back the envelope before the last gesture. The window never makes a
  loop the ROM would misread (`patch_shape_to_envelope`). It hands the drawing over at most once per
  window frame and at mouse up, and tells the host nothing (D19). While the longest envelope is
  re-stored every 33 ms, a note on another PSG channel sounds at worst 9.7 ms later than without
  drawing, against 18.3 ms for a note stored with its envelope (`reg_test`, one run).

- Window (stage 9, D29): every page puts the same things in the same places: number, name, source
  (GM, ROM or BANK and the place; FILE over it for a dropped file; drag it to change the Preset), and
  the channel's cells VOL, PAN (L C R), TUNE, BEND, PORTA, TIME in one row, PSG keeping PAN's
  place empty. A rail row shows the name, a `*` when the bank place's sound was tweaked, and a bar of
  16 segments: the channel's volume (CC7), and green while a note is held, CC7 x velocity / 127,
  grey after. The ROM itself has no velocity curve; the bar shows what the plugin sent. In Ch3
  Special Mode channel 3's row is light FM and shows four notes in a column at the right, its own
  on top; the bar keeps its length. The bank page's scroll track marks every
  channel's place.
- A `*` means the sound is tweaked against its base (D31): what the channel last got whole, its
  bank place or a file dropped on it. The base goes with the model and the project (block 8), undo
  gives it back, a drawing keeps it. An FM cell whose value differs from the base has an amber
  label and a mark where the base's value is; held, the footer says `TL 49, GM 39`. A channel's
  cell is a bar filled up to its value (TUNE from its middle), so the mark reads against the fill.
- A red dot at a cell's top right: the host sent that knob a value since the mouse last edited it
  (D32, D33, D34). Live sends values only while a lane moves; it echoes the window's own edit
  15-32 ms after the gesture ends, and a value within 200 ms of the window's last event on that
  knob is taken as that echo, not as a lane (D34). So
  the dot stays over the lane's flat stretches; a touch with the mouse takes it away.
- ALL (stage 10): every channel on the rail's rows, so drops and clicks work as on EDITOR. A row
  has its algorithm or envelope in small (a click opens the channel's page), VOL, PAN, the source,
  and TUNE, BEND, PORTA only when not at their default. On a PSG row a green line shows where the
  ROM is in the envelope, reckoned by the plugin from the Note On and Note Off it sent and the
  shape (`patch_shape_position`, tested against a byte walk of the ROM's code), for show only.
  The envelope editor draws the same line down its lanes.
  A click on an FM row's diagram puts its four envelopes in its place, a second click brings the
  diagram back; each row keeps its own choice, saved with the view (the view block's third byte).
  ALL draws no hover frames. EDITOR opens the selected channel.
- FM page (stage 11, `docs/design/ui-variants-v8-fm.html`): the channel's cells in one row on every
  page; a graph of the four operators' envelopes at the left, AMS, FMS, the diagram with FB and the
  algorithms 0-7 at the right; under them a column per operator: TL, MUL, AR, DR, SL, SR, RR as
  bars, DT in 7 segments (-3 to +3; the ROM's 4 is -0 and shows as 0), RS in 4 and AM. The graph's
  bright curve and a white frame on the header follow the column under the mouse, else the operator
  last edited; a value from the host moves neither. The graph is the chip's envelope generator stepped for a note C4
  (`patch_fm_envelope`, hardware-specialist's facts from BlastEm): rates double every 2 steps,
  KS by RS, SL 15 is silence, SL 0 skips DR, TL adds to the envelope and the sum stops at 96 dB (so
  a slow attack under a high TL is silent at first, as on the chip), SSG works between 0 and 48 dB four times as fast,
  columns too short for a fast SSG show its range. Time runs on a square-root axis to 2 s, Note
  Off at 1 s, where the active curve has its number. Each operator's SSG is a button at its
  header's right end, its shape or a dim SSG: a click opens OFF and the 8 shapes under the header,
  a click on one sets it as one gesture, any other click closes the choice. In Ch3 Special Mode
  channel 3's number on the page is light FM, as in the rail.

Toggling the DAC resets the YM2612 and the PSG, because the ROM swaps the Z80 driver: SGDK resets both chips,
and the Z80's reset line also clears the YM2612. Sounding notes stop, `src/patch.c` reloads every FM
channel's sound afterwards, and our ROM writes its PSG tones and volumes again (D35).

### From a MIDI track

A MIDI track sending to the plugin's channel changes that channel's knobs with these controllers, for example from a
clip envelope or Live's CC Control. Values are 0-127 and scaled to the knob's range. The window shows the new value.

| CC | Knob | Channels |
|---|---|---|
| Program Change | Preset, loads the preset's values and wipes tweaks | 1-10 |
| 3 | Preset, but the same preset again keeps tweaks | 1-10 |
| 7 | Volume | 1-10 |
| 10, 77 | Pan | 1-6 |
| 85 | Fine Tune | 1-10 |
| RPN 0,0 (101, 100, then 6) | Bend Range | 1-10 |
| 65 | Porta On | 1-10 |
| 5 | Porta Time | 1-10 |
| 14, 15, 76, 75 | Algorithm, Feedback, AMS, FMS | 1-6 |
| 16-19 | TL of operators 1-4 | 1-6 |
| 20-23, 24-27 | MUL, DT | 1-6 |
| 39-42, 43-46, 47-50 | RS, AR, DR | 1-6 |
| 51-54, 55-58, 59-62 | SR, SL, RR. SR above 15 only by knob, CC reaches 0-15 | 1-6 |
| 70-73, 90-93 | AM, SSG | 1-6 |
| 74, 1, 80, 78 | LFO On, LFO Rate, Ch3 Special Mode, DAC Enable | any |

Notes, pitch bend and other controllers pass to the ROM. CC 121 is turned into what the standard means by it, centre
bend and pedal up, because the ROM would also reset the channel's program and voices. CC 87 and CC 84 are dropped: one would make a program number mean another sound for good, the other lets the ROM give any note any voice, past the plugin.

At the same moment, knob changes and controllers come before notes start, so a preset point exactly on a note's
start is meant for that note.

The plugin tells the host only about edits in its window and a loaded project (D19). Knobs that a preset or MIDI
moved are shown in the window but not reported, because Live turns automation off for any value a plugin reports.
So after a preset step, a TL lane takes over again at its next point: put that point just after the step.

## Building

Needs Docker, Git Bash, and Python with numpy for the tests. All outside code is pinned in `tools/deps.lock`:

```bash
sh tools/fetch_deps.sh
sh tools/build_plugin.sh
sh tools/test/run_tests.sh
sh tools/install.sh
```

`run_tests.sh` also runs the ROM's own tests in Docker with `--privileged`: the image's address
sanitizer needs address randomization off. `fetch_deps.sh` clones `third_party/` and applies `patches/`. The build runs it too and refuses to start if
`third_party/` differs from `patches/`, so after changing code there, run `tools/save_patches.sh`. `run_tests.sh` exits with the number of failed checks. Plugins land in `build/cmake/out`.

`install.sh` copies them to `D:\ALL\VST\CHIPTUNE\MDMI` or the folder you name, checks the hashes, and writes
`BUILD.txt` there. It fails if Ableton Live has the VST3 locked. Live does not show plugin versions, so `BUILD.txt`
is how to tell what is installed.

The version is `0.N.M+revision`: N is the stage in `docs/ROADMAP.md`, M the delivery, and the revision is
`git describe`, ending in `-dirty` for uncommitted code.

## Licence

BlastEm and MDMI are GPL-3.0, so the plugin binary is GPL-3.0 as well. Anyone you give the binary to must
also be able to get the source.
