#ifndef MDMI_DAC_H
#define MDMI_DAC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "rom_image.h"

// The DAC samples the plugin puts on notes in place of the ROM's (stage 12, D36): already in the
// ROM's format, with the name of the file they came from. The project keeps them. Main thread only.
#define DAC_NAME 32

typedef struct {
	char name[DAC_NAME];
	rom_sample sample;
	int8_t bytes[];
} dac_sample;

typedef struct {
	// NULL: the note plays the ROM's sample.
	dac_sample *notes[ROM_IMAGE_NOTES];
	// Goes up with every change, so the plugin knows when to build the image again.
	uint32_t version;
} dac_set;

// A WAV file as the driver plays it: mono, signed 8 bits, resampled to the lowest of its rates that
// keeps the file's, at most 32 kHz, padded with silence to whole blocks. NULL and a reason if the
// file can't be read.
dac_sample *dac_from_wav(const uint8_t *wav, size_t size, const char *name, const char **error);
dac_sample *dac_copy(const dac_sample *sample);
void dac_clear(dac_set *set);
// Puts a sample on a note, NULL for the ROM's; the set owns it from then on.
void dac_put(dac_set *set, int note, dac_sample *sample);
// What the set's samples take of the image's room.
uint32_t dac_used(const dac_set *set);
bool dac_same(const dac_set *a, const dac_set *b);
// Moves the samples of from into to if they differ, else frees them; from is empty after.
void dac_take(dac_set *to, dac_set *from);
// The image the core boots with these samples. NULL if they don't fit.
uint8_t *dac_image(const dac_set *set, const uint8_t *rom, size_t size, size_t *built);

// The project's block (D36): per sample its note, rate, length and name, then its bytes.
size_t dac_block_size(const dac_set *set);
void dac_block_write(const dac_set *set, uint8_t *out);
// False if the block is damaged; the set is then empty.
bool dac_block_read(dac_set *set, const uint8_t *block, size_t length);

#endif
