#ifndef MDMI_RESAMPLER_H
#define MDMI_RESAMPLER_H

#include <stddef.h>

// Windowed-sinc resampler from the emulator's rate to the host's, for every channel of the outputs
// at once (stage 17: ten stereo outputs). Input frames are centered on HALF_TAPS, so the output lags
// by that many input frames.
#define RESAMPLER_HALF_TAPS 16
#define RESAMPLER_PHASES 512
#define RESAMPLER_MAX_CHANNELS 20

typedef struct {
	float table[RESAMPLER_PHASES + 1][RESAMPLER_HALF_TAPS * 2];
	float history[RESAMPLER_MAX_CHANNELS][RESAMPLER_HALF_TAPS * 4];
	int channels;
	int write_pos;
	int pending;
	double frac;
	double step;
} resampler;

void resampler_init(resampler *r, double in_rate, double out_rate, int channels);

// Feeds one input frame of every channel and writes up to max_out frames, each channels values long;
// returns how many were written.
int resampler_push(resampler *r, const float *in, float *out, int max_out);

#endif
