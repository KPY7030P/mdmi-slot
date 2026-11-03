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
	CHECK(sent_bytes == 2 && sent[0] == 0xC6 && sent[1] == 62, "a PSG channel gets its program");
	clear();
	midi(&patch, 0xC6, 62, 0);
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
	CHECK(count_voices() == 6 && sent[0] == 0xB6 && sent[1] == 78, "the DAC comes first, on channel 7 where it applies once, then all six voices");
	clear();
	midi(&patch, 0xB0, 78, 0);
	CHECK(count_voices() == 6 && patch.model.globals[GLOBAL_DAC] == 0, "the same from a clip's CC78");
	clear();
	midi(&patch, 0xB3, 74, 0);
	CHECK(sent_bytes == 3 && sent[0] == 0xB6 && sent[1] == 74 && patch.model.globals[GLOBAL_LFO_ON] == 0, "LFO from any channel goes to channel 7");
	clear();
	midi(&patch, 0xB2, 84, 127);
	CHECK(sent_bytes == 0, "CC84 stops here: it would let any note take any voice");
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
	CHECK(sent[0] == 0xFF && count_voices() == 6 && count_sysex(0x03) == 1, "a reset goes through, then static mapping and every voice, %d", count_voices());
	CHECK(patch.model.channels[2][op_param(0, OP_TL)] == 99, "the model survives the reset");
	clear();
	const uint8_t sysex[6] = {0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7};
	patch_midi(&patch, sysex, sizeof(sysex), record, NULL);
	CHECK(memcmp(sent, sysex, sizeof(sysex)) == 0 && count_voices() == 6 && count_sysex(0x03) == 1, "a GM reset goes through, then static mapping and every voice");
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
	patch_save(&patch, state);
	CHECK(patch_load(&other, state, sizeof(state)), "a saved state loads");
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
	CHECK(patch_load(&other, odd, sizeof(odd)) && patch_take_incoming(&other) && other.model.globals[GLOBAL_LFO_RATE] == 7
		&& other.model.globals[GLOBAL_RETIRED] == 0, "the old Polyphony id is skipped, a value out of range clamped");
	CHECK(other.model.channels[0][PARAM_PRESET] == 0, "and a knob the state lacks gets its default");
	CHECK(!patch_load(&other, odd, 7), "a cut state is refused");
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
		patch_save(&stress, state);
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
