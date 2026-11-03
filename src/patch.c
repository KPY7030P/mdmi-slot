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
#define CC_ALL_NOTES_OFF 123
#define CC_STORE_PROGRAM 87
#define YM_REG_SUSTAIN_RATE 0x70
#define SYSEX_REMAP 0x00
#define SYSEX_DYNAMIC 0x03
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
	define(&globals[GLOBAL_POLYPHONY], "Polyphony", PATCH_NUM_FM_CHANNELS, 0, true);
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
	for (int g = 0; g < PATCH_NUM_GLOBALS; g++) {
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

int patch_preset_value(int channel, int param, int preset)
{
	if (channel < PATCH_NUM_FM_CHANNELS && param >= PARAM_ALGORITHM) {
		return PRESETS_GM[preset & (PRESETS - 1)][preset_field(param)];
	}
	return param == PARAM_PRESET ? preset : channel_params[param].default_value;
}

int patch_voice_count(const int16_t *values, int channel)
{
	if (channel >= PATCH_NUM_FM_CHANNELS) {
		return 0;
	}
	int owner = values[GLOBAL_POLYPHONY] - 1;
	if (owner < 0) {
		return 1;
	}
	// With the DAC on, the sixth voice plays samples and stays with channel 6.
	bool dac = values[GLOBAL_DAC] != 0;
	if (channel == owner) {
		return dac ? PATCH_NUM_FM_CHANNELS - 1 : PATCH_NUM_FM_CHANNELS;
	}
	return dac && channel == PATCH_NUM_FM_CHANNELS - 1 ? 1 : 0;
}

// The MIDI channel a voice should answer to.
static int voice_owner(const int16_t *values, int voice)
{
	int owner = values[GLOBAL_POLYPHONY] - 1;
	if (owner < 0 || (voice == PATCH_NUM_FM_CHANNELS - 1 && values[GLOBAL_DAC])) {
		return voice;
	}
	return owner;
}

static void load_preset(int16_t *channel, int program)
{
	channel[PARAM_PRESET] = (int16_t)program;
	for (int param = PARAM_ALGORITHM; param < PATCH_NUM_CHANNEL_PARAMS; param++) {
		channel[param] = PRESETS_GM[program][preset_field(param)];
	}
}

static void default_values(patch_values *values)
{
	for (int g = 0; g < PATCH_NUM_GLOBALS; g++) {
		values->globals[g] = globals[g].default_value;
	}
	for (int channel = 0; channel < PATCH_NUM_CHANNELS; channel++) {
		for (int param = 0; param < PATCH_NUM_CHANNEL_PARAMS; param++) {
			values->channels[channel][param] = channel_params[param].default_value;
		}
	}
}

void patch_init(mdmi_patch *patch)
{
	memset(patch, 0, sizeof(*patch));
	default_values(&patch->model);
	// A booted ROM holds exactly these values, reg_test checks it.
	patch->rom.values = patch->model;
	for (int voice = 0; voice < PATCH_NUM_FM_CHANNELS; voice++) {
		patch->rom.voice_owner[voice] = (int8_t)voice;
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
	if (incoming == INCOMING_READY || incoming == INCOMING_TAKING) {
		return &patch->incoming;
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

int patch_get(mdmi_patch *patch, uint32_t id)
{
	const int16_t *value = value_of((patch_values *)patch_view(patch), id);
	return value ? *value : 0;
}

double patch_model_filter_hz(const mdmi_patch *patch)
{
	return patch_filter_hz(patch->model.globals[GLOBAL_FILTER]);
}

void patch_format_preset(int channel, int value, char *text, size_t size)
{
	if (channel < PATCH_NUM_FM_CHANNELS) {
		snprintf(text, size, "%d %s", value + 1, GM_NAMES[value & 127]);
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
	case GLOBAL_POLYPHONY:
		if (value) {
			snprintf(text, size, "Ch %d", value);
		} else {
			snprintf(text, size, "Off");
		}
		break;
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

void patch_format(uint32_t id, int value, char *text, size_t size)
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
		patch_format_preset(channel, value, text, size);
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
static bool set_preset(int16_t *values, int channel, int value)
{
	if (values[PARAM_PRESET] == value) {
		return false;
	}
	values[PARAM_PRESET] = (int16_t)value;
	if (channel < PATCH_NUM_FM_CHANNELS) {
		load_preset(values, value);
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
		return set_preset(values, channel, value);
	}
	values[param] = (int16_t)value;
	return false;
}

static void emit_cc(patch_emit_fn emit, void *context, int channel, int cc, int value)
{
	const uint8_t message[3] = {(uint8_t)(0xB0 | channel), (uint8_t)cc, (uint8_t)value};
	emit(context, message, sizeof(message));
}

static void forget_voice(patch_receiver *receiver, int channel)
{
	int16_t *rom = receiver->values.channels[channel];
	rom[PARAM_PRESET] = PATCH_UNKNOWN;
	for (int param = PARAM_ALGORITHM; param < PATCH_NUM_CHANNEL_PARAMS; param++) {
		rom[param] = PATCH_UNKNOWN;
	}
}

static void forget_everything(patch_receiver *receiver)
{
	memset(&receiver->values, 0xFF, sizeof(receiver->values));
	memset(receiver->voice_owner, 0xFF, sizeof(receiver->voice_owner));
}

static bool owns_voice(const patch_receiver *receiver, int channel)
{
	for (int voice = 0; voice < PATCH_NUM_FM_CHANNELS; voice++) {
		if (receiver->voice_owner[voice] == channel) {
			return true;
		}
	}
	return false;
}

// A whole voice as the ROM's stored program, selected under one of the channel's two service
// numbers: the ROM skips a program it thinks is loaded, and a stored one shadows the built-in.
// The program reaches every voice the channel has.
static void send_whole_voice(const int16_t *model, patch_receiver *receiver, int channel, patch_emit_fn emit, void *context)
{
	int program = FIRST_SERVICE_PROGRAM + channel * 2 + receiver->alternate[channel];
	receiver->alternate[channel] = !receiver->alternate[channel];
	uint8_t sysex[VOICE_SYSEX_HEAD + VOICE_BYTES + 1] = {0xF0, 0x00, 0x22, 0x77, 0x0A, 0x00, (uint8_t)program};
	uint8_t *voice = sysex + VOICE_SYSEX_HEAD;
	for (int param = PARAM_ALGORITHM; param < PATCH_NUM_CHANNEL_PARAMS; param++) {
		voice[preset_field(param)] = (uint8_t)model[param];
	}
	sysex[sizeof(sysex) - 1] = 0xF7;
	emit(context, sysex, sizeof(sysex));
	const uint8_t select[2] = {(uint8_t)(0xC0 | channel), (uint8_t)program};
	emit(context, select, sizeof(select));
	int16_t *rom = receiver->values.channels[channel];
	rom[PARAM_PRESET] = model[PARAM_PRESET];
	for (int param = PARAM_ALGORITHM; param < PATCH_NUM_CHANNEL_PARAMS; param++) {
		rom[param] = model[param];
	}
}

// The ROM's CC only reaches the first 16 sustain rates. Registers hold operators in the order 1, 3, 2, 4.
static void write_sustain_rate(int voice, int op, int value, patch_emit_fn emit, void *context)
{
	int slot = op == 1 ? 2 : op == 2 ? 1 : op;
	int reg = YM_REG_SUSTAIN_RATE + voice % 3 + slot * 4;
	const uint8_t sysex[10] = {0xF0, 0x00, 0x22, 0x77, (uint8_t)(voice < 3 ? 0x08 : 0x09), (uint8_t)(reg >> 4), (uint8_t)(reg & 15), (uint8_t)(value >> 4), (uint8_t)(value & 15), 0xF7};
	emit(context, sysex, sizeof(sysex));
}

static void sync_voice_param(const int16_t *model, patch_receiver *receiver, int param, int cc, int cc_range, int channel, patch_emit_fn emit, void *context)
{
	int16_t *rom = receiver->values.channels[channel];
	int value = model[param];
	if (value == rom[param]) {
		return;
	}
	rom[param] = (int16_t)value;
	if (value < cc_range) {
		emit_cc(emit, context, channel, cc, value * (128 / cc_range));
		return;
	}
	int op = (param - PARAM_OP_BASE) / PATCH_OP_PARAMS;
	for (int voice = 0; voice < PATCH_NUM_FM_CHANNELS; voice++) {
		if (receiver->voice_owner[voice] == channel) {
			write_sustain_rate(voice, op, value, emit, context);
		}
	}
}

static void sync_voice(const int16_t *model, patch_receiver *receiver, int channel, patch_emit_fn emit, void *context)
{
	int16_t *rom = receiver->values.channels[channel];
	bool unknown = rom[PARAM_ALGORITHM] == PATCH_UNKNOWN;
	if (unknown || (model[PARAM_PRESET] != rom[PARAM_PRESET] && model[PARAM_PRESET] >= FIRST_SERVICE_PROGRAM)) {
		send_whole_voice(model, receiver, channel, emit, context);
		return;
	}
	if (model[PARAM_PRESET] != rom[PARAM_PRESET]) {
		const uint8_t message[2] = {(uint8_t)(0xC0 | channel), (uint8_t)model[PARAM_PRESET]};
		emit(context, message, sizeof(message));
		load_preset(rom, model[PARAM_PRESET]);
	}
	sync_voice_param(model, receiver, PARAM_ALGORITHM, CC_ALGORITHM, 8, channel, emit, context);
	sync_voice_param(model, receiver, PARAM_FEEDBACK, CC_FEEDBACK, 8, channel, emit, context);
	sync_voice_param(model, receiver, PARAM_AMS, CC_AMS, 4, channel, emit, context);
	sync_voice_param(model, receiver, PARAM_FMS, CC_FMS, 8, channel, emit, context);
	for (int op = 0; op < PATCH_OPS; op++) {
		for (int i = 0; i < PATCH_OP_PARAMS; i++) {
			sync_voice_param(model, receiver, PARAM_OP_BASE + op * PATCH_OP_PARAMS + i, OP_PARAMS[i].cc + op, OP_PARAMS[i].cc_range, channel, emit, context);
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

// Gives each FM voice to the channel Polyphony names (D17). The ROM's static mapping plays a note
// only on its own channel's voices, so a channel without one stays silent and steals nothing.
static void sync_voice_owners(const int16_t *model_globals, patch_receiver *receiver, patch_emit_fn emit, void *context)
{
	bool unknown = false;
	for (int voice = 0; voice < PATCH_NUM_FM_CHANNELS; voice++) {
		unknown = unknown || receiver->voice_owner[voice] == PATCH_UNKNOWN;
	}
	if (unknown) {
		// Static mapping, which also gives every voice back to its own channel. A GM reset would
		// otherwise leave the ROM taking any free voice for any channel.
		const uint8_t sysex[7] = {0xF0, 0x00, 0x22, 0x77, SYSEX_DYNAMIC, 0x00, 0xF7};
		emit(context, sysex, sizeof(sysex));
		for (int voice = 0; voice < PATCH_NUM_FM_CHANNELS; voice++) {
			receiver->voice_owner[voice] = (int8_t)voice;
		}
	}
	bool silenced[PATCH_NUM_FM_CHANNELS] = {false};
	for (int voice = 0; voice < PATCH_NUM_FM_CHANNELS; voice++) {
		int owner = voice_owner(model_globals, voice), previous = receiver->voice_owner[voice];
		if (owner == previous) {
			continue;
		}
		// A note left sounding on a voice that changes hands would never get its note off.
		if (!silenced[previous]) {
			emit_cc(emit, context, previous, CC_ALL_NOTES_OFF, 0);
			silenced[previous] = true;
		}
		const uint8_t sysex[8] = {0xF0, 0x00, 0x22, 0x77, SYSEX_REMAP, (uint8_t)owner, (uint8_t)voice, 0xF7};
		emit(context, sysex, sizeof(sysex));
		receiver->voice_owner[voice] = (int8_t)owner;
		// The voice arrives with another channel's patch, which the ROM would replace with a
		// built-in program on its first note. Sending the channel whole puts the same patch on all.
		forget_voice(receiver, owner);
		receiver->values.channels[owner][PARAM_VOLUME] = PATCH_UNKNOWN;
		receiver->values.channels[owner][PARAM_PAN] = PATCH_UNKNOWN;
	}
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
		for (int channel = 0; channel < PATCH_NUM_FM_CHANNELS; channel++) {
			forget_voice(receiver, channel);
		}
	}
	sync_voice_owners(model_globals, receiver, emit, context);
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
		} else if (!owns_voice(receiver, channel)) {
			// Nothing receives these values until the channel has a voice again.
			continue;
		} else {
			sync_voice(model, receiver, channel, emit, context);
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
	if (bytes[0] == 0xF0 || bytes[0] == 0xFF) {
		emit(context, bytes, length);
		forget_everything(&patch->rom);
		patch_sync(patch, &patch->rom, emit, context);
		return false;
	}
	if (status == 0xC0 && length >= 2 && is_channel) {
		// Unlike a lane or a CC repeating the preset, a program from a clip always loads, tweaks gone.
		int16_t *model = patch->model.channels[channel];
		model[PARAM_PRESET] = bytes[1];
		if (fm) {
			load_preset(model, bytes[1]);
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
		if (cc == CC_STORE_PROGRAM) {
			// Would make a program number mean something other than its preset, for good.
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
				moved = set_preset(patch->model.channels[channel], channel, value * PRESETS / 128);
				changed = true;
			}
		} else if (cc == CC_POLYPHONY) {
			// All voices to the channel it came on, or none (D17). PSG channels can't have them.
			if (value < 64) {
				patch->model.globals[GLOBAL_POLYPHONY] = 0;
				changed = true;
			} else if (fm) {
				patch->model.globals[GLOBAL_POLYPHONY] = (int16_t)(channel + 1);
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

void patch_save(mdmi_patch *patch, void *buffer)
{
	const patch_values *values = patch_view(patch);
	uint8_t *out = buffer;
	for (uint32_t i = 0; i < PATCH_NUM_IDS; i++) {
		uint32_t id = panel_order[i];
		int16_t value = *value_of((patch_values *)values, id);
		memcpy(out, &id, sizeof(id));
		memcpy(out + sizeof(id), &value, sizeof(value));
		out += SAVED_PAIR_BYTES;
	}
}

bool patch_load(mdmi_patch *patch, const void *buffer, size_t size)
{
	if (size % SAVED_PAIR_BYTES) {
		return false;
	}
	patch_values incoming;
	default_values(&incoming);
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
	// A newer state replaces one that was never taken. Only a take in progress has to finish first.
	uint32_t expected;
	do {
		expected = INCOMING_FREE;
		if (__atomic_compare_exchange_n(&patch->incoming_state, &expected, INCOMING_WRITING, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
			break;
		}
		expected = INCOMING_READY;
	} while (!__atomic_compare_exchange_n(&patch->incoming_state, &expected, INCOMING_WRITING, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
	patch->incoming = incoming;
	__atomic_store_n(&patch->incoming_state, INCOMING_READY, __ATOMIC_RELEASE);
	return true;
}

bool patch_take_incoming(mdmi_patch *patch)
{
	uint32_t expected = INCOMING_READY;
	if (!__atomic_compare_exchange_n(&patch->incoming_state, &expected, INCOMING_TAKING, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
		return false;
	}
	patch->model = patch->incoming;
	patch->dirty = true;
	patch_publish(patch);
	__atomic_store_n(&patch->incoming_state, INCOMING_FREE, __ATOMIC_RELEASE);
	return true;
}
