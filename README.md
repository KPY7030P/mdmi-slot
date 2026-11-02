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

`Edit Channel` picks which MIDI channel (1-10) the channel knobs show and change. Each channel remembers its own
values, and the project saves them all. With `Polyphony` on, FM voices all belong to channel 1, so FM editing goes there.

- Global: Edit Channel, Polyphony, Filter (3392 Hz matches the original console, top is off), LFO On, LFO Rate,
  Ch3 Special Mode, DAC Enable
- Channel: Preset, Volume, Pan, Fine Tune, Pitch Bend Range, Portamento On, Portamento Time
- FM channel: Algorithm, Feedback, AMS, FMS, and for each of 4 operators TL, MUL, DT, RS, AR, DR, SR, SL, RR, AM, SSG

MDMI sets the limits: Pan has three positions and does nothing on PSG. SR is 0-31 in presets, but moving its
knob only reaches 0-15 (18 of the 128 presets use more). Portamento only works without Polyphony.

- Ch3 Special Mode: notes on MIDI channels 11-13 set the frequencies of channel 3's operators 1-3.
- DAC Enable: MIDI channel 6 plays the ROM's drum samples, chosen by note, with no FM voice of its own.

Toggling the DAC resets the YM2612, because the ROM swaps the Z80 driver and the Z80's reset line also clears
the chip. Sounding FM notes stop, and `src/patch.c` reloads every channel's sound afterwards.

Knob ids never change, because hosts store automation by id. New knobs take the next id, and
`patch_param_id_at()` places them on the panel. Saved state has a version, and older projects still load.

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
