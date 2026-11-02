// Experiment E1 (docs/ROADMAP.md): does a whole FM voice sent as SysEx 0A plus a Program Change
// end up in the YM2612 registers, on every preset and channel? Reads the registers of the running
// chip, which no other test does.
//
//   e1_test
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include "engine.h"
#include "presets_gm.h"

#define RATE 48000
#define BLOCK 256
#define SETTLE_BLOCKS 24
#define VOICE_BYTES 50
#define SYSEX_BYTES (VOICE_BYTES + 6)
#define FIRST_SERVICE_PROGRAM 116
#define FM_CHANNELS 6
#define LATE_LIMIT_MS 25.0

// The fields of one operator in a row of PRESETS_GM, the same order SysEx 0A takes.
enum { F_MUL, F_DT, F_AR, F_RS, F_DR, F_AM, F_SL, F_SR, F_RR, F_TL, F_SSG, F_COUNT };
#define ROW_HEAD 4

static mdmi_engine *engine;
static int failures;
static bool service_alternate[FM_CHANNELS];
static float left[BLOCK], right[BLOCK];

static void render(int blocks)
{
	for (int i = 0; i < blocks; i++) {
		mdmi_engine_render(engine, left, right, BLOCK);
	}
}

static void send_midi(const uint8_t *bytes, size_t length)
{
	mdmi_engine_midi(engine, 0, bytes, length);
}

static void send_cc(int channel, int cc, int value)
{
	const uint8_t message[3] = {(uint8_t)(0xB0 | channel), (uint8_t)cc, (uint8_t)value};
	send_midi(message, sizeof(message));
}

static void send_program(int channel, int program)
{
	const uint8_t message[2] = {(uint8_t)(0xC0 | channel), (uint8_t)program};
	send_midi(message, sizeof(message));
}

static void send_note(int channel, int key, bool on)
{
	const uint8_t message[3] = {(uint8_t)((on ? 0x90 : 0x80) | channel), (uint8_t)key, on ? 127 : 0};
	send_midi(message, sizeof(message));
}

// Stores the voice under a service number and selects it. The ROM skips a program it thinks is
// loaded, so each channel alternates between two numbers of its own.
static void send_voice(int midi_channel, int channel, const uint8_t *row)
{
	int program = FIRST_SERVICE_PROGRAM + channel * 2 + service_alternate[channel];
	service_alternate[channel] = !service_alternate[channel];
	uint8_t sysex[SYSEX_BYTES] = {0xF0, 0x00, 0x22, 0x77, 0x0A, 0x00, (uint8_t)program};
	memcpy(sysex + 7, row, VOICE_BYTES - 2);
	sysex[SYSEX_BYTES - 1] = 0xF7;
	send_midi(sysex, sizeof(sysex));
	send_program(midi_channel, program);
}

static uint8_t read_reg(int channel, int base, int slot)
{
	return mdmi_engine_test_ym(engine, channel >= 3, (uint8_t)(base + channel % 3 + slot * 4));
}

// How many registers of the channel differ from the row. Registers hold operators in the order 1, 3, 2, 4.
static int compare_voice(int channel, const uint8_t *row, bool report, const char *what)
{
	int wrong = 0;
#define EXPECT(reg_value, expected, name, op) \
	do { \
		int got_ = (reg_value), want_ = (expected); \
		if (got_ != want_) { \
			if (report && wrong < 4) { \
				printf("    %s ch%d op%d %s: register %d, expected %d\n", what, channel + 1, op + 1, name, got_, want_); \
			} \
			wrong++; \
		} \
	} while (0)
	for (int op = 0; op < 4; op++) {
		int slot = op == 1 ? 2 : op == 2 ? 1 : op;
		const uint8_t *f = row + ROW_HEAD + op * F_COUNT;
		EXPECT(read_reg(channel, 0x30, slot), f[F_DT] << 4 | f[F_MUL], "DT/MUL", op);
		EXPECT(read_reg(channel, 0x40, slot), f[F_TL], "TL", op);
		EXPECT(read_reg(channel, 0x50, slot), f[F_RS] << 6 | f[F_AR], "RS/AR", op);
		EXPECT(read_reg(channel, 0x60, slot), f[F_AM] << 7 | f[F_DR], "AM/DR", op);
		EXPECT(read_reg(channel, 0x70, slot), f[F_SR], "SR", op);
		EXPECT(read_reg(channel, 0x80, slot), f[F_SL] << 4 | f[F_RR], "SL/RR", op);
		EXPECT(read_reg(channel, 0x90, slot), f[F_SSG], "SSG", op);
	}
	EXPECT(read_reg(channel, 0xB0, 0), row[1] << 3 | row[0], "FB/ALG", 0);
	EXPECT(read_reg(channel, 0xB4, 0) & 0x3F, row[2] << 4 | row[3], "AMS/FMS", 0);
#undef EXPECT
	return wrong;
}

static void result(bool passed, const char *format, int a, int b)
{
	printf("  %s: ", passed ? "ok  " : "FAIL");
	printf(format, a, b);
	printf("\n");
	if (!passed) {
		failures++;
	}
}

static void row_of(int program, uint8_t *row)
{
	memcpy(row, PRESETS_GM[program], VOICE_BYTES - 2);
}

// Plain Program Change first, while no stored voice shadows any number: this compares presets_gm.h
// with the table inside the ROM, so a later difference means 0A and not our table.
static void test_table_matches_rom(void)
{
	printf("Plain Program Change against presets_gm.h\n");
	int bad_presets = 0;
	for (int program = 127; program >= 0; program--) {
		send_program(0, program);
		render(SETTLE_BLOCKS);
		uint8_t row[VOICE_BYTES];
		row_of(program, row);
		char what[16];
		snprintf(what, sizeof(what), "pgm %d", program);
		if (compare_voice(0, row, bad_presets < 6, what)) {
			bad_presets++;
		}
	}
	result(bad_presets == 0, "%d of %d presets differ", bad_presets, 128);
}

static void test_every_preset_every_channel(void)
{
	printf("Voice through 0A on every preset and FM channel\n");
	int bad = 0;
	for (int program = 0; program < 128; program++) {
		uint8_t row[VOICE_BYTES];
		row_of(program, row);
		for (int channel = 0; channel < FM_CHANNELS; channel++) {
			send_voice(channel, channel, row);
		}
		render(SETTLE_BLOCKS);
		for (int channel = 0; channel < FM_CHANNELS; channel++) {
			char what[16];
			snprintf(what, sizeof(what), "pgm %d", program);
			if (compare_voice(channel, row, bad < 6, what)) {
				bad++;
			}
		}
	}
	result(bad == 0, "%d of %d voices differ", bad, 128 * FM_CHANNELS);
}

static void test_extremes(void)
{
	printf("Values a CC cannot reach\n");
	uint8_t row[VOICE_BYTES];
	row_of(0, row);
	row[0] = 7;
	row[1] = 7;
	row[2] = 3;
	row[3] = 7;
	for (int op = 0; op < 4; op++) {
		uint8_t *f = row + ROW_HEAD + op * F_COUNT;
		f[F_SR] = 31;
		f[F_TL] = 127;
		f[F_SSG] = 15;
		f[F_AR] = 31;
		f[F_DR] = 31;
		f[F_SL] = 15;
		f[F_RR] = 15;
		f[F_MUL] = 15;
		f[F_DT] = 7;
		f[F_RS] = 3;
		f[F_AM] = 1;
	}
	send_voice(1, 1, row);
	render(SETTLE_BLOCKS);
	result(compare_voice(1, row, true, "extreme") == 0, "SR 31 and every other maximum reach the chip%.0d%.0d", 0, 0);
}

static void test_reload_clears_tweak(void)
{
	printf("Same voice again after a tweak\n");
	uint8_t row[VOICE_BYTES];
	row_of(5, row);
	send_voice(0, 0, row);
	render(SETTLE_BLOCKS);
	send_cc(0, 16, row[ROW_HEAD + F_TL] > 60 ? 0 : 127);
	render(SETTLE_BLOCKS);
	bool tweaked = compare_voice(0, row, false, "") != 0;
	send_voice(0, 0, row);
	render(SETTLE_BLOCKS);
	result(tweaked, "the CC changed a register first%.0d%.0d", 0, 0);
	result(compare_voice(0, row, true, "reload") == 0, "the same voice restores every register%.0d%.0d", 0, 0);
}

static void test_pan_survives(void)
{
	printf("Pan\n");
	uint8_t row[VOICE_BYTES];
	row_of(40, row);
	send_cc(2, 10, 0);
	render(SETTLE_BLOCKS);
	int before = read_reg(2, 0xB4, 0) & 0xC0;
	send_voice(2, 2, row);
	render(SETTLE_BLOCKS);
	int after = read_reg(2, 0xB4, 0) & 0xC0;
	result(before != 0xC0 && before == after, "pan bits %d stay %d after the voice", before, after);
	send_cc(2, 10, 64);
	render(SETTLE_BLOCKS);
}

static void test_polyphony(void)
{
	printf("Polyphony: voice on channel 1 reaches the voices a chord uses\n");
	uint8_t row[VOICE_BYTES];
	row_of(61, row);
	send_cc(0, 84, 127);
	render(SETTLE_BLOCKS);
	send_voice(0, 0, row);
	render(SETTLE_BLOCKS);
	for (int key = 0; key < FM_CHANNELS; key++) {
		send_note(0, 60 + key, true);
	}
	render(SETTLE_BLOCKS);
	int bad = 0;
	for (int channel = 0; channel < FM_CHANNELS; channel++) {
		if (compare_voice(channel, row, bad < 3, "poly")) {
			bad++;
		}
	}
	for (int key = 0; key < FM_CHANNELS; key++) {
		send_note(0, 60 + key, false);
	}
	result(bad == 0, "%d of %d voices differ", bad, FM_CHANNELS);
	send_cc(0, 84, 0);
	render(SETTLE_BLOCKS);
}

static void test_after_dac(void)
{
	printf("After the DAC switch resets the chip\n");
	uint8_t row[VOICE_BYTES];
	row_of(20, row);
	for (int channel = 0; channel < FM_CHANNELS; channel++) {
		send_voice(channel, channel, row);
	}
	render(SETTLE_BLOCKS);
	send_cc(0, 78, 127);
	render(SETTLE_BLOCKS);
	send_cc(0, 78, 0);
	render(SETTLE_BLOCKS);
	bool lost = compare_voice(0, row, false, "") != 0;
	for (int channel = 0; channel < FM_CHANNELS; channel++) {
		send_voice(channel, channel, row);
	}
	render(SETTLE_BLOCKS);
	int bad = 0;
	for (int channel = 0; channel < FM_CHANNELS; channel++) {
		if (compare_voice(channel, row, bad < 3, "dac")) {
			bad++;
		}
	}
	printf("    the reset %s the registers\n", lost ? "cleared" : "did not change");
	result(bad == 0, "%d of %d voices differ after sending them again", bad, FM_CHANNELS);
}

enum { LOAD_NONE, LOAD_ONE_VOICE, LOAD_SIX_VOICES, LOAD_SIX_PROGRAMS };

static int frames_until_key_on(int channel, int load)
{
	const uint8_t key_on = (uint8_t)(0xF0 | (channel >= 3 ? channel + 1 : channel));
	uint8_t row[VOICE_BYTES];
	row_of(10, row);
	send_note(channel, 64, false);
	render(SETTLE_BLOCKS);
	static int plain_program;
	plain_program = (plain_program + 1) & 63;
	for (int ch = 0; ch < FM_CHANNELS; ch++) {
		if (load == LOAD_SIX_VOICES || (load == LOAD_ONE_VOICE && ch == channel)) {
			send_voice(ch, ch, row);
		} else if (load == LOAD_SIX_PROGRAMS) {
			send_program(ch, plain_program);
		}
	}
	send_note(channel, 64, true);
	for (int block = 0; block < 400; block++) {
		render(1);
		if (mdmi_engine_test_ym(engine, 0, 0x28) == key_on) {
			send_note(channel, 64, false);
			return block * BLOCK;
		}
	}
	send_note(channel, 64, false);
	return -1;
}

static void test_lateness(void)
{
	printf("A note in the same block as six voice changes (%d bytes each)\n", SYSEX_BYTES + 2);
	int bare = frames_until_key_on(5, LOAD_NONE);
	int one = frames_until_key_on(5, LOAD_ONE_VOICE);
	int plain = frames_until_key_on(5, LOAD_SIX_PROGRAMS);
	int loaded = frames_until_key_on(5, LOAD_SIX_VOICES);
	double late_ms = (loaded - bare) * 1000.0 / RATE;
	printf("    key-on later than a bare note: one voice %.1f ms, six plain programs %.1f ms, six voices %.1f ms\n",
		(one - bare) * 1000.0 / RATE, (plain - bare) * 1000.0 / RATE, late_ms);
	result(bare >= 0 && loaded >= 0 && late_ms <= LATE_LIMIT_MS, "later by no more than the limit of %d ms%.0d", (int)LATE_LIMIT_MS, 0);
}

// Blocks paced against the clock while a lane changes all six voices ten times a second.
static void test_stream_in_real_time(int block, int seconds, int load)
{
	static const char *const names[] = {"nothing", "", "", "six plain programs"};
	printf("Real time, block %d, %d s: %s\n", block, seconds, load == LOAD_SIX_VOICES ? "six voices" : names[load]);
	mdmi_engine_deactivate(engine);
	mdmi_engine_set_offline(engine, false);
	mdmi_engine_activate(engine, RATE, (uint32_t)block);
	static float long_left[4096], long_right[4096];
	LARGE_INTEGER freq, start, now;
	QueryPerformanceFrequency(&freq);
	mdmi_engine_stats first = mdmi_engine_get_stats(engine);
	QueryPerformanceCounter(&start);
	int frames_per_change = RATE / 10;
	int next_change = RATE;
	int program = 0;
	for (int position = 0; position < RATE * seconds; position += block) {
		if (position >= next_change) {
			next_change += frames_per_change;
			program = (program + 1) & 127;
			uint8_t row[VOICE_BYTES];
			row_of(program, row);
			for (int channel = 0; channel < FM_CHANNELS; channel++) {
				if (load == LOAD_SIX_VOICES) {
					send_voice(channel, channel, row);
				} else if (load == LOAD_SIX_PROGRAMS) {
					send_program(channel, program & 63);
				}
			}
		}
		mdmi_engine_render(engine, long_left, long_right, (uint32_t)block);
		double due = (double)(position + block) / RATE;
		for (;;) {
			QueryPerformanceCounter(&now);
			if ((double)(now.QuadPart - start.QuadPart) / (double)freq.QuadPart >= due) {
				break;
			}
			SwitchToThread();
		}
	}
	mdmi_engine_stats last = mdmi_engine_get_stats(engine);
	int underruns = (int)(last.underruns - first.underruns);
	printf("    %d underruns, worst emulator step ever %.2f ms, midi lost %u\n", underruns, last.worst_run_ms, last.dropped_midi);
	if (load == LOAD_SIX_VOICES) {
		result(underruns == 0, "%d underruns%.0d", underruns, 0);
	}
	mdmi_engine_deactivate(engine);
	mdmi_engine_set_offline(engine, true);
	mdmi_engine_activate(engine, RATE, BLOCK);
}

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
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
	// A channel keeps the velocity of its last note, which scales the TL of the carriers, and the
	// engine's warm-up leaves one below full. Registers only equal the voice at full velocity.
	for (int channel = 0; channel < FM_CHANNELS; channel++) {
		send_note(channel, 60, true);
		send_note(channel, 60, false);
	}
	render(SETTLE_BLOCKS);
	test_table_matches_rom();
	test_every_preset_every_channel();
	test_extremes();
	test_reload_clears_tweak();
	test_pan_survives();
	test_polyphony();
	test_after_dac();
	test_lateness();
	for (int block = 512; block >= 128; block /= 4) {
		test_stream_in_real_time(block, 8, LOAD_NONE);
		test_stream_in_real_time(block, 8, LOAD_SIX_PROGRAMS);
		test_stream_in_real_time(block, 8, LOAD_SIX_VOICES);
	}
	printf("%s: %d failed\n", failures ? "FAILED" : "PASSED", failures);
	return failures;
}
