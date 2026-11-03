// The model without an emulator: what patch.c sends for each thing that can happen, and whether
// the main thread can ever read half a preset while the writer works.
//
//   patch_test [folder of the test instrument files]
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>
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
	patch_save(&patch, state);
	CHECK(patch_load(&other, state, sizeof(state), NULL), "a saved state loads");
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
	CHECK(patch_load(&other, odd, sizeof(odd), NULL) && patch_take_incoming(&other) && other.model.globals[GLOBAL_LFO_RATE] == 7
		&& other.model.globals[GLOBAL_RETIRED] == 0, "the old Polyphony id is skipped, a value out of range clamped");
	CHECK(other.model.channels[0][PARAM_PRESET] == 0, "and a knob the state lacks gets its default");
	CHECK(!patch_load(&other, odd, 7, NULL), "a cut state is refused");
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
			patch_load(&stress, loaded, sizeof(loaded), NULL);
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
	return files_open(files, path, channel, place, message, FILES_MESSAGE);
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
	CHECK(files_open(&files, bank_path, 0, -1, message, sizeof(message)) && !memcmp(&patch->bank, &thunder, sizeof(thunder)) && !memcmp(patch_bank_view(patch), &thunder, sizeof(thunder)), "and loaded back: %s", message);
	CHECK(files_undo(&files, message, sizeof(message)) && !strcmp(patch->bank.places[20].name, "Duck Guitar"), "undo of a bank puts the bank before it back");
	files_undo(&files, message, sizeof(message));

	// A snapshot: bank, channels and their values.
	open_file(&files, files_dir, "GTR_Dis._'94.vgi", 2, -1, message);
	patch_set(patch, PATCH_CHANNEL_ID(2, PARAM_OP_BASE), 33);
	patch_set(patch, PATCH_CHANNEL_ID(7, PARAM_VOLUME), 50);
	patch_set(patch, GLOBAL_LFO_RATE, 6);
	patch_publish(patch);
	files_view = (state_view){0, 2};
	patch_values snapped = patch->model;
	CHECK(files_save(&files, snap_path, FILES_SNAPSHOT, 0, message, sizeof(message)), "a snapshot saved: %s", message);
	open_file(&files, files_dir, "BAS_PSG2.vgi", 2, -1, message);
	open_file(&files, files_dir, "BAS_PSG2.vgi", 0, 9, message);
	patch_set(patch, PATCH_CHANNEL_ID(7, PARAM_VOLUME), 100);
	patch_set(patch, GLOBAL_LFO_RATE, 1);
	patch_set(patch, PATCH_CHANNEL_ID(0, PARAM_PRESET), 9);
	patch_publish(patch);
	files_view = (state_view){1, 5};
	handed_knobs = 0;
	CHECK(files_open(&files, snap_path, 0, -1, message, sizeof(message)) && same_model(&patch->model, &snapped) && !memcmp(&patch->bank, &thunder, sizeof(thunder)), "and loaded back: values, names and bank: %s", message);
	CHECK(files_view.page == 0 && files_view.channel == 2 && handed_knobs == 1, "with the view, and the host told to re-read the knobs");

	// A channel saved as VGI is the same voice when it comes back.
	patch_set(patch, PATCH_CHANNEL_ID(0, PARAM_PRESET), 9);
	patch_publish(patch);
	CHECK(files_save(&files, vgi_path, FILES_INSTRUMENT, 0, message, sizeof(message)), "channel 1 saved as VGI: %s", message);
	CHECK(files_open(&files, vgi_path, 4, -1, message, sizeof(message)) && !memcmp(patch->model.channels[4] + PARAM_ALGORITHM, patch->model.channels[0] + PARAM_ALGORITHM, PATCH_VOICE_PARAMS * sizeof(int16_t)) && !strcmp(patch->model.names[4], "Thunder again"),
		"and loads on channel 5 as the same voice");
	CHECK(!files_save(&files, vgi_path, FILES_INSTRUMENT, 7, message, sizeof(message)), "a PSG channel has no instrument to save: %s", message);
	files_free(&files);
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
	test_threads();
	printf("%s: %d failed\n", failures ? "FAILED" : "PASSED", failures);
	return failures;
}
