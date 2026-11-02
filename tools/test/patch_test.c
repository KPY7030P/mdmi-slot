// The model without an emulator: what patch.c sends for each thing that can happen, and whether
// the main thread can ever read half a preset while the writer works.
//
//   patch_test
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include "patch.h"
#include "presets_gm.h"

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

static int op_id(int op, int field)
{
	return PARAM_OP_BASE + op * OP_FIELDS + field;
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
	set(&patch, PARAM_PRESET, 5);
	CHECK(sent_bytes == 2 && sent[0] == 0xC0 && sent[1] == 5, "a new preset is one plain Program Change, %d bytes", (int)sent_bytes);

	clear();
	int tweaked = PRESETS_GM[5][4 + 9] > 60 ? 10 : 100;
	set(&patch, op_id(0, OP_TL), tweaked);
	CHECK(sent_bytes == 3 && sent[0] == 0xB0 && sent[1] == 16 && sent[2] == tweaked, "a tweak is one CC");

	clear();
	set(&patch, PARAM_PRESET, 5);
	CHECK(sent_bytes == 0 && patch.model.channels[0][op_id(0, OP_TL) - PARAM_PRESET] == tweaked, "a lane repeating the preset keeps the tweak and sends nothing");

	clear();
	midi(&patch, 0xC0, 5, 0);
	CHECK(count_status(0xC0) == 0 && count_status(0xB0) == 1 && sent[2] == PRESETS_GM[5][4 + 9], "a program from a clip wipes the tweak, with one CC and no program");

	clear();
	midi(&patch, 0xC0, 5, 0);
	CHECK(sent_bytes == 0, "the same program again sends nothing, %d bytes", (int)sent_bytes);

	clear();
	midi(&patch, 0xC6, 62, 0);
	CHECK(sent_bytes == 2 && sent[0] == 0xC6 && sent[1] == 62, "a PSG channel gets its program");
	clear();
	midi(&patch, 0xC6, 62, 0);
	CHECK(sent_bytes == 0, "and only once");
}

static void test_shadowed_presets(void)
{
	printf("Presets under the service numbers\n");
	static mdmi_patch patch;
	patch_init(&patch);
	clear();
	set(&patch, PARAM_PRESET, 120);
	CHECK(count_voices() == 1 && count_status(0xC0) == 1 && sent[sent_bytes - 2] == 0xC0 && sent[sent_bytes - 1] == 116, "preset 120 goes as a voice under service number 116, %d", sent[sent_bytes - 1]);
	CHECK(memcmp(sent + 7, PRESETS_GM[120], 48) == 0, "the voice bytes are the preset's");
	clear();
	set(&patch, PARAM_PRESET, 121);
	CHECK(count_voices() == 1 && sent[sent_bytes - 1] == 117, "the next voice takes the channel's other number, %d", sent[sent_bytes - 1]);
	clear();
	set(&patch, PARAM_PRESET, 3);
	CHECK(sent_bytes == 2 && sent[0] == 0xC0, "back below them is a plain program again");
}

static void test_sustain_rate(void)
{
	printf("Sustain rate above what a CC reaches\n");
	static mdmi_patch patch;
	patch_init(&patch);
	clear();
	set(&patch, op_id(1, OP_SR), 25);
	// Operator 2 of channel 1 lives in register 0x78, because registers run 1, 3, 2, 4.
	static const uint8_t expected[10] = {0xF0, 0x00, 0x22, 0x77, 0x08, 0x07, 0x08, 0x01, 0x09, 0xF7};
	CHECK(sent_bytes == 10 && memcmp(sent, expected, 10) == 0, "SR 25 on channel 1 is a register write");
	clear();
	set(&patch, op_id(1, OP_SR), 25);
	CHECK(sent_bytes == 0, "and only once");
	clear();
	set(&patch, op_id(1, OP_SR), 7);
	CHECK(sent_bytes == 3 && sent[0] == 0xB0, "SR 7 is a CC");
	clear();
	patch_set(&patch, PARAM_EDIT_CHANNEL, 4);
	set(&patch, op_id(0, OP_SR), 31);
	CHECK(sent_bytes == 10 && sent[4] == 0x09 && sent[6] == 0x01, "channel 5 uses the chip's second part");
	clear();
	patch_set(&patch, PARAM_EDIT_CHANNEL, 0);
	patch_set(&patch, PARAM_POLYPHONY, 1);
	sync(&patch);
	clear();
	set(&patch, op_id(0, OP_SR), 28);
	CHECK(count_status(0xF0) == 6, "with polyphony it goes to all six voices, %d", count_status(0xF0));
}

static void test_modes(void)
{
	printf("Polyphony and DAC\n");
	static mdmi_patch patch;
	patch_init(&patch);
	clear();
	set(&patch, PARAM_POLYPHONY, 1);
	CHECK(count_voices() == 1, "polyphony on sends channel 1 whole, %d voices", count_voices());
	clear();
	set(&patch, PARAM_POLYPHONY, 0);
	CHECK(count_voices() == 6, "polyphony off sends all six whole, %d voices", count_voices());
	clear();
	sync(&patch);
	CHECK(sent_bytes == 0, "after that nothing is left to send");
	clear();
	set(&patch, PARAM_DAC, 1);
	CHECK(count_voices() == 6 && sent[0] == 0xB0 && sent[1] == 78, "the DAC comes first, then all six voices");
	clear();
	midi(&patch, 0xB0, 78, 0);
	CHECK(count_voices() == 6 && patch.model.globals[PATCH_NUM_GLOBALS - 1] == 0, "the same from a clip's CC78");
}

static void test_incoming(void)
{
	printf("Messages from the host\n");
	static mdmi_patch patch;
	patch_init(&patch);
	clear();
	midi(&patch, 0xB2, 16, 99);
	CHECK(sent_bytes == 3 && sent[0] == 0xB2 && sent[2] == 99 && patch.model.channels[2][op_id(0, OP_TL) - PARAM_PRESET] == 99, "a CC goes into its channel's model and out once");
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
	CHECK(count_status(0xE0) == 1 && count_status(0xB0) == 1 && sent[4] == 64 && patch.model.channels[2][op_id(0, OP_TL) - PARAM_PRESET] == 99, "reset all controllers keeps the voice");
	clear();
	midi(&patch, 0xB3, 77, 64);
	CHECK(sent_bytes == 3 && sent[1] == 10 && sent[2] == 0, "stereo bits move the Pan knob");
	clear();
	const uint8_t reset = 0xFF;
	patch_midi(&patch, &reset, 1, record, NULL);
	CHECK(sent[0] == 0xFF && count_voices() == 6, "a reset goes through and every voice follows, %d", count_voices());
	CHECK(patch.model.channels[2][op_id(0, OP_TL) - PARAM_PRESET] == 99, "the model survives the reset");
	clear();
	const uint8_t sysex[6] = {0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7};
	patch_midi(&patch, sysex, sizeof(sysex), record, NULL);
	CHECK(memcmp(sent, sysex, sizeof(sysex)) == 0 && count_voices() == 6, "SysEx we can't read goes through and every voice follows");
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
	set(&patch, PARAM_PRESET, 9);
	set(&patch, op_id(2, OP_TL), 77);
	patch_publish(&patch);
	uint8_t state[sizeof(uint32_t) + sizeof(patch_values)];
	patch_save(&patch, state);
	CHECK(patch_load(&other, state, sizeof(state)), "a saved state loads");
	CHECK(patch_get(&other, PARAM_PRESET) == 9, "the main thread sees it before it is taken");
	CHECK(other.model.channels[0][0] == 0, "the writer's model is untouched until then");
	clear();
	CHECK(patch_take_incoming(&other) && !patch_take_incoming(&other), "it is taken once");
	sync(&other);
	CHECK(count_status(0xC0) == 1 && count_status(0xB0) == 1 && count_voices() == 0, "the ROM gets only the difference");
	CHECK(patch_get(&other, PARAM_PRESET) == 9 && patch_get(&other, op_id(2, OP_TL)) == 77, "the snapshot holds it afterwards");
	state[sizeof(uint32_t)] = 100;
	CHECK(!patch_load(&other, state, sizeof(state)), "a value out of range is refused");
}

static void discard(void *context, const uint8_t *bytes, size_t length)
{
	(void)context;
	(void)bytes;
	(void)length;
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
		patch_set(&stress, PARAM_PRESET, (program * 3) & 63);
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
	static uint8_t state[sizeof(uint32_t) + sizeof(patch_values)];
	static uint8_t loaded[sizeof(uint32_t) + sizeof(patch_values)];
	int torn = 0, reads = 0;
	DWORD start = GetTickCount();
	while (GetTickCount() - start < STRESS_MS) {
		patch_save(&stress, state);
		const patch_values *values = (const patch_values *)(state + sizeof(uint32_t));
		const int16_t *channel = values->channels[0];
		int preset = channel[0];
		bool same = channel[PARAM_ALGORITHM - PARAM_PRESET] == PRESETS_GM[preset][0];
		for (int op = 0; op < PATCH_OPS; op++) {
			same = same && channel[op_id(op, OP_TL) - PARAM_PRESET] == PRESETS_GM[preset][4 + op * OP_FIELDS + 9];
		}
		if (!same) {
			torn++;
		}
		reads++;
		if (reads % 64 == 0) {
			memcpy(loaded, state, sizeof(loaded));
			patch_load(&stress, loaded, sizeof(loaded));
		}
	}
	stop_writer = 1;
	WaitForSingleObject(writer, INFINITE);
	CloseHandle(writer);
	CHECK(torn == 0 && reads > 1000, "%d of %d saved states hold half a preset", torn, reads);
}

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	patch_global_init();
	test_nothing_to_send();
	test_preset_and_tweaks();
	test_shadowed_presets();
	test_sustain_rate();
	test_modes();
	test_incoming();
	test_load();
	test_threads();
	printf("%s: %d failed\n", failures ? "FAILED" : "PASSED", failures);
	return failures;
}
