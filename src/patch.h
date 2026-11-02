#ifndef MDMI_PATCH_H
#define MDMI_PATCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The synth's knobs. MDMI has no parameter memory the plugin can read back, so this keeps the model,
// which is the truth, and a copy of what the receiver holds. Syncing sends only the difference, so a
// knob, a clip, a preset change or a restored project all take the same path.
//
// One writer: whoever is processing audio, or the main thread while nothing is. Other threads only
// read the published snapshot, and a loaded state waits in its own slot until the writer takes it.
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

// What a receiver holds. A value we can't know is PATCH_UNKNOWN, and always differs from the model.
#define PATCH_UNKNOWN (-1)
typedef struct {
	patch_values values;
	// Which of its two service programs each FM channel uses next.
	bool alternate[PATCH_NUM_FM_CHANNELS];
} patch_receiver;

typedef struct {
	patch_values model;
	patch_receiver rom;
	// The RPN each MIDI channel last selected, to recognize a pitch bend range coming from a clip.
	uint8_t rpn_msb[16];
	uint8_t rpn_lsb[16];
	bool dirty;
	// Triple buffer: the writer fills snapshots[snapshot_back], the reader holds snapshots[snapshot_front].
	patch_values snapshots[3];
	uint32_t snapshot_back;
	uint32_t snapshot_front;
	uint32_t snapshot_shared;
	patch_values incoming;
	uint32_t incoming_state;
} mdmi_patch;

typedef void (*patch_emit_fn)(void *context, const uint8_t *bytes, size_t length);

void patch_global_init(void);
const patch_param_info *patch_param(uint32_t id);
uint32_t patch_param_id_at(uint32_t panel_index);

void patch_init(mdmi_patch *patch);

// The main thread's view: the snapshot, or a loaded state not taken yet.
int patch_get(mdmi_patch *patch, uint32_t id);
int patch_get_channel(mdmi_patch *patch, int channel, uint32_t id);
void patch_format_preset(int channel, int value, char *text, size_t size);
void patch_format(mdmi_patch *patch, uint32_t id, int value, char *text, size_t size);
double patch_filter_hz(int value);
size_t patch_state_size(void);
void patch_save(mdmi_patch *patch, void *buffer);
// Checks a saved state and leaves it for the writer to take.
bool patch_load(mdmi_patch *patch, const void *buffer, size_t size);

// The writer's. Sets a knob; true if other knobs now show different values.
bool patch_set(mdmi_patch *patch, uint32_t id, int value);
// Sends the receiver whatever differs from the model.
void patch_sync(mdmi_patch *patch, patch_receiver *receiver, patch_emit_fn emit, void *context);
// A message from the host: updates the model and sends the ROM what it needs. True if knobs moved.
bool patch_midi(mdmi_patch *patch, const uint8_t *bytes, size_t length, patch_emit_fn emit, void *context);
// True if a loaded state became the model. Sync afterwards.
bool patch_take_incoming(mdmi_patch *patch);
double patch_model_filter_hz(const mdmi_patch *patch);
// Makes the model visible to the main thread. Before telling the host to re-read the knobs.
void patch_publish(mdmi_patch *patch);

#endif
