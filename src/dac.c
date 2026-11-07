#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dac.h"

// Taps on each side of the resampling filter, for the input rate or the output's if lower.
#define SINC_SIDE 16
#define PI 3.14159265358979
// A file a little above a driver rate still takes it: 22050 Hz goes to the 21600 the driver plays.
#define RATE_SLACK 0.97

static uint32_t le32(const uint8_t *at)
{
	return (uint32_t)at[0] | (uint32_t)at[1] << 8 | (uint32_t)at[2] << 16 | (uint32_t)at[3] << 24;
}

static uint16_t le16(const uint8_t *at)
{
	return (uint16_t)(at[0] | at[1] << 8);
}

// One frame of the file as a mono value from -1 to 1.
static double frame_value(const uint8_t *frame, int channels, int bits, bool is_float)
{
	const int bytes = bits / 8;
	double sum = 0;
	for (int c = 0; c < channels; c++) {
		const uint8_t *at = frame + c * bytes;
		double v;
		if (is_float) {
			float f;
			memcpy(&f, at, sizeof(f));
			v = f;
		} else if (bits == 8) {
			v = (at[0] - 128) / 128.0;
		} else if (bits == 16) {
			v = (int16_t)le16(at) / 32768.0;
		} else if (bits == 24) {
			v = (double)((int32_t)((uint32_t)at[0] << 8 | (uint32_t)at[1] << 16 | (uint32_t)at[2] << 24) >> 8) / 8388608.0;
		} else {
			v = (int32_t)le32(at) / 2147483648.0;
		}
		sum += v;
	}
	return sum / channels;
}

static int rate_for(double file_hz)
{
	for (int i = ROM_SAMPLE_RATES - 1; i > 0; i--) {
		if (ROM_SAMPLE_HZ[i] >= file_hz * RATE_SLACK) {
			return i;
		}
	}
	return 0;
}

dac_sample *dac_from_wav(const uint8_t *wav, size_t size, const char *name, const char **error)
{
	*error = "not a WAV file";
	if (size < 12 || memcmp(wav, "RIFF", 4) || memcmp(wav + 8, "WAVE", 4)) {
		return NULL;
	}
	const uint8_t *fmt = NULL, *data = NULL;
	uint32_t data_size = 0;
	for (size_t at = 12; at + 8 <= size;) {
		const uint32_t length = le32(wav + at + 4);
		const size_t body = at + 8;
		if (length > size - body) {
			// A truncated last chunk: take what is there.
			if (!memcmp(wav + at, "data", 4)) {
				data = wav + body;
				data_size = (uint32_t)(size - body);
			}
			break;
		}
		if (!memcmp(wav + at, "fmt ", 4) && length >= 16) {
			fmt = wav + body;
		} else if (!memcmp(wav + at, "data", 4)) {
			data = wav + body;
			data_size = length;
		}
		at = body + length + (length & 1);
	}
	if (!fmt || !data) {
		return NULL;
	}
	uint16_t format = le16(fmt);
	const int channels = le16(fmt + 2), bits = le16(fmt + 14);
	const uint32_t file_hz = le32(fmt + 4);
	if (format == 0xFFFE && le16(fmt - 4) >= 40) {
		// WAVE_FORMAT_EXTENSIBLE: the first two bytes of the sub-format GUID are the format.
		format = le16(fmt + 24);
	}
	const bool is_float = format == 3;
	*error = "only PCM or float WAV of 8, 16, 24 or 32 bits";
	if ((format != 1 && !is_float) || channels < 1 || !file_hz || (is_float ? bits != 32 : bits != 8 && bits != 16 && bits != 24 && bits != 32)) {
		return NULL;
	}
	const size_t frame_bytes = (size_t)channels * bits / 8;
	const size_t frames = data_size / frame_bytes;
	*error = "the WAV file is empty";
	if (!frames) {
		return NULL;
	}
	const int rate = rate_for(file_hz);
	const double step = file_hz / ROM_SAMPLE_HZ[rate];
	const size_t length = (size_t)ceil(frames / step);
	*error = "the sound is longer than the ROM has room for";
	if (rom_image_cost((uint32_t)(length < ROM_IMAGE_MAX ? length : ROM_IMAGE_MAX)) > rom_image_room()) {
		return NULL;
	}
	*error = "out of memory";
	double *in = malloc(frames * sizeof(double));
	dac_sample *out = calloc(1, sizeof(dac_sample) + rom_image_cost((uint32_t)length));
	if (!in || !out) {
		free(in);
		free(out);
		return NULL;
	}
	for (size_t i = 0; i < frames; i++) {
		in[i] = frame_value(data + i * frame_bytes, channels, bits, is_float);
	}
	// Windowed sinc, its cutoff below the lower of the two rates' Nyquist.
	const double cutoff = step > 1 ? 0.95 / step : 1.0;
	const int side = (int)ceil(SINC_SIDE * (step > 1 ? step : 1));
	for (size_t k = 0; k < length; k++) {
		const double t = k * step;
		const long centre = (long)floor(t);
		double sum = 0;
		for (long n = centre - side + 1; n <= centre + side; n++) {
			if (n < 0 || n >= (long)frames) {
				continue;
			}
			const double d = t - n;
			const double x = PI * cutoff * d;
			const double sinc = fabs(x) < 1e-9 ? 1.0 : sin(x) / x;
			const double window = 0.5 + 0.5 * cos(PI * d / side);
			sum += in[n] * cutoff * sinc * window;
		}
		const long v = lround(sum * 128);
		out->bytes[k] = (int8_t)(v > 127 ? 127 : v < -128 ? -128 : v);
	}
	free(in);
	snprintf(out->name, sizeof(out->name), "%s", name);
	out->sample.rate = (uint8_t)rate;
	out->sample.length = rom_image_cost((uint32_t)length);
	out->sample.bytes = out->bytes;
	*error = NULL;
	return out;
}

dac_sample *dac_copy(const dac_sample *sample)
{
	if (!sample) {
		return NULL;
	}
	dac_sample *copy = malloc(sizeof(dac_sample) + sample->sample.length);
	if (copy) {
		memcpy(copy, sample, sizeof(dac_sample) + sample->sample.length);
		copy->sample.bytes = copy->bytes;
	}
	return copy;
}

void dac_clear(dac_set *set)
{
	for (int note = 0; note < ROM_IMAGE_NOTES; note++) {
		free(set->notes[note]);
		set->notes[note] = NULL;
	}
}

void dac_put(dac_set *set, int note, dac_sample *sample)
{
	free(set->notes[note]);
	set->notes[note] = sample;
	set->version++;
}

void dac_take(dac_set *to, dac_set *from)
{
	if (dac_same(to, from)) {
		dac_clear(from);
		return;
	}
	dac_clear(to);
	memcpy(to->notes, from->notes, sizeof(to->notes));
	memset(from->notes, 0, sizeof(from->notes));
	to->version++;
}

uint32_t dac_used(const dac_set *set)
{
	uint32_t used = 0;
	for (int note = 0; note < ROM_IMAGE_NOTES; note++) {
		if (set->notes[note]) {
			used += rom_image_cost(set->notes[note]->sample.length);
		}
	}
	return used;
}

bool dac_same(const dac_set *a, const dac_set *b)
{
	for (int note = 0; note < ROM_IMAGE_NOTES; note++) {
		const dac_sample *x = a->notes[note], *y = b->notes[note];
		if (!x != !y) {
			return false;
		}
		if (x && (x->sample.rate != y->sample.rate || x->sample.length != y->sample.length || strcmp(x->name, y->name) || memcmp(x->bytes, y->bytes, x->sample.length))) {
			return false;
		}
	}
	return true;
}

uint8_t *dac_image(const dac_set *set, const uint8_t *rom, size_t size, size_t *built)
{
	const rom_sample *samples[ROM_IMAGE_NOTES];
	for (int note = 0; note < ROM_IMAGE_NOTES; note++) {
		samples[note] = set->notes[note] ? &set->notes[note]->sample : NULL;
	}
	return rom_image_build(rom, size, samples, built);
}

#define SAMPLE_HEAD (1 + 1 + 4 + DAC_NAME)

size_t dac_block_size(const dac_set *set)
{
	size_t size = 0;
	for (int note = 0; note < ROM_IMAGE_NOTES; note++) {
		if (set->notes[note]) {
			size += SAMPLE_HEAD + set->notes[note]->sample.length;
		}
	}
	return size;
}

void dac_block_write(const dac_set *set, uint8_t *out)
{
	for (int note = 0; note < ROM_IMAGE_NOTES; note++) {
		const dac_sample *s = set->notes[note];
		if (!s) {
			continue;
		}
		out[0] = (uint8_t)note;
		out[1] = s->sample.rate;
		memcpy(out + 2, &s->sample.length, 4);
		memcpy(out + 6, s->name, DAC_NAME);
		memcpy(out + SAMPLE_HEAD, s->bytes, s->sample.length);
		out += SAMPLE_HEAD + s->sample.length;
	}
}

bool dac_block_read(dac_set *set, const uint8_t *block, size_t length)
{
	dac_clear(set);
	uint32_t used = 0;
	for (size_t at = 0; at < length;) {
		uint32_t bytes;
		if (length - at < SAMPLE_HEAD) {
			break;
		}
		memcpy(&bytes, block + at + 2, 4);
		const int note = block[at];
		used += bytes;
		if (note >= ROM_IMAGE_NOTES || block[at + 1] >= ROM_SAMPLE_RATES || !bytes || bytes % ROM_SAMPLE_BLOCK || bytes > length - at - SAMPLE_HEAD || used > rom_image_room()) {
			dac_clear(set);
			return false;
		}
		dac_sample *s = malloc(sizeof(dac_sample) + bytes);
		if (!s) {
			dac_clear(set);
			return false;
		}
		memcpy(s->name, block + at + 6, DAC_NAME);
		s->name[DAC_NAME - 1] = '\0';
		s->sample.rate = block[at + 1];
		s->sample.length = bytes;
		memcpy(s->bytes, block + at + SAMPLE_HEAD, bytes);
		s->sample.bytes = s->bytes;
		dac_put(set, note, s);
		at += SAMPLE_HEAD + bytes;
	}
	return true;
}
