#ifndef MDMI_PATCH_H
#define MDMI_PATCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The synth's knobs. MDMI has no parameter memory the plugin can read back, so this keeps two
// copies of every value: what the knobs show, and what the ROM currently has. Syncing sends only
// the difference as MIDI, so a knob, a preset change or a restored project all take the same path.
#define PATCH_OPS 4
#define PATCH_OP_PARAMS 11

// Hosts store automation by id, so ids never change. Knobs added later get the next free id,
// and patch_param_id_at() puts them in their logical place on the panel.
enum {
	PARAM_EDIT_CHANNEL,
	PARAM_POLYPHONY,
	PARAM_FILTER,
	PARAM_LFO_ON,
	PARAM_LFO_RATE,
	PARAM_PRESET,
	PARAM_VOLUME,
	PARAM_PAN,
	PARAM_ALGORITHM,
	PARAM_FEEDBACK,
	PARAM_AMS,
	PARAM_FMS,
	PARAM_OP_BASE,
	PARAM_CH3_MODE = PARAM_OP_BASE + PATCH_OPS * PATCH_OP_PARAMS,
	PARAM_DAC,
	PARAM_FINE_TUNE,
	PARAM_BEND_RANGE,
	PARAM_PORTA_ON,
	PARAM_PORTA_TIME,
	PATCH_NUM_PARAMS,
};

#define PATCH_NUM_GLOBALS 7
#define PATCH_NUM_CHANNEL_PARAMS (PATCH_NUM_PARAMS - PATCH_NUM_GLOBALS)
// MIDI channels 1-6 are FM, 7-9 PSG tone, 10 PSG noise.
#define PATCH_NUM_CHANNELS 10
#define PATCH_NUM_FM_CHANNELS 6

typedef struct {
	char name[32];
	int16_t max;
	int16_t default_value;
	bool stepped;
} patch_param_info;

typedef struct {
	int16_t globals[PATCH_NUM_GLOBALS];
	int16_t channels[PATCH_NUM_CHANNELS][PATCH_NUM_CHANNEL_PARAMS];
} patch_values;

typedef struct {
	patch_values shown;
	patch_values rom;
	bool force_resend[PATCH_NUM_CHANNELS];
	bool reload_preset[PATCH_NUM_FM_CHANNELS];
	// The RPN each MIDI channel last selected, to recognize a pitch bend range coming from a clip.
	uint8_t rpn_msb[16];
	uint8_t rpn_lsb[16];
} mdmi_patch;

typedef void (*patch_emit_fn)(void *context, const uint8_t *bytes, size_t length);

void patch_global_init(void);
const patch_param_info *patch_param(uint32_t id);
uint32_t patch_param_id_at(uint32_t panel_index);

void patch_init(mdmi_patch *patch);

// Safe from the main thread while the audio thread runs.
int patch_get(const mdmi_patch *patch, uint32_t id);
void patch_format(const mdmi_patch *patch, uint32_t id, int value, char *text, size_t size);
double patch_filter_hz(int value);

// Sets a knob. Returns true if other knobs now show different values.
bool patch_set(mdmi_patch *patch, uint32_t id, int value);

// Sends the ROM whatever differs from the knobs.
void patch_sync(mdmi_patch *patch, patch_emit_fn emit, void *context);

// Tracks MIDI arriving from the host, so knobs follow it. Returns false if the message must not
// reach the ROM as it is, because a later sync sends it instead. *knobs_changed is set when
// the knobs now show different values.
bool patch_track_midi(mdmi_patch *patch, const uint8_t *bytes, size_t length, bool *knobs_changed);

// Serialized form: a version followed by the shown values.
size_t patch_state_size(void);
void patch_save(const mdmi_patch *patch, void *buffer);
bool patch_load(mdmi_patch *patch, const void *buffer, size_t size);

#endif
