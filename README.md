# MDMI Mega Drive

CLAP and VST3 instrument for Windows x64: the [Mega Drive MIDI Interface](https://github.com/rhargreaves/mega-drive-midi-interface)
ROM running inside the [BlastEm](https://www.retrodev.com/repos/blastem/) emulator. MIDI goes straight into the
emulated cartridge's EverDrive USB registers, with no network or virtual MIDI cable.

## How it works

- `third_party/blastem` is built as `libblastem.dll` with `patches/0001-mdmi-everdrive-bridge.patch`.
- The ROM is MDMI v1.34 built from `third_party/mdmi` with `patches/0003-mdmi-psg-envelopes.patch` (D28), in
  the ROM author's SGDK image, into `build/rom/mdmi-everdrive.bin`. The same file is meant for the console.
  Its one addition: SysEx `0A 01 <program> <nibbles>` stores a PSG envelope under a program number.
  Unchanged, the tag builds the released `roms/mdmi-everdrive-v1.34.bin` byte for byte.
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
- DAC Enable: MIDI channel 6 plays the ROM's drum samples, chosen by note, with no FM voice of its own.
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
  frame (60 Hz): level bars on top, pitch below ordered from -5 to +5 semitones, the loop's two tabs
  above. The LEN cell sets the length by a vertical drag, 124 bytes at most with the loop marks,
  and the columns share the lane by it: 3 px at 124 frames, wider when shorter (0.8.1). A drawing is the channel's
  own envelope, as a dropped EEF: the Preset and the bank stay, the next preset step wipes it, SAVE
  writes it as EEF, UNDO puts back the envelope before the last gesture. The window never makes a
  loop the ROM would misread (`patch_shape_to_envelope`). It hands the drawing over at most once per
  window frame and at mouse up, and tells the host nothing (D19). While the longest envelope is
  re-stored every 33 ms, a note on another PSG channel sounds at worst 9.7 ms later than without
  drawing, against 18.3 ms for a note stored with its envelope (`reg_test`, one run).

Toggling the DAC resets the YM2612, because the ROM swaps the Z80 driver and the Z80's reset line also clears
the chip. Sounding FM notes stop, and `src/patch.c` reloads every channel's sound afterwards.

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
