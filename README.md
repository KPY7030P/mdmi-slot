# MDMI Mega Drive

CLAP and VST3 instrument for Windows x64: the [Mega Drive MIDI Interface](https://github.com/rhargreaves/mega-drive-midi-interface)
ROM running inside the [BlastEm](https://www.retrodev.com/repos/blastem/) emulator. MIDI goes straight into the
emulated cartridge's EverDrive USB registers, with no network or virtual MIDI cable.

## How it works

- `third_party/blastem` is built as `libblastem.dll` with `patches/0001-mdmi-everdrive-bridge.patch`.
- `src/embed.c` bundles that DLL and the ROM into the plugin. Each instance unpacks its own copy to
  `%TEMP%\MDMI-Plugin`, because BlastEm keeps its state in globals.
- `src/engine.c` runs the emulator on its own MMCSS thread and feeds a ring buffer. MIDI is applied at
  a fixed distance ahead of the audio, and that distance is reported to the host as latency.
- `src/plugin.c` is the CLAP layer. clap-wrapper builds the VST3 from it.

- `src/patch.c` holds the knobs. It tracks what they show and what the ROM has, and sends the difference as MIDI.
  `src/presets_gm.h` is generated from the ROM's source by `tools/gen_presets.py`, so knobs can show preset values.

## Knobs

Every MIDI channel is an instrument of its own: its knobs always mean that channel, so lanes for several channels
can run at once. There are 360 of them, too many for Live to fill its panel by itself: use Configure and touch a
cell in the window. Choosing a channel in the window only changes the view, no knob or lane.

- Global: Polyphony, Filter (3392 Hz matches the original console, top is off), LFO On, LFO Rate,
  Ch3 Special Mode, DAC Enable
- Every channel: Preset, Volume, Fine Tune, Bend Range, Porta On, Porta Time
- FM channels 1-6 also: Pan, Algorithm, Feedback, AMS, FMS, and for each of 4 operators TL, MUL, DT, RS, AR, DR,
  SR, SL, RR, AM, SSG

Knob ids never change, because hosts store automation by id: a global's id is its number above, a channel's knob is
1000 + channel * 100 + its place in `src/patch.h`, so 1411 is Op1 TL of channel 4. New knobs take the next free
number. A project stores pairs of id and value, so a knob added later starts at its default in an older project.

- Polyphony: Off, or the FM channel 1-6 that gets all six FM voices, five with the DAC on. The other FM channels then
  have no voice and stay silent; their notes take nothing. PSG channels keep theirs. Portamento needs one voice.
- Ch3 Special Mode: notes on MIDI channels 11-13 set the frequencies of channel 3's operators 1-3.
- DAC Enable: MIDI channel 6 plays the ROM's drum samples, chosen by note, with no FM voice of its own.
- PSG presets: the ROM has one envelope under every number except 81, 90 and 100, a square lead.

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
| 84 | Polyphony: 64 and up gives the voices to this channel, below 64 turns it off | 1-6, off from any |
| 74, 1, 80, 78 | LFO On, LFO Rate, Ch3 Special Mode, DAC Enable | any |

Notes, pitch bend and other controllers pass to the ROM. CC 121 is turned into what the standard means by it, centre
bend and pedal up, because the ROM would also reset the channel's program and voices. CC 87 is dropped.

At the same moment, knob changes and controllers come before notes start, so a preset point exactly on a note's
start is meant for that note.

## Building

Needs Docker, Git Bash, and Python with numpy for the tests. All outside code is pinned in `tools/deps.lock`:

```bash
sh tools/fetch_deps.sh
sh tools/build_plugin.sh
sh tools/test/run_tests.sh
sh tools/install.sh
```

`fetch_deps.sh` clones `third_party/` and applies `patches/`. The build runs it too and refuses to start if
`third_party/` differs from `patches/`, so after changing code there, run `tools/save_patches.sh`. `run_tests.sh` exits with the number of failed checks. Plugins land in `build/cmake/out`.

`install.sh` copies them to `D:\ALL\VST\CHIPTUNE\MDMI` or the folder you name, checks the hashes, and writes
`BUILD.txt` there. It fails if Ableton Live has the VST3 locked. Live does not show plugin versions, so `BUILD.txt`
is how to tell what is installed.

The version is `0.N.M+revision`: N is the stage in `docs/ROADMAP.md`, M the delivery, and the revision is
`git describe`, ending in `-dirty` for uncommitted code.

## Licence

BlastEm and MDMI are GPL-3.0, so the plugin binary is GPL-3.0 as well. Anyone you give the binary to must
also be able to get the source.
