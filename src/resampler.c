#include <math.h>
#include <string.h>
#include "resampler.h"

#define NUM_TAPS (RESAMPLER_HALF_TAPS * 2)
#define KAISER_BETA 9.0

static double bessel_i0(double x)
{
	double sum = 1.0, term = 1.0;
	for (int k = 1; k < 32; k++) {
		term *= (x / (2.0 * k)) * (x / (2.0 * k));
		sum += term;
	}
	return sum;
}

void resampler_init(resampler *r, double in_rate, double out_rate, int channels)
{
	memset(r, 0, sizeof(*r));
	r->channels = channels < 1 ? 1 : channels > RESAMPLER_MAX_CHANNELS ? RESAMPLER_MAX_CHANNELS : channels;
	r->step = in_rate / out_rate;
	double cutoff = out_rate < in_rate ? 0.97 * out_rate / in_rate : 0.97;
	for (int phase = 0; phase <= RESAMPLER_PHASES; phase++) {
		double frac = (double)phase / RESAMPLER_PHASES;
		double sum = 0;
		double kernel[NUM_TAPS];
		for (int tap = 0; tap < NUM_TAPS; tap++) {
			// Taps sit at integer offsets from the oldest sample, the read point at HALF_TAPS - 1 + frac.
			double x = tap - (RESAMPLER_HALF_TAPS - 1) - frac;
			double arg = M_PI * cutoff * x;
			double sinc = fabs(arg) < 1e-9 ? 1.0 : sin(arg) / arg;
			double w = x / RESAMPLER_HALF_TAPS;
			double window = fabs(w) >= 1.0 ? 0.0 : bessel_i0(KAISER_BETA * sqrt(1.0 - w * w)) / bessel_i0(KAISER_BETA);
			kernel[tap] = sinc * window;
			sum += kernel[tap];
		}
		for (int tap = 0; tap < NUM_TAPS; tap++) {
			r->table[phase][tap] = (float)(kernel[tap] / sum);
		}
	}
}

int resampler_push(resampler *r, const float *in, float *out, int max_out)
{
	// History is mirrored so the newest NUM_TAPS samples are always contiguous.
	for (int ch = 0; ch < r->channels; ch++) {
		r->history[ch][r->write_pos] = r->history[ch][r->write_pos + NUM_TAPS] = in[ch];
	}
	r->write_pos = (r->write_pos + 1) % NUM_TAPS;

	int written = 0;
	r->frac -= 1.0;
	while (r->frac < 0.0 && written < max_out) {
		double scaled = (r->frac + 1.0) * RESAMPLER_PHASES;
		int phase = (int)scaled;
		float blend = (float)(scaled - phase);
		const float *a = r->table[phase];
		const float *b = r->table[phase + 1];
		float coef[NUM_TAPS];
		for (int tap = 0; tap < NUM_TAPS; tap++) {
			coef[tap] = a[tap] + (b[tap] - a[tap]) * blend;
		}
		float *frame = out + written * r->channels;
		for (int ch = 0; ch < r->channels; ch++) {
			const float *h = r->history[ch] + r->write_pos;
			float sum = 0;
			for (int tap = 0; tap < NUM_TAPS; tap++) {
				sum += h[tap] * coef[tap];
			}
			frame[ch] = sum;
		}
		written++;
		r->frac += r->step;
	}
	return written;
}
