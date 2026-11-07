// Does the running YM2612 hold what the model says? Drives patch.c the way the plugin does and
// reads the chip's registers, which no test by ear can do. A channel keeps the velocity of its last
// note, and that scales the TL of the carriers, so notes here are at full velocity.
//
//   reg_test [folder of the test instrument files]
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define PSAPI_VERSION 2
#include <windows.h>
#include <psapi.h>
#include "dac.h"
#include "engine.h"
#include "instrument.h"
#include "patch.h"

#define RATE 48000
#define BLOCK 256
#define SETTLE_BLOCKS 24
#define RESET_BLOCKS 300
#define DAC_BLOCKS 400
#define FM_CHANNELS PATCH_NUM_FM_CHANNELS

enum { F_TL, F_MUL, F_DT, F_RS, F_AR, F_DR, F_SR, F_SL, F_RR, F_AM, F_SSG };

static mdmi_engine *engine;
static mdmi_patch *patch;
static int failures;
static float left[BLOCK], right[BLOCK];

static void emit(void *context, const uint8_t *bytes, size_t length)
{
	(void)context;
	mdmi_engine_midi(engine, 0, bytes, length);
}

static void render(int blocks)
{
	for (int i = 0; i < blocks; i++) {
		mdmi_engine_render(engine, left, right, BLOCK);
	}
}

static void sync_and_settle(void)
{
	patch_sync(patch, &patch->rom, emit, NULL);
	render(SETTLE_BLOCKS);
}

static void midi(uint8_t a, uint8_t b, uint8_t c)
{
	const uint8_t message[3] = {a, b, c};
	patch_midi(patch, message, (a & 0xF0) == 0xC0 ? 2 : 3, emit, NULL);
	patch_flush(patch, emit, NULL);
}

static void play_chord(int notes, bool on)
{
	uint8_t status = (uint8_t)(on ? 0x90 : 0x80);
	for (int i = 0; i < notes; i++) {
		midi(status, (uint8_t)(60 + i), on ? 127 : 0);
	}
	render(SETTLE_BLOCKS);
}

static void set_on(int channel, int param, int value)
{
	patch_set(patch, PATCH_CHANNEL_ID(channel, param), value);
	sync_and_settle();
}

static void set_global(int global, int value)
{
	patch_set(patch, (uint32_t)global, value);
}

static uint8_t read_reg(int voice, int base, int slot)
{
	return mdmi_engine_test_ym(engine, voice >= 3, (uint8_t)(base + voice % 3 + slot * 4));
}

static int model_value(int channel, int param)
{
	return patch->model.channels[channel][param];
}

// How many of the voice's registers differ from a channel of the model.
static int compare(int voice, int channel, const char *what)
{
	int wrong = 0;
#define EXPECT(got, want, name, op) \
	do { \
		int got_ = (got), want_ = (want); \
		if (got_ != want_) { \
			if (what && wrong < 2) { \
				printf("    %s voice %d op %d %s: register %d, model %d\n", what, voice + 1, op + 1, name, got_, want_); \
			} \
			wrong++; \
		} \
	} while (0)
	for (int op = 0; op < PATCH_OPS; op++) {
		const int slot = op == 1 ? 2 : op == 2 ? 1 : op;
		const int first = PARAM_OP_BASE + op * PATCH_OP_PARAMS;
#define F(field) model_value(channel, first + field)
		EXPECT(read_reg(voice, 0x30, slot), F(F_DT) << 4 | F(F_MUL), "DT/MUL", op);
		EXPECT(read_reg(voice, 0x40, slot), F(F_TL), "TL", op);
		EXPECT(read_reg(voice, 0x50, slot), F(F_RS) << 6 | F(F_AR), "RS/AR", op);
		EXPECT(read_reg(voice, 0x60, slot), F(F_AM) << 7 | F(F_DR), "AM/DR", op);
		EXPECT(read_reg(voice, 0x70, slot), F(F_SR), "SR", op);
		EXPECT(read_reg(voice, 0x80, slot), F(F_SL) << 4 | F(F_RR), "SL/RR", op);
		EXPECT(read_reg(voice, 0x90, slot), F(F_SSG), "SSG", op);
#undef F
	}
	EXPECT(read_reg(voice, 0xB0, 0), model_value(channel, PARAM_FEEDBACK) << 3 | model_value(channel, PARAM_ALGORITHM), "FB/ALG", 0);
	EXPECT(read_reg(voice, 0xB4, 0) & 0x3F, model_value(channel, PARAM_AMS) << 4 | model_value(channel, PARAM_FMS), "AMS/FMS", 0);
	static const uint8_t PAN_BITS[3] = {0x80, 0xC0, 0x40};
	EXPECT(read_reg(voice, 0xB4, 0) & 0xC0, PAN_BITS[model_value(channel, PARAM_PAN)], "pan", 0);
#undef EXPECT
	return wrong;
}

// Compares voices with their own channels, or with the one channel that owns them all.
static void check_voices(const char *what, int first_voice, int last_voice, int owner)
{
	int bad = 0;
	for (int voice = first_voice; voice <= last_voice; voice++) {
		if (compare(voice, owner >= 0 ? owner : voice, what)) {
			bad++;
		}
	}
	printf("  %s: %s, %d of %d voices differ\n", bad ? "FAIL" : "ok  ", what, bad, last_voice - first_voice + 1);
	if (bad) {
		failures++;
	}
}

static void check_globals(const char *what)
{
	patch_publish(patch);
	int lfo = patch_get(patch, GLOBAL_LFO_ON) << 3 | patch_get(patch, GLOBAL_LFO_RATE);
	int ch3 = patch_get(patch, GLOBAL_CH3_MODE) << 6;
	int dac = patch_get(patch, GLOBAL_DAC) << 7;
	int got_lfo = mdmi_engine_test_ym(engine, 0, 0x22), got_ch3 = mdmi_engine_test_ym(engine, 0, 0x27) & 0xC0;
	int got_dac = mdmi_engine_test_ym(engine, 0, 0x2B) & 0x80;
	bool same = got_lfo == lfo && got_ch3 == ch3 && got_dac == dac;
	printf("  %s: %s, LFO register %d model %d, Ch3 mode %d model %d, DAC %d model %d\n", same ? "ok  " : "FAIL", what, got_lfo, lfo, got_ch3, ch3, got_dac, dac);
	if (!same) {
		failures++;
	}
}

// The engine's warm-up leaves a lower velocity behind, and a channel keeps its last note's.
static void full_velocity(void)
{
	for (int channel = 0; channel < FM_CHANNELS; channel++) {
		midi((uint8_t)(0x90 | channel), 60, 127);
		midi((uint8_t)(0x80 | channel), 60, 0);
	}
	render(SETTLE_BLOCKS);
}

static void test_every_preset(void)
{
	for (int channel = 0; channel < FM_CHANNELS; channel++) {
		int bad = 0;
		for (int program = 127; program >= 0; program--) {
			patch_set(patch, PATCH_CHANNEL_ID(channel, PARAM_PRESET), program);
			sync_and_settle();
			if (compare(channel, channel, "preset")) {
				bad++;
			}
		}
		printf("  %s: channel %d, %d of 128 presets differ\n", bad ? "FAIL" : "ok  ", channel + 1, bad);
		if (bad) {
			failures++;
		}
	}
}

static void test_values(void)
{
	// One operator of each kind of register, at its maximum, by knob on the second channel.
	for (int field = 0; field <= F_SSG; field++) {
		int param = PARAM_OP_BASE + 2 * PATCH_OP_PARAMS + field;
		set_on(1, param, patch_param(PATCH_CHANNEL_ID(1, param))->max);
	}
	check_voices("every field of an operator at maximum", 1, 1, -1);
	set_on(1, PARAM_ALGORITHM, 7);
	set_on(1, PARAM_FEEDBACK, 7);
	set_on(1, PARAM_AMS, 3);
	set_on(1, PARAM_FMS, 7);
	check_voices("algorithm, feedback, AMS, FMS", 1, 1, -1);
	set_on(4, PARAM_PRESET, 8);
	set_on(4, PARAM_OP_BASE + F_SR, 31);
	set_on(4, PARAM_OP_BASE + PATCH_OP_PARAMS + F_SR, 22);
	set_on(4, PARAM_OP_BASE + 2 * PATCH_OP_PARAMS + F_SR, 3);
	check_voices("SR by knob, above and below 16, on the chip's second part", 4, 4, -1);
	midi(0xB4, (uint8_t)(16 + 3), 55);
	midi(0xB4, 53, 64);
	render(SETTLE_BLOCKS);
	check_voices("TL and SR from a clip's CC", 4, 4, -1);
}

static void test_program_from_clip(void)
{
	set_on(2, PARAM_PRESET, 30);
	set_on(2, PARAM_OP_BASE + F_TL, 90);
	set_on(2, PARAM_OP_BASE + F_SR, 29);
	midi(0xC2, 30, 0);
	render(SETTLE_BLOCKS);
	check_voices("the same program from a clip wipes tweaks in the chip too", 2, 2, -1);
	set_on(2, PARAM_OP_BASE + F_TL, 90);
	patch_set(patch, PATCH_CHANNEL_ID(2, PARAM_PRESET), 30);
	sync_and_settle();
	check_voices("a lane repeating the preset keeps them", 2, 2, -1);
	midi(0xB2, 121, 0);
	render(SETTLE_BLOCKS);
	midi(0x92, 60, 127);
	render(SETTLE_BLOCKS);
	midi(0x82, 60, 0);
	render(SETTLE_BLOCKS);
	check_voices("reset all controllers and a note leave the voice alone", 2, 2, -1);
}

// The ROM loads another Z80 driver for this, so the voices take a while to reach the chip.
static void wait_for_voices(int last_voice)
{
	patch_sync(patch, &patch->rom, emit, NULL);
	// The registers still match until the reset arrives, so it has to be seen first.
	bool reset_seen = false;
	int blocks = 0;
	for (; blocks < DAC_BLOCKS; blocks++) {
		render(1);
		bool same = true;
		for (int voice = 0; voice <= last_voice; voice++) {
			same = same && compare(voice, voice, NULL) == 0;
		}
		reset_seen = reset_seen || !same;
		if (reset_seen && same) {
			break;
		}
	}
	printf("    reset %s, voices back after %d ms\n", reset_seen ? "seen" : "NOT SEEN", blocks * BLOCK * 1000 / RATE);
	if (!reset_seen) {
		failures++;
	}
}

static void test_dac(void)
{
	set_on(3, PARAM_OP_BASE + F_TL, 66);
	set_on(3, PARAM_OP_BASE + F_SR, 24);
	set_on(3, PARAM_PAN, 2);
	set_global(GLOBAL_LFO_RATE, 5);
	set_global(GLOBAL_CH3_MODE, 1);
	sync_and_settle();
	check_globals("LFO rate and Ch3 mode by knob");
	set_global(GLOBAL_DAC, 1);
	wait_for_voices(FM_CHANNELS - 2);
	check_voices("DAC on: the five FM voices come back", 0, FM_CHANNELS - 2, -1);
	check_globals("and the LFO and Ch3 mode");
	set_global(GLOBAL_DAC, 0);
	wait_for_voices(FM_CHANNELS - 1);
	check_voices("DAC off: all six come back", 0, FM_CHANNELS - 1, -1);
	check_globals("and the LFO and Ch3 mode");

	set_global(GLOBAL_LFO_RATE, 3);
	set_global(GLOBAL_LFO_ON, 0);
	sync_and_settle();
	check_globals("LFO off and rate 3 by knob");
	set_global(GLOBAL_LFO_ON, 1);
	set_global(GLOBAL_CH3_MODE, 0);
	sync_and_settle();
	check_voices("every channel keeps its own voice", 0, FM_CHANNELS - 1, -1);
}

static void check(bool passed, const char *what);

// Switching the DAC swaps the Z80 driver, and SGDK resets the PSG with it: tone 0, silence, noise
// control 0. The same notes on every PSG channel must sound as before the switch.
static void psg_notes(bool on)
{
	for (int i = 0; i < PATCH_NUM_PSG_CHANNELS; i++) {
		midi((uint8_t)((on ? 0x90 : 0x80) | (PATCH_NUM_FM_CHANNELS + i)), (uint8_t)(64 + i * 3), on ? 127 : 0);
	}
	render(SETTLE_BLOCKS);
}

static void psg_state(uint16_t *tone, uint8_t *volume)
{
	for (int i = 0; i < PATCH_NUM_PSG_CHANNELS; i++) {
		tone[i] = mdmi_engine_test_psg_tone(engine, (uint8_t)i);
		volume[i] = mdmi_engine_test_psg_volume(engine, (uint8_t)i);
	}
}

static void check_psg_after_dac(const char *what, int dac, const uint16_t *tone, const uint8_t *volume)
{
	uint16_t now_tone[PATCH_NUM_PSG_CHANNELS];
	uint8_t now_volume[PATCH_NUM_PSG_CHANNELS];
	set_global(GLOBAL_DAC, dac);
	sync_and_settle();
	render(DAC_BLOCKS);
	psg_notes(true);
	psg_state(now_tone, now_volume);
	psg_notes(false);
	int bad = 0;
	for (int i = 0; i < PATCH_NUM_PSG_CHANNELS; i++) {
		if (tone[i] != now_tone[i] || volume[i] != now_volume[i]) {
			printf("    channel %d: tone %d, before %d; attenuation %d, before %d\n", PATCH_NUM_FM_CHANNELS + i + 1, now_tone[i], tone[i], now_volume[i], volume[i]);
			bad++;
		}
	}
	check(bad == 0, what);
}

static void test_dac_psg(void)
{
	uint16_t tone[PATCH_NUM_PSG_CHANNELS];
	uint8_t volume[PATCH_NUM_PSG_CHANNELS];
	psg_notes(true);
	psg_state(tone, volume);
	psg_notes(false);
	check_psg_after_dac("DAC on: the same PSG notes sound as before", 1, tone, volume);
	check_psg_after_dac("DAC off: the same PSG notes sound as before", 0, tone, volume);
}

// A new image restarts the core (stage 12): the plugin hands everything over again, and every channel
// sounds as before. The same image twenty times, so a core that doesn't unload cleanly shows.
#define RESTARTS 20

static size_t private_bytes(void)
{
	PROCESS_MEMORY_COUNTERS_EX counters = {.cb = sizeof(counters)};
	GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&counters, sizeof(counters));
	return counters.PrivateUsage;
}

static void steady(patch_envelope *envelope, uint8_t level, int steps);

static void test_restart(void)
{
	// As the DAC page restarts: with the DAC on, and a PSG channel playing an envelope of its own.
	patch_envelope own;
	steady(&own, 3, 40);
	patch_load_envelope(patch, PATCH_NUM_FM_CHANNELS + 1, &own, NULL);
	patch_take_incoming(patch);
	set_global(GLOBAL_DAC, 1);
	sync_and_settle();
	render(DAC_BLOCKS);
	set_on(1, PARAM_OP_BASE + F_TL, 50);
	set_on(4, PARAM_OP_BASE + 3 * PATCH_OP_PARAMS + F_AR, 9);
	set_on(2, PARAM_PAN, 0);
	set_global(GLOBAL_LFO_RATE, 6);
	sync_and_settle();
	uint16_t tone[PATCH_NUM_PSG_CHANNELS], now_tone[PATCH_NUM_PSG_CHANNELS];
	uint8_t volume[PATCH_NUM_PSG_CHANNELS], now_volume[PATCH_NUM_PSG_CHANNELS];
	psg_notes(true);
	psg_state(tone, volume);
	psg_notes(false);
	size_t rom_size;
	const uint8_t *rom = mdmi_engine_builtin_rom(&rom_size);
	size_t memory_before = 0;
	double slowest = 0, total = 0;
	int bad = 0;
	for (int i = 0; i < RESTARTS; i++) {
		const uint32_t boots = mdmi_engine_boots(engine);
		mdmi_engine_set_rom(engine, rom, rom_size);
		// Offline, render waits for the core, so one block spans the whole boot.
		while (mdmi_engine_boots(engine) == boots) {
			render(1);
		}
		patch_rom_booted(patch, emit, NULL);
		render(SETTLE_BLOCKS);
		full_velocity();
		const mdmi_engine_stats stats = mdmi_engine_get_stats(engine);
		slowest = stats.last_boot_ms > slowest ? stats.last_boot_ms : slowest;
		total += stats.last_boot_ms;
		psg_notes(true);
		psg_state(now_tone, now_volume);
		psg_notes(false);
		int wrong = memcmp(tone, now_tone, sizeof(tone)) || memcmp(volume, now_volume, sizeof(volume));
		// The DAC takes voice 6.
		for (int voice = 0; voice < FM_CHANNELS - 1; voice++) {
			wrong += compare(voice, voice, i ? NULL : "after a restart") != 0;
		}
		bad += wrong != 0;
		if (i == 1) {
			// The first restarts may still grow caches of the process.
			memory_before = private_bytes();
		}
	}
	const size_t memory_after = private_bytes();
	printf("    %d restarts: boot %.0f ms on average, slowest %.0f ms; private memory %+.1f MB over the last %d\n", RESTARTS, total / RESTARTS, slowest,
		((double)memory_after - (double)memory_before) / 1048576.0, RESTARTS - 2);
	check(bad == 0, "with the DAC on, after every restart the FM voices and PSG notes, one with its own envelope, are as before");
	check_globals("and the LFO, Ch3 mode and DAC");
	set_global(GLOBAL_DAC, 0);
	sync_and_settle();
	render(DAC_BLOCKS);
	// BlastEm still leaves about 0.5 MB a load behind (ROADMAP); its JIT code was 4 MB more.
	check(memory_after < memory_before + (RESTARTS - 2) * 1048576, "a restart leaves under 1 MB behind");
}

// Boots the core with an image and hands the ROM everything again, as the plugin does.
static void boot_image(const uint8_t *image, size_t size)
{
	const uint32_t boots = mdmi_engine_boots(engine);
	mdmi_engine_set_rom(engine, image, size);
	while (mdmi_engine_boots(engine) == boots) {
		render(1);
	}
	patch_rom_booted(patch, emit, NULL);
	render(SETTLE_BLOCKS);
	full_velocity();
}

// A ramp, so the DAC register says which byte of the sample plays: the driver adds 0x80.
static dac_sample *ramp_sample(int rate, uint32_t length)
{
	dac_sample *s = calloc(1, sizeof(dac_sample));
	s->bytes = malloc(length);
	for (uint32_t i = 0; i < length; i++) {
		s->bytes[i] = (int8_t)((int)(i % 256) - 128);
	}
	snprintf(s->name, sizeof(s->name), "ramp");
	s->sample = (rom_sample){(uint8_t)rate, length, s->bytes, 0};
	return s;
}

// Plays the note on the DAC's channel and follows the ramp in register 0x2A: the bytes must come in
// order, and how many per second is the rate the driver plays.
#define RAMP_BLOCK 16
static double ramp_rate(int key, bool *in_order)
{
	midi(0x95, (uint8_t)key, 127);
	long first_at = -1, last_at = 0, index = 0, last_index = 0;
	int last = -1;
	*in_order = true;
	for (long at = 0; at < RATE / 2; at += RAMP_BLOCK) {
		mdmi_engine_render(engine, left, right, RAMP_BLOCK);
		const int value = mdmi_engine_test_ym(engine, 0, 0x2A);
		if (last < 0) {
			if (value == 0x80) {
				continue;
			}
			first_at = at;
		} else {
			const int step = (value - last + 256) % 256;
			// The emulator steps 128 of its samples at a time, about 75 bytes at 32 kHz: anything
			// under half the ramp's period is the next bytes in order.
			*in_order = *in_order && step < 128;
			index += step;
		}
		last = value;
		last_index = index;
		last_at = at;
	}
	midi(0x85, (uint8_t)key, 0);
	render(SETTLE_BLOCKS);
	*in_order = *in_order && first_at >= 0 && last_index > 0;
	return last_at > first_at ? last_index * (double)RATE / (double)(last_at - first_at) : 0;
}

static void test_samples(void)
{
	size_t rom_size, image_size;
	const uint8_t *rom = mdmi_engine_builtin_rom(&rom_size);
	check(rom_image_check(rom, rom_size), "the built-in image has the sample table the plugin was built for");
	dac_set set = {0};
	// A note of the ROM's and one it has nothing on.
	dac_put(&set, 40, ramp_sample(1, 256 * 64));
	for (int rate = 0; rate < ROM_SAMPLE_RATES; rate++) {
		dac_put(&set, 100 + rate, ramp_sample(rate, 256 * 128));
	}
	uint8_t *image = dac_image(&set, rom, rom_size, &image_size);
	boot_image(image, image_size);
	free(image);
	set_global(GLOBAL_DAC, 1);
	sync_and_settle();
	render(DAC_BLOCKS);
	static const int KEYS[7] = {40, 100, 101, 102, 103, 104, 105}, RATES[7] = {1, 0, 1, 2, 3, 4, 5};
	for (int i = 0; i < 7; i++) {
		bool in_order;
		const double hz = ramp_rate(KEYS[i], &in_order);
		printf("    note %d: %.0f bytes a second, the driver's rate %.0f\n", KEYS[i], hz, ROM_SAMPLE_HZ[RATES[i]]);
		check(in_order && hz > ROM_SAMPLE_HZ[RATES[i]] * 0.99 && hz < ROM_SAMPLE_HZ[RATES[i]] * 1.01,
			i ? "a sample on a note the ROM has none for plays at its rate" : "a sample in place of the ROM's plays its bytes at its rate");
	}
	bool in_order;
	ramp_rate(41, &in_order);
	check(mdmi_engine_test_ym(engine, 0, 0x2A) == 0x80 || !in_order, "the ROM's own sample on the next note is not the ramp");
	set_global(GLOBAL_DAC, 0);
	sync_and_settle();
	dac_clear(&set);
	boot_image(rom, rom_size);
	check_voices("back on the built-in image every voice is as before", 0, FM_CHANNELS - 1, -1);
}

// A sine as a WAV, for the prepared samples.
static uint8_t *sine_wav(uint32_t rate, double hz, double seconds, size_t *size)
{
	const uint32_t frames = (uint32_t)(rate * seconds);
	*size = 44 + (size_t)frames * 2;
	uint8_t *wav = calloc(1, *size);
	uint32_t u;
	uint16_t h;
	memcpy(wav, "RIFF", 4);
	u = (uint32_t)*size - 8;
	memcpy(wav + 4, &u, 4);
	memcpy(wav + 8, "WAVEfmt ", 8);
	u = 16;
	memcpy(wav + 16, &u, 4);
	h = 1;
	memcpy(wav + 20, &h, 2);
	memcpy(wav + 22, &h, 2);
	memcpy(wav + 24, &rate, 4);
	u = rate * 2;
	memcpy(wav + 28, &u, 4);
	h = 2;
	memcpy(wav + 32, &h, 2);
	h = 16;
	memcpy(wav + 34, &h, 2);
	memcpy(wav + 36, "data", 4);
	u = frames * 2;
	memcpy(wav + 40, &u, 4);
	for (uint32_t i = 0; i < frames; i++) {
		const int16_t v = (int16_t)lround(16000 * sin(2 * 3.14159265358979 * hz * i / rate));
		memcpy(wav + 44 + i * 2, &v, 2);
	}
	return wav;
}

// Plays the note on the DAC's channel and measures the sound that comes out: rising crossings in the
// left channel over half a second, after the start, with hysteresis against the 8-bit steps.
static double played_hz(int key)
{
	midi(0x95, (uint8_t)key, 127);
	render(4);
	long crossings = 0, first = -1, last = 0, at = 0;
	bool low = false;
	for (int block = 0; block < RATE / 2 / BLOCK; block++) {
		mdmi_engine_render(engine, left, right, BLOCK);
		for (int i = 0; i < BLOCK; i++, at++) {
			if (left[i] < -0.01f) {
				low = true;
			} else if (low && left[i] > 0.01f) {
				low = false;
				first = first < 0 ? at : first;
				last = at;
				crossings++;
			}
		}
	}
	midi(0x85, (uint8_t)key, 0);
	render(SETTLE_BLOCKS);
	return crossings > 1 ? (crossings - 1) * (double)RATE / (last - first) : 0;
}

// The loudest sample of the left channel over some blocks.
static float loudest(int blocks)
{
	float most = 0;
	for (int b = 0; b < blocks; b++) {
		mdmi_engine_render(engine, left, right, BLOCK);
		for (int i = 0; i < BLOCK; i++) {
			most = fabsf(left[i]) > most ? fabsf(left[i]) : most;
		}
	}
	return most;
}

static void boot_set(const dac_set *set)
{
	size_t rom_size, image_size;
	const uint8_t *rom = mdmi_engine_builtin_rom(&rom_size);
	uint8_t *image = dac_image(set, rom, rom_size, &image_size);
	boot_image(image, image_size);
	free(image);
	set_global(GLOBAL_DAC, 1);
	sync_and_settle();
	render(DAC_BLOCKS);
}

// Prepared samples as the emulator plays them (D38): pitch and rate by ear, one-shot past Note Off.
static void test_shaping(void)
{
	size_t size;
	uint8_t *wav = sine_wav(22050, 440, 2.0, &size);
	const char *error;
	dac_set set = {0};
	dac_put(&set, 50, dac_from_wav(wav, size, "sine.wav", &error));
	free(wav);
	static const struct {
		int16_t pitch;
		int8_t rate;
	} EDITS[] = {{0, DAC_RATE_AUTO}, {1200, DAC_RATE_AUTO}, {-700, DAC_RATE_AUTO}, {2400, DAC_RATE_AUTO}, {-2400, DAC_RATE_AUTO}, {0, 4}};
	for (size_t i = 0; i < sizeof(EDITS) / sizeof(EDITS[0]); i++) {
		dac_edit edit = set.notes[50]->edit;
		edit.pitch = EDITS[i].pitch;
		edit.rate = EDITS[i].rate;
		dac_prepare(set.notes[50], &edit);
		set.version++;
		boot_set(&set);
		const double wanted = 440 * pow(2.0, EDITS[i].pitch / 1200.0), hz = played_hz(50);
		printf("    pitch %+d cents, rate %d: %.1f Hz played, %.1f wanted\n", EDITS[i].pitch, set.notes[50]->sample.rate, hz, wanted);
		check(fabs(hz / wanted - 1) < 0.01, EDITS[i].rate >= 0 ? "a rate by hand keeps the pitch" : "a sample prepared with a pitch sounds at it");
	}
	// One-shot: Note Off after 100 ms; the driver would stop within 50 ms.
	for (int shot = 0; shot < 2; shot++) {
		dac_edit edit = set.notes[50]->edit;
		edit.rate = DAC_RATE_AUTO;
		edit.one_shot = shot;
		dac_prepare(set.notes[50], &edit);
		set.version++;
		boot_set(&set);
		midi(0x95, 50, 127);
		render(RATE / 10 / BLOCK);
		midi(0x85, 50, 0);
		render(RATE / 10 / BLOCK);
		const float after = loudest(RATE / 10 / BLOCK);
		printf("    %s: loudest %.3f from 100 to 200 ms after Note Off\n", shot ? "one-shot" : "plain", after);
		check(shot ? after > 0.05f : after < 0.01f, shot ? "a one-shot sample plays on after Note Off" : "a plain one stops at Note Off");
	}
	set_global(GLOBAL_DAC, 0);
	sync_and_settle();
	dac_clear(&set);
	size_t rom_size;
	const uint8_t *rom = mdmi_engine_builtin_rom(&rom_size);
	boot_image(rom, rom_size);
}

static void test_load(void)
{
	static uint8_t state[PATCH_NUM_IDS * 6];
	patch_publish(patch);
	patch_save(patch_view(patch), state);
	set_on(0, PARAM_PRESET, 99);
	set_on(1, PARAM_OP_BASE + F_TL, 12);
	set_on(5, PARAM_OP_BASE + F_SR, 30);
	patch_load(patch, state, sizeof(state), NULL, NULL, NULL);
	patch_take_incoming(patch);
	sync_and_settle();
	check_voices("a loaded state reaches the chip", 0, FM_CHANNELS - 1, -1);
}

// Stage 4's first step, bytes by hand and past the model: does SysEx 00, a program change to the
// number the channel already has, and controllers for the difference give a lent voice exactly
// the sound of the channel it joins (D20)? Leaves the model's copy of the ROM wrong; a reset follows.
static void raw(const uint8_t *bytes, size_t length)
{
	mdmi_engine_midi(engine, 0, bytes, length);
}

static void raw3(uint8_t a, uint8_t b, uint8_t c)
{
	const uint8_t message[3] = {a, b, c};
	raw(message, (a & 0xF0) == 0xC0 ? 2 : 3);
}

static void remap(int channel, int voice)
{
	const uint8_t sysex[8] = {0xF0, 0x00, 0x22, 0x77, 0x00, (uint8_t)channel, (uint8_t)voice, 0xF7};
	raw(sysex, sizeof(sysex));
}

// The controller of each voice param and its range, as the ROM scales it.
static int voice_cc(int param, int *range)
{
	static const uint8_t OP_CC[11] = {16, 20, 24, 39, 43, 47, 51, 55, 59, 70, 90};
	static const uint8_t OP_RANGE[11] = {128, 16, 8, 4, 32, 32, 16, 16, 16, 2, 16};
	static const uint8_t CC[4] = {14, 15, 76, 75}, RANGE[4] = {8, 8, 4, 8};
	if (param < PARAM_OP_BASE) {
		*range = RANGE[param - PARAM_ALGORITHM];
		return CC[param - PARAM_ALGORITHM];
	}
	int op = (param - PARAM_OP_BASE) / PATCH_OP_PARAMS, i = (param - PARAM_OP_BASE) % PATCH_OP_PARAMS;
	*range = OP_RANGE[i];
	return OP_CC[i] + op;
}

// Brings a voice that holds `from` to the model's `channel`: PC to the channel's own number, then
// controllers for each value that differs, sustain rates above 15 straight into the voice's register.
// Returns the bytes sent.
static int send_difference(int voice, int channel, const int16_t *from)
{
	int bytes = 0;
	int program = model_value(channel, PARAM_PRESET);
	bool loads = from[PARAM_PRESET] != program;
	raw3((uint8_t)(0xC0 | channel), (uint8_t)program, 0);
	bytes += 2;
	for (int param = PARAM_ALGORITHM; param < PATCH_NUM_CHANNEL_PARAMS; param++) {
		int had = loads ? patch_preset_value(&patch->bank, channel, param, program) : from[param];
		int want = model_value(channel, param);
		if (had == want) {
			continue;
		}
		int range, cc = voice_cc(param, &range);
		if (want < range) {
			raw3((uint8_t)(0xB0 | channel), (uint8_t)cc, (uint8_t)(want * (128 / range)));
			bytes += 3;
		} else {
			int op = (param - PARAM_OP_BASE) / PATCH_OP_PARAMS, slot = op == 1 ? 2 : op == 2 ? 1 : op;
			int reg = 0x70 + voice % 3 + slot * 4;
			const uint8_t sysex[10] = {0xF0, 0x00, 0x22, 0x77, (uint8_t)(voice < 3 ? 0x08 : 0x09), (uint8_t)(reg >> 4), (uint8_t)(reg & 15), (uint8_t)(want >> 4), (uint8_t)(want & 15), 0xF7};
			raw(sysex, sizeof(sysex));
			bytes += 10;
		}
	}
	return bytes;
}

#define KEY_BLOCK 16

static uint8_t key_on_value(int voice)
{
	return (uint8_t)(0xF0 | (voice < 3 ? voice : voice + 1));
}

// Emulated time until the chip's key on/off register shows the voice keyed on, in ms. With a first
// voice, the time from that voice's key on instead: a plain note sent just before is the measure of
// what the engine adds to every note, so the rest is what the ROM spends on the transition.
static double time_to_key_on(int first, int voice)
{
	int blocks = 0, first_at = 0;
	bool first_seen = first < 0;
	for (; blocks < 2000; blocks++) {
		uint8_t key = mdmi_engine_test_ym(engine, 0, 0x28);
		if (!first_seen && key == key_on_value(first)) {
			first_seen = true;
			first_at = blocks;
		}
		if (first_seen && key == key_on_value(voice)) {
			break;
		}
		mdmi_engine_render(engine, left, right, KEY_BLOCK);
	}
	return (blocks - first_at) * KEY_BLOCK * 1000.0 / RATE;
}

static void lend_by_hand(const char *what)
{
	int16_t ch2[PATCH_NUM_CHANNEL_PARAMS], ch4[PATCH_NUM_CHANNEL_PARAMS];
	memcpy(ch2, patch->model.channels[1], sizeof(ch2));
	memcpy(ch4, patch->model.channels[3], sizeof(ch4));
	printf("  %s\n", what);
	check_voices("before: voice 2 holds channel 2", 1, 1, -1);
	check_voices("before: voice 4 holds channel 4", 3, 3, -1);

	// Channel 4's own voice is busy, so the chord's second note needs voice 2. A plain note on
	// channel 5 goes just before it.
	raw3(0x93, 60, 127);
	render(SETTLE_BLOCKS);
	raw3(0x94, 60, 127);
	double plain = time_to_key_on(-1, 4);
	raw3(0x84, 60, 0);
	render(SETTLE_BLOCKS);
	raw3(0x94, 60, 127);
	remap(3, 1);
	int bytes = 8 + send_difference(1, 3, ch2);
	raw3(0x93, 64, 127);
	bytes += 3;
	double lent = time_to_key_on(4, 1);
	render(SETTLE_BLOCKS);
	raw3(0x84, 60, 0);
	render(SETTLE_BLOCKS);
	check_voices("voice 2 lent to channel 4 holds channel 4", 1, 1, 3);
	check_voices("and voice 4 still does", 3, 3, -1);
	printf("    transition key on %.2f ms after a plain note, which takes %.2f ms; %d bytes, %.0f ms at 480 bytes/s\n", lent, plain, bytes, bytes * 1000.0 / 480);

	// The chord's note ends and the voice goes home with nothing but SysEx 00; its tail plays on.
	raw3(0x83, 64, 0);
	remap(1, 1);
	raw3(0x83, 60, 0);
	render(SETTLE_BLOCKS);
	check_voices("home again, the voice keeps channel 4's sound until channel 2 plays", 1, 1, 3);
	raw3(0x94, 60, 127);
	send_difference(1, 1, ch4);
	raw3(0x91, 60, 127);
	double home = time_to_key_on(4, 1);
	raw3(0x84, 60, 0);
	render(SETTLE_BLOCKS);
	raw3(0x81, 60, 0);
	render(SETTLE_BLOCKS);
	check_voices("channel 2's first note: voice 2 holds channel 2", 1, 1, -1);
	check_voices("and voice 4 channel 4", 3, 3, -1);
	printf("    at home, key on %.2f ms after a plain note\n", home);
}

#define CHORD 4

// Emulated time of each voice's first key on after the bytes queued so far, in ms from the first.
static void chord_key_ons(const int *voices, int count, double *at)
{
	const int block = 4;
	bool seen[FM_CHANNELS] = {false};
	int seen_count = 0, blocks = 0, first = -1;
	for (; seen_count < count && blocks < 20000; blocks++) {
		uint8_t key = mdmi_engine_test_ym(engine, 0, 0x28);
		for (int i = 0; i < count; i++) {
			if (!seen[i] && key == key_on_value(voices[i])) {
				seen[i] = true;
				seen_count++;
				first = first < 0 ? blocks : first;
				at[i] = (blocks - first) * block * 1000.0 / RATE;
			}
		}
		mdmi_engine_render(engine, left, right, (uint32_t)block);
	}
	for (int i = 0; i < count; i++) {
		if (!seen[i]) {
			at[i] = -1;
		}
	}
}

// A chord of four on channel 4: its own voice first, then voices 1-3, each brought to channel 4
// one at a time, or all remapped first with one PC and one difference for all of them.
static void chord_by_hand(const char *what, bool batch, const int16_t *const *had)
{
	static const int VOICES[CHORD] = {3, 0, 1, 2};
	const uint8_t keys[CHORD] = {60, 64, 67, 71};
	raw3(0x93, keys[0], 127);
	int bytes = 0;
	if (batch) {
		for (int i = 1; i < CHORD; i++) {
			remap(3, VOICES[i]);
			bytes += 8;
		}
		// One PC loads every voice with another number; the difference goes once, to all of them.
		bytes += send_difference(VOICES[1], 3, had[1]);
		for (int i = 1; i < CHORD; i++) {
			raw3(0x93, keys[i], 127);
			bytes += 3;
		}
	} else {
		for (int i = 1; i < CHORD; i++) {
			remap(3, VOICES[i]);
			bytes += 8 + send_difference(VOICES[i], 3, had[i]) + 3;
			raw3(0x93, keys[i], 127);
		}
	}
	double at[CHORD];
	chord_key_ons(VOICES, CHORD, at);
	render(SETTLE_BLOCKS);
	printf("    %s: key ons at", what);
	for (int i = 0; i < CHORD; i++) {
		printf(" %.2f", at[i]);
	}
	printf(" ms, %d bytes besides the notes (%.0f ms at 480 bytes/s)\n", bytes, bytes * 1000.0 / 480);
	check_voices("the chord's voices hold channel 4", 0, CHORD - 1, 3);
	for (int i = 0; i < CHORD; i++) {
		raw3(0x83, keys[i], 0);
		if (VOICES[i] != 3) {
			remap(VOICES[i], VOICES[i]);
		}
	}
	render(SETTLE_BLOCKS);
}

// Each voice back to its own channel's sound, by a note of that channel.
static void voices_home(const int16_t *ch4)
{
	for (int voice = 0; voice < CHORD - 1; voice++) {
		send_difference(voice, voice, ch4);
		raw3((uint8_t)(0x90 | voice), 60, 127);
		render(SETTLE_BLOCKS);
		raw3((uint8_t)(0x80 | voice), 60, 0);
		render(SETTLE_BLOCKS);
	}
	check_voices("voices 1-3 hold their own channels again", 0, CHORD - 2, -1);
}

static void test_chord_by_hand(void)
{
	static const int PRESET[CHORD] = {10, 20, 50, 40};
	for (int channel = 0; channel < CHORD; channel++) {
		set_on(channel, PARAM_PRESET, PRESET[channel]);
		set_on(channel, PARAM_OP_BASE + F_TL, 30 + channel);
	}
	set_on(3, PARAM_OP_BASE + PATCH_OP_PARAMS + F_TL, 50);
	set_on(3, PARAM_OP_BASE + 2 * PATCH_OP_PARAMS + F_MUL, 3);
	set_on(3, PARAM_OP_BASE + 3 * PATCH_OP_PARAMS + F_AR, 12);
	set_on(3, PARAM_FEEDBACK, 2);
	static int16_t own[FM_CHANNELS][PATCH_NUM_CHANNEL_PARAMS];
	memcpy(own, patch->model.channels, sizeof(own));
	const int16_t *cold[CHORD] = {own[3], own[0], own[1], own[2]};
	const int16_t *warm[CHORD] = {own[3], own[3], own[3], own[3]};
	printf("  by hand, a chord of 4 on channel 4 with 5 tweaks, the others on other presets\n");
	chord_by_hand("cold, one at a time", false, cold);
	voices_home(own[3]);
	chord_by_hand("cold, in one batch", true, cold);
	chord_by_hand("warm, voices home but still channel 4's, in one batch", true, warm);
	voices_home(own[3]);
}

static void test_voice_by_hand(void)
{
	test_chord_by_hand();
	set_on(1, PARAM_PRESET, 30);
	set_on(1, PARAM_OP_BASE + F_TL, 20);
	set_on(1, PARAM_OP_BASE + 2 * PATCH_OP_PARAMS + F_MUL, 5);
	set_on(3, PARAM_PRESET, 40);
	set_on(3, PARAM_OP_BASE + F_TL, 70);
	set_on(3, PARAM_OP_BASE + PATCH_OP_PARAMS + F_SR, 25);
	set_on(3, PARAM_OP_BASE + 3 * PATCH_OP_PARAMS + F_AR, 10);
	lend_by_hand("by hand, channels 2 and 4 on different presets");
	set_on(1, PARAM_PRESET, 40);
	set_on(1, PARAM_OP_BASE + F_TL, 20);
	set_on(1, PARAM_OP_BASE + 2 * PATCH_OP_PARAMS + F_MUL, 5);
	set_on(1, PARAM_OP_BASE + 2 * PATCH_OP_PARAMS + F_SR, 28);
	lend_by_hand("by hand, both on preset 41 with different tweaks");
}

// Stage 4 through the dealer in patch.c, the way the plugin plays (D20, D21).

#define SNAPSHOT_REGS 32

// Everything of a voice's sound the chip holds, and the pitch it plays.
static void snapshot(int voice, uint8_t *regs)
{
	int n = 0;
	for (int slot = 0; slot < 4; slot++) {
		for (int base = 0x30; base <= 0x90; base += 0x10) {
			regs[n++] = read_reg(voice, base, slot);
		}
	}
	regs[n++] = read_reg(voice, 0xB0, 0);
	regs[n++] = read_reg(voice, 0xB4, 0);
	regs[n++] = read_reg(voice, 0xA0, 0);
	regs[n++] = read_reg(voice, 0xA4, 0);
}

static void snapshot_all(uint8_t regs[FM_CHANNELS][SNAPSHOT_REGS])
{
	for (int voice = 0; voice < FM_CHANNELS; voice++) {
		snapshot(voice, regs[voice]);
	}
}

static int changed_voices(uint8_t before[FM_CHANNELS][SNAPSHOT_REGS], int skip)
{
	uint8_t now[SNAPSHOT_REGS];
	int changed = 0;
	for (int voice = 0; voice < FM_CHANNELS; voice++) {
		snapshot(voice, now);
		changed += voice != skip && memcmp(now, before[voice], SNAPSHOT_REGS) != 0;
	}
	return changed;
}

static void check(bool passed, const char *what)
{
	printf("  %s: %s\n", passed ? "ok  " : "FAIL", what);
	if (!passed) {
		failures++;
	}
}

// Notes of one moment on a channel, from the given key up in thirds.
static void moment(int channel, int first, int count, bool on)
{
	for (int i = 0; i < count; i++) {
		const uint8_t message[3] = {(uint8_t)((on ? 0x90 : 0x80) | channel), (uint8_t)(first + i * 4), on ? 127 : 0};
		patch_midi(patch, message, 3, emit, NULL);
	}
	patch_flush(patch, emit, NULL);
}

// The voices playing a channel's notes.
static int voices_of(int channel, int *voices)
{
	int count = 0;
	for (int voice = 0; voice < FM_CHANNELS; voice++) {
		if (patch->voices[voice].channel == channel) {
			voices[count++] = voice;
		}
	}
	return count;
}

static int chord_equals_model(int channel)
{
	int voices[FM_CHANNELS], count = voices_of(channel, voices), same = 0;
	for (int i = 0; i < count; i++) {
		same += compare(voices[i], channel, "chord") == 0;
	}
	return same;
}

static void tweak_channels(void)
{
	static const int PRESET[FM_CHANNELS] = {10, 20, 30, 40, 50, 60};
	for (int channel = 0; channel < FM_CHANNELS; channel++) {
		set_on(channel, PARAM_PRESET, PRESET[channel]);
		set_on(channel, PARAM_OP_BASE + F_TL, 20 + channel);
	}
	// Channel 4: TL by CC, SR above 15 by knob, and a few more.
	midi(0xB3, 17, 50);
	set_on(3, PARAM_OP_BASE + 2 * PATCH_OP_PARAMS + F_SR, 25);
	set_on(3, PARAM_OP_BASE + 2 * PATCH_OP_PARAMS + F_MUL, 3);
	set_on(3, PARAM_OP_BASE + 3 * PATCH_OP_PARAMS + F_AR, 12);
	set_on(3, PARAM_FEEDBACK, 2);
	render(SETTLE_BLOCKS);
}

// A chord's notes start once all its voices have their sound. How long after a plain note just
// before it the chord's last key on comes, in ms: what the transition adds to the engine's latency.
static double chord_delay(int channel, int count)
{
	moment(FM_CHANNELS - 1, 96, 1, true);
	moment(channel, 60, count, true);
	int voices[FM_CHANNELS + 1] = {FM_CHANNELS - 1};
	int playing = voices_of(channel, voices + 1);
	double at[FM_CHANNELS + 1], last = 0;
	chord_key_ons(voices, playing + 1, at);
	for (int i = 1; i <= playing; i++) {
		last = at[i] > last ? at[i] : last;
	}
	render(SETTLE_BLOCKS);
	moment(FM_CHANNELS - 1, 96, 1, false);
	return last;
}

// A note on each FM channel, so every voice has its own channel's sound and pan again.
static void play_each_channel(void)
{
	for (int channel = 0; channel < FM_CHANNELS; channel++) {
		moment(channel, 72, 1, true);
		render(SETTLE_BLOCKS);
		moment(channel, 72, 1, false);
	}
	render(SETTLE_BLOCKS);
}

static void test_dealer(void)
{
	printf("  the voice dealer\n");
	tweak_channels();
	double cold = chord_delay(3, 4);
	check(chord_equals_model(3) == 4, "a chord of 4 on channel 4 with tweaks: four voices hold channel 4");
	moment(3, 60, 4, false);
	render(SETTLE_BLOCKS);
	double warm = chord_delay(3, 4);
	check(chord_equals_model(3) == 4, "the same chord again, its voices still channel 4's");
	printf("    the chord's last key on after a plain note: %.2f ms cold, %.2f ms again\n", cold, warm);
	check(cold < 25 && warm < 10, "cold under 25 ms, again under 10 ms");
	moment(3, 60, 4, false);
	render(SETTLE_BLOCKS);

	// A chord of 5 leaves one voice free. A tune on a channel whose voice the chord holds takes its
	// own voice back, and the chord's note there moves to the free one.
	moment(3, 60, 5, true);
	render(SETTLE_BLOCKS);
	int lender = -1;
	for (int voice = 0; voice < FM_CHANNELS; voice++) {
		lender = lender < 0 && voice != 3 && patch->voices[voice].channel == 3 ? voice : lender;
	}
	uint8_t before[FM_CHANNELS][SNAPSHOT_REGS];
	moment(lender, 84, 1, true);
	render(SETTLE_BLOCKS);
	int tune[FM_CHANNELS];
	check(voices_of(lender, tune) == 1 && tune[0] == lender && compare(lender, lender, "tune") == 0, "a tune whose voice a chord holds takes it back with its own sound");
	check(chord_equals_model(3) == 5 && patch_muted_notes(patch, 3) == 0, "and the chord's note moves to the free voice, nothing cut");
	moment(lender, 84, 1, false);
	moment(3, 60, 5, false);
	render(SETTLE_BLOCKS);

	// A chord of 6 leaves nothing free, so channel 2's tune takes its voice back.
	moment(3, 48, 6, true);
	render(SETTLE_BLOCKS);
	snapshot_all(before);
	midi(0x91, 72, 127);
	render(SETTLE_BLOCKS);
	check_voices("with no voice free, channel 2's note takes its own back with its own sound", 1, 1, -1);
	check(changed_voices(before, 1) == 0 && chord_equals_model(3) == 5, "and the rest of the chord is untouched, pitch too");
	check(patch_muted_notes(patch, 3) == 1, "the cut note is marked");
	midi(0x81, 72, 0);
	moment(3, 48, 6, false);
	render(SETTLE_BLOCKS);
	check(patch_muted_notes(patch, 3) == 0, "no mark when the chord ends");

	// A voice home again with channel 4's sound, its own channel then on the same preset with other
	// tweaks: the ROM skips a program change to the number the voice has.
	moment(3, 60, 2, true);
	render(SETTLE_BLOCKS);
	int lent = patch->voices[3].channel == 3 && patch->voices[0].channel == 3 ? 0 : -1;
	for (int voice = 0; voice < FM_CHANNELS; voice++) {
		lent = voice != 3 && patch->voices[voice].channel == 3 ? voice : lent;
	}
	moment(3, 60, 2, false);
	render(SETTLE_BLOCKS);
	check(lent >= 0, "a chord of 2 lends a voice");
	if (lent >= 0) {
		set_on(lent, PARAM_PRESET, model_value(3, PARAM_PRESET));
		set_on(lent, PARAM_OP_BASE + F_TL, 5);
		moment(lent, 72, 1, true);
		render(SETTLE_BLOCKS);
		check_voices("home again: its channel's first note on the same preset has that channel's sound", lent, lent, -1);
		moment(lent, 72, 1, false);
		render(SETTLE_BLOCKS);
	}

	// Too many notes.
	moment(3, 48, 6, true);
	render(SETTLE_BLOCKS);
	snapshot_all(before);
	moment(3, 80, 1, true);
	render(SETTLE_BLOCKS);
	check(changed_voices(before, -1) == 0 && patch_muted_notes(patch, 3) == 1 && chord_equals_model(3) == 6, "the 7th note doesn't sound, changes nothing and is marked");
	moment(3, 80, 1, false);
	moment(3, 48, 6, false);
	render(SETTLE_BLOCKS);

	// Porta On: the channel plays alone.
	set_on(3, PARAM_PORTA_ON, 1);
	moment(3, 60, 3, true);
	render(SETTLE_BLOCKS);
	int voices[FM_CHANNELS];
	check(voices_of(3, voices) == 1 && voices[0] == 3, "Porta On: channel 4 takes no other voice");
	moment(0, 60, 2, true);
	render(SETTLE_BLOCKS);
	check(patch->voices[3].channel == 3, "and lends none");
	moment(0, 60, 2, false);
	moment(3, 60, 3, false);
	set_on(3, PARAM_PORTA_ON, 0);
	render(SETTLE_BLOCKS);

	// Ch3 Special keeps voice 3.
	set_global(GLOBAL_CH3_MODE, 1);
	sync_and_settle();
	snapshot(2, before[2]);
	moment(3, 48, 6, true);
	render(SETTLE_BLOCKS);
	uint8_t now[SNAPSHOT_REGS];
	snapshot(2, now);
	check(memcmp(now, before[2], sizeof(now)) == 0 && patch->voices[2].channel < 0 && patch_muted_notes(patch, 3) == 1, "Ch3 Special: voice 3 stays, the 6th note is marked");
	moment(3, 48, 6, false);
	set_global(GLOBAL_CH3_MODE, 0);
	sync_and_settle();

	// The DAC keeps voice 6.
	play_each_channel();
	set_global(GLOBAL_DAC, 1);
	wait_for_voices(FM_CHANNELS - 2);
	snapshot(5, before[5]);
	moment(3, 48, 6, true);
	render(SETTLE_BLOCKS);
	snapshot(5, now);
	check(memcmp(now, before[5], sizeof(now)) == 0 && patch->voices[5].channel < 0 && patch_muted_notes(patch, 3) == 1 && chord_equals_model(3) == 5, "DAC: voice 6 stays, five notes sound, the 6th is marked");
	moment(3, 48, 6, false);
	play_each_channel();
	set_global(GLOBAL_DAC, 0);
	wait_for_voices(FM_CHANNELS - 1);
}

// Stage 5: instrument files. What each file means for the chip is worked out here straight from its
// bytes by the formats' specs, not by the loader, so a loader error can't check itself.

#define FILE_MAX 64

typedef struct {
	const char *name;
	// The registers of each operator slot in register order, 30 40 50 60 70 80 90, then B0 and B4's low bits.
	uint8_t regs[4][7];
	uint8_t b0, b4;
} file_regs;

static const char *files_dir = "../../tools/test/files";

static size_t read_file(const char *name, uint8_t *bytes)
{
	char path[512];
	snprintf(path, sizeof(path), "%s/%s", files_dir, name);
	FILE *file = fopen(path, "rb");
	if (!file) {
		return 0;
	}
	size_t size = fread(bytes, 1, FILE_MAX, file);
	fclose(file);
	return size;
}

// TFI and VGI: per operator mul dt tl rs ar dr sr rr sl ssg, detune centred on 3, operators in
// register order. VGI has fms | ams << 4 in byte 2 and AM in the top bit of DR.
static void regs_centred(const uint8_t *bytes, bool vgi, file_regs *out)
{
	static const uint8_t DT[8] = {7, 6, 5, 0, 1, 2, 3, 4};
	const uint8_t *op = bytes + (vgi ? 3 : 2);
	for (int slot = 0; slot < 4; slot++, op += 10) {
		uint8_t *r = out->regs[slot];
		r[0] = (uint8_t)(DT[op[1]] << 4 | op[0]);
		r[1] = op[2];
		r[2] = (uint8_t)(op[3] << 6 | op[4]);
		r[3] = op[5];
		r[4] = op[6];
		r[5] = (uint8_t)(op[8] << 4 | op[7]);
		r[6] = op[9];
	}
	out->b0 = (uint8_t)(bytes[1] << 3 | bytes[0]);
	out->b4 = vgi ? bytes[2] : 0;
}

// TYI: register values, four of each register in register order, then FB/ALG and AMS/FMS.
static void regs_tyi(const uint8_t *bytes, file_regs *out)
{
	for (int slot = 0; slot < 4; slot++) {
		for (int reg = 0; reg < 7; reg++) {
			out->regs[slot][reg] = bytes[reg * 4 + slot];
		}
	}
	out->b0 = bytes[28];
	out->b4 = bytes[29];
}

static bool file_regs_of(const char *name, file_regs *out)
{
	uint8_t bytes[FILE_MAX];
	size_t size = read_file(name, bytes);
	const char *dot = strrchr(name, '.');
	out->name = name;
	if (size == 42 && !strcmp(dot, ".tfi")) {
		regs_centred(bytes, false, out);
	} else if (size == 43 && !strcmp(dot, ".vgi")) {
		regs_centred(bytes, true, out);
	} else if (size == 32 && !strcmp(dot, ".tyi")) {
		regs_tyi(bytes, out);
	} else {
		printf("  FAIL: %s can't be read from %s\n", name, files_dir);
		failures++;
		return false;
	}
	return true;
}

// How many registers of a voice differ from what the file means.
static int compare_file(int voice, const file_regs *file, bool say)
{
	static const char *const NAMES[7] = {"DT/MUL", "TL", "RS/AR", "AM/DR", "SR", "SL/RR", "SSG"};
	int wrong = 0;
	for (int slot = 0; slot < 4; slot++) {
		for (int reg = 0; reg < 7; reg++) {
			int got = read_reg(voice, 0x30 + reg * 0x10, slot), want = file->regs[slot][reg];
			if (got != want) {
				if (say && wrong < 2) {
					printf("    %s voice %d slot %d %s: register %d, file %d\n", file->name, voice + 1, slot + 1, NAMES[reg], got, want);
				}
				wrong++;
			}
		}
	}
	wrong += read_reg(voice, 0xB0, 0) != file->b0;
	wrong += (read_reg(voice, 0xB4, 0) & 0x37) != file->b4;
	return wrong;
}

static bool load_file(const char *name, patch_instrument *instrument)
{
	uint8_t bytes[FILE_MAX];
	size_t size = read_file(name, bytes);
	char error[96];
	if (!instrument_parse(name, bytes, size, instrument, error, sizeof(error))) {
		printf("  FAIL: %s: %s\n", name, error);
		failures++;
		return false;
	}
	return true;
}

// A plain note on voice 6 just before, so the time is what the transition adds to every note.
static double drop_and_play(int channel, const patch_instrument *instrument)
{
	moment(FM_CHANNELS - 1, 96, 1, true);
	patch_load_voice(patch, channel, instrument, NULL);
	patch_take_incoming(patch);
	patch_sync(patch, &patch->rom, emit, NULL);
	moment(channel, 60, 1, true);
	double ms = time_to_key_on(FM_CHANNELS - 1, channel);
	render(SETTLE_BLOCKS);
	moment(channel, 60, 1, false);
	moment(FM_CHANNELS - 1, 96, 1, false);
	render(SETTLE_BLOCKS);
	return ms;
}

static const char *const FILES[] = {
	"Duck Guitar.tfi", "Turtles Brass 02.tfi", "Turtles Guitar.tfi",
	"BAS_Magical_Bass.vgi", "BAS_PSG2.vgi", "DGL_4_o4a.vgi", "GTR_Dis._'94.vgi", "GTR_New_Dis..vgi", "OTHER_chip2.vgi",
	"2019 Bass 02.tyi", "Thunder Guitar 03.tyi",
};
#define FILE_COUNT (int)(sizeof(FILES) / sizeof(*FILES))

static void test_files(void)
{
	printf("  instrument files\n");
	double slowest = 0;
	for (int i = 0; i < FILE_COUNT; i++) {
		file_regs expected;
		patch_instrument instrument;
		if (!file_regs_of(FILES[i], &expected) || !load_file(FILES[i], &instrument)) {
			continue;
		}
		const int channel = i % 5;
		double ms = drop_and_play(channel, &instrument);
		slowest = ms > slowest ? ms : slowest;
		int wrong = compare_file(channel, &expected, true);
		bool named = !strncmp(patch->model.names[channel], FILES[i], strlen(patch->model.names[channel])) && patch->model.names[channel][0];
		printf("  %s: %s on channel %d, %d registers differ, key on %.2f ms after a plain note, named \"%s\"\n", wrong || !named ? "FAIL" : "ok  ", FILES[i], channel + 1, wrong, ms, patch->model.names[channel]);
		failures += wrong || !named;
	}
	check(slowest <= 15, "a dropped file reaches the ROM within 15 ms");
	printf("    slowest %.2f ms\n", slowest);

	// What the threshold of D24 weighs: the same note after a few values changed, which go as CC.
	for (int changed = 4; changed <= 16; changed += 4) {
		moment(FM_CHANNELS - 1, 96, 1, true);
		for (int i = 0; i < changed; i++) {
			int param = PARAM_OP_BASE + (i % PATCH_OPS) * PATCH_OP_PARAMS + (i / PATCH_OPS == 0 ? F_TL : i / PATCH_OPS == 1 ? F_MUL : i / PATCH_OPS == 2 ? F_AR : F_RR);
			patch_set(patch, PATCH_CHANNEL_ID(0, param), (model_value(0, param) + 1) % 8);
		}
		patch_sync(patch, &patch->rom, emit, NULL);
		moment(0, 60, 1, true);
		double ms = time_to_key_on(FM_CHANNELS - 1, 0);
		render(SETTLE_BLOCKS);
		moment(0, 60, 1, false);
		moment(FM_CHANNELS - 1, 96, 1, false);
		render(SETTLE_BLOCKS);
		printf("    %d values changed: key on %.2f ms after a plain note\n", changed, ms);
	}
	check_voices("and the voice holds them", 0, 0, -1);

	// What other programs agree on, written down by hand. Duck Guitar is one of the six sounds of
	// AriaHivi(ACSTC)2.vgm, which holds the very registers for its first operator.
	patch_instrument instrument;
	if (load_file("Duck Guitar.tfi", &instrument)) {
		drop_and_play(0, &instrument);
		// Registers 30-90 of each slot at the song's key ons; only the carrier's TL changes there.
		const uint8_t VGM[4][7] = {
			{0x51, 0x12, 0x1D, 0x00, 0x01, 0x10, 0x00},
			{0x01, 0x06, 0x1D, 0x0A, 0x01, 0x80, 0x00},
			{0x00, 0x1A, 0x1C, 0x1F, 0x00, 0x51, 0x00},
			{0x02, 0x00, 0x1F, 0x0A, 0x00, 0x09, 0x00},
		};
		bool same = read_reg(0, 0xB0, 0) == (4 << 3 | 0);
		for (int slot = 0; slot < 4; slot++) {
			for (int reg = 0; reg < 7; reg++) {
				same = same && (slot == 3 && reg == 1 ? true : read_reg(0, 0x30 + reg * 0x10, slot) == VGM[slot][reg]);
			}
		}
		check(same, "Duck Guitar holds the registers of the VGM it came from, every slot: operators 1, 3, 2, 4 and detune centred on 3");
	}
	if (load_file("Thunder Guitar 03.tyi", &instrument)) {
		drop_and_play(1, &instrument);
		check(read_reg(1, 0x70, 3) == 29 && read_reg(1, 0x30, 1) >> 4 == 7, "Thunder Guitar: SR 29 above 15 and raw detune 7 reach the chip");
	}
	if (load_file("2019 Bass 02.tyi", &instrument)) {
		drop_and_play(4, &instrument);
		check(read_reg(4, 0x70, 3) == 18, "2019 Bass: SR 18 on the chip's second part");
	}

	// Algorithm 4: operators 2 and 4 are the carriers, so swapping 2 and 3 is heard at once. Every
	// operator gets its own values, saved as VGI and read back.
	patch_instrument made = {"Algo 4", {0}};
	made.voice[0] = 4;
	made.voice[1] = 5;
	made.voice[2] = 2;
	made.voice[3] = 3;
	for (int op = 0; op < PATCH_OPS; op++) {
		int16_t *values = made.voice + 4 + op * PATCH_OP_PARAMS;
		values[F_TL] = (int16_t)(10 + op * 20);
		values[F_MUL] = (int16_t)(op + 1);
		values[F_DT] = (int16_t)(op + 4);
		values[F_RS] = (int16_t)op;
		values[F_AR] = (int16_t)(31 - op);
		values[F_DR] = (int16_t)(20 + op);
		values[F_SR] = (int16_t)(16 + op * 4);
		values[F_SL] = (int16_t)(op + 2);
		values[F_RR] = (int16_t)(15 - op);
		values[F_AM] = (int16_t)(op & 1);
		values[F_SSG] = op == 3 ? 9 : 0;
	}
	uint8_t vgi[INSTRUMENT_VGI_SIZE];
	instrument_write_vgi(&made, vgi);
	patch_instrument back;
	char error[96];
	bool read = instrument_parse("Algo 4.vgi", vgi, sizeof(vgi), &back, error, sizeof(error));
	// Register detune 4 is none, like 0, and comes back as 0.
	made.voice[4 + F_DT] = 0;
	check(read && !memcmp(back.voice, made.voice, sizeof(made.voice)), "a voice saved as VGI reads back the same");
	drop_and_play(3, &back);
	check_voices("and an algorithm 4 voice from it reaches the chip, operators in their places", 3, 3, -1);
	check(read_reg(3, 0x40, 1) == 50 && read_reg(3, 0x40, 2) == 30, "operator 3's TL in register slot 2, operator 2's in slot 3");
}

// A file in a place of the bank is what a program change or the Preset lane chooses, on lent voices too.
static void test_bank_place(void)
{
	file_regs expected;
	patch_instrument instrument;
	if (!file_regs_of("Thunder Guitar 03.tyi", &expected) || !load_file("Thunder Guitar 03.tyi", &instrument)) {
		return;
	}
	patch_load_place(patch, 20, &instrument);
	patch_take_incoming(patch);
	midi(0xC0, 20, 0);
	set_on(2, PARAM_PRESET, 20);
	play_each_channel();
	check(compare_file(0, &expected, true) == 0 && compare_file(2, &expected, true) == 0, "a file in place 21, by a clip's program on channel 1 and the Preset lane on channel 3");
	check(!strcmp(patch->model.names[0], "Thunder Guitar 03") && !strcmp(patch->model.names[2], "Thunder Guitar 03"), "both channels take its name");
	moment(0, 60, 4, true);
	render(SETTLE_BLOCKS);
	int voices[FM_CHANNELS], count = voices_of(0, voices), same = 0;
	for (int i = 0; i < count; i++) {
		same += compare_file(voices[i], &expected, true) == 0;
	}
	check(count == 4 && same == 4, "a chord of 4 on channel 1 plays it on every voice it borrowed");
	moment(0, 60, 4, false);
	render(SETTLE_BLOCKS);

	// A file dropped on the place a channel plays as it is: the channel takes it at once.
	file_regs duck;
	if (file_regs_of("Duck Guitar.tfi", &duck) && load_file("Duck Guitar.tfi", &instrument)) {
		patch_load_place(patch, 20, &instrument);
		patch_take_incoming(patch);
		patch_sync(patch, &patch->rom, emit, NULL);
		play_each_channel();
		check(compare_file(0, &duck, true) == 0 && !strcmp(patch->model.names[0], "Duck Guitar"), "a new file in the place channel 1 plays reaches it");
	}
	midi(0xC0, 0, 0);
	set_on(2, PARAM_PRESET, 0);
}

// Stage 7 (D28). An envelope that holds one level while the key is down, so the chip's attenuation
// says which envelope a channel plays: at full volume and velocity it is the envelope's level.
static void steady(patch_envelope *envelope, uint8_t level, int steps)
{
	uint8_t eef[PATCH_ENVELOPE_STEPS + 1];
	eef[0] = PATCH_EEF_LOOP_START;
	memset(eef + 1, level, (size_t)steps - 1);
	eef[steps] = PATCH_EEF_END;
	patch_envelope_from_eef(envelope, eef, (size_t)steps + 1);
	snprintf(envelope->name, sizeof(envelope->name), "Steady %d", level);
}

static double time_to_volume(int psg, uint8_t level)
{
	int blocks = 0;
	for (; blocks < 4000 && mdmi_engine_test_psg_volume(engine, (uint8_t)psg) != level; blocks++) {
		mdmi_engine_render(engine, left, right, KEY_BLOCK);
	}
	return blocks * KEY_BLOCK * 1000.0 / RATE;
}

// What storing the longest envelope added to a note, for stage 8 to compare drawing with.
static double stage7_extra_ms;

static void test_envelopes(void)
{
	static const uint8_t LEVELS[PATCH_NUM_PSG_CHANNELS] = {2, 5, 8, 11};
	for (int i = 0; i < PATCH_NUM_PSG_CHANNELS; i++) {
		patch_envelope envelope;
		steady(&envelope, LEVELS[i], 2);
		patch_load_envelope(patch, PATCH_NUM_FM_CHANNELS + i, &envelope, NULL);
	}
	patch_take_incoming(patch);
	sync_and_settle();
	for (int i = 0; i < PATCH_NUM_PSG_CHANNELS; i++) {
		midi((uint8_t)(0x90 | (PATCH_NUM_FM_CHANNELS + i)), 69, 127);
	}
	render(SETTLE_BLOCKS);
	bool own = true;
	for (int i = 0; i < PATCH_NUM_PSG_CHANNELS; i++) {
		own &= mdmi_engine_test_psg_volume(engine, (uint8_t)i) == LEVELS[i];
	}
	check(own, "four envelopes on channels 7-10 sound at once, each its own");

	// Square Lead holds its first level, 0, while the key is down.
	set_on(6, PARAM_PRESET, 80);
	check(mdmi_engine_test_psg_volume(engine, 0) == LEVELS[0], "a preset step leaves the sounding note's envelope");
	midi(0x86, 69, 0);
	render(SETTLE_BLOCKS);
	midi(0x96, 69, 127);
	render(SETTLE_BLOCKS);
	check(mdmi_engine_test_psg_volume(engine, 0) == 0, "and the next note plays the new place");
	// A note on a stored envelope, then two more stored while it sounds: the second goes under the
	// service number the note's envelope came from (D28, the ROM plays a copy).
	static const uint8_t STORED[3] = {6, 9, 12};
	midi(0x86, 69, 0);
	render(SETTLE_BLOCKS);
	int note_program = -1;
	for (int i = 0; i < 3; i++) {
		patch_envelope envelope;
		steady(&envelope, STORED[i], 2);
		patch_load_envelope(patch, 6, &envelope, NULL);
		patch_take_incoming(patch);
		sync_and_settle();
		if (i == 0) {
			note_program = patch->rom.psg_programs[0];
			midi(0x96, 69, 127);
			render(SETTLE_BLOCKS);
		}
	}
	check(mdmi_engine_test_psg_volume(engine, 0) == STORED[0] && patch->rom.psg_programs[0] == note_program,
		"two envelopes stored under a note on a stored one leave it as it was, the last under its number");
	midi(0x86, 69, 0);
	render(SETTLE_BLOCKS);
	midi(0x96, 69, 127);
	render(SETTLE_BLOCKS);
	check(mdmi_engine_test_psg_volume(engine, 0) == STORED[2], "the next note plays the last one");
	for (int i = 0; i < PATCH_NUM_PSG_CHANNELS; i++) {
		midi((uint8_t)(0x80 | (PATCH_NUM_FM_CHANNELS + i)), 69, 0);
	}
	render(SETTLE_BLOCKS);

	// What storing the longest envelope costs a note of the same moment, against a note whose
	// envelope is there already: both wait for the engine's own delay.
	midi(0x98, 69, 127);
	const double plain = time_to_volume(2, LEVELS[2]);
	midi(0x88, 69, 0);
	patch_envelope longest;
	steady(&longest, 3, PATCH_ENVELOPE_STEPS);
	patch_load_envelope(patch, 7, &longest, NULL);
	patch_take_incoming(patch);
	render(SETTLE_BLOCKS);
	patch_sync(patch, &patch->rom, emit, NULL);
	midi(0x97, 69, 127);
	const double stored = time_to_volume(1, 3);
	printf("  envelope of %d steps stored with the note: %.1f ms after it, a note without %.1f ms, so %.1f ms more\n", PATCH_ENVELOPE_STEPS, stored, plain, stored - plain);
	stage7_extra_ms = stored - plain;
	check(stored < 1000 && plain < 1000, "the note with the stored envelope sounds");
	midi(0x87, 69, 0);
	midi(0xC6, 0, 0);
	set_on(6, PARAM_PRESET, 0);
	render(RESET_BLOCKS);
}

// Stage 8. An envelope drawn in the editor's shape: count frames of level, then tail frames of
// tail_level, with the loop over frames loop_start..loop_end-1 (-1 for none).
static void drawn(patch_envelope *envelope, int count, uint8_t level, int loop_start, int loop_end, uint8_t tail_level)
{
	patch_shape shape = {.count = count, .loop_start = loop_start, .loop_end = loop_end};
	for (int i = 0; i < count; i++) {
		shape.frames[i] = loop_end >= 0 && i >= loop_end ? tail_level : level;
	}
	snprintf(envelope->name, sizeof(envelope->name), "Drawn");
	patch_shape_to_envelope(&shape, envelope);
}

static void draw_on(int channel, const patch_envelope *envelope)
{
	patch_load_envelope(patch, channel, envelope, NULL);
	patch_take_incoming(patch);
	patch_sync(patch, &patch->rom, emit, NULL);
}

static void render_ms(int ms)
{
	for (int i = 0; i < ms * RATE / 1000 / KEY_BLOCK; i++) {
		mdmi_engine_render(engine, left, right, KEY_BLOCK);
	}
}

static void test_drawing(void)
{
	// Drawn while a note sounds: the note keeps its envelope, the next note plays the drawing.
	patch_envelope envelope;
	drawn(&envelope, 4, 5, 0, -1, 0);
	draw_on(6, &envelope);
	render(SETTLE_BLOCKS);
	midi(0x96, 69, 127);
	render(SETTLE_BLOCKS);
	drawn(&envelope, 4, 9, 0, -1, 0);
	draw_on(6, &envelope);
	render(SETTLE_BLOCKS);
	const bool kept = mdmi_engine_test_psg_volume(engine, 0) == 5;
	midi(0x86, 69, 0);
	render(SETTLE_BLOCKS);
	midi(0x96, 69, 127);
	render(SETTLE_BLOCKS);
	check(kept && mdmi_engine_test_psg_volume(engine, 0) == 9, "a drawing under a held note: that note keeps its envelope, the next plays the drawing");
	midi(0x86, 69, 0);
	render(SETTLE_BLOCKS);

	// Release as drawn. Without a loop, Note Off cuts at once.
	drawn(&envelope, 60, 3, -1, -1, 0);
	draw_on(6, &envelope);
	render(SETTLE_BLOCKS);
	midi(0x96, 69, 127);
	render_ms(100);
	const bool held = mdmi_engine_test_psg_volume(engine, 0) == 3;
	midi(0x86, 69, 0);
	render_ms(40);
	check(held && mdmi_engine_test_psg_volume(engine, 0) == 15, "no loop: Note Off cuts the note");
	// A loop start alone: the pass of 30 frames plays out, then the note ends.
	drawn(&envelope, 30, 3, 0, -1, 0);
	draw_on(6, &envelope);
	render(SETTLE_BLOCKS);
	midi(0x96, 69, 127);
	render_ms(100);
	midi(0x86, 69, 0);
	render_ms(100);
	const bool pass = mdmi_engine_test_psg_volume(engine, 0) == 3;
	render_ms(500);
	check(pass && mdmi_engine_test_psg_volume(engine, 0) == 15, "a loop start alone: after Note Off the pass plays to the end, then silence");
	// Start and end: after Note Off the tail after the loop plays, then the note ends.
	drawn(&envelope, 26, 3, 2, 6, 9);
	draw_on(6, &envelope);
	render(SETTLE_BLOCKS);
	midi(0x96, 69, 127);
	render_ms(200);
	const bool looping = mdmi_engine_test_psg_volume(engine, 0) == 3;
	midi(0x86, 69, 0);
	render_ms(150);
	const bool tail = mdmi_engine_test_psg_volume(engine, 0) == 9;
	render_ms(400);
	check(looping && tail && mdmi_engine_test_psg_volume(engine, 0) == 15, "a loop with an end: held it loops, after Note Off the tail plays, then silence");

	// Drawing: the window hands the longest envelope over every 33 ms. Notes on channel 9 meet the
	// stores at every phase; none may wait longer than a note stored with its envelope (stage 7).
	patch_envelope steady_note;
	steady(&steady_note, 8, 2);
	draw_on(8, &steady_note);
	render(SETTLE_BLOCKS);
	double worst[2] = {0, 0};
	int notes[2] = {0, 0};
	for (int drawing = 0; drawing < 2; drawing++) {
		const int blocks = 3 * RATE / KEY_BLOCK;
		double next_store = 0, next_note = 50, note_at = 0;
		bool waiting = false;
		for (int b = 0; b < blocks; b++) {
			const double t = b * KEY_BLOCK * 1000.0 / RATE;
			if (drawing && t >= next_store) {
				drawn(&envelope, PATCH_ENVELOPE_STEPS - 1, (uint8_t)(1 + b % 13), 0, -1, 0);
				draw_on(6, &envelope);
				next_store += 1000.0 / 30;
			}
			if (!waiting && t >= next_note) {
				midi(0x98, 69, 127);
				note_at = t;
				waiting = true;
			}
			mdmi_engine_render(engine, left, right, KEY_BLOCK);
			if (waiting && mdmi_engine_test_psg_volume(engine, 2) == 8) {
				const double delay = t + KEY_BLOCK * 1000.0 / RATE - note_at;
				worst[drawing] = delay > worst[drawing] ? delay : worst[drawing];
				notes[drawing]++;
				midi(0x88, 69, 0);
				waiting = false;
				// Coprime with the stores' 33 ms, so notes come at every phase of them.
				next_note = t + 47;
			}
		}
		// The run's end cuts the last note short; it is not counted.
		if (waiting) {
			midi(0x88, 69, 0);
		}
		render(SETTLE_BLOCKS);
	}
	printf("  %d notes while drawing: worst %.1f ms after the note, %d without drawing %.1f ms, so %.1f ms more; stage 7 measured %.1f ms\n",
		notes[1], worst[1], notes[0], worst[0], worst[1] - worst[0], stage7_extra_ms);
	check(notes[0] >= 40 && notes[1] >= 40 && worst[1] - worst[0] <= stage7_extra_ms + 1, "drawing delays notes no more than a stored envelope did in stage 7");
	render(SETTLE_BLOCKS);
	midi(0xC6, 0, 0);
	midi(0xC8, 0, 0);
	set_on(6, PARAM_PRESET, 0);
	set_on(8, PARAM_PRESET, 0);
	render(RESET_BLOCKS);
}

static void test_reset(void)
{
	const uint8_t reset = 0xFF;
	patch_midi(patch, &reset, 1, emit, NULL);
	render(RESET_BLOCKS);
	play_chord(0, false);
	check_voices("the ROM's reset: every voice comes back", 0, FM_CHANNELS - 1, -1);
}

int main(int argc, char **argv)
{
	if (argc > 1) {
		files_dir = argv[1];
	}
	static mdmi_patch model;
	patch = &model;
	setvbuf(stdout, NULL, _IONBF, 0);
	patch_global_init();
	patch_init(patch);
	if (!mdmi_engine_global_init() || !(engine = mdmi_engine_create())) {
		printf("FAIL: engine\n");
		return 1;
	}
	mdmi_engine_set_offline(engine, true);
	// Registers are read in the position of the sound, not wherever the emulator ran ahead to.
	mdmi_engine_test_lockstep(engine, true);
	if (!mdmi_engine_activate(engine, RATE, BLOCK)) {
		printf("FAIL: activate\n");
		return 1;
	}
	render(SETTLE_BLOCKS);
	full_velocity();
	check_voices("a booted ROM holds the model's defaults", 0, FM_CHANNELS - 1, -1);
	check_globals("and its LFO and Ch3 mode");
	test_every_preset();
	test_values();
	test_program_from_clip();
	test_dac();
	test_dac_psg();
	test_restart();
	test_samples();
	test_shaping();
	test_load();
	test_voice_by_hand();
	test_reset();
	test_dealer();
	test_reset();
	test_files();
	test_bank_place();
	test_reset();
	test_envelopes();
	test_reset();
	test_drawing();
	test_reset();
	mdmi_engine_stats stats = mdmi_engine_get_stats(engine);
	if (stats.dropped_midi) {
		printf("  FAIL: %u MIDI messages lost\n", stats.dropped_midi);
		failures++;
	}
	printf("%s: %d failed\n", failures ? "FAILED" : "PASSED", failures);
	return failures;
}
