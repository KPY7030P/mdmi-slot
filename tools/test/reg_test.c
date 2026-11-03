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

// A chord on the channel Polyphony gives the voices to, or channel 1.
static void play_chord(int notes, bool on)
{
	int owner = patch->model.globals[GLOBAL_POLYPHONY] - 1;
	uint8_t status = (uint8_t)((on ? 0x90 : 0x80) | (owner < 0 ? 0 : owner));
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
	int got_lfo = mdmi_engine_test_ym(engine, 0, 0x22), got_ch3 = mdmi_engine_test_ym(engine, 0, 0x27) & 0xC0;
	bool same = got_lfo == lfo && got_ch3 == ch3;
	printf("  %s: %s, LFO register %d model %d, Ch3 mode %d model %d\n", same ? "ok  " : "FAIL", what, got_lfo, lfo, got_ch3, ch3);
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

// Every register a voice has, to see that nothing touched it.
#define VOICE_REGS 28
static void dump_voice(int voice, uint8_t *regs)
{
	static const uint8_t BASES[7] = {0x30, 0x40, 0x50, 0x60, 0x70, 0x80, 0x90};
	int n = 0;
	for (int b = 0; b < 7; b++) {
		for (int slot = 0; slot < 4; slot++) {
			regs[n++] = read_reg(voice, BASES[b], slot);
		}
	}
}

static void dump_voices(uint8_t regs[FM_CHANNELS][VOICE_REGS + 4])
{
	for (int voice = 0; voice < FM_CHANNELS; voice++) {
		dump_voice(voice, regs[voice]);
		regs[voice][VOICE_REGS] = read_reg(voice, 0xA0, 0);
		regs[voice][VOICE_REGS + 1] = read_reg(voice, 0xA4, 0);
		regs[voice][VOICE_REGS + 2] = read_reg(voice, 0xB0, 0);
		regs[voice][VOICE_REGS + 3] = read_reg(voice, 0xB4, 0);
	}
}

static void check_untouched(const char *what, uint8_t before[FM_CHANNELS][VOICE_REGS + 4], int first_voice, int last_voice)
{
	static uint8_t after[FM_CHANNELS][VOICE_REGS + 4];
	dump_voices(after);
	int changed = 0;
	for (int voice = first_voice; voice <= last_voice; voice++) {
		changed += memcmp(before[voice], after[voice], VOICE_REGS + 4) != 0;
	}
	printf("  %s: %s, %d voices changed\n", changed ? "FAIL" : "ok  ", what, changed);
	if (changed) {
		failures++;
	}
}

// Polyphony gives one channel all six voices (D17): its values reach all of them, including sustain
// rates above what a CC reaches, and a note on another channel finds no voice to take.
static void test_polyphony_on(int owner)
{
	char what[96];
	set_on(owner, PARAM_PRESET, 61);
	set_on(owner, PARAM_OP_BASE + F_TL, 70);
	set_on(owner, PARAM_OP_BASE + F_SR, 27);
	set_on(owner, PARAM_PAN, 0);
	for (int channel = 0; channel < FM_CHANNELS; channel++) {
		if (channel != owner) {
			set_on(channel, PARAM_PRESET, 10 + channel);
			set_on(channel, PARAM_OP_BASE + F_AR, 11);
		}
	}
	set_global(GLOBAL_POLYPHONY, owner + 1);
	sync_and_settle();
	snprintf(what, sizeof(what), "polyphony on channel %d: all voices take its values", owner + 1);
	check_voices(what, 0, FM_CHANNELS - 1, owner);
	const uint8_t on = (uint8_t)(0x90 | owner), off = (uint8_t)(0x80 | owner);
	for (int i = 0; i < FM_CHANNELS; i++) {
		midi(on, (uint8_t)(60 + i), 127);
	}
	render(SETTLE_BLOCKS);
	check_voices("and keep them on the first chord", 0, FM_CHANNELS - 1, owner);
	static uint8_t before[FM_CHANNELS][VOICE_REGS + 4];
	dump_voices(before);
	const int other = owner == 1 ? 2 : 1;
	midi((uint8_t)(0x90 | other), 72, 127);
	render(SETTLE_BLOCKS);
	midi((uint8_t)(0x80 | other), 72, 0);
	render(SETTLE_BLOCKS);
	snprintf(what, sizeof(what), "a note on channel %d with all voices playing touches no register", other + 1);
	check_untouched(what, before, 0, FM_CHANNELS - 1);
	for (int i = 0; i < FM_CHANNELS; i++) {
		midi(off, (uint8_t)(60 + i), 0);
	}
	render(SETTLE_BLOCKS);
	dump_voices(before);
	midi((uint8_t)(0x90 | other), 72, 127);
	render(SETTLE_BLOCKS);
	midi((uint8_t)(0x80 | other), 72, 0);
	render(SETTLE_BLOCKS);
	check_untouched("nor with all voices free", before, 0, FM_CHANNELS - 1);
	set_on(owner, PARAM_OP_BASE + 1 * PATCH_OP_PARAMS + F_TL, 44);
	set_on(owner, PARAM_OP_BASE + 2 * PATCH_OP_PARAMS + F_SR, 19);
	play_chord(FM_CHANNELS, true);
	play_chord(FM_CHANNELS, false);
	check_voices("a tweak while on reaches all of them, SR above 15 too", 0, FM_CHANNELS - 1, owner);
	set_on(owner, PARAM_PRESET, 77);
	check_voices("a preset while on reaches all of them", 0, FM_CHANNELS - 1, owner);
	set_global(GLOBAL_POLYPHONY, 0);
	sync_and_settle();
	check_voices("polyphony off: every channel has its own voice back", 0, FM_CHANNELS - 1, -1);
	for (int channel = 0; channel < FM_CHANNELS; channel++) {
		midi((uint8_t)(0x90 | channel), 60, 127);
		render(4);
		midi((uint8_t)(0x80 | channel), 60, 0);
	}
	render(SETTLE_BLOCKS);
	check_voices("and keeps it on the first note", 0, FM_CHANNELS - 1, -1);
}

static void play_chord_on(int channel, bool on)
{
	for (int i = 0; i < FM_CHANNELS; i++) {
		midi((uint8_t)((on ? 0x90 : 0x80) | channel), (uint8_t)(60 + i), on ? 127 : 0);
	}
	render(SETTLE_BLOCKS);
}

static void test_polyphony(void)
{
	test_polyphony_on(0);
	test_polyphony_on(3);
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
	// With the DAC on, Polyphony leaves voice 6 alone: it plays samples for channel 6.
	static uint8_t before[FM_CHANNELS][VOICE_REGS + 4];
	dump_voices(before);
	set_on(3, PARAM_OP_BASE + F_SR, 26);
	set_global(GLOBAL_POLYPHONY, 4);
	sync_and_settle();
	check_voices("DAC and polyphony on channel 4: five voices take its values", 0, FM_CHANNELS - 2, 3);
	play_chord_on(3, true);
	play_chord_on(3, false);
	set_on(3, PARAM_OP_BASE + F_TL, 20);
	set_global(GLOBAL_POLYPHONY, 0);
	sync_and_settle();
	check_untouched("and voice 6 is never touched", before, FM_CHANNELS - 1, FM_CHANNELS - 1);
	check_voices("polyphony off with the DAC on: five voices back", 0, FM_CHANNELS - 2, -1);
	set_global(GLOBAL_DAC, 0);
	wait_for_voices(FM_CHANNELS - 1);
	check_voices("DAC off: all six come back", 0, FM_CHANNELS - 1, -1);
	check_globals("and the LFO and Ch3 mode");
	set_global(GLOBAL_CH3_MODE, 0);
	sync_and_settle();
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
	test_polyphony();
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
