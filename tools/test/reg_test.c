// Does the running YM2612 hold what the model says? Drives patch.c the way the plugin does and
// reads the chip's registers, which no test by ear can do. A channel keeps the velocity of its last
// note, and that scales the TL of the carriers, so notes here are at full velocity.
//
//   reg_test [folder of the test instrument files]
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define PSAPI_VERSION 2
#include <windows.h>
#include <psapi.h>
#include <stdlib.h>
#include "dac.h"
#include "files.h"
#include "engine.h"
#include "instrument.h"
#include "patch.h"
#include "ch3_off_clip.h"

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
// Stage 17: each output's block, how much each output sounded since the last reset_energy, and how
// far Main strayed from the outputs' sum.
static float outputs[2 * MDMI_OUTPUTS][BLOCK];
static double out_energy[MDMI_OUTPUTS];
static float sum_error;

static void emit(void *context, const uint8_t *bytes, size_t length)
{
	(void)context;
	mdmi_engine_midi(engine, 0, bytes, length);
}

static void owner(void *context, int voice, int channel, bool at_once)
{
	(void)context;
	mdmi_engine_voice_owner(engine, 0, voice, channel, at_once);
}

static void render_block(int frames)
{
	float *channels[MDMI_RENDER_CHANNELS] = {left, right};
	for (int ch = 0; ch < 2 * MDMI_OUTPUTS; ch++) {
		channels[2 + ch] = outputs[ch];
	}
	mdmi_engine_render_outputs(engine, channels, (uint32_t)frames);
	for (int i = 0; i < frames; i++) {
		float sum[2] = {0, 0};
		for (int o = 0; o < MDMI_OUTPUTS; o++) {
			const float l = outputs[o * 2][i], r = outputs[o * 2 + 1][i];
			out_energy[o] += (double)l * l + (double)r * r;
			sum[0] += l;
			sum[1] += r;
		}
		const float error = fmaxf(fabsf(sum[0] - left[i]), fabsf(sum[1] - right[i]));
		sum_error = error > sum_error ? error : sum_error;
	}
}

// The dealer's clock (D41), the way the plugin keeps it: notes that wait start when due.
static uint64_t clock_samples;
// Where the notes that start when due go: emit, or a recorder (stage 21, test_ch3_off).
static patch_emit_fn due_emit = emit;

static void tick(int frames)
{
	patch_set_now(patch, clock_samples, RATE);
	patch_due(patch, due_emit, NULL);
	render_block(frames);
	clock_samples += (uint64_t)frames;
	patch_set_now(patch, clock_samples, RATE);
}

static void render(int blocks)
{
	for (int i = 0; i < blocks; i++) {
		tick(BLOCK);
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

// Rising crossings of the left channel over half a second while a note of the channel sounds. The
// threshold is the middle of the swing, so a narrow pulse (periodic noise: 1 of 16 steps) counts too.
#define NOTE_SAMPLES (RATE / 2 / BLOCK * BLOCK)
static float note_samples[NOTE_SAMPLES];

static double note_hz(int channel, int key, uint16_t *tone3)
{
	midi((uint8_t)(0x90 | channel), (uint8_t)key, 127);
	render(4);
	*tone3 = mdmi_engine_test_psg_tone(engine, 2);
	float low_most = 0, high_most = 0;
	for (int block = 0; block < NOTE_SAMPLES / BLOCK; block++) {
		mdmi_engine_render(engine, left, right, BLOCK);
		for (int i = 0; i < BLOCK; i++) {
			const float v = left[i];
			note_samples[block * BLOCK + i] = v;
			low_most = v < low_most ? v : low_most;
			high_most = v > high_most ? v : high_most;
		}
	}
	midi((uint8_t)(0x80 | channel), (uint8_t)key, 0);
	render(SETTLE_BLOCKS);
	const float middle = (low_most + high_most) / 2, band = (high_most - low_most) / 8;
	long crossings = 0, first = -1, last = 0;
	bool low = false;
	for (long at = 0; at < NOTE_SAMPLES && band > 0.001f; at++) {
		if (note_samples[at] < middle - band) {
			low = true;
		} else if (low && note_samples[at] > middle + band) {
			low = false;
			first = first < 0 ? at : first;
			last = at;
			crossings++;
		}
	}
	return crossings > 1 ? (crossings - 1) * (double)RATE / (last - first) : 0;
}

static void render_ms(int ms);

// Stage 16 (D45): periodic noise on tone 3 plays the note, and channel 9 stays silent meanwhile.
static void test_noise(void)
{
	const int noise = PATCH_NUM_CHANNELS - 1, tone3 = PATCH_NUM_CHANNELS - 2;
	// The notes before play their releases after Note Off (stage 22b, D54): the count starts in silence.
	int waited = 0;
	for (; waited < 3000; waited += 10) {
		bool silent = true;
		for (int i = 0; i < PATCH_NUM_PSG_CHANNELS; i++) {
			silent = silent && mdmi_engine_test_psg_volume(engine, (uint8_t)i) == 15;
		}
		if (silent) {
			break;
		}
		render_ms(10);
	}
	printf("    PSG silent after %d ms\n", waited);
	set_global(GLOBAL_NOISE_TYPE, PATCH_NOISE_PERIODIC);
	set_global(GLOBAL_NOISE_RATE, PATCH_NOISE_RATE_TONE3);
	sync_and_settle();
	// N from the table of key + 48; at high keys N is small and the table's steps coarse.
	static const int KEYS[] = {33, 36, 45, 57};
	int bad = 0;
	for (size_t i = 0; i < sizeof(KEYS) / sizeof(*KEYS); i++) {
		uint16_t n;
		const double hz = note_hz(noise, KEYS[i], &n), want = 440.0 * pow(2.0, (KEYS[i] - 69) / 12.0);
		const double chip = n ? 3579545.0 / (512.0 * n) : 0;
		printf("    key %d: %.2f Hz, 6991/%d = %.2f Hz, the note %.2f Hz\n", KEYS[i], hz, n, chip, want);
		bad += fabs(hz / chip - 1.0) > 0.01 || fabs(hz / want - 1.0) > 0.03;
	}
	check(bad == 0, "periodic noise on tone 3 sounds at 6991/N Hz within 1%, at the note within 3% (a quarter tone)");
	midi((uint8_t)(0x90 | tone3), 60, 127);
	render(SETTLE_BLOCKS);
	check(mdmi_engine_test_psg_volume(engine, 2) == 15 && mdmi_engine_test_psg_tone(engine, 2) != 427, "a note on channel 9 writes nothing while the noise has its tone");
	midi((uint8_t)(0x80 | tone3), 60, 0);
	set_global(GLOBAL_NOISE_TYPE, PATCH_NOISE_WHITE);
	set_global(GLOBAL_NOISE_RATE, PATCH_NOISE_RATE_HIGH);
	sync_and_settle();
	midi((uint8_t)(0x90 | tone3), 60, 127);
	render(SETTLE_BLOCKS);
	check(mdmi_engine_test_psg_volume(engine, 2) < 15 && mdmi_engine_test_psg_tone(engine, 2) == 427, "after a fixed rate channel 9 plays again");
	midi((uint8_t)(0x80 | tone3), 60, 0);
	render(SETTLE_BLOCKS);
	// Rate Kit: each key the type of its drum on tone 3, and an octave up the same drum an octave higher.
	set_global(GLOBAL_NOISE_TYPE, PATCH_NOISE_PERIODIC);
	set_global(GLOBAL_NOISE_RATE, PATCH_NOISE_RATE_KIT);
	sync_and_settle();
	static const struct {
		int key, control;
	} BY_KEY[] = {{36, 7}, {38, 7}, {41, 3}, {53, 3}};
	bad = 0;
	for (size_t i = 0; i < sizeof(BY_KEY) / sizeof(*BY_KEY); i++) {
		midi((uint8_t)(0x90 | noise), (uint8_t)BY_KEY[i].key, 127);
		render(SETTLE_BLOCKS);
		const int control = mdmi_engine_test_psg_tone(engine, 3);
		midi((uint8_t)(0x80 | noise), (uint8_t)BY_KEY[i].key, 0);
		render(SETTLE_BLOCKS);
		if (control != BY_KEY[i].control) {
			printf("    key %d: noise control %d, meant %d\n", BY_KEY[i].key, control, BY_KEY[i].control);
			bad++;
		}
	}
	check(bad == 0, "at rate Kit each key sets the chip's noise to its drum's type on tone 3");
	uint16_t n_low, n_high;
	const double tom = note_hz(noise, 41, &n_low), tom_up = note_hz(noise, 53, &n_high);
	printf("    low tom on key 41 %.2f Hz, on key 53 %.2f Hz\n", tom, tom_up);
	check(tom > 0 && fabs(tom_up / tom - 2.0) < 0.04, "the low tom an octave up sounds an octave higher");
	set_global(GLOBAL_NOISE_TYPE, PATCH_NOISE_WHITE);
	set_global(GLOBAL_NOISE_RATE, PATCH_NOISE_RATE_HIGH);
	sync_and_settle();
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
	// The same image would now go into the running core (D39): every other restart takes one that
	// differs in a byte of the header's name, so each is a real restart.
	uint8_t *other = malloc(rom_size);
	memcpy(other, rom, rom_size);
	other[0x150] ^= 0x20;
	size_t memory_before = 0;
	double slowest = 0, total = 0;
	int bad = 0;
	for (int i = 0; i < RESTARTS; i++) {
		const uint32_t boots = mdmi_engine_boots(engine);
		mdmi_engine_set_rom(engine, i % 2 ? rom : other, rom_size);
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
	free(other);
}

// Gives the core an image: written into it when only the samples differ (D39), else a restart, and
// then the ROM gets everything again, as the plugin does.
static void boot_image(const uint8_t *image, size_t size)
{
	const mdmi_engine_stats before = mdmi_engine_get_stats(engine);
	patch_set_dac_image(patch, image, size);
	mdmi_engine_set_rom(engine, image, size);
	mdmi_engine_stats now;
	do {
		render(1);
		now = mdmi_engine_get_stats(engine);
	} while (now.boots == before.boots && now.patches == before.patches);
	if (now.boots != before.boots) {
		patch_rom_booted(patch, emit, NULL);
		render(SETTLE_BLOCKS);
		full_velocity();
	}
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
	s->sample = (rom_sample){(uint8_t)rate, length, s->bytes, 0, 0, 0};
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
		// PCM4 plays every sample at one rate, whatever the record's rate byte (stage 19).
		printf("    note %d, rate byte %d: %.0f bytes a second, PCM4 %.0f\n", KEYS[i], RATES[i], hz, ROM_PCM4_HZ);
		check(in_order && hz > ROM_PCM4_HZ * 0.995 && hz < ROM_PCM4_HZ * 1.005,
			i ? "a sample on a note the ROM has none for plays at PCM4's rate" : "a sample in place of the ROM's plays its bytes at PCM4's rate");
	}
	bool in_order;
	ramp_rate(41, &in_order);
	check(mdmi_engine_test_ym(engine, 0, 0x2A) == 0x80 || !in_order, "the ROM's own sample on the next note is not the ramp");
	set_global(GLOBAL_DAC, 0);
	// Switching the DAC resets the chip, and the voices come back some 170 ms later.
	wait_for_voices(FM_CHANNELS - 1);
	// Channel 6's last notes went to the DAC at another velocity.
	full_velocity();
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

// A square at a driver rate, loud, or silence.
static dac_sample *square_sample(uint32_t length, int level)
{
	dac_sample *s = calloc(1, sizeof(dac_sample));
	s->bytes = malloc(length);
	for (uint32_t i = 0; i < length; i++) {
		s->bytes[i] = (int8_t)(i / 20 % 2 ? level : -level);
	}
	snprintf(s->name, sizeof(s->name), "square");
	s->sample = (rom_sample){1, length, s->bytes, 0, 0, 0};
	return s;
}

// Hands the engine the set's image and waits until the core took it. True if it was written into
// the running core, false if it restarted.
static bool take_set(const dac_set *set, double *ms)
{
	size_t rom_size, image_size;
	const uint8_t *rom = mdmi_engine_builtin_rom(&rom_size);
	uint8_t *image = dac_image(set, rom, rom_size, &image_size);
	const mdmi_engine_stats before = mdmi_engine_get_stats(engine);
	mdmi_engine_set_rom(engine, image, image_size);
	free(image);
	mdmi_engine_stats now;
	do {
		render(1);
		now = mdmi_engine_get_stats(engine);
	} while (now.patches == before.patches && now.boots == before.boots);
	*ms = now.last_patch_ms;
	return now.boots == before.boots;
}

static float note_loudness(int key)
{
	midi(0x95, (uint8_t)key, 127);
	render(RATE / 10 / BLOCK);
	const float level = loudest(RATE / 20 / BLOCK);
	midi(0x85, (uint8_t)key, 0);
	render(RATE / 10 / BLOCK);
	return level;
}

// Samples change in the running core (D39): no restart, a held FM note sounds on, the next DAC note
// plays the new bytes, data near the end of 4 MB plays, the records keep their places.
static void test_live(void)
{
	dac_set set = {0};
	set_global(GLOBAL_DAC, 1);
	sync_and_settle();
	render(DAC_BLOCKS);
	// A held FM note on channel 1 while the samples change.
	midi(0x90, 60, 127);
	render(SETTLE_BLOCKS);
	const float fm_before = loudest(RATE / 20 / BLOCK);
	const uint32_t boots = mdmi_engine_boots(engine);
	dac_put(&set, 50, square_sample(256 * 40, 100));
	double ms;
	const bool live = take_set(&set, &ms);
	const float fm_after = loudest(RATE / 20 / BLOCK);
	printf("    written into the running core in %.1f ms; FM held note %.3f before, %.3f after\n", ms, fm_before, fm_after);
	check(live && mdmi_engine_boots(engine) == boots && fm_after > fm_before * 0.5f && fm_before > 0.02f, "a new sample goes into the running core: no restart, a held FM note sounds on");
	midi(0x80, 60, 0);
	render(DAC_BLOCKS);
	const float loud = note_loudness(50);
	dac_put(&set, 50, square_sample(256 * 40, 0));
	take_set(&set, &ms);
	const float quiet = note_loudness(50);
	printf("    note 50: %.3f with the square, %.3f with silence\n", loud, quiet);
	check(loud > 0.05f && quiet < 0.01f, "the next DAC note plays the new bytes");
	// A long sample low, a short loud one high: its data lies near the end of 4 MB.
	dac_put(&set, 50, NULL);
	dac_put(&set, 20, square_sample((uint32_t)(rom_image_room() - 256 * 64) & ~255u, 0));
	dac_put(&set, 100, square_sample(256 * 40, 100));
	set.notes[100]->sample.flags = ROM_SAMPLE_ONE_SHOT;
	take_set(&set, &ms);
	const float last = note_loudness(100);
	printf("    a sample ending at the last 16 KB of 4 MB: %.3f, written in %.1f ms\n", last, ms);
	check(last > 0.05f, "a sample near the end of 4 MB plays");
	// A sample added below a one-shot: its record keeps its place, Note Off still doesn't cut it.
	dac_put(&set, 10, square_sample(256 * 4, 50));
	take_set(&set, &ms);
	midi(0x95, 100, 127);
	render(RATE / 20 / BLOCK);
	midi(0x85, 100, 0);
	render(RATE / 10 / BLOCK);
	const float on = loudest(RATE / 20 / BLOCK);
	render(RATE / 2 / BLOCK);
	printf("    one-shot 100 ms after Note Off: %.3f\n", on);
	check(on > 0.05f, "with a sample added below, the one-shot above plays on after Note Off");
	set_global(GLOBAL_DAC, 0);
	sync_and_settle();
	// An image that differs elsewhere still restarts the core.
	size_t rom_size;
	const uint8_t *rom = mdmi_engine_builtin_rom(&rom_size);
	uint8_t *other = malloc(rom_size);
	memcpy(other, rom, rom_size);
	other[0x150] ^= 0x20;
	boot_image(other, rom_size);
	check(mdmi_engine_boots(engine) == boots + 1, "an image that differs outside the samples restarts the core");
	free(other);
	dac_clear(&set);
	boot_image(rom, rom_size);
}

// Stage 18b: a sample of three parts told apart by their level, intro loud, loop quiet, release
// between, a square of 32 bytes; the record's marks around the loop.
static dac_sample *marked_sample(int rate, uint32_t intro, uint32_t loop, uint32_t release)
{
	const uint32_t length = (intro + loop + release) * ROM_SAMPLE_BLOCK;
	dac_sample *s = calloc(1, sizeof(dac_sample));
	s->bytes = malloc(length);
	for (uint32_t i = 0; i < length; i++) {
		const uint32_t block = i / ROM_SAMPLE_BLOCK;
		const int level = block < intro ? 120 : block < intro + loop ? 40 : 80;
		s->bytes[i] = (int8_t)(i / 16 % 2 ? level : -level);
	}
	snprintf(s->name, sizeof(s->name), "marked");
	s->sample = (rom_sample){(uint8_t)rate, length, s->bytes, ROM_SAMPLE_LOOP | ROM_SAMPLE_MARKS, (uint16_t)intro, (uint16_t)(intro + loop)};
	return s;
}

// 1 ms windows of output 6's left side as letters: I intro, L loop, R release, Q quiet, by the
// window's peak against the loudest (the intro's).
#define MARK_WINDOWS 400
static int mark_windows(float *peaks, int count)
{
	const int window = RATE / 1000;
	for (int w = 0; w < count; w++) {
		tick(window);
		float peak = 0;
		for (int i = 0; i < window; i++) {
			peak = fabsf(left[i]) > peak ? fabsf(left[i]) : peak;
		}
		peaks[w] = peak;
	}
	return count;
}

static void mark_letters(const float *peaks, int count, float loudest, char *out)
{
	for (int w = 0; w < count; w++) {
		const float r = peaks[w] / loudest;
		out[w] = r > 0.83f ? 'I' : r > 0.45f ? 'R' : r > 0.15f ? 'L' : r < 0.05f ? 'Q' : '?';
	}
	out[count] = '\0';
}

// Stage 18b: at every rate, a loop with the shortest intro the ROM allows goes back to the loop's
// start, not the sample's, with no quiet window, whatever the Note On's place in the ROM's frame;
// Note Off plays the loop out, then the release, then silence; Note Off in the intro plays the
// intro, the loop once and the release; without a release Note Off cuts, as in 0.18.
static void test_loop_marks(void)
{
	float peaks[MARK_WINDOWS];
	char held[MARK_WINDOWS + 1], after[MARK_WINDOWS + 1];
	bool seams = true, releases = true, intros = true;
	// On PCM4's voice 0, then on voice 3 with three silent one-shots holding voices 0-2 (stage 19).
	for (int voice = 0; voice < 4; voice += 3) {
		const int rate = voice;
		const uint32_t intro = 1, loop = 2, release = 2;
		const double block_ms = ROM_SAMPLE_BLOCK * 1000.0 / ROM_PCM4_HZ;
		dac_set set = {0};
		dac_put(&set, 50, marked_sample(1, intro, loop, release));
		for (int k = 0; k < 3; k++) {
			dac_put(&set, 60 + k, square_sample(256 * 400, 0));
			set.notes[60 + k]->sample.flags = ROM_SAMPLE_ONE_SHOT;
		}
		boot_set(&set);
		for (int k = 0; k < voice; k++) {
			midi(0x95, (uint8_t)(60 + k), 127);
		}
		for (int phase = 0; phase < 6; phase++) {
			// The Note On at six places a sixth of a ROM frame apart (16.69 ms, 801 samples at 48 kHz,
			// counted from the test's own clock, which runs with the core's): one of them comes just
			// after the ROM's frame tick, wherever that falls.
			const uint64_t frame = 801, want = (uint64_t)phase * frame / 6;
			uint64_t wait = (want + frame - clock_samples % frame) % frame + frame;
			while (wait) {
				const int step = wait > BLOCK ? BLOCK : (int)wait;
				tick(step);
				wait -= (uint64_t)step;
			}
			midi(0x95, 50, 127);
			const int hold = (int)(20 + block_ms * (intro + 4 * loop));
			mark_windows(peaks, hold);
			float loudest = 0;
			for (int w = 0; w < hold; w++) {
				loudest = peaks[w] > loudest ? peaks[w] : loudest;
			}
			mark_letters(peaks, hold, loudest, held);
			midi(0x85, 50, 0);
			const int tail = (int)(90 + block_ms * (loop + release) + 40);
			mark_windows(peaks, tail);
			mark_letters(peaks, tail, loudest, after);
			// Held: quiet until it sounds, the intro, then the loop only.
			const char *sound = held + strspn(held, "Q");
			const char *first_loop = strchr(sound, 'L');
			// A window where the level steps reads in between: one at the onset and at the seam.
			const char *body = sound + (*sound == 'R' || *sound == '?');
			const char *loop_at = first_loop && first_loop > body && (first_loop[-1] == 'R' || first_loop[-1] == '?') ? first_loop - 1 : first_loop;
			const bool seam = first_loop && (int)strspn(body, "I") == loop_at - body && strspn(first_loop, "L") == strlen(first_loop)
				&& first_loop - sound <= block_ms * intro + 3;
			// After Note Off: the loop plays out, the release, a window of its fall, then silence to the end.
			const char *rel = after + strspn(after, "L?");
			const size_t release_windows = strspn(rel, "R?");
			const char *quiet = rel + release_windows + (rel[release_windows] == 'L');
			const bool ends = release_windows >= block_ms * release - 3 && release_windows <= block_ms * release + 3
				&& strspn(quiet, "Q") == strlen(quiet) && strlen(quiet) >= 20;
			if (!seam || !ends) {
				printf("    rate %d phase %d: held %s\n    after Note Off %s\n", rate, phase, held, after);
			}
			seams = seams && seam;
			releases = releases && ends;
		}
		// Note Off at once: the intro, the loop once, the release.
		for (int ms = 0; ms < 7; ms++) {
			tick(RATE / 1000);
		}
		midi(0x95, 50, 127);
		midi(0x85, 50, 0);
		const int all = (int)(40 + block_ms * (intro + loop + release) + 40);
		mark_windows(peaks, all);
		float loudest = 0;
		for (int w = 0; w < all; w++) {
			loudest = peaks[w] > loudest ? peaks[w] : loudest;
		}
		mark_letters(peaks, all, loudest, held);
		const char *at = held + strspn(held, "Q");
		// The onset's window may read in between, as in the held check above.
		at += *at == 'R' || *at == '?';
		const size_t i_n = strspn(at, "I?");
		const size_t l_n = strspn(at + i_n, "L?");
		const size_t r_n = strspn(at + i_n + l_n, "R?");
		const char *rest = at + i_n + l_n + r_n;
		rest += *rest == 'L';
		const bool once = i_n >= 1 && l_n >= block_ms * loop - 3 && l_n <= block_ms * loop + 3 && r_n >= block_ms * release - 3 && strspn(rest, "Q") == strlen(rest);
		if (!once) {
			printf("    rate %d, Note Off in the intro: %s\n", rate, held);
		}
		intros = intros && once;
		printf("    voice %d: block %.1f ms, intro 1, loop %u, release 2 blocks: %s\n", voice, block_ms, loop, seams && releases && once ? "ok" : "FAILED");
		for (int k = 0; k < voice; k++) {
			midi(0x85, (uint8_t)(60 + k), 0);
		}
		set_global(GLOBAL_DAC, 0);
		sync_and_settle();
		dac_clear(&set);
	}
	check(seams, "a loop with the shortest intro goes back to the loop's start, no quiet window, on voices 0 and 3 at 6 places in the frame");
	check(releases, "Note Off plays the loop out, then the release, then silence");
	check(intros, "Note Off in the intro: the intro, the loop once, the release, silence");
	// No release: Note Off cuts, as in 0.18.
	{
		dac_set set = {0};
		dac_sample *s = marked_sample(1, 2, 4, 0);
		dac_put(&set, 50, s);
		boot_set(&set);
		midi(0x95, 50, 127);
		render(RATE / 10 / BLOCK);
		midi(0x85, 50, 0);
		render(RATE / 20 / BLOCK);
		const float cut = loudest(RATE / 10 / BLOCK);
		printf("    no release, 50 to 150 ms after Note Off: loudest %.4f\n", cut);
		check(cut < 0.01f, "marks without a release: Note Off cuts, as in 0.18");
		set_global(GLOBAL_DAC, 0);
		sync_and_settle();
		dac_clear(&set);
	}
	// Stage 22b (D54): a prepared sample with marks and a release, played with its release and with
	// stop. With stop the image holds no release and from 60 ms after Note Off it is under 0.01, as the
	// 18b cut (the loop's pass here would run about 84 ms).
	{
		float loud[2] = {0, 0}, after_off[2] = {0, 0};
		uint32_t lengths[2] = {0, 0};
		int release_windows[2] = {0, 0};
		for (int stop = 0; stop < 2; stop++) {
			const uint32_t intro = 1, loop = 6, release = 3, frames = (intro + loop + release) * ROM_SAMPLE_BLOCK;
			dac_sample *s = calloc(1, sizeof(dac_sample));
			s->source_hz = (uint32_t)ROM_PCM4_HZ;
			s->source_frames = frames;
			s->source = malloc(frames * sizeof(int16_t));
			for (uint32_t i = 0; i < frames; i++) {
				const uint32_t block = i / ROM_SAMPLE_BLOCK;
				const int level = block < intro ? 120 : block < intro + loop ? 40 : 80;
				s->source[i] = (int16_t)((i / 16 % 2 ? level : -level) * 256);
			}
			snprintf(s->name, sizeof(s->name), "stop");
			const dac_edit edit = {.start = 0, .end = frames, .rate = DAC_RATE_AUTO, .loop = true, .stop = stop != 0,
				.loop_start = intro * ROM_SAMPLE_BLOCK + 8, .loop_end = (intro + loop) * ROM_SAMPLE_BLOCK + 8};
			const bool made = dac_prepare(s, &edit);
			lengths[stop] = made ? s->sample.length : 0;
			dac_set set = {0};
			dac_put(&set, 50, s);
			boot_set(&set);
			midi(0x95, 50, 127);
			float peaks[300];
			char letters[301];
			mark_windows(peaks, 150);
			for (int w = 0; w < 150; w++) {
				loud[stop] = peaks[w] > loud[stop] ? peaks[w] : loud[stop];
			}
			midi(0x85, 50, 0);
			mark_windows(peaks, 300);
			mark_letters(peaks, 300, loud[stop], letters);
			// Stop takes the cut path of 18b: the message's way to the ROM (about 15 ms) and two of the
			// driver's blocks (34 ms), whatever the loop's length (here 6 blocks, 101 ms).
			for (int w = 60; w < 300; w++) {
				after_off[stop] = peaks[w] > after_off[stop] ? peaks[w] : after_off[stop];
			}
			release_windows[stop] = (int)(strchr(letters, 'R') ? strspn(strchr(letters, 'R'), "R?") : 0);
			printf("    %s: the loop sounds %d ms after Note Off\n", stop ? "stop" : "release", (int)strspn(letters, "L?"));
			printf("    %s: %u bytes, after Note Off %.140s\n", stop ? "stop" : "release", lengths[stop], letters);
			set_global(GLOBAL_DAC, 0);
			sync_and_settle();
			dac_clear(&set);
		}
		const double block_ms = ROM_SAMPLE_BLOCK * 1000.0 / ROM_PCM4_HZ;
		printf("    release %d ms of it; stop: %u bytes against %u, from 60 ms after Note Off %.5f of %.4f\n", release_windows[0], lengths[1], lengths[0], after_off[1], loud[1]);
		check(lengths[0] > 0 && release_windows[0] >= block_ms * 3 - 4, "release: after Note Off the loop's pass, then the release of 3 blocks");
		check(lengths[1] == 7 * ROM_SAMPLE_BLOCK && lengths[0] >= lengths[1] + 3 * ROM_SAMPLE_BLOCK, "stop: the image is shorter by the release");
		// The 18b cut's own measure ("marks without a release: Note Off cuts"): under 0.01 from 50 ms on;
		// -60 dB by 40 ms isn't the ROM's cut path (47 ms to silence here, the same for 2 and 6 blocks).
		check(loud[1] > 0 && after_off[1] < 0.01f && release_windows[1] == 0, "stop: from 60 ms after Note Off under 0.01 as the 18b cut, short of the loop's pass, no release");
	}
	size_t rom_size;
	const uint8_t *rom = mdmi_engine_builtin_rom(&rom_size);
	boot_image(rom, rom_size);
}

// A square of a period in bytes, as a one-shot long enough to hold through a test.
static dac_sample *tone_sample(int period, int level, uint32_t blocks)
{
	dac_sample *s = square_sample(blocks * ROM_SAMPLE_BLOCK, level);
	for (uint32_t i = 0; i < s->sample.length; i++) {
		s->bytes[i] = (int8_t)(i % (uint32_t)period < (uint32_t)period / 2 ? level : -level);
	}
	s->sample.flags = ROM_SAMPLE_ONE_SHOT;
	return s;
}

// Output 6's left side over some ms, and the magnitude at a frequency (Goertzel).
static float captured[RATE];
static int capture_ms(int ms)
{
	int n = 0;
	for (int w = 0; w < ms; w++) {
		tick(RATE / 1000);
		for (int i = 0; i < RATE / 1000; i++) {
			captured[n++] = outputs[10][i];
		}
	}
	return n;
}

static double magnitude_at(int n, double hz)
{
	const double k = 2 * cos(2 * 3.14159265358979 * hz / RATE);
	double s1 = 0, s2 = 0;
	for (int i = 0; i < n; i++) {
		const double s0 = captured[i] + k * s1 - s2;
		s2 = s1;
		s1 = s0;
	}
	return sqrt(s1 * s1 + s2 * s2 - k * s1 * s2) / n;
}

static const char *files_dir;

// A WAV's length in seconds from its header, 0 if it can't be read.
static double wav_seconds(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f) {
		return 0;
	}
	uint8_t head[4096];
	const size_t got = fread(head, 1, sizeof(head), f);
	fclose(f);
	uint32_t rate = 0, block = 0, data = 0;
	for (size_t at = 12; at + 8 <= got;) {
		uint32_t length;
		memcpy(&length, head + at + 4, 4);
		if (!memcmp(head + at, "fmt ", 4)) {
			memcpy(&rate, head + at + 12, 4);
			uint16_t b;
			memcpy(&b, head + at + 20, 2);
			block = b;
		} else if (!memcmp(head + at, "data", 4)) {
			data = length;
			break;
		}
		at += 8 + length + (length & 1);
	}
	return rate && block ? (double)data / block / rate : 0;
}

// Stage 19: PCM4's four voices. A kick keeps its level while a hat plays over it; four notes all
// sound and a fifth takes the oldest's voice; CC 10 pans them all; the ROM's samples last as long
// as in 0.18; how long a Note On and the DAC switch take.
static void test_four_voices(void)
{
	static const int PERIODS[5] = {128, 96, 64, 48, 40};
	dac_set set = {0};
	for (int k = 0; k < 5; k++) {
		dac_put(&set, 70 + k, tone_sample(PERIODS[k], 30, 120));
	}
	dac_put(&set, 76, tone_sample(4, 30, 120));
	boot_set(&set);
	// Kick alone, then the hat 100 ms in: the kick's fundamental stays within 1 dB.
	const double kick_hz = ROM_PCM4_HZ / PERIODS[0];
	midi(0x95, 70, 127);
	render(RATE / 10 / BLOCK);
	double alone = magnitude_at(capture_ms(100), kick_hz);
	midi(0x95, 76, 127);
	render(RATE / 20 / BLOCK);
	const double with_hat = magnitude_at(capture_ms(100), kick_hz);
	printf("    kick at %.0f Hz alone %.5f, with the hat %.5f (%+.2f dB)\n", kick_hz, alone, with_hat, 20 * log10(with_hat / alone));
	check(alone > 0.005 && fabs(20 * log10(with_hat / alone)) < 1, "a hat over the kick leaves the kick's level within 1 dB");
	midi(0xB5, 123, 0);
	render(RATE / 20 / BLOCK);
	// Four at once, then a fifth.
	for (int k = 0; k < 4; k++) {
		midi(0x95, (uint8_t)(70 + k), 127);
		render(2);
	}
	render(RATE / 10 / BLOCK);
	int n = capture_ms(100);
	bool all = true;
	for (int k = 0; k < 4; k++) {
		const double m = magnitude_at(n, ROM_PCM4_HZ / PERIODS[k]);
		printf("    four voices: %.0f Hz %.5f\n", ROM_PCM4_HZ / PERIODS[k], m);
		all = all && m > alone * 0.5;
	}
	check(all, "four samples at once all sound");
	midi(0x95, 74, 127);
	render(RATE / 10 / BLOCK);
	n = capture_ms(100);
	const double oldest = magnitude_at(n, ROM_PCM4_HZ / PERIODS[0]), fifth = magnitude_at(n, ROM_PCM4_HZ / PERIODS[4]);
	bool others = true;
	for (int k = 1; k < 4; k++) {
		others = others && magnitude_at(n, ROM_PCM4_HZ / PERIODS[k]) > alone * 0.5;
	}
	printf("    a fifth note: the oldest %.5f, the fifth %.5f\n", oldest, fifth);
	check(fifth > alone * 0.5 && oldest < alone * 0.1 && others, "a fifth note takes the oldest's voice, the other three sound on");
	// Pan left with two voices sounding.
	midi(0xB5, 123, 0);
	render(RATE / 20 / BLOCK);
	midi(0xB5, 10, 0);
	midi(0x95, 70, 127);
	midi(0x95, 71, 127);
	render(RATE / 10 / BLOCK);
	double side[2] = {0, 0};
	for (int b = 0; b < RATE / 10 / BLOCK; b++) {
		tick(BLOCK);
		for (int i = 0; i < BLOCK; i++) {
			side[0] += (double)outputs[10][i] * outputs[10][i];
			side[1] += (double)outputs[11][i] * outputs[11][i];
		}
	}
	printf("    two voices panned left: output 6 L %.1f dB, R %.1f dB\n", 10 * log10(side[0] + 1e-30), 10 * log10(side[1] + 1e-30));
	check(side[0] > 1 && side[1] < side[0] * 0.01, "CC 10 at 0 with two voices: output 6 right at least 20 dB down");
	midi(0xB5, 10, 64);
	midi(0xB5, 123, 0);
	render(RATE / 20 / BLOCK);
	// Note On to sound at 20 places in the ROM's frame.
	double least = 1e9, most = 0;
	for (int k = 0; k < 20; k++) {
		for (int ms = 0; ms < 3 + k % 17; ms++) {
			tick(RATE / 1000);
		}
		midi(0x95, 72, 127);
		int ms = 0;
		for (; ms < 200; ms++) {
			tick(RATE / 1000);
			float peak = 0;
			for (int i = 0; i < RATE / 1000; i++) {
				peak = fabsf(outputs[10][i]) > peak ? fabsf(outputs[10][i]) : peak;
			}
			if (peak > 0.02f) {
				break;
			}
		}
		least = ms < least ? ms : least;
		most = ms > most ? ms : most;
		midi(0x85, 72, 0);
		midi(0xB5, 123, 0);
		render(RATE / 20 / BLOCK);
	}
	printf("    Note On to sound, plugin latency included: %.0f to %.0f ms\n", least, most);
	check(most < 80, "a DAC note sounds within 80 ms of its Note On");
	// The DAC switched on: how long until a note sent at once sounds.
	set_global(GLOBAL_DAC, 0);
	sync_and_settle();
	set_global(GLOBAL_DAC, 1);
	patch_due(patch, emit, NULL);
	midi(0x95, 72, 127);
	int pause = 0, ready = -1;
	for (; pause < 1000; pause++) {
		tick(RATE / 1000);
		// The driver's init writes 0x2B = 0x80 once it is loaded: where the pause goes (stage 19).
		ready = ready < 0 && mdmi_engine_test_ym(engine, 0, 0x2B) == 0x80 ? pause : ready;
		float peak = 0;
		for (int i = 0; i < RATE / 1000; i++) {
			peak = fabsf(outputs[10][i]) > peak ? fabsf(outputs[10][i]) : peak;
		}
		if (peak > 0.02f) {
			break;
		}
	}
	printf("    DAC Enable to the first sample sounding: %d ms, to the driver ready (0x2B) %d ms\n", pause, ready);
	check(pause < 1000, "after DAC Enable a note sounds");
	midi(0xB5, 123, 0);
	set_global(GLOBAL_DAC, 0);
	sync_and_settle();
	dac_clear(&set);
	// The ROM's own samples: as long as in 0.18, where rescomp made them for 22050 Hz and the driver
	// played 21600. Their sound without the padding to a block.
	size_t rom_size;
	const uint8_t *rom = mdmi_engine_builtin_rom(&rom_size);
	static const int NOTES[3] = {35, 37, 38};
	static const char *const NAMES[3] = {"StdKick", "StdSnr1", "Clap"};
	bool lengths = true;
	for (int k = 0; k < 3; k++) {
		char path[512];
		snprintf(path, sizeof(path), "%s/../../../third_party/mdmi/res/samples/%s.wav", files_dir, NAMES[k]);
		rom_sample s;
		const double was = wav_seconds(path) * 22050 / 21600;
		uint32_t sound = 0;
		if (rom_image_sample(rom, rom_size, NOTES[k], &s)) {
			for (uint32_t i = 0; i < s.length; i++) {
				sound = s.bytes[i] ? i + 1 : sound;
			}
		}
		const double now = sound / ROM_PCM4_HZ;
		printf("    %s: %.3f s in 0.18, %.3f s now (%+.1f%%)\n", NAMES[k], was, now, was > 0 ? (now / was - 1) * 100 : 0);
		lengths = lengths && was > 0 && fabs(now / was - 1) < 0.03;
	}
	check(lengths, "the ROM's samples last as long as in 0.18, within 3%");
	boot_image(rom, rom_size);
}

// Stage 24c (D59): the window's copy of PCM4's voices against the driver's own bits, STATUS | COMMAND
// in the Z80's RAM (D6: tests only), recorded each ms. The ROM hears a note the plugin's latency and up
// to a frame later, and the driver ends a sample on its block: every change of the copy must come back
// as the same change of the same voice in the driver, all of them after one delay within two ROM frames.
#define DAC_SHOWN_MS 12000
#define DAC_BLOCK_MS 17
static uint8_t shown_copy[DAC_SHOWN_MS], shown_rom[DAC_SHOWN_MS];
static int shown_ms;
static uint8_t dac_copy_bits(void)
{
	uint8_t copy = 0;
	for (int v = 0; v < PATCH_DAC_VOICES; v++) {
		copy |= (uint8_t)((patch_dac_voice_state(patch, v, NULL) != PATCH_DAC_FREE) << v);
	}
	return copy;
}

static void dac_shown_follow(int ms)
{
	for (int t = 0; t < ms; t++) {
		const uint8_t copy = dac_copy_bits();
		tick(RATE / 1000);
		if (shown_ms < DAC_SHOWN_MS) {
			shown_copy[shown_ms] = copy;
			shown_rom[shown_ms] = (mdmi_engine_test_z80(engine, 0x102) | mdmi_engine_test_z80(engine, 0x100)) & 0x0F;
			shown_ms++;
		}
	}
}

static int busy_voices(uint8_t bits)
{
	return (bits & 1) + (bits >> 1 & 1) + (bits >> 2 & 1) + (bits >> 3 & 1);
}

static int compare_ints(const void *a, const void *b)
{
	return *(const int *)a - *(const int *)b;
}

static void test_dac_shown(void)
{
	dac_set set = {0};
	const uint32_t short_blocks = 12, long_blocks = 80;
	for (int k = 0; k < 5; k++) {
		dac_put(&set, 70 + k, square_sample(long_blocks * ROM_SAMPLE_BLOCK, 60));
	}
	dac_put(&set, 75, square_sample(short_blocks * ROM_SAMPLE_BLOCK, 60));
	set.notes[75]->sample.flags = ROM_SAMPLE_ONE_SHOT;
	dac_put(&set, 76, marked_sample(1, 4, 6, 10));
	dac_put(&set, 77, marked_sample(1, 4, 6, 10));
	// A loop with no release: Note Off stops it.
	set.notes[77]->sample.loop_end = (uint16_t)20;
	dac_put(&set, 78, square_sample(8 * ROM_SAMPLE_BLOCK, 60));
	set.notes[78]->sample.flags = ROM_SAMPLE_LOOP;
	boot_set(&set);
	// From silence: the first start is a change too.
	shown_copy[0] = shown_rom[0] = 0;
	shown_ms = 1;
	// A roll on one key while its one-shot sounds: its own voice each time, in the copy and the driver.
	const int roll_from = shown_ms;
	for (int k = 0; k < 6; k++) {
		midi(0x95, 75, 100);
		dac_shown_follow(40);
	}
	midi(0x85, 75, 0);
	dac_shown_follow(400);
	int roll_copy = 0, roll_rom = 0;
	for (int t = roll_from; t < shown_ms; t++) {
		roll_copy = busy_voices(shown_copy[t]) > roll_copy ? busy_voices(shown_copy[t]) : roll_copy;
		roll_rom = busy_voices(shown_rom[t]) > roll_rom ? busy_voices(shown_rom[t]) : roll_rom;
	}
	// Five keys held: the fifth takes the oldest start's voice.
	for (int k = 0; k < 5; k++) {
		midi(0x95, (uint8_t)(70 + k), 100);
		dac_shown_follow(150);
	}
	int five = 0;
	for (int v = 0; v < PATCH_DAC_VOICES; v++) {
		int key = 0;
		five += patch_dac_voice_state(patch, v, &key) == PATCH_DAC_HELD && key != 70;
	}
	for (int k = 0; k < 5; k++) {
		midi(0x85, (uint8_t)(70 + k), 0);
	}
	dac_shown_follow(300);
	// A one-shot let go at once sounds to its end, shown released.
	midi(0x95, 75, 100);
	dac_shown_follow(20);
	midi(0x85, 75, 0);
	dac_shown_follow(30);
	int key = 0;
	const bool out = patch_dac_voice_state(patch, 0, &key) == PATCH_DAC_OUT && key == 75;
	dac_shown_follow(400);
	// A loop with a release, let go in its intro and in its loop; one that stops; a whole-sample loop.
	static const int HOLD[4] = {40, 500, 500, 500};
	for (int k = 0; k < 4; k++) {
		const uint8_t note = (uint8_t)(k < 2 ? 76 : 75 + k);
		midi(0x95, note, 100);
		dac_shown_follow(HOLD[k]);
		midi(0x85, note, 0);
		dac_shown_follow(400);
	}
	// Every change of the copy, voice by voice, found again in the driver and how much later.
	int delays[256], edges = 0, missing = 0;
	for (int t = 1; t < shown_ms; t++) {
		for (int v = 0; v < PATCH_DAC_VOICES; v++) {
			const int was = shown_copy[t - 1] >> v & 1, now = shown_copy[t] >> v & 1;
			if (was == now) {
				continue;
			}
			// The copy reckons a sample's end from its blocks, the driver ends it on its block: up to
			// a block either way (the stage's definition: within a block of PCM4).
			int found = INT32_MIN;
			for (int u = t - DAC_BLOCK_MS > 1 ? t - DAC_BLOCK_MS : 1; u < t + 200 && u < shown_ms && found == INT32_MIN; u++) {
				found = (shown_rom[u - 1] >> v & 1) == was && (shown_rom[u] >> v & 1) == now ? u - t : INT32_MIN;
			}
			if (found == INT32_MIN) {
				if (missing < 6) {
					printf("    the copy's voice %d %s at %d ms: not in the driver\n", v + 1, now ? "starts" : "ends", t);
				}
				missing++;
			} else if (edges < 256) {
				delays[edges++] = found;
			}
		}
	}
	// And no change of the driver that the copy didn't make before it.
	int unseen = 0;
	for (int t = 1; t < shown_ms; t++) {
		for (int v = 0; v < PATCH_DAC_VOICES; v++) {
			const int was = shown_rom[t - 1] >> v & 1, now = shown_rom[t] >> v & 1;
			if (was == now) {
				continue;
			}
			bool made = false;
			for (int u = t + DAC_BLOCK_MS < shown_ms - 1 ? t + DAC_BLOCK_MS : shown_ms - 1; u > t - 200 && u > 0 && !made; u--) {
				made = (shown_copy[u - 1] >> v & 1) == was && (shown_copy[u] >> v & 1) == now;
			}
			if (!made && unseen < 6) {
				printf("    the driver's voice %d %s at %d ms: not in the copy\n", v + 1, now ? "starts" : "ends", t);
			}
			unseen += !made;
		}
	}
	missing += unseen;
	int sorted[256];
	memcpy(sorted, delays, sizeof(int) * (size_t)edges);
	qsort(sorted, (size_t)edges, sizeof(int), compare_ints);
	const int median = edges ? sorted[edges / 2] : 0;
	int outliers = 0;
	for (int i = 0; i < edges; i++) {
		outliers += abs(delays[i] - median) > 34;
	}
	printf("    %d changes of the copy: %d missing in the driver, delays %d-%d ms around %d, %d more than 34 ms off; roll %d voice(s) shown, %d in the driver; five keys gave %d others\n",
		edges + missing, missing, edges ? sorted[0] : 0, edges ? sorted[edges - 1] : 0, median, outliers, roll_copy, roll_rom, five);
	check(edges >= 20 && missing == 0 && outliers == 0, "the window's DAC voices change as the driver's own, voice by voice, one delay within two ROM frames");
	check(roll_copy == 1 && roll_rom == 1 && five == 4 && out, "a roll on one key keeps one voice, a fifth key takes the oldest's, a one-shot plays on released");
	midi(0xB5, 123, 0);
	set_global(GLOBAL_DAC, 0);
	sync_and_settle();
	dac_clear(&set);
	size_t rom_size;
	const uint8_t *rom = mdmi_engine_builtin_rom(&rom_size);
	boot_image(rom, rom_size);
}

// Stage 18: a LOOP sample repeats while the note is held, with no gap at the seam, and stops at
// Note Off; CC 10 pans the DAC on output 6, also while a sample sounds.
static void test_loop_pan(void)
{
	dac_set set = {0};
	// Four blocks at 22 kHz, about 46 ms: 500 ms held is ten repeats.
	dac_put(&set, 50, square_sample(256 * 4, 100));
	set.notes[50]->sample.flags = ROM_SAMPLE_LOOP;
	boot_set(&set);
	midi(0xB5, 10, 0);
	render(SETTLE_BLOCKS);
	midi(0x95, 50, 127);
	// The note sounds after the plugin's latency (12.6 ms) and the driver's start.
	render(RATE / 16 / BLOCK);
	// 1 ms windows over 500 ms: each must hold the square; a seam of silence would leave one quiet.
	const int window = RATE / 1000;
	int windows = 0, quiet_windows = 0, at = 0;
	float peak = 0, weakest = 1;
	double side[2] = {0, 0};
	for (int b = 0; b < RATE / 2 / BLOCK; b++) {
		tick(BLOCK);
		for (int i = 0; i < BLOCK; i++) {
			peak = fabsf(left[i]) > peak ? fabsf(left[i]) : peak;
			side[0] += (double)outputs[10][i] * outputs[10][i];
			side[1] += (double)outputs[11][i] * outputs[11][i];
			if (++at == window) {
				windows++;
				quiet_windows += peak < 0.05f;
				weakest = peak < weakest ? peak : weakest;
				peak = 0;
				at = 0;
			}
		}
	}
	printf("    LOOP held 500 ms: %d of %d 1 ms windows quiet, weakest %.3f; output 6 L %.1f dB, R %.1f dB\n", quiet_windows, windows,
		weakest, 10 * log10(side[0] + 1e-30), 10 * log10(side[1] + 1e-30));
	// A 1 ms window can't see a pause shorter than itself (the DAC holds its last byte); that there is
	// none comes from the driver, which outputs samples while it recopies the params (drv_pcm.s80).
	check(quiet_windows == 0, "a LOOP sample sounds through ten times its length, no 1 ms window quiet");
	// The side turned off isn't zero: the YM2612's ladder gives it an offset by the sample's sign, as
	// BlastEm models it (ym2612.c, zero_offset); about 29 dB down here.
	check(side[0] > 1 && side[1] < side[0] * 0.01, "CC 10 at 0: output 6 left sounds, right at least 20 dB down");
	// Pan right while it plays: the driver writes B6 only at a sample's start, the ROM writes it now.
	midi(0xB5, 10, 127);
	render(RATE / 50 / BLOCK);
	side[0] = side[1] = 0;
	for (int b = 0; b < RATE / 10 / BLOCK; b++) {
		tick(BLOCK);
		for (int i = 0; i < BLOCK; i++) {
			side[0] += (double)outputs[10][i] * outputs[10][i];
			side[1] += (double)outputs[11][i] * outputs[11][i];
		}
	}
	printf("    after CC 10 at 127 mid-sample: output 6 L %.1f dB, R %.1f dB\n", 10 * log10(side[0] + 1e-30), 10 * log10(side[1] + 1e-30));
	check(side[1] > 1 && side[0] < side[1] * 0.01, "CC 10 moves a sounding sample to the right");
	midi(0x85, 50, 0);
	render(RATE / 20 / BLOCK);
	const float after = loudest(RATE / 10 / BLOCK);
	printf("    LOOP 50 to 150 ms after Note Off: loudest %.4f\n", after);
	check(after < 0.01f, "a LOOP sample stops at Note Off");
	midi(0xB5, 10, 64);
	set_global(GLOBAL_DAC, 0);
	sync_and_settle();
	dac_clear(&set);
	size_t rom_size;
	const uint8_t *rom = mdmi_engine_builtin_rom(&rom_size);
	boot_image(rom, rom_size);
}

// Stage 20, item 3: a project without rows (block 10), an EEF and the ROM's envelopes send the
// bytes 0.19 sent and leave every pitch where 0.19 left it. rows-off-0.19.bin holds what 0.19
// (60bc925) sent for exactly these calls from a booted ROM after full_velocity: its patch.c and
// instrument.c driven by the same calls without an engine. The registers: no row writes no pitch,
// so a held note's frequency registers stay as they are, frame after frame.
static uint8_t rows_sent[16384];
static size_t rows_sent_bytes;

static void emit_kept(void *context, const uint8_t *bytes, size_t length)
{
	if (rows_sent_bytes + length <= sizeof(rows_sent)) {
		memcpy(rows_sent + rows_sent_bytes, bytes, length);
	}
	rows_sent_bytes += length;
	emit(context, bytes, length);
}

static void midi_kept(uint8_t a, uint8_t b, uint8_t c)
{
	const uint8_t message[3] = {a, b, c};
	patch_midi(patch, message, (a & 0xF0) == 0xC0 ? 2 : 3, emit_kept, NULL);
	patch_flush(patch, emit_kept, NULL);
}

// The held notes' pitch registers: FM voices 1 and 2 (A4, A0), PSG tones 1-3.
static void pitch_registers(uint16_t *out)
{
	for (int voice = 0; voice < 2; voice++) {
		out[voice] = (uint16_t)(read_reg(voice, 0xA4, 0) << 8 | read_reg(voice, 0xA0, 0));
	}
	for (int i = 0; i < 3; i++) {
		out[2 + i] = mdmi_engine_test_psg_tone(engine, (uint8_t)i);
	}
}

// An envelope with no pitch step, whose tone stays.
static bool no_pitch(const patch_envelope *envelope)
{
	for (int i = 0; i < PATCH_ENVELOPE_STEPS && envelope->steps[i] != PATCH_EEF_END; i++) {
		if (envelope->steps[i] < PATCH_EEF_LOOP_END && envelope->steps[i] >> 4) {
			return false;
		}
	}
	return true;
}

static void test_rows_off(void)
{
	printf("Without rows, as 0.19\n");
	rows_sent_bytes = 0;
	static uint8_t fresh[PATCH_NUM_IDS * 6];
	patch_publish(patch);
	patch_save(patch_view(patch), fresh);
	// A project's values: presets and a tweak on FM, the ROM's envelopes on PSG.
	patch_set(patch, PATCH_CHANNEL_ID(0, PARAM_PRESET), 7);
	patch_set(patch, PATCH_CHANNEL_ID(1, PARAM_PRESET), 40);
	patch_set(patch, PATCH_CHANNEL_ID(1, PARAM_OP_BASE + F_TL), 20);
	patch_set(patch, PATCH_CHANNEL_ID(6, PARAM_PRESET), 80);
	patch_set(patch, PATCH_CHANNEL_ID(7, PARAM_PRESET), 5);
	patch_publish(patch);
	static uint8_t state[PATCH_NUM_IDS * 6];
	patch_save(patch_view(patch), state);
	patch_load(patch, state, sizeof(state), NULL, NULL, NULL, NULL);
	patch_take_incoming(patch);
	patch_sync(patch, &patch->rom, emit_kept, NULL);
	render(SETTLE_BLOCKS);
	// An EEF on channel 9.
	uint8_t eef[256];
	char path[512];
	snprintf(path, sizeof(path), "%s/PSG Organ.eef", files_dir);
	FILE *file = fopen(path, "rb");
	const size_t eef_size = file ? fread(eef, 1, sizeof(eef), file) : 0;
	if (file) {
		fclose(file);
	}
	patch_envelope organ;
	char error[96];
	check(eef_size && instrument_parse_eef("PSG Organ.eef", eef, eef_size, &organ, error, sizeof(error)), "PSG Organ.eef reads");
	patch_load_envelope(patch, 8, &organ, &organ);
	patch_take_incoming(patch);
	patch_sync(patch, &patch->rom, emit_kept, NULL);
	render(SETTLE_BLOCKS);

	midi_kept(0x90, 60, 127);
	midi_kept(0x91, 64, 127);
	midi_kept(0x96, 72, 127);
	midi_kept(0x97, 67, 127);
	midi_kept(0x98, 60, 127);
	render(4);
	uint16_t first[5], now[5];
	pitch_registers(first);
	bool held = true;
	// 40 blocks of 5.3 ms: 12 frames.
	for (int block = 0; block < 40; block++) {
		render(1);
		pitch_registers(now);
		for (int i = 0; i < 5; i++) {
			const bool psg_steady = i < 2 || no_pitch(&patch->model.psg[i - 2]);
			held = held && (!psg_steady || now[i] == first[i]);
		}
	}
	check(held && first[0] && first[1], "held notes keep their pitch registers frame after frame: no row, no pitch write");
	midi_kept(0x80, 60, 0);
	midi_kept(0x81, 64, 0);
	midi_kept(0x86, 72, 0);
	midi_kept(0x87, 67, 0);
	midi_kept(0x88, 60, 0);
	render(SETTLE_BLOCKS);
	// Preset steps there and back, and a note on the new envelope.
	patch_set(patch, PATCH_CHANNEL_ID(0, PARAM_PRESET), 9);
	patch_sync(patch, &patch->rom, emit_kept, NULL);
	patch_set(patch, PATCH_CHANNEL_ID(0, PARAM_PRESET), 7);
	patch_set(patch, PATCH_CHANNEL_ID(7, PARAM_PRESET), 89);
	patch_sync(patch, &patch->rom, emit_kept, NULL);
	midi_kept(0x97, 69, 100);
	render(SETTLE_BLOCKS);
	midi_kept(0x87, 69, 0);
	render(SETTLE_BLOCKS);

	bool rows = false;
	for (size_t i = 0; i + 5 < rows_sent_bytes; i++) {
		rows = rows || (rows_sent[i] == 0xF0 && rows_sent[i + 4] == 0x0A && (rows_sent[i + 5] == 0x02 || rows_sent[i + 5] == 0x03));
	}
	static uint8_t reference[sizeof(rows_sent)];
	snprintf(path, sizeof(path), "%s/rows-off-0.19.bin", files_dir);
	file = fopen(path, "rb");
	const size_t reference_size = file ? fread(reference, 1, sizeof(reference), file) : 0;
	if (file) {
		fclose(file);
	}
	printf("    %d bytes sent, %d in 0.19's\n", (int)rows_sent_bytes, (int)reference_size);
	check(!rows && reference_size && reference_size == rows_sent_bytes && !memcmp(reference, rows_sent, reference_size), "the same MIDI bytes as 0.19, no 0A 02 or 0A 03 among them");
	// The tests after this one start from the defaults.
	patch_load(patch, fresh, sizeof(fresh), NULL, NULL, NULL, NULL);
	patch_take_incoming(patch);
	sync_and_settle();
}

// Stage 21, item 8: a 0.20 project with Ch3 Special off plays as in 0.20. The project and clip are
// in ch3_off_clip.h. (a) Everything the plugin sends from the boot to the clip's end, stage 21's
// controllers (CC 81, 82 on MIDI 7, CC 102-104 on MIDI 3 and 11-13) taken out, is byte for byte what
// 0.20's patch.c sent: ch3-off-0.20.bin, made by gen_reference.c. Only those 14 come on top, all at
// the boot. (b) The YM2612 and PSG registers, with the notes held and at the end, are those 0.20's
// ROM held after the same clip: ch3-off-0.20-regs.bin. That one this test writes itself when run as
//   reg_test <files> --ch3-off-ref <0.20's ROM> <output>
// It then boots 0.20's ROM and keeps stage 21's controllers from it, so the ROM gets exactly
// 0.20's bytes. 0.20's ROM: a worktree of third_party/mdmi at its commit (git -C third_party/mdmi
// worktree add <dir> HEAD), `git show f94414f:patches/0003-mdmi-psg-envelopes.patch` applied there,
// res/samples copied from third_party/mdmi, and out/rom.bin built in mdmi-rom-build as
// tools/build_plugin.sh builds it (make release, BUILD=v1.34p).
#define CH3_YM1 (0xB8 - 0x21)
#define CH3_YM2 (0xB8 - 0x30)
#define CH3_SNAPSHOT (CH3_YM1 + CH3_YM2 + 4 * 2 + 4)
static uint8_t ch3_sent[16384], ch3_regs[2 * CH3_SNAPSHOT];
static size_t ch3_sent_bytes;
static int ch3_new_ccs;
static bool ch3_keep_new_from_rom;
static mdmi_patch ch3_model, ch3_scratch;
// Stage 22, item 6: the same clip as a 0.21 project with Ch3 Special on in One Note, operator
// intervals and the DAC, without CSM.
static bool ch3_on_021;

// CC 81 and 82 on MIDI 7, CC 102-104 on MIDI 3 and 11-13: what stage 21 sends on top.
static bool stage21_cc(const uint8_t *bytes, size_t length)
{
	if (length != 3 || (bytes[0] & 0xF0) != 0xB0) {
		return false;
	}
	const int channel = bytes[0] & 0x0F;
	return ((bytes[1] == 81 || bytes[1] == 82) && channel == 6) || (bytes[1] >= 102 && bytes[1] <= 104 && (channel == 2 || (channel >= 10 && channel <= 12)));
}

static void emit_ch3(void *context, const uint8_t *bytes, size_t length)
{
	if (stage21_cc(bytes, length)) {
		ch3_new_ccs++;
		if (!ch3_keep_new_from_rom) {
			emit(context, bytes, length);
		}
		return;
	}
	if (ch3_sent_bytes + length <= sizeof(ch3_sent)) {
		memcpy(ch3_sent + ch3_sent_bytes, bytes, length);
	}
	ch3_sent_bytes += length;
	emit(context, bytes, length);
}

static void ch3_block(void)
{
	tick(BLOCK);
}

// YM part 1 0x21-0xB7, part 2 0x30-0xB7, PSG tones 1-3 and the noise (low byte first), volumes.
static void ch3_snapshot(uint8_t *out)
{
	for (int reg = 0x21; reg < 0xB8; reg++) {
		*out++ = mdmi_engine_test_ym(engine, 0, (uint8_t)reg);
	}
	for (int reg = 0x30; reg < 0xB8; reg++) {
		*out++ = mdmi_engine_test_ym(engine, 1, (uint8_t)reg);
	}
	for (int i = 0; i < 4; i++) {
		const uint16_t tone = mdmi_engine_test_psg_tone(engine, (uint8_t)i);
		*out++ = (uint8_t)tone;
		*out++ = (uint8_t)(tone >> 8);
	}
	for (int i = 0; i < 4; i++) {
		*out++ = mdmi_engine_test_psg_volume(engine, (uint8_t)i);
	}
}

static void ch3_held(void)
{
	ch3_snapshot(ch3_regs);
}

static void ch3_done(void)
{
	ch3_snapshot(ch3_regs + CH3_SNAPSHOT);
}

static void ch3_register_name(int at, char *name, size_t size)
{
	const char *when = at < CH3_SNAPSHOT ? "held" : "end";
	at %= CH3_SNAPSHOT;
	if (at < CH3_YM1) {
		snprintf(name, size, "%s YM 1 %02X", when, 0x21 + at);
	} else if (at < CH3_YM1 + CH3_YM2) {
		snprintf(name, size, "%s YM 2 %02X", when, 0x30 + at - CH3_YM1);
	} else if (at < CH3_YM1 + CH3_YM2 + 8) {
		snprintf(name, size, "%s PSG tone %d", when, (at - CH3_YM1 - CH3_YM2) / 2 + 1);
	} else {
		snprintf(name, size, "%s PSG volume %d", when, at - CH3_YM1 - CH3_YM2 - 8 + 1);
	}
}

static size_t read_reference(const char *name, uint8_t *buffer, size_t size)
{
	char path[512];
	snprintf(path, sizeof(path), "%s/%s", files_dir, name);
	FILE *file = fopen(path, "rb");
	const size_t got = file ? fread(buffer, 1, size, file) : 0;
	if (file) {
		fclose(file);
	}
	return got;
}

// Boots the image with a fresh model holding the 0.20 project and plays the clip.
static bool ch3_off_run(const uint8_t *image, size_t size)
{
	mdmi_patch *const before = patch;
	if (!ch3_off_load(&ch3_model, &ch3_scratch)) {
		return false;
	}
	if (ch3_on_021) {
		patch_set(&ch3_model, GLOBAL_CH3_MODE, 1);
		patch_set(&ch3_model, GLOBAL_CH3_NOTES, PATCH_CH3_ONE_NOTE);
		patch_set(&ch3_model, GLOBAL_CH3_OP(0, PATCH_CH3_SEMI), PATCH_CH3_SEMI_MAX + 7);
		patch_set(&ch3_model, GLOBAL_CH3_OP(1, PATCH_CH3_FINE), PATCH_CH3_FINE_MAX - 20);
		patch_set(&ch3_model, GLOBAL_CH3_OP(3, PATCH_CH3_FIXED), 1);
		patch_set(&ch3_model, GLOBAL_DAC, 1);
		patch_publish(&ch3_model);
	}
	ch3_model.owner = owner;
	patch = &ch3_model;
	const uint32_t boots = mdmi_engine_boots(engine);
	mdmi_engine_set_rom(engine, image, size);
	while (mdmi_engine_boots(engine) == boots) {
		render(1);
	}
	// gen_reference's clock: the same, so the dealer sees the same moments.
	if (clock_samples < CH3_OFF_CLOCK_START) {
		clock_samples = CH3_OFF_CLOCK_START;
	}
	patch_set_now(patch, clock_samples, RATE);
	ch3_sent_bytes = 0;
	ch3_new_ccs = 0;
	due_emit = emit_ch3;
	ch3_off_play(patch, emit_ch3, NULL, ch3_block, ch3_held, ch3_done);
	due_emit = emit;
	patch = before;
	return true;
}

static void test_ch3_off(void)
{
	printf("A 0.20 project with Ch3 Special off, as 0.20\n");
	_Static_assert(CH3_OFF_RATE == RATE && CH3_OFF_BLOCK == BLOCK, "ch3_off_clip.h runs at reg_test's rate and block");
	size_t rom_size;
	const uint8_t *rom = mdmi_engine_builtin_rom(&rom_size);
	// The core runs the built-in image: one that differs in a byte of the header's name restarts it.
	uint8_t *other = malloc(rom_size);
	memcpy(other, rom, rom_size);
	other[0x150] ^= 0x20;
	const bool ran = ch3_off_run(other, rom_size);
	free(other);
	static uint8_t reference[sizeof(ch3_sent)], regs[sizeof(ch3_regs) + 1];
	const size_t reference_size = read_reference("ch3-off-0.20.bin", reference, sizeof(reference));
	printf("    %d bytes sent besides %d stage 21 controllers, %d in 0.20's\n", (int)ch3_sent_bytes, ch3_new_ccs, (int)reference_size);
	check(ran && reference_size && reference_size == ch3_sent_bytes && !memcmp(reference, ch3_sent, reference_size),
		"from the boot to the clip's end the plugin sends 0.20's bytes besides stage 21's controllers");
	check(ch3_new_ccs == 14, "those are the 14, CC 81 and 82 and Semi, Fine, Fixed of four operators, no more");
	const size_t regs_size = read_reference("ch3-off-0.20-regs.bin", regs, sizeof(regs));
	int differ = 0;
	for (size_t i = 0; regs_size == sizeof(ch3_regs) && i < sizeof(ch3_regs); i++) {
		if (regs[i] != ch3_regs[i]) {
			char name[40];
			ch3_register_name((int)i, name, sizeof(name));
			if (differ++ < 8) {
				printf("    %s: %02X, 0.20 %02X\n", name, ch3_regs[i], regs[i]);
			}
		}
	}
	check(regs_size == sizeof(ch3_regs) && !differ, "with the notes held and at the end every YM and PSG register is as 0.20's ROM left it");
	// Back on the built-in image with the model of the tests after this one.
	boot_image(rom, rom_size);
	check_voices("back on the built-in image every voice is as before", 0, FM_CHANNELS - 1, -1);
}

// reg_test <files> --ch3-off-ref <0.20's ROM> <output>: writes ch3-off-0.20-regs.bin.
static int write_ch3_off_reference(const char *rom_path, const char *out_path)
{
	FILE *file = fopen(rom_path, "rb");
	static uint8_t image[4 << 20];
	const size_t size = file ? fread(image, 1, sizeof(image), file) : 0;
	if (file) {
		fclose(file);
	}
	if (!size) {
		printf("FAIL: %s can't be read\n", rom_path);
		return 1;
	}
	ch3_keep_new_from_rom = true;
	if (!ch3_off_run(image, size)) {
		printf("FAIL: the project doesn't load\n");
		return 1;
	}
	static uint8_t reference[sizeof(ch3_sent)];
	const size_t reference_size = read_reference("ch3-off-0.20.bin", reference, sizeof(reference));
	if (reference_size != ch3_sent_bytes || memcmp(reference, ch3_sent, reference_size)) {
		printf("FAIL: the bytes sent are not ch3-off-0.20.bin; make that first\n");
		return 1;
	}
	file = fopen(out_path, "wb");
	if (!file || fwrite(ch3_regs, 1, sizeof(ch3_regs), file) != sizeof(ch3_regs) || fclose(file)) {
		printf("FAIL: can't write %s\n", out_path);
		return 1;
	}
	printf("%s: %d bytes, 0.20's ROM got 0.20's %d bytes and none of stage 21's %d controllers\n", out_path, (int)sizeof(ch3_regs), (int)ch3_sent_bytes, ch3_new_ccs);
	return 0;
}

// Stage 22, item 6: the clip as a 0.21 project with Ch3 Special on and the DAC, no CSM: the
// registers with the notes held and at the end are those 0.21's ROM held (ch3-on-0.21-regs.bin,
// written by reg_test <files> --ch3-on-ref <0.21's ROM> <output>, 0.21's ROM built as above from
// `git show d373f7b:patches/0003-mdmi-psg-envelopes.patch`). The plugin's bytes are 0.21's: patch.c
// changed only for CSM.
static void test_ch3_on(void)
{
	printf("A 0.21 project with Ch3 Special and the DAC, as 0.21\n");
	size_t rom_size;
	const uint8_t *rom = mdmi_engine_builtin_rom(&rom_size);
	uint8_t *other = malloc(rom_size);
	memcpy(other, rom, rom_size);
	other[0x150] ^= 0x20;
	ch3_on_021 = true;
	const bool ran = ch3_off_run(other, rom_size);
	ch3_on_021 = false;
	free(other);
	static uint8_t regs[sizeof(ch3_regs) + 1];
	const size_t regs_size = read_reference("ch3-on-0.21-regs.bin", regs, sizeof(regs));
	int differ = 0;
	for (size_t i = 0; regs_size == sizeof(ch3_regs) && i < sizeof(ch3_regs); i++) {
		if (regs[i] != ch3_regs[i]) {
			char name[40];
			ch3_register_name((int)i, name, sizeof(name));
			if (differ++ < 8) {
				printf("    %s: %02X, 0.21 %02X\n", name, ch3_regs[i], regs[i]);
			}
		}
	}
	check(ran && regs_size == sizeof(ch3_regs) && !differ, "with the notes held and at the end every YM and PSG register is as 0.21's ROM left it");
	boot_image(rom, rom_size);
	check_voices("back on the built-in image every voice is as before", 0, FM_CHANNELS - 1, -1);
}

// reg_test <files> --ch3-on-ref <0.21's ROM> <output>: writes ch3-on-0.21-regs.bin.
static int write_ch3_on_reference(const char *rom_path, const char *out_path)
{
	FILE *file = fopen(rom_path, "rb");
	static uint8_t image[4 << 20];
	const size_t size = file ? fread(image, 1, sizeof(image), file) : 0;
	if (file) {
		fclose(file);
	}
	ch3_on_021 = true;
	if (!size || !ch3_off_run(image, size)) {
		printf("FAIL: %s can't be read or the project doesn't load\n", rom_path);
		return 1;
	}
	file = fopen(out_path, "wb");
	if (!file || fwrite(ch3_regs, 1, sizeof(ch3_regs), file) != sizeof(ch3_regs) || fclose(file)) {
		printf("FAIL: can't write %s\n", out_path);
		return 1;
	}
	printf("%s: %d bytes after %d bytes sent\n", out_path, (int)sizeof(ch3_regs), (int)ch3_sent_bytes);
	return 0;
}

static void test_load(void)
{
	static uint8_t state[PATCH_NUM_IDS * 6];
	patch_publish(patch);
	patch_save(patch_view(patch), state);
	set_on(0, PARAM_PRESET, 99);
	set_on(1, PARAM_OP_BASE + F_TL, 12);
	set_on(5, PARAM_OP_BASE + F_SR, 30);
	patch_load(patch, state, sizeof(state), NULL, NULL, NULL, NULL);
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
	patch_load_place(patch, 20, &instrument, NULL);
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
		patch_load_place(patch, 20, &instrument, NULL);
		patch_take_incoming(patch);
		patch_sync(patch, &patch->rom, emit, NULL);
		play_each_channel();
		check(compare_file(0, &duck, true) == 0 && !strcmp(patch->model.names[0], "Duck Guitar"), "a new file in the place channel 1 plays reaches it");
	}
	midi(0xC0, 0, 0);
	set_on(2, PARAM_PRESET, 0);
}

// Stage 22c (D56): a library's place tried on a channel goes the way of a dropped file and reaches
// the chip the same, row and all; the ROM bank stays.
static void take_tried(void *context, bool knobs)
{
	(void)context;
	(void)knobs;
	patch_take_incoming(patch);
}

static void test_library_try(void)
{
	file_regs expected;
	patch_instrument instrument;
	if (!file_regs_of("Thunder Guitar 03.tyi", &expected) || !load_file("Thunder Guitar 03.tyi", &instrument)) {
		return;
	}
	static patch_bank library, rom_before;
	memset(&library, 0, sizeof(library));
	patch_bank_rom(&library);
	library.places[7] = instrument;
	char path[MAX_PATH];
	GetTempPathA(sizeof(path), path);
	strcat(path, "mdmi-reg-library.mdbank");
	size_t size;
	uint8_t *bytes = state_bank_build(&library, &size);
	FILE *file = fopen(path, "wb");
	fwrite(bytes, 1, size, file);
	fclose(file);
	free(bytes);
	rom_before = *patch_bank_view(patch);
	static state_view view;
	const files_host host = {patch, &view, take_tried, NULL};
	static mdmi_files files;
	files_init(&files, &host);
	const files_spot spot = {path, 7};
	char message[FILES_MESSAGE];
	moment(FM_CHANNELS - 1, 96, 1, true);
	const bool tried = files_try_place(&files, 1, &spot, message, sizeof(message));
	patch_sync(patch, &patch->rom, emit, NULL);
	moment(1, 60, 1, true);
	render(SETTLE_BLOCKS);
	moment(1, 60, 1, false);
	moment(FM_CHANNELS - 1, 96, 1, false);
	render(SETTLE_BLOCKS);
	check(tried && compare_file(1, &expected, true) == 0 && !strcmp(patch->model.names[1], "Thunder Guitar 03") && !memcmp(patch_bank_view(patch), &rom_before, sizeof(rom_before)),
		"a library's place tried on channel 2 reaches the chip as the dropped file does, the ROM bank stays");
	files_undo(&files, message, sizeof(message));
	patch_sync(patch, &patch->rom, emit, NULL);
	files_free(&files);
	DeleteFileA(path);
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
		tick(KEY_BLOCK);
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
	// A loop start alone, no release: Note Off stops it at once (stage 22b, D54; before, the pass of
	// 30 frames played out).
	drawn(&envelope, 30, 3, 0, -1, 0);
	draw_on(6, &envelope);
	render(SETTLE_BLOCKS);
	midi(0x96, 69, 127);
	render_ms(100);
	const bool pass = mdmi_engine_test_psg_volume(engine, 0) == 3;
	midi(0x86, 69, 0);
	render_ms(40);
	check(pass && mdmi_engine_test_psg_volume(engine, 0) == 15, "a loop start alone: Note Off stops it at once");
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
	// Stage 22b (D54): a short note, Note Off in the intro, sounds the whole envelope: the loop once,
	// then the release.
	midi(0x96, 69, 127);
	render_ms(10);
	midi(0x86, 69, 0);
	render_ms(40);
	const bool short_loop = mdmi_engine_test_psg_volume(engine, 0) == 3;
	render_ms(150);
	const bool short_tail = mdmi_engine_test_psg_volume(engine, 0) == 9;
	render_ms(400);
	check(short_loop && short_tail && mdmi_engine_test_psg_volume(engine, 0) == 15, "a short note: Note Off in the intro, the loop plays once, the release, then silence");
	// Stop: the window hands the envelope without its release; Note Off silences it in the same frame.
	drawn(&envelope, 6, 3, 2, -1, 0);
	draw_on(6, &envelope);
	render(SETTLE_BLOCKS);
	midi(0x96, 69, 127);
	render_ms(200);
	const bool stop_held = mdmi_engine_test_psg_volume(engine, 0) == 3;
	midi(0x86, 69, 0);
	// 30 ms: a frame and the message's way, short of the loop's pass of 67 ms.
	render_ms(30);
	check(stop_held && mdmi_engine_test_psg_volume(engine, 0) == 15, "stop: the loop without its release is silent at Note Off, not after its pass");

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

// Half the swing of the sound between two times after now, in ms, rendered in steps of 1 ms: the
// chip's output has an offset, so the loudest sample alone never reaches silence.
static float peak_between(int from_ms, int to_ms)
{
	float low = 1, high = -1;
	const int step = RATE / 1000;
	for (int ms = 0; ms < to_ms; ms++) {
		tick(step);
		for (int i = 0; ms >= from_ms && i < step; i++) {
			low = left[i] < low ? left[i] : low;
			high = left[i] > high ? left[i] : high;
		}
	}
	return high > low ? (high - low) / 2 : 0;
}

// How long a message takes to reach the chip in the sound's time, measured by test_adjacent.
static int lag_ms;

// The level 6-10 ms into a note that starts now.
static float note_window(void)
{
	return peak_between(lag_ms + 6, lag_ms + 10);
}

static int waiting_voice(int channel, int key)
{
	for (int voice = 0; voice < FM_CHANNELS; voice++) {
		if (patch->voices[voice].channel == channel && patch->voices[voice].key == key) {
			return voice;
		}
	}
	return -1;
}

// A note right after another, with Retrigger Free and Cut (D41), on the slow attack sound of
// test_adjacent; alone is a note's level 6-10 ms in after silence.
static void test_retrigger(int ch, float alone)
{
	set_global(GLOBAL_RETRIGGER, PATCH_RETRIGGER_VOICE);
	midi(0x90, 60, 127);
	render_ms(500);
	midi(0x80, 60, 0);
	midi(0x90, 62, 127);
	const int free_voice = waiting_voice(ch, 62);
	const float free_level = note_window();
	const int keyed = mdmi_engine_test_ym(engine, 0, 0x28);
	midi(0x80, 62, 0);
	render_ms(500);
	printf("  retrigger Free: the next note on voice %d, the chip keyed %02X, %.3f 6-10 ms into it\n", free_voice + 1, keyed, free_level);
	// The level holds the last note's tail on its own voice, which is the point of Free; that the
	// new one starts from silence is that the chip keys the silent voice the dealer chose.
	check(free_voice >= 0 && free_voice != ch && keyed == key_on_value(free_voice) && free_level > 2 * alone,
		"Retrigger Free: a note right after starts on a silent voice, the one the dealer chose, while the last one's tail plays on");
	// Once silent, the own voice plays the channel's next note again.
	midi(0x90, 64, 127);
	peak_between(0, lag_ms + 2);
	const bool home = waiting_voice(ch, 64) == ch && mdmi_engine_test_ym(engine, 0, 0x28) == key_on_value(ch);
	midi(0x80, 64, 0);
	render_ms(500);
	check(home, "Retrigger Free: the own voice comes back for the next note after silence");

	// Cut on a sound with RR 8, so the fast release while it waits can be seen going back.
	set_global(GLOBAL_RETRIGGER, PATCH_RETRIGGER_CUT);
	for (int op = 0; op < PATCH_OPS; op++) {
		set_on(ch, PARAM_OP_BASE + op * PATCH_OP_PARAMS + F_RR, 8);
	}
	midi(0x90, 60, 127);
	render_ms(500);
	midi(0x80, 60, 0);
	midi(0x90, 62, 127);
	const bool waits = patch->waiting_count[ch] == 1;
	float level[100];
	int waited = 0;
	bool fast = false;
	int fast_at = -1, key_at = -1;
	bool keyed_off = false;
	for (int ms = 0; ms < 100; ms++) {
		waited = patch->waiting_count[ch] ? ms + 1 : waited;
		level[ms] = peak_between(0, 1);
		fast = fast || (read_reg(ch, 0x80, 3) & 0x0F) == 15;
		fast_at = fast && fast_at < 0 ? ms : fast_at;
		// The last note's key-off reaches the chip first, then the waiting note's key-on.
		const uint8_t key = mdmi_engine_test_ym(engine, 0, 0x28);
		keyed_off = keyed_off || key == (key_on_value(ch) & 0x0F);
		key_at = key_at < 0 && keyed_off && key == key_on_value(ch) ? ms : key_at;
	}
	float cut_level = 0;
	for (int ms = waited + lag_ms + 6; ms < waited + lag_ms + 10 && ms < 100; ms++) {
		cut_level = level[ms] > cut_level ? level[ms] : cut_level;
	}
	const int back_rr = read_reg(ch, 0x80, 3) & 0x0F;
	midi(0x80, 62, 0);
	render(SETTLE_BLOCKS);
	printf("  retrigger Cut: waits %d, RR 15 meanwhile %d, starts after %d ms with %.3f 6-10 ms into it, RR %d after; on the chip key-on %d ms after RR 15\n", waits, fast, waited, cut_level, back_rr, key_at - fast_at);
	// The plugin's wait is held to 8 ms +-1 (D41); `waited` counts the last waiting millisecond + 1, so
	// 8 ms reads 9. On the chip the gap from RR 15 to key-on also carries the ROM main loop's jitter (its
	// frame handlers delay the FIFO reads), which the plugin doesn't control: only order and a gross bound.
	check(waits && fast && fast_at >= 0 && waited >= 8 && waited <= 10 && key_at > fast_at && key_at - fast_at >= 7 && key_at - fast_at <= 12
			&& cut_level <= 2 * alone && back_rr == 8,
		"Retrigger Cut: the plugin holds the note 8 ms while the voice releases fast; on the chip the key-on follows the fast release (ROM loop jitter allowed) and starts from silence with its own release");

	// A Note Off before it is due: the note never starts, the release comes back.
	midi(0x90, 60, 127);
	render_ms(500);
	midi(0x80, 60, 0);
	midi(0x90, 62, 127);
	peak_between(0, 5);
	midi(0x80, 62, 0);
	const bool cancelled = !patch->waiting_count[ch] && waiting_voice(ch, 62) < 0;
	peak_between(0, lag_ms + 20);
	const bool gave_back = (read_reg(ch, 0x80, 3) & 0x0F) == 8;
	check(cancelled && gave_back, "Retrigger Cut: a Note Off before the note is due cancels it and gives the release back");
	render(SETTLE_BLOCKS);

	// The panic while a note waits, and another channel's note meanwhile.
	midi(0x90, 60, 127);
	render_ms(500);
	midi(0x80, 60, 0);
	midi(0x90, 62, 127);
	midi(0x91, 64, 127);
	const bool other_started = waiting_voice(1, 64) >= 0 && !patch->voices[waiting_voice(1, 64)].pending;
	midi(0xB0, 123, 0);
	const bool panic = !patch->waiting_count[ch];
	midi(0x81, 64, 0);
	peak_between(0, lag_ms + 20);
	const bool panic_back = (read_reg(ch, 0x80, 3) & 0x0F) == 8;
	render(SETTLE_BLOCKS);
	check(other_started && panic && panic_back, "Retrigger Cut: another channel's note doesn't wait, CC 123 ends the wait and gives the release back");

	// Notes with a pause shorter than the release (the owner's clip, 2026-11-08): each one has its
	// attack. Free takes a silent voice, Cut waits; with Porta On the channel glides on its own voice,
	// as in Chip.
	for (int mode = PATCH_RETRIGGER_VOICE; mode <= PATCH_RETRIGGER_CUT; mode++) {
		set_global(GLOBAL_RETRIGGER, mode);
		midi(0x90, 60, 127);
		render_ms(500);
		midi(0x80, 60, 0);
		peak_between(0, 20);
		midi(0x90, 62, 127);
		const bool acts = mode == PATCH_RETRIGGER_CUT ? patch->waiting_count[ch] == 1 : waiting_voice(ch, 62) != ch;
		peak_between(0, 20);
		midi(0x80, 62, 0);
		render_ms(500);
		check(acts, mode == PATCH_RETRIGGER_CUT ? "Retrigger Cut: a note 20 ms after the last, still in its release, waits for silence" : "Retrigger Free: a note 20 ms after the last, still in its release, takes a silent voice");

		set_on(ch, PARAM_PORTA_ON, 1);
		midi(0x90, 60, 127);
		render_ms(300);
		midi(0x80, 60, 0);
		// Not played over, in the last one's release: Cut waits on the channel's one voice, Free can't
		// move it anywhere.
		midi(0x90, 62, 127);
		const bool fresh = waiting_voice(ch, 62) == ch && (mode == PATCH_RETRIGGER_CUT ? patch->waiting_count[ch] == 1 : !patch->waiting_count[ch]);
		peak_between(0, 20);
		// Played over: it glides, nothing waits.
		midi(0x90, 64, 127);
		const bool glides = !patch->waiting_count[ch] && waiting_voice(ch, 64) == ch;
		midi(0x80, 62, 0);
		midi(0x80, 64, 0);
		render_ms(500);
		set_on(ch, PARAM_PORTA_ON, 0);
		render(SETTLE_BLOCKS);
		check(fresh && glides, mode == PATCH_RETRIGGER_CUT ? "Retrigger Cut with Porta On: a note after the last one's end waits for silence, one played over glides" : "Retrigger Free leaves Porta On as it is");
	}
	set_global(GLOBAL_RETRIGGER, PATCH_RETRIGGER_CUT);

	// After a pause longer than the release nothing waits.
	midi(0x90, 60, 127);
	render_ms(500);
	midi(0x80, 60, 0);
	render_ms(300);
	midi(0x90, 62, 127);
	const bool late_waits = patch->waiting_count[ch] != 0;
	midi(0x80, 62, 0);
	render(SETTLE_BLOCKS);
	check(!late_waits, "Retrigger Cut: a note after a pause longer than the release starts at once");

	// A modulator still releasing shapes the next attack's timbre though the carrier is silent (the
	// owner's Overdrive Guitar: OP3 AR 4 RR 3, carrier RR 5): the note waits all the same.
	const int op3 = PARAM_OP_BASE + 2 * PATCH_OP_PARAMS;
	set_on(ch, PARAM_ALGORITHM, 0);
	set_on(ch, op3 + F_TL, 20);
	set_on(ch, op3 + F_RR, 3);
	set_on(ch, PARAM_OP_BASE + 3 * PATCH_OP_PARAMS + F_RR, 15);
	midi(0x90, 60, 127);
	render_ms(500);
	midi(0x80, 60, 0);
	render_ms(100);
	midi(0x90, 62, 127);
	const bool modulator_waits = patch->waiting_count[ch] == 1;
	midi(0x80, 62, 0);
	render_ms(2000);
	set_on(ch, PARAM_ALGORITHM, 7);
	set_on(ch, op3 + F_TL, 127);
	check(modulator_waits, "Retrigger Cut: a note waits while a modulator still releases, its carrier silent");

	set_global(GLOBAL_RETRIGGER, PATCH_RETRIGGER_CHIP);
	for (int op = 0; op < PATCH_OPS; op++) {
		set_on(ch, PARAM_OP_BASE + op * PATCH_OP_PARAMS + F_RR, 8);
	}
}

// Stage 14: why an FM note right after another (its start where the last one ends) sounds as if it
// carried on, and one played over the last does not. One sine with a slow attack and RR 8, silent
// some 40 ms after Note Off; the level 6-10 ms into the second note, counted from when it reaches
// the chip, says where its attack started from.
static void test_adjacent(void)
{
	const int ch = 0;
	set_on(ch, PARAM_ALGORITHM, 7);
	for (int op = 0; op < PATCH_OPS; op++) {
		const int first = PARAM_OP_BASE + op * PATCH_OP_PARAMS;
		set_on(ch, first + F_TL, op == 3 ? 0 : 127);
		set_on(ch, first + F_MUL, 1);
		set_on(ch, first + F_AR, 10);
		set_on(ch, first + F_DR, 0);
		set_on(ch, first + F_SR, 0);
		set_on(ch, first + F_SL, 0);
		set_on(ch, first + F_RR, 8);
		set_on(ch, first + F_SSG, 0);
	}
	render(SETTLE_BLOCKS);
	for (int trial = 0; trial < 6; trial++) {
		peak_between(0, 1 + trial * 5 % 17);
		midi(0x90, 60, 127);
		int ms = 0;
		while (mdmi_engine_test_ym(engine, 0, 0x28) != key_on_value(0) && ms < 100) {
			peak_between(0, 1);
			ms++;
		}
		lag_ms = ms > lag_ms ? ms : lag_ms;
		midi(0x80, 60, 0);
		render(SETTLE_BLOCKS);
	}
	// The silence a note starts from, the floor of the levels below.
	const float floor_level = peak_between(0, 20);
	printf("  adjacent: a note reaches the chip %d ms after it is sent, silence is %.3f (PSG volumes %d %d %d %d)\n", lag_ms, floor_level, mdmi_engine_test_psg_volume(engine, 0),
		mdmi_engine_test_psg_volume(engine, 1), mdmi_engine_test_psg_volume(engine, 2), mdmi_engine_test_psg_volume(engine, 3));
	midi(0x90, 60, 127);
	const float steady = peak_between(400, 500);
	midi(0x80, 60, 0);
	render(SETTLE_BLOCKS);
	midi(0x90, 62, 127);
	const float alone = note_window();
	midi(0x80, 62, 0);
	render(SETTLE_BLOCKS);
	printf("  adjacent: steady %.3f, alone %.3f 6-10 ms into it\n", steady, alone);

	static const int gaps[] = {0, 1, 2, 10, 20, 40, 80};
	float right_after[7];
	for (int g = 0; g < 7; g++) {
		midi(0x90, 60, 127);
		render_ms(500);
		midi(0x80, 60, 0);
		peak_between(0, gaps[g]);
		midi(0x90, 62, 127);
		right_after[g] = note_window();
		midi(0x80, 62, 0);
		render(SETTLE_BLOCKS);
		printf("  adjacent: same voice after %2d ms, %.3f 6-10 ms into it\n", gaps[g], right_after[g]);
	}
	// Played over: the new note is dealt another voice, which is silent, while the last one holds.
	int first_voice = -1, over_voice = -1;
	midi(0x90, 60, 127);
	render_ms(500);
	midi(0x90, 62, 127);
	render_ms(50);
	for (int voice = 0; voice < FM_CHANNELS; voice++) {
		if (patch->voices[voice].channel == ch) {
			first_voice = patch->voices[voice].key == 60 ? voice : first_voice;
			over_voice = patch->voices[voice].key == 62 ? voice : over_voice;
		}
	}
	midi(0x80, 60, 0);
	midi(0x80, 62, 0);
	render(SETTLE_BLOCKS);
	printf("  adjacent: played over, first note on voice %d, the next on voice %d\n", first_voice + 1, over_voice + 1);
	check(alone < steady / 4, "a note alone starts from silence");
	check(first_voice >= 0 && over_voice >= 0 && first_voice != over_voice, "a note played over gets another voice");
	check(right_after[0] > steady / 2 && right_after[6] <= 2 * alone, "a note right after on the same voice starts its attack from the level the last one left, after a pause from silence (the chip)");

	test_retrigger(ch, alone);

	// How long the pause must be by RR before the last note is 20 dB down where the next one would
	// reach the chip: patch.c's RELEASE_MS (stage 22b, D41).
	static const int pauses[] = {4, 6, 8, 10, 12, 14, 16, 20, 24, 28, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 448, 512, 640, 768, 896, 1024, 1280, 1536, 1792, 2048, 2560, 3072, 3584, 4096};
	const int pause_count = (int)(sizeof(pauses) / sizeof(*pauses));
	bool counted_ok = true;
	for (int rr = 15; rr >= 2; rr--) {
		for (int op = 0; op < PATCH_OPS; op++) {
			set_on(ch, PARAM_OP_BASE + op * PATCH_OP_PARAMS + F_RR, rr);
		}
		// Stage 22b (D41): the fading note itself, its peak in the 4 ms (a period of key 60) before the
		// next note would reach the chip, at most -20 dB of steady, what the counts mean since stage 14
		// ("2x alone" leaned on a floor a stray PSG tone left until 22b; -30 dB comes at about 1.4 T).
		int needed = -1;
		float residual = 0;
		for (int p = 0; p < pause_count && needed < 0; p++) {
			midi(0x90, 60, 127);
			render_ms(500);
			midi(0x80, 60, 0);
			const int until = lag_ms + pauses[p];
			residual = peak_between(until - 4 > 0 ? until - 4 : 0, until);
			needed = residual <= steady / 10 ? pauses[p] : -1;
			render(SETTLE_BLOCKS);
		}
		printf("  adjacent: RR %2d needs a pause of %d ms (%.1f dB), the dealer counts %d\n", rr, needed, residual > 0 ? 20 * log10f(residual / steady) : -120.0f, patch_release_ms(rr));
		// 4 ms is the window itself: such counts are held to its floor, 8 ms (pauses 4, 6, 8).
		const int count = patch_release_ms(rr);
		const int limit = count <= 4 ? 8 : count * 3 / 2;
		counted_ok = counted_ok && needed > 0 && needed <= limit;
	}
	check(counted_ok, "the dealer's release times (RELEASE_MS) hold for the chip, within one step of the pauses");
	set_on(ch, PARAM_PRESET, 0);
}

static void reset_energy(void)
{
	memset(out_energy, 0, sizeof(out_energy));
}

// The outputs that sounded since reset_energy, as a bit per output: those within 30 dB of the loudest.
// Below that lie the YM2612's ladder steps: a silent voice's offset jumps when the ROM sets the next
// note's pan before its key-on, and that step still belongs to the voice's old channel (D46), 37 dB
// under the note in the Ch3 Special test.
static int loud_outputs(void)
{
	double most = 0;
	for (int o = 0; o < MDMI_OUTPUTS; o++) {
		most = out_energy[o] > most ? out_energy[o] : most;
	}
	int mask = 0;
	for (int o = 0; o < MDMI_OUTPUTS; o++) {
		if (most > 0 && out_energy[o] > most * 1e-3) {
			mask |= 1 << o;
		}
	}
	return mask;
}

static void check_outputs(int want, const char *what)
{
	const int got = loud_outputs();
	char list[64] = "";
	for (int o = 0; o < MDMI_OUTPUTS; o++) {
		if (got >> o & 1) {
			snprintf(list + strlen(list), sizeof(list) - strlen(list), " %d", o + 1);
		}
	}
	printf("  %s: %s (outputs sounding:%s)\n", got == want ? "ok  " : "FAIL", what, got ? list : " none");
	if (got != want) {
		failures++;
		for (int o = 0; o < MDMI_OUTPUTS; o++) {
			printf("    output %d: %.1f dB\n", o + 1, 10.0 * log10(out_energy[o] + 1e-30));
		}
	}
}

// Every note off, and then until the releases have died away, at most 10 s.
static void quiet(void)
{
	patch_all_notes_off(patch, emit, NULL);
	for (int i = 0; i < 200; i++) {
		reset_energy();
		render(10);
		double most = 0;
		for (int o = 0; o < MDMI_OUTPUTS; o++) {
			most = out_energy[o] > most ? out_energy[o] : most;
		}
		// Until the envelopes have ended: a release near silence that the next note's registers make
		// louder again before its key-on is still the old channel's (D46).
		if (most < 1e-10) {
			break;
		}
	}
	reset_energy();
}

// Stage 17 (D46): each MIDI channel has an output, and a voice plays into the output of the channel
// whose note it last started.
static void test_outputs(void)
{
	// test_restart leaves PSG 2 sounding with its envelope of no release (ROADMAP); a note with the
	// bank's envelope ends it.
	midi(0xC7, 0, 0);
	midi(0x97, 60, 127);
	midi(0x87, 60, 0);
	quiet();
	play_chord(3, true);
	int lent = 0;
	for (int voice = 1; voice < FM_CHANNELS; voice++) {
		lent += patch->playing[voice] == 0;
	}
	check(lent == 2, "a chord of three on channel 1 borrows two voices");
	reset_energy();
	render(SETTLE_BLOCKS);
	check_outputs(1 << 0, "the whole chord plays into output 1");
	// A channel whose own voice the chord borrowed plays: its note moves the chord's note on, and
	// the voice comes to its output only at the key-on of its note; until then the voice is output 1's.
	int taken = -1;
	for (int voice = FM_CHANNELS - 1; voice > 0; voice--) {
		taken = patch->playing[voice] == 0 ? voice : taken;
	}
	mdmi_engine_test_take_owner_wait(engine);
	midi((uint8_t)(0x90 | taken), 72, 127);
	double before_new = 0, before_old = 0;
	int blocks_before = 0;
	for (int i = 0; i < 400 && mdmi_engine_test_owner(engine, taken) != taken; i++) {
		reset_energy();
		render(1);
		// The block the key-on falls in already plays into the new output after it: only whole
		// blocks before it count (it depends on where in a block the key-on falls).
		if (mdmi_engine_test_owner(engine, taken) != taken) {
			before_new += out_energy[taken];
			before_old += out_energy[0];
			blocks_before++;
		}
	}
	const float wait = mdmi_engine_test_take_owner_wait(engine);
	printf("    voice %d came to output %d after %d blocks, %.1f ms after its note reached the ROM; before it output %d %.3g, output 1 %.3g\n", taken + 1,
		taken + 1, blocks_before, wait, taken + 1, before_new, before_old);
	check(mdmi_engine_test_owner(engine, taken) == taken && wait > 0.0f && before_new < 1e-12 && before_old > 0,
		"until its key-on the taken voice plays into output 1 and the new channel's output is silent");
	render(SETTLE_BLOCKS);
	reset_energy();
	render(SETTLE_BLOCKS);
	check_outputs(1 << 0 | 1 << taken, "then the channel plays into its own output, the chord stays in output 1");
	quiet();
	// Every voice the chord's: the DAC's voice must still be channel 6's once the DAC is on.
	play_chord(FM_CHANNELS, true);
	quiet();
	check(mdmi_engine_test_owner(engine, FM_CHANNELS - 1) == 0, "a chord of six on channel 1 holds voice 6 too");

	set_global(GLOBAL_CH3_MODE, 1);
	sync_and_settle();
	reset_energy();
	midi(0x92, 60, 127);
	for (int op = 0; op < 3; op++) {
		midi((uint8_t)(0x9A + op), (uint8_t)(64 + op * 3), 127);
	}
	render(SETTLE_BLOCKS);
	check_outputs(1 << 2, "Ch3 Special with MIDI 11-13: all in output 3");
	for (int op = 0; op < 3; op++) {
		midi((uint8_t)(0x8A + op), (uint8_t)(64 + op * 3), 0);
	}
	set_global(GLOBAL_CH3_MODE, 0);
	sync_and_settle();
	quiet();

	set_global(GLOBAL_DAC, 1);
	wait_for_voices(FM_CHANNELS - 2);
	quiet();
	check(mdmi_engine_test_owner(engine, FM_CHANNELS - 1) == FM_CHANNELS - 1, "the DAC switched on gives voice 6 to channel 6 at once");
	midi(0x95, 36, 127);
	render(SETTLE_BLOCKS);
	check_outputs(1 << 5, "a DAC sample plays into output 6");
	set_global(GLOBAL_DAC, 0);
	wait_for_voices(FM_CHANNELS - 1);
	quiet();

	midi(0x96, 64, 127);
	render(SETTLE_BLOCKS);
	check_outputs(1 << 6, "PSG tone 1 plays into output 7");
	quiet();
	set_global(GLOBAL_NOISE_TYPE, PATCH_NOISE_PERIODIC);
	set_global(GLOBAL_NOISE_RATE, PATCH_NOISE_RATE_TONE3);
	sync_and_settle();
	reset_energy();
	midi(0x99, 45, 127);
	render(SETTLE_BLOCKS);
	check_outputs(1 << 9, "noise on tone 3 plays into output 10, output 9 is silent");
	quiet();
	set_global(GLOBAL_NOISE_RATE, PATCH_NOISE_RATE_KIT);
	sync_and_settle();
	reset_energy();
	midi(0x99, 36, 127);
	render(SETTLE_BLOCKS);
	check_outputs(1 << 9, "a Kit drum plays into output 10, output 9 is silent");
	set_global(GLOBAL_NOISE_TYPE, PATCH_NOISE_WHITE);
	set_global(GLOBAL_NOISE_RATE, PATCH_NOISE_RATE_HIGH);
	sync_and_settle();
	quiet();
}

// Stage 18a (D48): the PSG's voices play 3 dB above BlastEm's own mix, FM as it is. A note alone,
// its level on the outputs against the core's mix of the same frames.
static double measured_rms;

static double measured_note_db(uint8_t status, uint8_t note, double *raw)
{
	quiet();
	mdmi_engine_test_measure(engine, true);
	midi(status, note, 127);
	render(SETTLE_BLOCKS);
	midi((uint8_t)(0x80 | (status & 0x0F)), note, 0);
	mdmi_engine_test_measure(engine, false);
	*raw = mdmi_engine_test_measured_db(engine, true);
	measured_rms = mdmi_engine_test_measured_rms(engine);
	return mdmi_engine_test_measured_db(engine, false);
}

static void test_psg_level(void)
{
	double tone_raw, noise_raw, fm_raw;
	const double tone = measured_note_db(0x96, 64, &tone_raw);
	const double tone_rms = measured_rms;
	const double noise = measured_note_db(0x99, 60, &noise_raw);
	const double noise_rms = measured_rms;
	const double fm = measured_note_db(0x90, 60, &fm_raw);
	printf("    against BlastEm's mix: PSG tone %+.3f dB, noise %+.3f dB, FM %+.3f dB; the voices as the core made them %+.3f, %+.3f, %+.3f dB\n",
		tone, noise, fm, tone_raw, noise_raw, fm_raw);
	printf("    BlastEm's mix RMS: tone %.1f, noise %.1f steps of 16 bits\n", tone_rms * 32768, noise_rms * 32768);
	check(fabs(tone - tone_raw - 3.0) <= 0.01 && fabs(noise - noise_raw - 3.0) <= 0.01, "the PSG's tones and noise play 3.00 dB above the voices the core made");
	check(fabs(fm - fm_raw) <= 0.01, "an FM note plays as the core made it");
	// The voices against the core's own int16 mix, as in 0.17: this white noise is quiet (about 70 steps
	// RMS), so the mix's rounding of up to 13 steps puts the voices 0.3 dB above it (ROADMAP, from 18a).
	check(fabs(tone - 3.0) <= 0.1 && fabs(fm) <= 0.1, "a PSG tone is 3 dB above BlastEm's mix and FM at it, within 0.1 dB");
	check(fabs(noise - 3.0) <= 0.5, "the PSG noise is 3 dB above BlastEm's mix, within 0.5 dB");
	quiet();
}

static void test_reset(void)
{
	const uint8_t reset = 0xFF;
	patch_midi(patch, &reset, 1, emit, NULL);
	render(RESET_BLOCKS);
	play_chord(0, false);
	check_voices("the ROM's reset: every voice comes back", 0, FM_CHANNELS - 1, -1);
}

// Stage 20, item 2: a row 0 +3 +4 +7 +12 looped from its first step, on C4, gives frame after frame
// the pitch registers of C4, D#4, E4, G4, C5 played without a row, on FM (block and F-num) and on PSG
// (tone); a chord of three on borrowed voices steps all three. The row comes through the model, as
// the window hands it.
#define ARP_TEST_STEPS 5
static const int ARP_TEST[ARP_TEST_STEPS] = {0, 3, 4, 7, 12};
#define ARP_POLL 16
// A value counts once it holds a quarter of a frame: a step holds a frame, a pitch written in two
// halves can show the half for a moment.
#define ARP_HOLD 200
#define ARP_SEEN 24

static void arp_on(int channel, bool on)
{
	patch_arp row = PATCH_ARP_EMPTY;
	if (on) {
		row.count = ARP_TEST_STEPS;
		row.loop = 0;
		for (int i = 0; i < ARP_TEST_STEPS; i++) {
			row.steps[i] = (int8_t)ARP_TEST[i];
		}
	}
	patch_load_arp(patch, channel, &row, NULL);
	patch_take_incoming(patch);
	patch_sync(patch, &patch->rom, emit, NULL);
	render(SETTLE_BLOCKS);
}

// A pitch register: an FM voice's A4 and A0 as one, or with a negative source PSG tone -1 - source.
static uint16_t pitch_of(int source)
{
	return source >= 0 ? (uint16_t)(read_reg(source, 0xA4, 0) << 8 | read_reg(source, 0xA0, 0)) : mdmi_engine_test_psg_tone(engine, (uint8_t)(-1 - source));
}

// A held note's register without a row, read on the voice the note took.
static uint16_t plain_pitch(int channel, int key)
{
	midi((uint8_t)(0x90 | channel), (uint8_t)key, 127);
	render(SETTLE_BLOCKS);
	int voices[FM_CHANNELS];
	const uint16_t pitch = pitch_of(channel < FM_CHANNELS ? (voices_of(channel, voices) ? voices[0] : channel) : -1 - (channel - FM_CHANNELS));
	midi((uint8_t)(0x80 | channel), (uint8_t)key, 0);
	render(SETTLE_BLOCKS);
	return pitch;
}

// The values each source's register takes for frames frames, in order, polled every 16 samples, each
// with the sample it began at; a value counts once it held ARP_HOLD samples.
static void poll_pitches(const int *sources, int count, int frames, uint16_t (*seen)[ARP_SEEN], int (*at)[ARP_SEEN], int *seen_count)
{
	uint16_t last[FM_CHANNELS];
	int since[FM_CHANNELS];
	for (int k = 0; k < count; k++) {
		seen_count[k] = 0;
		last[k] = pitch_of(sources[k]);
		since[k] = 0;
	}
	for (int t = 1; t <= frames * RATE / 60 / ARP_POLL; t++) {
		tick(ARP_POLL);
		for (int k = 0; k < count; k++) {
			const uint16_t pitch = pitch_of(sources[k]);
			if (pitch != last[k]) {
				last[k] = pitch;
				since[k] = t * ARP_POLL;
			} else if (t * ARP_POLL - since[k] >= ARP_HOLD && t * ARP_POLL - since[k] < ARP_HOLD + ARP_POLL
				&& (!seen_count[k] || seen[k][seen_count[k] - 1] != pitch) && seen_count[k] < ARP_SEEN) {
				at[k][seen_count[k]] = since[k];
				seen[k][seen_count[k]++] = pitch;
			}
		}
	}
}

// Two rounds of the row from the first value equal to the root, each value a frame (800 samples at
// 60 Hz) after the one before; the first one starts at the note, inside a frame.
static bool steps_like(const uint16_t *seen, const int *at, int count, const uint16_t *expected, const char *what)
{
	int first = 0;
	while (first < count && seen[first] != expected[0]) {
		first++;
	}
	bool same = first + 2 * ARP_TEST_STEPS <= count;
	// The first two steps aren't timed: the note starts inside a frame, and the ROM's frame handler
	// runs from its main loop, which a chord's notes keep busy for a few ms (stage 21: one step of a
	// borrowed voice was written 10 ms after its frame). From the third step on each lands on the
	// frames' grid.
	for (int j = 0; same && j < 2 * ARP_TEST_STEPS; j++) {
		const int off = j > 2 ? at[first + j] - at[first + 2] - (j - 2) * 800 : 0;
		same = seen[first + j] == expected[j % ARP_TEST_STEPS] && off >= -200 && off <= 200;
	}
	if (!same) {
		printf("    %s:", what);
		for (int j = 0; j < count; j++) {
			printf(" %04X@%d", seen[j], at[j]);
		}
		printf("; expected");
		for (int j = 0; j < ARP_TEST_STEPS; j++) {
			printf(" %04X", expected[j]);
		}
		printf("\n");
	}
	return same;
}

static void test_arp(void)
{
	printf("  arp rows\n");
	static const int CHANNELS[2] = {0, FM_CHANNELS};
	static const char *const NAMES[2] = {"FM channel 1", "PSG channel 7"};
	patch_envelope plain;
	steady(&plain, 0, 2);
	draw_on(FM_CHANNELS, &plain);
	for (int c = 0; c < 2; c++) {
		const int channel = CHANNELS[c];
		set_on(channel, PARAM_PORTA_ON, 0);
		arp_on(channel, false);
		uint16_t expected[ARP_TEST_STEPS];
		for (int i = 0; i < ARP_TEST_STEPS; i++) {
			expected[i] = plain_pitch(channel, 60 + ARP_TEST[i]);
		}
		arp_on(channel, true);
		midi((uint8_t)(0x90 | channel), 60, 127);
		const int source = c ? -1 : 0;
		uint16_t seen[1][ARP_SEEN];
		int at[1][ARP_SEEN], seen_count[1];
		poll_pitches(&source, 1, 20, seen, at, seen_count);
		midi((uint8_t)(0x80 | channel), 60, 0);
		render(SETTLE_BLOCKS);
		char what[96];
		snprintf(what, sizeof(what), "%s: C4 with the row plays C4, D#4, E4, G4, C5 a frame each, round again", NAMES[c]);
		check(steps_like(seen[0], at[0], seen_count[0], expected, NAMES[c]), what);
		arp_on(channel, false);
	}

	// A chord of three on channel 4: its voices are borrowed, each steps from its own note.
	static const int CHORD_KEYS[3] = {60, 64, 67};
	const int channel = 3;
	set_on(channel, PARAM_PORTA_ON, 0);
	uint16_t expected[3][ARP_TEST_STEPS];
	for (int n = 0; n < 3; n++) {
		for (int i = 0; i < ARP_TEST_STEPS; i++) {
			expected[n][i] = plain_pitch(channel, CHORD_KEYS[n] + ARP_TEST[i]);
		}
	}
	arp_on(channel, true);
	for (int n = 0; n < 3; n++) {
		const uint8_t on[3] = {0x93, (uint8_t)CHORD_KEYS[n], 127};
		patch_midi(patch, on, 3, emit, NULL);
	}
	patch_flush(patch, emit, NULL);
	int voices[FM_CHANNELS];
	const int count = voices_of(channel, voices);
	uint16_t seen[FM_CHANNELS][ARP_SEEN];
	int at[FM_CHANNELS][ARP_SEEN], seen_count[FM_CHANNELS];
	poll_pitches(voices, count, 24, seen, at, seen_count);
	int stepping = 0;
	for (int k = 0; k < count; k++) {
		for (int n = 0; n < 3; n++) {
			if (patch->voices[voices[k]].key == CHORD_KEYS[n]) {
				stepping += steps_like(seen[k], at[k], seen_count[k], expected[n], "chord voice");
			}
		}
	}
	for (int n = 0; n < 3; n++) {
		midi(0x83, (uint8_t)CHORD_KEYS[n], 0);
	}
	render(SETTLE_BLOCKS);
	printf("    the chord took %d voices, %d of them step\n", count, stepping);
	check(count == 3 && stepping == 3, "a chord of three on borrowed voices: all three step from their own notes");
	arp_on(channel, false);
}

static void checkf(bool passed, const char *format, ...)
{
	char text[256];
	va_list args;
	va_start(args, format);
	vsnprintf(text, sizeof(text), format, args);
	va_end(args);
	check(passed, text);
}

// Stage 21 (D51): channel 3 by operators. The chip's 0x28 holds the last byte written there.
#define CH3_CAPTURE (RATE / 2)
static float ch3_wave[CH3_CAPTURE];

// A plain sine voice on a channel: algorithm 7, every operator at MUL 1, AR 31, no decay, release
// rr, at its own TL; FMS for the vibrato test.
static void sine_voice(int channel, const int *tl, int rr, int fms)
{
	patch_set(patch, PATCH_CHANNEL_ID(channel, PARAM_ALGORITHM), 7);
	patch_set(patch, PATCH_CHANNEL_ID(channel, PARAM_FEEDBACK), 0);
	patch_set(patch, PATCH_CHANNEL_ID(channel, PARAM_AMS), 0);
	patch_set(patch, PATCH_CHANNEL_ID(channel, PARAM_FMS), fms);
	static const struct {
		int field, value;
	} PLAIN[] = {{F_MUL, 1}, {F_DT, 0}, {F_RS, 0}, {F_AR, 31}, {F_DR, 0}, {F_SR, 0}, {F_SL, 0}, {F_AM, 0}, {F_SSG, 0}};
	for (int op = 0; op < PATCH_OPS; op++) {
		const int first = PARAM_OP_BASE + op * PATCH_OP_PARAMS;
		for (size_t i = 0; i < sizeof(PLAIN) / sizeof(*PLAIN); i++) {
			patch_set(patch, PATCH_CHANNEL_ID(channel, first + PLAIN[i].field), PLAIN[i].value);
		}
		patch_set(patch, PATCH_CHANNEL_ID(channel, first + F_TL), tl[op]);
		patch_set(patch, PATCH_CHANNEL_ID(channel, first + F_RR), rr);
	}
	sync_and_settle();
}

// An output's left side for some samples, at most CH3_CAPTURE.
static int capture_output(int output, int samples)
{
	int n = 0;
	while (n < samples) {
		const int frames = samples - n < BLOCK ? samples - n : BLOCK;
		tick(frames);
		for (int i = 0; i < frames; i++) {
			ch3_wave[n++] = outputs[2 * output][i];
		}
	}
	return n;
}

// The level at a frequency over the captured samples, Hann window (Goertzel).
static double level_at(int n, double hz)
{
	const double k = 2 * cos(2 * 3.14159265358979 * hz / RATE);
	double s1 = 0, s2 = 0;
	for (int i = 0; i < n; i++) {
		const double w = 0.5 - 0.5 * cos(2 * 3.14159265358979 * i / (n - 1));
		const double s0 = ch3_wave[i] * w + k * s1 - s2;
		s2 = s1;
		s1 = s0;
	}
	return sqrt(fmax(s1 * s1 + s2 * s2 - k * s1 * s2, 0)) / n;
}

static double key_hz(double key)
{
	return 440.0 * pow(2.0, (key - 69) / 12.0);
}

// Where the strongest partial near a key lies, in cents from it, searched 60 cents either side.
static double peak_cents(int n, double key)
{
	double best = 0, best_level = -1;
	for (double c = -60; c <= 60; c += 0.25) {
		const double level = level_at(n, key_hz(key) * pow(2.0, c / 1200));
		if (level > best_level) {
			best_level = level;
			best = c;
		}
	}
	return best;
}

static double db(double level)
{
	return 20 * log10(level + 1e-12);
}

static uint8_t key_reg(void)
{
	return mdmi_engine_test_ym(engine, 0, 0x28);
}

// Items 5 and 1-3 through the plugin: the bytes of 0x28 on every event, a released operator dies
// while the others sound.
static void test_ch3_keys(void)
{
	printf("  channel 3 by operators\n");
	static const int LOUD[4] = {10, 10, 10, 10};
	// RR 8 is release rate 34 in BlastEm: about 300 dB a second, so 30 dB in about 100 ms.
	const int rr = 8, release_ms = 100;
	sine_voice(2, LOUD, rr, 0);
	set_global(GLOBAL_CH3_MODE, 1);
	set_global(GLOBAL_CH3_NOTES, PATCH_CH3_OPS);
	sync_and_settle();
	uint8_t seen[8];
	midi(0x9A, 60, 127);
	render(4);
	seen[0] = key_reg();
	midi(0x92, 72, 127);
	render(4);
	seen[1] = key_reg();
	midi(0x8A, 60, 0);
	render(4);
	seen[2] = key_reg();
	midi(0x82, 72, 0);
	render(4);
	seen[3] = key_reg();
	printf("    Ops: 0x28 %02X %02X %02X %02X\n", seen[0], seen[1], seen[2], seen[3]);
	checkf(seen[0] == 0x12 && seen[1] == 0x92 && seen[2] == 0x82 && seen[3] == 0x02, "Ops: MIDI 11 on 0x12, MIDI 3 on 0x92, MIDI 11 off 0x82, MIDI 3 off 0x02");
	midi(0x9A, 60, 127);
	midi(0x9B, 64, 127);
	render(4);
	seen[0] = key_reg();
	set_global(GLOBAL_CH3_NOTES, PATCH_CH3_ONE_NOTE);
	sync_and_settle();
	seen[1] = key_reg();
	midi(0x8A, 60, 0);
	midi(0x8B, 64, 0);
	render(4);
	checkf(seen[0] == 0x32 && seen[1] == 0x02, "Ch3 Notes changed while operators 1 and 2 are held (0x%02X): 0x28 0x%02X, all released", seen[0], seen[1]);

	midi(0x92, 60, 127);
	render(4);
	seen[0] = key_reg();
	const uint8_t a9 = mdmi_engine_test_ym(engine, 0, 0xA9), ad = mdmi_engine_test_ym(engine, 0, 0xAD);
	midi(0x9A, 79, 127);
	render(4);
	seen[1] = key_reg();
	const bool pitch_kept = mdmi_engine_test_ym(engine, 0, 0xA9) == a9 && mdmi_engine_test_ym(engine, 0, 0xAD) == ad;
	midi(0x8A, 79, 0);
	midi(0x82, 60, 0);
	render(4);
	seen[2] = key_reg();
	checkf(seen[0] == 0xF2 && seen[1] == 0xF2 && pitch_kept && seen[2] == 0x02, "One Note: MIDI 3 on 0x%02X, a MIDI 11 note writes neither 0x28 nor A9/AD, off 0x%02X", seen[0], seen[2]);
	set_global(GLOBAL_CH3_MODE, 0);
	sync_and_settle();
	const uint8_t a9_off = mdmi_engine_test_ym(engine, 0, 0xA9);
	seen[0] = key_reg();
	midi(0x9A, 84, 127);
	render(4);
	checkf(key_reg() == seen[0] && mdmi_engine_test_ym(engine, 0, 0xA9) == a9_off, "Ch3 Special off: a MIDI 11 note writes neither 0x28 nor A9");
	midi(0x8A, 84, 0);

	// Item 5: C4, E4, G4 on MIDI 11-13, C5 on MIDI 3; MIDI 12 off: E4 dies away within its release.
	set_global(GLOBAL_CH3_MODE, 1);
	set_global(GLOBAL_CH3_NOTES, PATCH_CH3_OPS);
	sync_and_settle();
	static const int KEYS[4] = {60, 64, 67, 72};
	static const uint8_t ON[4] = {0x9A, 0x9B, 0x9C, 0x92};
	for (int op = 0; op < 4; op++) {
		midi(ON[op], (uint8_t)KEYS[op], 127);
	}
	render(SETTLE_BLOCKS);
	reset_energy();
	int n = capture_output(2, RATE / 5);
	double held[4], after[4];
	for (int op = 0; op < 4; op++) {
		held[op] = level_at(n, key_hz(KEYS[op]));
	}
	const int others = loud_outputs();
	midi(0x8B, 64, 0);
	render_ms(release_ms);
	n = capture_output(2, RATE / 5);
	bool steady = true;
	for (int op = 0; op < 4; op++) {
		after[op] = level_at(n, key_hz(KEYS[op]));
		steady = steady && (op == 1 || fabs(db(after[op]) - db(held[op])) <= 1.0);
	}
	printf("    held %.1f %.1f %.1f %.1f dB, %d ms after MIDI 12's Note Off %.1f %.1f %.1f %.1f dB\n", db(held[0]), db(held[1]), db(held[2]), db(held[3]), release_ms,
		db(after[0]), db(after[1]), db(after[2]), db(after[3]));
	checkf(others == 1 << 2 && db(held[1]) - db(after[1]) >= 30 && steady, "algorithm 7: after MIDI 12's Note Off its E4 falls %.1f dB within RR, C4, G4, C5 stay within 1 dB, all in output 3",
		db(held[1]) - db(after[1]));
	for (int op = 0; op < 4; op++) {
		midi((uint8_t)(ON[op] - 0x10), (uint8_t)KEYS[op], 0);
	}
	render(SETTLE_BLOCKS);

	// Item 6: One Note, Semi 0 (Fixed), +4, +7, +12 on C4; a bend of +2 moves three, Fixed stays.
	set_global(GLOBAL_CH3_NOTES, PATCH_CH3_ONE_NOTE);
	static const int SEMI[4] = {0, 4, 7, 12};
	for (int op = 0; op < 4; op++) {
		set_global(GLOBAL_CH3_OP(op, PATCH_CH3_SEMI), PATCH_CH3_SEMI_MAX + SEMI[op]);
	}
	set_global(GLOBAL_CH3_OP(0, PATCH_CH3_FIXED), 1);
	sync_and_settle();
	midi(0x92, 60, 127);
	render(SETTLE_BLOCKS);
	n = capture_output(2, CH3_CAPTURE);
	double plain[4], bent[4];
	for (int op = 0; op < 4; op++) {
		plain[op] = peak_cents(n, 60 + SEMI[op]);
	}
	midi(0xE2, 0x7F, 0x7F);
	render(SETTLE_BLOCKS);
	n = capture_output(2, CH3_CAPTURE);
	bool in_tune = true, bend_right = true;
	for (int op = 0; op < 4; op++) {
		bent[op] = peak_cents(n, 60 + SEMI[op] + (op ? 2 : 0));
		in_tune = in_tune && fabs(plain[op]) <= 5;
		bend_right = bend_right && fabs(bent[op]) <= 5;
	}
	midi(0xE2, 0x00, 0x40);
	midi(0x82, 60, 0);
	render(SETTLE_BLOCKS);
	printf("    One Note on C4: %+.2f %+.2f %+.2f %+.2f cents from C4 E4 G4 C5; bent +2: %+.2f %+.2f %+.2f %+.2f from C4 F#4 A4 D5\n", plain[0], plain[1], plain[2], plain[3],
		bent[0], bent[1], bent[2], bent[3]);
	checkf(in_tune && bend_right, "One Note: four partials on their intervals within 5 cents; a bend of +2 moves three, the Fixed one stays");
	for (int op = 0; op < 4; op++) {
		set_global(GLOBAL_CH3_OP(op, PATCH_CH3_SEMI), PATCH_CH3_SEMI_MAX);
	}
	set_global(GLOBAL_CH3_OP(0, PATCH_CH3_FIXED), 0);
	set_global(GLOBAL_CH3_NOTES, PATCH_CH3_OPS);
	set_global(GLOBAL_CH3_MODE, 0);
	sync_and_settle();
	midi(0xC2, 0, 0);
	render(SETTLE_BLOCKS);
}

// The vibrato of a note in the captured wave, from its start: the frequency over each 5 periods,
// in cents from the key; how many it got.
#define VIBRATO_WINDOWS 128
static int vibrato_curve(int n, int key, double *cents)
{
	float peak = 0;
	for (int i = 0; i < n; i++) {
		peak = fabsf(ch3_wave[i]) > peak ? fabsf(ch3_wave[i]) : peak;
	}
	int onset = 0;
	while (onset < n && fabsf(ch3_wave[onset]) < peak * 0.01f) {
		onset++;
	}
	double crossings[1024];
	int count = 0;
	for (int i = onset + 1; i < n && count < 1024; i++) {
		if (ch3_wave[i - 1] < 0 && ch3_wave[i] >= 0) {
			crossings[count++] = i - 1 + ch3_wave[i - 1] / (ch3_wave[i - 1] - ch3_wave[i]);
		}
	}
	int windows = 0;
	for (int k = 0; k + 5 < count && windows < VIBRATO_WINDOWS && crossings[k + 5] - crossings[0] < RATE / 5; k += 5) {
		const double hz = 5.0 * RATE / (crossings[k + 5] - crossings[k]);
		cents[windows++] = 1200 * log2(hz / key_hz(key));
	}
	return windows;
}

// Item 7, the experiment before the work: FMS 7, five notes at random moments on MIDI 1. With LFO
// Reset 1 the vibrato starts alike on every note; without it, it doesn't.
static double vibrato_spread(int reset, double *range)
{
	set_global(GLOBAL_LFO_RESET, reset);
	sync_and_settle();
	static double curves[5][VIBRATO_WINDOWS];
	int windows = VIBRATO_WINDOWS;
	uint32_t seed = 0x2151u + (uint32_t)reset;
	double spread = 0, low = 1e9, high = -1e9;
	for (int note = 0; note < 5; note++) {
		seed = seed * 1664525u + 1013904223u;
		int wait = 300 * RATE / 1000 + (int)(seed >> 8) % (600 * RATE / 1000);
		while (wait > 0) {
			const int frames = wait < BLOCK ? wait : BLOCK;
			tick(frames);
			wait -= frames;
		}
		midi(0x90, 69, 127);
		const int n = capture_output(0, RATE / 2);
		midi(0x80, 69, 0);
		const int got = vibrato_curve(n, 69, curves[note]);
		windows = got < windows ? got : windows;
	}
	for (int j = 0; j < windows; j++) {
		for (int note = 0; note < 5; note++) {
			spread = fmax(spread, fabs(curves[note][j] - curves[0][j]));
			low = fmin(low, curves[note][j]);
			high = fmax(high, curves[note][j]);
		}
	}
	printf("    LFO Reset %d: %d windows of 5 periods over 200 ms, the notes differ by %.2f cents at most; the vibrato spans %+.1f to %+.1f cents\n", reset, windows, spread, low, high);
	*range = high - low;
	return windows >= 15 ? spread : 1e9;
}

static void test_lfo_reset(void)
{
	static const int ONE[4] = {127, 127, 127, 0};
	sine_voice(0, ONE, 15, 7);
	set_global(GLOBAL_LFO_ON, 1);
	set_global(GLOBAL_LFO_RATE, 0);
	double range_reset, range_free;
	const double with = vibrato_spread(1, &range_reset);
	const double without = vibrato_spread(0, &range_free);
	checkf(with <= 2.0 && range_reset > 20, "LFO Reset 1: five notes at random moments start the same vibrato within 2 cents (%.2f)", with);
	checkf(without > 20.0, "without it they differ by more than 20 cents (%.1f)", without);
	set_global(GLOBAL_LFO_RESET, 0);
	sync_and_settle();
	midi(0xC0, 0, 0);
	render(SETTLE_BLOCKS);
}

// Stage 22 (CSM): timer A keys channel 3's four operators 53267 / (1024 - NA) times a second.
#define CSM_CLOCK 53267.04
static void ym08(uint8_t reg, uint8_t value)
{
	const uint8_t sysex[10] = {0xF0, 0x00, 0x22, 0x77, 0x08, (uint8_t)(reg >> 4), (uint8_t)(reg & 15), (uint8_t)(value >> 4), (uint8_t)(value & 15), 0xF7};
	raw(sysex, sizeof(sysex));
}

static int csm_na(int key)
{
	const long na = 1024 - lround(CSM_CLOCK / key_hz(key));
	return na < 0 ? 0 : na > 1023 ? 1023 : (int)na;
}

static double csm_hz(int na)
{
	return CSM_CLOCK / (1024 - na);
}

// The strongest partial near hz, searched 80 cents either side, then to a fiftieth of a cent.
static double fine_peak_hz(int n, double hz)
{
	double best = 0, best_level = -1;
	for (double c = -80; c <= 80; c += 1) {
		const double level = level_at(n, hz * pow(2.0, c / 1200));
		if (level > best_level) {
			best_level = level;
			best = c;
		}
	}
	const double around = best;
	for (double c = around - 1; c <= around + 1; c += 0.02) {
		const double level = level_at(n, hz * pow(2.0, c / 1200));
		if (level > best_level) {
			best_level = level;
			best = c;
		}
	}
	return hz * pow(2.0, best / 1200);
}

static double rms_of(int n)
{
	double sum = 0;
	for (int i = 0; i < n; i++) {
		sum += (double)ch3_wave[i] * ch3_wave[i];
	}
	return sqrt(sum / (n ? n : 1));
}

static void wait_frames(int frames)
{
	while (frames > 0) {
		const int step = frames < BLOCK ? frames : BLOCK;
		tick(step);
		frames -= step;
	}
}

// The NA timer A holds, from 0x24 and 0x25.
static int csm_na_reg(void)
{
	return mdmi_engine_test_ym(engine, 0, 0x24) << 2 | (mdmi_engine_test_ym(engine, 0, 0x25) & 3);
}

static uint8_t ch3_mode_reg(void)
{
	return mdmi_engine_test_ym(engine, 0, 0x27);
}

// Output 3 after a CSM note's release: RR 15 ends in a few ms, the DC blocker's tail of the bursts
// takes some 200 ms more to fall 60 dB.
static double csm_tail(void)
{
	wait_frames(RATE / 5);
	return rms_of(capture_output(2, RATE / 100));
}

static void csm_mode(int mode)
{
	set_global(GLOBAL_CH3_MODE, 1);
	set_global(GLOBAL_CH3_NOTES, mode);
	sync_and_settle();
}

// Stage 22 (D52), items 3-6: CSM through the plugin. Every operator at the note (Semi 0), only
// operator 4 heard, AR 31.
static void test_csm(void)
{
	printf("  CSM\n");
	static const int ONE[4] = {127, 127, 127, 10};
	sine_voice(2, ONE, 8, 0);
	csm_mode(PATCH_CH3_CSM);
	checkf(ch3_mode_reg() == 0x40, "CSM without a note: 0x27 = 0x%02X, mode 01 and timer A stopped", ch3_mode_reg());

	// Item 3: the fundamental of output 3 against the note; NA as the formula has it.
	double worst = 0, clock = 0;
	bool na_right = true, alone = true, keys_kept = true;
	const uint8_t keys = key_reg();
	for (int key = 45; key <= 96; key++) {
		if (key > 71 && key != 84 && key != 96) {
			continue;
		}
		reset_energy();
		midi(0x92, (uint8_t)key, 127);
		render(8);
		const int na = csm_na_reg();
		na_right = na_right && na == csm_na(key) && ch3_mode_reg() == 0x81;
		const int n = capture_output(2, CH3_CAPTURE);
		const double got = fine_peak_hz(n, csm_hz(na));
		const double cents = 1200 * log2(got / key_hz(key));
		alone = alone && loud_outputs() == 1 << 2;
		if (key <= 71) {
			worst = fmax(worst, fabs(cents));
		} else {
			printf("    note %d: NA %d, %.2f Hz, %+.1f cents from the note\n", key, na, got, cents);
		}
		if (key == 69) {
			clock = got * (1024 - na);
		}
		midi(0x82, (uint8_t)key, 0);
		render(4);
		na_right = na_right && ch3_mode_reg() == 0x40;
		keys_kept = keys_kept && key_reg() == keys;
	}
	printf("    notes 45-71: %.2f cents at most from the note; timer A's clock from A4: %.1f Hz\n", worst, clock);
	checkf(na_right, "each note writes NA = 1024 - round(53267 / f), 0x27 = 0x81 under it and 0x40 after it");
	checkf(worst <= 8, "notes 45-71 (110-494 Hz) sound within 8 cents of the note (%.2f)", worst);
	checkf(fabs(clock / CSM_CLOCK - 1) <= 0.001, "timer A runs at 53267 Hz within 0.1%% (%.1f)", clock);
	checkf(alone && keys_kept, "CSM sounds on output 3 alone and writes no 0x28");

	// Item 4: 200 Note Offs at random moments on note 96, RR 15.
	sine_voice(2, ONE, 15, 0);
	midi(0x92, 96, 127);
	render(8);
	const double sounding = rms_of(capture_output(2, RATE / 20));
	midi(0x82, 96, 0);
	double loudest = 0;
	int hung = 0;
	uint32_t seed = 0x2222u;
	for (int trial = 0; trial < 200; trial++) {
		midi(0x92, 96, 127);
		seed = seed * 1664525u + 1013904223u;
		wait_frames(RATE / 100 + (int)(seed >> 8) % (RATE / 50));
		midi(0x82, 96, 0);
		const double after = csm_tail();
		loudest = fmax(loudest, after);
		hung += after > sounding * 1e-3;
	}
	printf("    200 Note Offs on note 96: %d left output 3 above -60 dB, the loudest %.1f dB under the note\n", hung, db(sounding) - db(loudest));
	checkf(hung == 0, "after each of 200 Note Offs at random moments output 3 falls 60 dB below the note");

	// Item 5: CC 80 and the DAC under a sounding CSM note; 0x27 from the shadow after the driver's
	// load, raw bits 1-3 kept.
	midi(0x92, 69, 127);
	render(8);
	const double a4 = rms_of(capture_output(2, RATE / 20));
	set_global(GLOBAL_CH3_MODE, 0);
	sync_and_settle();
	const double after_cc80 = csm_tail();
	const uint8_t mode_off = ch3_mode_reg();
	csm_mode(PATCH_CH3_CSM);
	midi(0x82, 69, 0);
	midi(0x92, 69, 127);
	render(8);
	const double again = rms_of(capture_output(2, RATE / 20));
	ym08(0x27, 0x0A);
	render(4);
	const uint8_t raw = ch3_mode_reg();
	set_global(GLOBAL_DAC, 1);
	sync_and_settle();
	render(DAC_BLOCKS);
	const double after_dac_on = csm_tail();
	const uint8_t mode_dac_on = ch3_mode_reg();
	midi(0x82, 69, 0);
	midi(0x92, 69, 127);
	render(8);
	const double with_dac = rms_of(capture_output(2, RATE / 20));
	const uint8_t mode_note_dac = ch3_mode_reg();
	set_global(GLOBAL_DAC, 0);
	sync_and_settle();
	render(DAC_BLOCKS);
	const double after_dac_off = csm_tail();
	const uint8_t mode_dac_off = ch3_mode_reg();
	midi(0x82, 69, 0);
	midi(0x92, 69, 127);
	render(8);
	const double after_all = rms_of(capture_output(2, RATE / 20));
	midi(0x82, 69, 0);
	ym08(0x27, 0x00);
	render(4);
	const uint8_t cleared = ch3_mode_reg();
	printf("    under A4: CC 80 off %.1f dB, DAC on %.1f dB, DAC off %.1f dB below it; the next notes %+.1f, %+.1f, %+.1f dB; 0x27 %02X raw %02X DAC %02X %02X %02X\n",
		db(a4) - db(after_cc80), db(a4) - db(after_dac_on), db(a4) - db(after_dac_off), db(again) - db(a4), db(with_dac) - db(a4), db(after_all) - db(a4), mode_off, raw, mode_dac_on,
		mode_note_dac, mode_dac_off);
	const double quiet_enough = a4 * 1e-3;
	checkf(after_cc80 < quiet_enough && after_dac_on < quiet_enough && after_dac_off < quiet_enough, "CC 80 off, DAC on and DAC off silence a sounding CSM note");
	checkf(again > a4 * 0.5 && with_dac > a4 * 0.5 && after_all > a4 * 0.5, "and the next CSM note sounds each time, within 6 dB");
	checkf(mode_off == 0x00 && raw == 0x8B && mode_dac_on == 0x4A && mode_note_dac == 0x8B && mode_dac_off == 0x4A && cleared == 0x40,
		"0x27: 00 with Ch3 Special off; a raw 0x0A under a note gives 8B and keeps bits 1-3 through both driver loads (4A); a raw 00 clears them");

	// Item 6, the arp row: in CSM it moves the repeats' pitch, C4 D#4 E4 G4 C5.
	arp_on(2, true);
	midi(0x92, 60, 127);
	bool seen[ARP_TEST_STEPS] = {false};
	for (int i = 0; i < 300; i++) {
		tick(64);
		const int na = csm_na_reg();
		for (int s = 0; s < ARP_TEST_STEPS; s++) {
			seen[s] = seen[s] || na == csm_na(60 + ARP_TEST[s]);
		}
	}
	midi(0x82, 60, 0);
	arp_on(2, false);
	bool all = true;
	for (int s = 0; s < ARP_TEST_STEPS; s++) {
		all = all && seen[s];
	}
	checkf(all, "an arp row 0 +3 +4 +7 +12 on C4 steps NA through the five notes");

	set_global(GLOBAL_CH3_NOTES, PATCH_CH3_OPS);
	set_global(GLOBAL_CH3_MODE, 0);
	sync_and_settle();
	midi(0xC2, 0, 0);
	render(SETTLE_BLOCKS);
}

// The formants' pitch registers of channel 3: A2/A6 for operator 4, A8-AA and AC-AE for 1-3.
static uint64_t ch3_formant_regs(void)
{
	static const uint8_t REGS[8] = {0xA2, 0xA6, 0xA8, 0xA9, 0xAA, 0xAC, 0xAD, 0xAE};
	uint64_t all = 0;
	for (int i = 0; i < 8; i++) {
		all = all << 8 | mdmi_engine_test_ym(engine, 0, REGS[i]);
	}
	return all;
}

// Operator 4's pitch as F-num times 2^block: grows with the frequency across octaves.
static double ch3_op4_rate(void)
{
	const uint16_t pitch = pitch_of(2);
	return (double)(pitch & 0x7FF) * (1 << (pitch >> 11 & 7));
}

// Stage 22a, item 3: Porta On, Porta Time 64, note 45 held, note 57 over it, 45 released halfway. In
// CSM with Fixed on four formants NA climbs frame by frame from note 45's to note 57's; in ONE NOTE
// without Fixed (the control) operator 4's F-num does the same. No key changes in either.
static void test_csm_portamento(void)
{
	printf("  portamento in CSM\n");
	static const int ONE[4] = {127, 127, 127, 10};
	sine_voice(2, ONE, 8, 0);
	patch_set(patch, PATCH_CHANNEL_ID(2, PARAM_PORTA_TIME), 64);
	set_on(2, PARAM_PORTA_ON, 1);
	for (int mode = 0; mode < 2; mode++) {
		const bool csm = mode == 0;
		for (int op = 0; op < 4; op++) {
			set_global(GLOBAL_CH3_OP(op, PATCH_CH3_FIXED), csm);
		}
		csm_mode(csm ? PATCH_CH3_CSM : PATCH_CH3_ONE_NOTE);
		midi(0x92, 45, 127);
		render(8);
		const uint8_t keys = key_reg(), mode_reg = ch3_mode_reg();
		const uint64_t formants = ch3_formant_regs();
		const int na_start = csm_na_reg();
		const double rate_start = ch3_op4_rate();
		midi(0x92, 57, 127);
		// 26 frames of 1/60 s glide; read every 64 samples for 700 ms.
		int steps = 0, na_last = na_start, na_mid = -1;
		double rate_last = rate_start, mid_hz = 0;
		bool monotone = true, keys_kept = true, formants_kept = true;
		for (int t = 0; t < RATE * 7 / 10 / 64; t++) {
			if (t == RATE / 5 / 64) {
				const int n = capture_output(2, RATE / 50);
				na_mid = csm_na_reg();
				mid_hz = csm ? fine_peak_hz(n, csm_hz(na_mid)) : fine_peak_hz(n, key_hz(51));
				midi(0x82, 45, 0);
			}
			tick(64);
			keys_kept = keys_kept && key_reg() == keys && ch3_mode_reg() == mode_reg;
			if (csm) {
				const int na = csm_na_reg();
				// The ROM writes 0x24 (NA's high 8 bits) then 0x25 (its low 2): a read between them sees the
				// new high with the old low (docs/research/ch3-csm.md), so the climb is judged on 0x24.
				monotone = monotone && (na >> 2) >= (na_last >> 2);
				steps += na != na_last;
				na_last = na;
				formants_kept = formants_kept && ch3_formant_regs() == formants;
			} else {
				const double rate = ch3_op4_rate();
				monotone = monotone && rate >= rate_last;
				steps += rate != rate_last;
				rate_last = rate;
			}
		}
		const bool between = mid_hz > key_hz(45) * 1.03 && mid_hz < key_hz(57) / 1.03;
		if (csm) {
			printf("    CSM: NA %d to %d in %d steps (notes 45 and 57: %d, %d); at 200 ms NA %d, output 3 at %.1f Hz\n", na_start, na_last, steps, csm_na(45), csm_na(57), na_mid, mid_hz);
			checkf(na_start == csm_na(45) && na_last == csm_na(57) && monotone && steps >= 20,
				"CSM, Porta On: NA climbs frame by frame from note 45's %d to note 57's %d and stays (%d steps)", csm_na(45), csm_na(57), steps);
			checkf(keys_kept && formants_kept, "and during the glide 0x27 and 0x28 stay, the Fixed formants keep their pitch");
			checkf(between, "the fundamental of output 3 at 200 ms lies between the two notes (%.1f Hz)", mid_hz);
		} else {
			printf("    ONE NOTE: operator 4 from %.0f to %.0f (x%.3f) in %d steps; at 200 ms %.1f Hz\n", rate_start, rate_last, rate_last / rate_start, steps, mid_hz);
			checkf(fabs(rate_last / rate_start - 2) < 0.01 && monotone && steps >= 20 && keys_kept && between,
				"ONE NOTE, Porta On (the control): operator 4 glides an octave frame by frame, 0x28 stays, the middle lies between the notes");
		}
		midi(0x82, 57, 0);
		render(SETTLE_BLOCKS);
	}
	for (int op = 0; op < 4; op++) {
		set_global(GLOBAL_CH3_OP(op, PATCH_CH3_FIXED), 0);
	}
	set_on(2, PARAM_PORTA_ON, 0);
	set_global(GLOBAL_CH3_NOTES, PATCH_CH3_OPS);
	set_global(GLOBAL_CH3_MODE, 0);
	sync_and_settle();
	midi(0xC2, 0, 0);
	render(SETTLE_BLOCKS);
}

// Stage 22b, item 3 (D57): output 3's RMS on 8 notes of a legato glide, each key pressed over the
// last and the last raised after it, then on 8 separate notes. Returns the quietest against the
// loudest.
#define CSM_ATTACK_NOTES 8
static double csm_attack_spread(double legato[CSM_ATTACK_NOTES], double separate[CSM_ATTACK_NOTES])
{
	static const int KEYS[CSM_ATTACK_NOTES] = {45, 48, 51, 54, 57, 60, 63, 66};
	double loudest = 0, quietest = 1e9;
	for (int i = 0; i < CSM_ATTACK_NOTES; i++) {
		midi(0x92, (uint8_t)KEYS[i], 127);
		if (i > 0) {
			midi(0x82, (uint8_t)KEYS[i - 1], 0);
		}
		// Porta Time 64 glides 3 semitones in about 110 ms.
		wait_frames(RATE * 3 / 10);
		legato[i] = rms_of(capture_output(2, RATE / 10));
	}
	midi(0x82, (uint8_t)KEYS[CSM_ATTACK_NOTES - 1], 0);
	csm_tail();
	for (int i = 0; i < CSM_ATTACK_NOTES; i++) {
		midi(0x92, (uint8_t)KEYS[i], 127);
		wait_frames(RATE / 20);
		separate[i] = rms_of(capture_output(2, RATE / 10));
		midi(0x82, (uint8_t)KEYS[i], 0);
		csm_tail();
	}
	for (int i = 0; i < CSM_ATTACK_NOTES; i++) {
		loudest = fmax(loudest, fmax(legato[i], separate[i]));
		quietest = fmin(quietest, fmin(legato[i], separate[i]));
	}
	return loudest > 0 ? quietest / loudest : 0;
}

// Channel 3's default voice (AR 26/31/24/27) in CSM with Porta On. Timer A keys the operators for one
// sample and only AR 31 attacks within it, so the ROM writes AR 31 to channel 3's four operators while
// it is in CSM and the voice's AR back after; the model keeps the voice's. In 0.22.2 an operator below
// AR 31 sounded or not by note and timer phase.
static void test_csm_attack(void)
{
	printf("  AR 31 in CSM\n");
	static const uint8_t AR_REGS[4] = {0x52, 0x5A, 0x56, 0x5E};
	midi(0xC2, 0, 0);
	render(SETTLE_BLOCKS);
	patch_set(patch, PATCH_CHANNEL_ID(2, PARAM_PORTA_TIME), 64);
	set_on(2, PARAM_PORTA_ON, 1);
	csm_mode(PATCH_CH3_CSM);
	bool ar31 = true, below31 = false;
	for (int op = 0; op < 4; op++) {
		const uint8_t reg = mdmi_engine_test_ym(engine, 0, AR_REGS[op]);
		const int ar = model_value(2, PARAM_OP_BASE + op * PATCH_OP_PARAMS + F_AR);
		const int rs = model_value(2, PARAM_OP_BASE + op * PATCH_OP_PARAMS + F_RS);
		ar31 = ar31 && reg == (rs << 6 | 31);
		below31 = below31 || ar < 31;
	}
	double legato[CSM_ATTACK_NOTES], separate[CSM_ATTACK_NOTES];
	const double spread = csm_attack_spread(legato, separate);
	printf("    output 3 RMS, legato:");
	for (int i = 0; i < CSM_ATTACK_NOTES; i++) {
		printf(" %.4f", legato[i]);
	}
	printf("; separate:");
	for (int i = 0; i < CSM_ATTACK_NOTES; i++) {
		printf(" %.4f", separate[i]);
	}
	printf("; the quietest %.2f of the loudest\n", spread);
	set_global(GLOBAL_CH3_NOTES, PATCH_CH3_OPS);
	sync_and_settle();
	bool restored = true;
	for (int op = 0; op < 4; op++) {
		const int ar = model_value(2, PARAM_OP_BASE + op * PATCH_OP_PARAMS + F_AR);
		const int rs = model_value(2, PARAM_OP_BASE + op * PATCH_OP_PARAMS + F_RS);
		restored = restored && mdmi_engine_test_ym(engine, 0, AR_REGS[op]) == (rs << 6 | ar);
	}
	checkf(below31 && ar31, "CSM on the default voice (an AR below 31): channel 3's four AR registers hold 31 with the voice's RS");
	checkf(spread >= 0.8, "with Porta On every note of an 8-note legato glide and 8 separate notes sounds at 0.8 of the loudest or more (%.2f)", spread);
	checkf(restored, "leaving CSM writes the voice's AR back");
	set_on(2, PARAM_PORTA_ON, 0);
	set_global(GLOBAL_CH3_MODE, 0);
	sync_and_settle();
	midi(0xC2, 0, 0);
	render(SETTLE_BLOCKS);
}

// Stage 24: the exported ROM plays the project by program changes and notes alone. Run A plays a
// bank through the plugin as ever; run B boots the file EXPORT wrote and gets no SysEx at all: a
// program change on each channel to its place, and the same notes. What the chip does must be the
// same: an FM voice's registers and pitch steps, a PSG channel's volume and tone from frame to frame,
// the DAC's sample.
#define EXPORT_SEEN 48
#define EXPORT_REGS 30
typedef struct {
	uint8_t regs[EXPORT_REGS];
	uint16_t pitches[EXPORT_SEEN];
	int pitch_count;
	// PSG: volume << 12 | tone, as it changes.
	uint32_t psg[EXPORT_SEEN];
	int psg_count;
} export_heard;

// FM 1-3, PSG 7-8 on these places; place 7's row is switched off.
static const int EXPORT_CHANNELS[5] = {0, 1, 2, FM_CHANNELS, FM_CHANNELS + 1};
static const int EXPORT_PLACES[5] = {5, 120, 7, 3, 121};
#define EXPORT_SAMPLE_NOTE 100

static bool export_raw;

static void export_midi(uint8_t a, uint8_t b, uint8_t c)
{
	if (export_raw) {
		const uint8_t message[3] = {a, b, c};
		mdmi_engine_midi(engine, 0, message, (a & 0xF0) == 0xC0 ? 2 : 3);
	} else {
		midi(a, b, c);
	}
}

static void export_listen(int channel, export_heard *heard)
{
	memset(heard, 0, sizeof(*heard));
	export_midi((uint8_t)(0x90 | channel), 60, 127);
	const bool psg = channel >= FM_CHANNELS;
	// A value counts once it held a quarter of a frame, as in poll_pitches: a tone is written in two
	// halves, which can show for a moment.
	uint32_t last = UINT32_MAX, held = UINT32_MAX;
	int since = 0;
	for (int t = 0; t < 50 * RATE / 60 / ARP_POLL; t++) {
		if (t == 30 * RATE / 60 / ARP_POLL) {
			export_midi((uint8_t)(0x80 | channel), 60, 0);
		}
		tick(ARP_POLL);
		uint32_t now;
		if (psg) {
			const uint8_t c = (uint8_t)(channel - FM_CHANNELS);
			now = (uint32_t)mdmi_engine_test_psg_volume(engine, c) << 12 | mdmi_engine_test_psg_tone(engine, c);
		} else {
			now = pitch_of(channel);
		}
		if (now != held) {
			held = now;
			since = t;
		}
		// From the third frame on: before it a register may still hold the last note's.
		if (t >= 3 * RATE / 60 / ARP_POLL && (t - since) * ARP_POLL >= ARP_HOLD && held != last && heard->psg_count < EXPORT_SEEN
			&& heard->pitch_count < EXPORT_SEEN) {
			if (psg) {
				heard->psg[heard->psg_count++] = held;
			} else {
				heard->pitches[heard->pitch_count++] = (uint16_t)held;
			}
			last = held;
		}
		if (!psg && t == 10 * RATE / 60 / ARP_POLL) {
			int n = 0;
			for (int base = 0x30; base <= 0x90; base += 0x10) {
				for (int slot = 0; slot < 4; slot++) {
					heard->regs[n++] = read_reg(channel, base, slot);
				}
			}
			heard->regs[n++] = read_reg(channel, 0xB0, 0);
			heard->regs[n++] = read_reg(channel, 0xB4, 0);
		}
	}
	render(SETTLE_BLOCKS);
}

// The DAC's notes of the test: the project's ramp where the ROM has none, a ROM sample made the
// project's, a ROM note removed, a loop with its release, a loop that stops at Note Off.
#define EXPORT_DAC_NOTES 5
static const int EXPORT_DAC[EXPORT_DAC_NOTES] = {EXPORT_SAMPLE_NOTE, 40, 41, 50, 52};
static const char *const EXPORT_DAC_WHAT[EXPORT_DAC_NOTES] = {"the project's sample on a note the ROM has none for",
	"a ROM sample made the project's", "a ROM note removed", "a loop with its release", "a loop that stops at Note Off"};

// Output 6's energy (index 5: the outputs here are channels 1-10) while each DAC note is held 300 ms, and in the 200 ms after its Note Off.
static void export_play(export_heard *heard, double (*dac)[2])
{
	for (int i = 0; i < 5; i++) {
		export_listen(EXPORT_CHANNELS[i], &heard[i]);
	}
	if (export_raw) {
		export_midi(0xB0, 78, 127);
	} else {
		set_global(GLOBAL_DAC, 1);
		sync_and_settle();
	}
	render(DAC_BLOCKS);
	for (int n = 0; n < EXPORT_DAC_NOTES; n++) {
		reset_energy();
		export_midi(0x95, (uint8_t)EXPORT_DAC[n], 127);
		render(RATE * 3 / 10 / BLOCK);
		dac[n][0] = out_energy[5];
		export_midi(0x85, (uint8_t)EXPORT_DAC[n], 0);
		reset_energy();
		render(RATE * 2 / 10 / BLOCK);
		dac[n][1] = out_energy[5];
		render(SETTLE_BLOCKS);
	}
	if (export_raw) {
		export_midi(0xB0, 78, 0);
	} else {
		set_global(GLOBAL_DAC, 0);
		sync_and_settle();
	}
	render(DAC_BLOCKS);
}

// The same within 15%, or both silent against the loudest held note.
static bool export_same_energy(double a, double b, double loudest)
{
	const double quiet = loudest * 1e-3;
	return (a < quiet && b < quiet) || fabs(a - b) <= 0.15 * fmax(a, b);
}

static bool export_same_dac(const double (*a)[2], const double (*b)[2], const char *run)
{
	double loudest = 0;
	for (int n = 0; n < EXPORT_DAC_NOTES; n++) {
		loudest = fmax(loudest, fmax(a[n][0], b[n][0]));
	}
	bool same = loudest > 0;
	for (int n = 0; n < EXPORT_DAC_NOTES; n++) {
		const bool ok = export_same_energy(a[n][0], b[n][0], loudest) && export_same_energy(a[n][1], b[n][1], loudest);
		printf("    %s, note %d, %s: held %.3g and %.3g, after Note Off %.3g and %.3g%s\n", run, EXPORT_DAC[n], EXPORT_DAC_WHAT[n], a[n][0], b[n][0], a[n][1], b[n][1], ok ? "" : ": DIFFER");
		same = same && ok;
	}
	return same;
}

// The values a source took, each once, in order of value.
static int export_values(const uint32_t *values, int count, uint32_t *out)
{
	int n = 0;
	for (int i = 0; i < count; i++) {
		int at = 0;
		while (at < n && out[at] < values[i]) {
			at++;
		}
		if (at == n || out[at] != values[i]) {
			memmove(out + at + 1, out + at, (size_t)(n - at) * sizeof(*out));
			out[at] = values[i];
			n++;
		}
	}
	return n;
}

// The same registers, and the same values in about as many changes: the two runs' notes meet the
// ROM's frame at another phase, so a loop may play a step more or less before its release.
static bool export_same(const export_heard *a, const export_heard *b, const char *what)
{
	uint32_t pa[EXPORT_SEEN], pb[EXPORT_SEEN], va[EXPORT_SEEN], vb[EXPORT_SEEN], sa[2 * EXPORT_SEEN], sb[2 * EXPORT_SEEN];
	for (int i = 0; i < EXPORT_SEEN; i++) {
		pa[i] = a->pitches[i];
		pb[i] = b->pitches[i];
	}
	const int na = export_values(pa, a->pitch_count, va), nb = export_values(pb, b->pitch_count, vb);
	// A PSG channel's volumes and tones each, not their pairs: the row and the envelope step on
	// their own, so which pairs meet hangs on the phase too.
	uint32_t qa[2 * EXPORT_SEEN], qb[2 * EXPORT_SEEN];
	for (int i = 0; i < EXPORT_SEEN; i++) {
		qa[i] = a->psg[i] >> 12 | 0x10000;
		qa[EXPORT_SEEN + i] = a->psg[i] & 0xFFF;
		qb[i] = b->psg[i] >> 12 | 0x10000;
		qb[EXPORT_SEEN + i] = b->psg[i] & 0xFFF;
	}
	memmove(qa + a->psg_count, qa + EXPORT_SEEN, (size_t)a->psg_count * sizeof(*qa));
	memmove(qb + b->psg_count, qb + EXPORT_SEEN, (size_t)b->psg_count * sizeof(*qb));
	const int ma = export_values(qa, 2 * a->psg_count, sa), mb = export_values(qb, 2 * b->psg_count, sb);
	int reg = -1;
	for (int i = 0; i < EXPORT_REGS && reg < 0; i++) {
		reg = a->regs[i] != b->regs[i] ? i : -1;
	}
	const bool same = reg < 0 && na == nb && !memcmp(va, vb, (size_t)na * sizeof(*va)) && ma == mb && !memcmp(sa, sb, (size_t)ma * sizeof(*sa))
		&& abs(a->pitch_count - b->pitch_count) <= 3 && abs(a->psg_count - b->psg_count) <= 8
		&& (a->psg_count < 4 || (b->psg_count >= 4 && !memcmp(a->psg + a->psg_count - 4, b->psg + b->psg_count - 4, 4 * sizeof(*a->psg))));
	if (!same) {
		printf("    %s: pitches %d and %d (%d and %d values), PSG changes %d and %d (%d and %d values)", what, a->pitch_count, b->pitch_count, na, nb,
			a->psg_count, b->psg_count, ma, mb);
		if (reg >= 0) {
			printf(", register %d: %02X and %02X", reg, a->regs[reg], b->regs[reg]);
		}
		printf("\n      ");
		for (int i = 0; i < a->psg_count; i++) {
			printf("%05X ", a->psg[i]);
		}
		printf("\n      ");
		for (int i = 0; i < b->psg_count; i++) {
			printf("%05X ", b->psg[i]);
		}
		printf("\n");
	}
	return same;
}

static void take_exported(void *context, bool knobs)
{
	(void)context;
	(void)knobs;
}

static void test_export(void)
{
	patch_instrument guitar, duck;
	if (!load_file("Thunder Guitar 03.tyi", &guitar) || !load_file("Duck Guitar.tfi", &duck)) {
		return;
	}
	// The bank: a file with an octave row, a voice on a service number of the plugin's, a GM voice
	// whose row is switched off; an envelope with a loop and its release, and a steady one on 121.
	patch_arp octave = PATCH_ARP_EMPTY, fifth = PATCH_ARP_EMPTY, off = PATCH_ARP_EMPTY;
	octave.count = 2, octave.loop = 0, octave.steps[1] = 12;
	fifth.count = 2, fifth.loop = 0, fifth.steps[1] = 7;
	off.count = 2, off.loop = 0, off.steps[1] = 5, off.off = 1;
	patch_instrument gm7 = patch_bank_view(patch)->places[7];
	patch_load_place(patch, 5, &guitar, &octave);
	patch_load_place(patch, 120, &duck, NULL);
	patch_load_place(patch, 7, &gm7, &off);
	static const uint8_t RELEASE[] = {0x00, 0x01, PATCH_EEF_LOOP_START, 0x02, 0x03, PATCH_EEF_LOOP_END, 0x05, 0x08, 0x0B, 0x0F, PATCH_EEF_END};
	patch_envelope released, still;
	patch_envelope_from_eef(&released, RELEASE, sizeof(RELEASE));
	snprintf(released.name, sizeof(released.name), "Release");
	steady(&still, 4, 3);
	patch_load_envelope_place(patch, 3, &released, &fifth);
	patch_load_envelope_place(patch, 121, &still, NULL);
	patch_take_incoming(patch);
	// The project's sample on a note the ROM has none for.
	size_t rom_size, image_size;
	const uint8_t *rom = mdmi_engine_builtin_rom(&rom_size);
	static state_view view;
	static dac_set set;
	memset(&set, 0, sizeof(set));
	dac_put(&set, EXPORT_SAMPLE_NOTE, ramp_sample(1, 256 * 64));
	// A ROM sample made the project's (D39), a ROM note removed, a loop with its release and one that
	// stops at Note Off: the stop leaves no release in the image (D54).
	rom_sample of_rom;
	if (rom_image_sample(rom, rom_size, 40, &of_rom)) {
		dac_put(&set, 40, dac_from_rom(&of_rom, "rom 40"));
	}
	dac_remove(&set, 41);
	dac_put(&set, 50, marked_sample(1, 1, 2, 4));
	dac_put(&set, 52, marked_sample(1, 1, 2, 0));
	view.dac = &set;
	uint8_t *image = dac_image(&set, rom, rom_size, &image_size);
	boot_image(image, image_size);
	free(image);
	// Every channel plays a place as it is: tweaks of the tests before go with a step of the Preset.
	// Pan isn't the bank's, the exported ROM starts centred; the tests after get theirs back.
	int pans[FM_CHANNELS];
	for (int channel = 0; channel < FM_CHANNELS; channel++) {
		pans[channel] = model_value(channel, PARAM_PAN);
	}
	for (int channel = 0; channel < PATCH_NUM_CHANNELS; channel++) {
		set_on(channel, PARAM_PRESET, 1);
		set_on(channel, PARAM_PRESET, 0);
		if (channel < FM_CHANNELS) {
			set_on(channel, PARAM_PAN, 1);
		}
	}
	for (int i = 0; i < 5; i++) {
		set_on(EXPORT_CHANNELS[i], PARAM_PRESET, EXPORT_PLACES[i]);
	}
	full_velocity();

	static export_heard a[5], b[5], c[5];
	static double a_dac[EXPORT_DAC_NOTES][2], b_dac[EXPORT_DAC_NOTES][2], c_dac[EXPORT_DAC_NOTES][2];
	export_raw = false;
	export_play(a, a_dac);

	// The window's view, which EXPORT reads, is the model's snapshot.
	patch_publish(patch);
	const files_host host = {patch, &view, take_exported, NULL};
	static mdmi_files files;
	files_init(&files, &host);
	char path[MAX_PATH], message[FILES_MESSAGE];
	GetTempPathA(sizeof(path), path);
	strcat(path, "mdmi-reg-export.bin");
	const bool written = files_export(&files, path, message, sizeof(message));
	printf("    %s\n", message);
	check(written && !strstr(message, "not in ROM"), "EXPORT writes the file; every channel plays a place of the bank as it is");
	FILE *file = fopen(path, "rb");
	uint8_t *exported = malloc(ROM_IMAGE_MAX + (512u << 10));
	size_t exported_size = file && exported ? fread(exported, 1, ROM_IMAGE_MAX + (512u << 10), file) : 0;
	if (file) {
		fclose(file);
	}
	check(exported_size && exported_size % (512u << 10) == 0, "the file is padded to a whole 512 KB");

	// Run B: the file alone, as the console would boot it.
	export_raw = true;
	const mdmi_engine_stats before = mdmi_engine_get_stats(engine);
	mdmi_engine_set_rom(engine, exported, exported_size);
	while (mdmi_engine_get_stats(engine).boots == before.boots) {
		render(1);
	}
	render(SETTLE_BLOCKS);
	for (int i = 0; i < 5; i++) {
		export_midi((uint8_t)(0xC0 | EXPORT_CHANNELS[i]), (uint8_t)EXPORT_PLACES[i], 0);
	}
	render(SETTLE_BLOCKS);
	export_play(b, b_dac);
	static const char *const WHAT[5] = {"FM 1, a file with a row", "FM 2 on 120", "FM 3, a row switched off", "PSG 7, loop and release with a row", "PSG 8 on 121, a loop that stops at Note Off"};
	for (int i = 0; i < 5; i++) {
		checkf(export_same(&a[i], &b[i], WHAT[i]), "exported ROM, by program change and notes alone: %s sounds as through the plugin", WHAT[i]);
	}
	check(export_same_dac(a_dac, b_dac, "file"), "the DAC's output 6 from the exported ROM as through the plugin: the project's sample, a ROM sample made the project's, a removed note, a loop with its release, a loop that stops");

	// A reset keeps the bank (stage 24): GM Reset and FF, then the same program changes.
	static const uint8_t GM_RESET[] = {0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7}, SYSTEM_RESET = 0xFF;
	mdmi_engine_midi(engine, 0, GM_RESET, sizeof(GM_RESET));
	mdmi_engine_midi(engine, 0, &SYSTEM_RESET, 1);
	render(RESET_BLOCKS);
	for (int i = 0; i < 5; i++) {
		export_midi((uint8_t)(0xC0 | EXPORT_CHANNELS[i]), (uint8_t)EXPORT_PLACES[i], 0);
	}
	render(SETTLE_BLOCKS);
	export_play(c, c_dac);
	bool kept = export_same_dac(a_dac, c_dac, "after the resets");
	for (int i = 0; i < 5; i++) {
		kept = export_same(&a[i], &c[i], WHAT[i]) && kept;
	}
	check(kept, "after GM Reset and FF a program change plays the bank's voice, envelope and row again");
	export_raw = false;

	free(exported);
	DeleteFileA(path);
	files_free(&files);
	patch_bank fresh;
	patch_bank_rom(&fresh);
	patch_load_bank(patch, &fresh);
	patch_take_incoming(patch);
	for (int i = 0; i < 5; i++) {
		patch_set(patch, PATCH_CHANNEL_ID(EXPORT_CHANNELS[i], PARAM_PRESET), 0);
	}
	dac_clear(&set);
	boot_image(rom, rom_size);
	for (int channel = 0; channel < FM_CHANNELS; channel++) {
		set_on(channel, PARAM_PAN, pans[channel]);
	}
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
	patch->owner = owner;
	{
		size_t size;
		const uint8_t *rom = mdmi_engine_builtin_rom(&size);
		patch_set_dac_image(patch, rom, size);
	}
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
	if (argc == 5 && !strcmp(argv[2], "--ch3-on-ref")) {
		return write_ch3_on_reference(argv[3], argv[4]);
	}
	if (argc == 5 && !strcmp(argv[2], "--ch3-off-ref")) {
		return write_ch3_off_reference(argv[3], argv[4]);
	}
	full_velocity();
	check_voices("a booted ROM holds the model's defaults", 0, FM_CHANNELS - 1, -1);
	check_globals("and its LFO and Ch3 mode");
	// First: its reference starts from a booted ROM after full_velocity.
	test_rows_off();
	test_every_preset();
	test_values();
	test_program_from_clip();
	test_dac();
	test_dac_psg();
	test_noise();
	test_restart();
	test_samples();
	test_shaping();
	test_live();
	test_loop_pan();
	test_loop_marks();
	test_four_voices();
	test_ch3_off();
	test_ch3_on();
	test_load();
	test_export();
	// After the export: its comparison is sensitive to the ROM frame's phase, which a test before it moves.
	test_dac_shown();
	// From here on no restart: an owner's wait is the ROM's reaction alone (architect: 40 ms at most).
	mdmi_engine_test_take_owner_wait(engine);
	test_voice_by_hand();
	test_reset();
	test_dealer();
	test_reset();
	test_files();
	test_bank_place();
	test_library_try();
	test_reset();
	test_envelopes();
	test_reset();
	test_drawing();
	test_reset();
	test_adjacent();
	test_reset();
	test_outputs();
	test_reset();
	test_psg_level();
	test_reset();
	test_arp();
	test_reset();
	test_ch3_keys();
	test_reset();
	test_lfo_reset();
	test_reset();
	test_csm();
	test_reset();
	test_csm_portamento();
	test_reset();
	test_csm_attack();
	test_reset();
	const float wait_no_restart = mdmi_engine_test_take_owner_wait(engine);
	mdmi_engine_stats stats = mdmi_engine_get_stats(engine);
	if (stats.dropped_midi) {
		printf("  FAIL: %u MIDI messages lost\n", stats.dropped_midi);
		failures++;
	}
	printf("    voices against the core's mix: %.2f steps of 16 bits at most; Main against the outputs' sum: %.2g\n", stats.voice_mix_error, sum_error);
	// BlastEm's lowpass truncates in its feedback, the voices run it in float (D46); a voice a sample
	// off would show hundreds of steps on an attack.
	check(stats.voice_mix_error <= 16.0f, "over every test the chip's voices add up to the core's mix within 16 steps of 16 bits after the DC blocker");
	check(sum_error < 1e-5f, "Main is the sum of the ten outputs");
	printf("    an owner waited for its key-on %.1f ms at most, %.1f ms in the tests without a restart\n", stats.owner_wait_ms, wait_no_restart);
	check(wait_no_restart <= 40.0f, "without a restart every owner met its key-on within 40 ms (chords, Retrigger, revive, Porta)");
	check(stats.owner_misses == 0, "every owner found a key-on on its voice within a second");
	check(stats.voice_batch_misses == 0, "every batch of the mix came with its voices");
	printf("%s: %d failed\n", failures ? "FAILED" : "PASSED", failures);
	return failures;
}
