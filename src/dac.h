#ifndef MDMI_DAC_H
#define MDMI_DAC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "rom_image.h"

// The DAC samples the plugin puts on notes in place of the ROM's (stage 12, D36, D38): the sound of
// the file they came from, how it is prepared, and the bytes the ROM plays, made from the two. The
// project keeps all three. Main thread only.
#define DAC_NAME 32
// What a project may hold of sources in all (D38), checked in Live.
#define DAC_SOURCE_MAX (16u << 20)
#define DAC_PITCH_MAX 2400
#define DAC_GAIN_MAX 240
#define DAC_RATE_AUTO (-1)

// How a sample is prepared from its source before it goes to the ROM.
typedef struct {
	// The source frames kept, [start, end).
	uint32_t start;
	uint32_t end;
	// Cents, +-DAC_PITCH_MAX, by resampling: up is shorter.
	int16_t pitch;
	// Tenths of a dB, +-DAC_GAIN_MAX.
	int16_t gain;
	// A driver rate index, or DAC_RATE_AUTO: the lowest that keeps the file's.
	int8_t rate;
	// A few ms fade to silence at the end.
	bool fade;
	// Note Off doesn't stop it (ROM_SAMPLE_ONE_SHOT).
	bool one_shot;
} dac_edit;

typedef struct {
	char name[DAC_NAME];
	dac_edit edit;
	// The file's sound: mono, 16 bits, at its own rate. A sample made otherwise (tests) may have none,
	// and then can't be prepared again.
	uint32_t source_hz;
	uint32_t source_frames;
	int16_t *source;
	// What the ROM plays, and whether the gain clipped it.
	rom_sample sample;
	int8_t *bytes;
	bool clipped;
} dac_sample;

typedef struct {
	// NULL: the note plays the ROM's sample.
	dac_sample *notes[ROM_IMAGE_NOTES];
	// Goes up with every change, so the plugin knows when to build the image again.
	uint32_t version;
} dac_set;

// A WAV file's sound as a sample, prepared the default way: all of it, the rate by the file's, a
// short fade. NULL and a reason if the file can't be read or doesn't fit the ROM.
dac_sample *dac_from_wav(const uint8_t *wav, size_t size, const char *name, const char **error);
// Makes the bytes from the source and edit again. False, the sample unchanged, if they don't fit
// the ROM or out of memory.
bool dac_prepare(dac_sample *sample, const dac_edit *edit);
// The gain that brings the kept part's peak just under full scale.
int dac_normalize_gain(const dac_sample *sample);
dac_sample *dac_copy(const dac_sample *sample);
void dac_free(dac_sample *sample);
void dac_clear(dac_set *set);
// Puts a sample on a note, NULL for the ROM's; the set owns it from then on.
void dac_put(dac_set *set, int note, dac_sample *sample);
// What the set's samples take of the image's room, and of the project's room for sources.
uint32_t dac_used(const dac_set *set);
uint32_t dac_sources(const dac_set *set);
bool dac_same(const dac_set *a, const dac_set *b);
// Moves the samples of from into to if they differ, else frees them; from is empty after.
void dac_take(dac_set *to, dac_set *from);
// The image the core boots with these samples. NULL if they don't fit.
uint8_t *dac_image(const dac_set *set, const uint8_t *rom, size_t size, size_t *built);

// The project's block (D38): per sample its note, the length of its head, the head (name, source
// format, edit, the bytes' rate, flags and length), then the source and the bytes. A head longer
// than known is skipped past, a shorter one leaves the rest at defaults.
size_t dac_block_size(const dac_set *set);
void dac_block_write(const dac_set *set, uint8_t *out);
// False if the block is damaged; the set is then empty.
bool dac_block_read(dac_set *set, const uint8_t *block, size_t length);

#endif
