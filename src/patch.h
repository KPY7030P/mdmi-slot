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
// MIDI channels 1-6 are FM, 7-9 PSG tone, 10 PSG noise.
#define PATCH_NUM_CHANNELS 10
#define PATCH_NUM_FM_CHANNELS 6

// Settings of the whole console. Slot 0 held Polyphony until D20 replaced it: its id stays unused,
// so a value saved under it means nothing.
enum {
	GLOBAL_RETIRED,
	GLOBAL_FILTER,
	GLOBAL_LFO_ON,
	GLOBAL_LFO_RATE,
	GLOBAL_CH3_MODE,
	GLOBAL_DAC,
	PATCH_NUM_GLOBALS,
};

// What a MIDI channel has. PSG channels have the first ones except Pan; the rest are the FM voice.
enum {
	PARAM_PRESET,
	PARAM_VOLUME,
	PARAM_PAN,
	PARAM_FINE_TUNE,
	PARAM_BEND_RANGE,
	PARAM_PORTA_ON,
	PARAM_PORTA_TIME,
	PARAM_ALGORITHM,
	PARAM_FEEDBACK,
	PARAM_AMS,
	PARAM_FMS,
	PARAM_OP_BASE,
	PATCH_NUM_CHANNEL_PARAMS = PARAM_OP_BASE + PATCH_OPS * PATCH_OP_PARAMS,
};

// Hosts store automation by id, so ids never change (D3). A global's id is its index, and a channel's
// knob is 1000 + channel number * 100 + param: 1411 is Op1 TL of channel 4. A new knob takes the next
// free number and patch_param_id_at() puts it in its place on the panel.
#define PATCH_CHANNEL_ID(channel, param) (1000u + ((uint32_t)(channel) + 1) * 100u + (uint32_t)(param))
// Everything before the FM voice, without Pan.
#define PATCH_NUM_PSG_PARAMS (PARAM_ALGORITHM - 1)
#define PATCH_NUM_IDS (PATCH_NUM_GLOBALS - 1 + PATCH_NUM_FM_CHANNELS * PATCH_NUM_CHANNEL_PARAMS + (PATCH_NUM_CHANNELS - PATCH_NUM_FM_CHANNELS) * PATCH_NUM_PSG_PARAMS)

typedef struct {
	char name[24];
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
#define PATCH_SERVICE_PROGRAMS 12
typedef struct {
	// Globals and what each MIDI channel has. For an FM channel the Preset is the preset its program
	// gives; the FM voice itself is in voices.
	patch_values values;
	// The program number each FM MIDI channel has in the ROM, a service number for a stored voice.
	int16_t programs[PATCH_NUM_FM_CHANNELS];
	// What each FM voice holds: its program number as Preset, and the voice's values. A voice keeps
	// them when it moves to another channel, until that channel's sync changes them.
	int16_t voices[PATCH_NUM_FM_CHANNELS][PATCH_NUM_CHANNEL_PARAMS];
	// What a program change to each service number loads.
	int16_t stored[PATCH_SERVICE_PROGRAMS][PATCH_NUM_CHANNEL_PARAMS];
	// The MIDI channel each FM voice answers to.
	int8_t voice_owner[PATCH_NUM_FM_CHANNELS];
	// Which of its two service programs each FM channel uses next.
	bool alternate[PATCH_NUM_FM_CHANNELS];
} patch_receiver;

// Who plays on an FM voice (D20). A voice is free when channel is -1, and then answers to its own
// channel. A pending voice is taken for a note that goes out when the moment is complete.
typedef struct {
	int8_t channel;
	uint8_t key;
	bool pending;
	// The channel whose sound the voice was last given.
	int8_t sound;
	uint8_t velocity;
} patch_voice;

#define PATCH_PENDING_NOTES 32
typedef struct {
	uint8_t channel;
	uint8_t key;
	uint8_t velocity;
} patch_note;

typedef struct {
	patch_values model;
	patch_receiver rom;
	// The dealer of FM voices: one for all receivers, so all get the same layout.
	patch_voice voices[PATCH_NUM_FM_CHANNELS];
	// Notes held on channels that play only on their own voice: Porta On, Ch3 Special, DAC.
	uint8_t held[PATCH_NUM_FM_CHANNELS];
	// Keys held on each FM channel that don't sound: cut off, or no voice was free.
	uint32_t muted[PATCH_NUM_FM_CHANNELS][4];
	// How many of them, for the window. Written by the writer only.
	uint8_t muted_count[PATCH_NUM_FM_CHANNELS];
	// The channel playing on each voice, or -1, for the window.
	int8_t playing[PATCH_NUM_FM_CHANNELS];
	patch_note pending[PATCH_PENDING_NOTES];
	uint32_t pending_count;
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
// Splits an id into a channel and a param. The channel is -1 for a global, and false means no such knob.
bool patch_id_split(uint32_t id, int *channel, int *param);
bool patch_channel_has(int channel, int param);
const patch_param_info *patch_param(uint32_t id);
void patch_param_name(uint32_t id, char *name, size_t size);
uint32_t patch_param_id_at(uint32_t panel_index);
// The value a param has in a preset, which a reset returns to.
int patch_preset_value(int channel, int param, int preset);

void patch_init(mdmi_patch *patch);

// The main thread's view: the snapshot, or a loaded state not taken yet.
const patch_values *patch_view(mdmi_patch *patch);
int patch_get(mdmi_patch *patch, uint32_t id);
void patch_format_preset(int channel, int value, char *text, size_t size);
void patch_format(uint32_t id, int value, char *text, size_t size);
double patch_filter_hz(int value);
// Saved values are pairs of id and value, so knobs can be added without breaking projects.
size_t patch_save_size(void);
void patch_save(mdmi_patch *patch, void *buffer);
// Takes what it knows from a saved state and leaves it for the writer to take.
bool patch_load(mdmi_patch *patch, const void *buffer, size_t size);

// The writer's. Sets a knob; true if other knobs now show different values.
bool patch_set(mdmi_patch *patch, uint32_t id, int value);
// Sends the receiver whatever differs from the model.
void patch_sync(mdmi_patch *patch, patch_receiver *receiver, patch_emit_fn emit, void *context);
// A message from the host: updates the model and sends the ROM what it needs. True if knobs moved.
// Notes starting at the same moment wait for patch_flush, so their voices change in one batch (D21).
bool patch_midi(mdmi_patch *patch, const uint8_t *bytes, size_t length, patch_emit_fn emit, void *context);
// The moment is complete: sends the waiting notes.
void patch_flush(mdmi_patch *patch, patch_emit_fn emit, void *context);
// Stops every note and gives every FM voice back to its own channel.
void patch_all_notes_off(mdmi_patch *patch, patch_emit_fn emit, void *context);
// Any thread: how many held notes of a channel don't sound.
int patch_muted_notes(const mdmi_patch *patch, int channel);
// Any thread: the channel playing on an FM voice, or -1.
int patch_voice_player(const mdmi_patch *patch, int voice);
// True if a loaded state became the model. Sync afterwards.
bool patch_take_incoming(mdmi_patch *patch);
double patch_model_filter_hz(const mdmi_patch *patch);
// Makes the model visible to the main thread. Before telling the host to re-read the knobs.
void patch_publish(mdmi_patch *patch);

#endif
