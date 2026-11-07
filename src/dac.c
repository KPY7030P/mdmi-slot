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
// The fade at the end, so the last sample does not click against the silence after it.
#define FADE_SECONDS 0.004

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


// The resampling filter, windowed sinc with its cutoff below the lower of the two rates' Nyquist,
// tabled at SINC_PHASES fractions of a source frame and read between them: the window's thread
// prepares a minute of sound in a fraction of a second (review of D38).
#define SINC_PHASES 256

static double kernel(double d, double cutoff, int side)
{
	if (fabs(d) >= side) {
		return 0;
	}
	const double x = PI * cutoff * d;
	const double sinc = fabs(x) < 1e-9 ? 1.0 : sin(x) / x;
	return cutoff * sinc * (0.5 + 0.5 * cos(PI * d / side));
}

// table[phase * taps + j] weighs source frame centre - side + 1 + j for an output at centre + phase / SINC_PHASES.
static double *sinc_table(double cutoff, int side)
{
	const int taps = 2 * side;
	double *table = malloc((size_t)(SINC_PHASES + 1) * taps * sizeof(double));
	for (int phase = 0; table && phase <= SINC_PHASES; phase++) {
		for (int j = 0; j < taps; j++) {
			table[phase * taps + j] = kernel((double)phase / SINC_PHASES + side - 1 - j, cutoff, side);
		}
	}
	return table;
}

static double source_at(const int16_t *in, long frames, double t, const double *table, int side)
{
	const long centre = (long)floor(t);
	const double where = (t - centre) * SINC_PHASES;
	const int phase = (int)where;
	const double between = where - phase;
	const int taps = 2 * side;
	const double *a = table + phase * taps, *b = a + taps;
	double sum = 0;
	for (int j = 0; j < taps; j++) {
		const long n = centre - side + 1 + j;
		if (n >= 0 && n < frames) {
			sum += in[n] * (a[j] + (b[j] - a[j]) * between);
		}
	}
	return sum / 32768.0;
}

static int rate_of(const dac_sample *sample, const dac_edit *edit)
{
	return edit->rate == DAC_RATE_AUTO ? rate_for(sample->source_hz) : edit->rate;
}

int dac_rate(const dac_sample *sample, const dac_edit *edit)
{
	return rate_of(sample, edit);
}

uint32_t dac_cost(const dac_sample *sample, const dac_edit *edit)
{
	const int rate = rate_of(sample, edit);
	const double step = sample->source_hz * pow(2.0, edit->pitch / 1200.0) / ROM_SAMPLE_HZ[rate];
	const double length = ceil((edit->end > edit->start ? edit->end - edit->start : 1) / step);
	return rom_image_cost(length < ROM_IMAGE_MAX ? (uint32_t)length : ROM_IMAGE_MAX);
}

bool dac_prepare(dac_sample *sample, const dac_edit *edit)
{
	if (!sample->source || edit->start >= edit->end || edit->end > sample->source_frames || edit->rate < DAC_RATE_AUTO || edit->rate >= ROM_SAMPLE_RATES) {
		return false;
	}
	const int rate = rate_of(sample, edit);
	const long frames = (long)(edit->end - edit->start);
	const double step = sample->source_hz * pow(2.0, edit->pitch / 1200.0) / ROM_SAMPLE_HZ[rate];
	const double length_wanted = ceil(frames / step);
	if (length_wanted > ROM_IMAGE_MAX || rom_image_cost((uint32_t)length_wanted) > rom_image_room()) {
		return false;
	}
	const uint32_t length = (uint32_t)length_wanted, cost = rom_image_cost(length);
	const double cutoff = step > 1 ? 0.95 / step : 1.0;
	const int side = (int)ceil(SINC_SIDE * (step > 1 ? step : 1));
	double *out = malloc(length * sizeof(double));
	int8_t *bytes = calloc(1, cost);
	double *table = sinc_table(cutoff, side);
	if (!out || !bytes || !table) {
		free(out);
		free(bytes);
		free(table);
		return false;
	}
	const double gain = pow(10.0, edit->gain / 200.0);
	for (uint32_t k = 0; k < length; k++) {
		out[k] = source_at(sample->source + edit->start, frames, k * step, table, side) * gain;
	}
	free(table);
	if (edit->fade) {
		// The last few ms go down to silence, so the end doesn't click.
		uint32_t fade = (uint32_t)lround(FADE_SECONDS * ROM_SAMPLE_HZ[rate]);
		fade = fade < length ? fade : length;
		for (uint32_t i = 0; i < fade; i++) {
			out[length - fade + i] *= (double)(fade - 1 - i) / fade;
		}
	}
	bool clipped = false;
	for (uint32_t k = 0; k < length; k++) {
		const long v = lround(out[k] * 128);
		clipped = clipped || v > 127 || v < -128;
		bytes[k] = (int8_t)(v > 127 ? 127 : v < -128 ? -128 : v);
	}
	free(out);
	free(sample->bytes);
	sample->bytes = bytes;
	sample->edit = *edit;
	sample->clipped = clipped;
	sample->sample.rate = (uint8_t)rate;
	sample->sample.length = cost;
	sample->sample.bytes = bytes;
	sample->sample.flags = edit->one_shot ? ROM_SAMPLE_ONE_SHOT : 0;
	return true;
}

int dac_normalize_gain(const dac_sample *sample)
{
	int peak = 1;
	for (uint32_t i = sample->edit.start; sample->source && i < sample->edit.end; i++) {
		const int v = abs(sample->source[i]);
		peak = v > peak ? v : peak;
	}
	// Half a dB under full scale: the resampling may overshoot a little.
	const long gain = lround(200.0 * log10(32768.0 / peak) - 5);
	return gain > DAC_GAIN_MAX ? DAC_GAIN_MAX : gain < -DAC_GAIN_MAX ? -DAC_GAIN_MAX : (int)gain;
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
	*error = "longer than the project keeps for sounds";
	if (frames * sizeof(int16_t) > DAC_SOURCE_MAX) {
		return NULL;
	}
	*error = "out of memory";
	dac_sample *out = calloc(1, sizeof(dac_sample));
	int16_t *source = malloc(frames * sizeof(int16_t));
	if (!out || !source) {
		free(out);
		free(source);
		return NULL;
	}
	for (size_t i = 0; i < frames; i++) {
		const long v = lround(frame_value(data + i * frame_bytes, channels, bits, is_float) * 32767);
		source[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
	}
	snprintf(out->name, sizeof(out->name), "%s", name);
	out->source = source;
	out->source_hz = file_hz;
	out->source_frames = (uint32_t)frames;
	const dac_edit edit = {.start = 0, .end = (uint32_t)frames, .rate = DAC_RATE_AUTO, .fade = true};
	if (!dac_prepare(out, &edit)) {
		*error = "the sound is longer than the ROM has room for";
		dac_free(out);
		return NULL;
	}
	*error = NULL;
	return out;
}

void dac_free(dac_sample *sample)
{
	if (sample) {
		free(sample->source);
		free(sample->bytes);
		free(sample);
	}
}

dac_sample *dac_copy(const dac_sample *sample)
{
	if (!sample) {
		return NULL;
	}
	dac_sample *copy = malloc(sizeof(dac_sample));
	int16_t *source = sample->source ? malloc(sample->source_frames * sizeof(int16_t)) : NULL;
	int8_t *bytes = malloc(sample->sample.length);
	if (!copy || !bytes || (sample->source && !source)) {
		free(copy);
		free(source);
		free(bytes);
		return NULL;
	}
	*copy = *sample;
	if (source) {
		memcpy(source, sample->source, sample->source_frames * sizeof(int16_t));
	}
	memcpy(bytes, sample->bytes, sample->sample.length);
	copy->source = source;
	copy->bytes = bytes;
	copy->sample.bytes = bytes;
	return copy;
}

void dac_clear(dac_set *set)
{
	for (int note = 0; note < ROM_IMAGE_NOTES; note++) {
		dac_free(set->notes[note]);
		set->notes[note] = NULL;
	}
}

void dac_put(dac_set *set, int note, dac_sample *sample)
{
	dac_free(set->notes[note]);
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

uint32_t dac_sources(const dac_set *set)
{
	uint32_t used = 0;
	for (int note = 0; note < ROM_IMAGE_NOTES; note++) {
		if (set->notes[note]) {
			used += set->notes[note]->source_frames * (uint32_t)sizeof(int16_t);
		}
	}
	return used;
}

static bool same_edit(const dac_edit *a, const dac_edit *b)
{
	return a->start == b->start && a->end == b->end && a->pitch == b->pitch && a->gain == b->gain && a->rate == b->rate && a->fade == b->fade && a->one_shot == b->one_shot;
}

bool dac_same(const dac_set *a, const dac_set *b)
{
	for (int note = 0; note < ROM_IMAGE_NOTES; note++) {
		const dac_sample *x = a->notes[note], *y = b->notes[note];
		if (!x != !y) {
			return false;
		}
		if (x && (strcmp(x->name, y->name) || !same_edit(&x->edit, &y->edit) || x->source_hz != y->source_hz || x->source_frames != y->source_frames
				|| !x->source != !y->source || (x->source && memcmp(x->source, y->source, x->source_frames * sizeof(int16_t)))
				|| x->sample.rate != y->sample.rate || x->sample.length != y->sample.length || x->sample.flags != y->sample.flags
				|| memcmp(x->bytes, y->bytes, x->sample.length))) {
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

// One sample's head in the project's block, little endian: name, source rate and frames, the edit
// (start, end, pitch, gain, rate, fade, one-shot), the bytes' rate, clipped, and their length.
#define HEAD_BYTES (DAC_NAME + 4 + 4 + 4 + 4 + 2 + 2 + 1 + 1 + 1 + 1 + 1 + 4)

static void put32(uint8_t **at, uint32_t v)
{
	memcpy(*at, &v, 4);
	*at += 4;
}

static void put16(uint8_t **at, uint16_t v)
{
	memcpy(*at, &v, 2);
	*at += 2;
}

static uint32_t get32(const uint8_t **at)
{
	uint32_t v;
	memcpy(&v, *at, 4);
	*at += 4;
	return v;
}

static uint16_t get16(const uint8_t **at)
{
	uint16_t v;
	memcpy(&v, *at, 2);
	*at += 2;
	return v;
}

size_t dac_block_size(const dac_set *set)
{
	size_t size = 0;
	for (int note = 0; note < ROM_IMAGE_NOTES; note++) {
		const dac_sample *s = set->notes[note];
		if (s) {
			size += 1 + 2 + HEAD_BYTES + (s->source ? s->source_frames * sizeof(int16_t) : 0) + s->sample.length;
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
		const uint32_t frames = s->source ? s->source_frames : 0;
		*out++ = (uint8_t)note;
		put16(&out, HEAD_BYTES);
		memcpy(out, s->name, DAC_NAME);
		out += DAC_NAME;
		put32(&out, s->source_hz);
		put32(&out, frames);
		put32(&out, s->edit.start);
		put32(&out, s->edit.end);
		put16(&out, (uint16_t)s->edit.pitch);
		put16(&out, (uint16_t)s->edit.gain);
		*out++ = (uint8_t)s->edit.rate;
		*out++ = s->edit.fade;
		*out++ = s->edit.one_shot;
		*out++ = s->sample.rate;
		*out++ = s->clipped;
		put32(&out, s->sample.length);
		if (frames) {
			memcpy(out, s->source, frames * sizeof(int16_t));
			out += frames * sizeof(int16_t);
		}
		memcpy(out, s->bytes, s->sample.length);
		out += s->sample.length;
	}
}

bool dac_block_read(dac_set *set, const uint8_t *block, size_t length)
{
	dac_clear(set);
	uint64_t used = 0, sources = 0;
	for (size_t at = 0; at < length;) {
		if (length - at < 3) {
			break;
		}
		const int note = block[at];
		uint16_t head_length;
		memcpy(&head_length, block + at + 1, 2);
		at += 3;
		if (head_length > length - at) {
			dac_clear(set);
			return false;
		}
		// A shorter head of another build leaves the rest at zero, a longer one is skipped past.
		uint8_t head[HEAD_BYTES] = {0};
		memcpy(head, block + at, head_length < HEAD_BYTES ? head_length : HEAD_BYTES);
		at += head_length;
		const uint8_t *h = head;
		dac_sample *s = calloc(1, sizeof(dac_sample));
		if (!s) {
			dac_clear(set);
			return false;
		}
		memcpy(s->name, h, DAC_NAME);
		s->name[DAC_NAME - 1] = '\0';
		h += DAC_NAME;
		s->source_hz = get32(&h);
		const uint32_t frames = get32(&h);
		s->edit.start = get32(&h);
		s->edit.end = get32(&h);
		s->edit.pitch = (int16_t)get16(&h);
		s->edit.gain = (int16_t)get16(&h);
		s->edit.rate = (int8_t)*h++;
		s->edit.fade = *h++ != 0;
		s->edit.one_shot = *h++ != 0;
		s->sample.rate = *h++;
		s->clipped = *h++ != 0;
		const uint32_t bytes = get32(&h);
		const uint64_t source_bytes = (uint64_t)frames * sizeof(int16_t);
		used += bytes;
		sources += source_bytes;
		const bool fine = note < ROM_IMAGE_NOTES && s->sample.rate < ROM_SAMPLE_RATES && bytes && bytes % ROM_SAMPLE_BLOCK == 0
			&& s->edit.rate >= DAC_RATE_AUTO && s->edit.rate < ROM_SAMPLE_RATES && abs(s->edit.pitch) <= DAC_PITCH_MAX && abs(s->edit.gain) <= DAC_GAIN_MAX
			&& source_bytes + bytes <= length - at && used <= rom_image_room() && sources <= DAC_SOURCE_MAX
			&& (!frames || (s->source_hz && s->edit.start < s->edit.end && s->edit.end <= frames));
		s->source = frames && fine ? malloc(source_bytes) : NULL;
		s->bytes = fine ? malloc(bytes) : NULL;
		if (!fine || !s->bytes || (frames && !s->source)) {
			dac_free(s);
			dac_clear(set);
			return false;
		}
		s->source_frames = frames;
		if (frames) {
			memcpy(s->source, block + at, source_bytes);
			at += source_bytes;
		}
		memcpy(s->bytes, block + at, bytes);
		at += bytes;
		s->sample.length = bytes;
		s->sample.bytes = s->bytes;
		s->sample.flags = s->edit.one_shot ? ROM_SAMPLE_ONE_SHOT : 0;
		dac_put(set, note, s);
	}
	return true;
}
