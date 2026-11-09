# MDMI Slot

A VST®3 and CLAP instrument for Windows that turns an emulated Sega Mega Drive into a synthesizer. Inside the
plugin, the [BlastEm](https://www.retrodev.com/blastem/) emulator runs the
[Mega Drive MIDI Interface](https://github.com/rhargreaves/mega-drive-midi-interface) (MDMI) ROM, so what you
hear is the console's own YM2612 FM chip and SN76489 PSG, played from your MIDI clips.

This is an **unofficial** project. It is not made or endorsed by the authors of MDMI or BlastEm, nor by Sega.

**Status: preview (0.17.0).** It has been tested on one computer and in one host. Expect rough edges; see [Known issues](#known-issues).

## What you get

- Six FM channels (MIDI channels 1-6) and four PSG channels (7-9 square, 10 noise) in one instrument.
- A window with the ROM's live screen, a page per channel, the FM operators' envelopes as a graph, a PSG
  envelope editor, a bank of 128 FM voices and 128 PSG envelopes.
- Every knob of every channel can be automated, so lanes for several channels run at once.
- Chords on FM channels, played on the voices of silent channels (not with Porta On, Ch3 Special Mode or
  the DAC).
- Instrument files: TFI, VGI and TYI for FM, EEF for PSG. Drop them from Explorer on a channel or a bank place.
- DAC samples on channel 6: drop WAV files on notes, trim, tune and shape them.
- The Retrigger switch decides how a note right after another starts: as the chip does, on a free voice, or
  from silence.
- PSG noise on channel 10: Noise Type (periodic or white) and Noise Rate (L M H, Tone 3, or Kit). On Tone 3
  the noise follows the note and periodic noise plays bass; Kit lays a drum kit over every octave (C kick,
  D snare, F# hat...), made for C1-B1, an octave up the same kit higher. Both take channel 9's tone, so channel 9 is silent.
  Not checked on a real console yet.
- An output per MIDI channel besides Main, so each channel can be recorded or processed on a track of its own.
  A channel's output carries all its notes, also a chord's notes on borrowed voices. Main still plays
  everything. This exists only in the emulator: a real console has one mixed output.

## Requirements

- Windows, 64-bit. Tested on Windows 11.
- A VST3 host. Tested in Ableton Live. The CLAP version is tested in a test host only: the
  version of Live it was made with does not load CLAP plugins.

## Installing

1. Download `MDMI-Slot-0.15.1-win64.zip` from the
   [Releases page](https://github.com/KPY7030P/mdmi-slot/releases) and unpack it.
2. Close your host. Copy `MDMI Slot.vst3` to `C:\Program Files\Common Files\VST3`.
   For a CLAP host, copy `MDMI Slot.clap` to `C:\Program Files\Common Files\CLAP`.
3. In Live: Settings (Preferences in older versions), Plug-Ins, turn on "Use VST3 Plug-In System Folders" and press Rescan.
   The instrument appears under Plug-Ins as **MDMI Slot**.

To update, replace the files while the host is closed: Live locks the VST3 while it runs.

## First steps

1. Put MDMI Slot on a MIDI track and open its window.
2. Play: MIDI channel 1 plays the first FM channel. To play several channels from one instrument, send
   other MIDI tracks to it on MIDI channels 2-10 (in Live: MIDI To the instrument's track, then the plugin
   and the channel). Each channel is an instrument of its own, with its own knobs.
3. In the window, pick a channel on the left. EDITOR shows its sound, BANK the bank, ALL every channel at once.
   Drag a value up or down to change it. Preset, a clip's Pgm, or Program Change choose a bank place.
4. To automate a knob in Live, press Configure on the device and touch the cell in the window. The plugin
   has hundreds of knobs, too many for Live to list by itself.
5. The project saves everything: knob values, the bank, dropped files, samples.
6. To take channels apart in Live: make an audio track per channel, set its Audio From to the plugin's track and
   then **Ch 1-MDMI Slot** ... **Ch 10-MDMI Slot**, and Monitor to **In**. Main keeps playing everything, so mute
   or route away the plugin's own track, or you hear the channels twice. Channel 3 in Ch3 Special Mode
   includes MIDI 11-13, channel 6 the DAC, channel 10 the noise.

[docs/GUIDE.md](docs/GUIDE.md) in the repository describes the pages, knobs and MIDI controllers in detail (in
English; the other notes in `docs/` are in Russian).

## Known issues

- **Closing or removing the plugin while it is still loading** can, in rare cases, leave a thread running and
  crash or hang the host. A crash inside the emulator can also hang the host instead of only silencing the
  plugin. A failed engine leaves its DLL in `%TEMP%`.
- **Unsigned DLL in `%TEMP%`.** Each instance unpacks the emulator, an unsigned DLL, to `%TEMP%\MDMI-Slot`.
  Some antivirus programs scan or block it; the first load can then take several seconds.
- **Audio dropouts with ASIO drivers** have not been checked yet: the dropout handling of 0.13 is tested in a
  test host, not yet with ASIO in Live. Long sessions, many instances, freezing and export are not fully
  checked in Live either.
- **Arrow keys** in the window work only in the VST3 version, not in CLAP hosts.
- **File names that are not in Latin letters** (for example Cyrillic) show as blanks in the window.
- **Windows only.** There are no Mac or Linux builds.
- The sound is BlastEm's emulation. A real console may sound different in places. The noise modes of 0.16 are
  written the same way for both, but have not been checked on a console.

## Building from source

Needs Docker and Git Bash on Windows; nothing else is installed on the system. Python with numpy runs the tests.

```bash
sh tools/build_plugin.sh
sh tools/test/run_tests.sh
```

The plugins land in `build/cmake/out`. `tools/install.sh <folder>` copies them to a folder and checks them.
The source zip of a release holds every dependency, so it builds without network apart from the Docker
images. [docs/GUIDE.md](docs/GUIDE.md) explains how the code works.

## Licence

The plugin's own code is licensed under the GNU General Public License, version 3 or later ([LICENSE](LICENSE)).
The plugin as distributed contains BlastEm (GPL-3.0-or-later) and the MDMI ROM (GPL-3.0), so the binaries are
under GPL-3.0. BlastEm, MDMI v1.34 and clap-wrapper are modified; the changes are in `patches/`, see
[NOTICE](NOTICE).
The licences and copyright notices of all included components are in
[THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt).

## Thanks

- Robert Hargreaves (rhargreaves) for the [Mega Drive MIDI Interface](https://github.com/rhargreaves/mega-drive-midi-interface),
  its PCM samples and its tests.
- Michael Pavone for [BlastEm](https://www.retrodev.com/blastem/), and ulalume for the fork the plugin builds on.
- Stephane Dallongeville for [SGDK](https://github.com/Stephane-D/SGDK).
- kuma4649 for the MDPlayer font and colours in the ROM, and Vitaliy Novichkov (Wohlstand) for the XG FM bank
  from libOPNMIDI and the OPN2 file format notes.
- Frank Wen for the Fluid R3 SoundFont the samples come from.
- The CLAP and clap-wrapper developers, and Steinberg for the VST3 SDK.

## Trademarks

Sega and Mega Drive are trademarks of Sega Corporation. VST is a registered trademark of Steinberg Media
Technologies GmbH. This project is not affiliated with either.
