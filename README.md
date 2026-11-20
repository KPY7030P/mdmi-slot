# MDMI Slot

A VST®3 and CLAP instrument for Windows that turns an emulated Sega Mega Drive into a synthesizer. Inside the
plugin, the [BlastEm](https://www.retrodev.com/blastem/) emulator runs the
[Mega Drive MIDI Interface](https://github.com/rhargreaves/mega-drive-midi-interface) (MDMI) ROM, so what you
hear is the console's own YM2612 FM chip and SN76489 PSG, played from your MIDI clips.

This is an **unofficial** project. It is not made or endorsed by the authors of MDMI or BlastEm, nor by Sega.

**Status: preview (0.41.1).** It has been tested on one computer and in one host. Expect rough edges; see [Known issues](#known-issues).

## What you get

- Six FM channels (MIDI channels 1-6) and four PSG channels (7-9 square, 10 noise) in one instrument.
- A window with the ROM's live screen, a page per channel, the FM operators' envelopes as a graph, a PSG
  envelope editor, a bank of 128 FM voices and 128 PSG envelopes.
- Every knob of every channel can be automated, so lanes for several channels run at once.
- Chords on FM channels, played on the voices of silent channels (not with Porta On, Ch3 Special Mode or
  the DAC).
- Instrument files: TFI, VGI and TYI for FM, EEF for PSG. Drop them from Explorer on a channel or a bank place.
- A ROM for the SD card (0.24): EXPORT on the ROM page writes the ROM with your ROM bank in it (the 128 FM
  voices with their rows, the 128 PSG envelopes with theirs), the project's DAC samples and the names of the
  places, for an EverDrive on a real console. There a Program Change plays a bank place, as in the plugin.
  The file holds the bank, not the channels: what a channel plays that isn't a bank place as it is (a
  dropped file, tweaks) isn't in it, and the footer names those channels; put them in the bank with STORE
  first. Knobs that aren't a sound (LFO, Ch3 Special and its notes, noise, DAC on, pan, volume) aren't in it
  either: the console starts as MDMI does, every channel on program 1. Master Volume and Retrigger are the
  plugin's own. The file is for an NTSC console (60 Hz); on PAL, envelopes and rows run 1.2 times slower. The
  ROM's screen, in the plugin and on the console, names each channel's sound where it showed controller
  numbers, writes DAC on FM6 while the DAC plays, keeps only warnings in its journal, and shows BANK and a
  signature in its top line: the same signature in the plugin and on the console means the card holds this
  bank and these samples. Not checked on a real console yet.
- CSM vowels (0.35.0): five snapshots in `presets/vowels` beside the plugin, the vowels А, О, У, И, Э as
  channel 3 in CSM: play MIDI 3, the note is the voice's pitch, the formants stay. A snapshot replaces the whole
  state, so open one in an empty instance. How to play and move the formants: [docs/csm-vowels.md](docs/csm-vowels.md) (Russian).
- The DAC's row on ALL (0.34.0): a click on its picture shows channel 6's live output, the last 20 ms at the
  same scale as the other rows' OUT boxes; a second click brings the last sample with its voices back.
- MONO on FM channels 1-6 (0.33.0): with MONO a note played over a held one sounds on the same voice and starts
  its own attack, with no glide and no second voice; letting go of the top key strikes the one below again.
  MONO and PORTA are a pair in the channel's row, one click picks either; PORTA wins if both are on (from a
  lane or a clip). The knobs are Mono CH1-CH6; CC 105 on the channel's MIDI track moves them too.
- The ROM page as on a TV (0.31.0): the switch `TV: OFF | RGB | COMP` over the picture's right edge draws the
  ROM's screen as the console looks on a CRT by SCART (scanlines, the TV's gamma) or on a cheap composite TV
  (blur, colour bleed, rainbow on thin grey lines and text). Only the window: the ROM, the sound and the console
  don't change. The project keeps the choice. How strong the rainbow is on a real TV is not checked yet.
- The ROM's status screen (0.24.6-0.24.8), in the plugin's ROM page and on the console. Each channel has a row:
  - left: its number, the MIDI channel that holds it, and a stripe joining the rows of one chord;
  - its sound's name, and under it a ribbon of hits scrolling left over about two seconds (a bar per Note On,
    a dim line while held). Since 0.27.0 the bars take the colour of the MIDI channel holding the row, as its
    number does; a voice lent to a chord draws in the chord channel's colour;
  - FM: a level bar, an estimate from velocity, CC 7 and the carrier's envelope. PSG 1-3: the square wave the
    chip plays, wider teeth for lower notes, taller for louder. Noise: a texture, brighter when louder.
    Channel 3 in Ch3 Special: four squares, one per operator, filled by its envelope;
  - the note: white on the hit, then the chip's colour, dim after the release. It stays until the next note;
  - right, FM: the carrier's envelope as a curve with a white dot running along it from the Note On. The dot
    is an estimate from AR, D1R, SL, D2R, RR and the time since Note On or Off: the ROM can't read the
    chip's envelope. PSG: the envelope as 12 bars, the playing step white;
  - channel 6 with the DAC: its four voices with their notes, dim once a sample has played out (also an
    estimate).
  Along the bottom: the journal's last warning, the bank signature, the link load and CPU (how long the
  screen's frame takes), and the ROM's version. CC 83 shows an FM channel's parameters instead.
- Window after 0.24 (0.24.1-0.24.3): over a channel row with cut chord notes the footer says why no voice
  was free (for example `1 CUT: PORTA 1 4, SP 3, DAC 6`). The level bar is the same length on every row;
  the note is at the right, `x3`, `1 CUT` and `CH4` under it. Channel 6 with the DAC lists the samples
  that sound, up to four, as the ROM plays them; channel 3 with CH3 SPECIAL lists its operators' notes.
  Channel 3 in Ch3 Special is pink everywhere. The source says `ROM N` on FM and PSG: grey while the
  place holds the factory sound, white once you replaced it. The PSG arp row reaches the right edge. A
  double click sets a default where that makes sense (a knob to its preset's value, an envelope column
  to 0 dB or no pitch shift, an arp step to 0, VOL on ALL), in one UNDO step with the click before it;
  on switches (ARP ON, a loop's strip, release or stop) the second click does nothing more.
- UNDO (0.41.0): a history of 32 steps; Shift+click on UNDO redoes. A Preset step made in the window
  (the wheel on a channel's name or PRESET, a click on a bank place, Up and Down) is an UNDO step that
  brings back the channel's whole sound with its old Preset, edits (`*`) included; a run of steps on one
  channel is one step. Knob edits and automation stay with the host's undo. The wheel down goes down
  the bank list. On ALL the OUT boxes are scaled per chip, so a channel at full volume fills its box.
- ALL page (0.24.4): each FM and PSG row has an OUT box with the channel's live output from the emulator,
  two periods of its lowest sounding note (the noise channel the last 5 ms), empty without a note. The DAC's
  row has PAN and VOL and shows the last sample struck with a line where each sounding voice is. DAC VOL is
  not MDMI canon: our ROM scales the velocity of new DAC notes by channel 6's CC 7, so the DAC can be
  turned down alone; it also works on a console with this ROM, not with another MDMI ROM. VOL 0 is about
  -24 dB, not silence, and channel 6 has one CC 7 for FM6 and the DAC.
- Bank libraries (0.22.4): the BANK page shows two lists side by side. The left one is the ROM bank,
  the one that plays (Preset, a clip's Pgm, Program Change) and is saved with the project; its bar can
  also switch it to a library. The right one is a library: any `.mdbank` file in the folder
  **MDMI Banks** next to the plugin file, for example `D:\VST\MDMI Banks` beside `D:\VST\MDMI Slot.vst3`.
  A library shows only its sounds; its empty places are counted as FREE. Click a library's sound to hear it
  on the selected channel (the Preset stays, as with a dropped file). Drag a sound to the other list to copy
  it: onto a ROM bank place it replaces that place, into a library it goes to the first free place. CLEAR on
  the row under the mouse empties a library's place. NEW (on a list's bar, or under NO FOLDER) makes an empty
  library, named in the save dialog, and the folder if it isn't there. Library changes are written to the
  file at once and UNDO writes them back; there is no SAVE for libraries. Every project and every MDMI Slot in
  a set sees the same libraries; another instance sees a change the next time its BANK page opens. A project
  never depends on them: what you tried or copied is in the project. A bank saved with SAVE earlier is a
  library as soon as it is in the folder. To hand the plugin to someone, give them the plugin files and the
  MDMI Banks folder. Rename or delete libraries in Explorer.
- Arpeggio rows (0.20): a sound can carry a row of up to 32 steps, one per frame (60 Hz), each a shift of
  -24 to +24 semitones, with a loop. The ROM plays it, so it works on a console too. Draw it under the PSG
  envelope or on the FM page (ARP). STORE puts the channel's sound with its row into its bank place; rows
  live in the project, the bank file and snapshots, not in TFI, VGI, TYI or EEF files (a dropped file comes
  without a row). ARP ON / ARP OFF at the row's right (0.22.3) switches it off without erasing it: off, it
  plays as no row, its steps stay dim and editable. The row doesn't act on the DAC, on channel 3 in Ch3 Special with OPS or on the noise
  channel unless its rate is Tone 3. Not checked on a
  real console yet.
- Channel 3 by operators (0.21): with CH3 SPECIAL on, CH3 NOTES in the window's bottom strip chooses how
  channel 3 plays. OPS (the default): each operator has its own note with its own Note On and Note Off,
  MIDI 11, 12 and 13 play operators 1-3 and MIDI 3 plays operator 4; a released operator dies away while
  the others sound. 1 NOTE: a note on MIDI 3 plays all four. In both, each operator stands at its own
  interval (SEMI, -48 to +48 semitones), detune (FINE, -63 to +63 cents) or, with FIX, a fixed pitch:
  C4 moved by its SEMI and FINE, whatever the key. On channel 3's FM page each operator's header shows a tag of its
  pitch; a click on it opens SEMI, FINE and FIX. Before 0.21 a MIDI 3 note keyed all four operators and
  MIDI 11-13 notes only set pitches: in OPS that is gone; the closest to it is 1 NOTE with FIX on the
  operators that stood still. The arpeggio row works on channel 3 in 1 NOTE. Switching CH3 NOTES, CH3
  SPECIAL or the DAC releases channel 3's notes. The settings belong to the project, not to a sound, bank or file.
- CSM (0.22), the third choice of CH3 NOTES: the chip's timer A keys all four operators of channel 3 over
  and over, as many times a second as the frequency of the note on MIDI 3. That gives buzzes, noise and
  vowel-like sounds; the operators' own pitches (SEMI, FINE, FIX, usually FIX) act as formants. Each key-on
  lasts one sample of the chip, and only AR 31 attacks within it, so while channel 3 is in CSM the ROM
  sets AR 31 on its four operators itself (0.22.3). The voice keeps its own AR and gets it back
  when you leave CSM; RR shapes each repeat. The repeat rate is exact to a few cents up to MIDI note 71 (B4, 494
  Hz: within 8 cents, 5.4 at most in BlastEm); above it the timer's steps get coarse: note 81 (880 Hz) is
  13 cents flat, note 84 (1047 Hz) 3 flat, note 96 (2093 Hz) 31 sharp. The lowest note is MIDI 32 (52 Hz);
  every note below plays as note 32. A bend, portamento or the
  arpeggio row moves the rate without a new attack.
  Portamento glides only between overlapping notes: with Porta On, a new note slides from the previous one
  only while that one is still held.
  On the chip the operators' TL works differently in CSM
  than in BlastEm: operators with a high TL may die away faster on a console. MIDI 11-13 do nothing in CSM.
  Not checked on a real console yet.
- LFO RESET in the bottom strip (0.21): every new FM note of one chosen MIDI channel 1-6 restarts the chip's
  LFO (DAC samples on channel 6 don't), so its vibrato and tremolo start the same way on each note. The chip has one LFO: the restart moves
  the vibrato and AM of every channel that uses them. On a real console the start may still wander by up
  to 1/128 of the LFO's cycle (about 2 ms at the slowest rate; BlastEm doesn't show this), and channels
  with AM jump in level at each restart (up to about 12 dB at AMS 3), which may click. Not checked on a
  real console yet.
- DAC samples on channel 6: drop WAV files on notes or on the open note's wave, trim, tune and shape them.
  Up to four samples sound at once (SGDK's PCM4 driver, 0.19): a hat doesn't cut the kick. A fifth note takes
  the voice of the oldest; the same key again restarts its own. Velocity sets a sample's volume in 16 steps
  (the lowest is about -24 dB, not silence). All four play at one rate, 15.2 kHz in the emulator (about 16 kHz
  on a console), so the sound reaches about 7.6 kHz; several loud samples together clip. A note starts 16-32 ms
  after its Note On, and switching the DAC on takes about 0.3 s, some 75 ms longer than in 0.18, because the
  driver loads a volume table. Since 0.30.0 FM notes held through a DAC switch (on or off) strike again on
  their own when it is done, on channels 1, 2, 4 and 5; channel 3's are released, as before. Channel 6's Pan (CC 10) puts all four left, centre or right, also while they
  play. A sample with LOOP repeats while the key is held: a click on the loop's row over the wave turns LOOP on
  or off (since 0.25.3 there is no separate LOOP switch). Its loop has its own tabs on the wave: the part
  before it plays once, the part after it (the release) plays when the key is let go. Drag the loop's body to
  move it whole. A click on the loop's end tab, or on the row after it, chooses how Note Off ends the note:
  RELEASE (the loop finishes its pass, then the release plays; a short note let go in the intro plays the loop
  once and the release) or STOP (silence at once; the release isn't written to the ROM, its marks stay). A
  double click on the end tab moves the loop's end to the last whole block: no release. Turning LOOP on or
  switching ONE-SHOT brings the loop back with its release. LOOP and ONE-SHOT exclude each other again (0.22.2 let them go
  together, a loop past Note Off; such a project opens as LOOP with its release).
  A loop is at least one block of the driver (256 bytes, about 17 ms). Not checked on a real console yet.
- Track audio through the DAC (0.26.0, emulator only): the plugin has a stereo input, DAC In, which Live shows as
  Sidechain. Another track's audio sent there plays through the emulated DAC with the samples: 8 bits, at the
  driver's 15.2 kHz, nothing above 7.6 kHz filtered (it folds back, as the console's DAC would make it), on
  channel 6's output and in Main, at the plugin's reported latency like a note. Only while DAC Enable is on.
  No knobs: the sidechain's gain sets the level; full scale fills the DAC, and the input and loud samples
  together clip. The DAC page's AUDIO IN lamp lights while the input reaches the DAC (0.26.1). The Filter
  knob's default, the console's 3392 Hz, softens the DAC's grit; open it fully to hear it. Routing in Live, in
  Russian: `docs/dac-input-live.md`.
- A PSG envelope's loop ends a note the same way (0.22.3): with steps after the loop (a release), Note Off lets
  the loop finish its pass and plays the release, also when the key is let go in the intro (the loop then plays
  once); without them, or without a loop, Note Off silences at once. Before 0.22.3 a loop without a release
  played out its pass and Note Off in the intro silenced: this changes how some EEF files and ROM envelopes end.
  To keep a pass playing out, add a quiet release step. A click on the loop's end tab or the strip after it
  chooses RELEASE or STOP; with STOP the envelope plays, saves (EEF), STOREs and goes to the ROM without its
  release, which the project keeps, dim in the editor, until the release is chosen again. All Notes Off and
  All Sound Off silence PSG notes at once, as the DAC's.
- The Retrigger switch decides how a note right after another starts: as the chip does, on a free voice, or
  from silence.
- PSG noise on channel 10: Noise Type (periodic or white) and Noise Rate (L M H, Tone 3, or Kit). On Tone 3
  the noise follows the note and periodic noise plays bass; Kit lays a drum kit over every octave (C kick,
  D snare, F# hat...), made for C1-B1, an octave up the same kit higher. Both take channel 9's tone, so channel 9 is silent.
  Not checked on a real console yet.
- An output per MIDI channel besides Main, so each channel can be recorded or processed on a track of its own.
  A channel's output carries all its notes, also a chord's notes on borrowed voices. Main still plays
  everything. This exists only in the emulator: a real console has one mixed output.
- MASTER, at the right of the window's bottom strip, sets the plugin's output level from silence to +12 dB,
  on Main and every channel's output alike. It can be automated and is saved with the project. There is no
  limiter, so above 0 dB the plugin can clip. The console has no such knob.
- The PSG plays 3 dB louder than in BlastEm, tones and noise, on Main and the channel outputs. BlastEm
  puts a full PSG tone about 10 dB below one FM channel at full level. That balance is real, but it
  differs between consoles, and it made the PSG hard to hear next to FM. A real console has its own
  balance; checking it against a Genesis MK-1631 is planned.

## Requirements

- Windows, 64-bit. Tested on Windows 11.
- A VST3 host. Tested in Ableton Live. The CLAP version is tested in a test host only: the
  version of Live it was made with does not load CLAP plugins.

## Installing

1. Download the newest `MDMI-Slot-<version>-win64.zip` from the
   [Releases page](https://github.com/KPY7030P/mdmi-slot/releases) and unpack it.
2. Close your host. Copy `MDMI Slot.vst3` to `C:\Program Files\Common Files\VST3`.
   For a CLAP host, copy `MDMI Slot.clap` to `C:\Program Files\Common Files\CLAP`.
3. In Live: Settings (Preferences in older versions), Plug-Ins, turn on "Use VST3 Plug-In System Folders" and press Rescan.
   The instrument appears under Plug-Ins as **MDMI Slot**.

To update, replace the files while the host is closed: Live locks the VST3 while it runs.
Bank libraries live in the folder `MDMI Banks` next to the plugin file; updating the plugin leaves it as
it is. In `C:\Program Files` Windows doesn't let the plugin write there, so libraries can be read and
copied from but not changed; put the plugin in a folder of your own to change them.

## First steps

1. Put MDMI Slot on a MIDI track and open its window.
2. Play: MIDI channel 1 plays the first FM channel. To play several channels from one instrument, send
   other MIDI tracks to it on MIDI channels 2-10 (in Live: MIDI To the instrument's track, then the plugin
   and the channel). Each channel is an instrument of its own, with its own knobs.
3. In the window, pick a channel on the left. EDITOR shows its sound, BANK the bank, ALL every channel at once.
   Drag a value up or down to change it, or turn the mouse wheel over it for one step a notch. Preset, a
   clip's Pgm, or Program Change choose a bank place.
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
- **File names that are not in Latin letters**: Cyrillic shows transliterated (Bas for Бас) in the window and
  on the ROM's screen, other letters as `?`. The name itself stays as it is in files and the project.
- **Windows only.** There are no Mac or Linux builds.
- The sound is BlastEm's emulation. A real console may sound different in places. The noise modes of 0.16 and the
  DAC pan and loop of 0.18, the loop marks of 0.18.2, the four DAC voices of 0.19 and channel 3 by operators
  and LFO RESET of 0.21 and CSM of 0.22 are written the same way for both, but have not been checked on a console.

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
