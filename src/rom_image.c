#include <stdlib.h>
#include <string.h>
#include "rom_image.h"
#include "rom_map.h"
#include "presets_gm.h"

const double ROM_SAMPLE_HZ[ROM_SAMPLE_RATES] = {
	31460, 21600, 15820, 13235, 10880, 7930,
};

// A PcmSample in the image: the data's address, its length, the rate index and the flags (our ROM,
// D38), big endian like everything the 68000 reads; with ROM_SAMPLE_MARKS two u16 of loop blocks
// follow (stage 18b). The table holds 128 addresses of such records; ours are 14 bytes apart.
#define RECORD_BYTES 10
#define RECORD_ROOM 14
#define RECORDS_AT ((ROM_USED_END + 1u) & ~1u)
#define DATA_AT ((RECORDS_AT + ROM_IMAGE_NOTES * RECORD_ROOM + ROM_SAMPLE_BLOCK - 1) & ~(ROM_SAMPLE_BLOCK - 1))

static uint32_t get32(const uint8_t *at)
{
	return (uint32_t)at[0] << 24 | (uint32_t)at[1] << 16 | (uint32_t)at[2] << 8 | at[3];
}

static void put32(uint8_t *at, uint32_t value)
{
	at[0] = (uint8_t)(value >> 24);
	at[1] = (uint8_t)(value >> 16);
	at[2] = (uint8_t)(value >> 8);
	at[3] = (uint8_t)value;
}

// The record a note points at, 0 for none; false if it doesn't look like the driver's data.
static bool read_record(const uint8_t *rom, size_t size, int note, uint32_t *record)
{
	*record = get32(rom + ROM_PCM_TABLE + note * 4);
	if (!*record) {
		return true;
	}
	if (*record & 1 || *record + RECORD_BYTES > size) {
		return false;
	}
	const uint32_t data = get32(rom + *record), length = get32(rom + *record + 4);
	return data % ROM_SAMPLE_BLOCK == 0 && length && length % ROM_SAMPLE_BLOCK == 0 && data + length <= size
		&& rom[*record + 8] < ROM_SAMPLE_RATES;
}

void rom_image_pack_voice(const uint8_t *f, uint8_t *voice)
{
	// No field straddles a byte: algorithm 3, feedback 3, AMS 2; FMS 3; then per operator multiple 4
	// and detune 3; attack 5 and rate scaling 2; decay 5 and AM 1; sustain level 4; sustain rate 5;
	// release 4; total level 7; SSG 4. Checked against the build's GM voices by rom_image_check.
	voice[0] = (uint8_t)(f[0] << 5 | f[1] << 2 | f[2]);
	voice[1] = (uint8_t)(f[3] << 5);
	for (int op = 0; op < 4; op++) {
		const uint8_t *o = f + 4 + op * 11;
		uint8_t *v = voice + 2 + op * 8;
		v[0] = (uint8_t)(o[0] << 4 | o[1] << 1);
		v[1] = (uint8_t)(o[2] << 3 | o[3] << 1);
		v[2] = (uint8_t)(o[4] << 3 | o[5] << 2);
		v[3] = (uint8_t)(o[6] << 4);
		v[4] = (uint8_t)(o[7] << 3);
		v[5] = (uint8_t)(o[8] << 4);
		v[6] = (uint8_t)(o[9] << 1);
		v[7] = (uint8_t)(o[10] << 4);
	}
}

uint32_t rom_image_names_at(void)
{
	return ROM_PROGRAM_NAMES + 8u;
}

void rom_image_tables(uint32_t *voices, uint32_t *envelopes, uint32_t *rows)
{
	*voices = ROM_FM_PRESETS;
	*envelopes = ROM_ENVELOPES;
	*rows = ROM_ARP_ROWS;
}

#define ARP_ROW_BYTES (2 + ROM_ARP_STEPS)

// The bank's tables are where the build put them: 128 distinct voices equal to GM as the plugin
// packs it, envelope pointers inside the image, rows that are all empty, the names table's magic.
static bool tables_check(const uint8_t *rom, size_t size)
{
	if (ROM_FM_PRESETS + ROM_PROGRAMS * 4 > size || ROM_ENVELOPES + ROM_PROGRAMS * 4 > size
		|| ROM_ARP_ROWS + 2 * ROM_PROGRAMS * ARP_ROW_BYTES > size || rom_image_names_at() + sizeof(rom_names) > size
		|| memcmp(rom + ROM_PROGRAM_NAMES, "MDMINAME", 8)) {
		return false;
	}
	for (int program = 0; program < ROM_PROGRAMS; program++) {
		const uint32_t at = get32(rom + ROM_FM_PRESETS + program * 4);
		uint8_t voice[ROM_VOICE_BYTES];
		rom_image_pack_voice(PRESETS_GM[program], voice);
		if (at + ROM_VOICE_BYTES > size || memcmp(rom + at, voice, sizeof(voice))) {
			return false;
		}
		for (int other = 0; other < program; other++) {
			if (get32(rom + ROM_FM_PRESETS + other * 4) == at) {
				return false;
			}
		}
		if (get32(rom + ROM_ENVELOPES + program * 4) >= size) {
			return false;
		}
	}
	for (int row = 0; row < 2 * ROM_PROGRAMS; row++) {
		const uint8_t *at = rom + ROM_ARP_ROWS + row * ARP_ROW_BYTES;
		if (at[0] || at[1] != ROM_ARP_NO_LOOP) {
			return false;
		}
	}
	return true;
}

bool rom_image_check(const uint8_t *rom, size_t size)
{
	if (size < ROM_USED_END || size > ROM_IMAGE_MAX || !tables_check(rom, size)) {
		return false;
	}
	int samples = 0;
	for (int note = 0; note < ROM_IMAGE_NOTES; note++) {
		uint32_t record;
		if (!read_record(rom, size, note, &record)) {
			return false;
		}
		samples += record != 0;
	}
	return samples > 0;
}

bool rom_image_sample(const uint8_t *rom, size_t size, int note, rom_sample *sample)
{
	uint32_t record;
	if (note < 0 || note >= ROM_IMAGE_NOTES || !read_record(rom, size, note, &record) || !record) {
		return false;
	}
	sample->bytes = (const int8_t *)(rom + get32(rom + record));
	sample->length = get32(rom + record + 4);
	sample->rate = rom[record + 8];
	sample->flags = rom[record + 9];
	sample->loop_start = sample->loop_end = 0;
	if (sample->flags & ROM_SAMPLE_MARKS && record + RECORD_ROOM <= size) {
		sample->loop_start = (uint16_t)(rom[record + 10] << 8 | rom[record + 11]);
		sample->loop_end = (uint16_t)(rom[record + 12] << 8 | rom[record + 13]);
	}
	return true;
}

void rom_image_zone(uint32_t *table, uint32_t *table_ram, uint32_t *records)
{
	*table = ROM_PCM_TABLE;
	*table_ram = ROM_PCM_TABLE_RAM;
	*records = RECORDS_AT;
}

uint32_t rom_image_room(void)
{
	return ROM_IMAGE_MAX - DATA_AT;
}

uint32_t rom_image_cost(uint32_t length)
{
	return (length + ROM_SAMPLE_BLOCK - 1) & ~(ROM_SAMPLE_BLOCK - 1);
}

// An envelope's length with its 0xFF, 0 if it has none within the room.
static size_t envelope_length(const uint8_t *steps, size_t room)
{
	for (size_t i = 0; i < room; i++) {
		if (steps[i] == 0xFF) {
			return i + 1;
		}
	}
	return 0;
}

// Where an envelope already is in the image: where the ROM's table points if it is the same, else
// one written after the samples between from and to, else 0.
static uint32_t envelope_in(const uint8_t *image, size_t size, uint32_t from, uint32_t to, const uint8_t *steps, size_t length, uint32_t rom_at)
{
	if (rom_at + length <= size && !memcmp(image + rom_at, steps, length)) {
		return rom_at;
	}
	for (uint32_t at = from; at < to;) {
		const size_t have = envelope_length(image + at, to - at);
		if (have == length && !memcmp(image + at, steps, length)) {
			return at;
		}
		at += (uint32_t)have;
	}
	return 0;
}

// The voices over the build's GM ones, where they are; the envelopes the image doesn't hold from
// *end on; the rows in their table.
static bool write_bank(uint8_t *out, size_t size, uint32_t *end, const rom_bank *bank)
{
	for (int program = 0; program < ROM_PROGRAMS; program++) {
		rom_image_pack_voice(bank->voices[program], out + get32(out + ROM_FM_PRESETS + program * 4));
	}
	const uint32_t from = *end;
	for (int program = 0; program < ROM_PROGRAMS; program++) {
		const uint8_t *steps = bank->envelopes[program];
		const size_t length = envelope_length(steps, ROM_ENVELOPE_BYTES);
		if (!length) {
			return false;
		}
		uint32_t at = envelope_in(out, size, from, *end, steps, length, get32(out + ROM_ENVELOPES + program * 4));
		if (!at) {
			if (*end + length > size) {
				return false;
			}
			at = *end;
			memcpy(out + at, steps, length);
			*end += (uint32_t)length;
		}
		put32(out + ROM_ENVELOPES + program * 4, at);
	}
	for (int type = 0; type < 2; type++) {
		for (int program = 0; program < ROM_PROGRAMS; program++) {
			const rom_arp *arp = &bank->arps[type][program];
			uint8_t *row = out + ROM_ARP_ROWS + (type * ROM_PROGRAMS + program) * ARP_ROW_BYTES;
			row[0] = arp->count;
			row[1] = arp->loop;
			memcpy(row + 2, arp->steps, ROM_ARP_STEPS);
		}
	}
	return true;
}

uint8_t *rom_image_build(const uint8_t *rom, size_t size, const rom_sample *const *samples, const bool *removed, const rom_bank *bank, const rom_names *names, size_t *built)
{
	uint32_t end = DATA_AT;
	for (int note = 0; note < ROM_IMAGE_NOTES; note++) {
		if (samples[note]) {
			end += rom_image_cost(samples[note]->length);
		}
	}
	const uint32_t samples_end = end;
	if (bank) {
		// Room for the most the envelopes can take after the samples, cut to what is left.
		end += ROM_PROGRAMS * ROM_ENVELOPE_BYTES;
		if (end > ROM_IMAGE_MAX && samples_end <= ROM_IMAGE_MAX) {
			end = ROM_IMAGE_MAX;
		}
	}
	// An image that isn't the one the plugin was built for is never patched blind.
	if (end > ROM_IMAGE_MAX || !rom_image_check(rom, size)) {
		return NULL;
	}
	*built = end > size ? end : size;
	uint8_t *out = calloc(1, *built);
	if (!out) {
		return NULL;
	}
	memcpy(out, rom, size);
	uint32_t data = DATA_AT;
	for (int note = 0; note < ROM_IMAGE_NOTES; note++) {
		const rom_sample *sample = samples[note];
		// Every note's record has its own place, so the ROM's pointer to a sounding note's record
		// still means that note after another note changed (D39).
		const uint32_t record = RECORDS_AT + (uint32_t)note * RECORD_ROOM;
		if (!sample) {
			if (removed && removed[note]) {
				put32(out + ROM_PCM_TABLE + note * 4, 0);
			}
			continue;
		}
		const uint32_t length = rom_image_cost(sample->length);
		memset(out + data, 0, length);
		memcpy(out + data, sample->bytes, sample->length);
		put32(out + record, data);
		put32(out + record + 4, length);
		out[record + 8] = sample->rate;
		out[record + 9] = sample->flags;
		if (sample->flags & ROM_SAMPLE_MARKS) {
			out[record + 10] = (uint8_t)(sample->loop_start >> 8);
			out[record + 11] = (uint8_t)sample->loop_start;
			out[record + 12] = (uint8_t)(sample->loop_end >> 8);
			out[record + 13] = (uint8_t)sample->loop_end;
		}
		put32(out + ROM_PCM_TABLE + note * 4, record);
		data += length;
	}
	if (names) {
		memcpy(out + rom_image_names_at(), names, sizeof(*names));
	}
	if (bank) {
		uint32_t used = samples_end;
		if (!write_bank(out, *built, &used, bank)) {
			free(out);
			return NULL;
		}
		// The room the envelopes didn't take goes; the image ends on a whole block, as samples do.
		used = rom_image_cost(used);
		*built = used > size ? used : size;
	}
	return out;
}

#define SIGNATURE_AT (ROM_PROGRAM_NAMES + 8u)
#define CHECKSUM_AT 0x18Eu

// The window reckons it on its thread whenever the bank changes, so a byte at a time by a table.
static uint16_t crc_step(uint16_t crc, uint8_t byte)
{
	static uint16_t table[256];
	if (!table[1]) {
		for (int value = 0; value < 256; value++) {
			uint16_t c = (uint16_t)(value << 8);
			for (int bit = 0; bit < 8; bit++) {
				c = (uint16_t)(c & 0x8000 ? c << 1 ^ 0x1021 : c << 1);
			}
			table[value] = c;
		}
	}
	return (uint16_t)(crc << 8 ^ table[(crc >> 8 ^ byte) & 0xFF]);
}

uint16_t rom_image_signature(const uint8_t *image, size_t size)
{
	uint16_t crc = 0xFFFF;
	for (size_t i = 0; i < size; i++) {
		if ((i >= SIGNATURE_AT && i < SIGNATURE_AT + ROM_SIGNATURE_BYTES) || i == CHECKSUM_AT || i == CHECKSUM_AT + 1) {
			continue;
		}
		crc = crc_step(crc, image[i]);
	}
	return crc;
}

#define SD_ALIGN (512u << 10)

bool rom_image_for_sd(uint8_t **image, size_t *size)
{
	const size_t padded = (*size + SD_ALIGN - 1) / SD_ALIGN * SD_ALIGN;
	uint8_t *out = realloc(*image, padded);
	if (!out) {
		return false;
	}
	memset(out + *size, 0, padded - *size);
	// The header's sum of the words from 0x200, which nothing is known to check (hardware-specialist,
	// stage 24); written because it costs nothing.
	uint16_t sum = 0;
	for (size_t i = 0x200; i + 1 < padded; i += 2) {
		sum = (uint16_t)(sum + (out[i] << 8 | out[i + 1]));
	}
	out[CHECKSUM_AT] = (uint8_t)(sum >> 8);
	out[CHECKSUM_AT + 1] = (uint8_t)sum;
	*image = out;
	*size = padded;
	return true;
}
