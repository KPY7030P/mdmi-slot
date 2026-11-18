#ifndef MDMI_ROM_IMAGE_H
#define MDMI_ROM_IMAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The ROM image the core boots, patched with the plugin's DAC samples (stage 12, D36). Where the
// sample table is comes from the ROM build (tools/gen_rom_map.py). New samples go after the used
// part of the image, up to 4 MB; the ROM's own stay where they are. No threads.
#define ROM_IMAGE_NOTES 128
#define ROM_IMAGE_MAX (4u << 20)
#define ROM_SAMPLE_BLOCK 256u
#define ROM_SAMPLE_RATES 6

// The driver's rates by index as BlastEm plays them, measured by reg_test with the ROM idle. SGDK
// names them 32000, 22050, 16000, 13400, 11025 and 8000; its cycle counts give 3579545 Hz over 110,
// 161, 221, 264, 321 and 443 (hardware-specialist, drv_pcm.s80), 2-3% above what the emulator plays.
// The console's own rates are still to be measured (ROADMAP).
extern const double ROM_SAMPLE_HZ[ROM_SAMPLE_RATES];
// Stage 19 (D49): PCM4 plays every sample at one rate, its four voices mixed. This is the rate
// BlastEm plays, measured by reg_test (15207-15266 over six notes); SGDK's cycles give 3579545 / 223
// = 16052 Hz on a console. The rates above only read projects of 0.18 and earlier; the record's rate
// byte stays and the ROM doesn't read it.
#define ROM_PCM4_HZ 15236.0

// Our ROM's flags in a sample's record: ONE_SHOT, Note Off doesn't stop it (D38); LOOP, it repeats
// until Note Off (stage 18).
#define ROM_SAMPLE_LOOP 0x01
#define ROM_SAMPLE_ONE_SHOT 0x02
// MARKS (stage 18b): the loop runs between two blocks, what follows it plays after Note Off. The
// record then has the two block numbers at +10 and +12; records are 14 bytes apart for that.
#define ROM_SAMPLE_MARKS 0x04

// Signed 8-bit samples at a rate index, the length a whole number of blocks, and the record's flags.
typedef struct {
	uint8_t rate;
	uint32_t length;
	const int8_t *bytes;
	uint8_t flags;
	// With ROM_SAMPLE_MARKS, the loop's blocks [loop_start, loop_end).
	uint16_t loop_start;
	uint16_t loop_end;
} rom_sample;

// Whether the image is the one the plugin was built for: its sample table, its 128 GM voices packed
// as the plugin packs PRESETS_GM, empty arp rows and the names table (stage 24).
bool rom_image_check(const uint8_t *rom, size_t size);
// The ROM's own sample on a note, false if the note has none.
bool rom_image_sample(const uint8_t *rom, size_t size, int note, rom_sample *sample);
// Where the samples are (D39): the table in the image and in the 68000's RAM (offset from 0xFF0000),
// 4 bytes a note, and from records on to ROM_IMAGE_MAX the records and data. An image that differs
// from the running one only there can be written into it.
#define ROM_IMAGE_TABLE_BYTES (ROM_IMAGE_NOTES * 4)
void rom_image_zone(uint32_t *table, uint32_t *table_ram, uint32_t *records);
// The room for new samples and what one takes of it.
// The room for samples a change may fill (rom_image_room), and the whole space after the records that
// a project's samples are read against (stage 37).
uint32_t rom_image_room(void);
uint32_t rom_image_space(void);
uint32_t rom_image_cost(uint32_t length);
// Stage 24: the bank as the exported image holds it, in the fork's tables (tools/gen_rom_map.py).
// A voice in the layout of PRESETS_GM: algorithm, feedback, AMS, FMS, then per operator multiple,
// detune, attack, rate scaling, decay, AM, sustain level, sustain rate, release, total level, SSG.
#define ROM_VOICE_FIELDS 48
#define ROM_VOICE_BYTES 34
// An envelope's EEF bytes up to and with its 0xFF.
#define ROM_ENVELOPE_BYTES 125
#define ROM_ARP_STEPS 32
#define ROM_ARP_NO_LOOP 0x7F
#define ROM_PROGRAMS 128
typedef struct {
	uint8_t count;
	// The step the loop starts at, ROM_ARP_NO_LOOP for none.
	uint8_t loop;
	int8_t steps[ROM_ARP_STEPS];
} rom_arp;
typedef struct {
	uint8_t voices[ROM_PROGRAMS][ROM_VOICE_FIELDS];
	uint8_t envelopes[ROM_PROGRAMS][ROM_ENVELOPE_BYTES];
	// FM's rows, then PSG's.
	rom_arp arps[2][ROM_PROGRAMS];
} rom_bank;
// The names the ROM's screen shows, ASCII ending in 0, and the header's signature (fork's
// program_names.h).
#define ROM_NAME_BYTES 24
#define ROM_SIGNATURE_BYTES 12
typedef struct {
	char signature[ROM_SIGNATURE_BYTES];
	char names[2][ROM_PROGRAMS][ROM_NAME_BYTES];
} rom_names;
// Where a rom_names lies in the image, as it is: the emulator gets them written into its cartridge.
uint32_t rom_image_names_at(void);
// Where the bank's tables are: 128 pointers to the FM voices and 128 to the envelopes, the 256 rows
// of 2 + ROM_ARP_STEPS bytes, FM's first.
void rom_image_tables(uint32_t *voices, uint32_t *envelopes, uint32_t *rows);

// The ROM's FM voice packed as the 68000's compiler packs FmPreset's bit fields.
void rom_image_pack_voice(const uint8_t *fields, uint8_t *voice);

// A copy of the image where each note with a sample plays it; NULL notes keep the ROM's, unless
// removed (may be NULL) says the note is empty. With a bank, its voices, envelopes and rows replace
// the ROM's (an envelope the image doesn't hold goes after the samples); with names, they and the
// signature go into the table. NULL if they don't fit, the image fails rom_image_check, or out of
// memory. Freed by the caller.
uint8_t *rom_image_build(const uint8_t *rom, size_t size, const rom_sample *const *samples, const bool *removed, const rom_bank *bank, const rom_names *names, size_t *built);
// A CRC-16 (CCITT) of an image, past the signature and the header's checksum, so the same bank and
// samples give the same value with any signature in it.
uint16_t rom_image_signature(const uint8_t *image, size_t size);
// What the file for the SD card needs on top: zeros up to a whole 512 KB, as SGDK's build pads, and
// the header's checksum. False if out of memory; *image may move.
bool rom_image_for_sd(uint8_t **image, size_t *size);

#endif
