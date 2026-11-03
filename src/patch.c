#include <math.h>
#include <stdio.h>
#include <string.h>
#include "patch.h"
#include "presets_gm.h"

#define CC_LFO_RATE 1
#define CC_PRESET 3
#define CC_PORTA_TIME 5
#define CC_DATA_ENTRY_MSB 6
#define CC_VOLUME 7
#define CC_PAN 10
#define CC_ALGORITHM 14
#define CC_FEEDBACK 15
#define CC_DATA_ENTRY_LSB 38
#define CC_PORTA_ON 65
#define CC_LFO_ON 74
#define CC_FMS 75
#define CC_AMS 76
#define CC_DAC 78
#define CC_CH3_MODE 80
#define CC_POLYPHONY 84
#define CC_FINE_TUNE 85
#define CC_RPN_LSB 100
#define CC_RPN_MSB 101
#define CC_ON 127
#define RPN_NULL 127
#define OP_SSG_FIELD 10
#define SSG_FIRST_ENVELOPE 8
#define BEND_RANGE_MAX 24
#define CC_SUSTAIN 64
#define CC_STEREO 77
#define CC_RESET_ALL 121
#define CC_STORE_PROGRAM 87
#define YM_REG_SUSTAIN_RATE 0x70
#define SYSEX_REMAP 0x00
#define SYSEX_DYNAMIC 0x03
#define CC_ALL_NOTES_OFF 123
#define CC_ALL_SOUND_OFF 120
// The ROM applies LFO and DAC controllers once per voice of the channel they come on. Channel 7's
// PSG voice is never remapped, so there they apply exactly once whatever Polyphony does.
#define GLOBAL_CC_CHANNEL 6
// Stored voices shadow the ROM's built-in programs from here on, two numbers per FM channel.
#define FIRST_SERVICE_PROGRAM 116
#define VOICE_SYSEX_HEAD 7
#define VOICE_BYTES 48
#define PRESETS 128
#define SNAPSHOT_INDEX 3u
#define SNAPSHOT_NEW 4u
#define SAVED_PAIR_BYTES 6
// More differences than this go as a whole voice (D24). reg_test measures what a note waits for: 4
// controllers 4.7 ms, 8 7.3 ms, 12 9.7 ms, a whole voice by SysEx 0A 9.3-9.7 ms.
#define WHOLE_VOICE_DIFFERENCES 12
enum { INCOMING_FREE, INCOMING_WRITING, INCOMING_READY, INCOMING_TAKING };

#define FILTER_MIN_HZ 500.0
#define FILTER_RANGE 40.0
#define FILTER_MAX 1000
#define FILTER_OFF_HZ 1000000.0
// Closest step to the 3390Hz low-pass of the original Mega Drive.
#define FILTER_DEFAULT 519

// The ROM scales each CC with value / (128 / range), so a knob value v goes out as v * (128 / range).
typedef struct {
	const char *name;
	uint8_t cc;
	uint8_t cc_range;
	int16_t max;
	uint8_t preset_field;
} op_param;

// preset_field is the position in the ROM's Operator struct, which PRESETS_GM follows.
static const op_param OP_PARAMS[PATCH_OP_PARAMS] = {
	{"TL", 16, 128, 127, 9},
	{"MUL", 20, 16, 15, 0},
	{"DT", 24, 8, 7, 1},
	{"RS", 39, 4, 3, 3},
	{"AR", 43, 32, 31, 2},
	{"DR", 47, 32, 31, 4},
	// Presets use all 32 sustain rates, but the ROM's CC only reaches the first 16.
	{"SR", 51, 16, 31, 7},
	{"SL", 55, 16, 15, 6},
	{"RR", 59, 16, 15, 8},
	{"AM", 70, 2, 1, 5},
	{"SSG", 90, 16, 15, 10},
};

static const char *const GM_NAMES[128] = {
	"Grand Piano", "Bright Piano", "Electric Grand", "Honky-tonk", "Rhodes", "Chorus Piano", "Harpsichord", "Clavinet",
	"Celesta", "Glockenspiel", "Music Box", "Vibraphone", "Marimba", "Xylophone", "Tubular Bells", "Dulcimer",
	"Drawbar Organ", "Percussive Organ", "Rock Organ", "Church Organ", "Reed Organ", "Accordion", "Harmonica", "Tango Accordion",
	"Nylon Guitar", "Steel Guitar", "Jazz Guitar", "Clean Guitar", "Muted Guitar", "Overdrive Guitar", "Distortion Guitar", "Guitar Harmonics",
	"Acoustic Bass", "Finger Bass", "Pick Bass", "Fretless Bass", "Slap Bass 1", "Slap Bass 2", "Synth Bass 1", "Synth Bass 2",
	"Violin", "Viola", "Cello", "Contrabass", "Tremolo Strings", "Pizzicato Strings", "Harp", "Timpani",
	"String Ensemble 1", "String Ensemble 2", "Synth Strings 1", "Synth Strings 2", "Choir Aahs", "Voice Oohs", "Synth Voice", "Orchestra Hit",
	"Trumpet", "Trombone", "Tuba", "Muted Trumpet", "French Horn", "Brass Section", "Synth Brass 1", "Synth Brass 2",
	"Soprano Sax", "Alto Sax", "Tenor Sax", "Baritone Sax", "Oboe", "English Horn", "Bassoon", "Clarinet",
	"Piccolo", "Flute", "Recorder", "Pan Flute", "Blown Bottle", "Shakuhachi", "Whistle", "Ocarina",
	"Square Lead", "Saw Lead", "Calliope Lead", "Chiff Lead", "Charang Lead", "Voice Lead", "Fifths Lead", "Bass Lead",
	"New Age Pad", "Warm Pad", "Polysynth Pad", "Choir Pad", "Bowed Pad", "Metallic Pad", "Halo Pad", "Sweep Pad",
	"Rain FX", "Soundtrack FX", "Crystal FX", "Atmosphere FX", "Brightness FX", "Goblins FX", "Echoes FX", "Sci-fi FX",
	"Sitar", "Banjo", "Shamisen", "Koto", "Kalimba", "Bagpipe", "Fiddle", "Shanai",
	"Tinkle Bell", "Agogo", "Steel Drums", "Woodblock", "Taiko Drum", "Melodic Tom", "Synth Drum", "Reverse Cymbal",
	"Guitar Fret Noise", "Breath Noise", "Seashore", "Bird Tweet", "Telephone", "Helicopter", "Applause", "Gunshot",
};

static const char *const LFO_RATES[8] = {"3.98 Hz", "5.56 Hz", "6.02 Hz", "6.37 Hz", "6.88 Hz", "9.63 Hz", "48.1 Hz", "72.2 Hz"};

// FM channels only have these three positions; PSG ignores pan.
static const uint8_t PAN_CC[3] = {0, 64, 127};

static patch_param_info globals[PATCH_NUM_GLOBALS];
static patch_param_info channel_params[PATCH_NUM_CHANNEL_PARAMS];
static uint32_t panel_order[PATCH_NUM_IDS];

static void define(patch_param_info *info, const char *name, int max, int default_value, bool stepped)
{
	snprintf(info->name, sizeof(info->name), "%s", name);
	info->max = (int16_t)max;
	info->default_value = (int16_t)default_value;
	info->stepped = stepped;
}

static int preset_field(int param)
{
	if (param < PARAM_OP_BASE) {
		return param - PARAM_ALGORITHM;
	}
	int op = (param - PARAM_OP_BASE) / PATCH_OP_PARAMS, i = (param - PARAM_OP_BASE) % PATCH_OP_PARAMS;
	return 4 + op * PATCH_OP_PARAMS + OP_PARAMS[i].preset_field;
}

void patch_global_init(void)
{
	define(&globals[GLOBAL_RETIRED], "", 0, 0, true);
	define(&globals[GLOBAL_FILTER], "Filter", FILTER_MAX, FILTER_DEFAULT, false);
	define(&globals[GLOBAL_LFO_ON], "LFO On", 1, 1, true);
	define(&globals[GLOBAL_LFO_RATE], "LFO Rate", 7, 0, true);
	define(&globals[GLOBAL_CH3_MODE], "Ch3 Special Mode", 1, 0, true);
	define(&globals[GLOBAL_DAC], "DAC Enable", 1, 0, true);

	define(&channel_params[PARAM_PRESET], "Preset", PRESETS - 1, 0, true);
	define(&channel_params[PARAM_VOLUME], "Volume", 127, 127, true);
	define(&channel_params[PARAM_PAN], "Pan", 2, 1, true);
	define(&channel_params[PARAM_FINE_TUNE], "Fine Tune", 127, 64, true);
	define(&channel_params[PARAM_BEND_RANGE], "Bend Range", BEND_RANGE_MAX, 2, true);
	define(&channel_params[PARAM_PORTA_ON], "Porta On", 1, 0, true);
	define(&channel_params[PARAM_PORTA_TIME], "Porta Time", 127, 64, true);
	define(&channel_params[PARAM_ALGORITHM], "Algorithm", 7, PRESETS_GM[0][0], true);
	define(&channel_params[PARAM_FEEDBACK], "Feedback", 7, PRESETS_GM[0][1], true);
	define(&channel_params[PARAM_AMS], "AMS", 3, PRESETS_GM[0][2], true);
	define(&channel_params[PARAM_FMS], "FMS", 7, PRESETS_GM[0][3], true);
	for (int op = 0; op < PATCH_OPS; op++) {
		for (int i = 0; i < PATCH_OP_PARAMS; i++) {
			int param = PARAM_OP_BASE + op * PATCH_OP_PARAMS + i;
			char name[24];
			snprintf(name, sizeof(name), "Op%d %s", op + 1, OP_PARAMS[i].name);
			define(&channel_params[param], name, OP_PARAMS[i].max, PRESETS_GM[0][preset_field(param)], true);
		}
	}

	uint32_t position = 0;
	for (int g = GLOBAL_RETIRED + 1; g < PATCH_NUM_GLOBALS; g++) {
		panel_order[position++] = (uint32_t)g;
	}
	for (int channel = 0; channel < PATCH_NUM_CHANNELS; channel++) {
		for (int param = 0; param < PATCH_NUM_CHANNEL_PARAMS; param++) {
			if (patch_channel_has(channel, param)) {
				panel_order[position++] = PATCH_CHANNEL_ID(channel, param);
			}
		}
	}
}

bool patch_channel_has(int channel, int param)
{
	if (channel < 0 || channel >= PATCH_NUM_CHANNELS || param < 0 || param >= PATCH_NUM_CHANNEL_PARAMS) {
		return false;
	}
	return channel < PATCH_NUM_FM_CHANNELS || (param != PARAM_PAN && param < PARAM_ALGORITHM);
}

bool patch_id_split(uint32_t id, int *channel, int *param)
{
	if (id < PATCH_NUM_GLOBALS) {
		if (id == GLOBAL_RETIRED) {
			return false;
		}
		*channel = -1;
		*param = (int)id;
		return true;
	}
	if (id < 1100) {
		return false;
	}
	*channel = (int)(id - 1000) / 100 - 1;
	*param = (int)(id % 100);
	return patch_channel_has(*channel, *param);
}

const patch_param_info *patch_param(uint32_t id)
{
	int channel, param;
	if (!patch_id_split(id, &channel, &param)) {
		return NULL;
	}
	return channel < 0 ? &globals[param] : &channel_params[param];
}

void patch_param_name(uint32_t id, char *name, size_t size)
{
	int channel, param;
	if (!patch_id_split(id, &channel, &param)) {
		snprintf(name, size, "?");
	} else if (channel < 0) {
		snprintf(name, size, "%s", globals[param].name);
	} else {
		snprintf(name, size, "Ch%d %s", channel + 1, channel_params[param].name);
	}
}

uint32_t patch_param_id_at(uint32_t panel_index)
{
	return panel_index < PATCH_NUM_IDS ? panel_order[panel_index] : UINT32_MAX;
}

int patch_preset_value(const patch_bank *bank, int channel, int param, int preset)
{
	if (channel < PATCH_NUM_FM_CHANNELS && param >= PARAM_ALGORITHM) {
		return bank->places[preset & (PATCH_BANK_SIZE - 1)].voice[param - PARAM_ALGORITHM];
	}
	return param == PARAM_PRESET ? preset : channel_params[param].default_value;
}

const char *patch_gm_name(int place)
{
	return GM_NAMES[place & (PATCH_BANK_SIZE - 1)];
}

// What the ROM loads under a built-in program number.
static void load_rom_preset(int16_t *voice, int program)
{
	voice[PARAM_PRESET] = (int16_t)program;
	for (int param = PARAM_ALGORITHM; param < PATCH_NUM_CHANNEL_PARAMS; param++) {
		voice[param] = PRESETS_GM[program][preset_field(param)];
	}
}

static void gm_bank(patch_bank *bank)
{
	for (int place = 0; place < PATCH_BANK_SIZE; place++) {
		patch_instrument *instrument = &bank->places[place];
		snprintf(instrument->name, sizeof(instrument->name), "%s", GM_NAMES[place]);
		for (int param = PARAM_ALGORITHM; param < PATCH_NUM_CHANNEL_PARAMS; param++) {
			instrument->voice[param - PARAM_ALGORITHM] = PRESETS_GM[place][preset_field(param)];
		}
	}
}

static void put_voice(patch_values *values, int channel, const patch_instrument *instrument)
{
	memcpy(values->channels[channel] + PARAM_ALGORITHM, instrument->voice, sizeof(instrument->voice));
	memcpy(values->names[channel], instrument->name, PATCH_NAME);
	values->names[channel][PATCH_NAME - 1] = 0;
}

// A place of the bank becomes the channel's sound, tweaks gone.
static void load_place(const patch_bank *bank, patch_values *values, int channel, int place)
{
	values->channels[channel][PARAM_PRESET] = (int16_t)place;
	put_voice(values, channel, &bank->places[place]);
}

static void default_values(const patch_bank *bank, patch_values *values)
{
	for (int g = 0; g < PATCH_NUM_GLOBALS; g++) {
		values->globals[g] = globals[g].default_value;
	}
	for (int channel = 0; channel < PATCH_NUM_CHANNELS; channel++) {
		for (int param = 0; param < PATCH_NUM_CHANNEL_PARAMS; param++) {
			values->channels[channel][param] = channel_params[param].default_value;
		}
	}
	for (int channel = 0; channel < PATCH_NUM_FM_CHANNELS; channel++) {
		load_place(bank, values, channel, values->channels[channel][PARAM_PRESET]);
	}
}

void patch_init(mdmi_patch *patch)
{
	memset(patch, 0, sizeof(*patch));
	gm_bank(&patch->bank);
	patch->shown_bank = patch->bank;
	default_values(&patch->bank, &patch->model);
	// A booted ROM holds exactly these values, reg_test checks it.
	patch->rom.values = patch->model;
	for (int voice = 0; voice < PATCH_NUM_FM_CHANNELS; voice++) {
		patch->rom.voice_owner[voice] = (int8_t)voice;
		patch->rom.programs[voice] = patch->model.channels[voice][PARAM_PRESET];
		memcpy(patch->rom.voices[voice], patch->model.channels[voice], sizeof(patch->rom.voices[voice]));
		patch->voices[voice].channel = -1;
		patch->voices[voice].sound = (int8_t)voice;
		patch->playing[voice] = -1;
	}
	// Nothing is stored yet, so a service number still loads its built-in preset.
	for (int i = 0; i < PATCH_SERVICE_PROGRAMS; i++) {
		load_rom_preset(patch->rom.stored[i], FIRST_SERVICE_PROGRAM + i);
	}
	memset(patch->rpn_msb, RPN_NULL, sizeof(patch->rpn_msb));
	memset(patch->rpn_lsb, RPN_NULL, sizeof(patch->rpn_lsb));
	patch->snapshots[0] = patch->snapshots[1] = patch->snapshots[2] = patch->model;
	patch->snapshot_back = 0;
	patch->snapshot_shared = 1;
	patch->snapshot_front = 2;
}

void patch_publish(mdmi_patch *patch)
{
	if (!patch->dirty) {
		return;
	}
	patch->dirty = false;
	patch->snapshots[patch->snapshot_back] = patch->model;
	patch->snapshot_back = __atomic_exchange_n(&patch->snapshot_shared, patch->snapshot_back | SNAPSHOT_NEW, __ATOMIC_ACQ_REL) & SNAPSHOT_INDEX;
}

// What the main thread reads: a state waiting to be taken, or else the newest whole snapshot.
const patch_values *patch_view(mdmi_patch *patch)
{
	uint32_t incoming = __atomic_load_n(&patch->incoming_state, __ATOMIC_ACQUIRE);
	if ((incoming == INCOMING_READY || incoming == INCOMING_TAKING) && patch->incoming.parts & PATCH_INCOMING_VALUES) {
		return &patch->incoming.values;
	}
	if (__atomic_load_n(&patch->snapshot_shared, __ATOMIC_ACQUIRE) & SNAPSHOT_NEW) {
		patch->snapshot_front = __atomic_exchange_n(&patch->snapshot_shared, patch->snapshot_front, __ATOMIC_ACQ_REL) & SNAPSHOT_INDEX;
	}
	return &patch->snapshots[patch->snapshot_front];
}

static int16_t *value_of(patch_values *values, uint32_t id)
{
	int channel, param;
	if (!patch_id_split(id, &channel, &param)) {
		return NULL;
	}
	return channel < 0 ? &values->globals[param] : &values->channels[channel][param];
}

const patch_bank *patch_bank_view(const mdmi_patch *patch)
{
	return &patch->shown_bank;
}

int patch_get(mdmi_patch *patch, uint32_t id)
{
	const int16_t *value = value_of((patch_values *)patch_view(patch), id);
	return value ? *value : 0;
}

double patch_model_filter_hz(const mdmi_patch *patch)
{
	return patch_filter_hz(patch->model.globals[GLOBAL_FILTER]);
}

void patch_format_preset(const patch_bank *bank, int channel, int value, char *text, size_t size)
{
	if (channel < PATCH_NUM_FM_CHANNELS) {
		snprintf(text, size, "%d %s", value + 1, bank->places[value & (PATCH_BANK_SIZE - 1)].name);
	} else {
		// The ROM's only other envelope sits under the GM numbers of square waves.
		bool square = value == 80 || value == 89 || value == 99;
		snprintf(text, size, "%d %s", value + 1, square ? "Square Lead" : "Plain");
	}
}

double patch_filter_hz(int value)
{
	if (value >= FILTER_MAX) {
		return FILTER_OFF_HZ;
	}
	return FILTER_MIN_HZ * pow(FILTER_RANGE, value / (double)FILTER_MAX);
}

static void format_global(int global, int value, char *text, size_t size)
{
	switch (global) {
	case GLOBAL_FILTER:
		if (value >= FILTER_MAX) {
			snprintf(text, size, "Off");
		} else {
			snprintf(text, size, "%.0f Hz", patch_filter_hz(value));
		}
		break;
	case GLOBAL_LFO_RATE:
		snprintf(text, size, "%s", LFO_RATES[value & 7]);
		break;
	default:
		snprintf(text, size, "%s", value ? "On" : "Off");
		break;
	}
}

void patch_format(const patch_bank *bank, uint32_t id, int value, char *text, size_t size)
{
	int channel, param;
	if (!patch_id_split(id, &channel, &param)) {
		snprintf(text, size, "%d", value);
		return;
	}
	if (channel < 0) {
		format_global(param, value, text, size);
		return;
	}
	switch (param) {
	case PARAM_PORTA_ON:
		snprintf(text, size, "%s", value ? "On" : "Off");
		break;
	case PARAM_FINE_TUNE:
		snprintf(text, size, "%+d ct", value - 64);
		break;
	case PARAM_BEND_RANGE:
		snprintf(text, size, "%d st", value);
		break;
	case PARAM_PRESET:
		patch_format_preset(bank, channel, value, text, size);
		break;
	case PARAM_PAN:
		snprintf(text, size, "%s", value == 0 ? "Left" : value == 1 ? "Center" : "Right");
		break;
	default:
		if (param >= PARAM_OP_BASE && (param - PARAM_OP_BASE) % PATCH_OP_PARAMS == OP_SSG_FIELD) {
			if (value < SSG_FIRST_ENVELOPE) {
				snprintf(text, size, "Off");
			} else {
				snprintf(text, size, "Env %d", value - SSG_FIRST_ENVELOPE + 1);
			}
		} else {
			snprintf(text, size, "%d", value);
		}
		break;
	}
}

static int clamp(const patch_param_info *info, int value)
{
	return value < 0 ? 0 : value > info->max ? info->max : value;
}

// A lane repeating its value must not wipe tweaks, so only a new preset loads. True if it did.
static bool set_preset(mdmi_patch *patch, int channel, int value)
{
	int16_t *values = patch->model.channels[channel];
	if (values[PARAM_PRESET] == value) {
		return false;
	}
	values[PARAM_PRESET] = (int16_t)value;
	if (channel < PATCH_NUM_FM_CHANNELS) {
		load_place(&patch->bank, &patch->model, channel, value);
		return true;
	}
	return false;
}

bool patch_set(mdmi_patch *patch, uint32_t id, int value)
{
	int channel, param;
	if (!patch_id_split(id, &channel, &param)) {
		return false;
	}
	value = clamp(patch_param(id), value);
	patch->dirty = true;
	if (channel < 0) {
		patch->model.globals[param] = (int16_t)value;
		return false;
	}
	int16_t *values = patch->model.channels[channel];
	if (param == PARAM_PRESET) {
		return set_preset(patch, channel, value);
	}
	values[param] = (int16_t)value;
	return false;
}

static void emit_cc(patch_emit_fn emit, void *context, int channel, int cc, int value)
{
	const uint8_t message[3] = {(uint8_t)(0xB0 | channel), (uint8_t)cc, (uint8_t)value};
	emit(context, message, sizeof(message));
}

// A reset of the chip the ROM doesn't know about: the voices lose their sound, not their numbers.
static void forget_voices(patch_receiver *receiver)
{
	for (int voice = 0; voice < PATCH_NUM_FM_CHANNELS; voice++) {
		for (int param = PARAM_ALGORITHM; param < PATCH_NUM_CHANNEL_PARAMS; param++) {
			receiver->voices[voice][param] = PATCH_UNKNOWN;
		}
	}
}

static void forget_everything(patch_receiver *receiver)
{
	memset(receiver, 0xFF, sizeof(*receiver));
	memset(receiver->alternate, 0, sizeof(receiver->alternate));
}

// What a program change to a number loads into a voice.
static void load_program(const patch_receiver *receiver, int program, int16_t *voice)
{
	voice[PARAM_PRESET] = (int16_t)program;
	if (program >= FIRST_SERVICE_PROGRAM) {
		memcpy(voice + PARAM_ALGORITHM, receiver->stored[program - FIRST_SERVICE_PROGRAM] + PARAM_ALGORITHM, (PATCH_NUM_CHANNEL_PARAMS - PARAM_ALGORITHM) * sizeof(*voice));
	} else {
		load_rom_preset(voice, program);
	}
}

// A program change reaches every voice of the channel, and the ROM loads it into each voice that
// has another number. One that has this number keeps what it holds.
static void select_program(patch_receiver *receiver, int channel, int program, patch_emit_fn emit, void *context)
{
	const uint8_t message[2] = {(uint8_t)(0xC0 | channel), (uint8_t)program};
	emit(context, message, sizeof(message));
	receiver->programs[channel] = (int16_t)program;
	for (int voice = 0; voice < PATCH_NUM_FM_CHANNELS; voice++) {
		if (receiver->voice_owner[voice] == channel && receiver->voices[voice][PARAM_PRESET] != program) {
			load_program(receiver, program, receiver->voices[voice]);
		}
	}
}

// A whole voice as the ROM's stored program, selected under one of the channel's two service
// numbers: the ROM skips a program it thinks is loaded, and a stored one shadows the built-in.
static void send_whole_voice(const int16_t *model, patch_receiver *receiver, int channel, patch_emit_fn emit, void *context)
{
	int program = FIRST_SERVICE_PROGRAM + channel * 2 + receiver->alternate[channel];
	receiver->alternate[channel] = !receiver->alternate[channel];
	uint8_t sysex[VOICE_SYSEX_HEAD + VOICE_BYTES + 1] = {0xF0, 0x00, 0x22, 0x77, 0x0A, 0x00, (uint8_t)program};
	uint8_t *voice = sysex + VOICE_SYSEX_HEAD;
	int16_t *stored = receiver->stored[program - FIRST_SERVICE_PROGRAM];
	for (int param = PARAM_ALGORITHM; param < PATCH_NUM_CHANNEL_PARAMS; param++) {
		voice[preset_field(param)] = (uint8_t)model[param];
		stored[param] = model[param];
	}
	sysex[sizeof(sysex) - 1] = 0xF7;
	emit(context, sysex, sizeof(sysex));
	select_program(receiver, channel, program, emit, context);
	receiver->values.channels[channel][PARAM_PRESET] = model[PARAM_PRESET];
}

// The ROM's CC only reaches the first 16 sustain rates. Registers hold operators in the order 1, 3, 2, 4.
static void write_sustain_rate(int voice, int op, int value, patch_emit_fn emit, void *context)
{
	int slot = op == 1 ? 2 : op == 2 ? 1 : op;
	int reg = YM_REG_SUSTAIN_RATE + voice % 3 + slot * 4;
	const uint8_t sysex[10] = {0xF0, 0x00, 0x22, 0x77, (uint8_t)(voice < 3 ? 0x08 : 0x09), (uint8_t)(reg >> 4), (uint8_t)(reg & 15), (uint8_t)(value >> 4), (uint8_t)(value & 15), 0xF7};
	emit(context, sysex, sizeof(sysex));
}

// A controller reaches every voice of its channel, so it goes once if any voice it is meant for
// differs. A sustain rate above 15 is written into each of them.
static void sync_voice_param(const int16_t *model, patch_receiver *receiver, unsigned meant, int param, int cc, int cc_range, int channel, patch_emit_fn emit, void *context)
{
	int value = model[param];
	bool differs = false;
	for (int voice = 0; voice < PATCH_NUM_FM_CHANNELS; voice++) {
		differs = differs || (meant >> voice & 1 && receiver->voices[voice][param] != value);
	}
	if (!differs) {
		return;
	}
	bool by_cc = value < cc_range;
	if (by_cc) {
		emit_cc(emit, context, channel, cc, value * (128 / cc_range));
	}
	int op = (param - PARAM_OP_BASE) / PATCH_OP_PARAMS;
	for (int voice = 0; voice < PATCH_NUM_FM_CHANNELS; voice++) {
		if (receiver->voice_owner[voice] == channel && (by_cc || meant >> voice & 1)) {
			if (!by_cc) {
				write_sustain_rate(voice, op, value, emit, context);
			}
			receiver->voices[voice][param] = (int16_t)value;
		}
	}
}

// How many values of the voices still differ from the model once a program change to the program
// reached them: a voice with that number keeps what it holds, and one we know nothing about stays
// unknown, because the ROM may think it has the number already.
static int differences_after(const patch_receiver *receiver, const int16_t *model, unsigned meant, int program)
{
	bool differs[PATCH_NUM_CHANNEL_PARAMS] = {false};
	for (int voice = 0; voice < PATCH_NUM_FM_CHANNELS; voice++) {
		if (!(meant >> voice & 1)) {
			continue;
		}
		int16_t after[PATCH_NUM_CHANNEL_PARAMS];
		memcpy(after, receiver->voices[voice], sizeof(after));
		if (program == PATCH_UNKNOWN || after[PARAM_PRESET] == PATCH_UNKNOWN) {
			return PATCH_VOICE_PARAMS;
		}
		if (after[PARAM_PRESET] != program) {
			load_program(receiver, program, after);
		}
		for (int param = PARAM_ALGORITHM; param < PATCH_NUM_CHANNEL_PARAMS; param++) {
			differs[param] = differs[param] || after[param] != model[param];
		}
	}
	int count = 0;
	for (int param = PARAM_ALGORITHM; param < PATCH_NUM_CHANNEL_PARAMS; param++) {
		count += differs[param];
	}
	return count;
}

// Brings the voices of the channel to the model: a voice that came from another channel gets the
// channel's program number, then whatever still differs goes as controllers (D20). A free voice
// that went home with another channel's sound waits for its own channel's next note, so its tail
// plays on as it was.
static void sync_voice(mdmi_patch *patch, const int16_t *model, patch_receiver *receiver, int channel, patch_emit_fn emit, void *context)
{
	int16_t *rom = receiver->values.channels[channel];
	unsigned meant = 0;
	bool other_program = false;
	for (int voice = 0; voice < PATCH_NUM_FM_CHANNELS; voice++) {
		const patch_voice *dealt = &patch->voices[voice];
		if (receiver->voice_owner[voice] == channel && (dealt->channel == channel || dealt->sound == channel)) {
			meant |= 1u << voice;
			other_program = other_program || receiver->voices[voice][PARAM_PRESET] != receiver->programs[channel];
		}
	}
	bool new_preset = model[PARAM_PRESET] != rom[PARAM_PRESET];
	int program = new_preset ? model[PARAM_PRESET] : receiver->programs[channel];
	if ((new_preset && program >= FIRST_SERVICE_PROGRAM) || differences_after(receiver, model, meant, program) > WHOLE_VOICE_DIFFERENCES) {
		send_whole_voice(model, receiver, channel, emit, context);
	} else if (new_preset) {
		select_program(receiver, channel, model[PARAM_PRESET], emit, context);
		rom[PARAM_PRESET] = model[PARAM_PRESET];
	} else if (other_program) {
		select_program(receiver, channel, receiver->programs[channel], emit, context);
	}
	sync_voice_param(model, receiver, meant, PARAM_ALGORITHM, CC_ALGORITHM, 8, channel, emit, context);
	sync_voice_param(model, receiver, meant, PARAM_FEEDBACK, CC_FEEDBACK, 8, channel, emit, context);
	sync_voice_param(model, receiver, meant, PARAM_AMS, CC_AMS, 4, channel, emit, context);
	sync_voice_param(model, receiver, meant, PARAM_FMS, CC_FMS, 8, channel, emit, context);
	for (int op = 0; op < PATCH_OPS; op++) {
		for (int i = 0; i < PATCH_OP_PARAMS; i++) {
			sync_voice_param(model, receiver, meant, PARAM_OP_BASE + op * PATCH_OP_PARAMS + i, OP_PARAMS[i].cc + op, OP_PARAMS[i].cc_range, channel, emit, context);
		}
	}
	for (int voice = 0; voice < PATCH_NUM_FM_CHANNELS; voice++) {
		if (meant >> voice & 1) {
			patch->voices[voice].sound = (int8_t)channel;
		}
	}
}

static void sync_channel_params(mdmi_patch *patch, const int16_t *model, int16_t *rom, int channel, patch_emit_fn emit, void *context)
{
	if (model[PARAM_VOLUME] != rom[PARAM_VOLUME]) {
		emit_cc(emit, context, channel, CC_VOLUME, model[PARAM_VOLUME]);
		rom[PARAM_VOLUME] = model[PARAM_VOLUME];
	}
	if (channel < PATCH_NUM_FM_CHANNELS && model[PARAM_PAN] != rom[PARAM_PAN]) {
		emit_cc(emit, context, channel, CC_PAN, PAN_CC[model[PARAM_PAN]]);
		rom[PARAM_PAN] = model[PARAM_PAN];
	}
	if (model[PARAM_FINE_TUNE] != rom[PARAM_FINE_TUNE]) {
		emit_cc(emit, context, channel, CC_FINE_TUNE, model[PARAM_FINE_TUNE]);
		rom[PARAM_FINE_TUNE] = model[PARAM_FINE_TUNE];
	}
	if (model[PARAM_PORTA_TIME] != rom[PARAM_PORTA_TIME]) {
		emit_cc(emit, context, channel, CC_PORTA_TIME, model[PARAM_PORTA_TIME]);
		rom[PARAM_PORTA_TIME] = model[PARAM_PORTA_TIME];
	}
	if (model[PARAM_PORTA_ON] != rom[PARAM_PORTA_ON]) {
		emit_cc(emit, context, channel, CC_PORTA_ON, model[PARAM_PORTA_ON] ? CC_ON : 0);
		rom[PARAM_PORTA_ON] = model[PARAM_PORTA_ON];
	}
	if (model[PARAM_BEND_RANGE] != rom[PARAM_BEND_RANGE]) {
		// RPN 0,0, then back to the null RPN so stray data entry messages change nothing.
		emit_cc(emit, context, channel, CC_RPN_MSB, 0);
		emit_cc(emit, context, channel, CC_RPN_LSB, 0);
		emit_cc(emit, context, channel, CC_DATA_ENTRY_MSB, model[PARAM_BEND_RANGE]);
		emit_cc(emit, context, channel, CC_DATA_ENTRY_LSB, 0);
		emit_cc(emit, context, channel, CC_RPN_MSB, RPN_NULL);
		emit_cc(emit, context, channel, CC_RPN_LSB, RPN_NULL);
		patch->rpn_msb[channel] = patch->rpn_lsb[channel] = RPN_NULL;
		rom[PARAM_BEND_RANGE] = model[PARAM_BEND_RANGE];
	}
}

// The ROM's static mapping plays a channel's notes only on voices given to it. After a reset or
// SysEx we can't read, every voice goes back to its own channel: a GM reset would otherwise leave
// the ROM taking any free voice for any channel, past the model.
static void sync_voice_owners(patch_receiver *receiver, patch_emit_fn emit, void *context)
{
	bool unknown = false;
	for (int voice = 0; voice < PATCH_NUM_FM_CHANNELS; voice++) {
		unknown = unknown || receiver->voice_owner[voice] == PATCH_UNKNOWN;
	}
	if (unknown) {
		const uint8_t sysex[7] = {0xF0, 0x00, 0x22, 0x77, SYSEX_DYNAMIC, 0x00, 0xF7};
		emit(context, sysex, sizeof(sysex));
		for (int voice = 0; voice < PATCH_NUM_FM_CHANNELS; voice++) {
			receiver->voice_owner[voice] = (int8_t)voice;
		}
	}
}

// The voice dealer (D20, D21). Every FM channel plays on its own voice; a note it plays while that
// voice is busy borrows the free voice of another channel that costs least to turn into its sound.
// A channel takes its own voice back for its next note and cuts the chord's note on it.

// Channels that play only on their own voice, which is never lent: the ROM glides on one voice,
// Ch3 Special gives voice 3 a pitch per operator, and the DAC takes voice 6.
static bool plays_alone(const mdmi_patch *patch, int channel)
{
	return patch->model.channels[channel][PARAM_PORTA_ON]
		|| (channel == 2 && patch->model.globals[GLOBAL_CH3_MODE])
		|| (channel == PATCH_NUM_FM_CHANNELS - 1 && patch->model.globals[GLOBAL_DAC]);
}

static void emit_note(patch_emit_fn emit, void *context, int status, int channel, int key, int velocity)
{
	const uint8_t message[3] = {(uint8_t)(status | channel), (uint8_t)key, (uint8_t)velocity};
	emit(context, message, sizeof(message));
}

static bool is_muted(const mdmi_patch *patch, int channel, int key)
{
	return patch->muted[channel][key >> 5] >> (key & 31) & 1;
}

static void set_muted(mdmi_patch *patch, int channel, int key, bool muted)
{
	if (is_muted(patch, channel, key) == muted) {
		return;
	}
	patch->muted[channel][key >> 5] ^= 1u << (key & 31);
	__atomic_store_n(&patch->muted_count[channel], (uint8_t)(patch->muted_count[channel] + (muted ? 1 : -1)), __ATOMIC_RELAXED);
}

static void mute(mdmi_patch *patch, int channel, int key, int velocity)
{
	set_muted(patch, channel, key, true);
	patch->muted_velocity[channel][key] = (uint8_t)velocity;
}

int patch_muted_notes(const mdmi_patch *patch, int channel)
{
	if (channel < 0 || channel >= PATCH_NUM_FM_CHANNELS) {
		return 0;
	}
	return __atomic_load_n(&patch->muted_count[channel], __ATOMIC_RELAXED);
}

int patch_voice_player(const mdmi_patch *patch, int voice)
{
	if (voice < 0 || voice >= PATCH_NUM_FM_CHANNELS) {
		return -1;
	}
	return __atomic_load_n(&patch->playing[voice], __ATOMIC_RELAXED);
}

int patch_voice_key(const mdmi_patch *patch, int voice)
{
	if (voice < 0 || voice >= PATCH_NUM_FM_CHANNELS) {
		return 0;
	}
	return __atomic_load_n(&patch->playing_key[voice], __ATOMIC_RELAXED);
}

// Shows the window who plays where, once the notes are out.
static void publish_voices(mdmi_patch *patch)
{
	for (int voice = 0; voice < PATCH_NUM_FM_CHANNELS; voice++) {
		const patch_voice *v = &patch->voices[voice];
		__atomic_store_n(&patch->playing_key[voice], v->key, __ATOMIC_RELAXED);
		__atomic_store_n(&patch->playing[voice], (int8_t)(v->pending ? -1 : v->channel), __ATOMIC_RELAXED);
	}
}

static void remap(patch_receiver *receiver, int channel, int voice, patch_emit_fn emit, void *context)
{
	if (receiver->voice_owner[voice] == channel) {
		return;
	}
	const uint8_t sysex[8] = {0xF0, 0x00, 0x22, 0x77, SYSEX_REMAP, (uint8_t)channel, (uint8_t)voice, 0xF7};
	emit(context, sysex, sizeof(sysex));
	receiver->voice_owner[voice] = (int8_t)channel;
}

// A voice whose note ended goes home at once with nothing but its mapping: the tail plays on in the
// sound it has, and its own channel's next note brings that channel's sound back.
static void free_voice(mdmi_patch *patch, int voice, patch_emit_fn emit, void *context)
{
	patch->voices[voice].channel = -1;
	patch->voices[voice].pending = false;
	remap(&patch->rom, voice, voice, emit, context);
}

// Takes a voice from the note on it, which stays held but silent.
static void cut_voice(mdmi_patch *patch, int voice, patch_emit_fn emit, void *context)
{
	const patch_voice *taken = &patch->voices[voice];
	if (taken->channel < 0) {
		return;
	}
	mute(patch, taken->channel, taken->key, taken->velocity);
	if (!taken->pending) {
		emit_note(emit, context, 0x80, taken->channel, taken->key, 0);
	}
	free_voice(patch, voice, emit, context);
}

// About how long the ROM takes to give a voice a channel's sound: a program it loads costs as
// much as a few controllers (D21).
#define PROGRAM_COST 4
static int lending_cost(const mdmi_patch *patch, int voice, int channel)
{
	int16_t after[PATCH_NUM_CHANNEL_PARAMS];
	memcpy(after, patch->rom.voices[voice], sizeof(after));
	int cost = 0;
	if (after[PARAM_PRESET] != patch->rom.programs[channel]) {
		if (patch->rom.programs[channel] == PATCH_UNKNOWN) {
			return PROGRAM_COST + PATCH_NUM_CHANNEL_PARAMS;
		}
		load_program(&patch->rom, patch->rom.programs[channel], after);
		cost = PROGRAM_COST;
	}
	for (int param = PARAM_ALGORITHM; param < PATCH_NUM_CHANNEL_PARAMS; param++) {
		cost += after[param] != patch->model.channels[channel][param];
	}
	return cost;
}

// The free voice that costs least to turn into a channel's sound, or -1 if none is free.
static int cheapest_voice(const mdmi_patch *patch, int channel)
{
	int best = -1, best_cost = 0;
	for (int voice = 0; voice < PATCH_NUM_FM_CHANNELS; voice++) {
		if (patch->voices[voice].channel >= 0 || plays_alone(patch, voice)) {
			continue;
		}
		int cost = lending_cost(patch, voice, channel);
		if (best < 0 || cost < best_cost) {
			best = voice;
			best_cost = cost;
		}
	}
	return best;
}

static void take_voice(mdmi_patch *patch, int voice, const patch_note *note, bool pending, patch_emit_fn emit, void *context)
{
	remap(&patch->rom, note->channel, voice, emit, context);
	patch->voices[voice] = (patch_voice){(int8_t)note->channel, note->key, pending, patch->voices[voice].sound, note->velocity};
}

// A note waits for the end of its moment, when the voices are dealt.
static void note_on(mdmi_patch *patch, int channel, int key, int velocity, patch_emit_fn emit, void *context)
{
	if (patch->pending_count == PATCH_PENDING_NOTES) {
		patch_flush(patch, emit, context);
	}
	for (uint32_t i = 0; i < patch->pending_count; i++) {
		if (patch->pending[i].channel == channel && patch->pending[i].key == key) {
			return;
		}
	}
	set_muted(patch, channel, key, false);
	if (!plays_alone(patch, channel)) {
		// The same key again: its note ends first.
		for (int voice = 0; voice < PATCH_NUM_FM_CHANNELS; voice++) {
			if (patch->voices[voice].channel == channel && patch->voices[voice].key == key) {
				emit_note(emit, context, 0x80, channel, key, 0);
				free_voice(patch, voice, emit, context);
			}
		}
	}
	patch->pending[patch->pending_count++] = (patch_note){(uint8_t)channel, (uint8_t)key, (uint8_t)velocity};
}

// A voice came free: held notes that found none start again on it, from their attack.
static void revive(mdmi_patch *patch, patch_emit_fn emit, void *context)
{
	int free = 0;
	for (int voice = 0; voice < PATCH_NUM_FM_CHANNELS; voice++) {
		free += patch->voices[voice].channel < 0 && !plays_alone(patch, voice);
	}
	for (int channel = 0; channel < PATCH_NUM_FM_CHANNELS && free > 0; channel++) {
		for (int key = 0; key < 128 && free > 0 && !plays_alone(patch, channel); key++) {
			if (is_muted(patch, channel, key) && patch->pending_count < PATCH_PENDING_NOTES) {
				set_muted(patch, channel, key, false);
				patch->pending[patch->pending_count++] = (patch_note){(uint8_t)channel, (uint8_t)key, patch->muted_velocity[channel][key]};
				free--;
			}
		}
	}
	patch_flush(patch, emit, context);
}

static void note_off(mdmi_patch *patch, int channel, int key, patch_emit_fn emit, void *context)
{
	if (is_muted(patch, channel, key)) {
		set_muted(patch, channel, key, false);
		return;
	}
	emit_note(emit, context, 0x80, channel, key, 0);
	// A note dealt its own voice, also when the channel came to play alone since. The channel's
	// own voice, while it plays alone, is free only once none of its keys is held.
	for (int voice = 0; voice < PATCH_NUM_FM_CHANNELS; voice++) {
		if (patch->voices[voice].channel == channel && patch->voices[voice].key == key && !(voice == channel && patch->held[channel])) {
			free_voice(patch, voice, emit, context);
			revive(patch, emit, context);
			publish_voices(patch);
			return;
		}
	}
	if (patch->held[channel] && --patch->held[channel] == 0 && patch->voices[channel].channel == channel) {
		patch->voices[channel].channel = -1;
		revive(patch, emit, context);
	}
	publish_voices(patch);
}

// After the ROM stopped a channel's notes.
static void release_channel(mdmi_patch *patch, int channel, patch_emit_fn emit, void *context)
{
	for (int voice = 0; voice < PATCH_NUM_FM_CHANNELS; voice++) {
		if (patch->voices[voice].channel == channel) {
			free_voice(patch, voice, emit, context);
		}
	}
	patch->held[channel] = 0;
	memset(patch->muted[channel], 0, sizeof(patch->muted[channel]));
	__atomic_store_n(&patch->muted_count[channel], 0, __ATOMIC_RELAXED);
	publish_voices(patch);
}

static void stop_channel(mdmi_patch *patch, int channel, patch_emit_fn emit, void *context)
{
	emit_cc(emit, context, channel, CC_ALL_NOTES_OFF, 0);
	release_channel(patch, channel, emit, context);
}

// Stops the FM channels that play, before something we can't follow reaches the ROM.
static void stop_playing(mdmi_patch *patch, patch_emit_fn emit, void *context)
{
	patch->pending_count = 0;
	for (int channel = 0; channel < PATCH_NUM_FM_CHANNELS; channel++) {
		bool playing = patch->held[channel] > 0;
		for (int voice = 0; voice < PATCH_NUM_FM_CHANNELS; voice++) {
			playing = playing || patch->voices[voice].channel == channel;
		}
		if (playing) {
			stop_channel(patch, channel, emit, context);
		}
	}
}

void patch_all_notes_off(mdmi_patch *patch, patch_emit_fn emit, void *context)
{
	patch->pending_count = 0;
	for (int channel = 0; channel < 16; channel++) {
		if (channel < PATCH_NUM_FM_CHANNELS) {
			stop_channel(patch, channel, emit, context);
		} else {
			emit_cc(emit, context, channel, CC_ALL_NOTES_OFF, 0);
		}
	}
}

// The moment is complete. First every note takes its channel's own voice if that voice is free or
// plays another channel's chord; the chord's note there stops and moves to a free voice, where it
// starts again (D20, D22). Then the moved notes and the other notes borrow free voices, in that
// order, and what finds none is cut. The borrowed voices get their sound in one go, then the
// notes start (D21).
void patch_flush(mdmi_patch *patch, patch_emit_fn emit, void *context)
{
	if (patch->pending_count == 0) {
		return;
	}
	enum { WAITS, MOVED, OWN, ALONE, MUTED };
	uint8_t dealt[PATCH_PENDING_NOTES];
	memset(dealt, WAITS, sizeof(dealt));
	for (uint32_t i = 0; i < patch->pending_count; i++) {
		const patch_note *note = &patch->pending[i];
		if (dealt[i] == MOVED) {
			continue;
		}
		if (plays_alone(patch, note->channel)) {
			if (patch->voices[note->channel].channel != note->channel) {
				cut_voice(patch, note->channel, emit, context);
			}
			take_voice(patch, note->channel, note, false, emit, context);
			if (patch->held[note->channel] < 255) {
				patch->held[note->channel]++;
			}
			dealt[i] = ALONE;
		} else if (patch->voices[note->channel].channel != note->channel) {
			const patch_voice *chord = &patch->voices[note->channel];
			if (chord->channel >= 0) {
				const patch_note moved = {(uint8_t)chord->channel, chord->key, chord->velocity};
				emit_note(emit, context, 0x80, moved.channel, moved.key, 0);
				free_voice(patch, note->channel, emit, context);
				if (patch->pending_count < PATCH_PENDING_NOTES) {
					dealt[patch->pending_count] = MOVED;
					patch->pending[patch->pending_count++] = moved;
				} else {
					mute(patch, moved.channel, moved.key, moved.velocity);
				}
			}
			take_voice(patch, note->channel, note, true, emit, context);
			dealt[i] = OWN;
		}
	}
	for (int moved = 1; moved >= 0; moved--) {
		for (uint32_t i = 0; i < patch->pending_count; i++) {
			const patch_note *note = &patch->pending[i];
			if (dealt[i] != (moved ? MOVED : WAITS)) {
				continue;
			}
			int voice = cheapest_voice(patch, note->channel);
			if (voice < 0) {
				mute(patch, note->channel, note->key, note->velocity);
				dealt[i] = MUTED;
			} else {
				take_voice(patch, voice, note, true, emit, context);
			}
		}
	}
	patch_sync(patch, &patch->rom, emit, context);
	for (uint32_t i = 0; i < patch->pending_count; i++) {
		const patch_note *note = &patch->pending[i];
		if (dealt[i] == MUTED) {
			continue;
		}
		if (dealt[i] != ALONE) {
			// The ROM gives a note the free voice of its channel with the lowest number.
			int waiting = -1, first = -1;
			for (int voice = PATCH_NUM_FM_CHANNELS - 1; voice >= 0; voice--) {
				const patch_voice *v = &patch->voices[voice];
				if (v->channel == note->channel && v->pending) {
					first = voice;
					waiting = v->key == note->key ? voice : waiting;
				}
			}
			if (waiting < 0) {
				// No voice waits for it.
				continue;
			}
			patch->voices[waiting].key = patch->voices[first].key;
			patch->voices[first].key = note->key;
			patch->voices[first].pending = false;
		}
		emit_note(emit, context, 0x90, note->channel, note->key, note->velocity);
	}
	patch->pending_count = 0;
	publish_voices(patch);
}

void patch_sync(mdmi_patch *patch, patch_receiver *receiver, patch_emit_fn emit, void *context)
{
	const int16_t *model_globals = patch->model.globals;
	int16_t *rom_globals = receiver->values.globals;
	if (model_globals[GLOBAL_DAC] != rom_globals[GLOBAL_DAC]) {
		// The ROM swaps the Z80 driver for this, and the Z80's reset line also clears the YM2612.
		// The ROM doesn't know, so FM stays silent until its registers are written again.
		emit_cc(emit, context, GLOBAL_CC_CHANNEL, CC_DAC, model_globals[GLOBAL_DAC] ? CC_ON : 0);
		rom_globals[GLOBAL_DAC] = model_globals[GLOBAL_DAC];
		rom_globals[GLOBAL_LFO_ON] = rom_globals[GLOBAL_LFO_RATE] = rom_globals[GLOBAL_CH3_MODE] = PATCH_UNKNOWN;
		forget_voices(receiver);
	}
	sync_voice_owners(receiver, emit, context);
	// A voice lent before its channel came to play alone comes back.
	for (int channel = 0; channel < PATCH_NUM_FM_CHANNELS; channel++) {
		if (plays_alone(patch, channel) && patch->voices[channel].channel >= 0 && patch->voices[channel].channel != channel) {
			cut_voice(patch, channel, emit, context);
		}
	}
	if (model_globals[GLOBAL_LFO_ON] != rom_globals[GLOBAL_LFO_ON]) {
		emit_cc(emit, context, GLOBAL_CC_CHANNEL, CC_LFO_ON, model_globals[GLOBAL_LFO_ON] ? CC_ON : 0);
		rom_globals[GLOBAL_LFO_ON] = model_globals[GLOBAL_LFO_ON];
	}
	if (model_globals[GLOBAL_LFO_RATE] != rom_globals[GLOBAL_LFO_RATE]) {
		emit_cc(emit, context, GLOBAL_CC_CHANNEL, CC_LFO_RATE, model_globals[GLOBAL_LFO_RATE] * (128 / 8));
		rom_globals[GLOBAL_LFO_RATE] = model_globals[GLOBAL_LFO_RATE];
	}
	if (model_globals[GLOBAL_CH3_MODE] != rom_globals[GLOBAL_CH3_MODE]) {
		emit_cc(emit, context, GLOBAL_CC_CHANNEL, CC_CH3_MODE, model_globals[GLOBAL_CH3_MODE] ? CC_ON : 0);
		rom_globals[GLOBAL_CH3_MODE] = model_globals[GLOBAL_CH3_MODE];
	}
	for (int channel = 0; channel < PATCH_NUM_CHANNELS; channel++) {
		const int16_t *model = patch->model.channels[channel];
		int16_t *rom = receiver->values.channels[channel];
		if (channel >= PATCH_NUM_FM_CHANNELS) {
			if (model[PARAM_PRESET] != rom[PARAM_PRESET]) {
				const uint8_t message[2] = {(uint8_t)(0xC0 | channel), (uint8_t)model[PARAM_PRESET]};
				emit(context, message, sizeof(message));
				rom[PARAM_PRESET] = model[PARAM_PRESET];
			}
		} else {
			sync_voice(patch, model, receiver, channel, emit, context);
		}
		sync_channel_params(patch, model, rom, channel, emit, context);
	}
}

// Controllers that are the model's values. Operator controllers follow OP_PARAMS.
typedef struct {
	uint8_t cc;
	int8_t global;
	uint8_t param;
	uint8_t cc_range;
} cc_param;

static const cc_param CC_PARAMS[] = {
	{CC_ALGORITHM, -1, PARAM_ALGORITHM, 8},
	{CC_FEEDBACK, -1, PARAM_FEEDBACK, 8},
	{CC_AMS, -1, PARAM_AMS, 4},
	{CC_FMS, -1, PARAM_FMS, 8},
	{CC_VOLUME, -1, PARAM_VOLUME, 128},
	{CC_FINE_TUNE, -1, PARAM_FINE_TUNE, 128},
	{CC_PORTA_TIME, -1, PARAM_PORTA_TIME, 128},
	{CC_PORTA_ON, -1, PARAM_PORTA_ON, 2},
	{CC_LFO_ON, GLOBAL_LFO_ON, 0, 2},
	{CC_LFO_RATE, GLOBAL_LFO_RATE, 0, 8},
	{CC_CH3_MODE, GLOBAL_CH3_MODE, 0, 2},
	{CC_DAC, GLOBAL_DAC, 0, 2},
};

// Finds the model's value for a controller: a global, or a param of the channel it came on.
// False means it isn't one of ours.
static bool cc_to_model(int cc, int value, int *global, int *param, int *result)
{
	for (int op = 0; op < PATCH_OPS; op++) {
		for (int i = 0; i < PATCH_OP_PARAMS; i++) {
			if (OP_PARAMS[i].cc + op == cc) {
				*global = -1;
				*param = PARAM_OP_BASE + op * PATCH_OP_PARAMS + i;
				*result = value / (128 / OP_PARAMS[i].cc_range);
				return true;
			}
		}
	}
	for (size_t i = 0; i < sizeof(CC_PARAMS) / sizeof(*CC_PARAMS); i++) {
		if (CC_PARAMS[i].cc == cc) {
			*global = CC_PARAMS[i].global;
			*param = CC_PARAMS[i].param;
			*result = value / (128 / CC_PARAMS[i].cc_range);
			return true;
		}
	}
	if (cc == CC_PAN) {
		*global = -1;
		*param = PARAM_PAN;
		*result = value > 95 ? 2 : value > 31 ? 1 : 0;
		return true;
	}
	return false;
}

// One message from the host, by what it means to the model:
//   a value of the model: goes into the model only, and sync sends the difference;
//   a reset, or SysEx we can't read: goes to the ROM, which we then know nothing about;
//   anything else plays: goes straight to the ROM.
bool patch_midi(mdmi_patch *patch, const uint8_t *bytes, size_t length, patch_emit_fn emit, void *context)
{
	int status = bytes[0] & 0xF0;
	int channel = bytes[0] & 0x0F;
	bool is_channel = channel < PATCH_NUM_CHANNELS;
	bool fm = channel < PATCH_NUM_FM_CHANNELS;
	bool changed = false, moved = false;
	bool note = fm && (status == 0x90 || status == 0x80) && length >= 3;
	if (note && status == 0x90 && bytes[2]) {
		note_on(patch, channel, bytes[1] & 0x7F, bytes[2], emit, context);
		return false;
	}
	// Whatever comes after the notes of a moment comes after them in the ROM too.
	patch_flush(patch, emit, context);
	if (note) {
		note_off(patch, channel, bytes[1] & 0x7F, emit, context);
		return false;
	}
	if (bytes[0] == 0xF0 || bytes[0] == 0xFF) {
		stop_playing(patch, emit, context);
		emit(context, bytes, length);
		forget_everything(&patch->rom);
		// Every voice gets its own channel's whole sound again.
		for (int voice = 0; voice < PATCH_NUM_FM_CHANNELS; voice++) {
			patch->voices[voice].sound = (int8_t)voice;
		}
		patch_sync(patch, &patch->rom, emit, context);
		return false;
	}
	if (status == 0xC0 && length >= 2 && is_channel) {
		// Unlike a lane or a CC repeating the preset, a program from a clip always loads, tweaks gone.
		int16_t *model = patch->model.channels[channel];
		model[PARAM_PRESET] = bytes[1];
		if (fm) {
			load_place(&patch->bank, &patch->model, channel, bytes[1]);
		}
		changed = moved = true;
	} else if (status == 0xB0 && length >= 3) {
		int cc = bytes[1], value = bytes[2];
		int global, param, result;
		if (cc == CC_RESET_ALL) {
			// The ROM takes this as a reset of the channel, its program and the voice mapping, so send
			// what it means by the standard.
			const uint8_t bend[3] = {(uint8_t)(0xE0 | channel), 0, 0x40};
			emit(context, bend, sizeof(bend));
			emit_cc(emit, context, channel, CC_SUSTAIN, 0);
			return false;
		}
		if (cc == CC_STORE_PROGRAM || cc == CC_POLYPHONY) {
			// The first would make a program number mean something other than its preset, for good.
			// The second puts the ROM in a mode where any note takes any voice, past the model.
			return false;
		}
		if (cc == CC_RPN_MSB || cc == CC_RPN_LSB) {
			(cc == CC_RPN_MSB ? patch->rpn_msb : patch->rpn_lsb)[channel] = (uint8_t)value;
			emit(context, bytes, length);
			return false;
		}
		if (cc == CC_DATA_ENTRY_MSB && is_channel && patch->rpn_msb[channel] == 0 && patch->rpn_lsb[channel] == 0) {
			patch->model.channels[channel][PARAM_BEND_RANGE] = (int16_t)(value > BEND_RANGE_MAX ? BEND_RANGE_MAX : value);
			changed = true;
		} else if (cc == CC_PRESET) {
			// Like the Preset lane, so a clip envelope repeating the preset keeps the tweaks (D17).
			if (is_channel) {
				moved = set_preset(patch, channel, value * PRESETS / 128);
				changed = true;
			}
		} else if (cc == CC_STEREO) {
			// The stereo bits of the chip: 1 right, 2 left, 3 both. Off has no place on the Pan knob.
			static const int8_t STEREO_TO_PAN[4] = {-1, 2, 0, 1};
			int pan = STEREO_TO_PAN[value / (128 / 4)];
			if (pan >= 0 && fm) {
				patch->model.channels[channel][PARAM_PAN] = (int16_t)pan;
				changed = true;
			}
		} else if (cc_to_model(cc, value, &global, &param, &result)) {
			if (global >= 0) {
				patch->model.globals[global] = (int16_t)result;
				changed = true;
			} else if (patch_channel_has(channel, param)) {
				patch->model.channels[channel][param] = (int16_t)result;
				changed = true;
			}
			// Dropped otherwise: on channels without these values, the ROM would apply them to an FM voice.
		} else {
			emit(context, bytes, length);
			if ((cc == CC_ALL_NOTES_OFF || cc == CC_ALL_SOUND_OFF) && fm) {
				// The ROM stops the notes of every voice the channel has, so the lent ones go home.
				release_channel(patch, channel, emit, context);
				revive(patch, emit, context);
			}
		}
	} else {
		emit(context, bytes, length);
	}
	if (changed) {
		patch->dirty = true;
		patch_sync(patch, &patch->rom, emit, context);
	}
	return changed || moved;
}

size_t patch_save_size(void)
{
	return PATCH_NUM_IDS * SAVED_PAIR_BYTES;
}

void patch_save(const patch_values *values, void *buffer)
{
	uint8_t *out = buffer;
	for (uint32_t i = 0; i < PATCH_NUM_IDS; i++) {
		uint32_t id = panel_order[i];
		int16_t value = *value_of((patch_values *)values, id);
		memcpy(out, &id, sizeof(id));
		memcpy(out + sizeof(id), &value, sizeof(value));
		out += SAVED_PAIR_BYTES;
	}
}

// The incoming slot is the main thread's while it writes. Parts handed over before and not taken
// yet stay, so a dropped file and a bank wait together.
static void begin_incoming(mdmi_patch *patch)
{
	uint32_t expected;
	for (;;) {
		expected = INCOMING_FREE;
		if (__atomic_compare_exchange_n(&patch->incoming_state, &expected, INCOMING_WRITING, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
			patch->incoming.parts = 0;
			return;
		}
		expected = INCOMING_READY;
		if (__atomic_compare_exchange_n(&patch->incoming_state, &expected, INCOMING_WRITING, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
			return;
		}
	}
}

static void end_incoming(mdmi_patch *patch)
{
	__atomic_store_n(&patch->incoming_state, INCOMING_READY, __ATOMIC_RELEASE);
}

bool patch_load(mdmi_patch *patch, const void *buffer, size_t size, const char (*names)[PATCH_NAME])
{
	if (size % SAVED_PAIR_BYTES) {
		return false;
	}
	patch_values incoming;
	default_values(&patch->shown_bank, &incoming);
	const uint8_t *in = buffer;
	for (size_t i = 0; i < size / SAVED_PAIR_BYTES; i++) {
		uint32_t id;
		int16_t value;
		memcpy(&id, in, sizeof(id));
		memcpy(&value, in + sizeof(id), sizeof(value));
		in += SAVED_PAIR_BYTES;
		// A knob this build doesn't have is skipped, and one the state doesn't have keeps its default.
		int16_t *slot = value_of(&incoming, id);
		if (slot) {
			*slot = (int16_t)clamp(patch_param(id), value);
		}
	}
	for (int channel = 0; channel < PATCH_NUM_FM_CHANNELS; channel++) {
		if (names) {
			memcpy(incoming.names[channel], names[channel], PATCH_NAME);
			incoming.names[channel][PATCH_NAME - 1] = 0;
		} else {
			memcpy(incoming.names[channel], patch->shown_bank.places[incoming.channels[channel][PARAM_PRESET]].name, PATCH_NAME);
		}
	}
	begin_incoming(patch);
	// A loaded state replaces voices dropped before it.
	patch->incoming.parts = (patch->incoming.parts & PATCH_INCOMING_BANK) | PATCH_INCOMING_VALUES;
	patch->incoming.values = incoming;
	end_incoming(patch);
	return true;
}

void patch_save_names(const patch_values *values, void *buffer)
{
	memcpy(buffer, values->names, sizeof(values->names));
}

// A bank place from a file holds only what the knobs can.
static void clean_place(patch_instrument *instrument)
{
	instrument->name[PATCH_NAME - 1] = 0;
	for (int i = 0; i < PATCH_VOICE_PARAMS; i++) {
		instrument->voice[i] = (int16_t)clamp(&channel_params[PARAM_ALGORITHM + i], instrument->voice[i]);
	}
}

static void send_bank(mdmi_patch *patch)
{
	begin_incoming(patch);
	patch->incoming.parts |= PATCH_INCOMING_BANK;
	patch->incoming.bank = patch->shown_bank;
	end_incoming(patch);
}

void patch_load_bank(mdmi_patch *patch, const patch_bank *bank)
{
	patch->shown_bank = *bank;
	for (int place = 0; place < PATCH_BANK_SIZE; place++) {
		clean_place(&patch->shown_bank.places[place]);
	}
	send_bank(patch);
}

void patch_load_place(mdmi_patch *patch, int place, const patch_instrument *instrument)
{
	patch_instrument *at = &patch->shown_bank.places[place & (PATCH_BANK_SIZE - 1)];
	*at = *instrument;
	clean_place(at);
	send_bank(patch);
}

void patch_load_voice(mdmi_patch *patch, int channel, const patch_instrument *instrument)
{
	if (channel < 0 || channel >= PATCH_NUM_FM_CHANNELS) {
		return;
	}
	begin_incoming(patch);
	patch->incoming.parts |= PATCH_INCOMING_VOICE << channel;
	patch->incoming.voices[channel] = *instrument;
	end_incoming(patch);
}

bool patch_take_incoming(mdmi_patch *patch)
{
	uint32_t expected = INCOMING_READY;
	if (!__atomic_compare_exchange_n(&patch->incoming_state, &expected, INCOMING_TAKING, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
		return false;
	}
	const patch_incoming *incoming = &patch->incoming;
	if (incoming->parts & PATCH_INCOMING_VALUES) {
		patch->model = incoming->values;
	}
	if (incoming->parts & PATCH_INCOMING_BANK) {
		// A channel that plays its place as it is follows the place; one with tweaks or a file of its
		// own keeps them. A whole state brings its channels with it.
		for (int channel = 0; channel < PATCH_NUM_FM_CHANNELS && !(incoming->parts & PATCH_INCOMING_VALUES); channel++) {
			const int place = patch->model.channels[channel][PARAM_PRESET];
			const patch_instrument *was = &patch->bank.places[place];
			if (!memcmp(patch->model.channels[channel] + PARAM_ALGORITHM, was->voice, sizeof(was->voice)) && !strcmp(patch->model.names[channel], was->name)) {
				put_voice(&patch->model, channel, &incoming->bank.places[place]);
			}
		}
		patch->bank = incoming->bank;
	}
	for (int channel = 0; channel < PATCH_NUM_FM_CHANNELS; channel++) {
		if (incoming->parts & PATCH_INCOMING_VOICE << channel) {
			put_voice(&patch->model, channel, &incoming->voices[channel]);
		}
	}
	patch->dirty = true;
	patch_publish(patch);
	__atomic_store_n(&patch->incoming_state, INCOMING_FREE, __ATOMIC_RELEASE);
	return true;
}
