#ifndef MDMI_RESAMPLER_H
#define MDMI_RESAMPLER_H

#include <stddef.h>

// Stereo windowed-sinc resampler from the emulator's rate to the host's.
// Input frames are centered on HALF_TAPS, so the output lags by that many input frames.
#define RESAMPLER_HALF_TAPS 16
#define RESAMPLER_PHASES 512

typedef struct {
	float table[RESAMPLER_PHASES + 1][RESAMPLER_HALF_TAPS * 2];
	float history[2][RESAMPLER_HALF_TAPS * 4];
	int write_pos;
	int pending;
	double frac;
	double step;
} resampler;

void resampler_init(resampler *r, double in_rate, double out_rate);

// Feeds one input frame and writes up to max_out frames; returns how many were written.
int resampler_push(resampler *r, float left, float right, float *out_left, float *out_right, int max_out);

#endif
