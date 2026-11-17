// Stage 36 (D81): the ROM screen as on a TV, the model of docs/design/tv-look.md (from the study
// build/tv-look/make_tv.py; tools/test/check_tv.py holds it in numpy and checks this against it).
//
// Each H40 pixel is held for 16 subsamples; the colour subcarrier is 30 of them (1.875 pixels) and has
// the same phase on every line and frame. COMP: s = Y + I sin p + Q cos p; the TV takes luma as the
// mean over one cycle and demodulates the rest twice through 45-subsample means. Both looks then take
// the TV's gamma (2.5 against the monitor's 2.2) and draw each line as a bright and a dark window row,
// the dark one filling in on bright colours. Window column 2x + k samples subsample 16x + 4 + 8k.
#include "tv.h"

#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define TV_W 320
#define TV_H_MAX 240
#define SUB 16
#define N (TV_W * SUB)
#define CYCLE 30
#define CHROMA 45
#define GAMMA_STEPS 4096

struct tv_cache {
	int look;
	int height;
	bool valid[TV_H_MAX];
	uint32_t source[TV_H_MAX][TV_W];
	uint32_t out[TV_H_MAX][2][2 * TV_W];
};

tv_cache *tv_cache_new(void)
{
	return calloc(1, sizeof(tv_cache));
}

void tv_cache_free(tv_cache *cache)
{
	free(cache);
}

static float gamma_table[GAMMA_STEPS + 1];
static uint32_t rgb_rows[2][256];
static float sin2[CYCLE], cos2[CYCLE];
static bool tables_made;

static void make_tables(void)
{
	for (int i = 0; i <= GAMMA_STEPS; i++) {
		gamma_table[i] = (float)pow((double)i / GAMMA_STEPS, 2.5 / 2.2);
	}
	for (int v = 0; v < 256; v++) {
		const double g = pow(v / 255.0, 2.5 / 2.2);
		const double bright = fmin(1.0, 1.15 * g), dark = fmin(1.0, 1.15 * g * (0.25 + 0.50 * g));
		rgb_rows[0][v] = (uint32_t)(bright * 255 + 0.5);
		rgb_rows[1][v] = (uint32_t)(dark * 255 + 0.5);
	}
	for (int i = 0; i < CYCLE; i++) {
		const double p = 2 * 3.14159265358979323846 * i / CYCLE;
		sin2[i] = (float)(2 * sin(p));
		cos2[i] = (float)(2 * cos(p));
	}
	tables_made = true;
}

// The TV's gamma of a decoded value, clipped to 0..1 first.
static float tv_gamma(float c)
{
	if (c <= 0) {
		return 0;
	}
	if (c >= 1) {
		return 1;
	}
	const float at = c * GAMMA_STEPS;
	const int i = (int)at;
	return gamma_table[i] + (gamma_table[i + 1] - gamma_table[i]) * (at - (float)i);
}

// Prefix sums of `a` with its edges repeated, for a centred mean of n: the mean at k is
// (prefix[k + n] - prefix[k]) / n over a[k - n/2 .. k - n/2 + n - 1], as make_tv.py's box().
static void prefix_of(const float *a, int n, double *prefix)
{
	const int left = n / 2;
	prefix[0] = 0;
	for (int t = 0; t < N + n - 1; t++) {
		int k = t - left;
		k = k < 0 ? 0 : k >= N ? N - 1 : k;
		prefix[t + 1] = prefix[t] + a[k];
	}
}

static void box_all(const float *a, int n, float *out, double *prefix)
{
	prefix_of(a, n, prefix);
	for (int k = 0; k < N; k++) {
		out[k] = (float)((prefix[k + n] - prefix[k]) / n);
	}
}

static uint32_t channel_byte(float v)
{
	v = v < 0 ? 0 : v > 1 ? 1 : v;
	return (uint32_t)(v * 255 + 0.5f);
}

static void composite_line(const uint32_t *source, uint32_t *bright, uint32_t *dark)
{
	static float s[N], y[N], ci[N], cq[N], i1[N], q1[N];
	static double prefix[N + CHROMA + 1], prefix_i[N + CHROMA + 1], prefix_q[N + CHROMA + 1];
	for (int x = 0; x < TV_W; x++) {
		const float r = (float)(source[x] >> 16 & 0xFF) / 255, g = (float)(source[x] >> 8 & 0xFF) / 255, b = (float)(source[x] & 0xFF) / 255;
		const float Y = 0.299f * r + 0.587f * g + 0.114f * b;
		const float I = 0.596f * r - 0.274f * g - 0.322f * b;
		const float Q = 0.211f * r - 0.523f * g + 0.312f * b;
		for (int j = 0; j < SUB; j++) {
			const int at = x * SUB + j, phase = at % CYCLE;
			s[at] = Y + I * sin2[phase] / 2 + Q * cos2[phase] / 2;
		}
	}
	box_all(s, CYCLE, y, prefix);
	for (int k = 0; k < N; k++) {
		const float c = s[k] - y[k];
		ci[k] = c * sin2[k % CYCLE];
		cq[k] = c * cos2[k % CYCLE];
	}
	box_all(ci, CHROMA, i1, prefix);
	box_all(cq, CHROMA, q1, prefix);
	prefix_of(i1, CHROMA, prefix_i);
	prefix_of(q1, CHROMA, prefix_q);
	for (int col = 0; col < 2 * TV_W; col++) {
		const int k = (col / 2) * SUB + 4 + 8 * (col & 1);
		const float i = (float)((prefix_i[k + CHROMA] - prefix_i[k]) / CHROMA);
		const float q = (float)((prefix_q[k + CHROMA] - prefix_q[k]) / CHROMA);
		const float l = y[k];
		const float rgb[3] = {l + 0.956f * i + 0.621f * q, l - 0.272f * i - 0.647f * q, l - 1.106f * i + 1.703f * q};
		uint32_t hi = 0, lo = 0;
		for (int ch = 0; ch < 3; ch++) {
			const float line = tv_gamma(rgb[ch]);
			hi = hi << 8 | channel_byte(1.15f * line);
			lo = lo << 8 | channel_byte(1.15f * line * (0.25f + 0.50f * line));
		}
		bright[col] = hi;
		dark[col] = lo;
	}
}

static void rgb_line(const uint32_t *source, uint32_t *bright, uint32_t *dark)
{
	for (int x = 0; x < TV_W; x++) {
		const uint32_t p = source[x];
		const uint32_t hi = rgb_rows[0][p >> 16 & 0xFF] << 16 | rgb_rows[0][p >> 8 & 0xFF] << 8 | rgb_rows[0][p & 0xFF];
		const uint32_t lo = rgb_rows[1][p >> 16 & 0xFF] << 16 | rgb_rows[1][p >> 8 & 0xFF] << 8 | rgb_rows[1][p & 0xFF];
		bright[2 * x] = bright[2 * x + 1] = hi;
		dark[2 * x] = dark[2 * x + 1] = lo;
	}
}

void tv_draw(tv_cache *cache, int look, const uint32_t *frame, int stride, int height, uint32_t *out, int out_stride)
{
	if (!tables_made) {
		make_tables();
	}
	height = height > TV_H_MAX ? TV_H_MAX : height;
	if (cache && (cache->look != look || cache->height != height)) {
		memset(cache->valid, 0, sizeof(cache->valid));
		cache->look = look;
		cache->height = height;
	}
	static uint32_t line[TV_W], rows[2][2 * TV_W];
	for (int y = 0; y < height; y++) {
		for (int x = 0; x < TV_W; x++) {
			line[x] = frame[y * stride + x] & 0xFFFFFF;
		}
		uint32_t *bright = rows[0], *dark = rows[1];
		if (cache) {
			bright = cache->out[y][0];
			dark = cache->out[y][1];
		}
		if (!cache || !cache->valid[y] || memcmp(cache->source[y], line, sizeof(line))) {
			if (look == TV_COMP) {
				composite_line(line, bright, dark);
			} else {
				rgb_line(line, bright, dark);
			}
			if (cache) {
				memcpy(cache->source[y], line, sizeof(line));
				cache->valid[y] = true;
			}
		}
		memcpy(out + (2 * y) * out_stride, bright, sizeof(rows[0]));
		memcpy(out + (2 * y + 1) * out_stride, dark, sizeof(rows[1]));
	}
}
