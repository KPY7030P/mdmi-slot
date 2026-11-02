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

Needs Docker and Git Bash. Clone with `core.autocrlf=false`, or BlastEm's scripts break:

- `third_party/blastem`: `ulalume/blastem` at `patches/BASE_COMMIT`, with `0001` applied
- `third_party/clap`: `free-audio/clap`
- `third_party/clap-wrapper`: `free-audio/clap-wrapper` at `patches/BASE_COMMIT_CLAP_WRAPPER`, with `0002` applied
- `third_party/vst3sdk`: `steinbergmedia/vst3sdk` at `v3.8.0_build_66`, the version clap-wrapper expects
- `third_party/mdmi`: `rhargreaves/mega-drive-midi-interface`, only for regenerating the preset table
- `roms/mdmi-everdrive-v1.34.bin`: from the MDMI v1.34 release

```bash
sh tools/build_plugin.sh
sh tools/test/run_tests.sh
build/cmake/param_test.exe "build/cmake/out/CLAP/MDMI Mega Drive.clap"
```

Plugins land in `build/cmake/out`. `crash-*` tests report `NO SOUND` on purpose: they kill the engine and
check that the host survives and shuts down cleanly (`failed=1`, `clean_shutdown=1`).

## Licence

BlastEm and MDMI are GPL-3.0, so the plugin binary is GPL-3.0 as well. Anyone you give the binary to must
also be able to get the source.
