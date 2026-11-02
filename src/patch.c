#include <math.h>
#include <stdio.h>
#include <string.h>
#include "patch.h"
#include "presets_gm.h"

#define STATE_VERSION 2
#define STATE_V1_GLOBALS 5
#define STATE_V1_CHANNEL_PARAMS 51
#define STATE_V1_SIZE (sizeof(uint32_t) + sizeof(int16_t) * (STATE_V1_GLOBALS + PATCH_NUM_CHANNELS * STATE_V1_CHANNEL_PARAMS))

#define CC_LFO_RATE 1
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
// Stored voices shadow the ROM's built-in programs from here on, two numbers per FM channel.
#define FIRST_SERVICE_PROGRAM 116
#define VOICE_SYSEX_HEAD 7
#define VOICE_BYTES 48
#define SNAPSHOT_INDEX 3u
#define SNAPSHOT_NEW 4u
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

static patch_param_info params[PATCH_NUM_PARAMS];
static uint32_t panel_order[PATCH_NUM_PARAMS];

static bool is_global(uint32_t id)
{
	return id < PARAM_PRESET || id == PARAM_CH3_MODE || id == PARAM_DAC;
}

static int global_index(uint32_t id)
{
	return id < PARAM_PRESET ? (int)id : PARAM_PRESET + (int)(id - PARAM_CH3_MODE);
}

static int channel_index(uint32_t id)
{
	return id < PARAM_CH3_MODE ? (int)id - PARAM_PRESET : PARAM_CH3_MODE - PARAM_PRESET + (int)(id - PARAM_FINE_TUNE);
}

static void define_param(uint32_t id, const char *name, int max, int default_value, bool stepped)
{
	snprintf(params[id].name, sizeof(params[id].name), "%s", name);
	params[id].max = (int16_t)max;
	params[id].default_value = (int16_t)default_value;
	params[id].stepped = stepped;
}

void patch_global_init(void)
{
	define_param(PARAM_EDIT_CHANNEL, "Edit Channel", PATCH_NUM_CHANNELS - 1, 0, true);
	define_param(PARAM_POLYPHONY, "Polyphony", 1, 0, true);
	define_param(PARAM_FILTER, "Filter", FILTER_MAX, FILTER_DEFAULT, false);
	define_param(PARAM_LFO_ON, "LFO On", 1, 1, true);
	define_param(PARAM_LFO_RATE, "LFO Rate", 7, 0, true);
	define_param(PARAM_PRESET, "Preset", 127, 0, true);
	define_param(PARAM_VOLUME, "Volume", 127, 127, true);
	define_param(PARAM_PAN, "Pan", 2, 1, true);
	define_param(PARAM_ALGORITHM, "Algorithm", 7, PRESETS_GM[0][0], true);
	define_param(PARAM_FEEDBACK, "Feedback", 7, PRESETS_GM[0][1], true);
	define_param(PARAM_AMS, "AMS", 3, PRESETS_GM[0][2], true);
	define_param(PARAM_FMS, "FMS", 7, PRESETS_GM[0][3], true);
	for (int op = 0; op < PATCH_OPS; op++) {
		for (int i = 0; i < PATCH_OP_PARAMS; i++) {
			char name[32];
			snprintf(name, sizeof(name), "Op%d %s", op + 1, OP_PARAMS[i].name);
			define_param(PARAM_OP_BASE + op * PATCH_OP_PARAMS + i, name, OP_PARAMS[i].max,
				PRESETS_GM[0][4 + op * PATCH_OP_PARAMS + OP_PARAMS[i].preset_field], true);
		}
	}
	define_param(PARAM_CH3_MODE, "Ch3 Special Mode", 1, 0, true);
	define_param(PARAM_DAC, "DAC Enable", 1, 0, true);
	define_param(PARAM_FINE_TUNE, "Fine Tune", 127, 64, true);
	define_param(PARAM_BEND_RANGE, "Pitch Bend Range", BEND_RANGE_MAX, 2, true);
	define_param(PARAM_PORTA_ON, "Portamento On", 1, 0, true);
	define_param(PARAM_PORTA_TIME, "Portamento Time", 127, 64, true);

	static const uint32_t head[] = {
		PARAM_EDIT_CHANNEL, PARAM_POLYPHONY, PARAM_FILTER, PARAM_LFO_ON, PARAM_LFO_RATE, PARAM_CH3_MODE, PARAM_DAC,
		PARAM_PRESET, PARAM_VOLUME, PARAM_PAN, PARAM_FINE_TUNE, PARAM_BEND_RANGE, PARAM_PORTA_ON, PARAM_PORTA_TIME,
		PARAM_ALGORITHM, PARAM_FEEDBACK, PARAM_AMS, PARAM_FMS,
	};
	uint32_t position = 0;
	for (size_t i = 0; i < sizeof(head) / sizeof(*head); i++) {
		panel_order[position++] = head[i];
	}
	for (uint32_t id = PARAM_OP_BASE; id < PARAM_CH3_MODE; id++) {
		panel_order[position++] = id;
	}
}

uint32_t patch_param_id_at(uint32_t panel_index)
{
	return panel_index < PATCH_NUM_PARAMS ? panel_order[panel_index] : PATCH_NUM_PARAMS;
}

const patch_param_info *patch_param(uint32_t id)
{
	return id < PATCH_NUM_PARAMS ? &params[id] : NULL;
}

static bool is_fm_only(uint32_t id)
{
	return id >= PARAM_ALGORITHM && id < PARAM_CH3_MODE;
}

static void load_preset(int16_t *channel, int program)
{
	channel[channel_index(PARAM_PRESET)] = (int16_t)program;
	channel[channel_index(PARAM_ALGORITHM)] = PRESETS_GM[program][0];
	channel[channel_index(PARAM_FEEDBACK)] = PRESETS_GM[program][1];
	channel[channel_index(PARAM_AMS)] = PRESETS_GM[program][2];
	channel[channel_index(PARAM_FMS)] = PRESETS_GM[program][3];
	for (int op = 0; op < PATCH_OPS; op++) {
		for (int i = 0; i < PATCH_OP_PARAMS; i++) {
			channel[channel_index(PARAM_OP_BASE) + op * PATCH_OP_PARAMS + i] =
				PRESETS_GM[program][4 + op * PATCH_OP_PARAMS + OP_PARAMS[i].preset_field];
		}
	}
}

static void default_values(patch_values *values)
{
	for (uint32_t id = 0; id < PATCH_NUM_PARAMS; id++) {
		if (is_global(id)) {
			values->globals[global_index(id)] = params[id].default_value;
		} else {
			for (int ch = 0; ch < PATCH_NUM_CHANNELS; ch++) {
				values->channels[ch][channel_index(id)] = params[id].default_value;
			}
		}
	}
}

void patch_init(mdmi_patch *patch)
{
	memset(patch, 0, sizeof(*patch));
	default_values(&patch->model);
	// A booted ROM holds exactly these values, reg_test checks it.
	patch->rom.values = patch->model;
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
static const patch_values *latest(mdmi_patch *patch)
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

// With polyphony on, every FM voice belongs to MIDI channel 1, so all FM editing goes there.
static int effective_channel(const patch_values *values)
{
	int channel = values->globals[PARAM_EDIT_CHANNEL];
	return values->globals[PARAM_POLYPHONY] && channel < PATCH_NUM_FM_CHANNELS ? 0 : channel;
}

int patch_get(mdmi_patch *patch, uint32_t id)
{
	const patch_values *values = latest(patch);
	if (is_global(id)) {
		return values->globals[global_index(id)];
	}
	return values->channels[effective_channel(values)][channel_index(id)];
}

int patch_get_channel(mdmi_patch *patch, int channel, uint32_t id)
{
	return latest(patch)->channels[channel][channel_index(id)];
}

double patch_model_filter_hz(const mdmi_patch *patch)
{
	return patch_filter_hz(patch->model.globals[PARAM_FILTER]);
}

void patch_format_preset(int channel, int value, char *text, size_t size)
{
	if (channel < PATCH_NUM_FM_CHANNELS) {
		snprintf(text, size, "%d %s", value + 1, GM_NAMES[value & 127]);
	} else {
		snprintf(text, size, "Envelope %d", value);
	}
}

double patch_filter_hz(int value)
{
	if (value >= FILTER_MAX) {
		return FILTER_OFF_HZ;
	}
	return FILTER_MIN_HZ * pow(FILTER_RANGE, value / (double)FILTER_MAX);
}

static void format_channel(int channel, char *text, size_t size)
{
	if (channel < PATCH_NUM_FM_CHANNELS) {
		snprintf(text, size, "%d FM", channel + 1);
	} else if (channel < PATCH_NUM_CHANNELS - 1) {
		snprintf(text, size, "%d PSG", channel + 1);
	} else {
		snprintf(text, size, "%d Noise", channel + 1);
	}
}

void patch_format(mdmi_patch *patch, uint32_t id, int value, char *text, size_t size)
{
	switch (id) {
	case PARAM_EDIT_CHANNEL:
		format_channel(value, text, size);
		break;
	case PARAM_POLYPHONY:
	case PARAM_LFO_ON:
	case PARAM_CH3_MODE:
	case PARAM_DAC:
	case PARAM_PORTA_ON:
		snprintf(text, size, "%s", value ? "On" : "Off");
		break;
	case PARAM_FINE_TUNE:
		snprintf(text, size, "%+d ct", value - 64);
		break;
	case PARAM_BEND_RANGE:
		snprintf(text, size, "%d st", value);
		break;
	case PARAM_FILTER:
		if (value >= FILTER_MAX) {
			snprintf(text, size, "Off");
		} else {
			snprintf(text, size, "%.0f Hz", patch_filter_hz(value));
		}
		break;
	case PARAM_LFO_RATE:
		snprintf(text, size, "%s", LFO_RATES[value & 7]);
		break;
	case PARAM_PRESET:
		patch_format_preset(effective_channel(latest(patch)), value, text, size);
		break;
	case PARAM_PAN:
		snprintf(text, size, "%s", value == 0 ? "Left" : value == 1 ? "Center" : "Right");
		break;
	default:
		if (id >= PARAM_OP_BASE && id < PARAM_CH3_MODE && (id - PARAM_OP_BASE) % PATCH_OP_PARAMS == OP_SSG_FIELD) {
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

bool patch_set(mdmi_patch *patch, uint32_t id, int value)
{
	if (id >= PATCH_NUM_PARAMS) {
		return false;
	}
	if (value < 0) {
		value = 0;
	} else if (value > params[id].max) {
		value = params[id].max;
	}
	patch->dirty = true;
	if (is_global(id)) {
		bool changed = patch->model.globals[global_index(id)] != value;
		patch->model.globals[global_index(id)] = (int16_t)value;
		// These two change which channel the other knobs show.
		return changed && (id == PARAM_EDIT_CHANNEL || id == PARAM_POLYPHONY);
	}
	int channel = effective_channel(&patch->model);
	if (is_fm_only(id) && channel >= PATCH_NUM_FM_CHANNELS) {
		return false;
	}
	int16_t *values = patch->model.channels[channel];
	if (id == PARAM_PRESET) {
		// A lane repeating its value must not wipe tweaks. Only a new preset loads.
		if (values[channel_index(id)] == value) {
			return false;
		}
		values[channel_index(PARAM_PRESET)] = (int16_t)value;
		if (channel < PATCH_NUM_FM_CHANNELS) {
			load_preset(values, value);
		}
		return true;
	}
	values[channel_index(id)] = (int16_t)value;
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
	rom[channel_index(PARAM_PRESET)] = PATCH_UNKNOWN;
	for (uint32_t id = PARAM_ALGORITHM; id < PARAM_CH3_MODE; id++) {
		rom[channel_index(id)] = PATCH_UNKNOWN;
	}
}

static void forget_everything(patch_receiver *receiver)
{
	memset(&receiver->values, 0xFF, sizeof(receiver->values));
}

// A whole voice as the ROM's stored program, selected under one of the channel's two service
// numbers: the ROM skips a program it thinks is loaded, and a stored one shadows the built-in.
static void send_whole_voice(const int16_t *model, patch_receiver *receiver, int channel, patch_emit_fn emit, void *context)
{
	int program = FIRST_SERVICE_PROGRAM + channel * 2 + receiver->alternate[channel];
	receiver->alternate[channel] = !receiver->alternate[channel];
	uint8_t sysex[VOICE_SYSEX_HEAD + VOICE_BYTES + 1] = {0xF0, 0x00, 0x22, 0x77, 0x0A, 0x00, (uint8_t)program};
	uint8_t *voice = sysex + VOICE_SYSEX_HEAD;
	voice[0] = (uint8_t)model[channel_index(PARAM_ALGORITHM)];
	voice[1] = (uint8_t)model[channel_index(PARAM_FEEDBACK)];
	voice[2] = (uint8_t)model[channel_index(PARAM_AMS)];
	voice[3] = (uint8_t)model[channel_index(PARAM_FMS)];
	for (int op = 0; op < PATCH_OPS; op++) {
		for (int i = 0; i < PATCH_OP_PARAMS; i++) {
			voice[4 + op * PATCH_OP_PARAMS + OP_PARAMS[i].preset_field] = (uint8_t)model[channel_index(PARAM_OP_BASE) + op * PATCH_OP_PARAMS + i];
		}
	}
	sysex[sizeof(sysex) - 1] = 0xF7;
	emit(context, sysex, sizeof(sysex));
	const uint8_t select[2] = {(uint8_t)(0xC0 | channel), (uint8_t)program};
	emit(context, select, sizeof(select));
	int16_t *rom = receiver->values.channels[channel];
	rom[channel_index(PARAM_PRESET)] = model[channel_index(PARAM_PRESET)];
	for (uint32_t id = PARAM_ALGORITHM; id < PARAM_CH3_MODE; id++) {
		rom[channel_index(id)] = model[channel_index(id)];
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

static void sync_voice_param(const int16_t *model, int16_t *rom, uint32_t id, int cc, int cc_range, int channel, bool all_voices, patch_emit_fn emit, void *context)
{
	int index = channel_index(id);
	int value = model[index];
	if (value == rom[index]) {
		return;
	}
	rom[index] = (int16_t)value;
	if (value < cc_range) {
		emit_cc(emit, context, channel, cc, value * (128 / cc_range));
		return;
	}
	int op = (int)(id - PARAM_OP_BASE) / PATCH_OP_PARAMS;
	for (int voice = all_voices ? 0 : channel; voice < (all_voices ? PATCH_NUM_FM_CHANNELS : channel + 1); voice++) {
		write_sustain_rate(voice, op, value, emit, context);
	}
}

static void sync_voice(const int16_t *model, patch_receiver *receiver, int channel, bool all_voices, patch_emit_fn emit, void *context)
{
	int16_t *rom = receiver->values.channels[channel];
	int preset = channel_index(PARAM_PRESET);
	bool unknown = rom[channel_index(PARAM_ALGORITHM)] == PATCH_UNKNOWN;
	if (unknown || (model[preset] != rom[preset] && model[preset] >= FIRST_SERVICE_PROGRAM)) {
		send_whole_voice(model, receiver, channel, emit, context);
		return;
	}
	if (model[preset] != rom[preset]) {
		const uint8_t message[2] = {(uint8_t)(0xC0 | channel), (uint8_t)model[preset]};
		emit(context, message, sizeof(message));
		load_preset(rom, model[preset]);
	}
	sync_voice_param(model, rom, PARAM_ALGORITHM, CC_ALGORITHM, 8, channel, all_voices, emit, context);
	sync_voice_param(model, rom, PARAM_FEEDBACK, CC_FEEDBACK, 8, channel, all_voices, emit, context);
	sync_voice_param(model, rom, PARAM_AMS, CC_AMS, 4, channel, all_voices, emit, context);
	sync_voice_param(model, rom, PARAM_FMS, CC_FMS, 8, channel, all_voices, emit, context);
	for (int op = 0; op < PATCH_OPS; op++) {
		for (int i = 0; i < PATCH_OP_PARAMS; i++) {
			sync_voice_param(model, rom, PARAM_OP_BASE + op * PATCH_OP_PARAMS + i, OP_PARAMS[i].cc + op, OP_PARAMS[i].cc_range, channel, all_voices, emit, context);
		}
	}
}

static void sync_channel_params(mdmi_patch *patch, const int16_t *model, int16_t *rom, int channel, patch_emit_fn emit, void *context)
{
	int volume = channel_index(PARAM_VOLUME), pan = channel_index(PARAM_PAN);
	if (model[volume] != rom[volume]) {
		emit_cc(emit, context, channel, CC_VOLUME, model[volume]);
		rom[volume] = model[volume];
	}
	if (model[pan] != rom[pan]) {
		emit_cc(emit, context, channel, CC_PAN, PAN_CC[model[pan]]);
		rom[pan] = model[pan];
	}
	int fine_tune = channel_index(PARAM_FINE_TUNE);
	if (model[fine_tune] != rom[fine_tune]) {
		emit_cc(emit, context, channel, CC_FINE_TUNE, model[fine_tune]);
		rom[fine_tune] = model[fine_tune];
	}
	int time = channel_index(PARAM_PORTA_TIME);
	if (model[time] != rom[time]) {
		emit_cc(emit, context, channel, CC_PORTA_TIME, model[time]);
		rom[time] = model[time];
	}
	int on = channel_index(PARAM_PORTA_ON);
	if (model[on] != rom[on]) {
		emit_cc(emit, context, channel, CC_PORTA_ON, model[on] ? CC_ON : 0);
		rom[on] = model[on];
	}
	int bend = channel_index(PARAM_BEND_RANGE);
	if (model[bend] != rom[bend]) {
		// RPN 0,0, then back to the null RPN so stray data entry messages change nothing.
		emit_cc(emit, context, channel, CC_RPN_MSB, 0);
		emit_cc(emit, context, channel, CC_RPN_LSB, 0);
		emit_cc(emit, context, channel, CC_DATA_ENTRY_MSB, model[bend]);
		emit_cc(emit, context, channel, CC_DATA_ENTRY_LSB, 0);
		emit_cc(emit, context, channel, CC_RPN_MSB, RPN_NULL);
		emit_cc(emit, context, channel, CC_RPN_LSB, RPN_NULL);
		patch->rpn_msb[channel] = patch->rpn_lsb[channel] = RPN_NULL;
		rom[bend] = model[bend];
	}
}

void patch_sync(mdmi_patch *patch, patch_receiver *receiver, patch_emit_fn emit, void *context)
{
	const int16_t *model_globals = patch->model.globals;
	int16_t *rom_globals = receiver->values.globals;
	int dac = global_index(PARAM_DAC), ch3 = global_index(PARAM_CH3_MODE);
	if (model_globals[dac] != rom_globals[dac]) {
		// The ROM swaps the Z80 driver for this, and the Z80's reset line also clears the YM2612.
		// The ROM doesn't know, so FM stays silent until its registers are written again.
		emit_cc(emit, context, 0, CC_DAC, model_globals[dac] ? CC_ON : 0);
		rom_globals[dac] = model_globals[dac];
		rom_globals[PARAM_LFO_ON] = rom_globals[PARAM_LFO_RATE] = rom_globals[ch3] = PATCH_UNKNOWN;
		for (int channel = 0; channel < PATCH_NUM_FM_CHANNELS; channel++) {
			forget_voice(receiver, channel);
		}
	}
	if (model_globals[PARAM_POLYPHONY] != rom_globals[PARAM_POLYPHONY]) {
		emit_cc(emit, context, 0, CC_POLYPHONY, model_globals[PARAM_POLYPHONY] ? CC_ON : 0);
		rom_globals[PARAM_POLYPHONY] = model_globals[PARAM_POLYPHONY];
		// Remapping leaves voices with whatever patch they had, and the ROM loads a voice's program
		// over it on the first note unless that program is the channel's.
		for (int channel = 0; channel < PATCH_NUM_FM_CHANNELS; channel++) {
			forget_voice(receiver, channel);
			receiver->values.channels[channel][channel_index(PARAM_VOLUME)] = PATCH_UNKNOWN;
			receiver->values.channels[channel][channel_index(PARAM_PAN)] = PATCH_UNKNOWN;
		}
	}
	if (model_globals[PARAM_LFO_ON] != rom_globals[PARAM_LFO_ON]) {
		emit_cc(emit, context, 0, CC_LFO_ON, model_globals[PARAM_LFO_ON] ? CC_ON : 0);
		rom_globals[PARAM_LFO_ON] = model_globals[PARAM_LFO_ON];
	}
	if (model_globals[PARAM_LFO_RATE] != rom_globals[PARAM_LFO_RATE]) {
		emit_cc(emit, context, 0, CC_LFO_RATE, model_globals[PARAM_LFO_RATE] * (128 / 8));
		rom_globals[PARAM_LFO_RATE] = model_globals[PARAM_LFO_RATE];
	}
	if (model_globals[ch3] != rom_globals[ch3]) {
		emit_cc(emit, context, 0, CC_CH3_MODE, model_globals[ch3] ? CC_ON : 0);
		rom_globals[ch3] = model_globals[ch3];
	}
	bool polyphony = model_globals[PARAM_POLYPHONY] != 0;
	for (int channel = 0; channel < PATCH_NUM_CHANNELS; channel++) {
		const int16_t *model = patch->model.channels[channel];
		int16_t *rom = receiver->values.channels[channel];
		if (channel >= PATCH_NUM_FM_CHANNELS) {
			int preset = channel_index(PARAM_PRESET);
			if (model[preset] != rom[preset]) {
				const uint8_t message[2] = {(uint8_t)(0xC0 | channel), (uint8_t)model[preset]};
				emit(context, message, sizeof(message));
				rom[preset] = model[preset];
			}
		} else if (polyphony && channel != 0) {
			// Nothing is mapped to receive these values until polyphony ends.
			continue;
		} else {
			sync_voice(model, receiver, channel, polyphony, emit, context);
		}
		sync_channel_params(patch, model, rom, channel, emit, context);
	}
}

// Controllers that are the model's values. Operator controllers follow OP_PARAMS.
typedef struct {
	uint8_t cc;
	uint8_t id;
	uint8_t cc_range;
} cc_param;

static const cc_param CC_PARAMS[] = {
	{CC_ALGORITHM, PARAM_ALGORITHM, 8},
	{CC_FEEDBACK, PARAM_FEEDBACK, 8},
	{CC_AMS, PARAM_AMS, 4},
	{CC_FMS, PARAM_FMS, 8},
	{CC_VOLUME, PARAM_VOLUME, 128},
	{CC_FINE_TUNE, PARAM_FINE_TUNE, 128},
	{CC_PORTA_TIME, PARAM_PORTA_TIME, 128},
	{CC_PORTA_ON, PARAM_PORTA_ON, 2},
	{CC_LFO_ON, PARAM_LFO_ON, 2},
	{CC_LFO_RATE, PARAM_LFO_RATE, 8},
	{CC_CH3_MODE, PARAM_CH3_MODE, 2},
	{CC_DAC, PARAM_DAC, 2},
	{CC_POLYPHONY, PARAM_POLYPHONY, 2},
};

// Finds the model's value for a controller on a channel. False means it isn't one of ours.
static bool cc_to_model(int cc, int value, uint32_t *id, int *result)
{
	for (int op = 0; op < PATCH_OPS; op++) {
		for (int i = 0; i < PATCH_OP_PARAMS; i++) {
			if (OP_PARAMS[i].cc + op == cc) {
				*id = PARAM_OP_BASE + op * PATCH_OP_PARAMS + i;
				*result = value / (128 / OP_PARAMS[i].cc_range);
				return true;
			}
		}
	}
	for (size_t i = 0; i < sizeof(CC_PARAMS) / sizeof(*CC_PARAMS); i++) {
		if (CC_PARAMS[i].cc == cc) {
			*id = CC_PARAMS[i].id;
			*result = value / (128 / CC_PARAMS[i].cc_range);
			return true;
		}
	}
	if (cc == CC_PAN) {
		*id = PARAM_PAN;
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
	bool changed = false;
	if (bytes[0] == 0xF0 || bytes[0] == 0xFF) {
		emit(context, bytes, length);
		forget_everything(&patch->rom);
		patch_sync(patch, &patch->rom, emit, context);
		return false;
	}
	if (status == 0xC0 && length >= 2 && is_channel) {
		// Unlike a lane repeating the Preset knob, a program from a clip always loads, tweaks gone.
		int16_t *model = patch->model.channels[channel];
		model[channel_index(PARAM_PRESET)] = bytes[1];
		if (fm) {
			load_preset(model, bytes[1]);
		}
		changed = true;
	} else if (status == 0xB0 && length >= 3) {
		int cc = bytes[1], value = bytes[2];
		uint32_t id;
		int result;
		if (cc == CC_RESET_ALL) {
			// The ROM takes this as a reset of the channel and its program, so send what it means by the standard.
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
			patch->model.channels[channel][channel_index(PARAM_BEND_RANGE)] = (int16_t)(value > BEND_RANGE_MAX ? BEND_RANGE_MAX : value);
			changed = true;
		} else if (cc == CC_STEREO) {
			// The stereo bits of the chip: 1 right, 2 left, 3 both. Off has no place on the Pan knob.
			static const int8_t STEREO_TO_PAN[4] = {-1, 2, 0, 1};
			int pan = STEREO_TO_PAN[value / (128 / 4)];
			if (pan >= 0 && fm) {
				patch->model.channels[channel][channel_index(PARAM_PAN)] = (int16_t)pan;
				changed = true;
			}
		} else if (cc_to_model(cc, value, &id, &result)) {
			if (is_global(id)) {
				patch->model.globals[global_index(id)] = (int16_t)result;
				changed = true;
			} else if (is_channel && !(is_fm_only(id) && !fm)) {
				patch->model.channels[channel][channel_index(id)] = (int16_t)result;
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
	return changed;
}

size_t patch_state_size(void)
{
	return sizeof(uint32_t) + sizeof(patch_values);
}

void patch_save(mdmi_patch *patch, void *buffer)
{
	const uint32_t version = STATE_VERSION;
	memcpy(buffer, &version, sizeof(version));
	memcpy((uint8_t *)buffer + sizeof(version), latest(patch), sizeof(patch_values));
}

bool patch_load(mdmi_patch *patch, const void *buffer, size_t size)
{
	uint32_t version;
	if (size < sizeof(version)) {
		return false;
	}
	memcpy(&version, buffer, sizeof(version));
	const uint8_t *data = (const uint8_t *)buffer + sizeof(version);

	patch_values incoming;
	if (version == STATE_VERSION && size == patch_state_size()) {
		memcpy(&incoming, data, sizeof(incoming));
	} else if (version == 1 && size == STATE_V1_SIZE) {
		// Projects saved before the tuning knobs existed: those knobs keep their defaults.
		default_values(&incoming);
		memcpy(incoming.globals, data, sizeof(int16_t) * STATE_V1_GLOBALS);
		data += sizeof(int16_t) * STATE_V1_GLOBALS;
		for (int channel = 0; channel < PATCH_NUM_CHANNELS; channel++) {
			memcpy(incoming.channels[channel], data + sizeof(int16_t) * STATE_V1_CHANNEL_PARAMS * channel, sizeof(int16_t) * STATE_V1_CHANNEL_PARAMS);
		}
	} else {
		return false;
	}

	for (uint32_t id = 0; id < PATCH_NUM_PARAMS; id++) {
		if (is_global(id)) {
			int value = incoming.globals[global_index(id)];
			if (value < 0 || value > params[id].max) {
				return false;
			}
		} else {
			for (int channel = 0; channel < PATCH_NUM_CHANNELS; channel++) {
				int value = incoming.channels[channel][channel_index(id)];
				if (value < 0 || value > params[id].max) {
					return false;
				}
			}
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
