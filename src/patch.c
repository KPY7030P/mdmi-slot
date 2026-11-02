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
	default_values(&patch->shown);
	patch->rom = patch->shown;
	memset(patch->rpn_msb, RPN_NULL, sizeof(patch->rpn_msb));
	memset(patch->rpn_lsb, RPN_NULL, sizeof(patch->rpn_lsb));
}

// With polyphony on, every FM voice belongs to MIDI channel 1, so all FM editing goes there.
static int effective_channel(const patch_values *values)
{
	int channel = __atomic_load_n(&values->globals[PARAM_EDIT_CHANNEL], __ATOMIC_RELAXED);
	bool polyphony = __atomic_load_n(&values->globals[PARAM_POLYPHONY], __ATOMIC_RELAXED) != 0;
	return polyphony && channel < PATCH_NUM_FM_CHANNELS ? 0 : channel;
}

int patch_get(const mdmi_patch *patch, uint32_t id)
{
	if (is_global(id)) {
		return __atomic_load_n(&patch->shown.globals[global_index(id)], __ATOMIC_RELAXED);
	}
	return __atomic_load_n(&patch->shown.channels[effective_channel(&patch->shown)][channel_index(id)], __ATOMIC_RELAXED);
}

int patch_get_channel(const mdmi_patch *patch, int channel, uint32_t id)
{
	return __atomic_load_n(&patch->shown.channels[channel][channel_index(id)], __ATOMIC_RELAXED);
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

void patch_format(const mdmi_patch *patch, uint32_t id, int value, char *text, size_t size)
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
		patch_format_preset(effective_channel(&patch->shown), value, text, size);
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
	if (is_global(id)) {
		bool changed = patch->shown.globals[global_index(id)] != value;
		__atomic_store_n(&patch->shown.globals[global_index(id)], (int16_t)value, __ATOMIC_RELAXED);
		// These two change which channel the other knobs show.
		return changed && (id == PARAM_EDIT_CHANNEL || id == PARAM_POLYPHONY);
	}
	int channel = effective_channel(&patch->shown);
	if (is_fm_only(id) && channel >= PATCH_NUM_FM_CHANNELS) {
		return false;
	}
	int16_t *values = patch->shown.channels[channel];
	if (id == PARAM_PRESET) {
		if (values[channel_index(id)] == value && channel >= PATCH_NUM_FM_CHANNELS) {
			return false;
		}
		values[channel_index(PARAM_PRESET)] = (int16_t)value;
		if (channel < PATCH_NUM_FM_CHANNELS) {
			load_preset(values, value);
		}
		return true;
	}
	__atomic_store_n(&values[channel_index(id)], (int16_t)value, __ATOMIC_RELAXED);
	return false;
}

static void emit_cc(patch_emit_fn emit, void *context, int channel, int cc, int value)
{
	const uint8_t message[3] = {(uint8_t)(0xB0 | channel), (uint8_t)cc, (uint8_t)value};
	emit(context, message, sizeof(message));
}

static void sync_shared_params(mdmi_patch *patch, int channel, bool force, patch_emit_fn emit, void *context)
{
	int16_t *shown = patch->shown.channels[channel];
	int16_t *rom = patch->rom.channels[channel];
	int preset = channel_index(PARAM_PRESET);
	if (shown[preset] != rom[preset]) {
		const uint8_t message[2] = {(uint8_t)(0xC0 | channel), (uint8_t)shown[preset]};
		emit(context, message, sizeof(message));
		rom[preset] = shown[preset];
		if (channel < PATCH_NUM_FM_CHANNELS) {
			// The ROM has just loaded this preset's values, so only tweaks on top of it still differ.
			load_preset(rom, shown[preset]);
		}
	}
	int volume = channel_index(PARAM_VOLUME), pan = channel_index(PARAM_PAN);
	if (force || shown[volume] != rom[volume]) {
		emit_cc(emit, context, channel, CC_VOLUME, shown[volume]);
		rom[volume] = shown[volume];
	}
	if (force || shown[pan] != rom[pan]) {
		emit_cc(emit, context, channel, CC_PAN, PAN_CC[shown[pan]]);
		rom[pan] = shown[pan];
	}
}

static void sync_tuning_params(mdmi_patch *patch, int channel, patch_emit_fn emit, void *context)
{
	int16_t *shown = patch->shown.channels[channel];
	int16_t *rom = patch->rom.channels[channel];
	int fine_tune = channel_index(PARAM_FINE_TUNE);
	if (shown[fine_tune] != rom[fine_tune]) {
		emit_cc(emit, context, channel, CC_FINE_TUNE, shown[fine_tune]);
		rom[fine_tune] = shown[fine_tune];
	}
	int time = channel_index(PARAM_PORTA_TIME);
	if (shown[time] != rom[time]) {
		emit_cc(emit, context, channel, CC_PORTA_TIME, shown[time]);
		rom[time] = shown[time];
	}
	int on = channel_index(PARAM_PORTA_ON);
	if (shown[on] != rom[on]) {
		emit_cc(emit, context, channel, CC_PORTA_ON, shown[on] ? CC_ON : 0);
		rom[on] = shown[on];
	}
	int bend = channel_index(PARAM_BEND_RANGE);
	if (shown[bend] != rom[bend]) {
		// RPN 0,0, then back to the null RPN so stray data entry messages change nothing.
		emit_cc(emit, context, channel, CC_RPN_MSB, 0);
		emit_cc(emit, context, channel, CC_RPN_LSB, 0);
		emit_cc(emit, context, channel, CC_DATA_ENTRY_MSB, shown[bend]);
		emit_cc(emit, context, channel, CC_DATA_ENTRY_LSB, 0);
		emit_cc(emit, context, channel, CC_RPN_MSB, RPN_NULL);
		emit_cc(emit, context, channel, CC_RPN_LSB, RPN_NULL);
		patch->rpn_msb[channel] = patch->rpn_lsb[channel] = RPN_NULL;
		rom[bend] = shown[bend];
	}
}

static void sync_fm_param(int16_t *shown, int16_t *rom, uint32_t id, int cc, int cc_range, bool force, int channel, patch_emit_fn emit, void *context)
{
	int index = channel_index(id);
	if (!force && shown[index] == rom[index]) {
		return;
	}
	int value = shown[index];
	int steps = 128 / cc_range;
	// A value only the preset itself can set, which a resend through CC would clip.
	if (value >= cc_range && shown[index] == rom[index]) {
		return;
	}
	if (value >= cc_range) {
		value = cc_range - 1;
	}
	emit_cc(emit, context, channel, cc, value * steps);
	rom[index] = shown[index];
}

static void sync_fm_channel(mdmi_patch *patch, int channel, bool force, patch_emit_fn emit, void *context)
{
	int16_t *shown = patch->shown.channels[channel];
	int16_t *rom = patch->rom.channels[channel];
	sync_fm_param(shown, rom, PARAM_ALGORITHM, CC_ALGORITHM, 8, force, channel, emit, context);
	sync_fm_param(shown, rom, PARAM_FEEDBACK, CC_FEEDBACK, 8, force, channel, emit, context);
	sync_fm_param(shown, rom, PARAM_AMS, CC_AMS, 4, force, channel, emit, context);
	sync_fm_param(shown, rom, PARAM_FMS, CC_FMS, 8, force, channel, emit, context);
	for (int op = 0; op < PATCH_OPS; op++) {
		for (int i = 0; i < PATCH_OP_PARAMS; i++) {
			sync_fm_param(shown, rom, PARAM_OP_BASE + op * PATCH_OP_PARAMS + i, OP_PARAMS[i].cc + op, OP_PARAMS[i].cc_range, force, channel, emit, context);
		}
	}
}

void patch_sync(mdmi_patch *patch, patch_emit_fn emit, void *context)
{
	int16_t *shown_globals = patch->shown.globals;
	int16_t *rom_globals = patch->rom.globals;
	int dac = global_index(PARAM_DAC);
	bool chip_reset = shown_globals[dac] != rom_globals[dac];
	if (chip_reset) {
		// The ROM swaps the Z80 driver for this, and the Z80's reset line also clears the YM2612.
		// The ROM doesn't know, so FM stays silent until its registers are written again.
		emit_cc(emit, context, 0, CC_DAC, shown_globals[dac] ? CC_ON : 0);
		rom_globals[dac] = shown_globals[dac];
		for (int channel = 0; channel < PATCH_NUM_FM_CHANNELS; channel++) {
			patch->reload_preset[channel] = true;
		}
	}
	if (shown_globals[PARAM_POLYPHONY] != rom_globals[PARAM_POLYPHONY]) {
		emit_cc(emit, context, 0, CC_POLYPHONY, shown_globals[PARAM_POLYPHONY] ? CC_ON : 0);
		rom_globals[PARAM_POLYPHONY] = shown_globals[PARAM_POLYPHONY];
		// Remapping leaves voices with whatever patch they had, so every FM value goes out again.
		for (int channel = 0; channel < PATCH_NUM_FM_CHANNELS; channel++) {
			patch->force_resend[channel] = true;
		}
	}
	if (chip_reset || shown_globals[PARAM_LFO_ON] != rom_globals[PARAM_LFO_ON]) {
		emit_cc(emit, context, 0, CC_LFO_ON, shown_globals[PARAM_LFO_ON] ? CC_ON : 0);
		rom_globals[PARAM_LFO_ON] = shown_globals[PARAM_LFO_ON];
	}
	if (chip_reset || shown_globals[PARAM_LFO_RATE] != rom_globals[PARAM_LFO_RATE]) {
		emit_cc(emit, context, 0, CC_LFO_RATE, shown_globals[PARAM_LFO_RATE] * (128 / 8));
		rom_globals[PARAM_LFO_RATE] = shown_globals[PARAM_LFO_RATE];
	}
	int ch3 = global_index(PARAM_CH3_MODE);
	if (chip_reset || shown_globals[ch3] != rom_globals[ch3]) {
		emit_cc(emit, context, 0, CC_CH3_MODE, shown_globals[ch3] ? CC_ON : 0);
		rom_globals[ch3] = shown_globals[ch3];
	}
	bool polyphony = shown_globals[PARAM_POLYPHONY] != 0;
	for (int channel = 0; channel < PATCH_NUM_CHANNELS; channel++) {
		bool is_fm = channel < PATCH_NUM_FM_CHANNELS;
		// While polyphony owns the FM voices, channels 2-6 have nothing mapped to receive their values.
		if (is_fm && polyphony && channel != 0) {
			continue;
		}
		if (is_fm && patch->reload_preset[channel]) {
			// The ROM skips a program it thinks is loaded, so go through another one. A preset also
			// restores values CCs can't reach; the sync below then re-applies any knob tweaks.
			patch->reload_preset[channel] = false;
			int16_t *rom = patch->rom.channels[channel];
			int program = rom[channel_index(PARAM_PRESET)];
			const uint8_t other[2] = {(uint8_t)(0xC0 | channel), (uint8_t)((program + 1) & 127)};
			const uint8_t same[2] = {(uint8_t)(0xC0 | channel), (uint8_t)program};
			emit(context, other, sizeof(other));
			emit(context, same, sizeof(same));
			load_preset(rom, program);
		}
		bool force = patch->force_resend[channel];
		patch->force_resend[channel] = false;
		sync_shared_params(patch, channel, force, emit, context);
		sync_tuning_params(patch, channel, emit, context);
		if (is_fm) {
			sync_fm_channel(patch, channel, force, emit, context);
		}
	}
}

// Knob value for an incoming CC, the inverse of what sync sends.
static void track_cc(int16_t *shown, int16_t *rom, int cc, int value)
{
	for (int op = 0; op < PATCH_OPS; op++) {
		for (int i = 0; i < PATCH_OP_PARAMS; i++) {
			if (OP_PARAMS[i].cc + op == cc) {
				int index = channel_index(PARAM_OP_BASE + op * PATCH_OP_PARAMS + i);
				shown[index] = rom[index] = (int16_t)(value / (128 / OP_PARAMS[i].cc_range));
				return;
			}
		}
	}
	int index, range;
	switch (cc) {
	case CC_ALGORITHM: index = channel_index(PARAM_ALGORITHM); range = 8; break;
	case CC_FEEDBACK: index = channel_index(PARAM_FEEDBACK); range = 8; break;
	case CC_AMS: index = channel_index(PARAM_AMS); range = 4; break;
	case CC_FMS: index = channel_index(PARAM_FMS); range = 8; break;
	default: return;
	}
	shown[index] = rom[index] = (int16_t)(value / (128 / range));
}

bool patch_track_midi(mdmi_patch *patch, const uint8_t *bytes, size_t length, bool *knobs_changed)
{
	*knobs_changed = false;
	if (length < 2) {
		return true;
	}
	int status = bytes[0] & 0xF0;
	int channel = bytes[0] & 0x0F;
	if (status == 0xC0) {
		if (channel < PATCH_NUM_CHANNELS) {
			int16_t *shown = patch->shown.channels[channel];
			int16_t *rom = patch->rom.channels[channel];
			shown[channel_index(PARAM_PRESET)] = rom[channel_index(PARAM_PRESET)] = bytes[1];
			if (channel < PATCH_NUM_FM_CHANNELS) {
				load_preset(shown, bytes[1]);
				load_preset(rom, bytes[1]);
			}
			*knobs_changed = true;
		}
		return true;
	}
	if (status != 0xB0 || length < 3) {
		return true;
	}
	int cc = bytes[1], value = bytes[2];
	if (cc == CC_POLYPHONY) {
		// Polyphony has to go through sync, which re-sends the FM values after the ROM remaps its voices.
		__atomic_store_n(&patch->shown.globals[PARAM_POLYPHONY], (int16_t)(value >= 64), __ATOMIC_RELAXED);
		*knobs_changed = true;
		return false;
	}
	if (cc == CC_LFO_ON) {
		patch->shown.globals[PARAM_LFO_ON] = patch->rom.globals[PARAM_LFO_ON] = (int16_t)(value >= 64);
		*knobs_changed = true;
		return true;
	}
	if (cc == CC_LFO_RATE) {
		patch->shown.globals[PARAM_LFO_RATE] = patch->rom.globals[PARAM_LFO_RATE] = (int16_t)(value / (128 / 8));
		*knobs_changed = true;
		return true;
	}
	if (cc == CC_DAC) {
		// Goes through sync, which restores FM after the chip reset this causes.
		__atomic_store_n(&patch->shown.globals[global_index(PARAM_DAC)], (int16_t)(value >= 64), __ATOMIC_RELAXED);
		*knobs_changed = true;
		return false;
	}
	if (cc == CC_CH3_MODE) {
		int index = global_index(PARAM_CH3_MODE);
		patch->shown.globals[index] = patch->rom.globals[index] = (int16_t)(value >= 64);
		*knobs_changed = true;
		return true;
	}
	if (cc == CC_RPN_MSB) {
		patch->rpn_msb[channel] = (uint8_t)value;
		return true;
	}
	if (cc == CC_RPN_LSB) {
		patch->rpn_lsb[channel] = (uint8_t)value;
		return true;
	}
	if (channel >= PATCH_NUM_CHANNELS) {
		return true;
	}
	int16_t *shown = patch->shown.channels[channel];
	int16_t *rom = patch->rom.channels[channel];
	if (cc == CC_DATA_ENTRY_MSB) {
		if (patch->rpn_msb[channel] != 0 || patch->rpn_lsb[channel] != 0) {
			return true;
		}
		shown[channel_index(PARAM_BEND_RANGE)] = rom[channel_index(PARAM_BEND_RANGE)] = (int16_t)(value > BEND_RANGE_MAX ? BEND_RANGE_MAX : value);
	} else if (cc == CC_DATA_ENTRY_LSB) {
		return true;
	} else if (cc == CC_FINE_TUNE) {
		shown[channel_index(PARAM_FINE_TUNE)] = rom[channel_index(PARAM_FINE_TUNE)] = (int16_t)value;
	} else if (cc == CC_PORTA_TIME) {
		shown[channel_index(PARAM_PORTA_TIME)] = rom[channel_index(PARAM_PORTA_TIME)] = (int16_t)value;
	} else if (cc == CC_PORTA_ON) {
		shown[channel_index(PARAM_PORTA_ON)] = rom[channel_index(PARAM_PORTA_ON)] = (int16_t)(value >= 64);
	} else if (cc == CC_VOLUME) {
		shown[channel_index(PARAM_VOLUME)] = rom[channel_index(PARAM_VOLUME)] = (int16_t)value;
	} else if (cc == CC_PAN) {
		shown[channel_index(PARAM_PAN)] = rom[channel_index(PARAM_PAN)] = (int16_t)(value > 95 ? 2 : value > 31 ? 1 : 0);
	} else if (channel < PATCH_NUM_FM_CHANNELS) {
		track_cc(shown, rom, cc, value);
	} else {
		return true;
	}
	*knobs_changed = true;
	return true;
}

size_t patch_state_size(void)
{
	return sizeof(uint32_t) + sizeof(patch_values);
}

void patch_save(const mdmi_patch *patch, void *buffer)
{
	const uint32_t version = STATE_VERSION;
	memcpy(buffer, &version, sizeof(version));
	memcpy((uint8_t *)buffer + sizeof(version), &patch->shown, sizeof(patch->shown));
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
	memcpy(&patch->shown, &incoming, sizeof(patch->shown));
	return true;
}
