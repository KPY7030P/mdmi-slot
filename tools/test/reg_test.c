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
		int had = loads ? patch_preset_value(channel, param, program) : from[param];
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
	test_voice_by_hand();
	test_reset();
	mdmi_engine_stats stats = mdmi_engine_get_stats(engine);
	if (stats.dropped_midi) {
		printf("  FAIL: %u MIDI messages lost\n", stats.dropped_midi);
		failures++;
	}
	printf("%s: %d failed\n", failures ? "FAILED" : "PASSED", failures);
	return failures;
}
