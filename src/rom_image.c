#include <stdlib.h>
#include <string.h>
#include "rom_image.h"
#include "rom_map.h"

const double ROM_SAMPLE_HZ[ROM_SAMPLE_RATES] = {
	31460, 21600, 15820, 13235, 10880, 7930,
};

// A PcmSample in the image: the data's address, its length, the rate index and a pad byte, big
// endian like everything the 68000 reads. The table holds 128 addresses of such records.
#define RECORD_BYTES 10
#define RECORDS_AT ((ROM_USED_END + 1u) & ~1u)
#define DATA_AT ((RECORDS_AT + ROM_IMAGE_NOTES * RECORD_BYTES + ROM_SAMPLE_BLOCK - 1) & ~(ROM_SAMPLE_BLOCK - 1))

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

bool rom_image_check(const uint8_t *rom, size_t size)
{
	if (size < ROM_USED_END || size > ROM_IMAGE_MAX) {
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
	return true;
}

uint32_t rom_image_room(void)
{
	return ROM_IMAGE_MAX - DATA_AT;
}

uint32_t rom_image_cost(uint32_t length)
{
	return (length + ROM_SAMPLE_BLOCK - 1) & ~(ROM_SAMPLE_BLOCK - 1);
}

uint8_t *rom_image_build(const uint8_t *rom, size_t size, const rom_sample *const *samples, size_t *built)
{
	uint32_t end = DATA_AT;
	for (int note = 0; note < ROM_IMAGE_NOTES; note++) {
		if (samples[note]) {
			end += rom_image_cost(samples[note]->length);
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
	uint32_t record = RECORDS_AT, data = DATA_AT;
	for (int note = 0; note < ROM_IMAGE_NOTES; note++) {
		const rom_sample *sample = samples[note];
		if (!sample) {
			continue;
		}
		const uint32_t length = rom_image_cost(sample->length);
		memset(out + data, 0, length);
		memcpy(out + data, sample->bytes, sample->length);
		put32(out + record, data);
		put32(out + record + 4, length);
		out[record + 8] = sample->rate;
		out[record + 9] = 0;
		put32(out + ROM_PCM_TABLE + note * 4, record);
		record += RECORD_BYTES;
		data += length;
	}
	return out;
}
