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

// Signed 8-bit samples at a rate index, the length a whole number of blocks.
typedef struct {
	uint8_t rate;
	uint32_t length;
	const int8_t *bytes;
} rom_sample;

// Whether the image has the sample table the plugin was built for.
bool rom_image_check(const uint8_t *rom, size_t size);
// The ROM's own sample on a note, false if the note has none.
bool rom_image_sample(const uint8_t *rom, size_t size, int note, rom_sample *sample);
// The room for new samples and what one takes of it.
uint32_t rom_image_room(void);
uint32_t rom_image_cost(uint32_t length);
// A copy of the image where each note with a sample plays it; NULL notes keep the ROM's. NULL if
// they don't fit, the image fails rom_image_check, or out of memory. Freed by the caller.
uint8_t *rom_image_build(const uint8_t *rom, size_t size, const rom_sample *const *samples, size_t *built);

#endif
