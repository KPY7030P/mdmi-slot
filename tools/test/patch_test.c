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
	CHECK(sent[0] == 0xB1 && sent[1] == 123 && sent[3] == 0xFF && count_voices() == 6 && count_sysex(0x03) == 1, "the held note stops, a reset goes through, then static mapping and every voice, %d", count_voices());
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
	patch_save(patch_view(&patch), state);
	CHECK(patch_load(&other, state, sizeof(state), NULL, NULL, NULL), "a saved state loads");
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
	CHECK(patch_load(&other, odd, sizeof(odd), NULL, NULL, NULL) && patch_take_incoming(&other) && other.model.globals[GLOBAL_LFO_RATE] == 7
		&& other.model.globals[GLOBAL_RETIRED] == 0, "the old Polyphony id is skipped, a value out of range clamped");
	CHECK(other.model.channels[0][PARAM_PRESET] == 0, "and a knob the state lacks gets its default");
	CHECK(!patch_load(&other, odd, 7, NULL, NULL, NULL), "a cut state is refused");
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
			patch_load(&stress, loaded, sizeof(loaded), NULL, NULL, NULL);
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

static void walk_note_off(rom_walk *w)
{
	if (w->loop >= 0) {
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
	CHECK(patch_shape_position(&organ, 2, 0) == -1, "Note Off before the loop began is silent at once, as the ROM does");
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
	return crossings > 1 ? (crossings - 1) * ROM_SAMPLE_HZ[s->sample.rate] / (last - first) : 0;
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
		const double seconds = s ? s->sample.length / ROM_SAMPLE_HZ[s->sample.rate] : 0;
		CHECK(s && s->sample.rate == WAVS[i].index && s->sample.length % ROM_SAMPLE_BLOCK == 0 && fabs(hz / 440 - 1) < 0.005 && peak >= 60 && peak <= 66 && seconds >= 0.5 && seconds < 0.5 + 256 / ROM_SAMPLE_HZ[WAVS[i].index] + 0.001,
			"WAV %u Hz, %d ch, %d bit%s: driver rate %d, %.1f Hz, peak %d, %.3f s", WAVS[i].rate, WAVS[i].channels, WAVS[i].bits, WAVS[i].is_float ? " float" : "", s ? s->sample.rate : -1, hz, peak, seconds);
		free(s);
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
	memset(bad + 0x200000, 0x55, rom_size - 0x200000);
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
	test_incoming();
	test_load();
	test_dealer();
	test_files();
	test_places();
	test_envelopes();
	test_drawing();
	test_sound_changed();
	test_bases();
	test_cursor();
	test_fm_envelope();
	test_dac();
	test_dac_files();
	test_threads();
	printf("%s: %d failed\n", failures ? "FAILED" : "PASSED", failures);
	return failures;
}
