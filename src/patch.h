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
#define PATCH_NUM_PSG_CHANNELS (PATCH_NUM_CHANNELS - PATCH_NUM_FM_CHANNELS)

// Settings of the whole console. Slot 0 held Polyphony until D20 replaced it: its id stays unused,
// so a value saved under it means nothing.
enum {
	GLOBAL_RETIRED,
	GLOBAL_FILTER,
	GLOBAL_LFO_ON,
	GLOBAL_LFO_RATE,
	GLOBAL_CH3_MODE,
	GLOBAL_DAC,
	// What an FM note right after another on the same voice does (D41): the plugin's, not the ROM's.
	GLOBAL_RETRIGGER,
	// The PSG noise's mode (D45): the chip has one noise register, so these are the console's.
	GLOBAL_NOISE_TYPE,
	GLOBAL_NOISE_RATE,
	// The plugin's output level in tenths of dB, 0 silence (stage 18a): the plugin's, never sent to the ROM.
	GLOBAL_MASTER,
	// Channel 3 in Ch3 Special Mode (stage 21, D51): each operator its own note (Ops) or one note
	// on MIDI 3 for all four (One Note), and the MIDI channel whose notes reset the shared LFO. The
	// console's, as the noise is: sent as CC 81 and 82, not part of any sound.
	GLOBAL_CH3_NOTES,
	GLOBAL_LFO_RESET,
	// Each of channel 3's operators: Semi, Fine, Fixed, operator 1 first (GLOBAL_CH3_OP).
	GLOBAL_CH3_OP_BASE,
	GLOBAL_CH3_OP_END = GLOBAL_CH3_OP_BASE + 4 * 3,
	// Stage 38 (D83): MONO of FM channels 1-6, CC 105 on the channel (GLOBAL_MONO). The console's,
	// as the operators' are: not part of a sound, the bank or files.
	GLOBAL_MONO_BASE = GLOBAL_CH3_OP_END,
	// Stage 44 (D88): the envelope's speed of PSG channels 7-10, CC 9 on the channel
	// (GLOBAL_ENV_SPEED). The console's, as MONO: not part of a sound; 64 is the stock x1.
	GLOBAL_ENV_SPEED_BASE = GLOBAL_MONO_BASE + 6,
	PATCH_NUM_GLOBALS = GLOBAL_ENV_SPEED_BASE + 4,
};
#define GLOBAL_MONO(channel) (GLOBAL_MONO_BASE + (channel))
// psg: 0-3, channels 7-10.
#define GLOBAL_ENV_SPEED(psg) (GLOBAL_ENV_SPEED_BASE + (psg))
enum { PATCH_CH3_SEMI, PATCH_CH3_FINE, PATCH_CH3_FIXED, PATCH_CH3_OP_FIELDS };
#define GLOBAL_CH3_OP(op, field) (GLOBAL_CH3_OP_BASE + (op) * PATCH_CH3_OP_FIELDS + (field))
// Stage 22 (D52): CSM, timer A keys all four operators at the rate of MIDI 3's note.
enum { PATCH_CH3_OPS, PATCH_CH3_ONE_NOTE, PATCH_CH3_CSM };
// Semi -48..+48 and Fine -63..+63 are stored from 0, their middle the default.
#define PATCH_CH3_SEMI_MAX 48
#define PATCH_CH3_FINE_MAX 63
// With Fixed an operator starts from C4, not from the key; the ROM keeps every operator's note in 11-106.
#define PATCH_CH3_FIXED_KEY 60
#define PATCH_CH3_LOWEST_KEY 11
#define PATCH_CH3_HIGHEST_KEY 106
#define PATCH_MASTER_MAX 720
#define PATCH_MASTER_UNITY 600
enum { PATCH_RETRIGGER_CHIP, PATCH_RETRIGGER_VOICE, PATCH_RETRIGGER_CUT };
enum { PATCH_NOISE_PERIODIC, PATCH_NOISE_WHITE };
// Noise Rate: three fixed rates, highest first, then channel 9's tone (channel 3 of the PSG), then a
// drum kit on that tone, repeated every octave, whose drums choose their own type (owner, D45).
enum { PATCH_NOISE_RATE_HIGH, PATCH_NOISE_RATE_MID, PATCH_NOISE_RATE_LOW, PATCH_NOISE_RATE_TONE3, PATCH_NOISE_RATE_KIT };

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

// An FM sound with its name: a place of the bank, or what a channel has (D23).
#define PATCH_NAME 24
#define PATCH_VOICE_PARAMS (PATCH_NUM_CHANNEL_PARAMS - PARAM_ALGORITHM)
typedef struct {
	char name[PATCH_NAME];
	// From Algorithm on: voice[0] is PARAM_ALGORITHM.
	int16_t voice[PATCH_VOICE_PARAMS];
} patch_instrument;

// A PSG envelope with its name: a place of the bank's second list, or what a PSG channel has (D28).
// The steps are EEF bytes as the ROM holds them up to the first EEF_END, and EEF_END after it, so
// two envelopes compare whole. 124 steps at most: a SysEx of 125 would leave no room for its F7 in
// the ROM's receive buffer.
#define PATCH_ENVELOPE_STEPS 124
#define PATCH_EEF_END 0xFF
#define PATCH_EEF_LOOP_START 0xFE
#define PATCH_EEF_LOOP_END 0xFD
typedef struct {
	char name[PATCH_NAME];
	uint8_t steps[PATCH_ENVELOPE_STEPS + 1];
} patch_envelope;

// An envelope as the editor draws it (stage 8): its frames without the loop marks, and the frame
// the loop starts at and the frame after its end, -1 when there is none. Frames are EEF bytes: pitch
// code in the high nibble, attenuation in the low one.
typedef struct {
	uint8_t frames[PATCH_ENVELOPE_STEPS];
	int count;
	int loop_start;
	int loop_end;
} patch_shape;

// An arp row (stage 20, D50): a shift in whole semitones per frame (60 Hz) from Note On, step 0 at
// once, while the key is held. The loop goes from its step to the last; without one the row plays
// once and holds its last step. 0 steps is no arp. It is part of a sound, as a voice or an envelope
// is: every channel, every base and every place of both lists has one. The ROM holds one per program
// of each type, which SysEx 0A 02 (FM) and 0A 03 (PSG) write. A loop switched off in the window
// leaves its shadow where it started, so switching it on puts it back (as D30); the shadow plays
// nothing and two rows that differ only in it sound the same. A row switched off (stage 22b, D55)
// plays and compares as an empty one and keeps its steps; an empty row is never off.
#define PATCH_ARP_STEPS 32
#define PATCH_ARP_SHIFT 24
#define PATCH_ARP_EMPTY ((patch_arp){.count = 0, .loop = -1})
typedef struct {
	uint8_t count;
	// The step the loop starts at, -1 for none.
	int8_t loop;
	// The row's third byte, saved as it is: bit 0 shadow_kept, only without a loop, says a loop was
	// switched off, shadow where it started; bit 1 the row switched off (0.22.3, older builds wrote
	// only bit 0 and read any value as a kept shadow). The other bits are 0.
	uint8_t shadow_kept : 1;
	uint8_t off : 1;
	uint8_t spare : 6;
	uint8_t shadow;
	// -24..+24; 0 past count, so rows compare whole.
	int8_t steps[PATCH_ARP_STEPS];
} patch_arp;

// One bank for the plugin. Place N is program number N, so a program change, the Preset lane and
// CC 3 choose from it: an FM sound on an FM channel, an envelope on a PSG channel. It starts as the
// ROM's GM presets and envelopes, every row empty.
#define PATCH_BANK_SIZE 128
typedef struct {
	patch_instrument places[PATCH_BANK_SIZE];
	patch_envelope envelopes[PATCH_BANK_SIZE];
	// The rows of the FM places, and of the PSG places.
	patch_arp place_arps[PATCH_BANK_SIZE];
	patch_arp envelope_arps[PATCH_BANK_SIZE];
} patch_bank;

typedef struct {
	int16_t globals[PATCH_NUM_GLOBALS];
	int16_t channels[PATCH_NUM_CHANNELS][PATCH_NUM_CHANNEL_PARAMS];
	// The name of what each FM channel has: its bank place, or the file dropped on it.
	char names[PATCH_NUM_FM_CHANNELS][PATCH_NAME];
	// The envelope each PSG channel plays, the same way.
	patch_envelope psg[PATCH_NUM_PSG_CHANNELS];
	// What each channel last got whole, which the window compares its sound with (D31): its bank
	// place, or the file dropped on it. The FM voice from Algorithm on; the name stays in names.
	int16_t base[PATCH_NUM_FM_CHANNELS][PATCH_VOICE_PARAMS];
	patch_envelope psg_base[PATCH_NUM_PSG_CHANNELS];
	// Each channel's arp row, and its base's (stage 20).
	patch_arp arps[PATCH_NUM_CHANNELS];
	patch_arp arp_bases[PATCH_NUM_CHANNELS];
} patch_values;

// What a receiver holds. A value we can't know is PATCH_UNKNOWN, and always differs from the model.
#define PATCH_UNKNOWN (-1)
#define PATCH_SERVICE_PROGRAMS 12
#define PATCH_PSG_SERVICE_PROGRAMS 8
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
	// The program number each PSG channel has, and what the next note of a PSG channel plays under a
	// service number. Two numbers per channel, taken in turn, keep its last two envelopes.
	int16_t psg_programs[PATCH_NUM_PSG_CHANNELS];
	uint8_t psg_stored[PATCH_PSG_SERVICE_PROGRAMS][PATCH_ENVELOPE_STEPS + 1];
	bool psg_alternate[PATCH_NUM_PSG_CHANNELS];
	// The row the ROM holds under each program number, FM then PSG (stage 20); count
	// PATCH_ARP_UNKNOWN when we can't know it.
	patch_arp arps[2][PATCH_BANK_SIZE];
} patch_receiver;
#define PATCH_ARP_UNKNOWN 0xFF

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

// What the main thread hands the writer in one piece: a loaded state, a bank, or the voices and
// envelopes of files dropped on channels.
enum { PATCH_INCOMING_VALUES = 1, PATCH_INCOMING_BANK = 2, PATCH_INCOMING_VOICE = 4, PATCH_INCOMING_BASE = 4 << PATCH_NUM_CHANNELS };
typedef struct {
	uint32_t parts;
	patch_values values;
	patch_bank bank;
	// PATCH_INCOMING_VOICE << channel says which of these came, PATCH_INCOMING_BASE << channel
	// whether the channel's base came with it.
	patch_instrument voices[PATCH_NUM_FM_CHANNELS];
	patch_envelope envelopes[PATCH_NUM_PSG_CHANNELS];
	int16_t bases[PATCH_NUM_FM_CHANNELS][PATCH_VOICE_PARAMS];
	patch_envelope psg_bases[PATCH_NUM_PSG_CHANNELS];
	// Rows for channels (stage 20): bit channel of arp_parts says a row came, bit
	// PATCH_INCOMING_ARP_BASE + channel that its base came with it.
	uint32_t arp_parts;
	// Channels whose program number sync chooses again, as after a preset step (STORE).
	uint32_t choose_again;
	patch_arp arps[PATCH_NUM_CHANNELS];
	patch_arp arp_bases[PATCH_NUM_CHANNELS];
} patch_incoming;
#define PATCH_INCOMING_ARP_BASE 16

#define PATCH_PENDING_NOTES 32
typedef struct {
	uint8_t channel;
	uint8_t key;
	uint8_t velocity;
} patch_note;

// Which MIDI channel 0-9 a chip voice 0-9 plays for from its next key-on, or at once (stage 17, D46),
// told with the context of the emit that sends the note.
typedef void (*patch_owner_fn)(void *context, int voice, int channel, bool at_once);

// Stage 24c (D59): the four voices of PCM4 as our ROM deals them (midi_dac.c), a copy for the window
// only. The writer keeps it by its clock; a voice ends by its sample's length and loop marks.
#define PATCH_DAC_VOICES 4
enum { PATCH_DAC_FREE, PATCH_DAC_HELD, PATCH_DAC_OUT };
typedef struct {
	uint8_t key;
	// The ROM's own state of the voice (VOICE_FREE ... VOICE_RELEASE), and whether its key is down.
	uint8_t rom_state;
	bool held;
	uint16_t order;
	uint64_t start;
	// When it falls silent; UINT64_MAX while a loop repeats.
	uint64_t end;
	// Its sample's entry of dac_notes, as it was at the start.
	uint64_t note;
} patch_dac_voice;

typedef struct {
	patch_values model;
	patch_receiver rom;
	// D59: each note's sample as the image holds it, packed by patch_set_dac_note; 0 is none. Written
	// by the main thread, read by the writer.
	uint64_t dac_notes[128];
	patch_dac_voice dac[PATCH_DAC_VOICES];
	uint16_t dac_starts;
	// For the window: key | state << 8 per voice, and the voice's start on the writer's clock.
	uint32_t dac_shown[PATCH_DAC_VOICES];
	uint64_t dac_shown_start[PATCH_DAC_VOICES];
	uint64_t dac_shown_note[PATCH_DAC_VOICES];
	// Told whose note each FM voice starts; NULL when nobody listens.
	patch_owner_fn owner;
	// The dealer of FM voices: one for all receivers, so all get the same layout.
	patch_voice voices[PATCH_NUM_FM_CHANNELS];
	// Notes held on channels that play only on their own voice: Porta On, Ch3 Special, DAC.
	uint8_t held[PATCH_NUM_FM_CHANNELS];
	// The voices that lend none, one bit each, as the last patch_sync saw them (stage 28): a change frees
	// or takes voices, and the notes cut for want of one try again.
	uint8_t never_lend_mask;
	bool revive_due;
	// Keys held on each FM channel that don't sound: cut off, or no voice was free.
	uint32_t muted[PATCH_NUM_FM_CHANNELS][4];
	uint8_t muted_velocity[PATCH_NUM_FM_CHANNELS][128];
	// How many of them, for the window. Written by the writer only.
	uint8_t muted_count[PATCH_NUM_FM_CHANNELS];
	// The channel playing on each voice, or -1, and its key, for the window.
	int8_t playing[PATCH_NUM_FM_CHANNELS];
	uint8_t playing_key[PATCH_NUM_FM_CHANNELS];
	uint8_t playing_velocity[PATCH_NUM_FM_CHANNELS];
	patch_note pending[PATCH_PENDING_NOTES];
	uint32_t pending_count;
	// The writer's clock in samples, for Retrigger (D41): when each voice's last note ended, and the
	// notes of a channel that wait for its voice to fall silent, until wait_until.
	uint64_t now;
	uint32_t rate;
	uint64_t released_at[PATCH_NUM_FM_CHANNELS];
	bool released[PATCH_NUM_FM_CHANNELS];
	uint64_t wait_until[PATCH_NUM_FM_CHANNELS];
	patch_note waiting[PATCH_NUM_FM_CHANNELS][PATCH_NUM_FM_CHANNELS];
	uint8_t waiting_count[PATCH_NUM_FM_CHANNELS];
	bool resync;
	// The RPN each MIDI channel last selected, to recognize a pitch bend range coming from a clip.
	uint8_t rpn_msb[16];
	uint8_t rpn_lsb[16];
	bool dirty;
	// Triple buffer: the writer fills snapshots[snapshot_back], the reader holds snapshots[snapshot_front].
	patch_values snapshots[3];
	uint32_t snapshot_back;
	uint32_t snapshot_front;
	uint32_t snapshot_shared;
	// The writer's bank, and the main thread's, which is the one files change.
	patch_bank bank;
	patch_bank shown_bank;
	patch_incoming incoming;
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
// The name a place of the bank has in the ROM's GM presets.
const char *patch_gm_name(int place);
// The place still holds what the ROM has under its number, voice and name (D26).
bool patch_place_is_gm(const patch_bank *bank, int place);
// The same for the place's envelope.
bool patch_envelope_is_rom(const patch_bank *bank, int place);
// The bank a fresh plugin has: the ROM's GM presets and envelopes.
void patch_bank_rom(patch_bank *bank);
// A voice (from PARAM_ALGORITHM on) in the layout of the ROM's presets, PRESETS_GM's (stage 24).
void patch_voice_fields(const int16_t *voice, uint8_t *fields);
// Makes an envelope from EEF bytes: false if they don't fit. A missing EEF_END is added.
bool patch_envelope_from_eef(patch_envelope *envelope, const uint8_t *eef, size_t size);
// The steps before EEF_END.
int patch_envelope_length(const patch_envelope *envelope);
// The first loop start and the first end after it are the loop; other marks are left out.
void patch_shape_of(const patch_envelope *envelope, patch_shape *shape);
// How many frames fit with the shape's loop marks.
int patch_shape_room(const patch_shape *shape);
// The frame the ROM plays, for the window only (D29): held is the frames from Note On to Note Off,
// or to now while the note is held, released the frames since Note Off or -1 while held. One step
// per frame from frame 0, round the loop while held; after Note Off on from where it was, through
// the loop's end into the release, from the intro too (stage 22b, D54). -1 when silent: past the
// end, or Note Off without a loop or with a loop that has no steps after it.
int patch_shape_position(const patch_shape *shape, long held, long released);
// An FM operator's envelope as the YM2612 steps it, for the window's graph only (stage 11): the
// attenuation in the chip's 10-bit steps (0 loud, 1023 silent, 0.094 dB each) with TL added, its
// lowest and highest in each of columns columns. Column c of n spans from 2 (c / n)^2 to
// 2 ((c + 1) / n)^2 seconds after Note On; Note Off comes at PATCH_FM_OFF_SECONDS. op holds the
// operator's PATCH_OP_PARAMS values in patch.h's order; keycode is the chip's for the note (16 for
// C4), which RS scales the rates by.
#define PATCH_FM_SECONDS 2.0
#define PATCH_FM_OFF_SECONDS 1.0
#define PATCH_FM_SILENT 1023
void patch_fm_envelope(const int16_t *op, int keycode, int columns, uint16_t *low, uint16_t *high);
// Only what the ROM reads as drawn (D28, stage 8): at least one frame, a frame inside the loop, an
// end at the last frame is no end, frames cut to the room. Keeps the envelope's name.
void patch_shape_to_envelope(patch_shape *shape, patch_envelope *envelope);
// The value a param has in a place of the bank, which a reset returns to.
int patch_preset_value(const patch_bank *bank, int channel, int param, int preset);
// The channel's sound differs from its base (the rail's *, D31): an FM channel's voice values, a
// PSG channel's envelope as drawn, and the arp row as it plays. Volume, pan and the rest of the
// channel don't count.
bool patch_sound_changed(const patch_values *values, const patch_envelope *envelope, int channel);
// The value a param has in the channel's base, or -1 when the base doesn't hold it: only an FM
// channel's voice, from Algorithm on (D31).
int patch_base_value(const patch_values *values, int channel, int param);
// Makes a row one the ROM takes (stage 20): count 0-32, shifts -24..+24, a loop inside the steps or
// none, a shadow only without a loop and inside the steps, zeros past count, off only with steps.
void patch_arp_clean(patch_arp *arp);
// The two rows play the same: count, loop and steps as they play; the shadow doesn't count, a row
// switched off is an empty one.
bool patch_arp_same(const patch_arp *a, const patch_arp *b);
// The row as it plays: an empty one when it is switched off.
const patch_arp *patch_arp_played(const patch_arp *arp);
// The channel's row does something now: not channel 3 in Ch3 Special with Ops, channel 6 with the DAC, or
// channel 10 at a noise rate other than Tone 3 (the window dims the row then).
bool patch_arp_works(const patch_values *values, int channel);
// Channel 3 in Ch3 Special with each operator on its own note (stage 21): MIDI 11-13 play operators 1-3.
bool patch_ch3_ops(const patch_values *values);
// Stage 22: channel 3 in Ch3 Special with CSM.
bool patch_ch3_csm(const patch_values *values);

void patch_init(mdmi_patch *patch);

// The main thread's view: the snapshot, or a loaded state not taken yet.
const patch_values *patch_view(mdmi_patch *patch);
// The main thread's view of a PSG channel's envelope: one handed over and not taken yet, so the
// window and undo build on what was drawn last.
const patch_envelope *patch_envelope_view(mdmi_patch *patch, int channel);
// The main thread's view of a channel's arp row, the same way: one handed over and not taken yet.
const patch_arp *patch_arp_view(mdmi_patch *patch, int channel);
// The main thread's bank.
const patch_bank *patch_bank_view(const mdmi_patch *patch);
int patch_get(mdmi_patch *patch, uint32_t id);
void patch_format_preset(const patch_bank *bank, int channel, int value, char *text, size_t size);
void patch_format(const patch_bank *bank, uint32_t id, int value, char *text, size_t size);
// What a typed label means, the inverse of patch_format. False if the text is no label or number.
bool patch_parse(const patch_bank *bank, uint32_t id, const char *text, int *value);
double patch_filter_hz(int value);
// Master Volume's factor: 0 is silence, PATCH_MASTER_UNITY is 1.
double patch_master_gain(int value);
// Saved values are pairs of id and value, so knobs can be added without breaking projects.
size_t patch_save_size(void);
// Of one view, so the values and the names of a saved state belong together.
void patch_save(const patch_values *values, void *buffer);
// Takes what it knows from a saved state and leaves it for the writer to take. Without names, a
// channel is named after its bank place.
// Without envelopes, a PSG channel plays its bank place. Without bases (PATCH_BASES_BYTES, D31), a
// channel named as its place has the place as base, any other its own sound.
#define PATCH_BASES_BYTES (PATCH_NUM_FM_CHANNELS * PATCH_VOICE_PARAMS * 2 + PATCH_NUM_PSG_CHANNELS * sizeof(patch_envelope))
// Without arps (PATCH_CHANNEL_ARPS_BYTES: the channels' rows, then their bases' rows), every row is
// empty (stage 20).
#define PATCH_CHANNEL_ARPS_BYTES (2 * PATCH_NUM_CHANNELS * sizeof(patch_arp))
bool patch_load(mdmi_patch *patch, const void *buffer, size_t size, const char (*names)[PATCH_NAME], const patch_envelope *psg, const void *bases, const void *arps);
// Main thread: the bases of all channels, PATCH_BASES_BYTES.
void patch_save_bases(const patch_values *values, void *buffer);
// Main thread: the channels' rows and their bases' rows, PATCH_CHANNEL_ARPS_BYTES.
void patch_save_arps(const patch_values *values, void *buffer);
// Main thread: the names of the FM channels, PATCH_NUM_FM_CHANNELS * PATCH_NAME bytes.
void patch_save_names(const patch_values *values, void *buffer);
// Main thread: a new bank, or one place of it, for the writer to take.
void patch_load_bank(mdmi_patch *patch, const patch_bank *bank);
// The place's row comes with it, empty when arp is NULL: files have none (D44).
void patch_load_place(mdmi_patch *patch, int place, const patch_instrument *instrument, const patch_arp *arp);
void patch_load_envelope_place(mdmi_patch *patch, int place, const patch_envelope *envelope, const patch_arp *arp);
// Main thread: a file's voice for an FM channel, as if its knobs moved; the Preset stays (D23).
// The base becomes base's voice, or stays when it is NULL (D31): a drawing keeps it.
void patch_load_voice(mdmi_patch *patch, int channel, const patch_instrument *instrument, const patch_instrument *base);
// The same for a PSG channel's envelope (D28).
void patch_load_envelope(mdmi_patch *patch, int channel, const patch_envelope *envelope, const patch_envelope *base);
// Main thread: what the channel's base is now, a handed one not taken yet included.
void patch_base_view(mdmi_patch *patch, int channel, patch_instrument *voice, patch_envelope *envelope);
// Main thread: a row for a channel, as a drawing or with a file (stage 20). The base's row becomes
// base, or stays when it is NULL: a drawing keeps it, as with an envelope.
void patch_load_arp(mdmi_patch *patch, int channel, const patch_arp *arp, const patch_arp *base);
// Main thread: the channel's program number is chosen again at the next sync, as after a preset
// step, so a channel that now plays its place as it is goes back to the place's number (STORE).
void patch_choose_program_again(mdmi_patch *patch, int channel);
// Main thread: the row of the channel's base now, a handed one not taken yet included.
void patch_arp_base_view(mdmi_patch *patch, int channel, patch_arp *base);

// The writer's. Sets a knob; true if other knobs now show different values.
bool patch_set(mdmi_patch *patch, uint32_t id, int value);
// Sends the receiver whatever differs from the model.
void patch_sync(mdmi_patch *patch, patch_receiver *receiver, patch_emit_fn emit, void *context);
// Stage 44 (D88): envelope steps a frame at an Env Speed value, 1.0 at the default 64.
double patch_env_speed(int value);
// A message from the host: updates the model and sends the ROM what it needs. True if knobs moved.
// Notes starting at the same moment wait for patch_flush, so their voices change in one batch (D21).
bool patch_midi(mdmi_patch *patch, const uint8_t *bytes, size_t length, patch_emit_fn emit, void *context);
// The moment is complete: sends the waiting notes.
void patch_flush(mdmi_patch *patch, patch_emit_fn emit, void *context);
// The writer's clock, before each moment: the sample it is at and the rate.
void patch_set_now(mdmi_patch *patch, uint64_t sample, uint32_t rate);
// Retrigger Cut (D41): the earliest sample a channel's waiting notes are due at, false if none wait.
bool patch_next_due(const mdmi_patch *patch, uint64_t *sample);
// Starts the waiting notes that are due by now.
void patch_due(mdmi_patch *patch, patch_emit_fn emit, void *context);
// How long an FM voice sounds after Note Off by its RR, in ms, as the dealer counts it (D41).
int patch_release_ms(int rr);
// Stops every note and gives every FM voice that plays back to its own channel; one Retrigger Free
// took off every channel stays off until a note takes it (D41).
void patch_all_notes_off(mdmi_patch *patch, patch_emit_fn emit, void *context);
// The ROM booted again (a new image, stage 12): notes are gone, and the receiver gets everything.
void patch_rom_booted(mdmi_patch *patch, patch_emit_fn emit, void *context);
// Why an FM channel plays only on its own voice (D20, stage 24b): channel 6 with the DAC, channel 3 in
// Ch3 Special Mode, Porta On. None of them borrows a voice.
enum { PATCH_ALONE_NO, PATCH_ALONE_PORTA, PATCH_ALONE_CH3, PATCH_ALONE_DAC, PATCH_ALONE_MONO };
int patch_alone_reason(const patch_values *values, int channel);
// Why an FM channel's voice is never lent to a chord (D69): the DAC's and Ch3 Special's, PATCH_ALONE_NO
// for every other. A Porta channel's voice is lent while the channel is silent.
int patch_lend_reason(const patch_values *values, int channel);
// Main thread (D59): a note's sample as the image holds it (rom_image_sample), its length and loop
// marks in blocks of 256 bytes and its record's flags; blocks 0 for none.
void patch_set_dac_note(mdmi_patch *patch, int note, uint32_t blocks, uint16_t loop_start, uint16_t loop_end, uint8_t flags);
// Main thread (D59): every note's sample from the image the core plays.
void patch_set_dac_image(mdmi_patch *patch, const uint8_t *image, size_t size);
// Any thread: a DAC voice's state (PATCH_DAC_*) and key.
int patch_dac_voice_state(const mdmi_patch *patch, int voice, int *key);
// Any thread (stage 24e): where a sounding DAC voice is in its sample at the writer's clock `now` and
// `rate`, as a share of the sample from its start, wrapped inside its loop; -1 for a free voice.
double patch_dac_voice_place(const mdmi_patch *patch, int voice, uint64_t now, uint32_t rate);
// Any thread: how many held notes of a channel don't sound.
int patch_muted_notes(const mdmi_patch *patch, int channel);
// Any thread: the channel playing on an FM voice, or -1, and the key it plays there.
int patch_voice_player(const mdmi_patch *patch, int voice);
int patch_voice_key(const mdmi_patch *patch, int voice);
int patch_voice_velocity(const mdmi_patch *patch, int voice);
// True if what the main thread handed over became the model. Sync afterwards.
bool patch_take_incoming(mdmi_patch *patch);
double patch_model_filter_hz(const mdmi_patch *patch);
// Makes the model visible to the main thread. Before telling the host to re-read the knobs.
void patch_publish(mdmi_patch *patch);

#endif
