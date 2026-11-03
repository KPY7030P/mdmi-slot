// Does the running YM2612 hold what the model says? Drives patch.c the way the plugin does and
// reads the chip's registers, which no test by ear can do. A channel keeps the velocity of its last
// note, and that scales the TL of the carriers, so notes here are at full velocity.
//
//   reg_test
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "engine.h"
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

static void test_load(void)
{
	static uint8_t state[PATCH_NUM_IDS * 6];
	patch_publish(patch);
	patch_save(patch, state);
	set_on(0, PARAM_PRESET, 99);
	set_on(1, PARAM_OP_BASE + F_TL, 12);
	set_on(5, PARAM_OP_BASE + F_SR, 30);
	patch_load(patch, state, sizeof(state));
	patch_take_incoming(patch);
	sync_and_settle();
	check_voices("a loaded state reaches the chip", 0, FM_CHANNELS - 1, -1);
}

static void test_reset(void)
{
	const uint8_t reset = 0xFF;
	patch_midi(patch, &reset, 1, emit, NULL);
	render(RESET_BLOCKS);
	play_chord(0, false);
	check_voices("the ROM's reset: every voice comes back", 0, FM_CHANNELS - 1, -1);
}

int main(void)
{
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
	if (!mdmi_engine_activate(engine, RATE, BLOCK)) {
		printf("FAIL: activate\n");
		return 1;
	}
	render(SETTLE_BLOCKS);
	// The engine's warm-up leaves a lower velocity behind.
	for (int channel = 0; channel < FM_CHANNELS; channel++) {
		midi((uint8_t)(0x90 | channel), 60, 127);
		midi((uint8_t)(0x80 | channel), 60, 0);
	}
	render(SETTLE_BLOCKS);
	check_voices("a booted ROM holds the model's defaults", 0, FM_CHANNELS - 1, -1);
	check_globals("and its LFO and Ch3 mode");
	test_every_preset();
	test_values();
	test_program_from_clip();
	test_dac();
	test_load();
	test_reset();
	mdmi_engine_stats stats = mdmi_engine_get_stats(engine);
	if (stats.dropped_midi) {
		printf("  FAIL: %u MIDI messages lost\n", stats.dropped_midi);
		failures++;
	}
	printf("%s: %d failed\n", failures ? "FAILED" : "PASSED", failures);
	return failures;
}
