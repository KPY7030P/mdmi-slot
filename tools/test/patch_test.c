// The model without an emulator: what patch.c sends for each thing that can happen, and whether
// the main thread can ever read half a preset while the writer works.
//
//   patch_test [folder of the test instrument files]
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "dac.h"
#include "engine.h"
#include "files.h"
#include "patch.h"
#include "presets_gm.h"
#include "names.h"
#include "rom_image.h"

#define OP_TL 0
#define OP_SR 6
#define OP_FIELDS PATCH_OP_PARAMS
#define VOICE_SYSEX_BYTES 56
#define CELESTA 8
#define STRESS_MS 1500

static int failures;
static uint8_t sent[65536];
static size_t sent_bytes;

#define CHECK(condition, ...) \
	do { \
		bool check_passed = (condition); \
		printf("  %s: ", check_passed ? "ok  " : "FAIL"); \
		printf(__VA_ARGS__); \
		printf("\n"); \
		if (!check_passed) failures++; \
	} while (0)

static void record(void *context, const uint8_t *bytes, size_t length)
{
	(void)context;
	memcpy(sent + sent_bytes, bytes, length);
	sent_bytes += length;
}

static void clear(void)
{
	sent_bytes = 0;
}

static void sync(mdmi_patch *patch)
{
	patch_sync(patch, &patch->rom, record, NULL);
}

static void set(mdmi_patch *patch, uint32_t id, int value)
{
	patch_set(patch, id, value);
	sync(patch);
}

static void midi(mdmi_patch *patch, uint8_t a, uint8_t b, uint8_t c)
{
	const uint8_t message[3] = {a, b, c};
	patch_midi(patch, message, (a & 0xF0) == 0xC0 ? 2 : 3, record, NULL);
	patch_flush(patch, record, NULL);
}

// How many messages of a kind went out. 0xF0 counts SysEx with the given command.
static int count_status(uint8_t status)
{
	int count = 0;
	for (size_t i = 0; i < sent_bytes;) {
		size_t length;
		if (sent[i] == 0xF0) {
			const uint8_t *end = memchr(sent + i, 0xF7, sent_bytes - i);
			length = (size_t)(end - (sent + i)) + 1;
		} else {
			length = (sent[i] & 0xF0) == 0xC0 ? 2 : 3;
		}
		if (sent[i] == status || (sent[i] & 0xF0) == status) {
			count++;
		}
		i += length;
	}
	return count;
}

static int count_voices(void)
{
	int count = 0;
	for (size_t i = 0; i + 4 < sent_bytes; i++) {
		if (sent[i] == 0xF0 && sent[i + 4] == 0x0A) {
			count++;
		}
	}
	return count;
}

// How many SysEx 0A of a type went out: 00 a voice, 01 an envelope, 02 an FM row, 03 a PSG row.
static int count_stored(uint8_t type)
{
	int count = 0;
	for (size_t i = 0; i + 5 < sent_bytes; i++) {
		if (sent[i] == 0xF0 && sent[i + 1] == 0x00 && sent[i + 4] == 0x0A && sent[i + 5] == type) {
			count++;
		}
	}
	return count;
}

static int op_param(int op, int field)
{
	return PARAM_OP_BASE + op * OP_FIELDS + field;
}

static uint32_t op_id(int channel, int op, int field)
{
	return PATCH_CHANNEL_ID(channel, op_param(op, field));
}

#define PRESET_ID(channel) PATCH_CHANNEL_ID(channel, PARAM_PRESET)

// How many of our SysEx with a command went out.
static int count_sysex(uint8_t command)
{
	int count = 0;
	for (size_t i = 0; i + 4 < sent_bytes; i++) {
		if (sent[i] == 0xF0 && sent[i + 1] == 0x00 && sent[i + 4] == command) {
			count++;
		}
	}
	return count;
}

static void test_nothing_to_send(void)
{
	printf("Fresh model\n");
	static mdmi_patch patch;
	patch_init(&patch);
	clear();
	sync(&patch);
	CHECK(sent_bytes == 0, "the first sync sends nothing, %d bytes", (int)sent_bytes);
}

static void test_preset_and_tweaks(void)
{
	printf("Preset knob and program from a clip\n");
	static mdmi_patch patch;
	patch_init(&patch);
	clear();
	set(&patch, PRESET_ID(0), 5);
	CHECK(sent_bytes == 2 && sent[0] == 0xC0 && sent[1] == 5, "a new preset is one plain Program Change, %d bytes", (int)sent_bytes);

	clear();
	int tweaked = PRESETS_GM[5][4 + 9] > 60 ? 10 : 100;
	set(&patch, op_id(0, 0, OP_TL), tweaked);
	CHECK(sent_bytes == 3 && sent[0] == 0xB0 && sent[1] == 16 && sent[2] == tweaked, "a tweak is one CC");

	clear();
	set(&patch, PRESET_ID(0), 5);
	CHECK(sent_bytes == 0 && patch.model.channels[0][op_param(0, OP_TL)] == tweaked, "a lane repeating the preset keeps the tweak and sends nothing");

	clear();
	midi(&patch, 0xC0, 5, 0);
	CHECK(count_status(0xC0) == 0 && count_status(0xB0) == 1 && sent[2] == PRESETS_GM[5][4 + 9], "a program from a clip wipes the tweak, with one CC and no program");

	clear();
	midi(&patch, 0xC0, 5, 0);
	CHECK(sent_bytes == 0, "the same program again sends nothing, %d bytes", (int)sent_bytes);

	clear();
	midi(&patch, 0xC6, 62, 0);
	CHECK(sent_bytes == 0, "a PSG program with the envelope the channel plays sends nothing (D28)");
	midi(&patch, 0xC6, 80, 0);
	CHECK(sent_bytes == 2 && sent[0] == 0xC6 && sent[1] == 80, "one with another envelope is a program change");
	clear();
	midi(&patch, 0xC6, 80, 0);
	CHECK(sent_bytes == 0, "and only once");

	set(&patch, op_id(0, 0, OP_TL), tweaked);
	clear();
	midi(&patch, 0xB0, 3, 5);
	CHECK(sent_bytes == 0 && patch.model.channels[0][op_param(0, OP_TL)] == tweaked, "the preset CC with the same preset keeps the tweak");
	midi(&patch, 0xB3, 3, 40);
	CHECK(count_status(0xC0) == 1 && sent[0] == 0xC3 && patch.model.channels[3][PARAM_PRESET] == 40, "and a new one on channel 4 is a program there");
}

static void test_shadowed_presets(void)
{
	printf("Presets under the service numbers\n");
	static mdmi_patch patch;
	patch_init(&patch);
	clear();
	set(&patch, PRESET_ID(0), 120);
	CHECK(count_voices() == 1 && count_status(0xC0) == 1 && sent[sent_bytes - 2] == 0xC0 && sent[sent_bytes - 1] == 116, "preset 120 goes as a voice under service number 116, %d", sent[sent_bytes - 1]);
	CHECK(memcmp(sent + 7, PRESETS_GM[120], 48) == 0, "the voice bytes are the preset's");
	clear();
	set(&patch, PRESET_ID(0), 121);
	CHECK(count_voices() == 1 && sent[sent_bytes - 1] == 117, "the next voice takes the channel's other number, %d", sent[sent_bytes - 1]);
	clear();
	set(&patch, PRESET_ID(0), 3);
	CHECK(sent_bytes == 2 && sent[0] == 0xC0, "back below them is a plain program again");
}

static void test_sustain_rate(void)
{
	printf("Sustain rate above what a CC reaches\n");
	static mdmi_patch patch;
	patch_init(&patch);
	clear();
	set(&patch, op_id(0, 1, OP_SR), 25);
	// Operator 2 of channel 1 lives in register 0x78, because registers run 1, 3, 2, 4.
	static const uint8_t expected[10] = {0xF0, 0x00, 0x22, 0x77, 0x08, 0x07, 0x08, 0x01, 0x09, 0xF7};
	CHECK(sent_bytes == 10 && memcmp(sent, expected, 10) == 0, "SR 25 on channel 1 is a register write");
	clear();
	set(&patch, op_id(0, 1, OP_SR), 25);
	CHECK(sent_bytes == 0, "and only once");
	clear();
	set(&patch, op_id(0, 1, OP_SR), 7);
	CHECK(sent_bytes == 3 && sent[0] == 0xB0, "SR 7 is a CC");
	clear();
	set(&patch, op_id(4, 0, OP_SR), 31);
	CHECK(sent_bytes == 10 && sent[4] == 0x09 && sent[6] == 0x01, "channel 5 uses the chip's second part");
}

static void test_modes(void)
{
	printf("DAC and global controllers\n");
	static mdmi_patch patch;
	patch_init(&patch);
	clear();
	set(&patch, GLOBAL_DAC, 1);
	// Stage 35 (D80): the ROM puts the chip back after the driver's load; the plugin sends the switch alone.
	CHECK(count_voices() == 0 && sent_bytes == 3 && sent[0] == 0xB6 && sent[1] == 78, "the DAC goes on channel 7, where it applies once, and nothing else: the ROM restores the voices");
	clear();
	midi(&patch, 0xB0, 78, 0);
	CHECK(count_voices() == 0 && sent_bytes == 3 && patch.model.globals[GLOBAL_DAC] == 0, "the same from a clip's CC78");
	clear();
	midi(&patch, 0xB3, 74, 0);
	CHECK(sent_bytes == 3 && sent[0] == 0xB6 && sent[1] == 74 && patch.model.globals[GLOBAL_LFO_ON] == 0, "LFO from any channel goes to channel 7");
	clear();
	midi(&patch, 0xB2, 84, 127);
	CHECK(sent_bytes == 0, "CC84 stops here: it would let any note take any voice");
}

static bool sent_cc(uint8_t status, uint8_t cc, uint8_t value);

// Stage 38 (D83): MONO of FM channels 1-6 is a global knob each, ids 24-29, CC 105 on the channel.
static void test_mono(void)
{
	printf("MONO\n");
	static mdmi_patch patch, other;
	patch_init(&patch);
	clear();
	sync(&patch);
	CHECK(sent_bytes == 0, "MONO off is what the ROM starts with");
	char name[64];
	snprintf(name, sizeof(name), "%s", patch_param(GLOBAL_MONO(0))->name);
	CHECK(!strcmp(name, "Mono CH1") && !strcmp(patch_param(GLOBAL_MONO(5))->name, "Mono CH6") && patch_param(GLOBAL_MONO(5))->max == 1, "the knobs are Mono CH1 to Mono CH6: %s", name);
	clear();
	set(&patch, GLOBAL_MONO(3), 1);
	CHECK(sent_bytes == 3 && sent[0] == 0xB3 && sent[1] == 105 && sent[2] == 127, "Mono CH4 on goes as CC 105 127 on MIDI 4");
	CHECK(patch_alone_reason(&patch.model, 3) == PATCH_ALONE_MONO && patch_lend_reason(&patch.model, 3) == PATCH_ALONE_NO,
		"a MONO channel plays alone and lends its silent voice, as Porta");
	clear();
	midi(&patch, 0xB1, 105, 100);
	CHECK(patch.model.globals[GLOBAL_MONO(1)] == 1, "CC 105 from a clip on MIDI 2 turns Mono CH2 on");
	midi(&patch, 0xB1, 105, 10);
	CHECK(patch.model.globals[GLOBAL_MONO(1)] == 0, "and off");
	const patch_values before = patch.model;
	midi(&patch, 0xB7, 105, 127);
	CHECK(!memcmp(patch.model.globals, before.globals, sizeof(before.globals)), "on MIDI 8 it moves nothing");
	set(&patch, GLOBAL_CH3_MODE, 1);
	set(&patch, GLOBAL_MONO(2), 1);
	CHECK(patch_alone_reason(&patch.model, 2) == PATCH_ALONE_CH3, "Ch3 Special comes before MONO on channel 3");
	set(&patch, GLOBAL_CH3_MODE, 0);
	set(&patch, GLOBAL_MONO(2), 0);
	// After a ROM reset the ROM gets the channel's MONO again.
	clear();
	patch_rom_booted(&patch, record, NULL);
	CHECK(sent_cc(0xB3, 105, 127), "a booted ROM gets Mono CH4 again");
	// A project from before stage 38 opens with MONO off.
	static uint8_t state[PATCH_NUM_IDS * 6], old[PATCH_NUM_IDS * 6];
	patch_publish(&patch);
	patch_save(patch_view(&patch), state);
	size_t old_size = 0;
	for (size_t i = 0; i < patch_save_size() / 6; i++) {
		uint32_t id;
		memcpy(&id, state + i * 6, sizeof(id));
		if (id < GLOBAL_MONO_BASE || id >= PATCH_NUM_GLOBALS) {
			memcpy(old + old_size, state + i * 6, 6);
			old_size += 6;
		}
	}
	patch_init(&other);
	CHECK(old_size == patch_save_size() - 6 * 6 && patch_load(&other, old, old_size, NULL, NULL, NULL, NULL) && patch_take_incoming(&other) && !other.model.globals[GLOBAL_MONO(3)],
		"a project without ids 24-29 opens with MONO off");
}

// Stage 16 (D45): the noise's two knobs go as CC 12 and 13 on MIDI 10 and again after a reset.
static bool sent_cc(uint8_t status, uint8_t cc, uint8_t value)
{
	for (size_t i = 0; i + 2 < sent_bytes; i++) {
		if (sent[i] == status && sent[i + 1] == cc && sent[i + 2] == value) {
			return true;
		}
	}
	return false;
}

static void test_noise(void)
{
	printf("PSG noise\n");
	static mdmi_patch patch, other;
	patch_init(&patch);
	clear();
	sync(&patch);
	CHECK(sent_bytes == 0, "White and High are what the ROM starts with");
	char text[16];
	patch_format(&patch.bank, GLOBAL_NOISE_RATE, PATCH_NOISE_RATE_TONE3, text, sizeof(text));
	CHECK(!strcmp(text, "Tone 3"), "the rate reads %s", text);
	clear();
	set(&patch, GLOBAL_NOISE_RATE, PATCH_NOISE_RATE_KIT);
	CHECK(sent_cc(0xB9, 13, 104), "Kit goes as CC 13 104, the ROM's fifth range");
	midi(&patch, 0xB9, 13, 127);
	CHECK(patch.model.globals[GLOBAL_NOISE_RATE] == PATCH_NOISE_RATE_KIT, "CC 13 127 from a clip is Kit");
	set(&patch, GLOBAL_NOISE_RATE, PATCH_NOISE_RATE_HIGH);
	clear();
	set(&patch, GLOBAL_NOISE_TYPE, PATCH_NOISE_PERIODIC);
	set(&patch, GLOBAL_NOISE_RATE, PATCH_NOISE_RATE_TONE3);
	CHECK(sent_bytes == 6 && sent_cc(0xB9, 12, 0) && sent_cc(0xB9, 13, 78), "type and rate go as CC 12 and 13 on MIDI 10");
	clear();
	set(&patch, GLOBAL_DAC, 1);
	CHECK(!sent_cc(0xB9, 12, 0) && !sent_cc(0xB9, 13, 78), "a driver swap sends neither: the ROM writes the mode again");
	clear();
	const uint8_t reset = 0xFF;
	patch_midi(&patch, &reset, 1, record, NULL);
	CHECK(sent_cc(0xB9, 12, 0) && sent_cc(0xB9, 13, 78), "after a reset both go again");
	clear();
	midi(&patch, 0xB9, 13, 40);
	CHECK(patch.model.globals[GLOBAL_NOISE_RATE] == PATCH_NOISE_RATE_MID && sent_cc(0xB9, 13, 26), "CC 13 from a clip on MIDI 10 moves the knob");
	clear();
	midi(&patch, 0xB8, 12, 127);
	CHECK(patch.model.globals[GLOBAL_NOISE_TYPE] == PATCH_NOISE_PERIODIC, "CC 12 on another channel doesn't");
	patch_publish(&patch);
	static uint8_t state[PATCH_NUM_IDS * 6];
	patch_save(patch_view(&patch), state);
	patch_init(&other);
	CHECK(patch_load(&other, state, sizeof(state), NULL, NULL, NULL, NULL) && patch_take_incoming(&other), "a saved state loads");
	clear();
	patch_rom_booted(&other, record, NULL);
	CHECK(sent_cc(0xB9, 12, 0) && sent_cc(0xB9, 13, 26), "a loaded project sends its noise to a booted ROM");
}

static void test_incoming(void)
{
	printf("Messages from the host\n");
	static mdmi_patch patch;
	patch_init(&patch);
	clear();
	midi(&patch, 0xB2, 16, 99);
	CHECK(sent_bytes == 3 && sent[0] == 0xB2 && sent[2] == 99 && patch.model.channels[2][op_param(0, OP_TL)] == 99, "a CC goes into its channel's model and out once");
	clear();
	midi(&patch, 0xB2, 16, 99);
	CHECK(sent_bytes == 0, "the same CC again sends nothing");
	clear();
	midi(&patch, 0x91, 60, 100);
	midi(&patch, 0xE1, 0, 64);
	midi(&patch, 0xB1, 11, 90);
	CHECK(sent_bytes == 9 && sent[0] == 0x91 && sent[3] == 0xE1 && sent[6] == 0xB1, "what plays passes through");
	clear();
	midi(&patch, 0xB0, 121, 0);
	CHECK(count_status(0xE0) == 1 && count_status(0xB0) == 1 && sent[4] == 64 && patch.model.channels[2][op_param(0, OP_TL)] == 99, "reset all controllers keeps the voice");
	clear();
	midi(&patch, 0xB3, 77, 64);
	CHECK(sent_bytes == 3 && sent[1] == 10 && sent[2] == 0, "stereo bits move the Pan knob");
	clear();
	midi(&patch, 0xBA, 16, 50);
	midi(&patch, 0xB6, 77, 64);
	midi(&patch, 0xB6, 16, 50);
	midi(&patch, 0xB0, 87, 5);
	CHECK(sent_bytes == 0, "voice values on channels without a voice, and storing a program, stop here, %d bytes", (int)sent_bytes);
	clear();
	const uint8_t reset = 0xFF;
	patch_midi(&patch, &reset, 1, record, NULL);
	CHECK(sent[0] == 0xB1 && sent[1] == 123 && sent[3] == 0xFF && count_voices() == 6 && count_sysex(0x03) == 1, "the held note stops, a reset goes through, then static mapping and every voice, %d", count_voices());
	CHECK(patch.model.channels[2][op_param(0, OP_TL)] == 99, "the model survives the reset");
	clear();
	const uint8_t sysex[6] = {0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7};
	patch_midi(&patch, sysex, sizeof(sysex), record, NULL);
	CHECK(memcmp(sent, sysex, sizeof(sysex)) == 0 && count_stored(0x00) == 6 && count_sysex(0x03) == 1 && count_stored(0x02) == 6 && count_stored(0x03) == 1,
		"a GM reset goes through, then static mapping, every voice, and the row under each number in use, unknown now (the PSG channels share 1): %d FM, %d PSG", count_stored(0x02), count_stored(0x03));
	clear();
	sync(&patch);
	CHECK(sent_bytes == 0, "after that nothing is left to send");
}

static void test_load(void)
{
	printf("Saving and loading\n");
	static mdmi_patch patch, other;
	patch_init(&patch);
	patch_init(&other);
	set(&patch, PRESET_ID(0), 9);
	set(&patch, op_id(0, 2, OP_TL), 77);
	set(&patch, PATCH_CHANNEL_ID(9, PARAM_VOLUME), 50);
	set(&patch, GLOBAL_LFO_RATE, 5);
	patch_publish(&patch);
	static uint8_t state[PATCH_NUM_IDS * 6];
	CHECK(patch_save_size() == sizeof(state), "%d knobs, %d bytes", PATCH_NUM_IDS, (int)patch_save_size());
	patch_save(patch_view(&patch), state);
	CHECK(patch_load(&other, state, sizeof(state), NULL, NULL, NULL, NULL), "a saved state loads");
	CHECK(patch_get(&other, PRESET_ID(0)) == 9, "the main thread sees it before it is taken");
	CHECK(other.model.channels[0][PARAM_PRESET] == 0, "the writer's model is untouched until then");
	CHECK(patch_take_incoming(&other) && !patch_take_incoming(&other), "it is taken once");
	CHECK(patch_get(&other, PRESET_ID(0)) == 9 && patch_get(&other, op_id(0, 2, OP_TL)) == 77 && patch_get(&other, GLOBAL_LFO_RATE) == 5
		&& patch_get(&other, PATCH_CHANNEL_ID(9, PARAM_VOLUME)) == 50, "the snapshot holds it afterwards, on every channel");
	// An unknown knob is skipped and a value out of range is clamped.
	static uint8_t odd[12];
	const uint32_t unknown = GLOBAL_RETIRED, rate = GLOBAL_LFO_RATE;
	const int16_t big = 99, any = 1;
	memcpy(odd, &unknown, 4);
	memcpy(odd + 4, &any, 2);
	memcpy(odd + 6, &rate, 4);
	memcpy(odd + 10, &big, 2);
	CHECK(patch_load(&other, odd, sizeof(odd), NULL, NULL, NULL, NULL) && patch_take_incoming(&other) && other.model.globals[GLOBAL_LFO_RATE] == 7
		&& other.model.globals[GLOBAL_RETIRED] == 0, "the old Polyphony id is skipped, a value out of range clamped");
	CHECK(other.model.channels[0][PARAM_PRESET] == 0, "and a knob the state lacks gets its default");
	CHECK(!patch_load(&other, odd, 7, NULL, NULL, NULL, NULL), "a cut state is refused");
	CHECK(other.model.globals[GLOBAL_MASTER] == PATCH_MASTER_UNITY, "a project from before Master Volume opens at 0 dB");
}

// Master Volume (stage 18a): the plugin's output level, never the ROM's.
static void test_master(void)
{
	printf("Master Volume\n");
	static mdmi_patch patch;
	patch_init(&patch);
	clear();
	sync(&patch);
	CHECK(GLOBAL_MASTER == 9 && patch_get(&patch, GLOBAL_MASTER) == PATCH_MASTER_UNITY, "id 9, at 0 dB by default");
	char text[16];
	patch_format(&patch.bank, GLOBAL_MASTER, 0, text, sizeof(text));
	CHECK(!strcmp(text, "-inf dB"), "0 reads %s", text);
	patch_format(&patch.bank, GLOBAL_MASTER, 540, text, sizeof(text));
	CHECK(!strcmp(text, "-6.0 dB"), "540 reads %s", text);
	patch_format(&patch.bank, GLOBAL_MASTER, PATCH_MASTER_UNITY, text, sizeof(text));
	CHECK(!strcmp(text, "0.0 dB"), "600 reads %s", text);
	patch_format(&patch.bank, GLOBAL_MASTER, PATCH_MASTER_MAX, text, sizeof(text));
	CHECK(!strcmp(text, "+12.0 dB"), "720 reads %s", text);
	int value = -1;
	CHECK(patch_parse(&patch.bank, GLOBAL_MASTER, "-6", &value) && value == 540, "\"-6\" is -6 dB (%d)", value);
	CHECK(patch_parse(&patch.bank, GLOBAL_MASTER, "-6 dB", &value) && value == 540, "\"-6 dB\" is -6 dB (%d)", value);
	CHECK(patch_parse(&patch.bank, GLOBAL_MASTER, "-inf", &value) && value == 0, "\"-inf\" is silence (%d)", value);
	CHECK(patch_master_gain(0) == 0 && fabs(patch_master_gain(PATCH_MASTER_UNITY) - 1) < 1e-9 && fabs(patch_master_gain(540) - 0.501187) < 1e-5,
		"the factor is 0, 1 and 0.501 at -inf, 0 and -6 dB");
	clear();
	set(&patch, GLOBAL_MASTER, 300);
	CHECK(sent_bytes == 0, "a move sends the ROM nothing");
}

static void discard(void *context, const uint8_t *bytes, size_t length)
{
	(void)context;
	(void)bytes;
	(void)length;
}

// A value from a saved state.
static int saved(const uint8_t *state, uint32_t id)
{
	for (int i = 0; i < PATCH_NUM_IDS; i++) {
		uint32_t at;
		int16_t value;
		memcpy(&at, state + i * 6, 4);
		memcpy(&value, state + i * 6 + 4, 2);
		if (at == id) {
			return value;
		}
	}
	return -1;
}

static volatile LONG stop_writer;
static mdmi_patch stress;

// Loads whole presets as fast as it can, the way a clip and a lane would.
static DWORD WINAPI writer_thread(LPVOID unused)
{
	(void)unused;
	int program = 0;
	while (!stop_writer) {
		program = (program + 1) & 63;
		patch_take_incoming(&stress);
		const uint8_t message[2] = {0xC0, (uint8_t)program};
		patch_midi(&stress, message, sizeof(message), discard, NULL);
		patch_publish(&stress);
		patch_set(&stress, PRESET_ID(0), (program * 3) & 63);
		patch_sync(&stress, &stress.rom, discard, NULL);
		patch_publish(&stress);
	}
	return 0;
}

static void test_threads(void)
{
	printf("Main thread against a busy writer, %d ms\n", STRESS_MS);
	patch_init(&stress);
	HANDLE writer = CreateThread(NULL, 0, writer_thread, NULL, 0, NULL);
	static uint8_t state[PATCH_NUM_IDS * 6];
	static uint8_t loaded[PATCH_NUM_IDS * 6];
	int torn = 0, reads = 0;
	DWORD start = GetTickCount();
	while (GetTickCount() - start < STRESS_MS) {
		patch_save(patch_view(&stress), state);
		int preset = saved(state, PRESET_ID(0));
		bool same = saved(state, PATCH_CHANNEL_ID(0, PARAM_ALGORITHM)) == PRESETS_GM[preset][0];
		for (int op = 0; op < PATCH_OPS; op++) {
			same = same && saved(state, op_id(0, op, OP_TL)) == PRESETS_GM[preset][4 + op * OP_FIELDS + 9];
		}
		if (!same) {
			torn++;
		}
		reads++;
		if (reads % 64 == 0) {
			memcpy(loaded, state, sizeof(loaded));
			patch_load(&stress, loaded, sizeof(loaded), NULL, NULL, NULL, NULL);
		}
	}
	stop_writer = 1;
	WaitForSingleObject(writer, INFINITE);
	CloseHandle(writer);
	CHECK(torn == 0 && reads > 1000, "%d of %d saved states hold half a preset", torn, reads);
}

// Notes of one moment on a channel, from the given key up in thirds.
static void moment(mdmi_patch *patch, int channel, int first, int count, bool on)
{
	for (int i = 0; i < count; i++) {
		const uint8_t message[3] = {(uint8_t)((on ? 0x90 : 0x80) | channel), (uint8_t)(first + i * 4), on ? 100 : 0};
		patch_midi(patch, message, 3, record, NULL);
	}
	patch_flush(patch, record, NULL);
}

static int free_voices(const mdmi_patch *patch)
{
	int count = 0;
	for (int voice = 0; voice < PATCH_NUM_FM_CHANNELS; voice++) {
		count += patch->voices[voice].channel < 0 && patch->rom.voice_owner[voice] == voice;
	}
	return count;
}

// A clip that loops: a chord on channel 4 with tweaks, a tune on channel 2 through it, a note on 1.
static void play_clip(mdmi_patch *patch)
{
	midi(patch, 0xB3, 16, 40);
	moment(patch, 3, 60, 4, true);
	moment(patch, 1, 72, 1, true);
	moment(patch, 1, 72, 1, false);
	moment(patch, 0, 50, 1, true);
	moment(patch, 1, 74, 1, true);
	moment(patch, 3, 60, 4, false);
	moment(patch, 0, 50, 1, false);
	moment(patch, 1, 74, 1, false);
}

static void test_dealer(void)
{
	printf("Voice dealer\n");
	static mdmi_patch patch;
	patch_init(&patch);
	clear();
	moment(&patch, 3, 60, 3, true);
	CHECK(count_sysex(0x00) == 2 && count_status(0x90) == 3 && count_status(0xB0) == 0 && count_status(0xC0) == 0, "a chord of 3 on fresh channels: two voices remapped, nothing else to send, then the notes");
	CHECK(sent[sent_bytes - 9] == 0x93 && sent[sent_bytes - 3] == 0x93, "the notes come after all the remapping");
	clear();
	moment(&patch, 3, 60, 3, false);
	CHECK(count_sysex(0x00) == 2 && count_status(0x80) == 3 && free_voices(&patch) == 6, "the chord ends: the lent voices go home with SysEx 00 only");

	clear();
	moment(&patch, 3, 48, 6, true);
	size_t chord = sent_bytes;
	moment(&patch, 3, 80, 1, true);
	CHECK(sent_bytes == chord && patch_muted_notes(&patch, 3) == 1, "the 7th note sends nothing and is marked");
	moment(&patch, 3, 80, 1, false);
	CHECK(sent_bytes == chord && patch_muted_notes(&patch, 3) == 0, "nor does its note off");
	clear();
	midi(&patch, 0xB3, 123, 0);
	CHECK(count_sysex(0x00) == 5 && free_voices(&patch) == 6, "All Notes Off sends the lent voices home");

	// A tune takes its voice back from a chord of 5: the chord's note moves to the free voice.
	moment(&patch, 3, 60, 5, true);
	int lender = -1;
	for (int voice = 0; voice < PATCH_NUM_FM_CHANNELS; voice++) {
		lender = lender < 0 && voice != 3 && patch.voices[voice].channel == 3 ? voice : lender;
	}
	int moved_key = patch.voices[lender].key;
	clear();
	moment(&patch, lender, 90, 1, true);
	bool moved = false;
	for (int voice = 0; voice < PATCH_NUM_FM_CHANNELS; voice++) {
		moved = moved || (voice != lender && patch.voices[voice].channel == 3 && patch.voices[voice].key == moved_key);
	}
	CHECK(patch.voices[lender].channel == lender && moved && free_voices(&patch) == 0 && patch_muted_notes(&patch, 3) == 0 && count_status(0x80) == 1 && count_status(0x90) == 2, "a tune takes its own voice, the chord's note there stops and starts again on the free voice");
	moment(&patch, lender, 90, 1, false);
	moment(&patch, 3, 60, 5, false);
	CHECK(free_voices(&patch) == 6, "and all of it ends without a voice left taken");

	// With every voice taken, a tune cuts a chord's note; when the tune ends, the note comes back.
	moment(&patch, 3, 48, 6, true);
	moment(&patch, 1, 90, 1, true);
	CHECK(patch_muted_notes(&patch, 3) == 1, "a tune over a chord of 6 cuts one of its notes");
	clear();
	moment(&patch, 1, 90, 1, false);
	int chord_voices = 0;
	for (int voice = 0; voice < PATCH_NUM_FM_CHANNELS; voice++) {
		chord_voices += patch.voices[voice].channel == 3;
	}
	CHECK(patch_muted_notes(&patch, 3) == 0 && chord_voices == 6 && count_status(0x90) == 1, "the tune ends and the cut note starts again on the freed voice");
	moment(&patch, 3, 48, 6, false);
	CHECK(free_voices(&patch) == 6, "and the chord ends with every voice free");

	// The owner's tst6: Porta On on channels 1, 3 and 4, Ch3 Special and the DAC. Since stage 28 (D69) a
	// silent Porta channel lends its voice, after the other free ones; Ch3 Special's and the DAC's never.
	set(&patch, PATCH_CHANNEL_ID(0, PARAM_PORTA_ON), 1);
	set(&patch, PATCH_CHANNEL_ID(2, PARAM_PORTA_ON), 1);
	set(&patch, PATCH_CHANNEL_ID(3, PARAM_PORTA_ON), 1);
	set(&patch, GLOBAL_CH3_MODE, 1);
	set(&patch, GLOBAL_DAC, 1);
	moment(&patch, 4, 60, 3, true);
	const bool porta_lent = patch.voices[0].channel == 4 || patch.voices[3].channel == 4;
	CHECK(patch_muted_notes(&patch, 4) == 0 && patch.voices[4].channel == 4 && patch.voices[1].channel == 4 && porta_lent
			&& patch.voices[2].channel < 0 && patch.voices[5].channel < 0,
		"tst6 (28): a chord of 3 on channel 5 has voices 5, 2 and a silent Porta channel's, none of Ch3 Special or the DAC");
	moment(&patch, 3, 34, 1, true);
	CHECK(patch_muted_notes(&patch, 4) == 0 && patch.voices[3].channel == 3 && patch.voices[0].channel == 4,
		"channel 4's own note takes its voice back, the chord's note moves to channel 1's");
	moment(&patch, 0, 40, 1, true);
	CHECK(patch_muted_notes(&patch, 4) == 1 && patch.voices[0].channel == 0, "channel 1's own note takes its voice too; with none free the chord's note is cut");
	CHECK(patch_alone_reason(&patch.model, 0) == PATCH_ALONE_PORTA && patch_alone_reason(&patch.model, 2) == PATCH_ALONE_CH3
			&& patch_alone_reason(&patch.model, 5) == PATCH_ALONE_DAC && patch_lend_reason(&patch.model, 0) == PATCH_ALONE_NO
			&& patch_lend_reason(&patch.model, 1) == PATCH_ALONE_NO && patch_lend_reason(&patch.model, 2) == PATCH_ALONE_CH3
			&& patch_lend_reason(&patch.model, 3) == PATCH_ALONE_NO && patch_lend_reason(&patch.model, 5) == PATCH_ALONE_DAC,
		"and the reasons: Porta 1 4 play alone but lend, Ch3 Special 3 and the DAC 6 lend none");
	moment(&patch, 0, 40, 1, false);
	CHECK(patch_muted_notes(&patch, 4) == 0 && patch.voices[0].channel == 4, "channel 1 silent again: the cut note comes back on its voice");
	// A switch frees or takes voices (stage 28, from 24b's parking): the DAC off frees voice 6, and the
	// cut note comes back with no Note Off; the DAC on takes it again and cuts it.
	moment(&patch, 0, 40, 1, true);
	CHECK(patch_muted_notes(&patch, 4) == 1, "channel 1 again: the chord's note is cut");
	clear();
	set(&patch, GLOBAL_DAC, 0);
	CHECK(patch_muted_notes(&patch, 4) == 0 && patch.voices[5].channel == 4 && count_status(0x84) == 0,
		"DAC off: the cut note comes back on voice 6 at once, with no Note Off");
	set(&patch, GLOBAL_DAC, 1);
	CHECK(patch_muted_notes(&patch, 4) == 1 && patch.voices[5].channel < 0, "DAC on: voice 6 is the DAC's, the note is cut again");
	clear();
	set(&patch, GLOBAL_CH3_MODE, 0);
	CHECK(patch_muted_notes(&patch, 4) == 0 && patch.voices[2].channel == 4 && count_status(0x84) == 0,
		"Ch3 Special off: the cut note comes back on voice 3, with no Note Off");
	set(&patch, GLOBAL_CH3_MODE, 1);
	CHECK(patch_muted_notes(&patch, 4) == 1 && patch.voices[2].channel < 0, "Ch3 Special on: voice 3 is channel 3's alone, the note is cut again");
	// Porta Off on a playing channel frees nothing: its note keeps its own voice, and nothing is lost.
	set(&patch, PATCH_CHANNEL_ID(0, PARAM_PORTA_ON), 0);
	CHECK(patch_muted_notes(&patch, 4) == 1 && patch.voices[0].channel == 0, "Porta Off over channel 1's note: its voice stays, the cut note waits");
	set(&patch, PATCH_CHANNEL_ID(0, PARAM_PORTA_ON), 1);
	moment(&patch, 0, 40, 1, false);
	moment(&patch, 3, 34, 1, false);
	moment(&patch, 4, 60, 3, false);
	set(&patch, PATCH_CHANNEL_ID(0, PARAM_PORTA_ON), 0);
	set(&patch, PATCH_CHANNEL_ID(2, PARAM_PORTA_ON), 0);
	set(&patch, PATCH_CHANNEL_ID(3, PARAM_PORTA_ON), 0);
	set(&patch, GLOBAL_CH3_MODE, 0);
	set(&patch, GLOBAL_DAC, 0);
	CHECK(free_voices(&patch) == 6 && patch_muted_notes(&patch, 4) == 0, "and tst6's case ends with every voice free");

	// held[] and the ROM's stack (architect, 28): Porta Off, key A, Porta On, key B, A off: the voice stays
	// channel 1's and isn't lent to a chord; B off frees it. Then the reverse: Porta On, two keys held,
	// Porta Off, both off: nothing stays taken.
	moment(&patch, 0, 50, 1, true);
	set(&patch, PATCH_CHANNEL_ID(0, PARAM_PORTA_ON), 1);
	moment(&patch, 0, 55, 1, true);
	moment(&patch, 0, 50, 1, false);
	moment(&patch, 4, 60, 6, true);
	const bool stays = patch.voices[0].channel == 0 && patch_muted_notes(&patch, 4) == 1;
	moment(&patch, 4, 60, 6, false);
	moment(&patch, 0, 55, 1, false);
	CHECK(stays && free_voices(&patch) == 6, "Porta On over a held note: the next key and the old one's Note Off keep the voice the channel's, unlent; its last Note Off frees it");
	moment(&patch, 0, 50, 1, true);
	moment(&patch, 0, 55, 1, true);
	set(&patch, PATCH_CHANNEL_ID(0, PARAM_PORTA_ON), 0);
	moment(&patch, 0, 50, 1, false);
	moment(&patch, 0, 55, 1, false);
	CHECK(free_voices(&patch) == 6, "Porta Off over two held keys: both Note Offs free everything");

	// The stage's own case (ROADMAP 28): Porta On on channels 1, 3 and 4, the others silent: a chord of 5 on
	// channel 5 sounds whole, on its own voice, 2 and 6 first, then two of the Porta channels'.
	set(&patch, PATCH_CHANNEL_ID(0, PARAM_PORTA_ON), 1);
	set(&patch, PATCH_CHANNEL_ID(2, PARAM_PORTA_ON), 1);
	set(&patch, PATCH_CHANNEL_ID(3, PARAM_PORTA_ON), 1);
	moment(&patch, 4, 60, 5, true);
	int porta_voices = 0;
	for (int voice = 0; voice < PATCH_NUM_FM_CHANNELS; voice++) {
		porta_voices += (voice == 0 || voice == 2 || voice == 3) && patch.voices[voice].channel == 4;
	}
	CHECK(patch_muted_notes(&patch, 4) == 0 && patch.voices[4].channel == 4 && patch.voices[1].channel == 4 && patch.voices[5].channel == 4 && porta_voices == 2,
		"Porta On on 1, 3, 4: a chord of 5 on channel 5 sounds whole, the Porta voices last (28)");
	moment(&patch, 4, 60, 5, false);
	set(&patch, PATCH_CHANNEL_ID(0, PARAM_PORTA_ON), 0);
	set(&patch, PATCH_CHANNEL_ID(2, PARAM_PORTA_ON), 0);
	set(&patch, PATCH_CHANNEL_ID(3, PARAM_PORTA_ON), 0);
	CHECK(free_voices(&patch) == 6, "and every voice is free after it");

	// Porta's held count against the ROM's note stack (architect, 28): the same key twice, then one
	// Note Off, leaves the channel holding; its voice isn't lent while the ROM still holds a note.
	set(&patch, PATCH_CHANNEL_ID(0, PARAM_PORTA_ON), 1);
	moment(&patch, 0, 50, 1, true);
	moment(&patch, 0, 50, 1, true);
	moment(&patch, 0, 50, 1, false);
	moment(&patch, 4, 60, 6, true);
	const bool kept = patch.voices[0].channel == 0;
	moment(&patch, 4, 60, 6, false);
	moment(&patch, 0, 50, 1, false);
	set(&patch, PATCH_CHANNEL_ID(0, PARAM_PORTA_ON), 0);
	CHECK(kept && free_voices(&patch) == 6 && patch_muted_notes(&patch, 4) == 0,
		"a Porta key struck twice and let go once still holds its voice; nothing is left taken");

	// A note and its note off at one moment.
	const uint8_t on[3] = {0x90, 70, 100}, off[3] = {0x80, 70, 0};
	patch_midi(&patch, on, 3, record, NULL);
	patch_midi(&patch, off, 3, record, NULL);
	patch_flush(&patch, record, NULL);
	CHECK(free_voices(&patch) == 6, "a note ending where it starts holds no voice");

	// Porta On switched while notes are held, either way.
	set(&patch, PATCH_CHANNEL_ID(0, PARAM_PORTA_ON), 1);
	moment(&patch, 0, 60, 1, true);
	set(&patch, PATCH_CHANNEL_ID(0, PARAM_PORTA_ON), 0);
	moment(&patch, 0, 64, 1, true);
	moment(&patch, 0, 64, 1, false);
	moment(&patch, 0, 60, 1, false);
	moment(&patch, 0, 60, 3, true);
	set(&patch, PATCH_CHANNEL_ID(0, PARAM_PORTA_ON), 1);
	moment(&patch, 0, 60, 3, false);
	set(&patch, PATCH_CHANNEL_ID(0, PARAM_PORTA_ON), 0);
	CHECK(free_voices(&patch) == 6, "Porta On switched over held notes leaves no voice taken");

	// Ten tweaks by CC on channel 4, every other channel on another preset: the worst a lent voice
	// costs the console without 0A.
	for (int channel = 0; channel < PATCH_NUM_FM_CHANNELS; channel++) {
		if (channel != 3) {
			set(&patch, PRESET_ID(channel), 20);
		}
	}
	for (int i = 0; i < 10; i++) {
		midi(&patch, 0xB3, (uint8_t)(16 + i), 8 + i * 8);
	}
	moment(&patch, 3, 60, 1, true);
	clear();
	moment(&patch, 3, 64, 1, true);
	size_t transition = sent_bytes - 3;
	CHECK(count_sysex(0x0A) == 0 && transition * 1000 / 480 <= 100, "a transition with tweaks goes without 0A in %d bytes, %d ms at 480 bytes/s", (int)transition, (int)(transition * 1000 / 480));
	moment(&patch, 3, 60, 1, false);
	moment(&patch, 3, 64, 1, false);

	// The same clip, looped: once the first pass settled the voices, every pass is the same.
	static uint8_t passes[2][4096];
	size_t lengths[2];
	play_clip(&patch);
	for (int pass = 0; pass < 2; pass++) {
		clear();
		play_clip(&patch);
		memcpy(passes[pass], sent, sent_bytes);
		lengths[pass] = sent_bytes;
	}
	CHECK(lengths[0] == lengths[1] && memcmp(passes[0], passes[1], lengths[0]) == 0 && free_voices(&patch) == 6, "a looped clip sends the same bytes every pass, %d bytes", (int)lengths[0]);

	clear();
	moment(&patch, 3, 60, 3, true);
	const uint8_t reset = 0xFF;
	clear();
	patch_midi(&patch, &reset, 1, record, NULL);
	CHECK(sent[0] == 0xB3 && sent[1] == 123 && free_voices(&patch) == 6, "a reset stops the chord first and sends its voices home");
}

// Stage 5: files on the main thread, with the writer taking them at once.
static const char *files_dir = "../../tools/test/files";
static char temp_dir[MAX_PATH];
static mdmi_patch files_patch;
static state_view files_view;
static int handed_knobs;

static void take_now(void *context, bool knobs)
{
	(void)context;
	patch_take_incoming(&files_patch);
	handed_knobs += knobs;
}

static void path_in(char *path, size_t size, const char *dir, const char *name)
{
	snprintf(path, size, "%s/%s", dir, name);
}

static void write_bytes(const char *name, const uint8_t *bytes, size_t size)
{
	char path[MAX_PATH];
	path_in(path, sizeof(path), temp_dir, name);
	FILE *file = fopen(path, "wb");
	fwrite(bytes, 1, size, file);
	fclose(file);
}

static bool open_file(mdmi_files *files, const char *dir, const char *name, int channel, int place, char *message)
{
	char path[MAX_PATH];
	path_in(path, sizeof(path), dir, name);
	const char *one = path;
	return place >= 0 ? files_put_places(files, &one, 1, false, place, NULL, message, FILES_MESSAGE) : files_open(files, path, channel, message, FILES_MESSAGE);
}

static bool files_put_places_one(mdmi_files *files, const char *name, int place, char *message)
{
	char path[MAX_PATH];
	path_in(path, sizeof(path), files_dir, name);
	const char *one = path;
	return files_put_places(files, &one, 1, true, place, NULL, message, FILES_MESSAGE) == 1;
}

static bool same_model(const patch_values *a, const patch_values *b)
{
	return !memcmp(a->globals, b->globals, sizeof(a->globals)) && !memcmp(a->channels, b->channels, sizeof(a->channels)) && !memcmp(a->names, b->names, sizeof(a->names));
}

static void test_files(void)
{
	mdmi_patch *patch = &files_patch;
	patch_init(patch);
	GetTempPathA(sizeof(temp_dir), temp_dir);
	strcat(temp_dir, "mdmi-patch-test");
	CreateDirectoryA(temp_dir, NULL);
	const files_host host = {patch, &files_view, take_now, NULL};
	static mdmi_files files;
	files_init(&files, &host);
	char message[FILES_MESSAGE];
	patch_values gm = patch->model;

	CHECK(open_file(&files, files_dir, "Duck Guitar.tfi", 3, -1, message) && !strcmp(patch->model.names[3], "Duck Guitar") && patch->model.channels[3][PARAM_ALGORITHM] == 0 && patch->model.channels[3][PARAM_FEEDBACK] == 4,
		"a TFI on channel 4: %s", message);
	CHECK(patch->model.channels[3][PARAM_PRESET] == 0 && !strcmp(patch_bank_view(patch)->places[0].name, "Grand Piano"), "the Preset and the bank stay");
	CHECK(files_undo_text(&files) && !strcmp(files_undo_text(&files), "CH4 back to Grand Piano"), "undo says what it puts back: %s", files_undo_text(&files) ? files_undo_text(&files) : "nothing");
	patch_values duck = patch->model;
	CHECK(files_undo(&files, message, sizeof(message)) && same_model(&patch->model, &gm), "undo puts the channel back: %s", message);
	CHECK(files_undo(&files, message, sizeof(message)) && same_model(&patch->model, &duck), "and again brings the file back");

	// Bad files leave everything as it was.
	patch_values before = patch->model;
	static patch_bank bank_before;
	bank_before = *patch_bank_view(patch);
	uint8_t junk[42];
	memset(junk, 0x7F, sizeof(junk));
	write_bytes("short.tfi", junk, 41);
	write_bytes("junk.tfi", junk, 42);
	write_bytes("song.mid", junk, 42);
	write_bytes("junk.mdsnap", junk, 42);
	struct {
		const char *dir, *name;
		int channel, place;
	} bad[] = {
		{temp_dir, "short.tfi", 0, -1}, {temp_dir, "junk.tfi", 0, -1}, {temp_dir, "song.mid", 0, -1}, {temp_dir, "junk.mdsnap", 0, -1},
		{temp_dir, "missing.vgi", 0, -1}, {files_dir, "BAS_PSG2.vgi", 6, -1}, {temp_dir, "junk.tfi", -1, 5},
	};
	for (size_t i = 0; i < sizeof(bad) / sizeof(*bad); i++) {
		bool opened = open_file(&files, bad[i].dir, bad[i].name, bad[i].channel, bad[i].place, message);
		CHECK(!opened && same_model(&patch->model, &before) && !memcmp(patch_bank_view(patch), &bank_before, sizeof(bank_before)) && !memcmp(&patch->bank, &bank_before, sizeof(bank_before)),
			"refused, nothing changed: %s", message);
	}

	// A file in a place of the bank, then the bank saved, everything changed, and the bank loaded.
	char bank_path[MAX_PATH], snap_path[MAX_PATH], vgi_path[MAX_PATH];
	path_in(bank_path, sizeof(bank_path), temp_dir, "test.mdbank");
	path_in(snap_path, sizeof(snap_path), temp_dir, "test.mdsnap");
	path_in(vgi_path, sizeof(vgi_path), temp_dir, "Thunder again.vgi");
	CHECK(open_file(&files, files_dir, "Thunder Guitar 03.tyi", 0, 9, message) && !strcmp(patch->bank.places[9].name, "Thunder Guitar 03"), "a TYI in place 10: %s", message);
	CHECK(files_undo_text(&files) && !strcmp(files_undo_text(&files), "slot 10 back to Glockenspiel"), "undo would put back: %s", files_undo_text(&files) ? files_undo_text(&files) : "nothing");
	static patch_bank thunder;
	thunder = patch->bank;
	CHECK(files_save(&files, bank_path, FILES_BANK, 0, message, sizeof(message)), "the bank saved: %s", message);
	open_file(&files, files_dir, "Duck Guitar.tfi", 0, 9, message);
	open_file(&files, files_dir, "Duck Guitar.tfi", 0, 20, message);
	CHECK(files_open(&files, bank_path, 0, message, sizeof(message)) && !memcmp(&patch->bank, &thunder, sizeof(thunder)) && !memcmp(patch_bank_view(patch), &thunder, sizeof(thunder)), "and loaded back: %s", message);
	CHECK(files_undo(&files, message, sizeof(message)) && !strcmp(patch->bank.places[20].name, "Duck Guitar"), "undo of a bank puts the bank before it back");
	files_undo(&files, message, sizeof(message));

	// A snapshot: bank, channels and their values.
	open_file(&files, files_dir, "GTR_Dis._'94.vgi", 2, -1, message);
	patch_set(patch, PATCH_CHANNEL_ID(2, PARAM_OP_BASE), 33);
	patch_set(patch, PATCH_CHANNEL_ID(7, PARAM_VOLUME), 50);
	patch_set(patch, GLOBAL_LFO_RATE, 6);
	patch_publish(patch);
	files_view = (state_view){.page = 0, .channel = 2};
	patch_values snapped = patch->model;
	CHECK(files_save(&files, snap_path, FILES_SNAPSHOT, 0, message, sizeof(message)), "a snapshot saved: %s", message);
	open_file(&files, files_dir, "BAS_PSG2.vgi", 2, -1, message);
	open_file(&files, files_dir, "BAS_PSG2.vgi", 0, 9, message);
	patch_set(patch, PATCH_CHANNEL_ID(7, PARAM_VOLUME), 100);
	patch_set(patch, GLOBAL_LFO_RATE, 1);
	patch_set(patch, PATCH_CHANNEL_ID(0, PARAM_PRESET), 9);
	patch_publish(patch);
	files_view = (state_view){.page = 1, .channel = 5};
	handed_knobs = 0;
	CHECK(files_open(&files, snap_path, 0, message, sizeof(message)) && same_model(&patch->model, &snapped) && !memcmp(&patch->bank, &thunder, sizeof(thunder)), "and loaded back: values, names and bank: %s", message);
	CHECK(files_view.page == 0 && files_view.channel == 2 && handed_knobs == 1, "with the view, and the host told to re-read the knobs");

	// A channel saved as VGI is the same voice when it comes back.
	patch_set(patch, PATCH_CHANNEL_ID(0, PARAM_PRESET), 9);
	patch_publish(patch);
	CHECK(files_save(&files, vgi_path, FILES_INSTRUMENT, 0, message, sizeof(message)), "channel 1 saved as VGI: %s", message);
	CHECK(files_open(&files, vgi_path, 4, message, sizeof(message)) && !memcmp(patch->model.channels[4] + PARAM_ALGORITHM, patch->model.channels[0] + PARAM_ALGORITHM, PATCH_VOICE_PARAMS * sizeof(int16_t)) && !strcmp(patch->model.names[4], "Thunder again"),
		"and loads on channel 5 as the same voice");
	CHECK(!files_save(&files, vgi_path, FILES_INSTRUMENT, 7, message, sizeof(message)), "a PSG channel has no instrument to save: %s", message);
	files_free(&files);
}

// Several files into the bank at once (D26).
static void test_places(void)
{
	mdmi_patch *patch = &files_patch;
	patch_init(patch);
	const files_host host = {patch, &files_view, take_now, NULL};
	static mdmi_files files;
	files_init(&files, &host);
	char message[FILES_MESSAGE];
	static const char *const NAMES[5] = {"Turtles Guitar.tfi", "Duck Guitar.tfi", "2019 Bass 02.tyi", "BAS_Magical_Bass.vgi", "OTHER_chip2.vgi"};
	static const char *const SORTED[5] = {"2019 Bass 02", "BAS_Magical_Bass", "Duck Guitar", "OTHER_chip2", "Turtles Guitar"};
	char paths[5][MAX_PATH];
	const char *list[6];
	for (int i = 0; i < 5; i++) {
		path_in(paths[i], sizeof(paths[i]), files_dir, NAMES[i]);
		list[i] = paths[i];
	}
	CHECK(files_add_place(&files, false) == 0, "an untouched bank adds from slot 1");

	// Channel 1 plays place 12 as it is, so it follows what lands there.
	patch_set(patch, PATCH_CHANNEL_ID(0, PARAM_PRESET), 11);
	patch_publish(patch);
	static patch_bank gm;
	gm = patch->bank;
	CHECK(files_put_places(&files, list, 5, false, 9, NULL, message, sizeof(message)) && !strcmp(message, "Loaded 5 to slots 10-14"), "five files on slot 10: %s", message);
	bool in_order = true;
	for (int i = 0; i < 5; i++) {
		in_order &= !strcmp(patch->bank.places[9 + i].name, SORTED[i]) && !strcmp(patch_bank_view(patch)->places[9 + i].name, SORTED[i]);
	}
	CHECK(in_order && !strcmp(patch->bank.places[14].name, patch_gm_name(14)), "take places 10-14 in the order of their names");
	CHECK(!strcmp(patch->model.names[0], "Duck Guitar"), "the channel on place 12 plays the file now there");
	CHECK(files_add_place(&files, false) == 14, "adding starts after the last file, slot 15");
	CHECK(files_undo_text(&files) && !strcmp(files_undo_text(&files), "bank before 5 files"), "undo would put the bank back");
	CHECK(files_undo(&files, message, sizeof(message)) && !memcmp(&patch->bank, &gm, sizeof(gm)) && !strcmp(patch->model.names[0], patch_gm_name(11)), "one undo puts the whole bank and the channel back");
	files_undo(&files, message, sizeof(message));

	CHECK(files_put_places(&files, list, 5, false, -1, NULL, message, sizeof(message)) && !strcmp(message, "Loaded 5 to slots 15-19"), "the same five added: %s", message);
	CHECK(!strcmp(patch->bank.places[14].name, SORTED[0]) && !strcmp(patch->bank.places[18].name, SORTED[4]), "after the last file");

	CHECK(files_put_places(&files, list, 5, false, 125, NULL, message, sizeof(message)) && !strcmp(patch->bank.places[127].name, SORTED[2]) && strstr(message, "2 didn't fit"),
		"what doesn't fit before 128 is skipped and told: %s", message);
	CHECK(files_add_place(&files, false) == PATCH_BANK_SIZE && !files_put_places(&files, list, 1, false, -1, NULL, message, sizeof(message)), "with a file in slot 128 nothing more is added: %s", message);

	uint8_t junk[42];
	memset(junk, 0x7F, sizeof(junk));
	write_bytes("junk.tfi", junk, 42);
	char junk_path[MAX_PATH];
	path_in(junk_path, sizeof(junk_path), temp_dir, "junk.tfi");
	const char *mixed[3] = {list[0], junk_path, list[1]};
	CHECK(files_put_places(&files, mixed, 3, false, 30, NULL, message, sizeof(message)) && !strcmp(patch->bank.places[30].name, "Duck Guitar") && !strcmp(patch->bank.places[31].name, "Turtles Guitar")
		&& !strcmp(patch->bank.places[32].name, patch_gm_name(32)) && strstr(message, "skipped 1: junk.tfi"),
		"a file that can't be read is skipped, the rest close up: %s", message);
	static patch_bank kept;
	kept = patch->bank;
	const char *bad[1] = {junk_path};
	CHECK(!files_put_places(&files, bad, 1, false, 40, NULL, message, sizeof(message)) && !memcmp(&patch->bank, &kept, sizeof(kept)), "only bad files: the bank stays: %s", message);
	files_free(&files);
}

// Stage 7: PSG envelopes (D28). The first stored envelope of a PSG channel, the SysEx 0A 01 it
// went as, or NULL.
static const uint8_t *stored_envelope(int *program, size_t *length)
{
	for (size_t i = 0; i < sent_bytes; i++) {
		if (sent[i] == 0xF0 && i + 6 < sent_bytes && sent[i + 4] == 0x0A && sent[i + 5] == 0x01) {
			const uint8_t *end = memchr(sent + i, 0xF7, sent_bytes - i);
			*program = sent[i + 6];
			*length = (size_t)(end - (sent + i)) + 1;
			return sent + i;
		}
	}
	return NULL;
}

// The program change a channel got last, or -1.
static int last_program(int channel)
{
	int program = -1;
	for (size_t i = 0; i < sent_bytes;) {
		if (sent[i] == 0xF0) {
			i += (size_t)((const uint8_t *)memchr(sent + i, 0xF7, sent_bytes - i) - (sent + i)) + 1;
			continue;
		}
		if (sent[i] == (0xC0 | channel)) {
			program = sent[i + 1];
		}
		i += (sent[i] & 0xF0) == 0xC0 ? 2 : 3;
	}
	return program;
}

static void test_envelopes(void)
{
	mdmi_patch *patch = &files_patch;
	patch_init(patch);
	const files_host host = {patch, &files_view, take_now, NULL};
	static mdmi_files files;
	files_init(&files, &host);
	char message[FILES_MESSAGE];
	int program;
	size_t length;

	clear();
	sync(patch);
	CHECK(sent_bytes == 0 && !strcmp(patch->model.psg[0].name, "Plain") && !strcmp(patch_bank_view(patch)->envelopes[80].name, "Square Lead"),
		"a booted ROM already plays the bank's envelopes: Plain, Square Lead on 81");
	char text[64];
	patch_format_preset(patch_bank_view(patch), 7, 80, text, sizeof(text));
	CHECK(!strcmp(text, "81 Square Lead"), "the Preset of a PSG channel is named after its place: %s", text);

	// An envelope dropped on channel 7: stored under the channel's service number, which it selects.
	clear();
	CHECK(open_file(&files, files_dir, "PSG Organ.eef", 6, -1, message) && !strcmp(patch->model.psg[0].name, "PSG Organ") && patch->model.channels[6][PARAM_PRESET] == 0,
		"an EEF on channel 7 is its envelope, the Preset stays: %s", message);
	sync(patch);
	const uint8_t organ[] = {0x04, 0x02, 0x00, 0xFE, 0x00, 0xFD, 0x02, 0x04, 0x06, 0x08, 0x0A, 0x0C, 0x0E};
	const uint8_t *sysex = stored_envelope(&program, &length);
	bool nibbles = sysex && length == 8 + 2 * sizeof(organ);
	for (size_t i = 0; nibbles && i < sizeof(organ); i++) {
		nibbles = sysex[7 + 2 * i] == organ[i] >> 4 && sysex[8 + 2 * i] == (organ[i] & 15);
	}
	CHECK(nibbles && program == 120 && last_program(6) == 120, "it goes as SysEx 0A 01 to program 120, then a program change to 120");
	CHECK(files_undo_text(&files) && !strcmp(files_undo_text(&files), "CH7 back to Plain"), "undo would put Plain back: %s", files_undo_text(&files) ? files_undo_text(&files) : "nothing");

	// Another for the same channel takes the other number, so a sounding note keeps its envelope.
	clear();
	open_file(&files, files_dir, "PSG Pluck.eef", 6, -1, message);
	sync(patch);
	CHECK(stored_envelope(&program, &length) && program == 121 && last_program(6) == 121, "the next one goes to 121");
	clear();
	files_undo(&files, message, sizeof(message));
	sync(patch);
	CHECK(!stored_envelope(&program, &length) && last_program(6) == 120 && !strcmp(patch->model.psg[0].name, "PSG Organ"), "undo brings the organ back, still held under 120: %s", message);

	// A preset step wipes it, and an envelope the ROM has is only a program change.
	clear();
	set(patch, PATCH_CHANNEL_ID(6, PARAM_PRESET), 80);
	CHECK(!stored_envelope(&program, &length) && last_program(6) == 80 && !strcmp(patch->model.psg[0].name, "Square Lead"), "a preset step plays the place: Square Lead by program change 81");
	clear();
	set(patch, PATCH_CHANNEL_ID(6, PARAM_PRESET), 3);
	CHECK(sent_bytes == 2 && last_program(6) == 3, "and Plain on place 4 by a program change too");

	// Different envelopes on all four channels at once.
	static const char *const FOUR[4] = {"PSG Pluck.eef", "PSG Organ.eef", "PSG Vibrato.eef", "Noise Hit.eef"};
	clear();
	for (int i = 0; i < 4; i++) {
		open_file(&files, files_dir, FOUR[i], 6 + i, -1, message);
	}
	sync(patch);
	bool four = true;
	for (int i = 0; i < 4; i++) {
		four &= last_program(6 + i) >= 120 + 2 * i && last_program(6 + i) <= 121 + 2 * i;
	}
	CHECK(four, "four channels, four envelopes, each under its own channel's numbers");

	// A place of the bank: a channel playing it as it is follows, one with its own envelope doesn't.
	set(patch, PATCH_CHANNEL_ID(7, PARAM_PRESET), 9);
	set(patch, PATCH_CHANNEL_ID(8, PARAM_PRESET), 9);
	open_file(&files, files_dir, "PSG Vibrato.eef", 8, -1, message);
	clear();
	CHECK(files_put_places_one(&files, "PSG Trill.eef", 9, message) && !strcmp(patch->bank.envelopes[9].name, "PSG Trill"), "an EEF in place 10 of the envelopes: %s", message);
	sync(patch);
	CHECK(!strcmp(patch->model.psg[1].name, "PSG Trill") && !strcmp(patch->model.psg[2].name, "PSG Vibrato"), "channel 8 on it follows, channel 9 with its own file keeps it");
	CHECK(!strcmp(patch->bank.places[9].name, patch_gm_name(9)), "the FM voices of the bank stay");
	CHECK(files_add_place(&files, true) == 10 && files_add_place(&files, false) == 0, "adding envelopes starts after the last one from a file, voices stay at slot 1");
	CHECK(files_undo(&files, message, sizeof(message)) && patch_envelope_is_rom(&patch->bank, 9), "undo puts the place back");
	files_undo(&files, message, sizeof(message));

	// Several envelopes into the bank, and added after the last one from a file.
	static const char *const PLACES[3] = {"PSG Vibrato.eef", "Duck Guitar.tfi", "PSG Pluck.eef"};
	char place_paths[3][MAX_PATH];
	const char *place_list[3];
	for (int i = 0; i < 3; i++) {
		path_in(place_paths[i], sizeof(place_paths[i]), files_dir, PLACES[i]);
		place_list[i] = place_paths[i];
	}
	CHECK(files_put_places(&files, place_list, 3, true, 40, NULL, message, sizeof(message)) == 2 && !strcmp(patch->bank.envelopes[40].name, "PSG Pluck")
		&& !strcmp(patch->bank.envelopes[41].name, "PSG Vibrato") && strstr(message, "skipped 1: Duck Guitar.tfi") && !strcmp(patch->bank.places[40].name, patch_gm_name(40)),
		"envelopes into places 41-42 by name, the TFI skipped: %s", message);
	CHECK(files_undo_text(&files) && !strcmp(files_undo_text(&files), "envelopes before 2 files"), "undo would put the envelopes back: %s", files_undo_text(&files) ? files_undo_text(&files) : "nothing");
	CHECK(files_put_places(&files, place_list, 3, true, -1, NULL, message, sizeof(message)) == 2 && !strcmp(patch->bank.envelopes[42].name, "PSG Pluck"), "added after them: %s", message);
	CHECK(files_undo(&files, message, sizeof(message)) && patch_envelope_is_rom(&patch->bank, 42) && !strcmp(patch->bank.envelopes[41].name, "PSG Vibrato"), "undo takes the added ones away");

	// A place from 121 on is a place: Plain there is the channel's own service number holding it,
	// not program 127, which belongs to channel 10.
	patch_init(patch);
	set(patch, PATCH_CHANNEL_ID(6, PARAM_PRESET), 80);
	clear();
	set(patch, PATCH_CHANNEL_ID(6, PARAM_PRESET), 126);
	CHECK(last_program(6) == 120, "Preset 127 on channel 7 plays under 120, its own number: %d", last_program(6));
	// After SysEx we can't read, the service numbers hold what we don't know, until stored again.
	patch_init(patch);
	open_file(&files, files_dir, "PSG Organ.eef", 6, -1, message);
	sync(patch);
	const uint8_t other[] = {0xF0, 0x00, 0x22, 0x77, 0x03, 0x00, 0xF7};
	patch_midi(patch, other, sizeof(other), record, NULL);
	clear();
	set(patch, PATCH_CHANNEL_ID(6, PARAM_PRESET), 126);
	CHECK(stored_envelope(&program, &length) != NULL, "after SysEx we can't read, Plain is stored again, not trusted to be under 121");

	// Bad files.
	static patch_bank kept;
	kept = patch->bank;
	patch_values was = patch->model;
	CHECK(!open_file(&files, files_dir, "Too Long.eef", 6, -1, message) && !strcmp(message, "Too Long.eef: 125 steps, max 124"), "a longer envelope is refused: %s", message);
	CHECK(!open_file(&files, files_dir, "PSG Pluck.eef", 0, -1, message) && !strcmp(message, "Select CH7-10 for an EEF file"), "an EEF on FM is refused: %s", message);
	CHECK(!open_file(&files, files_dir, "Duck Guitar.tfi", 6, -1, message) && !strcmp(message, "Select CH1-6 for an FM file"), "an FM file on PSG is refused: %s", message);
	write_bytes("empty.eef", (const uint8_t *)"", 0);
	CHECK(!open_file(&files, temp_dir, "empty.eef", 6, -1, message), "an empty file is refused: %s", message);
	CHECK(!memcmp(&patch->bank, &kept, sizeof(kept)) && !memcmp(&patch->model.psg, &was.psg, sizeof(was.psg)), "and nothing changed");

	// Saved, and loaded back: the envelope of a channel as EEF, a snapshot with everything.
	char eef_path[MAX_PATH], snap_path[MAX_PATH];
	path_in(eef_path, sizeof(eef_path), temp_dir, "Organ again.eef");
	path_in(snap_path, sizeof(snap_path), temp_dir, "envelopes.mdsnap");
	patch_publish(patch);
	CHECK(files_save(&files, eef_path, FILES_ENVELOPE, 7, message, sizeof(message)) && files_open(&files, eef_path, 9, message, sizeof(message))
		&& !memcmp(patch->model.psg[3].steps, patch->model.psg[1].steps, sizeof(patch->model.psg[1].steps)) && !strcmp(patch->model.psg[3].name, "Organ again"),
		"channel 8 saved as EEF loads on channel 10 as the same envelope");
	CHECK(!files_save(&files, eef_path, FILES_ENVELOPE, 2, message, sizeof(message)), "an FM channel has no envelope to save: %s", message);
	files_put_places_one(&files, "PSG Pluck.eef", 30, message);
	patch_publish(patch);
	static patch_bank snapped_bank;
	snapped_bank = patch->bank;
	patch_values snapped = patch->model;
	CHECK(files_save(&files, snap_path, FILES_SNAPSHOT, 0, message, sizeof(message)), "a snapshot saved: %s", message);
	open_file(&files, files_dir, "Noise Hit.eef", 7, -1, message);
	files_put_places_one(&files, "PSG Organ.eef", 30, message);
	CHECK(files_open(&files, snap_path, 0, message, sizeof(message)) && !memcmp(&patch->model.psg, &snapped.psg, sizeof(snapped.psg)) && !memcmp(&patch->bank, &snapped_bank, sizeof(snapped_bank)),
		"and loaded back: the channels' envelopes and the bank's");

	// A state of v0.6.0 has neither: the ROM's envelopes, each PSG channel on its place.
	patch_init(patch);
	size_t size;
	uint8_t *state = state_build(patch, &files_view, &size);
	uint8_t *old = malloc(size);
	size_t old_size = 0;
	memcpy(old, state, sizeof(uint32_t));
	old_size = sizeof(uint32_t);
	for (size_t at = sizeof(uint32_t); at + 8 <= size;) {
		uint32_t type, block;
		memcpy(&type, state + at, 4);
		memcpy(&block, state + at + 4, 4);
		if (type != 5 && type != 6) {
			memcpy(old + old_size, state + at, 8 + block);
			old_size += 8 + block;
		}
		at += 8 + block;
	}
	open_file(&files, files_dir, "PSG Organ.eef", 6, -1, message);
	files_put_places_one(&files, "PSG Organ.eef", 2, message);
	patch_set(patch, PATCH_CHANNEL_ID(9, PARAM_PRESET), 80);
	bool knobs;
	state_view view = files_view;
	CHECK(state_read(patch, old, old_size, &view, &knobs) && patch_take_incoming(patch) && patch_envelope_is_rom(&patch->bank, 2) && !strcmp(patch->model.psg[0].name, "Plain")
		&& !strcmp(patch->model.psg[3].name, "Plain"), "a state without envelopes: the ROM's, PSG channels on their places");
	free(state);
	free(old);

	// Envelopes from a clip would play past the model.
	patch_init(patch);
	clear();
	const uint8_t load[] = {0xF0, 0x00, 0x22, 0x77, 0x06, 0x00, 0x01, 0xF7};
	const uint8_t store[] = {0xF0, 0x00, 0x22, 0x77, 0x0A, 0x01, 0x05, 0x00, 0x01, 0xF7};
	patch_midi(patch, load, sizeof(load), record, NULL);
	patch_midi(patch, store, sizeof(store), record, NULL);
	CHECK(sent_bytes == 0 && patch->rom.psg_programs[0] == 0, "SysEx 06 and 0A 01 from a clip are dropped");
	files_free(&files);
}

// What the ROM reads as drawn (midi_psg.c apply_envelope_step): at most one loop start, an end
// only after it, a frame between them and after the start, nothing past the limit.
static bool rom_reads_as_drawn(const patch_envelope *envelope)
{
	const int length = patch_envelope_length(envelope);
	int start = -1, end = -1;
	for (int i = 0; i < length; i++) {
		const uint8_t step = envelope->steps[i];
		if (step == PATCH_EEF_LOOP_START) {
			if (start >= 0) {
				return false;
			}
			start = i;
		} else if (step == PATCH_EEF_LOOP_END) {
			if (start < 0 || end >= 0 || i == start + 1) {
				return false;
			}
			end = i;
		}
	}
	return length >= 1 && length <= PATCH_ENVELOPE_STEPS && (start < 0 || start + 1 < length) && envelope->steps[length] == PATCH_EEF_END;
}

// Stage 8: the editor's shape of an envelope, and drawing on a channel the way a file goes.
static void test_drawing(void)
{
	mdmi_patch *patch = &files_patch;
	patch_init(patch);
	const files_host host = {patch, &files_view, take_now, NULL};
	static mdmi_files files;
	files_init(&files, &host);
	char message[FILES_MESSAGE];

	// An envelope keeps its bytes through the editor's shape.
	CHECK(open_file(&files, files_dir, "PSG Organ.eef", 7, -1, message), "PSG Organ on channel 8: %s", message);
	patch_envelope organ = patch->model.psg[1], back = organ;
	patch_shape shape;
	patch_shape_of(&organ, &shape);
	patch_shape_to_envelope(&shape, &back);
	CHECK(shape.count == 11 && shape.loop_start == 3 && shape.loop_end == 4 && !memcmp(&back, &organ, sizeof(organ)),
		"PSG Organ: 11 frames, loop over frame 4, the same bytes back");

	// Whatever the shape, the envelope is one the ROM reads as drawn.
	srand(8);
	bool valid = true;
	int worst = 0;
	for (int i = 0; i < 20000 && valid; i++) {
		shape.count = rand() % 132 - 2;
		shape.loop_start = rand() % 132 - 2;
		shape.loop_end = rand() % 132 - 2;
		for (int f = 0; f < PATCH_ENVELOPE_STEPS; f++) {
			shape.frames[f] = (uint8_t)rand();
		}
		patch_envelope envelope = organ;
		patch_shape_to_envelope(&shape, &envelope);
		valid = rom_reads_as_drawn(&envelope) && shape.count + (shape.loop_start >= 0) + (shape.loop_end >= 0) == patch_envelope_length(&envelope);
		worst = patch_envelope_length(&envelope) > worst ? patch_envelope_length(&envelope) : worst;
	}
	CHECK(valid && worst == PATCH_ENVELOPE_STEPS, "20000 random shapes: no empty loop, no stray mark, %d bytes at most", worst);
	// A loop end at the last frame is no end: it plays the same, one byte less.
	shape.count = 5;
	shape.loop_start = 1;
	shape.loop_end = 5;
	patch_envelope envelope = organ;
	patch_shape_to_envelope(&shape, &envelope);
	CHECK(shape.loop_end == -1 && patch_envelope_length(&envelope) == 6, "a loop to the last frame keeps only its start");
	// An empty envelope, silent in the ROM, stays silent in the editor.
	const uint8_t end = PATCH_EEF_END;
	patch_envelope_from_eef(&envelope, &end, 1);
	patch_shape_of(&envelope, &shape);
	CHECK(shape.count == 1 && (shape.frames[0] & 15) == 15, "an empty envelope shows as one silent frame");

	// A gesture of two steps: the channel's own envelope, the Preset and the bank stay, undo puts back
	// what was before the gesture, a second undo the drawing.
	const int preset = patch->model.channels[7][PARAM_PRESET];
	static patch_bank bank_before;
	bank_before = *patch_bank_view(patch);
	patch_shape_of(&organ, &shape);
	shape.frames[0] = 0x0F;
	patch_envelope first = organ, drawn = organ;
	patch_shape_to_envelope(&shape, &first);
	files_draw_envelope(&files, 7, &organ, &first);
	shape.frames[1] = 0x50;
	patch_shape_to_envelope(&shape, &drawn);
	files_draw_envelope(&files, 7, NULL, &drawn);
	CHECK(!memcmp(&patch->model.psg[1], &drawn, sizeof(drawn)) && patch->model.channels[7][PARAM_PRESET] == preset && !memcmp(patch_bank_view(patch), &bank_before, sizeof(bank_before)),
		"drawing changes the channel's envelope, not its Preset or the bank");
	clear();
	sync(patch);
	int program;
	size_t length;
	CHECK(stored_envelope(&program, &length) && length == 8 + 2 * (size_t)patch_envelope_length(&drawn), "and goes to the ROM as a stored envelope");
	CHECK(files_undo_text(&files) && !strcmp(files_undo_text(&files), "CH8 envelope before drawing"), "undo says: %s", files_undo_text(&files) ? files_undo_text(&files) : "nothing");
	CHECK(files_undo(&files, message, sizeof(message)) && !memcmp(&patch->model.psg[1], &organ, sizeof(organ)), "undo puts back the envelope before the gesture: %s", message);
	CHECK(files_undo(&files, message, sizeof(message)) && !memcmp(&patch->model.psg[1], &drawn, sizeof(drawn)), "and again the drawing");

	// SAVE writes the drawing, and the file sounds the same on another channel.
	char eef_path[MAX_PATH];
	path_in(eef_path, sizeof(eef_path), temp_dir, "drawn.eef");
	CHECK(files_save(&files, eef_path, FILES_ENVELOPE, 7, message, sizeof(message)) && files_open(&files, eef_path, 9, message, sizeof(message))
		&& !memcmp(patch->model.psg[3].steps, drawn.steps, sizeof(drawn.steps)), "SAVE writes the drawing as EEF, dropped back it is the same: %s", message);

	// A project keeps it.
	size_t size;
	uint8_t *state = state_build(patch, &files_view, &size);
	patch_init(patch);
	state_view view = {0};
	bool knobs;
	CHECK(state && state_read(patch, state, size, &view, &knobs) && patch_take_incoming(patch) && !memcmp(&patch->model.psg[1], &drawn, sizeof(drawn)),
		"after a project is opened the drawing is there");
	free(state);
	// Stage 39: the ALL mask goes with the view whole, the DAC's live view (bit 6) with the FM rows' (0-5).
	for (int k = 0; k < 3; k++) {
		static const uint8_t MASKS[3] = {0x40, 0x7F, 0x05};
		state_view all = files_view;
		all.envelopes = MASKS[k];
		state = state_build(patch, &all, &size);
		state_view back = {0};
		CHECK(state && state_read(patch, state, size, &back, &knobs) && back.envelopes == MASKS[k], "the ALL mask 0x%02X comes back as 0x%02X", MASKS[k], back.envelopes);
		free(state);
	}
	// Stage 36 (D81): the ROM page's TV look goes with the view, the block's fourth byte; a value
	// no build writes reads as OFF.
	for (int look = 0; look < 4; look++) {
		state_view tv = files_view;
		tv.tv = (uint8_t)(look == 3 ? 7 : look);
		state = state_build(patch, &tv, &size);
		state_view back = {0};
		back.tv = 5;
		CHECK(state && state_read(patch, state, size, &back, &knobs) && back.tv == (look == 3 ? 0 : look), "the TV look %d comes back as %d", tv.tv, back.tv);
		free(state);
	}
	// The loops switched off in the editor go with it (D30); a state without them has none.
	state_view kept = files_view;
	kept.loops[2] = (state_loop){true, 5, -1, "Plain"};
	kept.loops[3] = (state_loop){true, 0, 7, "Drawn"};
	state = state_build(patch, &kept, &size);
	state_view opened = {0};
	opened.loops[0].kept = true;
	CHECK(state && state_read(patch, state, size, &opened, &knobs) && !opened.loops[0].kept && !opened.loops[1].kept
		&& opened.loops[2].kept && opened.loops[2].start == 5 && opened.loops[2].end == -1 && !strcmp(opened.loops[2].name, "Plain")
		&& opened.loops[3].kept && opened.loops[3].start == 0 && opened.loops[3].end == 7 && !strcmp(opened.loops[3].name, "Drawn"),
		"a project keeps the loops switched off, where they were");
	free(state);

	// Stage 22b (D54): with stop the window hands the envelope without its release, so the ROM, SAVE
	// and STORE get what plays; the view keeps the release, block 11.
	patch_shape cut;
	patch_shape_of(&drawn, &cut);
	const int released = cut.count - cut.loop_end;
	CHECK(cut.loop_start >= 0 && cut.loop_end >= 0 && released > 0, "the drawing has a loop and a release of %d steps", released);
	kept.tails[1] = (state_tail){.stop = true, .count = released};
	memcpy(kept.tails[1].steps, cut.frames + cut.loop_end, (size_t)released);
	snprintf(kept.tails[1].name, sizeof(kept.tails[1].name), "%s", drawn.name);
	cut.count = cut.loop_end;
	cut.loop_end = -1;
	patch_envelope stopped = drawn;
	patch_shape_to_envelope(&cut, &stopped);
	files_draw_envelope(&files, 7, &drawn, &stopped);
	clear();
	sync(patch);
	CHECK(stored_envelope(&program, &length) && length == 8 + 2 * (size_t)patch_envelope_length(&stopped) && patch_envelope_length(&stopped) == patch_envelope_length(&drawn) - released - 1,
		"stop sends the ROM the envelope without its release and its loop end mark (%d of %d bytes)", patch_envelope_length(&stopped), patch_envelope_length(&drawn));
	path_in(eef_path, sizeof(eef_path), temp_dir, "stopped.eef");
	uint8_t eef[PATCH_ENVELOPE_STEPS + 2] = {0};
	size_t eef_size = 0;
	if (files_save(&files, eef_path, FILES_ENVELOPE, 7, message, sizeof(message))) {
		FILE *f = fopen(eef_path, "rb");
		eef_size = f ? fread(eef, 1, sizeof(eef), f) : 0;
		if (f) {
			fclose(f);
		}
	}
	CHECK(eef_size == (size_t)patch_envelope_length(&stopped) + 1 && !memcmp(eef, stopped.steps, eef_size), "SAVE writes it as EEF, %zu bytes", eef_size);
	const int stored_place = patch->model.channels[7][PARAM_PRESET] & (PATCH_BANK_SIZE - 1);
	CHECK(files_store(&files, 7, message, sizeof(message)) && !memcmp(patch_bank_view(patch)->envelopes[stored_place].steps, stopped.steps, sizeof(stopped.steps)),
		"STORE puts it into the bank: %s", message);
	state = state_build(patch, &kept, &size);
	memset(&opened, 0, sizeof(opened));
	opened.tails[0].stop = true;
	CHECK(state && state_read(patch, state, size, &opened, &knobs) && !opened.tails[0].stop && opened.tails[1].stop && opened.tails[1].count == released
			&& !memcmp(opened.tails[1].steps, kept.tails[1].steps, (size_t)released) && !strcmp(opened.tails[1].name, drawn.name) && !opened.tails[2].stop,
		"a project keeps the release while the loop stops");
	// A project of 0.22.2, without block 11, opens as it did, with no release kept.
	size_t at = sizeof(uint32_t), cut_size = size;
	while (at + 8 <= cut_size) {
		uint32_t type, block_length;
		memcpy(&type, state + at, 4);
		memcpy(&block_length, state + at + 4, 4);
		if (type == 11) {
			memmove(state + at, state + at + 8 + block_length, cut_size - at - 8 - block_length);
			cut_size -= 8 + block_length;
			break;
		}
		at += 8 + block_length;
	}
	memset(&opened, 0, sizeof(opened));
	opened.tails[1].stop = true;
	CHECK(cut_size < size && state_read(patch, state, cut_size, &opened, &knobs) && !opened.tails[1].stop && opened.loops[2].kept && opened.loops[3].kept
			&& !memcmp(&patch->model.psg[1], &stopped, sizeof(stopped)),
		"a project without block 11 opens with its envelopes and loops, no release kept");
	free(state);
	files_free(&files);
}

// The rail's * (stage 9): a channel's sound against its bank place, the voice or the drawn shape.
static void test_sound_changed(void)
{
	printf("Sound changed against its bank place\n");
	static mdmi_patch patch;
	patch_init(&patch);
	int fm_changed = 0, psg_changed = 0;
	for (int place = 0; place < PATCH_BANK_SIZE; place++) {
		set(&patch, PRESET_ID(0), place);
		set(&patch, PRESET_ID(7), place);
		fm_changed += patch_sound_changed(&patch.model, NULL, 0);
		psg_changed += patch_sound_changed(&patch.model, &patch.model.psg[1], 7);
	}
	CHECK(fm_changed == 0 && psg_changed == 0, "no place of 128 is changed when chosen: FM %d, PSG %d", fm_changed, psg_changed);
	set(&patch, PRESET_ID(0), 5);
	set(&patch, PATCH_CHANNEL_ID(0, PARAM_VOLUME), 40);
	set(&patch, PATCH_CHANNEL_ID(0, PARAM_PAN), 0);
	CHECK(!patch_sound_changed(&patch.model, NULL, 0), "volume and pan don't count");
	set(&patch, op_id(0, 0, OP_TL), patch.model.channels[0][op_param(0, OP_TL)] + 1);
	CHECK(patch_sound_changed(&patch.model, NULL, 0), "TL one up does");
	midi(&patch, 0xC0, 5, 0);
	sync(&patch);
	CHECK(!patch_sound_changed(&patch.model, NULL, 0), "a program from a clip wipes it");
	patch_envelope drawn = patch.model.psg[1];
	patch_shape shape;
	patch_shape_of(&drawn, &shape);
	shape.frames[0] ^= 1;
	patch_shape_to_envelope(&shape, &drawn);
	CHECK(patch_sound_changed(&patch.model, &drawn, 7), "a drawn frame does");
	patch_envelope marked = patch.model.psg[1];
	const int length = patch_envelope_length(&marked);
	if (length + 1 < PATCH_ENVELOPE_STEPS) {
		// A loop end with no loop start: the ROM reads nothing of it, nor does the editor.
		memmove(marked.steps + 1, marked.steps, (size_t)length + 1);
		marked.steps[0] = PATCH_EEF_LOOP_END;
	}
	CHECK(!patch_sound_changed(&patch.model, &marked, 7), "a mark the editor drops doesn't");
}

// A state without one block type, as an older build wrote it.
static uint8_t *without_block(const uint8_t *state, size_t size, uint32_t type, size_t *out_size)
{
	uint8_t *out = malloc(size);
	memcpy(out, state, sizeof(uint32_t));
	size_t to = sizeof(uint32_t);
	for (size_t at = sizeof(uint32_t); at + 8 <= size;) {
		uint32_t kind, length;
		memcpy(&kind, state + at, sizeof(kind));
		memcpy(&length, state + at + 4, sizeof(length));
		if (kind != type) {
			memcpy(out + to, state + at, 8 + length);
			to += 8 + length;
		}
		at += 8 + length;
	}
	*out_size = to;
	return out;
}

// D31: a channel's base is what it got whole, its bank place or a dropped file. The rail's * and
// the window's labels compare with it, undo puts it back, and a project keeps it.
static void test_bases(void)
{
	printf("Bases\n");
	mdmi_patch *patch = &files_patch;
	patch_init(patch);
	files_view = (state_view){0};
	const files_host host = {patch, &files_view, take_now, NULL};
	static mdmi_files files;
	files_init(&files, &host);
	char message[FILES_MESSAGE];
	const int tl = op_param(0, OP_TL);
	CHECK(open_file(&files, files_dir, "Duck Guitar.tfi", 3, -1, message) && !patch_sound_changed(&patch->model, NULL, 3), "a dropped file is its own base: %s", message);
	const int file_tl = patch->model.channels[3][tl];
	CHECK(patch_base_value(&patch->model, 3, tl) == file_tl, "the base holds the file's TL");
	patch_set(patch, op_id(3, 0, OP_TL), file_tl + 5);
	patch_publish(patch);
	CHECK(patch_sound_changed(&patch->model, NULL, 3) && patch_base_value(&patch->model, 3, tl) == file_tl, "a tweak changes the sound, not the base");
	CHECK(patch_base_value(&patch->model, 3, PARAM_VOLUME) == -1 && patch_base_value(&patch->model, 7, PARAM_ALGORITHM) == -1, "volume and PSG channels have no base value");

	size_t size;
	uint8_t *state = state_build(patch, &files_view, &size);
	static mdmi_patch other;
	patch_init(&other);
	state_view view = {0};
	bool knobs;
	CHECK(state && state_read(&other, state, size, &view, &knobs) && patch_take_incoming(&other) && !memcmp(other.model.base, patch->model.base, sizeof(patch->model.base))
		&& !memcmp(other.model.psg_base, patch->model.psg_base, sizeof(patch->model.psg_base)), "a project keeps the bases");
	free(state);

	CHECK(files_undo(&files, message, sizeof(message)) && !patch_sound_changed(&patch->model, NULL, 3) && !memcmp(patch->model.base[3], patch->bank.places[0].voice, sizeof(patch->model.base[3])),
		"undo puts the place back as base");
	CHECK(files_undo(&files, message, sizeof(message)) && patch_sound_changed(&patch->model, NULL, 3) && patch_base_value(&patch->model, 3, tl) == file_tl, "a second undo brings the file back as base, tweak and all");

	patch_set(patch, PATCH_CHANNEL_ID(3, PARAM_PRESET), 9);
	patch_publish(patch);
	CHECK(!patch_sound_changed(&patch->model, NULL, 3) && !memcmp(patch->model.base[3], patch->bank.places[9].voice, sizeof(patch->model.base[3])), "a preset step makes the place the base");

	const int place = patch->model.channels[7][PARAM_PRESET];
	patch_envelope drawn = patch->model.psg[1];
	patch_shape shape;
	patch_shape_of(&drawn, &shape);
	shape.frames[0] ^= 1;
	patch_shape_to_envelope(&shape, &drawn);
	const patch_envelope before = patch->model.psg[1];
	files_draw_envelope(&files, 7, &before, &drawn);
	CHECK(patch_sound_changed(&patch->model, &patch->model.psg[1], 7) && !patch_sound_changed(&patch->model, &patch->bank.envelopes[place], 7), "a drawing keeps the place as base");
	CHECK(files_undo(&files, message, sizeof(message)) && !patch_sound_changed(&patch->model, &patch->model.psg[1], 7), "undo of a drawing is the place again");

	// A project of an older build: a channel named as its place has the place as base, a file is its own.
	open_file(&files, files_dir, "Duck Guitar.tfi", 3, -1, message);
	patch_set(patch, op_id(0, 0, OP_TL), patch->model.channels[0][tl] + 3);
	patch_publish(patch);
	state = state_build(patch, &files_view, &size);
	size_t old_size;
	uint8_t *old = without_block(state, size, 8, &old_size);
	patch_init(&other);
	CHECK(old_size < size && state_read(&other, old, old_size, &view, &knobs) && patch_take_incoming(&other) && patch_sound_changed(&other.model, NULL, 0) && !patch_sound_changed(&other.model, NULL, 3)
		&& !patch_sound_changed(&other.model, &other.model.psg[1], 7), "an older project: the tweak shows, the file doesn't");
	free(old);
	free(state);
	files_free(&files);
}

// Stage 20: arp rows. The SysEx 0A of a row type (02 FM, 03 PSG) under a program that went out
// last, with its length, or NULL.
static const uint8_t *sent_row(uint8_t type, int program, size_t *length)
{
	const uint8_t *found = NULL;
	for (size_t i = 0; i + 7 < sent_bytes; i++) {
		if (sent[i] == 0xF0 && sent[i + 1] == 0x00 && sent[i + 4] == 0x0A && sent[i + 5] == type && sent[i + 6] == program) {
			const uint8_t *end = memchr(sent + i, 0xF7, sent_bytes - i);
			*length = (size_t)(end - (sent + i)) + 1;
			found = sent + i;
		}
	}
	return found;
}

// The program of the last row of a type that went out, or -1.
static int row_program(uint8_t type)
{
	int program = -1;
	for (size_t i = 0; i + 7 < sent_bytes; i++) {
		if (sent[i] == 0xF0 && sent[i + 1] == 0x00 && sent[i + 4] == 0x0A && sent[i + 5] == type) {
			program = sent[i + 6];
		}
	}
	return program;
}

static patch_arp make_arp(int count, int loop, const int *shifts)
{
	patch_arp arp = PATCH_ARP_EMPTY;
	arp.count = (uint8_t)count;
	arp.loop = (int8_t)loop;
	for (int i = 0; i < count; i++) {
		arp.steps[i] = (int8_t)shifts[i];
	}
	return arp;
}

static bool same_rows(const patch_arp *a, const patch_arp *b, int count)
{
	return !memcmp(a, b, (size_t)count * sizeof(patch_arp));
}

static bool all_empty(const patch_arp *rows, int count)
{
	const patch_arp none = PATCH_ARP_EMPTY;
	for (int i = 0; i < count; i++) {
		if (memcmp(&rows[i], &none, sizeof(none))) {
			return false;
		}
	}
	return true;
}

static void test_arps(void)
{
	printf("Arp rows\n");
	mdmi_patch *patch = &files_patch;
	patch_init(patch);
	files_view = (state_view){0};
	const files_host host = {patch, &files_view, take_now, NULL};
	static mdmi_files files;
	files_init(&files, &host);
	char message[FILES_MESSAGE];
	size_t length;
	const uint8_t *row;

	// Two GM places with rows: a major chord looped, and the longest row there is.
	static const int MAJOR[3] = {0, 4, 7};
	int shifts[PATCH_ARP_STEPS];
	for (int i = 0; i < PATCH_ARP_STEPS; i++) {
		shifts[i] = i * 7 % 49 - PATCH_ARP_SHIFT;
	}
	const patch_arp chord = make_arp(3, 0, MAJOR), longest = make_arp(PATCH_ARP_STEPS, 5, shifts);
	patch_instrument place = patch_bank_view(patch)->places[10];
	patch_load_place(patch, 10, &place, &chord);
	place = patch_bank_view(patch)->places[20];
	patch_load_place(patch, 20, &place, &longest);
	patch_take_incoming(patch);
	clear();
	sync(patch);
	CHECK(patch_place_is_gm(&patch->bank, 10) && patch_place_is_gm(&patch->bank, 20) && sent_bytes == 0, "places 11 and 21 keep GM with their rows, which aren't sent before use");

	// A Preset lane between them: each row goes once, at its first use, then a step is a PC only.
	clear();
	set(patch, PRESET_ID(0), 10);
	row = sent_row(0x02, 10, &length);
	CHECK(last_program(0) == 10 && count_stored(0x02) == 1 && count_stored(0x00) == 0 && row && length == 12 && row[7] == 0 && row[8] == 64 && row[9] == 68 && row[10] == 71 && row[11] == 0xF7,
		"first step to 11: the PC and its row, 0A 02 10 loop 0, steps +64: %d bytes", (int)sent_bytes);
	clear();
	set(patch, PRESET_ID(0), 20);
	row = sent_row(0x02, 20, &length);
	CHECK(last_program(0) == 20 && count_stored(0x02) == 1 && row && length == 41 && row[7] == 5 && row[8] == 64 - PATCH_ARP_SHIFT, "first step to 21: the longest row is one SysEx of %d bytes", row ? (int)length : 0);
	bool only_pc = true;
	for (int pass = 0; pass < 3; pass++) {
		for (int step = 0; step < 2; step++) {
			const int to = step ? 20 : 10;
			clear();
			set(patch, PRESET_ID(0), to);
			only_pc = only_pc && sent_bytes == 2 && sent[0] == 0xC0 && sent[1] == to;
		}
	}
	CHECK(only_pc, "after the first pass each step sends only its program change, as without rows");

	// A row SysEx from a clip goes nowhere.
	clear();
	const uint8_t from_clip[2][10] = {{0xF0, 0x00, 0x22, 0x77, 0x0A, 0x02, 0x05, 0x7F, 0x40, 0xF7}, {0xF0, 0x00, 0x22, 0x77, 0x0A, 0x03, 0x05, 0x7F, 0x40, 0xF7}};
	patch_midi(patch, from_clip[0], sizeof(from_clip[0]), record, NULL);
	patch_midi(patch, from_clip[1], sizeof(from_clip[1]), record, NULL);
	CHECK(sent_bytes == 0, "0A 02 and 0A 03 from a clip are dropped");

	// Channel 2 on Grand Piano draws a row of its own: a place's number holds the place's row, so it
	// takes a service number with the whole voice.
	const patch_arp none = PATCH_ARP_EMPTY;
	files_draw_arp(&files, 1, &none, &chord);
	clear();
	sync(patch);
	const int service = last_program(1);
	CHECK(service >= 116 && count_stored(0x00) == 1 && row_program(0x02) == service && patch_sound_changed(&patch->model, NULL, 1) && files_can_store(&files, 1),
		"a channel with its own row on a GM place goes to service number %d with its voice", service);
	static const int UP[2] = {0, 12};
	const patch_arp octave = make_arp(2, 0, UP);
	files_draw_arp(&files, 1, NULL, &octave);
	clear();
	sync(patch);
	CHECK(sent_bytes == 11 && count_stored(0x02) == 1 && row_program(0x02) == service, "drawing on, it sends only 0A 02 under the same number: %d bytes", (int)sent_bytes);
	CHECK(files_undo_text(&files) && !strcmp(files_undo_text(&files), "CH2 arp before drawing"), "undo says: %s", files_undo_text(&files) ? files_undo_text(&files) : "nothing");

	// STORE: the channel's sound and row into place 1, and the channel back on number 0.
	CHECK(files_store(&files, 1, message, sizeof(message)) && !strcmp(message, "CH2 stored in bank 1"), "STORE: %s", message);
	clear();
	sync(patch);
	row = sent_row(0x02, 0, &length);
	CHECK(last_program(1) == 0 && patch->rom.programs[1] == 0 && count_stored(0x00) == 0 && row && length == 11, "the channel is back on the place's number, with the row under it");
	CHECK(patch_arp_same(&patch->bank.place_arps[0], &octave) && patch_arp_same(&patch_bank_view(patch)->place_arps[0], &octave) && patch_place_is_gm(&patch->bank, 0),
		"place 1 has the row, its voice is still GM");
	CHECK(!patch_sound_changed(&patch->model, NULL, 1) && !files_can_store(&files, 1) && !files_store(&files, 1, message, sizeof(message)), "the * is out and STORE has nothing to do: %s", message);
	CHECK(patch_arp_same(&patch->model.arps[2], &octave), "channel 3, which played place 1 as it was, follows it");
	CHECK(files_undo_text(&files) && !strcmp(files_undo_text(&files), "slot 1 back to Grand Piano"), "undo says: %s", files_undo_text(&files) ? files_undo_text(&files) : "nothing");
	CHECK(files_undo(&files, message, sizeof(message)) && patch->bank.place_arps[0].count == 0 && patch_arp_same(&patch->model.arps[1], &octave) && patch_sound_changed(&patch->model, NULL, 1)
		&& patch->model.arps[2].count == 0, "undo of STORE: the place's row goes, the channel keeps its own with the * again");
	clear();
	sync(patch);
	row = sent_row(0x02, 0, &length);
	CHECK(last_program(1) >= 116 && row && length == 9 && row[7] == 0x7F, "back on a service number; number 0 gets its empty row again as 0A 02 00 7F F7");
	CHECK(files_undo(&files, message, sizeof(message)) && patch_arp_same(&patch->bank.place_arps[0], &octave) && !patch_sound_changed(&patch->model, NULL, 1), "a second undo stores it again");
	clear();
	sync(patch);
	CHECK(last_program(1) == 0, "and the channel is on number 0 again");

	// Undo of a drawing; a dropped file has no row.
	files_draw_arp(&files, 1, &octave, &chord);
	CHECK(files_undo(&files, message, sizeof(message)) && patch_arp_same(&patch->model.arps[1], &octave), "undo of a drawing puts the row before the gesture back");
	// A knob moved after the drawing stays when the drawing is undone (review of stage 20).
	files_draw_arp(&files, 1, &octave, &chord);
	// Op1 TL of channel 2 (1411 is channel 4's, patch.h).
	const uint32_t tl = 1111;
	const int tl_was = patch_get(patch, tl);
	patch_set(patch, tl, tl_was + 7);
	CHECK(files_undo(&files, message, sizeof(message)) && patch_arp_same(&patch->model.arps[1], &octave) && patch_get(patch, tl) == tl_was + 7,
		"undo of a drawing on FM puts back only the row, a knob moved since stays (TL %d)", patch_get(patch, tl));
	CHECK(files_undo(&files, message, sizeof(message)) && patch_arp_same(&patch->model.arps[1], &chord), "a second undo brings the drawing back");
	files_undo(&files, message, sizeof(message));
	CHECK(open_file(&files, files_dir, "Duck Guitar.tfi", 1, -1, message) && patch->model.arps[1].count == 0 && patch->model.arp_bases[1].count == 0 && !patch_sound_changed(&patch->model, NULL, 1),
		"a dropped TFI brings an empty row, its base too: %s", message);
	CHECK(files_undo(&files, message, sizeof(message)) && patch_arp_same(&patch->model.arps[1], &octave) && patch_arp_same(&patch->model.arp_bases[1], &octave), "undo gives the row and base row back");

	// PSG: channel 8 with a row of its own goes to a service number with its envelope, then only 0A 03.
	files_draw_arp(&files, 7, &none, &chord);
	clear();
	sync(patch);
	const int psg_service = last_program(7);
	// The service numbers hold the ROM's Plain since the reset, so the envelope needs no 0A 01 here.
	CHECK(psg_service >= 120 && row_program(0x03) == psg_service && !memcmp(patch->rom.psg_stored[psg_service - 120], patch->model.psg[1].steps, PATCH_ENVELOPE_STEPS + 1),
		"a PSG channel with its own row takes service number %d, which holds its envelope", psg_service);
	files_draw_arp(&files, 7, NULL, &octave);
	clear();
	sync(patch);
	CHECK(sent_bytes == 11 && count_stored(0x03) == 1 && row_program(0x03) == psg_service, "drawing on, it sends only 0A 03: %d bytes", (int)sent_bytes);
	CHECK(files_store(&files, 7, message, sizeof(message)), "STORE on channel 8: %s", message);
	clear();
	sync(patch);
	CHECK(last_program(7) == 0 && count_stored(0x01) == 0 && sent_row(0x03, 0, &length) && patch_arp_same(&patch->bank.envelope_arps[0], &octave) && !patch_sound_changed(&patch->model, &patch->model.psg[1], 7),
		"STORE puts it back on the place's number, the ROM's envelope with the row");
	open_file(&files, files_dir, "PSG Organ.eef", 8, -1, message);
	CHECK(patch->model.arps[8].count == 0 && patch->model.arp_bases[8].count == 0, "a dropped EEF brings an empty row");
	files_undo(&files, message, sizeof(message));

	// Stage 22b (D55): a row switched off plays and compares as empty and keeps its steps. Bit 1 of
	// the row's third byte holds it.
	patch_arp chord_off = chord;
	chord_off.off = 1;
	CHECK(((const uint8_t *)&chord_off)[2] == 2 && patch_arp_same(&chord_off, &none) && !patch_arp_same(&chord_off, &chord) && patch_arp_played(&chord_off)->count == 0,
		"the switch is bit 1 of the third byte (%d); an off row is the same as an empty one", ((const uint8_t *)&chord_off)[2]);
	patch_arp cleaned = none;
	cleaned.off = 1;
	patch_arp_clean(&cleaned);
	patch_arp spare = chord;
	((uint8_t *)&spare)[2] = 0xFD;
	patch_arp_clean(&spare);
	CHECK(!cleaned.off && ((const uint8_t *)&spare)[2] == 0 && spare.count == 3, "an empty row is never off; a loop's row keeps neither a shadow nor the other bits");
	// Channel 5 on place 31, which has no row: its own row switched off keeps it on the place's number.
	set(patch, PRESET_ID(4), 30);
	clear();
	sync(patch);
	files_draw_arp(&files, 4, &none, &chord_off);
	clear();
	sync(patch);
	CHECK(patch->rom.programs[4] == 30 && sent_bytes == 0 && !patch_sound_changed(&patch->model, NULL, 4) && !files_can_store(&files, 4) && patch->model.arps[4].count == 3 && patch->model.arps[4].off,
		"channel 5 with its row off stays on number 30 (%d), sends nothing (%d bytes), no * and nothing to STORE; the steps are kept", patch->rom.programs[4], (int)sent_bytes);
	// Switched on: its own row takes a service number with it; undo switches it off again.
	files_draw_arp(&files, 4, &chord_off, &chord);
	clear();
	sync(patch);
	const int on_service = last_program(4);
	row = sent_row(0x02, on_service, &length);
	CHECK(on_service >= 116 && row && length == 12 && row[8] == 64 && row[9] == 68 && row[10] == 71 && patch_sound_changed(&patch->model, NULL, 4), "switched on, the row goes under service number %d before the next note", on_service);
	CHECK(files_undo(&files, message, sizeof(message)) && !memcmp(&patch->model.arps[4], &chord_off, sizeof(chord_off)), "one undo switches it off again with its steps");
	clear();
	sync(patch);
	CHECK(patch->rom.programs[4] == on_service && patch->rom.arps[0][on_service].count == 0, "and the service number it stays on gets 0 steps");
	// On place 11, whose row is the chord: switched off, it takes a service number with 0 steps.
	set(patch, PRESET_ID(4), 10);
	clear();
	sync(patch);
	files_draw_arp(&files, 4, &chord, &chord_off);
	clear();
	sync(patch);
	const int off_service = last_program(4);
	row = sent_row(0x02, off_service, &length);
	CHECK(off_service >= 116 && (!row || (length == 9 && row[7] == 0x7F)) && patch->rom.arps[0][off_service].count == 0 && patch_sound_changed(&patch->model, NULL, 4) && files_can_store(&files, 4),
		"switched off on a place with a row: service number %d, which holds 0 steps, the * on", off_service);
	// STORE takes the switch into the place: the channel is back on 10, which now holds no steps.
	CHECK(files_store(&files, 4, message, sizeof(message)) && patch->bank.place_arps[10].off && patch->bank.place_arps[10].count == 3, "STORE puts the row off into place 11: %s", message);
	clear();
	sync(patch);
	row = sent_row(0x02, 10, &length);
	CHECK(last_program(4) == 10 && row && length == 9 && row[7] == 0x7F && !patch_sound_changed(&patch->model, NULL, 4), "the channel is on number 10 again, which gets 0 steps");
	CHECK(files_undo(&files, message, sizeof(message)) && !patch->bank.place_arps[10].off && patch->model.arps[4].off && patch->model.arps[4].count == 3,
		"undo of STORE: the place's row is on again, the channel's stays off");

	// A project, a bank file and a snapshot keep the rows and a loop's shadow.
	patch_arp shadowed = make_arp(4, -1, shifts);
	shadowed.shadow_kept = 1;
	shadowed.shadow = 2;
	files_draw_arp(&files, 3, &none, &shadowed);
	place = patch_bank_view(patch)->places[50];
	patch_load_place(patch, 50, &place, &shadowed);
	patch_envelope envelope = patch_bank_view(patch)->envelopes[60];
	patch_load_envelope_place(patch, 60, &envelope, &longest);
	// Stage 22b: rows switched off, on channel 5 (above), on place 52 and on PSG place 62.
	place = patch_bank_view(patch)->places[51];
	patch_load_place(patch, 51, &place, &chord_off);
	envelope = patch_bank_view(patch)->envelopes[61];
	patch_load_envelope_place(patch, 61, &envelope, &chord_off);
	patch_take_incoming(patch);
	CHECK(patch->model.arps[3].shadow_kept && patch->model.arps[3].shadow == 2 && patch->bank.place_arps[50].shadow_kept, "a row keeps its loop's shadow");
	CHECK(patch->model.arps[4].off && patch->bank.place_arps[51].off && patch->bank.envelope_arps[61].off && !patch->model.arps[3].off, "rows switched off on a channel and two places");
	static patch_values saved;
	static patch_bank saved_bank;
	saved = patch->model;
	saved_bank = patch->bank;
	size_t size;
	uint8_t *state = state_build(patch, &files_view, &size);
	static mdmi_patch other;
	patch_init(&other);
	state_view view = {0};
	bool knobs;
	CHECK(state && state_read(&other, state, size, &view, &knobs) && patch_take_incoming(&other) && same_rows(other.model.arps, saved.arps, PATCH_NUM_CHANNELS)
		&& same_rows(other.model.arp_bases, saved.arp_bases, PATCH_NUM_CHANNELS) && same_rows(other.bank.place_arps, saved_bank.place_arps, PATCH_BANK_SIZE)
		&& same_rows(other.bank.envelope_arps, saved_bank.envelope_arps, PATCH_BANK_SIZE) && other.model.arps[3].shadow_kept && other.model.arps[3].shadow == 2,
		"a project reads back with every row and the shadow");
	// The third byte as 0.22 wrote it (1 for a kept shadow) reads as a row switched on.
	CHECK(other.model.arps[4].off && other.model.arps[4].count == 3 && other.bank.place_arps[51].off && other.bank.envelope_arps[61].off && !other.model.arps[3].off && !other.model.arps[5].off,
		"block 10 keeps the switch, its length the same; a row of 0.22 is on");
	size_t old_size;
	uint8_t *old = without_block(state, size, 10, &old_size);
	patch_init(&other);
	CHECK(old_size + 8 + 276 * sizeof(patch_arp) == size && state_read(&other, old, old_size, &view, &knobs) && patch_take_incoming(&other) && all_empty(other.model.arps, PATCH_NUM_CHANNELS)
		&& all_empty(other.model.arp_bases, PATCH_NUM_CHANNELS) && all_empty(other.bank.place_arps, PATCH_BANK_SIZE) && all_empty(other.bank.envelope_arps, PATCH_BANK_SIZE)
		&& !memcmp(other.model.channels, saved.channels, sizeof(saved.channels)),
		"a project of 0.19, without block 10, opens with every row empty");
	free(old);
	free(state);

	char bank_path[MAX_PATH], snap_path[MAX_PATH];
	path_in(bank_path, sizeof(bank_path), temp_dir, "arps.mdbank");
	path_in(snap_path, sizeof(snap_path), temp_dir, "arps.mdsnap");
	CHECK(files_save(&files, bank_path, FILES_BANK, 0, message, sizeof(message)) && files_save(&files, snap_path, FILES_SNAPSHOT, 0, message, sizeof(message)), "bank and snapshot saved: %s", message);
	patch_init(patch);
	CHECK(files_open(&files, bank_path, 0, message, sizeof(message)) && same_rows(patch->bank.place_arps, saved_bank.place_arps, PATCH_BANK_SIZE)
		&& same_rows(patch->bank.envelope_arps, saved_bank.envelope_arps, PATCH_BANK_SIZE) && patch_arp_same(&patch->model.arps[5], &octave) && !patch->model.arps[3].shadow_kept,
		"an .mdbank brings the places' rows, not the channels' own; a channel on place 1 follows it: %s", message);
	CHECK(patch->bank.place_arps[51].off && patch->bank.envelope_arps[61].off && patch->bank.place_arps[51].count == 3 && !patch->model.arps[4].off, "the .mdbank keeps the places' switches");
	patch_init(patch);
	CHECK(files_open(&files, snap_path, 0, message, sizeof(message)) && same_rows(patch->model.arps, saved.arps, PATCH_NUM_CHANNELS) && same_rows(patch->model.arp_bases, saved.arp_bases, PATCH_NUM_CHANNELS)
		&& same_rows(patch->bank.place_arps, saved_bank.place_arps, PATCH_BANK_SIZE), "an .mdsnap brings every row: %s", message);
	CHECK(patch->model.arps[4].off && patch->model.arps[4].count == 3 && patch->bank.place_arps[51].off, "the .mdsnap keeps the switches");
	files_free(&files);
}

// The ROM's own walk of an envelope, byte by byte, as midi_psg.c does it: apply_envelope_step,
// increment_envelope_step once a frame, midi_psg_note_off.
typedef struct {
	const uint8_t *steps;
	int at;
	int loop;
	bool released;
	bool on;
} rom_walk;

static void walk_apply(rom_walk *w)
{
	if (w->steps[w->at] == PATCH_EEF_LOOP_START) {
		w->at++;
		w->loop = w->at;
	}
	if (w->steps[w->at] == PATCH_EEF_LOOP_END && w->loop >= 0) {
		w->at++;
		if (!w->released) {
			w->at = w->loop;
		}
	}
	if (w->steps[w->at] == PATCH_EEF_END) {
		if (w->loop >= 0 && !w->released) {
			w->at = w->loop;
		} else {
			w->on = false;
		}
	}
}

static void walk_note_on(rom_walk *w, const uint8_t *steps)
{
	*w = (rom_walk){steps, 0, -1, false, true};
	walk_apply(w);
}

static void walk_tick(rom_walk *w)
{
	if (!w->on) {
		return;
	}
	if (w->steps[w->at] != PATCH_EEF_END) {
		w->at++;
	}
	walk_apply(w);
}

// As midi_psg_note_off since stage 22b (D54): a loop with steps after its end releases, in its
// intro too; anything else stops at once.
static void walk_note_off(rom_walk *w)
{
	bool looped = false, release = false;
	for (int i = 0; w->steps[i] != PATCH_EEF_END; i++) {
		if (w->steps[i] == PATCH_EEF_LOOP_START) {
			looped = true;
		} else if (w->steps[i] == PATCH_EEF_LOOP_END && looped) {
			release = w->steps[i + 1] != PATCH_EEF_END;
			break;
		}
	}
	if (release) {
		w->released = true;
	} else {
		w->on = false;
	}
}

// The frame the walk is on, counting frames, not marks.
static int walk_frame(const rom_walk *w)
{
	if (!w->on) {
		return -1;
	}
	int frame = 0;
	for (int i = 0; i < w->at; i++) {
		frame += w->steps[i] != PATCH_EEF_LOOP_START && w->steps[i] != PATCH_EEF_LOOP_END;
	}
	return frame;
}

// Stage 10: the cursor on ALL is the plugin's own reckoning (D29). It must be where the ROM's walk is.
static void test_cursor(void)
{
	printf("Envelope cursor against the ROM's walk\n");
	srand(10);
	patch_envelope base;
	patch_envelope_from_eef(&base, (const uint8_t[]){0x00, 0xFF}, 2);
	int checked = 0, wrong = 0;
	char first[160] = "";
	for (int n = 0; n < 3000; n++) {
		patch_shape shape;
		shape.count = 1 + rand() % 40;
		shape.loop_start = rand() % 3 == 0 ? -1 : rand() % shape.count;
		shape.loop_end = rand() % 3 == 0 ? -1 : rand() % (shape.count + 1);
		for (int f = 0; f < PATCH_ENVELOPE_STEPS; f++) {
			shape.frames[f] = (uint8_t)(rand() % 0xF0);
		}
		patch_envelope envelope = base;
		patch_shape_to_envelope(&shape, &envelope);
		patch_shape drawn;
		patch_shape_of(&envelope, &drawn);
		const int length = drawn.count;
		for (int off = 0; off < 3 * length + 3; off++) {
			rom_walk w;
			walk_note_on(&w, envelope.steps);
			for (int t = 0; t < off; t++) {
				walk_tick(&w);
			}
			for (int k = 0; k < length + 4; k++) {
				const bool held = k == 0;
				const int expected = held ? walk_frame(&w) : 0;
				if (held) {
					const int got = patch_shape_position(&drawn, off, -1);
					checked++;
					if (got != expected && !wrong++) {
						snprintf(first, sizeof(first), "held %d frames of %d, loop %d-%d: %d, ROM %d", off, length, drawn.loop_start, drawn.loop_end, got, expected);
					}
					walk_note_off(&w);
				} else {
					walk_tick(&w);
				}
				const int rom = walk_frame(&w);
				const int got = patch_shape_position(&drawn, off, held ? 0 : k);
				checked++;
				if (got != rom && !wrong++) {
					snprintf(first, sizeof(first), "off after %d frames of %d, loop %d-%d, %d later: %d, ROM %d", off, length, drawn.loop_start, drawn.loop_end, held ? 0 : k, got, rom);
				}
			}
		}
	}
	CHECK(wrong == 0, "%d positions on 3000 random envelopes, %d wrong%s%s", checked, wrong, wrong ? ", first: " : "", first);
	patch_shape organ = {{0}, 11, 3, 4};
	CHECK(patch_shape_position(&organ, 2, -1) == 2 && patch_shape_position(&organ, 7, -1) == 3 && patch_shape_position(&organ, 7, 2) == 5 && patch_shape_position(&organ, 7, 8) == -1,
		"PSG Organ: frame 3, round frame 4 while held, on after Note Off, silent past the end");
	// Stage 22b (D54): a loop with a release releases in its intro too; one without stops at once.
	patch_shape no_release = {{0}, 11, 3, -1};
	CHECK(patch_shape_position(&organ, 2, 0) == 2 && patch_shape_position(&organ, 2, 3) == 5 && patch_shape_position(&no_release, 7, 0) == -1 && patch_shape_position(&no_release, 2, 0) == -1,
		"Note Off in the intro plays on through the loop once and the release; a loop without a release is silent at once, as the ROM does");
}

// Stage 11: the FM graph's envelope, against hardware-specialist's facts about the chip.
#define FM_COLUMNS 280
static int fm_column(double seconds)
{
	return (int)(FM_COLUMNS * sqrt(seconds / PATCH_FM_SECONDS));
}

static void fm_curve(int tl, int ar, int dr, int sl, int sr, int rr, int rs, int ssg, uint16_t *low, uint16_t *high)
{
	// patch.h's order: TL MUL DT RS AR DR SR SL RR AM SSG.
	const int16_t op[PATCH_OP_PARAMS] = {(int16_t)tl, 1, 0, (int16_t)rs, (int16_t)ar, (int16_t)dr, (int16_t)sr, (int16_t)sl, (int16_t)rr, 0, (int16_t)ssg};
	patch_fm_envelope(op, 16, FM_COLUMNS, low, high);
}

static void test_fm_envelope(void)
{
	uint16_t low[FM_COLUMNS], high[FM_COLUMNS], low2[FM_COLUMNS], high2[FM_COLUMNS];
	fm_curve(20, 31, 31, 15, 0, 15, 0, 0, low, high);
	CHECK(low[0] == 160 && low[fm_column(0.010)] == PATCH_FM_SILENT,
		"AR 31 is loud at once, down by TL 20 (%d steps), DR 31 is silent within 10 ms (%d)", low[0], low[fm_column(0.010)]);
	fm_curve(0, 20, 6, 8, 3, 5, 1, 0, low, high);
	bool falls = true;
	int peak = 0;
	for (int c = 1; c < FM_COLUMNS; c++) {
		peak = low[c] < low[peak] ? c : peak;
	}
	for (int c = peak + 1; c < FM_COLUMNS; c++) {
		falls = falls && low[c] >= low[c - 1];
	}
	CHECK(low[0] > 900 && low[peak] == 0 && peak > 0 && falls, "without SSG an attack rises to 0 dB (column %d) and the rest only falls", peak);
	fm_curve(0, 31, 4, 15, 0, 5, 0, 0, low, high);
	fm_curve(0, 31, 4, 15, 31, 5, 0, 0, low2, high2);
	CHECK(!memcmp(low, low2, sizeof(low)), "SL 15 is silence: SR never comes");
	fm_curve(0, 31, 0, 0, 6, 5, 0, 0, low, high);
	fm_curve(0, 31, 31, 0, 6, 5, 0, 0, low2, high2);
	CHECK(!memcmp(low, low2, sizeof(low)), "SL 0 skips DR: SR falls from the top");
	fm_curve(0, 31, 4, 15, 0, 5, 0, 0, low, high);
	fm_curve(0, 31, 4, 15, 0, 5, 3, 0, low2, high2);
	const int at = fm_column(0.3);
	CHECK(low2[at] > low[at], "RS 3 makes the same DR faster at C4 (%d against %d at 0.3 s)", low2[at], low[at]);
	// SSG 8: DR to 48 dB and again, fast enough that a column spans its whole range.
	fm_curve(0, 31, 28, 15, 0, 5, 0, 8, low, high);
	const int middle = fm_column(0.5);
	CHECK(low[middle] == 0 && high[middle] >= 500 && high[middle] <= 512, "SSG 8 repeats between 0 and 48 dB (%d to %d at 0.5 s)", low[middle], high[middle]);
	// SSG 10 with SL 11 and SR 0 stops at 33 dB and never turns over (hardware-specialist's trap).
	fm_curve(0, 31, 20, 11, 0, 5, 0, 10, low, high);
	CHECK(low[middle] == 352 && high[middle] == 352, "SSG 10 with SR 0 holds at SL, no triangle (%d)", low[middle]);
	// SSG 11 holds loud after its first fall; 9 goes silent.
	fm_curve(0, 31, 20, 15, 0, 5, 0, 11, low, high);
	fm_curve(0, 31, 20, 15, 0, 5, 0, 9, low2, high2);
	CHECK(low[middle] == 0 && high[middle] == 0 && low2[middle] == PATCH_FM_SILENT, "SSG 11 holds at 0 dB, SSG 9 goes silent");
	// After Note Off SSG falls 4 times as fast to 48 dB: 9 then goes silent, 11 stays there (BlastEm).
	fm_curve(0, 31, 4, 15, 0, 5, 0, 9, low, high);
	fm_curve(0, 31, 4, 15, 0, 5, 0, 11, low2, high2);
	CHECK(low[fm_column(0.9)] < 512 && low[fm_column(1.9)] == PATCH_FM_SILENT && low2[fm_column(1.9)] == 512,
		"after Note Off SSG 9 goes silent at 48 dB (%d), SSG 11 stays there (%d)", low[fm_column(1.9)], low2[fm_column(1.9)]);
	// Release: RR 15 goes silent soon after Note Off at 1 s.
	fm_curve(0, 31, 0, 0, 0, 15, 0, 0, low, high);
	CHECK(low[fm_column(0.9)] == 0 && low[fm_column(1.05)] == PATCH_FM_SILENT, "DR 0 holds the top until Note Off, RR 15 closes in 50 ms");
}

// A WAV of a sine, as 8, 16 or 24-bit PCM or 32-bit float.
static uint8_t *sine_wav(uint32_t rate, int channels, int bits, bool is_float, double hz, double seconds, double level, size_t *size)
{
	const uint32_t frames = (uint32_t)(rate * seconds), frame_bytes = (uint32_t)(channels * bits / 8);
	*size = 44 + (size_t)frames * frame_bytes;
	uint8_t *wav = calloc(1, *size);
	uint32_t u;
	uint16_t h;
	memcpy(wav, "RIFF", 4);
	u = (uint32_t)*size - 8;
	memcpy(wav + 4, &u, 4);
	memcpy(wav + 8, "WAVEfmt ", 8);
	u = 16;
	memcpy(wav + 16, &u, 4);
	h = is_float ? 3 : 1;
	memcpy(wav + 20, &h, 2);
	h = (uint16_t)channels;
	memcpy(wav + 22, &h, 2);
	memcpy(wav + 24, &rate, 4);
	u = rate * frame_bytes;
	memcpy(wav + 28, &u, 4);
	h = (uint16_t)frame_bytes;
	memcpy(wav + 32, &h, 2);
	h = (uint16_t)bits;
	memcpy(wav + 34, &h, 2);
	memcpy(wav + 36, "data", 4);
	u = frames * frame_bytes;
	memcpy(wav + 40, &u, 4);
	for (uint32_t i = 0; i < frames; i++) {
		const double v = level * sin(2 * 3.14159265358979 * hz * i / rate);
		for (int c = 0; c < channels; c++) {
			uint8_t *at = wav + 44 + i * frame_bytes + c * bits / 8;
			if (is_float) {
				const float x = (float)v;
				memcpy(at, &x, 4);
			} else if (bits == 8) {
				at[0] = (uint8_t)lround(128 + v * 127);
			} else if (bits == 16) {
				const int16_t x = (int16_t)lround(v * 32767);
				memcpy(at, &x, 2);
			} else {
				const int32_t x = (int32_t)lround(v * 8388607);
				memcpy(at, &x, 3);
			}
		}
	}
	return wav;
}

// The frequency of a converted sine by its rising zero crossings, and its peak.
static double sine_hz(const dac_sample *s, int *peak)
{
	int first = -1, last = 0, crossings = 0;
	*peak = 0;
	for (uint32_t i = 1; i < s->sample.length; i++) {
		*peak = abs(s->bytes[i]) > *peak ? abs(s->bytes[i]) : *peak;
		if (s->bytes[i - 1] < 0 && s->bytes[i] >= 0) {
			first = first < 0 ? (int)i : first;
			last = (int)i;
			crossings++;
		}
	}
	return crossings > 1 ? (crossings - 1) * ROM_PCM4_HZ / (last - first) : 0;
}

static void test_dac(void)
{
	printf("DAC samples\n");
	static const struct {
		uint32_t rate;
		int channels, bits;
		bool is_float;
		int index;
	} WAVS[] = {{44100, 2, 16, false, 0}, {48000, 1, 24, false, 0}, {22050, 1, 8, false, 1}, {16000, 2, 32, true, 2}, {11025, 1, 16, false, 4}, {8000, 1, 16, false, 5}, {96000, 1, 16, false, 0}};
	for (size_t i = 0; i < sizeof(WAVS) / sizeof(WAVS[0]); i++) {
		size_t size;
		uint8_t *wav = sine_wav(WAVS[i].rate, WAVS[i].channels, WAVS[i].bits, WAVS[i].is_float, 440, 0.5, 0.5, &size);
		const char *error;
		dac_sample *s = dac_from_wav(wav, size, "sine", &error);
		int peak = 0;
		const double hz = s ? sine_hz(s, &peak) : 0;
		const double seconds = s ? s->sample.length / ROM_PCM4_HZ : 0;
		CHECK(s && s->sample.rate == WAVS[i].index && s->sample.length % ROM_SAMPLE_BLOCK == 0 && fabs(hz / 440 - 1) < 0.005 && peak >= 60 && peak <= 66 && seconds >= 0.5 && seconds < 0.5 + 256 / ROM_PCM4_HZ + 0.001,
			"WAV %u Hz, %d ch, %d bit%s: driver rate %d, %.1f Hz, peak %d, %.3f s", WAVS[i].rate, WAVS[i].channels, WAVS[i].bits, WAVS[i].is_float ? " float" : "", s ? s->sample.rate : -1, hz, peak, seconds);
		dac_free(s);
		free(wav);
	}
	const char *error;
	const uint8_t junk[64] = "RIFF....WAVEjunk";
	CHECK(!dac_from_wav(junk, sizeof(junk), "junk", &error) && error, "a file without fmt and data is refused: %s", error ? error : "");
	size_t size;
	uint8_t *long_wav = sine_wav(8000, 1, 16, false, 100, 300, 0.5, &size);
	CHECK(!dac_from_wav(long_wav, size, "long", &error) && error, "300 s at 8 kHz doesn't fit the ROM: %s", error ? error : "");
	free(long_wav);

	size_t rom_size, built;
	const uint8_t *rom = mdmi_engine_builtin_rom(&rom_size);
	dac_set set = {0};
	uint8_t *wav = sine_wav(22050, 1, 16, false, 440, 0.3, 0.5, &size);
	dac_put(&set, 38, dac_from_wav(wav, size, "Snare.wav", &error));
	dac_put(&set, 5, dac_from_wav(wav, size, "Low.wav", &error));
	free(wav);
	uint8_t *image = dac_image(&set, rom, rom_size, &built);
	rom_sample got, before, other;
	bool same_rest = true;
	for (int note = 0; image && note < ROM_IMAGE_NOTES; note++) {
		if (note != 38 && note != 5) {
			const bool had = rom_image_sample(rom, rom_size, note, &before), has = rom_image_sample(image, built, note, &other);
			same_rest = same_rest && had == has && (!had || (before.length == other.length && !memcmp(before.bytes, other.bytes, before.length)));
		}
	}
	CHECK(image && rom_image_check(image, built) && rom_image_sample(image, built, 38, &got) && got.rate == 1 && got.length == set.notes[38]->sample.length
		&& !memcmp(got.bytes, set.notes[38]->bytes, got.length) && rom_image_sample(image, built, 5, &got) && !memcmp(got.bytes, set.notes[5]->bytes, got.length) && same_rest,
		"the image plays the new samples on notes 38 and 5, every other note as the ROM");
	CHECK(!memcmp(image, rom, 0x200) && built >= rom_size, "the image keeps the ROM's header");
	free(image);
	uint8_t *bad = malloc(rom_size);
	memcpy(bad, rom, rom_size);
	uint32_t bad_table, bad_table_ram, bad_records;
	rom_image_zone(&bad_table, &bad_table_ram, &bad_records);
	memset(bad + bad_table, 0x55, ROM_IMAGE_TABLE_BYTES);
	CHECK(!dac_image(&set, bad, rom_size, &built), "an image without the expected table is not patched");
	free(bad);

	state_view view = {0}, opened = {0};
	view.dac = &set;
	dac_set read = {0};
	opened.dac = &read;
	mdmi_patch *p = calloc(1, sizeof(*p)), *q = calloc(1, sizeof(*q));
	patch_init(p);
	patch_init(q);
	size_t state_size;
	uint8_t *state = state_build(p, &view, &state_size);
	bool knobs;
	const uint32_t version = read.version;
	CHECK(state && state_read(q, state, state_size, &opened, &knobs) && dac_same(&set, &read) && read.version != version && !strcmp(read.notes[38]->name, "Snare.wav"),
		"a project keeps its samples, %zu bytes", state_size);
	const uint32_t again = read.version;
	CHECK(state_read(q, state, state_size, &opened, &knobs) && read.version == again, "the same samples again change nothing, so the core doesn't restart");
	free(state);
	view.dac = NULL;
	state = state_build(p, &view, &state_size);
	CHECK(state && state_read(q, state, state_size, &opened, &knobs) && !read.notes[38] && !read.notes[5] && read.version != again, "a project without samples has the ROM's");
	free(state);
	state = state_build(p, NULL, &state_size);
	dac_put(&read, 7, NULL);
	const uint32_t bank_version = read.version;
	CHECK(state && state_read(q, state, state_size, &opened, &knobs) && read.version == bank_version, "a bank leaves the samples alone");
	free(state);
	free(p);
	free(q);
	dac_clear(&set);
	dac_clear(&read);
}

static int samples_handed;

static void count_handed(void *context, bool knobs)
{
	(void)context;
	(void)knobs;
	samples_handed++;
}

static void write_wav(const char *path, uint32_t rate, double seconds)
{
	size_t size;
	uint8_t *wav = sine_wav(rate, 1, 16, false, 220, seconds, 0.5, &size);
	FILE *file = fopen(path, "wb");
	fwrite(wav, 1, size, file);
	fclose(file);
	free(wav);
}

// What the DAC's pages do with WAV files: several onto notes in a row, all or nothing, CLEAR, UNDO.
static void test_dac_files(void)
{
	printf("DAC samples from files\n");
	char dir[MAX_PATH], good[MAX_PATH], other[MAX_PATH], bad[MAX_PATH], big1[MAX_PATH], big2[MAX_PATH];
	GetTempPathA(sizeof(dir), dir);
	snprintf(good, sizeof(good), "%smdmi-test-kick.wav", dir);
	snprintf(other, sizeof(other), "%smdmi-test-snare.wav", dir);
	snprintf(bad, sizeof(bad), "%smdmi-test-bad.wav", dir);
	snprintf(big1, sizeof(big1), "%smdmi-test-big1.wav", dir);
	snprintf(big2, sizeof(big2), "%smdmi-test-big2.wav", dir);
	write_wav(good, 44100, 0.3);
	write_wav(other, 22050, 0.2);
	FILE *junk = fopen(bad, "wb");
	fputs("not a wav at all", junk);
	fclose(junk);
	// Two of 130 s at 8 kHz take 2 MB, more than the room.
	write_wav(big1, 8000, 130);
	write_wav(big2, 8000, 130);

	mdmi_patch *p = calloc(1, sizeof(*p));
	patch_init(p);
	dac_set set = {0};
	state_view view = {0};
	view.dac = &set;
	const files_host host = {p, &view, count_handed, NULL};
	mdmi_files files;
	files_init(&files, &host);
	char message[FILES_MESSAGE];
	const char *two[2] = {good, other};
	samples_handed = 0;
	int loaded = files_put_samples(&files, two, 2, 30, message, sizeof(message));
	CHECK(loaded == 2 && set.notes[30] && set.notes[31] && !strcmp(set.notes[31]->name, "mdmi-test-snare.wav") && set.notes[30]->sample.rate == 0 && set.notes[31]->sample.rate == 1 && samples_handed == 1,
		"two WAV go to notes 30 and 31 in one step: %s", message);
	const uint32_t version = set.version;
	const char *with_bad[2] = {good, bad};
	loaded = files_put_samples(&files, with_bad, 2, 50, message, sizeof(message));
	CHECK(!loaded && !set.notes[50] && !set.notes[51] && set.version == version, "one unreadable file and none go in: %s", message);
	const char *big[2] = {big1, big2};
	loaded = files_put_samples(&files, big, 2, 60, message, sizeof(message));
	CHECK(!loaded && !set.notes[60] && !set.notes[61] && set.version == version, "files past the ROM's room and none go in: %s", message);
	CHECK(files_reset_sample(&files, 30, message, sizeof(message)) && !set.notes[30] && set.notes[31], "CLEAR gives note 30 the ROM's sample: %s", message);
	CHECK(files_undo(&files, message, sizeof(message)) && set.notes[30] && !strcmp(set.notes[30]->name, "mdmi-test-kick.wav"), "undo brings the file back: %s", message);
	CHECK(files_undo(&files, message, sizeof(message)) && !set.notes[30] && set.notes[31], "and a second undo clears it again");
	const char *one[1] = {other};
	loaded = files_put_samples(&files, one, 1, 127, message, sizeof(message));
	CHECK(loaded == 1 && set.notes[127] && files_put_samples(&files, two, 2, 127, message, sizeof(message)) == 1, "files past note 127 are left out");
	files_free(&files);
	dac_clear(&set);
	free(p);
	DeleteFileA(good);
	DeleteFileA(other);
	DeleteFileA(bad);
	DeleteFileA(big1);
	DeleteFileA(big2);
}

// Preparing a sample from its source (D38): trim, pitch, gain, rate, fade, one-shot, and the
// project keeps it all.
// The source frame of a block's start at the edit's rate, a little inside it so it rounds there.
static uint32_t block_frame(const dac_sample *s, const dac_edit *edit, uint32_t block)
{
	const double step = s->source_hz / ROM_PCM4_HZ;
	return edit->start + (uint32_t)(block * ROM_SAMPLE_BLOCK * step + step);
}

// Stage 18b: loop marks snap to blocks, keep the limits, go into the record and the project; a
// project of 0.18 reads as before.
static void test_dac_marks(void)
{
	printf("DAC loop marks\n");
	size_t size;
	uint8_t *wav = sine_wav(22050, 1, 16, false, 440, 1.0, 0.5, &size);
	const char *error;
	dac_sample *s = dac_from_wav(wav, size, "marks.wav", &error);
	free(wav);
	dac_edit edit = s->edit;
	edit.loop = true;
	edit.loop_start = block_frame(s, &edit, 6);
	edit.loop_end = block_frame(s, &edit, 20);
	dac_marks m;
	dac_marks_of(s, &edit, &m);
	CHECK(dac_prepare(s, &edit) && s->sample.flags == (ROM_SAMPLE_LOOP | ROM_SAMPLE_MARKS) && s->sample.loop_start == 6 && s->sample.loop_end == 20 && m.release,
		"marks snap to blocks 6 and 20, the record says so, the rest is the release");
	int tail = 0;
	for (uint32_t i = m.length - 3; i < m.length; i++) {
		tail = abs(s->bytes[i]) > tail ? abs(s->bytes[i]) : tail;
	}
	CHECK(s->sample.length == rom_image_cost(m.length) && tail <= 8, "with a release, FADE ends it (last 3 bytes at most %d)", tail);
	// Stage 22b (D54): with stop the bytes end on the loop's last block, the release isn't written,
	// the record is the one of a loop ending at END; the marks stay in the edit and the project.
	{
		dac_sample *stopped = dac_copy(s);
		dac_edit with = edit;
		with.stop = true;
		dac_marks sm;
		dac_marks_of(stopped, &with, &sm);
		const bool made = dac_prepare(stopped, &with);
		CHECK(made && sm.tail && !sm.release && stopped->sample.length == 20 * ROM_SAMPLE_BLOCK && stopped->sample.flags == (ROM_SAMPLE_LOOP | ROM_SAMPLE_MARKS)
				&& stopped->sample.loop_start == 6 && stopped->sample.loop_end == 20 && stopped->sample.length < s->sample.length && sm.frame_end == sm.frame_loop_end
				&& sm.frame_tail_end == m.frame_end && dac_cost(stopped, &with) < dac_cost(s, &edit),
			"stop: %u bytes, the loop's 20 blocks without the release of %u, flags 0x%02x, marks 6 and 20 kept", stopped->sample.length, s->sample.length - stopped->sample.length, stopped->sample.flags);
		CHECK(!dac_same_edit(&with, &edit), "stop is part of the edit");
		dac_set one = {0}, back = {0};
		dac_put(&one, 40, stopped);
		const size_t one_size = dac_block_size(&one);
		uint8_t *one_block = malloc(one_size);
		dac_block_write(&one, one_block);
		CHECK(dac_block_read(&back, one_block, one_size) && back.notes[40]->edit.loop && back.notes[40]->edit.stop && !back.notes[40]->edit.one_shot
				&& back.notes[40]->edit.loop_end == with.loop_end && back.notes[40]->sample.length == 20 * ROM_SAMPLE_BLOCK && dac_same(&one, &back),
			"and the project reads stop back with its marks");
		free(one_block);
		dac_clear(&one);
		dac_clear(&back);
		// Stage 37 (review): a project whose samples use the whole space, past what a change may fill
		// now, still opens (samples saved up to 0.31 against the space without the envelopes' room).
		dac_sample *big = dac_copy(stopped);
		const uint32_t whole = rom_image_space() & ~(uint32_t)(ROM_SAMPLE_BLOCK - 1);
		free(big->bytes);
		big->bytes = calloc(whole, 1);
		big->sample.bytes = big->bytes;
		big->sample.length = whole;
		big->sample.flags = 0;
		big->edit.loop = big->edit.stop = false;
		free(big->source);
		big->source = NULL;
		big->source_frames = 0;
		big->source_hz = 0;
		dac_put(&one, 41, big);
		const size_t big_size = dac_block_size(&one);
		uint8_t *big_block = malloc(big_size);
		dac_block_write(&one, big_block);
		CHECK(whole > rom_image_room() && dac_block_read(&back, big_block, big_size) && back.notes[41] && back.notes[41]->sample.length == whole,
			"a project's samples over the room a change may fill, within the whole space, still open (%u > %u)", whole, rom_image_room());
		free(big_block);
		dac_clear(&one);
		dac_clear(&back);
	}
	// Into the image and back.
	size_t rom_size, built;
	const uint8_t *rom = mdmi_engine_builtin_rom(&rom_size);
	dac_set set = {0};
	dac_put(&set, 40, dac_copy(s));
	edit.loop_start = 0;
	edit.loop_end = 0;
	dac_sample *plain = dac_copy(s);
	dac_prepare(plain, &edit);
	dac_put(&set, 41, plain);
	uint8_t *image = dac_image(&set, rom, rom_size, &built);
	rom_sample back, back_plain;
	CHECK(image && rom_image_sample(image, built, 40, &back) && back.flags == (ROM_SAMPLE_LOOP | ROM_SAMPLE_MARKS) && back.loop_start == 6 && back.loop_end == 20,
		"the image's record holds the marks at +10 and +12");
	CHECK(image && rom_image_sample(image, built, 41, &back_plain) && back_plain.flags == ROM_SAMPLE_LOOP && back_plain.length % ROM_SAMPLE_BLOCK == 0,
		"no marks: the record of stage 18, LOOP alone, whole blocks");
	// An unmarked record's +10..+13 stay zero, as in 0.18's image.
	uint32_t table, table_ram, records;
	rom_image_zone(&table, &table_ram, &records);
	const uint8_t *record41 = image ? image + records + 41 * 14 : NULL;
	CHECK(record41 && !record41[10] && !record41[11] && !record41[12] && !record41[13], "an unmarked record has zeros where the marks would be");
	free(image);
	// The project keeps the marks; a head of 0.18, 8 bytes shorter, reads as no marks.
	const size_t block_size = dac_block_size(&set);
	uint8_t *block = malloc(block_size);
	dac_block_write(&set, block);
	dac_set read = {0};
	CHECK(dac_block_read(&read, block, block_size) && dac_same(&set, &read) && read.notes[40]->sample.loop_end == 20, "the project keeps the marks");
	uint16_t head;
	memcpy(&head, block + 1, 2);
	const size_t first = 3 + head + s->source_frames * 2 + s->sample.length;
	uint8_t *old = malloc(block_size);
	memcpy(old, block + first, 3);
	const uint16_t old_head = (uint16_t)(head - 8);
	memcpy(old + 1, &old_head, 2);
	memcpy(old + 3, block + first + 3, old_head);
	const size_t rest = block_size - first - 3 - head;
	memcpy(old + 3 + old_head, block + first + 3 + head, rest);
	dac_set from_old = {0};
	const size_t old_size = 3 + old_head + rest;
	CHECK(dac_block_read(&from_old, old, old_size) && from_old.notes[41] && from_old.notes[41]->sample.flags == ROM_SAMPLE_LOOP
			&& from_old.notes[41]->sample.length == plain->sample.length && !memcmp(from_old.notes[41]->bytes, plain->bytes, plain->sample.length)
			&& !from_old.notes[41]->edit.loop_end,
		"a LOOP sample of a 0.18 project reads with the same bytes and flag, no marks");
	{
		// And makes the same record as when it was saved.
		dac_set only_old = {0}, only_new = {0};
		dac_put(&only_old, 41, dac_copy(from_old.notes[41]));
		dac_put(&only_new, 41, dac_copy(plain));
		size_t built_old, built_new;
		uint8_t *image_old = dac_image(&only_old, rom, rom_size, &built_old), *image_new = dac_image(&only_new, rom, rom_size, &built_new);
		CHECK(image_old && image_new && built_old == built_new && !memcmp(image_old, image_new, built_old), "the 0.18 project's sample builds the same image");
		free(image_old);
		free(image_new);
		dac_clear(&only_old);
		dac_clear(&only_new);
	}
	free(block);
	free(old);
	dac_clear(&read);
	dac_clear(&from_old);
	// Limits: one block at least, an intro only with the loop's end far enough, at every rate.
	edit = s->edit;
	edit.loop = true;
	edit.loop_start = block_frame(s, &edit, 8);
	edit.loop_end = block_frame(s, &edit, 8) + 10;
	dac_marks_of(s, &edit, &m);
	CHECK(m.loop_end - m.loop_start == 1, "a loop is never shorter than a block (%u..%u)", m.loop_start, m.loop_end);
	bool intro_kept = true, intro_refused = true;
	for (int rate = 0; rate < ROM_SAMPLE_RATES; rate++) {
		edit.rate = (int8_t)rate;
		dac_marks_of(s, &edit, &m);
		const uint32_t least = m.least_with_intro;
		edit.loop_start = block_frame(s, &edit, 1);
		edit.loop_end = block_frame(s, &edit, least - 1);
		dac_marks_of(s, &edit, &m);
		intro_refused = intro_refused && (least < 3 || m.loop_start == 0);
		edit.loop_end = block_frame(s, &edit, least);
		dac_marks_of(s, &edit, &m);
		intro_kept = intro_kept && m.loop_start == 1 && m.loop_end == least;
	}
	CHECK(intro_refused && intro_kept, "at every rate an intro needs its loop's end at the least blocks, else the loop starts at the start");
	edit = s->edit;
	edit.loop = true;
	edit.end = 100;
	dac_marks_of(s, &edit, &m);
	CHECK(m.too_short && !m.loops && dac_prepare(s, &edit) && s->sample.flags == 0, "under one block: too short, it plays once");
	dac_clear(&set);
	dac_free(s);
}

// Stage 19: a project of 0.18 holds bytes made at drv_pcm's rates; opened, they are made again
// at PCM4's one rate from the sources, last as long as the sources, and RATE stays as it was.
static void test_dac_old_project(void)
{
	printf("DAC: a 0.18 project at PCM4's rate\n");
	size_t size;
	uint8_t *wav = sine_wav(22050, 1, 16, false, 440, 2.0, 0.5, &size);
	const char *error;
	dac_sample *s = dac_from_wav(wav, size, "old.wav", &error);
	free(wav);
	dac_set set = {0};
	static const int RATES[3] = {0, 1, 5};
	for (int k = 0; k < 3; k++) {
		dac_sample *c = dac_copy(s);
		dac_edit e = c->edit;
		e.rate = (int8_t)RATES[k];
		if (k == 1) {
			e.loop = true;
			e.loop_start = 5000;
			e.loop_end = 30000;
		}
		dac_prepare(c, &e);
		// The bytes 0.18 made: as many as its driver's rate gave the source.
		const uint32_t old = rom_image_cost((uint32_t)ceil(s->source_frames * ROM_SAMPLE_HZ[RATES[k]] / s->source_hz));
		free(c->bytes);
		c->bytes = calloc(1, old);
		c->sample.bytes = c->bytes;
		c->sample.length = old;
		dac_put(&set, 40 + k, c);
	}
	// A copy of one of the ROM's samples in 0.18: bytes for 22050 Hz, played at 21600.
	int8_t *rom_bytes = calloc(1, 256 * 200);
	for (int i = 0; i < 256 * 200; i++) {
		rom_bytes[i] = (int8_t)(i % 40 < 20 ? 60 : -60);
	}
	const rom_sample old_rom = {1, 256 * 200, rom_bytes, 0, 0, 0};
	dac_sample *copy = dac_from_rom(&old_rom, "OldRom");
	copy->source_hz = 21600;
	dac_put(&set, 50, copy);
	free(rom_bytes);
	const size_t block_size = dac_block_size(&set);
	uint8_t *block = malloc(block_size);
	dac_block_write(&set, block);
	dac_set read = {0};
	bool opened = dac_block_read(&read, block, block_size), lengths = true, rates = true;
	for (int k = 0; k < 3 && opened; k++) {
		const dac_sample *r = read.notes[40 + k];
		const double now = r->sample.length / ROM_PCM4_HZ, was = (double)s->source_frames / s->source_hz;
		printf("    RATE %d: %.3f s of %.3f s, %u bytes\n", RATES[k], now, was, r->sample.length);
		lengths = lengths && fabs(now / was - 1) < 0.01;
		rates = rates && r->edit.rate == RATES[k];
	}
	const dac_sample *rom_read = opened ? read.notes[50] : NULL;
	const double rom_now = rom_read ? rom_read->sample.length / ROM_PCM4_HZ : 0, rom_was = 256 * 200 / 21600.0;
	printf("    ROM copy: %.3f s of %.3f s\n", rom_now, rom_was);
	CHECK(opened && lengths && rom_read && fabs(rom_now / rom_was - 1) < 0.01, "a 0.18 project's samples are made again at PCM4's rate and last as long, within 1%%");
	CHECK(opened && read.notes[41]->sample.flags == (ROM_SAMPLE_LOOP | ROM_SAMPLE_MARKS) && read.notes[41]->edit.loop_end == 30000, "its LOOP and marks stay");
	// Saved again and opened: RATE is still what 0.18 had.
	const size_t again_size = opened ? dac_block_size(&read) : 0;
	uint8_t *again = malloc(again_size ? again_size : 1);
	if (opened) {
		dac_block_write(&read, again);
	}
	dac_set twice = {0};
	const bool reopened = opened && dac_block_read(&twice, again, again_size);
	for (int k = 0; k < 3 && reopened; k++) {
		rates = rates && twice.notes[40 + k]->edit.rate == RATES[k];
	}
	CHECK(rates && reopened && dac_same(&read, &twice), "RATE is written back as it was, and the project reads the same again");
	free(block);
	free(again);
	dac_clear(&set);
	dac_clear(&read);
	dac_clear(&twice);
	dac_free(s);
}

static void test_dac_edit(void)
{
	printf("DAC samples prepared\n");
	size_t size;
	uint8_t *wav = sine_wav(22050, 1, 16, false, 440, 1.0, 0.25, &size);
	const char *error;
	dac_sample *s = dac_from_wav(wav, size, "sine.wav", &error);
	free(wav);
	CHECK(s && s->source && s->source_frames == 22050 && s->source_hz == 22050 && s->edit.end == 22050 && s->edit.rate == DAC_RATE_AUTO && s->edit.fade && !s->edit.one_shot,
		"a WAV keeps its source, 22050 frames, all of it kept, rate AUTO, fade on");
	const uint32_t whole = s->sample.length;
	dac_edit edit = s->edit;
	edit.start = 5512;
	edit.end = 16537;
	CHECK(dac_prepare(s, &edit) && s->sample.length < whole * 6 / 10 && s->sample.length > whole * 4 / 10, "half the source kept: %u bytes of %u", s->sample.length, whole);
	edit.start = 0;
	edit.end = 22050;
	CHECK(dac_prepare(s, &edit) && s->sample.length == whole, "and all of it again, as long as before");
	int peak;
	edit.pitch = 1200;
	CHECK(dac_prepare(s, &edit) && fabs(sine_hz(s, &peak) / 880 - 1) < 0.01 && s->sample.length <= whole / 2 + 256, "an octave up: %.1f Hz, half as long", sine_hz(s, &peak));
	edit.pitch = 2400;
	CHECK(dac_prepare(s, &edit) && fabs(sine_hz(s, &peak) / 1760 - 1) < 0.01 && s->sample.length <= whole / 4 + 256, "two octaves up: %.1f Hz, a quarter as long", sine_hz(s, &peak));
	edit.pitch = -2400;
	CHECK(dac_prepare(s, &edit) && fabs(sine_hz(s, &peak) / 110 - 1) < 0.01 && s->sample.length >= whole * 4 - 1024 && s->sample.length <= whole * 4 + 256, "two octaves down: %.1f Hz, four times as long", sine_hz(s, &peak));
	edit.pitch = -700;
	const double fifth = 440 * pow(2.0, -7.0 / 12);
	CHECK(dac_prepare(s, &edit) && fabs(sine_hz(s, &peak) / fifth - 1) < 0.01, "a fifth down: %.1f Hz, %.1f wanted", sine_hz(s, &peak), fifth);
	edit.pitch = 0;
	edit.rate = 4;
	// PCM4 has one rate (stage 19): the field stays in the edit, the bytes are the same.
	CHECK(dac_prepare(s, &edit) && s->edit.rate == 4 && fabs(sine_hz(s, &peak) / 440 - 1) < 0.01 && s->sample.length == whole, "RATE 11 kHz kept in the edit: same pitch %.1f Hz, same %u bytes", sine_hz(s, &peak), s->sample.length);
	edit.rate = DAC_RATE_AUTO;
	CHECK(dac_prepare(s, &edit) && sine_hz(s, &peak) > 0 && peak >= 30 && peak <= 34 && !s->clipped, "a quarter of full scale peaks at %d", peak);
	edit.gain = (int16_t)dac_normalize_gain(s);
	CHECK(dac_prepare(s, &edit) && sine_hz(s, &peak) > 0 && peak >= 118 && peak <= 127 && !s->clipped, "NORMALIZE: gain %+.1f dB, peak %d, nothing clipped", edit.gain / 10.0, peak);
	const int16_t normal = edit.gain;
	edit.gain = (int16_t)(normal + 60);
	CHECK(dac_prepare(s, &edit) && s->clipped, "6 dB over NORMALIZE clips, and says so");
	edit.gain = normal;
	dac_prepare(s, &edit);
	int last = (int)s->sample.length - 1, tail = 0;
	while (last > 0 && !s->bytes[last]) {
		last--;
	}
	for (int i = last - 2; i <= last; i++) {
		tail = abs(s->bytes[i]) > tail ? abs(s->bytes[i]) : tail;
	}
	CHECK(tail <= 8 && !s->sample.flags, "a full-scale sine fades to silence at its end (last 3 samples at most %d), no flag", tail);
	edit.gain = 0;
	edit.one_shot = true;
	CHECK(dac_prepare(s, &edit) && s->sample.flags == ROM_SAMPLE_ONE_SHOT, "ONE-SHOT is the record's flag");
	// LOOP (stage 18): whole blocks, no fade, its own flag; under one block it plays once.
	{
		dac_edit loop = edit;
		loop.one_shot = false;
		loop.loop = true;
		loop.gain = normal;
		const uint32_t plain = (uint32_t)ceil(22050 / (22050 / ROM_PCM4_HZ));
		const bool made = dac_prepare(s, &loop);
		int end_peak = 0;
		for (uint32_t i = s->sample.length - 20; made && i < s->sample.length; i++) {
			end_peak = abs(s->bytes[i]) > end_peak ? abs(s->bytes[i]) : end_peak;
		}
		CHECK(made && s->sample.flags == ROM_SAMPLE_LOOP && s->sample.length % ROM_SAMPLE_BLOCK == 0 && s->sample.length <= plain && s->sample.length > plain - ROM_SAMPLE_BLOCK
				&& dac_cost(s, &loop) == s->sample.length,
			"LOOP: its flag, %u bytes of %u cut to whole blocks, the cost says so", s->sample.length, plain);
		CHECK(end_peak > 80, "LOOP: no fade at the seam (last 20 samples peak %d)", end_peak);
		dac_marks marks;
		dac_marks_of(s, &loop, &marks);
		const uint32_t seam = marks.frame_end;
		CHECK(seam < loop.end && seam > loop.end - 300 && !marks.too_short && !marks.release, "LOOP: the repeat goes back at frame %u of %u", seam, loop.end);
		loop.end = 200;
		dac_marks_of(s, &loop, &marks);
		CHECK(dac_prepare(s, &loop) && marks.too_short && s->sample.flags == 0 && marks.frame_end == loop.end,
			"LOOP under one block: plays once, no flag");
		loop.one_shot = true;
		CHECK(dac_prepare(s, &loop) && s->sample.flags == ROM_SAMPLE_ONE_SHOT, "LOOP under one block with ONE-SHOT: only ONE-SHOT");
		loop.end = 22050;
		// Stage 22b (D54 cancels D53): the two never go together in the record.
		CHECK(dac_prepare(s, &loop) && s->sample.flags == ROM_SAMPLE_LOOP, "an edit with LOOP and ONE-SHOT gets LOOP alone (0x%02x)", s->sample.flags);
		loop.one_shot = false;
		dac_set one = {0}, back = {0};
		loop.end = 22050;
		dac_prepare(s, &loop);
		dac_put(&one, 45, dac_copy(s));
		const size_t one_size = dac_block_size(&one);
		uint8_t *one_block = malloc(one_size);
		dac_block_write(&one, one_block);
		CHECK(dac_block_read(&back, one_block, one_size) && back.notes[45]->edit.loop && !back.notes[45]->edit.one_shot && back.notes[45]->sample.flags == ROM_SAMPLE_LOOP,
			"the project keeps LOOP");
		// An older build's head: the same byte 1 is one-shot, as it always was.
		const size_t mode_at = 3 + DAC_NAME + 4 + 4 + 4 + 4 + 2 + 2 + 1 + 1;
		CHECK(one_block[mode_at] == 2, "LOOP is mode 2 in the head");
		one_block[mode_at] = 1;
		CHECK(dac_block_read(&back, one_block, one_size) && !back.notes[45]->edit.loop && back.notes[45]->edit.one_shot && back.notes[45]->sample.flags == ROM_SAMPLE_ONE_SHOT,
			"mode 1 reads as one-shot with its flag alone, as before stage 18");
		// Stage 22b (D54): the modes as each reads and writes back. 0-2 as in 0.22; 3, LOOP with
		// ONE-SHOT of 0.22.2, is LOOP with its release; 4 is LOOP that Note Off stops.
		{
			static const struct {
				uint8_t mode, written, flags;
				bool loop, one_shot, stop;
			} MODES[] = {
				{0, 0, 0, false, false, false},
				{1, 1, ROM_SAMPLE_ONE_SHOT, false, true, false},
				{2, 2, ROM_SAMPLE_LOOP, true, false, false},
				{3, 2, ROM_SAMPLE_LOOP, true, false, false},
				{4, 4, ROM_SAMPLE_LOOP, true, false, true},
			};
			for (size_t i = 0; i < sizeof(MODES) / sizeof(*MODES); i++) {
				one_block[mode_at] = MODES[i].mode;
				const bool read_back = dac_block_read(&back, one_block, one_size);
				const dac_edit *e = read_back ? &back.notes[45]->edit : NULL;
				uint8_t written = 255;
				if (read_back) {
					uint8_t *again_block = malloc(dac_block_size(&back));
					dac_block_write(&back, again_block);
					written = again_block[mode_at];
					// 0-2 give what 0.22 prepared from the same edit: bytes, flags and record.
					if (MODES[i].mode <= 2) {
						dac_sample *expected = dac_copy(s);
						dac_edit as_022 = loop;
						as_022.loop = MODES[i].loop;
						as_022.one_shot = MODES[i].one_shot;
						const bool same = dac_prepare(expected, &as_022) && expected->sample.length == back.notes[45]->sample.length
							&& expected->sample.flags == back.notes[45]->sample.flags && !memcmp(expected->bytes, back.notes[45]->bytes, expected->sample.length);
						CHECK(same, "mode %u prepares the bytes and record of 0.22", MODES[i].mode);
						dac_free(expected);
					}
					if (MODES[i].mode == 2) {
						CHECK(!memcmp(again_block, one_block, one_size), "mode 2 writes back the very block");
					}
					free(again_block);
				}
				CHECK(e && e->loop == MODES[i].loop && e->one_shot == MODES[i].one_shot && e->stop == MODES[i].stop && back.notes[45]->sample.flags == MODES[i].flags
						&& written == MODES[i].written,
					"mode %u reads as loop %d, one-shot %d, stop %d, flags 0x%02x, and is written as %u", MODES[i].mode, MODES[i].loop, MODES[i].one_shot, MODES[i].stop, MODES[i].flags,
					written);
			}
		}
		free(one_block);
		dac_clear(&one);
		dac_clear(&back);
	}
	edit.start = 30000;
	CHECK(!dac_prepare(s, &edit) && s->edit.start == 0, "a start past the source is refused, the sample unchanged");

	dac_set set = {0}, read = {0};
	edit = s->edit;
	edit.start = 1000;
	edit.pitch = 300;
	edit.gain = -30;
	dac_prepare(s, &edit);
	dac_put(&set, 45, s);
	const size_t block_size = dac_block_size(&set);
	uint8_t *block = malloc(block_size);
	dac_block_write(&set, block);
	CHECK(dac_block_read(&read, block, block_size) && dac_same(&set, &read) && read.notes[45]->edit.start == 1000 && read.notes[45]->edit.pitch == 300 && read.notes[45]->source_frames == 22050,
		"the project keeps the source, the edit and the bytes: %zu bytes", block_size);
	dac_edit back = read.notes[45]->edit;
	back.start = 0;
	CHECK(dac_prepare(read.notes[45], &back) && read.notes[45]->sample.length > s->sample.length, "after opening, the trim can go back");
	block[3 + 52] = 9;
	CHECK(!dac_block_read(&read, block, block_size), "a rate past the driver's is refused");
	block[3 + 52] = 0xFF;
	block[3 + 32] ^= 1;
	dac_clear(&read);
	CHECK(dac_block_read(&read, block, block_size) && !dac_same(&set, &read), "another source rate is another sample");
	CHECK(!dac_block_read(&read, block, block_size - 10), "a cut block is refused");
	free(block);

	// What preparing costs the window's thread: a minute of 48 kHz sound at the highest rate.
	uint8_t *minute = sine_wav(48000, 1, 16, false, 440, 58, 0.5, &size);
	dac_sample *long_one = dac_from_wav(minute, size, "minute.wav", &error);
	free(minute);
	LARGE_INTEGER frequency, before, after;
	QueryPerformanceFrequency(&frequency);
	QueryPerformanceCounter(&before);
	dac_edit up = long_one ? long_one->edit : edit;
	up.pitch = 50;
	const bool prepared = long_one && dac_prepare(long_one, &up);
	QueryPerformanceCounter(&after);
	const double prepare_ms = (after.QuadPart - before.QuadPart) * 1000.0 / frequency.QuadPart;
	CHECK(prepared && prepare_ms < 1000, "58 s at 48 kHz prepared in %.0f ms", prepare_ms);
	dac_free(long_one);

	// Through the files layer: one undo step per change.
	mdmi_patch *p = calloc(1, sizeof(*p));
	patch_init(p);
	state_view view = {0};
	view.dac = &set;
	const files_host host = {p, &view, count_handed, NULL};
	mdmi_files files;
	files_init(&files, &host);
	char message[FILES_MESSAGE];
	edit = set.notes[45]->edit;
	edit.pitch = -1200;
	const uint32_t version = set.version;
	CHECK(files_edit_sample(&files, 45, &edit, false, message, sizeof(message)) && set.notes[45]->edit.pitch == -1200 && set.version != version, "an edit goes in: %s", message);
	CHECK(files_undo(&files, message, sizeof(message)) && set.notes[45]->edit.pitch == 300, "undo puts the edit before back");
	CHECK(!files_edit_sample(&files, 100, &edit, false, message, sizeof(message)), "an empty note has nothing to edit");
	files_free(&files);
	dac_clear(&set);
	dac_clear(&read);
	free(p);
}

// Samples between notes (owner after 0.12.0): swapped, copied, the ROM's removed and back.
static void test_dac_moves(void)
{
	printf("DAC samples moved\n");
	mdmi_patch *p = calloc(1, sizeof(*p));
	patch_init(p);
	dac_set set = {0};
	state_view view = {0};
	view.dac = &set;
	const files_host host = {p, &view, count_handed, NULL};
	mdmi_files files;
	files_init(&files, &host);
	char message[FILES_MESSAGE];
	size_t size;
	const char *error;
	uint8_t *wav = sine_wav(22050, 1, 16, false, 440, 0.2, 0.5, &size);
	dac_put(&set, 40, dac_from_wav(wav, size, "file.wav", &error));
	free(wav);
	// A file onto a ROM note: they swap, the ROM's sample comes along as a copy.
	CHECK(files_move_sample(&files, 40, 36, false, message, sizeof(message)) && set.notes[36] && !strcmp(set.notes[36]->name, "file.wav")
		&& set.notes[40] && !strcmp(set.notes[40]->name, "RimTap") && set.notes[40]->sample.rate == 1, "a file swapped with RimTap's note: %s", message);
	size_t rom_size;
	const uint8_t *rom = mdmi_engine_builtin_rom(&rom_size);
	rom_sample rim;
	rom_image_sample(rom, rom_size, 36, &rim);
	CHECK(set.notes[40]->sample.length == rim.length && !memcmp(set.notes[40]->bytes, rim.bytes, rim.length), "the ROM's sample keeps its bytes on the other note");
	CHECK(files_undo(&files, message, sizeof(message)) && set.notes[40] && !strcmp(set.notes[40]->name, "file.wav") && !set.notes[36], "undo puts both back");
	CHECK(files_move_sample(&files, 40, 90, true, message, sizeof(message)) && set.notes[40] && set.notes[90], "a copy keeps the file where it was");
	CHECK(files_move_sample(&files, 90, 100, false, message, sizeof(message)) && !set.notes[90] && set.notes[100], "moved to an empty note, the note left is empty");
	CHECK(files_reset_sample(&files, 36, message, sizeof(message)) && set.removed[36] && !set.notes[36], "CLEAR on a ROM note removes its sample: %s", message);
	size_t built;
	uint8_t *image = dac_image(&set, rom, rom_size, &built);
	rom_sample none;
	CHECK(image && !rom_image_sample(image, built, 36, &none), "the image has nothing on the removed note");
	free(image);
	dac_set read = {0};
	const size_t block_size = dac_block_size(&set);
	uint8_t *block = malloc(block_size);
	dac_block_write(&set, block);
	CHECK(dac_block_read(&read, block, block_size) && read.removed[36] && dac_same(&set, &read), "the project keeps the removed note");
	free(block);
	CHECK(files_reset_sample(&files, 36, message, sizeof(message)) && !set.removed[36] && !set.notes[36], "and on it, RESTORE brings the ROM's back: %s", message);
	CHECK(!files_move_sample(&files, 95, 96, false, message, sizeof(message)), "an empty note has nothing to move");
	// A sample of the ROM's changed on the panel becomes the project's (owner after 0.12.0).
	rom_sample kick;
	rom_image_sample(rom, rom_size, 34, &kick);
	dac_edit louder = {.start = 0, .end = kick.length, .gain = 30, .rate = (int8_t)kick.rate};
	CHECK(!set.notes[34] && files_edit_sample(&files, 34, &louder, false, message, sizeof(message)) && set.notes[34] && set.notes[34]->edit.gain == 30
		&& !strcmp(set.notes[34]->name, "StdKick"), "a ROM sample changed becomes the project's: %s", message);
	CHECK(files_reset_sample(&files, 34, message, sizeof(message)) && !set.notes[34] && !set.removed[34], "and CLEAR gives the ROM's back");
	files_free(&files);
	dac_clear(&set);
	dac_clear(&read);
	free(p);
}

// How many controllers cc went out, on any channel.
static int count_cc(uint8_t cc)
{
	int count = 0;
	for (size_t i = 0; i + 2 < sent_bytes; i++) {
		count += (sent[i] & 0xF0) == 0xB0 && sent[i + 1] == cc;
	}
	return count;
}

static int count_ch3_ccs(void)
{
	return count_cc(81) + count_cc(82) + count_cc(102) + count_cc(103) + count_cc(104);
}

// Stage 21 (D51): Ch3 Notes, LFO Reset and Semi, Fine, Fixed of channel 3's four operators are
// globals 10-23, last in the host's list. CC 81 and 82 go on MIDI 7 as the other globals, CC 102-104
// on the operator's own channel: MIDI 11-13 for operators 1-3, MIDI 3 for operator 4.
static void test_ch3_ops(void)
{
	printf("Channel 3 by operators\n");
	static mdmi_patch patch, other;
	patch_init(&patch);
	char name[32], text[16];
	bool ids = GLOBAL_CH3_NOTES == 10 && GLOBAL_LFO_RESET == 11 && GLOBAL_CH3_OP(0, PATCH_CH3_SEMI) == 12 && GLOBAL_CH3_OP(3, PATCH_CH3_FIXED) == 23 && GLOBAL_CH3_OP_END == 24 && GLOBAL_MONO(0) == 24 && PATCH_NUM_GLOBALS == 30;
	patch_param_name(GLOBAL_CH3_NOTES, name, sizeof(name));
	ids = ids && !strcmp(name, "Ch3 Notes") && patch_param(GLOBAL_CH3_NOTES)->max == 2 && patch_param(GLOBAL_CH3_NOTES)->default_value == PATCH_CH3_OPS;
	patch_param_name(GLOBAL_LFO_RESET, name, sizeof(name));
	ids = ids && !strcmp(name, "LFO Reset") && patch_param(GLOBAL_LFO_RESET)->max == 6 && patch_param(GLOBAL_LFO_RESET)->default_value == 0;
	static const char *const FIELDS[3] = {"Semi", "Fine", "Fixed"};
	static const int MAX[3] = {96, 126, 1}, DEFAULT[3] = {48, 63, 0};
	for (int op = 0; op < PATCH_OPS; op++) {
		for (int field = 0; field < 3; field++) {
			const uint32_t id = (uint32_t)(12 + op * 3 + field);
			char want[32];
			snprintf(want, sizeof(want), "Ch3 Op%d %s", op + 1, FIELDS[field]);
			patch_param_name(id, name, sizeof(name));
			ids = ids && !strcmp(name, want) && patch_param(id)->max == MAX[field] && patch_param(id)->default_value == DEFAULT[field] && patch_param(id)->stepped;
		}
	}
	CHECK(ids, "ids 10-23: Ch3 Notes (three positions since 0.22), LFO Reset, then Semi, Fine, Fixed of operators 1-4, with their ranges and defaults");
	// Stage 38: the six MONO knobs come last, after the operators' (ids 24-29).
	CHECK(patch_param_id_at(PATCH_NUM_IDS - 21) == GLOBAL_MASTER && patch_param_id_at(PATCH_NUM_IDS - 20) == 10 && patch_param_id_at(PATCH_NUM_IDS - 7) == 23
			&& patch_param_id_at(PATCH_NUM_IDS - 6) == 24 && patch_param_id_at(PATCH_NUM_IDS - 1) == 29 && PATCH_NUM_IDS == 383,
		"in the host's list after Master Volume, %d knobs", PATCH_NUM_IDS);
	bool labels = true;
	patch_format(&patch.bank, GLOBAL_CH3_NOTES, PATCH_CH3_ONE_NOTE, text, sizeof(text));
	labels = labels && !strcmp(text, "One Note");
	patch_format(&patch.bank, GLOBAL_CH3_NOTES, PATCH_CH3_CSM, text, sizeof(text));
	labels = labels && !strcmp(text, "CSM");
	patch_format(&patch.bank, GLOBAL_LFO_RESET, 3, text, sizeof(text));
	labels = labels && !strcmp(text, "Ch 3");
	patch_format(&patch.bank, GLOBAL_CH3_OP(1, PATCH_CH3_SEMI), 55, text, sizeof(text));
	labels = labels && !strcmp(text, "+7 st");
	patch_format(&patch.bank, GLOBAL_CH3_OP(1, PATCH_CH3_FINE), 53, text, sizeof(text));
	labels = labels && !strcmp(text, "-10 ct");
	int value = -1;
	labels = labels && patch_parse(&patch.bank, GLOBAL_CH3_OP(0, PATCH_CH3_SEMI), "-12", &value) && value == 36;
	CHECK(labels, "they read One Note, CSM, Ch 3, +7 st, -10 ct, and -12 is Semi 36");

	clear();
	sync(&patch);
	CHECK(sent_bytes == 0, "the defaults are what the ROM starts with: nothing to send");
	set(&patch, GLOBAL_CH3_NOTES, PATCH_CH3_ONE_NOTE);
	CHECK(sent_bytes == 3 && sent_cc(0xB6, 81, 64), "One Note goes as CC 81 64 on MIDI 7");
	clear();
	set(&patch, GLOBAL_CH3_NOTES, PATCH_CH3_CSM);
	CHECK(sent_bytes == 3 && sent_cc(0xB6, 81, 127), "CSM goes as CC 81 127, which the ROM reads as 2 (stage 22)");
	clear();
	set(&patch, GLOBAL_CH3_NOTES, PATCH_CH3_OPS);
	CHECK(sent_bytes == 3 && sent_cc(0xB6, 81, 0), "Ops goes as CC 81 0");
	set(&patch, GLOBAL_CH3_NOTES, PATCH_CH3_ONE_NOTE);
	bool resets = true;
	for (int n = 1; n <= 6; n++) {
		clear();
		set(&patch, GLOBAL_LFO_RESET, n);
		resets = resets && sent_bytes == 3 && sent[0] == 0xB6 && sent[1] == 82 && sent[2] * 7 / 128 == n;
	}
	CHECK(resets, "LFO Reset Ch 1-6 go as CC 82 that the ROM reads back as value * 7 / 128");
	clear();
	set(&patch, GLOBAL_CH3_OP(0, PATCH_CH3_SEMI), 48 + 7);
	set(&patch, GLOBAL_CH3_OP(1, PATCH_CH3_FINE), 63 - 10);
	set(&patch, GLOBAL_CH3_OP(2, PATCH_CH3_SEMI), 0);
	set(&patch, GLOBAL_CH3_OP(3, PATCH_CH3_FIXED), 1);
	set(&patch, GLOBAL_CH3_OP(3, PATCH_CH3_FINE), 126);
	CHECK(sent_bytes == 15 && sent_cc(0xBA, 102, 71) && sent_cc(0xBB, 103, 54) && sent_cc(0xBC, 102, 16) && sent_cc(0xB2, 104, 127) && sent_cc(0xB2, 103, 127),
		"Semi, Fine and Fixed go as CC 102-104, value + 64, on MIDI 11, 12, 13 and MIDI 3 for operator 4");
	clear();
	set(&patch, GLOBAL_DAC, 1);
	CHECK(count_ch3_ccs() == 0, "a driver swap sends none of them: they are in the ROM's RAM");
	set(&patch, GLOBAL_DAC, 0);
	clear();
	const uint8_t reset = 0xFF;
	patch_midi(&patch, &reset, 1, record, NULL);
	CHECK(count_cc(81) == 1 && count_cc(82) == 1 && count_cc(102) == 4 && count_cc(103) == 4 && count_cc(104) == 4, "after a reset all 14 go again");

	// From a clip: CC 81 and 82 on any channel, CC 102-104 only on the operator's channel.
	patch_values before = patch.model;
	clear();
	midi(&patch, 0xB4, 81, 127);
	CHECK(sent_bytes == 3 && sent_cc(0xB6, 81, 127) && patch.model.globals[GLOBAL_CH3_NOTES] == PATCH_CH3_CSM, "CC 81 127 on MIDI 5 is CSM, sent on MIDI 7");
	clear();
	midi(&patch, 0xB4, 81, 64);
	midi(&patch, 0xB0, 81, 86);
	CHECK(sent_bytes == 6 && sent_cc(0xB6, 81, 64) && sent_cc(0xB6, 81, 127) && patch.model.globals[GLOBAL_CH3_NOTES] == PATCH_CH3_CSM, "CC 81 64 is One Note, 86 on MIDI 1 CSM again");
	clear();
	midi(&patch, 0xB4, 81, 20);
	CHECK(sent_bytes == 3 && sent_cc(0xB6, 81, 0) && patch.model.globals[GLOBAL_CH3_NOTES] == PATCH_CH3_OPS, "CC 81 20 on MIDI 5 is Ops, sent on MIDI 7");
	clear();
	midi(&patch, 0xB8, 82, 40);
	CHECK(patch.model.globals[GLOBAL_LFO_RESET] == 2 && sent_bytes == 3 && sent[0] == 0xB6 && sent[1] == 82, "CC 82 40 on MIDI 9 is Ch 2");
	clear();
	midi(&patch, 0xBA, 102, 76);
	midi(&patch, 0xBB, 103, 0);
	midi(&patch, 0xBC, 102, 127);
	midi(&patch, 0xB2, 104, 10);
	const int16_t *g = patch.model.globals;
	CHECK(g[GLOBAL_CH3_OP(0, PATCH_CH3_SEMI)] == 60 && g[GLOBAL_CH3_OP(1, PATCH_CH3_FINE)] == 0 && g[GLOBAL_CH3_OP(2, PATCH_CH3_SEMI)] == 96 && g[GLOBAL_CH3_OP(3, PATCH_CH3_FIXED)] == 0,
		"CC 102-104 on MIDI 11-13 and 3 move their operator's knob: +12, Fine -64 kept at -63, Semi +63 kept at +48, Fixed off");
	bool others = true;
	for (int op = 0; op < PATCH_OPS; op++) {
		for (int field = 0; field < 3; field++) {
			const int id = GLOBAL_CH3_OP(op, field);
			const bool touched = id == GLOBAL_CH3_OP(0, PATCH_CH3_SEMI) || id == GLOBAL_CH3_OP(1, PATCH_CH3_FINE) || id == GLOBAL_CH3_OP(2, PATCH_CH3_SEMI) || id == GLOBAL_CH3_OP(3, PATCH_CH3_FIXED);
			others = others && (touched || g[id] == before.globals[id]);
		}
	}
	CHECK(others && sent_bytes == 12 && sent_cc(0xBB, 103, 1), "the other operators' knobs stay; the ROM gets the kept values");
	before = patch.model;
	clear();
	static const uint8_t ELSEWHERE[] = {0xB0, 0xB1, 0xB3, 0xB5, 0xB6, 0xB9, 0xBD, 0xBF};
	for (size_t i = 0; i < sizeof(ELSEWHERE); i++) {
		midi(&patch, ELSEWHERE[i], 102, 100);
		midi(&patch, ELSEWHERE[i], 104, 127);
	}
	CHECK(sent_bytes == 0 && !memcmp(patch.model.globals, before.globals, sizeof(before.globals)), "CC 102-104 on MIDI 1, 2, 4, 6, 7, 10, 14, 16 move nothing and go nowhere");

	// The project keeps them, a project from before has the defaults, a bank file doesn't touch them.
	set(&patch, GLOBAL_CH3_NOTES, PATCH_CH3_ONE_NOTE);
	patch_publish(&patch);
	static uint8_t state[PATCH_NUM_IDS * 6], old[PATCH_NUM_IDS * 6];
	patch_save(patch_view(&patch), state);
	patch_init(&other);
	CHECK(patch_load(&other, state, sizeof(state), NULL, NULL, NULL, NULL) && patch_take_incoming(&other) && !memcmp(other.model.globals, patch.model.globals, sizeof(patch.model.globals)),
		"a saved state brings all 14 back");
	size_t old_size = 0;
	for (int i = 0; i < PATCH_NUM_IDS; i++) {
		uint32_t id;
		memcpy(&id, state + i * 6, 4);
		if (id < GLOBAL_CH3_NOTES || id >= GLOBAL_CH3_OP_END) {
			memcpy(old + old_size, state + i * 6, 6);
			old_size += 6;
		}
	}
	patch_init(&other);
	bool defaults = old_size == sizeof(state) - 14 * 6 && patch_load(&other, old, old_size, NULL, NULL, NULL, NULL) && patch_take_incoming(&other);
	for (int id = GLOBAL_CH3_NOTES; id < GLOBAL_CH3_OP_END; id++) {
		defaults = defaults && other.model.globals[id] == patch_param((uint32_t)id)->default_value;
	}
	CHECK(defaults && other.model.globals[GLOBAL_LFO_RATE] == patch.model.globals[GLOBAL_LFO_RATE], "a project of 0.20, without ids 10-23, opens with their defaults");
	clear();
	patch_rom_booted(&other, record, NULL);
	// That the rest is 0.20's bytes, byte for byte, reg_test's test_ch3_off checks against 0.20's patch.c.
	CHECK(count_ch3_ccs() == 14, "a booted ROM gets all 14 controllers, at their defaults too (%d)", count_ch3_ccs());

	mdmi_patch *files_model = &files_patch;
	patch_init(files_model);
	files_view = (state_view){0};
	const files_host host = {files_model, &files_view, take_now, NULL};
	static mdmi_files files;
	files_init(&files, &host);
	char message[FILES_MESSAGE], snap_path[MAX_PATH], bank_path[MAX_PATH];
	path_in(snap_path, sizeof(snap_path), temp_dir, "ch3.mdsnap");
	path_in(bank_path, sizeof(bank_path), temp_dir, "ch3.mdbank");
	patch_set(files_model, GLOBAL_CH3_NOTES, PATCH_CH3_ONE_NOTE);
	patch_set(files_model, GLOBAL_LFO_RESET, 4);
	patch_set(files_model, GLOBAL_CH3_OP(2, PATCH_CH3_FINE), 99);
	patch_publish(files_model);
	static patch_values snapped;
	snapped = files_model->model;
	CHECK(files_save(&files, snap_path, FILES_SNAPSHOT, 0, message, sizeof(message)) && files_save(&files, bank_path, FILES_BANK, 0, message, sizeof(message)), "a snapshot and a bank saved: %s", message);
	patch_init(files_model);
	patch_set(files_model, GLOBAL_LFO_RESET, 1);
	patch_publish(files_model);
	CHECK(files_open(&files, bank_path, 0, message, sizeof(message)) && files_model->model.globals[GLOBAL_LFO_RESET] == 1 && files_model->model.globals[GLOBAL_CH3_NOTES] == PATCH_CH3_OPS,
		"a bank file leaves them as they are: %s", message);
	CHECK(files_open(&files, snap_path, 0, message, sizeof(message)) && !memcmp(files_model->model.globals, snapped.globals, sizeof(snapped.globals)), "an .mdsnap brings them back: %s", message);
	files_free(&files);

	// Channel 3 plays alone in Ch3 Special in every mode; MIDI 11-13 notes pass to the ROM as they are.
	for (int mode = PATCH_CH3_OPS; mode <= PATCH_CH3_CSM; mode++) {
		patch_init(&patch);
		set(&patch, GLOBAL_CH3_MODE, 1);
		set(&patch, GLOBAL_CH3_NOTES, mode);
		clear();
		moment(&patch, 2, 60, 3, true);
		midi(&patch, 0x9A, 64, 100);
		const uint8_t op_note[3] = {0x9A, 64, 100};
		CHECK(count_sysex(0x00) == 0 && patch.voices[2].channel == 2 && free_voices(&patch) == 5 && patch_muted_notes(&patch, 2) == 0 && count_status(0x90) == 4 && sent_bytes >= 3 && !memcmp(sent + sent_bytes - 3, op_note, 3),
			"%s: a chord of 3 on MIDI 3 goes to the ROM on voice 3 alone, a MIDI 11 note goes through", mode == PATCH_CH3_CSM ? "CSM" : mode ? "One Note" : "Ops");
		moment(&patch, 2, 60, 3, false);
		midi(&patch, 0x8A, 64, 0);
	}
}

static bool same_file(const char *path, const uint8_t *bytes, size_t size)
{
	FILE *file = fopen(path, "rb");
	if (!file) {
		return false;
	}
	static uint8_t got[1 << 17];
	const size_t read = fread(got, 1, sizeof(got), file);
	fclose(file);
	return read == size && !memcmp(got, bytes, size);
}

// Libraries beside the plugin (stage 22c, D56): files of a folder, read and written place by place,
// never the ROM bank unless copied into it.
static void test_libraries(void)
{
	char folder[LIBRARY_PATH];
	library_folder_for("D:\\VST\\MDMI Slot.clap", folder, sizeof(folder));
	CHECK(!strcmp(folder, "D:\\VST\\MDMI Banks"), "beside a plugin file: %s", folder);
	library_folder_for("D:\\VST\\MDMI Slot.vst3\\Contents\\x86_64-win\\MDMI Slot.vst3", folder, sizeof(folder));
	CHECK(!strcmp(folder, "D:\\VST\\MDMI Banks"), "beside a VST3 bundle, never inside it: %s", folder);
	library_folder_for("C:/Plugins/x.vst3", folder, sizeof(folder));
	CHECK(!strcmp(folder, "C:/Plugins\\MDMI Banks"), "a single VST3 file: %s", folder);
	library_folder_for("D:\\\xD0\x9C\xD1\x83\xD0\xB7\xD1\x8B\xD0\xBA\xD0\xB0\\MDMI Slot.clap", folder, sizeof(folder));
	CHECK(!strcmp(folder, "D:\\\xD0\x9C\xD1\x83\xD0\xB7\xD1\x8B\xD0\xBA\xD0\xB0\\MDMI Banks"), "a path in Cyrillic");

	mdmi_patch *patch = &files_patch;
	patch_init(patch);
	files_view = (state_view){0};
	const files_host host = {patch, &files_view, take_now, NULL};
	static mdmi_files files;
	files_init(&files, &host);
	char message[FILES_MESSAGE];
	snprintf(folder, sizeof(folder), "%s\\libraries", temp_dir);
	// What a run before left.
	const char *const left[] = {"b drums.mdbank", "A Bass.MDBANK", "junk.mdbank", "notes.txt", "c.mdbank"};
	for (size_t i = 0; i < sizeof(left) / sizeof(*left); i++) {
		char path[LIBRARY_PATH];
		library_path(folder, left[i], path, sizeof(path));
		SetFileAttributesA(path, FILE_ATTRIBUTE_NORMAL);
		DeleteFileA(path);
	}
	char sub[LIBRARY_PATH];
	library_path(folder, "sub.mdbank", sub, sizeof(sub));
	RemoveDirectoryA(sub);
	RemoveDirectoryA(folder);
	library_list list;
	library_scan(folder, &list);
	CHECK(!list.found && list.count == 0, "no folder: nothing listed");
	CHECK(library_make_folder(folder) && library_make_folder(folder), "the folder made, twice is fine");
	char a[LIBRARY_PATH], b[LIBRARY_PATH], junk[LIBRARY_PATH], other[LIBRARY_PATH];
	library_path(folder, "b drums.mdbank", b, sizeof(b));
	library_path(folder, "A Bass.MDBANK", a, sizeof(a));
	library_path(folder, "junk.mdbank", junk, sizeof(junk));
	library_path(folder, "notes.txt", other, sizeof(other));
	CHECK(files_new_library(b, message, sizeof(message)) && files_new_library(a, message, sizeof(message)), "two new libraries: %s", message);
	FILE *file = fopen(other, "wb");
	fclose(file);
	file = fopen(junk, "wb");
	fputs("not a bank", file);
	fclose(file);
	CreateDirectoryA(sub, NULL);
	library_scan(folder, &list);
	CHECK(list.found && list.count == 3 && list.total == 3 && !strcmp(list.names[0], "A Bass.MDBANK") && !strcmp(list.names[1], "b drums.mdbank") && !strcmp(list.names[2], "junk.mdbank"),
		"only .mdbank files, by name ignoring case: %d %s", list.count, list.names[0]);

	// A new library is every place empty, and the same bytes a GM bank saves as.
	static patch_bank library, rom;
	patch_bank_rom(&rom);
	bool empty = files_read_library(a, &library);
	for (int place = 0; place < PATCH_BANK_SIZE; place++) {
		empty = empty && files_place_empty(&library, false, place) && files_place_empty(&library, true, place);
	}
	CHECK(empty, "a new library is all empty");
	char saved[LIBRARY_PATH];
	library_path(temp_dir, "gm.mdbank", saved, sizeof(saved));
	size_t size;
	uint8_t *bytes = state_bank_build(&rom, &size);
	CHECK(files_save(&files, saved, FILES_BANK, 0, message, sizeof(message)) && same_file(saved, bytes, size), "the ROM bank saved is the same file: a saved bank is a library");
	free(bytes);

	// The ROM bank's place into a library: the bank and the channels stay.
	CHECK(open_file(&files, files_dir, "Thunder Guitar 03.tyi", 0, 9, message), "a TYI in place 10: %s", message);
	const patch_arp row = {.count = 2, .loop = -1, .steps = {0, 7}};
	patch_load_place(patch, 9, &patch->bank.places[9], &row);
	static patch_bank bank_before;
	bank_before = *patch_bank_view(patch);
	patch_values values_before = patch->model;
	files_spot rom10 = {NULL, 9}, a5 = {a, 4}, b1 = {b, 0}, rom1 = {NULL, 0};
	CHECK(files_copy_place(&files, false, &rom10, &a5, NULL, message, sizeof(message)) && !strcmp(message, "Thunder Guitar 03 to A Bass 5"), "ROM bank to a library: %s", message);
	CHECK(files_read_library(a, &library) && !strcmp(library.places[4].name, "Thunder Guitar 03") && library.place_arps[4].count == 2
		&& !memcmp(library.places[4].voice, bank_before.places[9].voice, sizeof(library.places[4].voice)) && !files_place_empty(&library, false, 4),
		"the file has sound, name and row");
	CHECK(!memcmp(patch_bank_view(patch), &bank_before, sizeof(bank_before)) && same_model(&patch->model, &values_before), "the ROM bank and the channels stay");
	char temporary[LIBRARY_PATH + 8];
	snprintf(temporary, sizeof(temporary), "%s.tmp", a);
	CHECK(GetFileAttributesA(temporary) == INVALID_FILE_ATTRIBUTES, "no .tmp is left after a write");
	char expected[FILES_MESSAGE];
	// Stage 37: the place was empty, so its undo says so, not the GM name it held then (22c).
	snprintf(expected, sizeof(expected), "A Bass 5 back to empty");
	CHECK(files_undo_text(&files) && !strcmp(files_undo_text(&files), expected), "undo says: %s", files_undo_text(&files) ? files_undo_text(&files) : "nothing");
	CHECK(files_undo(&files, message, sizeof(message)) && files_read_library(a, &library) && files_place_empty(&library, false, 4), "undo empties it in the file: %s", message);
	CHECK(files_undo(&files, message, sizeof(message)) && files_read_library(a, &library) && !strcmp(library.places[4].name, "Thunder Guitar 03"), "and again brings it back");

	// Library to library, then into the ROM bank: a channel playing the place as it is sounds new.
	CHECK(files_copy_place(&files, false, &a5, &b1, NULL, message, sizeof(message)) && files_read_library(b, &library) && !strcmp(library.places[0].name, "Thunder Guitar 03"), "library to library: %s", message);
	CHECK(!strcmp(patch->model.names[0], "Grand Piano"), "channel 1 plays place 1");
	CHECK(files_copy_place(&files, false, &b1, &rom1, NULL, message, sizeof(message)) && !strcmp(patch->bank.places[0].name, "Thunder Guitar 03") && patch->bank.place_arps[0].count == 2 && !strcmp(patch->model.names[0], "Thunder Guitar 03"),
		"into the ROM bank, and channel 1 sounds it: %s", message);
	CHECK(files_undo(&files, message, sizeof(message)) && !strcmp(patch->bank.places[0].name, "Grand Piano") && !strcmp(patch->model.names[0], "Grand Piano"), "undo puts the place back: %s", message);
	CHECK(!files_copy_place(&files, false, &a5, &a5, NULL, message, sizeof(message)), "a place onto itself: %s", message);

	// Trying a place on a channel: as a dropped file, Preset stays.
	CHECK(files_try_place(&files, 3, &a5, message, sizeof(message)) && !strcmp(patch->model.names[3], "Thunder Guitar 03") && patch->model.channels[3][PARAM_PRESET] == 0 && patch_arp_view(patch, 3)->count == 2,
		"a library place tried on channel 4: %s", message);
	CHECK(files_undo(&files, message, sizeof(message)) && !strcmp(patch->model.names[3], "Grand Piano"), "undo gives the channel back");

	// Envelopes, and clearing.
	CHECK(files_put_places_one(&files, "PSG Organ.eef", 3, message), "an envelope in place 4: %s", message);
	files_spot rom4 = {NULL, 3}, b4 = {b, 3};
	CHECK(files_copy_place(&files, true, &rom4, &b4, NULL, message, sizeof(message)) && files_read_library(b, &library) && !strcmp(library.envelopes[3].name, "PSG Organ") && !strcmp(library.places[3].name, rom.places[3].name),
		"an envelope to a library, its FM place stays: %s", message);
	CHECK(files_clear_place(&files, true, &b4, message, sizeof(message)) && files_read_library(b, &library) && files_place_empty(&library, true, 3) && !strcmp(library.places[0].name, "Thunder Guitar 03"),
		"cleared, the other places stay: %s", message);
	CHECK(files_undo(&files, message, sizeof(message)) && files_read_library(b, &library) && !strcmp(library.envelopes[3].name, "PSG Organ"), "undo of a clear");

	// A file that isn't a bank is never written; a file that can't be written changes nothing.
	files_spot junk1 = {junk, 0};
	CHECK(!files_copy_place(&files, false, &rom10, &junk1, NULL, message, sizeof(message)) && same_file(junk, (const uint8_t *)"not a bank", 10), "no bank, not written: %s", message);
	CHECK(!files_try_place(&files, 0, &junk1, message, sizeof(message)) && !strcmp(patch->model.names[0], "Grand Piano"), "nor tried: %s", message);
	wchar_t wide[LIBRARY_PATH];
	MultiByteToWideChar(CP_UTF8, 0, a, -1, wide, LIBRARY_PATH);
	SetFileAttributesW(wide, FILE_ATTRIBUTE_READONLY);
	files_spot a6 = {a, 5};
	CHECK(!files_copy_place(&files, false, &rom10, &a6, NULL, message, sizeof(message)) && files_read_library(a, &library) && files_place_empty(&library, false, 5) && strstr(message, "Can't write"),
		"a read-only library: %s", message);
	CHECK(files_copy_place(&files, false, &a5, &rom1, NULL, message, sizeof(message)), "but it still gives its places: %s", message);
	SetFileAttributesW(wide, FILE_ATTRIBUTE_NORMAL);
	// Another plugin wrote another place since: both stay.
	static patch_bank elsewhere;
	files_read_library(a, &elsewhere);
	snprintf(elsewhere.places[20].name, PATCH_NAME, "Other");
	bytes = state_bank_build(&elsewhere, &size);
	file = fopen(a, "wb");
	fwrite(bytes, 1, size, file);
	fclose(file);
	free(bytes);
	CHECK(files_copy_place(&files, false, &rom10, &a6, NULL, message, sizeof(message)) && files_read_library(a, &library) && !strcmp(library.places[20].name, "Other") && !strcmp(library.places[5].name, "Thunder Guitar 03"),
		"the file is read again before a change: %s", message);
	// The file went: undo says so and keeps nothing.
	DeleteFileW(wide);
	CHECK(!files_undo(&files, message, sizeof(message)) && GetFileAttributesW(wide) == INVALID_FILE_ATTRIBUTES && !strcmp(message, "Can't undo: A Bass is gone"), "undo into a removed library: %s", message);

	// Review of stage 22c. Two instances add to a library's first free place, each without seeing the
	// other's: the place is chosen in the file as it is now, so neither is lost.
	static mdmi_files second;
	files_init(&second, &host);
	files_spot b_free = {b, -1}, rom3 = {NULL, 2};
	static patch_bank before_copy;
	files_read_library(b, &before_copy);
	int landed = -1, landed_too = -1;
	CHECK(files_copy_place(&second, false, &rom10, &b_free, &landed, message, sizeof(message)) && landed == 1, "another instance adds to the first free place: %d", landed);
	CHECK(files_copy_place(&files, false, &rom3, &b_free, &landed_too, message, sizeof(message)) && landed_too == 3, "this one, not told, adds after it, past place 3 where its GM sound would read as empty: %d", landed_too);
	// The file is the bank before with exactly those places: what SAVE of that bank writes.
	before_copy.places[1] = patch->bank.places[9];
	before_copy.place_arps[1] = patch->bank.place_arps[9];
	before_copy.places[3] = patch->bank.places[2];
	before_copy.place_arps[3] = patch->bank.place_arps[2];
	bytes = state_bank_build(&before_copy, &size);
	CHECK(same_file(b, bytes, size), "the file changed by those two places only, as a saved bank");
	free(bytes);
	// One place written by both: the last one stays.
	files_spot b11 = {b, 10};
	files_copy_place(&second, false, &rom10, &b11, NULL, message, sizeof(message));
	files_copy_place(&files, false, &rom3, &b11, NULL, message, sizeof(message));
	CHECK(files_read_library(b, &library) && !strcmp(library.places[10].name, patch->bank.places[2].name), "the same place: the last write stays");
	// A GM sound doesn't go to its own GM place, where it would read as empty and vanish.
	char c[LIBRARY_PATH];
	library_path(folder, "c.mdbank", c, sizeof(c));
	files_new_library(c, message, sizeof(message));
	const patch_arp none = PATCH_ARP_EMPTY;
	patch_load_place(patch, 0, &rom.places[0], &none);
	files_spot c_free = {c, -1};
	CHECK(files_copy_place(&files, false, &rom1, &c_free, &landed, message, sizeof(message)) && landed == 1 && files_read_library(c, &library) && !files_place_empty(&library, false, 1),
		"GM place 1 into a new library goes to place 2, not its own: %d", landed);
	// A library copied from leaves its file as it was.
	size_t c_size;
	uint8_t *c_bytes = state_bank_build(&library, &c_size);
	files_spot c2 = {c, 1}, rom20 = {NULL, 19};
	CHECK(files_copy_place(&files, false, &c2, &rom20, NULL, message, sizeof(message)) && same_file(c, c_bytes, c_size), "a library copied into the ROM bank stays as it was");
	// A read-only library: NEW over it and CLEAR are refused, the file stays.
	MultiByteToWideChar(CP_UTF8, 0, c, -1, wide, LIBRARY_PATH);
	SetFileAttributesW(wide, FILE_ATTRIBUTE_READONLY);
	CHECK(!files_clear_place(&files, false, &c2, message, sizeof(message)) && same_file(c, c_bytes, c_size), "CLEAR on a read-only library: %s", message);
	CHECK(!files_new_library(c, message, sizeof(message)) && same_file(c, c_bytes, c_size), "NEW over a read-only library: %s", message);
	SetFileAttributesW(wide, FILE_ATTRIBUTE_NORMAL);
	free(c_bytes);
	DeleteFileW(wide);
	files_free(&second);

	// Stage 22c adds no block to a project: libraries are never in it.
	files_view = (state_view){0};
	bytes = state_build(patch, &files_view, &size);
	bool known = bytes != NULL;
	for (size_t at = sizeof(uint32_t); known && at + 8 <= size;) {
		uint32_t type, length;
		memcpy(&type, bytes + at, 4);
		memcpy(&length, bytes + at + 4, 4);
		known = type >= 1 && type <= 11;
		at += 8 + length;
	}
	CHECK(known, "a 0.22.4 project has blocks 1-11 only");
	free(bytes);
	files_free(&files);
}

static uint8_t *read_whole(const char *path, size_t *size)
{
	FILE *file = fopen(path, "rb");
	if (!file) {
		return NULL;
	}
	fseek(file, 0, SEEK_END);
	*size = (size_t)ftell(file);
	fseek(file, 0, SEEK_SET);
	uint8_t *bytes = malloc(*size ? *size : 1);
	if (bytes && fread(bytes, 1, *size, file) != *size) {
		free(bytes);
		bytes = NULL;
	}
	fclose(file);
	return bytes;
}

static uint32_t be32(const uint8_t *at)
{
	return (uint32_t)at[0] << 24 | (uint32_t)at[1] << 16 | (uint32_t)at[2] << 8 | at[3];
}

// Stage 24: the image EXPORT writes, and names as the ROM's screen and the window show them.
static void test_export(void)
{
	char ascii[64];
	names_ascii("\xD0\x91\xD0\xB0\xD1\x81 \xD0\x96\xD0\xB8\xD1\x80\xD0\xBD\xD1\x8B\xD0\xB9.tfi", ascii, sizeof(ascii));
	CHECK(!strcmp(ascii, "Bas Zhirnyy.tfi"), "a Cyrillic name is transliterated: %s", ascii);
	names_ascii("\xD0\xA9\xD1\x91\xD1\x82\xD0\xBA\xD0\xB0 \xE2\x80\x94 x\xD0", ascii, sizeof(ascii));
	CHECK(!strcmp(ascii, "Shyotka ? x"), "two letters at most, other characters as ?, a cut letter dropped: %s", ascii);
	char cut[PATCH_NAME];
	// 23 bytes of Cyrillic and ASCII, the last letter cut in half at 24.
	snprintf(cut, sizeof(cut), "%s", "abcdefghijklmnopqrstuv\xD0\x96\xD0\x96");
	names_cut_utf8(cut);
	CHECK(!strcmp(cut, "abcdefghijklmnopqrstuv"), "a name cut at its 24 bytes ends on a whole letter: %s", cut);

	size_t rom_size, built;
	const uint8_t *rom = mdmi_engine_builtin_rom(&rom_size);
	CHECK(rom_image_check(rom, rom_size), "the built ROM's GM voices are as the plugin packs them, its rows empty");
	uint32_t voices, envelopes, rows;
	rom_image_tables(&voices, &envelopes, &rows);
	// Past the ROM's own data: the sample records and data start there, and what the bank adds.
	uint32_t table, table_ram, records;
	rom_image_zone(&table, &table_ram, &records);
	uint8_t *bad = malloc(rom_size);
	memcpy(bad, rom, rom_size);
	bad[be32(rom + voices + 4 * 9) + 3] ^= 0x08;
	CHECK(!rom_image_check(bad, rom_size), "an image whose voice 10 differs from GM is refused");
	free(bad);

	// The GM bank changes nothing but the names.
	static rom_bank gm;
	memset(&gm, 0, sizeof(gm));
	for (int place = 0; place < 128; place++) {
		memcpy(gm.voices[place], PRESETS_GM[place], ROM_VOICE_FIELDS);
		memcpy(gm.envelopes[place], rom + be32(rom + envelopes + 4 * place), ROM_ENVELOPE_BYTES);
		gm.arps[0][place].loop = gm.arps[1][place].loop = ROM_ARP_NO_LOOP;
	}
	const rom_sample *none[ROM_IMAGE_NOTES] = {0};
	uint8_t *same = rom_image_build(rom, rom_size, none, NULL, &gm, NULL, &built);
	CHECK(same && built == rom_size && !memcmp(same, rom, rom_size), "the image of the GM bank without samples is the emulator's");
	free(same);

	// Stage 37: samples that fill the room the plugin gives them still leave EXPORT the place for 128
	// envelopes of the user's at their longest, all different: no "over 4 MB" after the sample was taken.
	{
		static rom_bank full;
		full = gm;
		for (int place = 0; place < 128; place++) {
			memset(full.envelopes[place], place & 0x0F, ROM_ENVELOPE_BYTES - 1);
			full.envelopes[place][0] = (uint8_t)(place >> 4);
			full.envelopes[place][ROM_ENVELOPE_BYTES - 1] = 0xFF;
		}
		const uint32_t room = rom_image_room() & ~(uint32_t)(ROM_SAMPLE_BLOCK - 1);
		int8_t *bytes = calloc(room, 1);
		const rom_sample big = {.rate = 0, .length = room, .bytes = bytes};
		const rom_sample *one[ROM_IMAGE_NOTES] = {0};
		one[60] = &big;
		uint8_t *fits = rom_image_build(rom, rom_size, one, NULL, &full, NULL, &built);
		CHECK(fits != NULL && built <= ROM_IMAGE_MAX, "samples filling their room and 128 longest envelopes still make an image (%zu bytes)", built);
		free(fits);
		free(bytes);
	}

	// A bank: a voice on 5 with its row, an envelope of the user's on 3, a row switched off on 7.
	mdmi_patch *patch = &files_patch;
	patch_init(patch);
	static dac_set set;
	memset(&set, 0, sizeof(set));
	files_view = (state_view){0};
	files_view.dac = &set;
	const files_host host = {patch, &files_view, take_now, NULL};
	static mdmi_files files;
	files_init(&files, &host);
	patch_instrument voice = patch->bank.places[5];
	voice.voice[0] = 7;
	voice.voice[PATCH_VOICE_PARAMS - 1] = 3;
	snprintf(voice.name, sizeof(voice.name), "%s", "\xD0\x91\xD0\xB0\xD1\x81");
	patch_arp octave = PATCH_ARP_EMPTY, off = PATCH_ARP_EMPTY;
	octave.count = 2, octave.loop = 0, octave.steps[1] = 12;
	off.count = 1, off.steps[0] = 5, off.off = 1;
	patch_load_place(patch, 5, &voice, &octave);
	patch_load_place(patch, 7, &patch->bank.places[7], &off);
	static const uint8_t EEF[] = {0x00, PATCH_EEF_LOOP_START, 0x03, PATCH_EEF_LOOP_END, 0x0F, PATCH_EEF_END};
	patch_envelope envelope;
	patch_envelope_from_eef(&envelope, EEF, sizeof(EEF));
	snprintf(envelope.name, sizeof(envelope.name), "Mine");
	patch_load_envelope_place(patch, 3, &envelope, NULL);
	patch_take_incoming(patch);
	char path[MAX_PATH], message[FILES_MESSAGE];
	snprintf(path, sizeof(path), "%s\\export.bin", temp_dir);
	CHECK(files_export(&files, path, message, sizeof(message)) && !strncmp(message, "ROM ", 4), "EXPORT: %s", message);
	size_t size;
	uint8_t *image = read_whole(path, &size);
	uint8_t packed[ROM_VOICE_BYTES], fields[ROM_VOICE_FIELDS];
	patch_voice_fields(voice.voice, fields);
	rom_image_pack_voice(fields, packed);
	const uint8_t *row5 = image ? image + rows + 5 * (2 + ROM_ARP_STEPS) : NULL, *row7 = image ? image + rows + 7 * (2 + ROM_ARP_STEPS) : NULL;
	const uint32_t at3 = image ? be32(image + envelopes + 4 * 3) : 0;
	CHECK(image && size % (512u << 10) == 0 && !memcmp(image + be32(image + voices + 4 * 5), packed, sizeof(packed)), "place 5's voice is in the image where GM's was");
	CHECK(image && row5[0] == 2 && row5[1] == 0 && row5[2] == 0 && row5[3] == 12, "its row is in the table");
	CHECK(image && row7[0] == 0 && row7[1] == ROM_ARP_NO_LOOP, "a row switched off goes with no steps");
	CHECK(image && at3 >= records && at3 + sizeof(EEF) <= size && !memcmp(image + at3, EEF, sizeof(EEF)), "the user's envelope is past the ROM's data and place 3 points at it");
	const rom_names *names = image ? (const rom_names *)(image + rom_image_names_at()) : NULL;
	CHECK(names && !strcmp(names->names[0][5], "Bas") && !strcmp(names->names[1][3], "Mine") && !strcmp(names->names[0][0], "Grand Piano")
		&& !strncmp(names->signature, "BANK ", 5), "names: %s, %s, %s; %s", names ? names->names[0][5] : "", names ? names->names[1][3] : "", names ? names->names[0][0] : "", names ? names->signature : "");
	// The window's signature is the one in the file.
	static rom_names screen;
	CHECK(files_screen_names(&files, &screen) && names && !memcmp(screen.signature, names->signature, ROM_SIGNATURE_BYTES), "the ROM page shows the file's signature: %s", screen.signature);
	free(image);

	// A channel with a file not in the bank is named; the file is still written.
	patch_instrument dropped = voice;
	snprintf(dropped.name, sizeof(dropped.name), "Slap");
	dropped.voice[0] = 2;
	patch_load_voice(patch, 1, &dropped, NULL);
	patch_take_incoming(patch);
	CHECK(files_export(&files, path, message, sizeof(message)) && strstr(message, "CH2 Slap not in ROM"), "a channel not from the bank: %s", message);
	// A read-only file isn't touched.
	wchar_t wide[MAX_PATH];
	MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, MAX_PATH);
	SetFileAttributesW(wide, FILE_ATTRIBUTE_READONLY);
	uint8_t *was = read_whole(path, &size);
	CHECK(!files_export(&files, path, message, sizeof(message)) && strstr(message, "NOT WRITTEN"), "a read-only file: %s", message);
	size_t now_size;
	uint8_t *now = read_whole(path, &now_size);
	CHECK(was && now && now_size == size && !memcmp(was, now, size), "and its bytes stay");
	free(was);
	free(now);
	SetFileAttributesW(wide, FILE_ATTRIBUTE_NORMAL);
	DeleteFileW(wide);
	files_free(&files);
}

// Stage 40: CSM vowels, channel 3's four operators as formants (hardware-specialist,
// docs/research/stages-35-38-40.md): Semi and Fine as knob values (64 is 0), Fixed on, TL, RR 11, 11,
// 12, 12; algorithm 7, FB 0, MUL 1, DT 0, AR 31, the rest 0; Ch3 Special on, Ch3 Notes CSM.
typedef struct {
	const char *letter;
	int semi[4], fine[4], tl[4];
	double formant[4];
} vowel;
static const vowel VOWELS[5] = {
	{"A", {81, 90, 103, 109}, {70, 102, 73, 58}, {8, 11, 33, 44}, {700, 1200, 2500, 3500}},
	{"O", {75, 84, 103, 108}, {86, 106, 73, 106}, {5, 14, 40, 48}, {500, 850, 2500, 3400}},
	{"U", {66, 80, 102, 108}, {102, 42, 102, 54}, {3, 24, 46, 52}, {300, 650, 2400, 3300}},
	{"I", {63, 101, 106, 109}, {86, 91, 89, 104}, {4, 28, 25, 33}, {250, 2250, 3000, 3600}},
	{"E", {73, 97, 103, 109}, {104, 104, 107, 58}, {5, 22, 24, 34}, {450, 1800, 2550, 3500}},
};
static const int VOWEL_RR[4] = {11, 11, 12, 12};

// The snapshot of a vowel into path, from a fresh model.
static bool vowel_snapshot(const vowel *v, const char *path, char *message, size_t size)
{
	static mdmi_patch patch;
	static dac_set set;
	static state_view view;
	patch_init(&patch);
	memset(&set, 0, sizeof(set));
	view = (state_view){.page = 0, .channel = 2, .dac = &set};
	const files_host host = {&patch, &view, take_now, NULL};
	static mdmi_files files;
	files_init(&files, &host);
	patch_instrument voice = {0};
	snprintf(voice.name, sizeof(voice.name), "Vowel %s", v->letter);
	voice.voice[PARAM_ALGORITHM - PARAM_ALGORITHM] = 7;
	for (int op = 0; op < PATCH_OPS; op++) {
		int16_t *o = voice.voice + (PARAM_OP_BASE - PARAM_ALGORITHM) + op * PATCH_OP_PARAMS;
		o[OP_TL] = (int16_t)v->tl[op];
		o[1] = 1;   // MUL
		o[4] = 31;  // AR
		o[8] = (int16_t)VOWEL_RR[op];
	}
	patch_load_voice(&patch, 2, &voice, &voice);
	patch_take_incoming(&patch);
	patch_set(&patch, GLOBAL_CH3_MODE, 1);
	patch_set(&patch, GLOBAL_CH3_NOTES, PATCH_CH3_CSM);
	for (int op = 0; op < PATCH_OPS; op++) {
		patch_set(&patch, (uint32_t)GLOBAL_CH3_OP(op, PATCH_CH3_SEMI), v->semi[op] - 64 + PATCH_CH3_SEMI_MAX);
		patch_set(&patch, (uint32_t)GLOBAL_CH3_OP(op, PATCH_CH3_FINE), v->fine[op] - 64 + PATCH_CH3_FINE_MAX);
		patch_set(&patch, (uint32_t)GLOBAL_CH3_OP(op, PATCH_CH3_FIXED), 1);
	}
	patch_publish(&patch);
	const bool saved = files_save(&files, path, FILES_SNAPSHOT, -1, message, size);
	files_free(&files);
	return saved;
}

// The five snapshots in the repository (presets/vowels) are what the generator makes; with
// MDMI_WRITE_VOWELS=<folder> it writes them there instead. Each opens with Ch3 Special in CSM and the
// table's operators.
static void test_vowels(void)
{
	printf("CSM vowels\n");
	const char *write = getenv("MDMI_WRITE_VOWELS");
	char dir[MAX_PATH], message[FILES_MESSAGE];
	snprintf(dir, sizeof(dir), "%s\\..\\..\\..\\presets\\vowels", files_dir);
	for (int i = 0; i < 5; i++) {
		const vowel *v = &VOWELS[i];
		char name[64], made[MAX_PATH], kept[MAX_PATH];
		snprintf(name, sizeof(name), "CSM vowel %s.mdsnap", v->letter);
		if (write) {
			path_in(made, sizeof(made), write, name);
			CHECK(vowel_snapshot(v, made, message, sizeof(message)), "vowel %s written: %s", v->letter, message);
			continue;
		}
		GetTempPathA(sizeof(made), made);
		strcat(made, name);
		path_in(kept, sizeof(kept), dir, name);
		size_t made_size = 0, kept_size = 0;
		const bool built = vowel_snapshot(v, made, message, sizeof(message));
		uint8_t *a = read_whole(made, &made_size), *b = read_whole(kept, &kept_size);
		CHECK(built && a && b && made_size == kept_size && !memcmp(a, b, made_size), "presets/vowels/%s is the generator's (%zu bytes)", name, kept_size);
		free(a);
		free(b);
		static mdmi_patch opened;
		static dac_set set;
		static state_view view;
		patch_init(&opened);
		memset(&set, 0, sizeof(set));
		view = (state_view){.dac = &set};
		const files_host host = {&opened, &view, take_now, NULL};
		static mdmi_files files;
		files_init(&files, &host);
		bool same = files_open(&files, kept, 0, message, sizeof(message)) && patch_take_incoming(&opened);
		const int16_t *g = opened.model.globals;
		same = same && g[GLOBAL_CH3_MODE] == 1 && g[GLOBAL_CH3_NOTES] == PATCH_CH3_CSM && opened.model.channels[2][PARAM_ALGORITHM] == 7 && !strncmp(opened.model.names[2], "Vowel ", 6) && !strcmp(opened.model.names[2] + 6, v->letter);
		for (int op = 0; op < PATCH_OPS && same; op++) {
			same = g[GLOBAL_CH3_OP(op, PATCH_CH3_SEMI)] == v->semi[op] - 64 + PATCH_CH3_SEMI_MAX && g[GLOBAL_CH3_OP(op, PATCH_CH3_FINE)] == v->fine[op] - 64 + PATCH_CH3_FINE_MAX
				&& g[GLOBAL_CH3_OP(op, PATCH_CH3_FIXED)] == 1 && opened.model.channels[2][op_param(op, OP_TL)] == v->tl[op];
		}
		CHECK(same, "vowel %s opens: Ch3 Special in CSM, algorithm 7, the table's Semi, Fine, Fixed and TL", v->letter);
		files_free(&files);
	}
}

int main(int argc, char **argv)
{
	if (argc > 1) {
		files_dir = argv[1];
	}
	setvbuf(stdout, NULL, _IONBF, 0);
	patch_global_init();
	test_nothing_to_send();
	test_preset_and_tweaks();
	test_shadowed_presets();
	test_sustain_rate();
	test_modes();
	test_mono();
	test_vowels();
	test_noise();
	test_incoming();
	test_load();
	test_master();
	test_dealer();
	test_files();
	test_places();
	test_envelopes();
	test_drawing();
	test_sound_changed();
	test_bases();
	test_arps();
	test_ch3_ops();
	test_cursor();
	test_fm_envelope();
	test_dac();
	test_dac_files();
	test_dac_edit();
	test_dac_marks();
	test_dac_old_project();
	test_dac_moves();
	test_libraries();
	test_export();
	test_threads();
	printf("%s: %d failed\n", failures ? "FAILED" : "PASSED", failures);
	return failures;
}
