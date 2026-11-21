# MDMI Slot: how it works

The developer's guide; the musician's is the [README](../README.md). It was the README up to 0.14.0.

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
  `%TEMP%\MDMI-Slot`, because BlastEm keeps its state in globals.
- `src/engine.c` runs the emulator on its own MMCSS thread and feeds a ring buffer. MIDI is applied at
  a fixed distance ahead of the audio, and that distance is reported to the host as latency.
  A host with a large device buffer calls several blocks in a row; a block of such a burst waits for the
  emulator up to half its own length, a block alone up to 1 ms (D14).
- The engine journal: create an empty file `%TEMP%\mdmi-journal` (PowerShell: `ni $env:TEMP\mdmi-journal`)
  and every instance activated after that writes `%TEMP%\mdmi-journal-<process>-<n>.csv`, one line a second:
  blocks and bursts from the host, how full the ring was, waits, shortfalls, notes, and the emulator's runs,
  time, cycles and core moves. Delete the flag to stop. Tests name a folder in `MDMI_JOURNAL` instead.
- `src/plugin.c` is the CLAP layer. clap-wrapper builds the VST3 from it.

- `src/patch.c` holds the knobs. It tracks what they show and what the ROM has, and sends the difference as MIDI.
  `src/presets_gm.h` is generated from the ROM's source by `tools/gen_presets.py`, so knobs can show preset values.

## Knobs

Every MIDI channel is an instrument of its own: its knobs always mean that channel, so lanes for several channels
can run at once. There are 377 of them, too many for Live to fill its panel by itself: use Configure and touch a
cell in the window. Choosing a channel in the window only changes the view, no knob or lane. A cell with a
number takes a vertical drag (`drag_value`: pixels a step by its range; channel 3's Semi 6 px, Fine 3 px, stage
22a) or a wheel notch, one step and one gesture to the host each; parts of a notch from smooth wheels add up.
The wheel scrolls the BANK and DAC lists where it did; the DAC page's own cells, envelope and arp bars take no wheel.

- Global: Filter (3392 Hz matches the original console, top is off), LFO On, LFO Rate,
  Ch3 Special Mode, DAC Enable, Retrigger, Noise Type, Noise Rate, Master Volume, Ch3 Notes, LFO Reset, and
  Semi, Fine, Fixed of each of channel 3's four operators (ids 10-23, last in the host's list)
- Every channel: Preset, Volume, Fine Tune, Bend Range, Porta On, Porta Time
- FM channels 1-6 also: Pan, Algorithm, Feedback, AMS, FMS, and for each of 4 operators TL, MUL, DT, RS, AR, DR,
  SR, SL, RR, AM, SSG

Knob ids never change, because hosts store automation by id: a global's id is its place in `src/patch.h` (0 held Polyphony and stays unused), a channel's knob is
1000 + channel * 100 + its place in `src/patch.h`, so 1411 is Op1 TL of channel 4. New knobs take the next free
number. A project stores pairs of id and value, so a knob added later starts at its default in an older project.

- Voices (D20, D21): each FM channel plays on its own voice, and a chord borrows the voices of channels
  that are silent, nothing to set up. A channel always plays on its own voice: when a chord holds it,
  the chord's note there moves to a free voice and starts again, and is cut only if none is free (D22). A cut note that is still held starts again as soon as a voice comes free. Notes beyond the free
  voices don't sound, and the window marks them as CUT; over such a row the footer names the voices that never lend and why (stage 24b, D69, `patch_lend_reason`: SP, DAC); a fresh message comes first (stage 49: the footer's one order is a gesture under way or the button under the mouse, what just happened, what the mouse is over, the engine's numbers). A green bracket in the window joins a chord's
  channel to the rows whose voices it plays. A borrowed voice
  first gets the channel's sound: notes of one moment wait until all their voices have it, a few ms, up to
  about 20 ms for a chord on voices that held other presets. Channels with Porta On, channel 3 in Ch3
  Special Mode and channel 6 with the DAC play only on their own voice and borrow none (`patch_alone_reason`).
  Channel 3 in Ch3 Special and channel 6 with the DAC lend none either; a Porta channel lends its voice while it
  is silent, after the other free voices, and its own note takes it back (D69, stage 28). The ROM glides only
  between the channel's own held notes, so a chord's note on its voice leaves no glide behind; a voice let go
  forgets its glide target (`dev_chan_note_off`, stage 28), else a legato raised at once left one for the
  next note on that chip voice. Switching Ch3
  Special or the DAC lets cut notes try the voices free since, with no Note Off (`never_lend_mask`).
- Bank (D23): one bank of 128 places for the plugin, at first the ROM's GM presets. Place N is
  program N: Preset, CC 3 and a clip's Pgm choose from it on any FM channel. The BANK page lists it.
- Files (D25): a TFI, VGI or TYI dropped from Explorer on an FM channel's row gives that channel its
  voice and name, like tweaks: the Preset stays, and the next preset step wipes it. Dropped on a place
  of the BANK page, it replaces that place. LOAD opens an instrument for the selected channel, a
  `.mdbank` for the bank or a `.mdsnap` for everything; SAVE writes the channel as VGI, the bank or a
  snapshot, which is the project's own state. UNDO puts back what the last file replaced, Shift+UNDO
  brings the file back; the history holds 32 steps (D90). A Preset step made in the window (the wheel, a
  bank place, the arrows) is a step: `files_preset_step` keeps the whole values before it. Steps on one
  channel join while the model shows it untouched on a place the run's steps went to (`went`, those the
  model hasn't taken yet included); on any other place the host stepped it (Program Change, CC 3, the
  Preset lane), and the next step is a step of its own, so UNDO stops where the host put the channel
  (stage 50). `src/instrument.c` reads the formats, `src/files.c` and `src/state.c` the rest.
- Several files at once (LOAD, ADD or a drop): the window takes 128, `files_put_places` counts the rest
  (`beyond`) with those past place 128 as extra, the unread apart: `Loaded 127 to slots 1-127, 2 extra,
  1 unread` (stage 49). SAVE adds the chosen type's extension to a name that has none of ours; when it
  did and that file exists (`Bass v1.2`: Windows takes `.2` for an extension and asked about another
  name), `files_save_path` says so and the window asks before replacing.
- Libraries (stage 22c, D56): `.mdbank` files in the folder `MDMI Banks` beside the plugin's DLL, or
  beside the `.vst3` bundle that holds it (`src/library.c`: `library_folder`, `library_folder_for` from a
  path, `MDMI_BANKS` for tests; `library_scan` lists up to 64 by name). The format is the bank file's;
  `state_bank_build` and `state_bank_read` write and read one outside the model. A place is empty when it
  is the ROM's GM preset (or envelope) with an empty row (`files_place_empty`). `files_try_place` gives a
  place to a channel as a dropped file goes, `files_copy_place` copies between the ROM bank and libraries
  in any direction, `files_clear_place` empties a library's place. A library is read again before every
  change, one place changes, and the whole file goes to `<name>.mdbank.tmp`, then `MoveFileExW` replaces
  the old one; undo (`UNDO_LIBRARY`) writes the place back the same way. The audio thread never sees
  libraries. The window (`draw_bank` and the functions around it in `src/gui.c`) shows list A (the ROM bank
  or a library) and list B (a library); which ones is the instance's (`gui_banks`), not the project's. It
  reads the folder and both files again when the page opens, a bank is chosen and after each action. A
  press on a library row tries it, a drag to the other list copies (into a library: its first free place),
  CLEAR on the hovered row empties it, NEW asks the save dialog (`MDMI_TEST_NEW` in tests).
- EXPORT (stage 24, D58): `files_export` writes the ROM for the SD card. `rom_image_build` takes, besides
  the samples, a `rom_bank` (the ROM bank as the fork's tables hold it, from `rom_bank_of` in `files.c`:
  an envelope and a row as they play, a row switched off as 0 steps) and a `rom_names`. The FM voices are
  packed over the build's GM ones where `M_BANK_0` points (`rom_image_pack_voice`, GCC's m68k bit fields:
  no field straddles a byte; `rom_image_check` refuses an image whose GM voices differ from the plugin's
  packing, its rows aren't empty or the names table's magic `MDMINAME` is missing); an envelope the image
  doesn't hold goes after the samples and `ENVELOPES` points at it; the rows go into the fork's `ARP_ROWS`
  (FM's 128, then PSG's), which a reset copies to RAM, so GM Reset and `FF` keep the bank; the names and
  the signature into `PROGRAM_NAMES`. `tools/gen_rom_map.py` finds all four by `symbol.txt` and fails
  without one. The signature is `BANK` and a CRC-16 of the image past the signature and the header's
  checksum (`rom_image_signature`); `rom_image_for_sd` pads to a whole 512 KB and writes the checksum. The
  file goes beside the target and then over it (`write_replace`). The emulator keeps booting the GM tables
  and its sound goes by `sync` as before; only the names reach it: `files_screen_names` gives the bank's,
  with a channel's own sound's name on its service numbers (FM 116 + 2c, PSG 120 + 2p), and the signature
  EXPORT would write now (reckoned again when the bank or the samples change). The window hands them to
  `mdmi_engine_set_names` while the ROM page shows; the engine writes them into the cartridge between
  frames and after every boot, and `patch_core` leaves their bytes out of its comparison. The fork's
  `ui.c` draws a row's name at column 11, 23 characters, only when the row's program, MIDI channel, DAC
  or the name's bytes changed (the frame handler shares the frame with MIDI: a `sprintf` per row each
  activity frame shifted the timing of seven `reg_test` checks), `DAC` on FM6 while the DAC plays,
  `PROGRAM n` where a name is empty, the signature at the top right, and only warnings of the journal
  (`ui_log_shown`); `No SRAM` became an info line. `names_ascii` (`src/names.c`) gives the ASCII both fonts
  draw: Cyrillic transliterated, two letters at most, so a name cut to bytes still fits its columns; the
  window's `text()` draws through it. `names_cut_utf8` ends a file's name cut to 24 bytes on a whole
  letter. Tests name the file in `MDMI_TEST_EXPORT` instead of the save dialog.
- Stage 24a's ROM status screen (`docs/design/rom-screen.md`, D63-D66) lives in the fork's `ui.c`. Plane A cells are
  written by `update` under one ceiling (32 words a frame), compared with a shadow. Colours (the live note cells,
  the noise's 4 cells) and plane B's scroll go through a fixed-slot queue that `ui_vint` (called from
  `scheduler_vsync`) writes at the start of VBlank, only inside it. Cell writes go as one long control word plus
  the data, again if a VInt came between (the VInt parks the address on 0xFFFE). The ribbon is plane B: it
  scrolls a point a frame, and only its leading cell at c22 is written, against a shadow per column. A
  channel's `hits` (in `DeviceChannel`) counts Note Ons for the ribbon and the white flash. For timing tests,
  `ui_test_off` (read once a frame) is 1 screen off, 2 clean stamps, 3 by part, 4 stamps alone, 5 without
  24a2's elements, 6 the same by part. `reg_test --screen` runs the screen's tests alone.
- 24a3 (D64, D67, the owner's option B). The SGDK sprite engine is gone: the ROM owns the sprite table, and CC 83's
  algorithm picture is plane A tiles (1285-1335). `UiEnvelope` (`ui.h`) is an operator's envelope estimate, built by
  tables from AR, D1R, SL, D2R, RR (`gen_rom_screen.py`, BlastEm's YM timing). Frame counters since Note On / Off
  run for FM 1-6 and channel 3's operator channels. They feed the Ch3 squares (filled 4 to 0 at 6, 12, 24,
  48 dB down), the FM dots (a segment at a time, a step a frame) and W1. W1 is in dB since stage 29
  (rom-screen.md 11): the chip's own attenuation for volume x velocity (`synth_volume_attenuation`, TL units)
  plus the dot's height as attenuation, through `LEVEL_LIT` (16 stripes, fine steps at the top). Stage 30
  (rom-screen.md 12, D72): it rises at once and falls at most `LEVEL_FALL` 2 stripes a look; a note over
  before its first look shows its velocity; the peak tick is off (`UI_LEVEL_PEAK` 0, its code kept, a peak
  holds only a rise). The ribbon's strike cell, while its key is down, takes the second set of 256 patterns
  (`UI_T_RIBBON_HOLD`, VRAM 649-904): the hold line starts at the strike. Notes of octave -1 are named
  `C--` in the window and on the screen, every name 3 characters. Stage 31 (rom-screen.md 13): a strike
  cell takes the owner's hue as the owner number does, k = MIDI channel & 3 (`ribbon_hue`; 0 for the DAC
  and Ch3 Special): k = 1-3 draw from `UI_TILES_HUE`, a set of 102 tiles each at 905, 1092, 1336 (92
  patterns of 1-3 strikes, 8 of one strike + the hold, 2 combs for 4 or more strikes, every other column
  from the first strike's), chosen by `UI_HUE_CELL[held][bits]`; the line stays grey and the hold the
  chip's dim. A lent voice's new cells take the chord channel's hue; written cells stay. Curves
  (1020 + 12k) and PSG bars (1237 + 12k) are tiles written while
  music plays, one address and 16 words each. A rebuild starts only in a frame with fewer than 53 lines so far
  and goes a raster column or a tile at a time until 64 lines (N is `ui_stat_curve_lag_max`). The VInt queue writes runs: a
  palette's live colours, the noise's 4, each dot's whole sprite entry. Hard limits found:
  - the 68000's RAM: SGDK takes its 8 KB DMA buffer from the heap before `main`, so bss must end below about
    0xFFC300 or the ROM hangs at boot (the plane A shadow keeps 40 columns);
  - an address + word pair costs about 0.5 line in active display;
  - `bool` is a u8;
  - a u32 product goes to `__mulsi3` (use `mul16`);
  - a u8 array may sit at an odd address.
  For the tests: `reg_test --adjacent N` and `--loop N` run one test with switch N; `MDMI_SCREEN_SWITCH=N` holds
  it for a whole run; `MDMI_CLIP_PARTS=1` breaks the busy clip down by part; the run prints the worst owner wait
  after each test.
- A voice that differs from what the ROM holds in more than 12 values goes as one SysEx `0A`, which
  takes as long as 12 controllers (D24).
- MONO (stage 38, D83): globals 24-29 `Mono CH1`-`CH6` (after `GLOBAL_CH3_OP_END`), CC 105 on the channel
  to our ROM, which keeps a `mono` flag per MIDI channel (a reset clears it, the plugin sends it again).
  On an FM channel that plays alone, without Porta, a note over the sounding one and the raised top key's
  return write key-off, then key-on (`mono_voice`, not channel 3 in Ch3 Special, not the DAC), so the
  attack starts again. The dealer treats MONO as Porta (`PATCH_ALONE_MONO`, lent while silent). The window
  pairs MONO and PORTA (art-director, ui-concept.md): a click makes one the mode, the other goes off; the
  wheel and a drag keep the same rule (stage 49, `pair_rule`).
- Envelope speed (stage 44, D88): globals `Ch7`-`Ch10 Env Speed`, CC 9 on MIDI 7-10, the ROM's table
  `ENV_SPEED_RATE` (`third_party/mdmi/src/env_speed.h`, steps a frame times 256), read by the plugin through
  `patch_env_rate` and `patch_env_speed` only. SPD on the PSG page; the envelope editor's footer times
  (STEP, LOOP, LENGTH, RELEASE) and its axis' MS row count at the channel's SPD (`env_ms`, stage 49), the
  axis' other row counts steps (STEP). The arp row's times stay frames: SPD doesn't move the arp.
- PSG noise (stage 16, D45): Noise Type (Periodic, White) and Noise Rate (High, Mid, Low: the chip's clock
  / 512, 1024, 2048; Tone 3; Kit) are the console's, one noise register, and go as CC 12 and 13 on MIDI 10.
  The window shows the fixed rates low to high, L M H. Kit is a General MIDI drum kit in every octave: C
  kick, C# rim, D snare, D# clap, E snare, F low tom, F# closed hat, G tom, G# pedal hat, A tom, A# open
  hat, B tom. It is made for keys 36-47 (C1 in Live), each drum a type and a tone 3 value N; an octave up
  halves N, down doubles it (1-1023). It runs on tone 3, so channel 9 is silent as at Tone 3; Noise Type, tune, bend
  and the envelope's pitch do nothing there (the ROM takes the pressed key), and the window dims them. Our
  ROM writes the mode as the one byte `E0|mode`, never followed by a data byte, on every note of channel 10,
  on a change and after a driver swap; a note chooses it only at Kit, by its drum. On Tone 3, note k writes the tone of
  key k + 48 into the PSG's channel 3: periodic noise loops over 16 bits, so it sounds at note k, notes 0-79.
  Channel 9 is silent meanwhile: the ROM cuts its note and takes none of its events. At a fixed rate every
  key plays the noise, 0-44 too (before 0.16 they were dropped), and the envelope's pitch shift does nothing.
  Not checked on a console.
- Ch3 Special Mode (stage 21, D51, `docs/research/ch3-csm.md`, `docs/design/ch3-ops.md`): channel 3's
  operators have pitches of their own, and Ch3 Notes (id 10, CC 81: 0 Ops, 64 One Note, 127 CSM; the ROM
  reads value * 3 / 128) chooses how they play. Ops, the default: MIDI 11-13
  play operators 1-3 and MIDI 3 operator 4, each with its own Note On and Note Off; our ROM keeps a shadow
  of channel 3's four key bits and writes 0x28 once per event as `mask << 4 | 2` (operator k is bit
  `0x10 << k`). One Note: a MIDI 3 note keys all four (0xF2), MIDI 11-13 notes write nothing. One pitch rule
  for both: the source note + Semi + Fine, kept in 11-106; with Fixed the source is C4. The source is the
  operator's own note in Ops (with its channel's bend and Fine Tune), MIDI 3's note in One Note (after bend,
  Fine Tune, portamento and the arp row). Semi (value - 64, ±48), Fine (value - 64 cents) and Fixed (64-127
  on) are CC 102-104 on the operator's channel only, MIDI 11, 12, 13 and 3; a sounding note moves at once.
  The knobs are globals like the noise's (D45), ids 12-23 (Op1 Semi, Fine, Fixed, then Op2-Op4): not part of
  a sound, the bank or files. `patch_sync` sends CC 81 and 82 on MIDI 7 and CC 102-104 on the operators'
  channels; a ROM reset loses them, a driver swap doesn't (they are in the ROM's RAM). From a clip CC 81 and
  82 move the knobs on any channel, CC 102-104 only on MIDI 3 and 11-13 and are dropped elsewhere. CC 80,
  CC 81, a DAC switch and a reset release channel 3's keys in the ROM. Only a ROM reset brings Ops back:
  a GM reset and CC 121 don't. Before 0.21 a MIDI 3 note keyed all four operators and MIDI 11-13 notes only
  set pitches; One Note with Fixed is the closest to it. Channel 3 plays alone in both modes. Without Ch3
  Special MIDI 11-13 notes write nothing.
- CSM (stage 22, D52, `docs/research/ch3-csm.md`): Ch3 Notes' third position, only in Ch3 Special. A MIDI 3
  note writes the formants as One Note does, then timer A's NA = 1024 - round(53267 / f) (52781 on PAL),
  0x24 = NA >> 2 and 0x25 = NA & 3, then 0x27 = 0x81: the timer starts with an overflow at once, and every
  overflow keys all four operators for one sample, 53267 / (1024 - NA) times a second. The table of periods
  in `midi_fm_sm.c` runs over the notes, cents between neighbours as the F-num; notes below 32 are NA 0
  (52.02 Hz). Our ROM writes no 0x28 for channel 3 in CSM: a key held there would hide the repeats. Note
  Off goes back to mode 01 (0x27 = 0x40): stopping timer A alone leaves the operators keyed in BlastEm when
  it lands just after an overflow (the stage's experiment: 7 of 200 stops at random moments, by the mode 0
  of 200). A bend, portamento, the arp row, a note over the sounding one and a raised note of the stack write
  only the formants and 0x24/0x25: the timer keeps its phase and takes the new NA at its next reload; a
  reload between the two writes gives one period off by up to 3 ticks, accepted. CC 80, CC 81, a DAC switch
  and All Notes Off end a CSM note by the mode too; MIDI 11-13 write nothing; 0x26 is never written. Velocity
  as in One Note. `reg_test` measures in BlastEm: notes 45-71 within 5.4 cents of the note, note 84 -3.4,
  96 +30.9; timer A's clock 53267.0 Hz. AR 31 in CSM (stage 22b, D57): while `global.specialMode && csm`,
  `synth.c` writes AR 31 with the voice's RS to channel 3's four operators (`effective_attack_rate`, as
  `effective_total_level` puts the volume into TL); `synth_set_csm` and `synth_set_special_mode` rewrite the
  four AR registers when that condition changes, a note writes none. The stored voice, the model and
  `synth_extract_preset` keep the voice's AR; a raw SysEx `08` to 0x50-0x5E stays raw. Every 0x27 comes from one shadow
  in `synth.c`: bits 7-6 and 0 are the ROM's mode (Ch3 Special off 00; Ops and One Note 01; CSM 01 without
  a note, 10 with timer A on with one), bits 1-3 only a raw SysEx `08` write of 0x27 sets, and that write's
  bits 4-5 go once; its bits 7-6 and 0 are not taken. A driver load (DAC on or off) writes 0x27 from the
  shadow after SGDK's `YM2612_reset` cleared it, which leaves 0x24-0x26 alone; a ROM reset clears the
  shadow. The dealer gives voice 3 to channel 3 at once for a CSM note (`owner` with `at_once`): no 0x28
  key-on marks it. So does a glide note on any FM channel (stage 22a, `start_note` `glides`: a note dealt
  alone over a held one with Porta On): it only moves the pitch, no key-on comes to settle a waiting owner;
  the voice is the same channel's already, so nothing sounded wrong, but `reg_test` counted the owner as
  missed. Not checked on a console; the chip's TL in CSM differs from BlastEm's (stage 26).
- Note stack (stage 22b): `midi_note_off` reads the next note with `note_priority_peek` and leaves it on the
  stack, so the stack holds exactly the held keys. Upstream MDMI popped it and pushed it back only when the
  raised key was the sounding one with Porta On: raising another key lost the sounding note, and the next note
  of a Porta legato chain struck instead of gliding. Without Porta the raised top key strikes the one below,
  as before.
- LFO Reset (id 11, CC 82: off, or MIDI 1-6 at the start of each seventh, value * 7 / 128 back): our ROM
  writes 0x22 without bit 3 and right after it 0x22 from its shadow before the 0x28 of every new attack on
  that channel's FM voices (a chord's note on another voice and a raised note of the stack too), not for
  legato with portamento, nothing with the LFO off. No pause of its own: BlastEm clears the LFO's step on a
  timer tick (18.77 us) and SGDK's write already waits about 25 us. `reg_test`: five notes at random moments
  with FMS 7 start their vibrato within 0.2 cents of each other, without the reset up to 160 cents apart.
  The chip has one LFO: the reset moves every channel's vibrato and AM. On a console the prescaler isn't
  reset (the start wanders by up to 1/128 of a cycle) and AM jumps to its step 0; README.
- TV look (stage 36, D81, `docs/design/tv-look.md`): the ROM page's switch `TV: OFF | RGB | COMP` over the
  picture's right edge draws the ROM's frame as the console looks on a TV, in the window only. `src/tv.c`
  (`tv_draw`) reads the 320-wide frame and writes the window's 640x448: RGB is the TV's gamma (2.5 against
  2.2) and scanlines (a bright and a dark window row per line, the dark one filling in on bright colours),
  COMP first a cheap composite TV (16 subsamples a pixel, the subcarrier every 30, the same phase on every
  line; luma the mean over a cycle, chroma demodulated through two 45-subsample means). A line unchanged
  since the last frame is taken from the kept result. Another width draws as OFF. `rom->pixels`, the ROM and
  the sound are untouched; test snapshots are OFF unless a test chooses. The view block's fourth byte keeps
  the choice. `gui_test` checks OFF is the frame at x2 and the frame unchanged after the looks;
  `tools/test/check_tv.py` checks RGB and COMP against the numpy model within 2 levels.
- DAC Enable: MIDI channel 6 plays drum samples, chosen by note, with no FM voice of its own. Since stage
  19 (D49, `docs/research/pcm4.md`) the driver is SGDK's PCM4: four voices mixed on the Z80 at one rate,
  `ROM_PCM4_HZ` 15236 Hz as BlastEm plays it (3579545 / 223 = 16052 on a console). Our ROM writes the
  driver's RAM itself (`pcm_driver.h`, `midi_dac.c`): a channel 6 note goes past the one-voice note stack
  to `midi_dac_play`, which gives the key its own voice while it sounds, else a free one, else the oldest
  start's, and sets the voice's volume to velocity >> 3; Note Off touches only the key's voice. The ROM's
  samples are made by rescomp `PCM4 15500`, so they last as in 0.18. Channel 6's Pan (CC 10) pans them
  (stage 18): our ROM writes B6 at once for all four, the driver never writes it; switching the DAC on or
  off writes channel 6's pan again for the side now in charge.
- Track audio through the DAC (stage 23, D73, emulator only). The plugin's one input port, `DAC In` (not
  main; clap-wrapper makes it an aux bus, Live's Sidechain), goes mono, (L + R) / 2, to
  `mdmi_engine_input` before each block: a ring at the host's rate by ring position. Patch 0001 counts the
  YM2612's output samples (`mdmi_samples`) and, at each write of 0x2A while the DAC is on, adds
  `mdmi_dac_input(n)` to `value - 0x80` and clamps to -128..127: a fifth step of PCM4's saturating sum, at
  the driver's moments, so the 8-bit step, pan, filter and the FM 6 output all apply. The engine's
  `dac_input_cb` maps the chip's sample n to a ring position by an anchor taken at the first batch heard after
  a boot or an activation (the chip's samples and the core's frames since its start are one count), minus
  the reported latency, so the input comes out where a Note On at the same moment would; linear
  interpolation between the host's samples; no filter. A write whose input isn't there gives 0 and counts
  `input_misses` (stats, journal column `in_misses`). With the DAC off writes to 0x2A are ignored: the input
  is dropped. `reg_test --input`: in channel 6 and Main with a sample, saturation, 0.3 of a step silent,
  out at the latency (15.6 ms against 15.3 at 48 kHz, 256), silent with the DAC off, no misses; it costs
  about 2-5% of the engine's time offline, within run-to-run noise. `vst3_host` gives the input bus a silent
  buffer, as Live does; with `MDMI_TEST_INPUT=stereo` a 1 kHz sine and DAC Enable on (run_tests' «vst3 input»,
  `check_input.py`). The DAC page's AUDIO IN is a lamp, not a switch (D74): lit while `input_signal` (the
  input reached half a DAC step within 0.3 s, `in_signal_at`); the page shows only with the DAC on.
- DAC pages (stage 12, D36, D37, `docs/design/dac-page.md`): EDITOR of channel 6 while the DAC is on is a
  grid of the 128 notes and the selected note's wave with REPLACE and CLEAR; BANK lists the same notes.
  Each note plays the ROM's sample (notes 26-86, one note below GM, kept as the ROM has
  it), a WAV dropped on it, or nothing. WAV files dropped on a row go to that note and the next ones; a
  file is turned into the driver's format (mono, 8 bits, PCM4's one rate since stage 19; RATE and AUTO
  stay in the project and do nothing, a project of 0.18 is prepared again from its sources when it
  opens) and kept in the project, up to the image's free room, about 1.9 MB. CLEAR
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
  last 4 ms), ONE-SHOT (Note Off doesn't cut it; a flag our ROM reads) and LOOP (stage 18: it repeats
  until Note Off, record flag 0x01 for the SGDK driver's own loop; the length is cut to whole 256-byte
  blocks and FADE is off, so the repeat has no gap; under one block it plays once; since stage 30 the loop's
  row is its only switch, D72). LOOP and ONE-SHOT
  exclude each other (stage 18; 22a's loop past Note Off, D53, is cancelled by D54). `dac_edit.stop`
  (stage 22b, D54): Note Off stops a loop that has a release; `dac_marks_of` then ends the bytes on the
  loop's last block (`tail` says a release exists, `release` that it plays, `frame_tail_end` where it
  ends), so the image is the one of a loop ending at END, which the ROM already cuts at Note Off; the marks
  stay. Switching LOOP or ONE-SHOT drops stop. The project's head byte is 0 cut, 1 one-shot, 2 loop with
  its release, 4 loop that stops; 3 (0.22.2's loop with one-shot) reads as 2. A WAV dropped on the open
  note's wave goes to that note. Loop marks (stage 18b, `docs/research/drv-pcm-loop-marks.md`): a row
  under the trim strip has the loop's own tabs; `dac_marks_of` snaps them to blocks from START, at least
  one block, and an intro only when start to loop end spans 5, 4, 3, 3, 2, 2 blocks by rate (the ROM moves
  on to the loop once a frame), else the loop starts at START. A loop end before the last whole block
  leaves a release, which FADE ends; at it or past it there is none and the length is cut to blocks as in
  18. The project's head ends with `loop_start` and `loop_end` in source frames, 0 is no marks (a 0.18
  project reads so). The image's records are 14 bytes apart; with marks the flag 0x04 and two u16 blocks
  follow at +10 and +12. Our ROM plays such a sample by writing the driver's params itself
  (`pcm_driver.c`, `midi_dac.c`): start to loop end with the loop flag, then in `midi_tick` once
  STATUS.0 says the driver copied them, the loop alone. The loop flags are the ROM's alone (stage 27, D70):
  after loading PCM4, `pcm_driver_keep_loop_bits` turns each `RES v,(STATUS+1)` of the driver's stop into a
  read of the same length and time, because an idle voice's stop ran every pass and could write back a flag
  the 68000 had just set or cleared. Note Off clears the
  loop flag and moves IWAVLEN to
  the release's end, checks it 600 us later and puts it right once. Without a release Note Off cuts. A
  press on the loop's body or a tab, here and in the PSG editor, changes nothing until it moves 3 px (the
  loop or the tab moves) or is let go (a click: the body off, or on from the shadow; the end tab or the
  row after it, `DACC_LOOP_TAIL` / `ENV_TAIL`, release or stop, stage 22b). A double click on the DAC's
  end tab undoes its click and drops the release, joined to the click's undo step
  (`files_edit_sample(join)`). Nothing changes while the
  mouse is down; on mouse up the sample is prepared once and goes into the running core. A double click puts a
  value back. The project keeps the file's sound (mono, 16 bits, up to 16 MB in all), the settings and
  the ROM's bytes, so an opened project plays the same bytes and the trim can still go back.
- Arpeggio rows (stage 20, D50, `docs/design/arpeggio.md`, `docs/research/arpeggio.md`): `patch_arp`
  (count 0-32, loop step or -1, the window's loop shadow, 32 shifts of ±24) belongs to each channel, each
  base and each place of both bank lists. Our ROM keeps two RAM tables of 128 rows (FM, PSG), stored by
  SysEx `0A 02` / `0A 03 <program> <loop|7F> <shift+64>... F7` (a clear is 0 shifts with 7F); a voice
  copies its program's row at Note On and steps it once a frame in `midi_tick` while the key is held, the
  shift added after bend and fine tune. A channel stays on its place's program number only if its row is
  the place's; otherwise it takes a service number. Each row goes at its first use, so a Preset step
  costs a Program Change as before. `files_draw_arp` hands a drawing like an envelope's; STORE
  (`files_store`) writes the channel's sound and row into its place, one undo step. Project block 10
  holds all rows; `.mdbank` the places', `.mdsnap` all. The row is dim (`patch_arp_works`) on channel 3
  in Ch3 Special with Ops (in One Note the ROM steps it and moves all four operators), channel 6 with the DAC
  and channel 10 off Tone 3. Stage 22b (D55): `patch_arp.off`, bit 1 of the row's third byte (bit 0 is the
  shadow), switches a row off without erasing it. `patch_arp_played` gives an off row as an empty one, and
  `patch_arp_same` and `sync_arp` see only that, so an off row compares as empty for the place's number, `*`
  and STORE, and the ROM gets 0 steps. `patch_arp_clean` keeps `off` only with steps. The flag travels with
  the row everywhere it goes; in the window it is `ARP_ON_OFF` at the row's right, one undo step.
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
  loops the whole envelope. LOOP and RELEASE name the loop and the tail after it. With STOP (stage 22b,
  D54) the channel's envelope ends at the loop's end; the release lives in the view as `state_tail`,
  project block 11 (stop, the steps, the envelope's name), and goes as the loop's shadow does. The editor's
  copy (`channel_shape`, `env_shape`) holds the release after the loop, drawn dim; `envelope_hand` hands
  the model what plays (`played_shape`), so the ROM, EEF, the bank and STORE get it. The loop switched off
  drops stop: its steps play as the envelope's. The ROM (`midi_psg_note_off`) releases a loop with steps
  after its end also from the intro and stops any other note at once; All Notes Off stops at once. The number at the axis' end is the length,
  set by a vertical drag, 124 bytes at most with the loop marks (amber MAX at the limit),
  and the columns share the lane, which reaches the page's right edge at every length: 3 or 4 px at 124 frames, wider when shorter (0.8.1). A drawing is the channel's
  own envelope, as a dropped EEF: the Preset and the bank stay, the next preset step wipes it, SAVE
  writes it as EEF, UNDO puts back the envelope before the last gesture with the loop shadow and kept
  release it had (stage 50: `envelope_take` keeps them, so UNDO of "stop to release" gives stop and its
  release back), and redo says `envelope as drawn`. The window never makes a
  loop the ROM would misread (`patch_shape_to_envelope`). It hands the drawing over at most once per
  window frame and at mouse up, and tells the host nothing (D19). While the longest envelope is
  re-stored every 33 ms, a note on another PSG channel sounds at worst 9.7 ms later than without
  drawing, against 18.3 ms for a note stored with its envelope (`reg_test`, one run).

- Window (stage 9, D29): every page puts the same things in the same places: number, name, source
  (`ROM N`, the place of the ROM bank, grey while it holds the factory sound and white once replaced, stage 24d; FILE over it for a dropped file; drag it to change the Preset), and
  the channel's cells VOL, PAN (L C R), TUNE, BEND, PORTA, TIME in one row, PSG keeping PAN's
  place empty (TIME pads 2 px, not 4, so 100-127 fit at size 2, and has its host dot top left: stage 49). A rail row shows the name, a `*` when the bank place's sound was tweaked, and a bar of
  16 segments: the channel's volume (CC7), and green while a note is held, CC7 x velocity / 127,
  grey after. The ROM itself has no velocity curve; the bar shows what the plugin sent. In Ch3
  Special Mode channel 3 is pink (`C_SP`, D62) everywhere FM's colours are: `fill` swaps FM's three for `C_SP_DARK`/`C_SP` while `g->sp` is set (its rail row, its page, its ALL row, its cells and the pitch editor), text on a pink fill is black (`on_fill`); with Ops it shows four notes in a column at the right, its own
  on top. Stage 24c: the bar is 16 segments of 9 px on every row (x 54-196); the note is at the right
  (x 202-225, 3 characters, `C--` for MIDI 0 since stage 30), and under it, right to left, `xN`, `n CUT` and `CHn`. The column (held green,
  let go grey, `---` none) also shows the DAC's four voices on channel 6's row, line k voice k, from
  `patch_dac_voice_state` (D59: `patch.c` copies `midi_dac.c`'s choice of voice and reckons each end from
  the playing image's record, `patch_set_dac_image`; `reg_test` `test_dac_shown` compares it with PCM4's
  STATUS | COMMAND in the Z80's RAM, `mdmi_engine_test_z80`: every change of a voice comes back in the
  driver after one delay within two ROM frames). A switch of Ch3 Special, Ch3 Notes or the DAC shows
  channel 3's and MIDI 11-13's held notes let go, a DAC switch channel 6's too, as the ROM releases them
  (`release_switched_notes` in `plugin.c`, before each moment's notes). The bank page's scroll track marks every
  channel's place.
- Double click (stage 24d): it sets a default wherever that means something, in the undo step of the click before it: knobs (the preset's value), the globals, an envelope column (level 0 dB, pitch 0, `envelope_reset`), an arp column (0, `arp_zero`), the DAC panel's cells and marks, ALL's VOL and PAN (the preset's value). Where the first click switched something (ARP ON/OFF, the ARP view, the arp loop's body and shadow, the PSG loop's strip and tabs, the release or stop of a PSG or DAC loop) the second does nothing more. Steps of 8 values or fewer step again on EDITOR; Preset, the FM graph's points, ALL's sound and the BANK lists take none.
- PSG arp row (stage 24d): the widget takes its width from `arp_w` (FM 282 in the graph's place, PSG 460 to the page's right edge); lane `w - 82`, right column at `w - 54`. On CH7-CH9 the noise's warning is one amber line over the envelope; on CH10 TYPE and RATE's caveat is in the footer over that row (`noise_hint`).
- Chord arrows (stage 24d): the spine, branches and heads sit at the names' height (y + 11), a head is a solid 5 x 10 triangle, clear of the bars.
- ALL's OUT and DAC row (stage 24e, D60, D61): `publish_waves` in `engine.c` copies each output's last 8192
  samples (the louder of left and right, stage 47) into a triple buffer after every core run while the window shows ALL
  (`mdmi_engine_watch_waves`, set by `render`); `draw_out` draws 2 periods of `lowest_key` (the channel's
  voices from the dealer, or its PSG note) from the rising zero crossing nearest a grid of whole periods on the outputs' clock (the same crest each frame, a chord too), at one scale (`OUT_FULL`),
  the noise 5 ms. The source cell moved to x 620. The DAC's row has VOL (bar in the DAC's dark colour) and PAN,
  and `draw_all_dac` draws the last struck note's sample (`note_sample`) with its loop on grey and a line per
  voice from `patch_dac_voice_place` (D59's copy; since stage 53 in the OUT box, below). Our ROM's `midi_dac_play` writes the voice's volume as
  velocity x (CC 7 + 1) >> 10 (`midi_dac_channel_volume`; `enable_dac` hands it channel 6's volume), not MDMI canon.
- ALL's DAC row (stage 53, D92, art-director, ui-concept.md) reads as the other rows. Where FM has its diagram
  (x 242-345), four squares 23 x 20, square k voice k as the ROM screen's DAC table: lit `C_DAC` with the key in
  black while the voice sounds, the key in orange with no fill once it played out (it kept its key: a one-shot run
  out, a loop's release ended), `---` dark grey when free (key 0: never struck, or let go and freed). Then VOL and
  PAN; channel 6's live output in the OUT box (the last 20 ms, no anchor, `OUT_FULL_DAC` 0.168 from
  `reg_test --peaks`, so full swing at volume 15 comes to the box's edge; silence is the frame and the middle
  line); the last sample's name in the source column (`sample_label`, cut to 14 letters by `text_right`, `FILE`
  above a file's, `---` before any note on MIDI 6 since the plugin opened or on a key with no sample). Bit 6 of
  the ALL mask (`DAC_PICTURE_BIT`, stage 39's switch; bits 0-5 are FM rows' envelopes) puts the last sample in the
  box instead: its loop on grey, the bytes at the wave's height, a line per sounding voice, green with black sides
  on its key, grey on another. A click on the box (or a double click: two) switches; the squares and the name only
  select the channel. `dac_voices_read` reads D59's copy of the four voices once a frame (`patch_dac_voice_state`,
  `patch_dac_voice_place`) for the squares and the lines; the footer over the squares reads it again, so for one
  frame it can disagree with them (review 53, parked). Footers: over the squares how many sound,
  over the box what it shows and what a click does, over the name the key and the whole name. The picture's
  columns are worked out from the bytes each frame: on the longest sample the plugin takes (2289920 bytes)
  `gui_test` prints about 1.6 ms a frame, under the stage's 2 ms for keeping them. A project of 0.41-0.44 saved
  with the live view opens on the picture.
- A lent voice in ALL's OUT box (stage 54, D93, art-director, ui-concept.md). While ALL is watched, `audio_batch_cb`
  also writes each FM voice 1-6 after its DC blocker (the louder side) into `voice_ring` at the core's rate, before
  any output or the resampler; `publish_waves` copies its last 8192 frames into `mdmi_waves.voices` with
  `voice_end` (core frames) and `voice_rate`. The output path (D46) is unchanged. In the window `out_source_of`
  picks what a row draws: its own output while `lowest_key` finds a note, else its voice while `voice_lent_to`
  (the rail's rule: the dealer's player is another channel and the row holds no note of its own) names the
  chord's channel, else nothing; `out_span` finds 2 periods on either clock (Fine Tune of the playing channel). A
  lent voice draws `C_PLAYING` over `C_PLAYING_DARK`; the footer says where it is heard
  (`VOICE 1 PLAYS CH5 E-3, HEARD ON CH5 OUT`). The player comes from the dealer, so for the first frame or two
  after a chord's Note On (until the ROM keys the voice on) the box shows the voice's ring as it was.
  `reg_test --cost` prints the worker's worst run and worst copy with ALL watched on a busy clip;
  `reg_test --first-frames` how long after the dealer names a lent voice the engine's owner changes (~30 ms
  as the window sees it). `clap_host` with `MDMI_TEST_WINDOW` opens each instance's window on ALL on a thread
  of its own, so the realtime runs copy the waves and voices as a host with the plugin open does.
- FM cursor (stage 24f): `time_psg_notes` in `plugin.c` also keeps each FM channel's last Note On and Note
  Off as sent (`fm_on`, `fm_off`, a chord's moved note too); `fm_cursor_seconds` gives the graph's time: held,
  up to Note Off at 1 s and parked there (OFF turns green); released, 1 s plus the time since; none from 2 s.
  The FM page draws a green line over the graph with a dot on the active curve, ALL's small envelopes a line.
- A `*` means the sound is tweaked against its base (D31): what the channel last got whole, its
  bank place or a file dropped on it. The base goes with the model and the project (block 8), undo
  gives it back, a drawing keeps it. An FM cell whose value differs from the base has an amber
  label and a mark where the base's value is; held, the footer says `TL 49, ROM 39`. A channel's
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
  channel 3's number on the page is pink in Ch3 Special (D62), as in the rail. On channel 3 (stage 21) each header says only
  OP1-OP4 and has a tag of the operator's pitch (`+7 +10`, or the note with FIX, filled pink); a click on
  it opens SEMI, FINE and FIX where the choice of SSG opens, one of the two at a time; any other click,
  another channel or Ch3 Special off closes it. The choice of SSG also closes when the channel changes
  without a click, as when a project's view comes in (stage 49, `ssg_channel`). Without Ch3 Special the tags are dim and take no mouse.

- Outputs (stage 17, D46): Main and Ch 1 to Ch 10, one stereo port each, in this order and with these ids for
  good (Live keeps routing by them). The patched BlastEm hands over each of the chip's ten voices (FM 1-6, the
  DAC on 6, PSG tones 1-3, noise) beside its mix, through the same lowpass and interpolation in float, and
  marks each 0x28 key-on. The dealer (`start_note`) tells the engine which MIDI channel a voice's note is
  for, queued with the note; the engine moves the voice to that channel's output at the voice's next key-on,
  so a release tail stays with its channel until another channel's note starts on the voice. A newer owner
  replaces one still waiting. The DAC's voice is channel 6's the moment the DAC switches on (the switch
  resets the YM2612), MIDI 11-13 notes in Ch3 Special Mode with Ops give voice 3 to channel 3. Each voice has its
  DC blocker before it goes to an output, so the YM2612's ladder offset of a silent voice doesn't step
  between outputs when the voice changes owner; one 20-channel resampler serves the outputs, and Main
  is their sum. The PSG's four voices go to their outputs 3 dB louder (x1.4125, D48), after the DC blocker;
  the check against BlastEm's mix takes the voices as the core made them. `reg_test` measures a PSG note
  on the outputs against BlastEm's own mix: +3 dB, FM 0 dB. The voices' sum differs from BlastEm's own
  int16 mix by up to about 14 steps of 16 bits after the DC blocker: BlastEm's lowpass truncates in its
  feedback. The worker runs with denormals flushed to zero. Emulator only: a console has one mixed output.

- Master Volume (stage 18a, id 9): tenths of dB, 0..720 is silence and -59.9..+12 dB, 600 is 0 dB. The
  plugin's, never sent to the ROM. `plugin.c` multiplies every output buffer, Main included, right after
  `mdmi_engine_render_outputs`, moving in a line across the block from the last block's factor, so Main
  stays the outputs' sum and a lane has no steps. No limiter. In the window MASTER sits at the right end
  of the strip of globals, on every page but ROM; the footer says the console ignores it, for RETRIGGER too
  (stage 49). A double click
  on any global cell puts its default back on every page.
  The strip's cells are as wide as what they show (stage 21): LFO, LFO RATE, LFO RESET, FILTER, CH3 SPECIAL
  (On in pink), CH3 NOTES (segments OPS, 1 NOTE and CSM, `docs/design/ch3-ops.md`), DAC, RETRIGGER, MASTER. LFO RESET is dim and
  takes no mouse with the LFO off, CH3 NOTES without Ch3 Special; a double click puts OPS and Off back,
  and 0 and Off in the pitch editor.

Toggling the DAC resets the YM2612 and the PSG, because the ROM swaps the Z80 driver: SGDK resets both chips,
and the Z80's reset line also clears the YM2612. Since stage 35 (D80) our ROM puts the chip back itself:
`synth_restore_chip` writes its shadow of channels 1-6 (every operator register, algorithm, feedback,
pan, AMS/FMS, the frequency) and the LFO under one take of the Z80's bus (~5 ms with the DAC on), and
`enable_dac` keys again the FM voices 1, 2, 4, 5 that still hold a note: they strike again at once,
from silence, with no new note (D39 left them mute until the next one). Channel 3's keys go off with any
switch, as before; channel 6 is not keyed (the DAC's, or after it its note was a sample's). The plugin
sends nothing but CC 78: its copy of the ROM stays true. The ROM also writes its PSG tones and volumes
again (D35).

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
| 81, 82 | Ch3 Notes (value * 3 / 128: Ops, One Note, CSM), LFO Reset (value * 7 / 128: Off, MIDI 1-6) | any |
| 102, 103, 104 | Semi (value - 64, ±48), Fine (value - 64, ±63 cents), Fixed (64-127 on) of channel 3's operator 4 on MIDI 3, of operators 1-3 on MIDI 11-13 | 3, 11-13 |
| 12, 13 | Noise Type (0-63 Periodic, 64-127 White), Noise Rate (value * 5 / 128: High, Mid, Low, Tone 3, Kit) | 10 |

Notes, pitch bend and other controllers pass to the ROM. CC 121 is turned into what the standard means by it, centre
bend and pedal up, because the ROM would also reset the channel's program and voices. CC 102-104 on channels other than 3 and 11-13 are dropped, as the ROM drops them. CC 87 and CC 84 are dropped: one would make a program number mean another sound for good, the other lets the ROM give any note any voice, past the plugin.

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
sh tools/install.sh <folder>
```

`run_tests.sh` also runs the ROM's own tests in Docker with `--privileged`: the image's address
sanitizer needs address randomization off. `fetch_deps.sh` clones `third_party/` (or unpacks `vendor/<name>-<commit>.tar.gz` where there is one, as in the source release) and applies `patches/`; it also puts the ROM's samples into `third_party/mdmi/res/samples`. The builds in Docker run without network. The build runs it too and refuses to start if
`third_party/` differs from `patches/`, so after changing code there, run `tools/save_patches.sh`. `run_tests.sh` exits with the number of failed checks. Plugins land in `build/cmake/out`.

`install.sh` copies them to the folder you name, checks the hashes, and writes
`BUILD.txt` there. It fails if Ableton Live has the VST3 locked. Live does not show plugin versions, so `BUILD.txt`
is how to tell what is installed.

`python tools/make_live_check.py` writes the Live set for the owner's check, `build/live-check/MDMI check
Project/`, from the owner's `ALS_tsts` set and `presets/vowels`: clips and automation on the arrangement, one
part per checked item, and a Russian guide next to it. Stages add their parts to the script.

The version is `0.N.M+revision`. Up to 0.24.8, N was the stage in `docs/ROADMAP.md` and M the delivery; from
stage 27 on, each stage simply takes the next version (0.24.9, 0.25.0, ...), not tied to its number. The revision
is `git describe`, ending in `-dirty` for uncommitted code.

## Release

`sh tools/make_release.sh v0.N.M` clones the tag into `dist/`, builds it, runs the tests and writes
`MDMI-Slot-0.N.M-win64.zip` and `MDMI-Slot-0.N.M-source.zip` with their SHA-256. The source
zip holds the tag's tree and, in `vendor/`, every dependency at its pinned commit and the ROM's samples,
so it builds without network apart from the Docker images (D42). Licences: see the README.
