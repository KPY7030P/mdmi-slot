#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <clap/clap.h>
#include <windows.h>
#include "dac.h"
#include "engine.h"
#include "gui.h"
#include "patch.h"
#include "state.h"

#define PLUGIN_ID "dev.mdmi-plugin.mega-drive"
#define PLUGIN_NAME "MDMI Mega Drive"
#define EDIT_QUEUE 256

static const char *const plugin_features[] = {
	CLAP_PLUGIN_FEATURE_INSTRUMENT,
	CLAP_PLUGIN_FEATURE_SYNTHESIZER,
	CLAP_PLUGIN_FEATURE_STEREO,
	NULL,
};

static const clap_plugin_descriptor_t plugin_descriptor = {
	.clap_version = CLAP_VERSION_INIT,
	.id = PLUGIN_ID,
	.name = PLUGIN_NAME,
	.vendor = "MDMI Plugin",
	.url = "",
	.manual_url = "",
	.support_url = "",
	.version = MDMI_VERSION,
	.description = "Mega Drive MIDI Interface ROM running in the BlastEm emulator, build " MDMI_VERSION,
	.features = plugin_features,
};

// A change from the window, waiting for the writer (D4).
typedef struct {
	uint8_t kind;
	int16_t value;
	uint32_t id;
} edit_command;

typedef struct {
	clap_plugin_t plugin;
	const clap_host_t *host;
	mdmi_engine *engine;
	const clap_host_params_t *host_params;
	const clap_host_state_t *host_state;
	mdmi_patch patch;
	uint32_t sync_pending;
	// After activation the ROM's notes stopped, and lent voices go home.
	uint32_t notes_off_pending;
	// The engine's boots the writer has seen: a new one is a restart with another image (stage 12).
	uint32_t boots_seen;
	uint32_t dirty_pending;
	bool knobs_moved;
	bool active;
	mdmi_gui *gui;
	// The window's page and channel and the loops switched off, saved with the project.
	state_view view;
	// The project's DAC samples (D36), and the version of them the core was last given.
	dac_set dac;
	uint32_t dac_applied;
	mdmi_files files;
	// Single producer, the main thread; single consumer, the writer.
	edit_command edits[EDIT_QUEUE];
	uint32_t edits_written;
	uint32_t edits_read;
	uint32_t notes[GUI_NUM_MIDI_CHANNELS];
	uint8_t held[GUI_NUM_MIDI_CHANNELS];
	// Written by the writer only.
	gui_psg_times psg_times;
	// Set by the audio thread, cleared by the window (D33).
	uint32_t host_values[GUI_HOST_SLOTS];
	// When the window last sent each knob something, in ms (D34): what the host sends back within
	// HOST_ECHO_MS is Live's echo of it, not a lane.
	uint32_t window_ms[GUI_HOST_SLOTS];
} mdmi_plugin;

typedef struct {
	mdmi_plugin *self;
	uint32_t offset;
} emit_target;

// What the ROM's PSG channels get decides where their envelopes are: a Note On starts one, a Note
// Off of the key it plays or All Notes Off releases it (midi.c midi_note_off). Legato, a held note
// coming back and portamento between held keys aren't followed (parking).
static void time_psg_notes(mdmi_plugin *self, uint64_t when, const uint8_t *bytes, size_t length)
{
	const int status = bytes[0] & 0xF0, psg = (bytes[0] & 0x0F) - PATCH_NUM_FM_CHANNELS;
	if (length < 3 || psg < 0 || psg >= PATCH_NUM_PSG_CHANNELS) {
		return;
	}
	gui_psg_times *times = &self->psg_times;
	if (status == 0x90 && bytes[2] > 0) {
		times->key[psg] = bytes[1];
		__atomic_store_n(&times->off[psg], GUI_NEVER, __ATOMIC_RELEASE);
		__atomic_store_n(&times->on[psg], when, __ATOMIC_RELEASE);
	} else if ((((status == 0x80 || status == 0x90) && bytes[1] == times->key[psg]) || (status == 0xB0 && bytes[1] == 123))
		&& __atomic_load_n(&times->off[psg], __ATOMIC_ACQUIRE) == GUI_NEVER) {
		__atomic_store_n(&times->off[psg], when, __ATOMIC_RELEASE);
	}
}

static void emit_to_engine(void *context, const uint8_t *bytes, size_t length)
{
	emit_target *target = context;
	time_psg_notes(target->self, target->self->psg_times.clock + target->offset, bytes, length);
	mdmi_engine_midi(target->self->engine, target->offset, bytes, length);
}

static void apply_filter(mdmi_plugin *self)
{
	mdmi_engine_set_filter(self->engine, patch_model_filter_hz(&self->patch));
}

// Knobs the host didn't move itself, by a preset or MIDI, are not reported: Live takes any value
// from the plugin as a manual change and turns automation off (D19). The window shows them, and
// the project only has to know it changed.
static void request_mark_dirty(mdmi_plugin *self)
{
	__atomic_store_n(&self->dirty_pending, 1, __ATOMIC_RELEASE);
	self->host->request_callback(self->host);
}

static void release_notes(mdmi_plugin *self, int channel);
static void after_load(mdmi_plugin *self, bool knobs);

// A file the window opened went to the model. The project changed, and only a whole state is
// told to the host as moved knobs (D19).
static void files_handed(void *context, bool knobs)
{
	mdmi_plugin *self = context;
	after_load(self, knobs);
	request_mark_dirty(self);
}

static mdmi_plugin *self_of(const clap_plugin_t *plugin)
{
	return plugin->plugin_data;
}

static bool plugin_init(const clap_plugin_t *plugin)
{
	mdmi_plugin *self = self_of(plugin);
	self->engine = mdmi_engine_create();
	if (!self->engine) {
		return false;
	}
	self->host_params = self->host->get_extension(self->host, CLAP_EXT_PARAMS);
	self->host_state = self->host->get_extension(self->host, CLAP_EXT_STATE);
	patch_init(&self->patch);
	self->view.dac = &self->dac;
	const files_host files = {&self->patch, &self->view, files_handed, self};
	files_init(&self->files, &files);
	apply_filter(self);
	return true;
}

static void plugin_destroy(const clap_plugin_t *plugin)
{
	mdmi_plugin *self = self_of(plugin);
	if (getenv("MDMI_PLUGIN_STATS")) {
		mdmi_engine_stats stats = mdmi_engine_get_stats(self->engine);
		fprintf(stderr, "stats underruns=%u dropped_midi=%u failed=%d booted=%d boot_stage=%s boot_error=%u worst_wait=%.2fms worst_run=%.2fms@%llu underruns_at=%llu..%llu\n", stats.underruns, stats.dropped_midi, stats.failed, stats.booted, stats.boot_stage ? stats.boot_stage : "none", stats.boot_error,
			stats.worst_wait_ms, stats.worst_run_ms, (unsigned long long)stats.worst_run_pos, (unsigned long long)stats.first_underrun_pos, (unsigned long long)stats.last_underrun_pos);
	}
	gui_destroy(self->gui);
	files_free(&self->files);
	mdmi_engine_destroy(self->engine);
	dac_clear(&self->dac);
	free(self);
}

static bool plugin_activate(const clap_plugin_t *plugin, double sample_rate, uint32_t min_frames, uint32_t max_frames)
{
	(void)min_frames;
	mdmi_plugin *self = self_of(plugin);
	self->active = true;
	__atomic_store_n(&self->sync_pending, 1, __ATOMIC_RELEASE);
	__atomic_store_n(&self->notes_off_pending, 1, __ATOMIC_RELEASE);
	for (int channel = 0; channel < GUI_NUM_MIDI_CHANNELS; channel++) {
		release_notes(self, channel);
	}
	return mdmi_engine_activate(self->engine, sample_rate, max_frames);
}

static void plugin_deactivate(const clap_plugin_t *plugin)
{
	self_of(plugin)->active = false;
	mdmi_engine_deactivate(self_of(plugin)->engine);
}

static bool plugin_start_processing(const clap_plugin_t *plugin)
{
	(void)plugin;
	return true;
}

static void plugin_stop_processing(const clap_plugin_t *plugin)
{
	(void)plugin;
}

static void plugin_reset(const clap_plugin_t *plugin)
{
	mdmi_plugin *self = self_of(plugin);
	emit_target target = {self, 0};
	patch_all_notes_off(&self->patch, emit_to_engine, &target);
	for (int channel = 0; channel < GUI_NUM_MIDI_CHANNELS; channel++) {
		release_notes(self, channel);
	}
}

// What the window shows as playing. Only the audio thread writes these while processing.
static void track_note(mdmi_plugin *self, int channel, int key, int velocity, bool on)
{
	uint32_t note = __atomic_load_n(&self->notes[channel], __ATOMIC_RELAXED);
	if (on) {
		if (self->held[channel] < 255) {
			self->held[channel]++;
		}
	} else {
		if (self->held[channel] > 0) {
			self->held[channel]--;
		}
		key = GUI_NOTE_SEEN(note) ? (int)GUI_NOTE_KEY(note) : key;
		velocity = (int)GUI_NOTE_VELOCITY(note);
	}
	__atomic_store_n(&self->notes[channel], GUI_NOTE_PACK(key, velocity, self->held[channel]), __ATOMIC_RELEASE);
}

static void release_notes(mdmi_plugin *self, int channel)
{
	uint32_t note = __atomic_load_n(&self->notes[channel], __ATOMIC_RELAXED);
	self->held[channel] = 0;
	if (GUI_NOTE_SEEN(note)) {
		__atomic_store_n(&self->notes[channel], GUI_NOTE_PACK(GUI_NOTE_KEY(note), GUI_NOTE_VELOCITY(note), 0), __ATOMIC_RELEASE);
	}
}

static void track_midi_notes(mdmi_plugin *self, const uint8_t *bytes)
{
	const int channel = bytes[0] & 0x0F;
	switch (bytes[0] & 0xF0) {
	case 0x90:
		track_note(self, channel, bytes[1] & 0x7F, bytes[2] & 0x7F, bytes[2] != 0);
		break;
	case 0x80:
		track_note(self, channel, bytes[1] & 0x7F, 0, false);
		break;
	case 0xB0:
		if (bytes[1] == 120 || bytes[1] == 123) {
			release_notes(self, channel);
		}
		break;
	default:
		if (bytes[0] == 0xFF) {
			for (int ch = 0; ch < GUI_NUM_MIDI_CHANNELS; ch++) {
				release_notes(self, ch);
			}
		}
		break;
	}
}

static void handle_midi(mdmi_plugin *self, uint32_t time, const uint8_t *bytes, size_t length)
{
	emit_target target = {self, time};
	if (patch_midi(&self->patch, bytes, length, emit_to_engine, &target)) {
		self->knobs_moved = true;
	}
}

static void set_param(mdmi_plugin *self, uint32_t id, int value, uint32_t time)
{
	if (patch_set(&self->patch, id, value)) {
		self->knobs_moved = true;
	}
	if (id == GLOBAL_FILTER) {
		apply_filter(self);
	}
	emit_target target = {self, time};
	patch_sync(&self->patch, &self->patch.rom, emit_to_engine, &target);
}

// A value from the host, whatever it is: Live sends them only while a lane plays, and doesn't send
// back what the window told it (stage 10 experiment, D32). The knob stays marked until the window
// edits it (D33).
// Live echoes the window's edit right after its gesture ends, sometimes the old value and then the
// new (journal after 0.11.0, D34). A lane can't come then: after a mouse edit Live stays quiet (D32).
#define HOST_ECHO_MS 200
static void count_host_value(mdmi_plugin *self, uint32_t id)
{
	int channel, param;
	if (patch_id_split(id, &channel, &param)) {
		const int slot = GUI_HOST_SLOT(channel, param);
		if ((uint32_t)GetTickCount() - __atomic_load_n(&self->window_ms[slot], __ATOMIC_RELAXED) < HOST_ECHO_MS) {
			return;
		}
		__atomic_store_n(&self->host_values[slot], 1, __ATOMIC_RELAXED);
	}
}

static void handle_param(mdmi_plugin *self, const clap_event_param_value_t *event)
{
	count_host_value(self, event->param_id);
	set_param(self, event->param_id, (int)(event->value + 0.5), event->header.time);
}

// Main thread. The window's changes go to the writer, which tells the host as if a knob moved, so
// Configure and automation recording see them.
static void gui_edit(void *context, int kind, uint32_t id, int value)
{
	mdmi_plugin *self = context;
	uint32_t written = self->edits_written;
	if (written - __atomic_load_n(&self->edits_read, __ATOMIC_ACQUIRE) >= EDIT_QUEUE) {
		return;
	}
	edit_command *command = &self->edits[written % EDIT_QUEUE];
	command->kind = (uint8_t)kind;
	command->id = id;
	command->value = (int16_t)value;
	__atomic_store_n(&self->edits_written, written + 1, __ATOMIC_RELEASE);
	if (self->host_params) {
		self->host_params->request_flush(self->host);
	}
}

static void push_gesture(const clap_output_events_t *out, uint16_t type, uint32_t id)
{
	clap_event_param_gesture_t event = {{sizeof(event), 0, CLAP_CORE_EVENT_SPACE_ID, type, 0}, id};
	out->try_push(out, &event.header);
}

// The writer's: applies what the window asked for and reports it to the host. Outside processing
// nothing can reach the ROM, so values wait in the model for the next process call.
static void take_edits(mdmi_plugin *self, const clap_output_events_t *out, bool processing)
{
	uint32_t read = self->edits_read;
	uint32_t written = __atomic_load_n(&self->edits_written, __ATOMIC_ACQUIRE);
	for (; read != written; read++) {
		const edit_command *command = &self->edits[read % EDIT_QUEUE];
		int channel, param;
		if (patch_id_split(command->id, &channel, &param)) {
			__atomic_store_n(&self->window_ms[GUI_HOST_SLOT(channel, param)], (uint32_t)GetTickCount(), __ATOMIC_RELAXED);
		}
		switch (command->kind) {
		case GUI_EDIT_BEGIN:
			push_gesture(out, CLAP_EVENT_PARAM_GESTURE_BEGIN, command->id);
			break;
		case GUI_EDIT_END:
			push_gesture(out, CLAP_EVENT_PARAM_GESTURE_END, command->id);
			break;
		default: {
			if (processing) {
				set_param(self, command->id, command->value, 0);
			} else if (patch_set(&self->patch, command->id, command->value)) {
				request_mark_dirty(self);
			}
			clap_event_param_value_t event = {{sizeof(event), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0}, command->id, NULL, -1, -1, -1, -1, command->value};
			out->try_push(out, &event.header);
			break;
		}
		}
	}
	__atomic_store_n(&self->edits_read, read, __ATOMIC_RELEASE);
}

static void handle_event(mdmi_plugin *self, const clap_event_header_t *header)
{
	if (header->space_id != CLAP_CORE_EVENT_SPACE_ID) {
		return;
	}
	switch (header->type) {
	case CLAP_EVENT_NOTE_ON:
	case CLAP_EVENT_NOTE_OFF: {
		const clap_event_note_t *note = (const clap_event_note_t *)header;
		if (note->key < 0 || note->key > 127 || note->channel < 0 || note->channel > 15) {
			break;
		}
		int velocity = (int)(note->velocity * 127.0 + 0.5);
		velocity = velocity < 1 ? 1 : velocity > 127 ? 127 : velocity;
		uint8_t message[3] = {
			(uint8_t)((header->type == CLAP_EVENT_NOTE_ON ? 0x90 : 0x80) | note->channel),
			(uint8_t)note->key,
			(uint8_t)velocity,
		};
		handle_midi(self, header->time, message, sizeof(message));
		track_note(self, note->channel, note->key, velocity, header->type == CLAP_EVENT_NOTE_ON);
		break;
	}
	case CLAP_EVENT_MIDI: {
		const clap_event_midi_t *midi = (const clap_event_midi_t *)header;
		uint8_t status = midi->data[0];
		size_t length = (status & 0xF0) == 0xC0 || (status & 0xF0) == 0xD0 ? 2 : 3;
		if (status >= 0xF0) {
			length = status == 0xF2 ? 3 : status == 0xF1 || status == 0xF3 ? 2 : 1;
		}
		track_midi_notes(self, midi->data);
		handle_midi(self, header->time, midi->data, length);
		break;
	}
	case CLAP_EVENT_MIDI_SYSEX: {
		const clap_event_midi_sysex_t *sysex = (const clap_event_midi_sysex_t *)header;
		emit_target target = {self, header->time};
		if (sysex->size > 0) {
			patch_midi(&self->patch, sysex->buffer, sysex->size, emit_to_engine, &target);
		}
		break;
	}
	case CLAP_EVENT_PARAM_VALUE:
		handle_param(self, (const clap_event_param_value_t *)header);
		break;
	default:
		break;
	}
}

static bool is_note(const clap_event_header_t *header)
{
	if (header->space_id != CLAP_CORE_EVENT_SPACE_ID) {
		return false;
	}
	if (header->type == CLAP_EVENT_NOTE_ON || header->type == CLAP_EVENT_NOTE_OFF) {
		return true;
	}
	if (header->type != CLAP_EVENT_MIDI) {
		return false;
	}
	const uint8_t status = ((const clap_event_midi_t *)header)->data[0] & 0xF0;
	return status == 0x90 || status == 0x80;
}

static clap_process_status plugin_process(const clap_plugin_t *plugin, const clap_process_t *process)
{
	mdmi_plugin *self = self_of(plugin);
	mdmi_engine *engine = self->engine;
	mdmi_engine_set_playing(engine, process->transport && (process->transport->flags & CLAP_TRANSPORT_IS_PLAYING));
	bool state_taken = patch_take_incoming(&self->patch);
	const uint32_t boots = mdmi_engine_boots(engine);
	if (boots != self->boots_seen) {
		// The first boot holds what patch_init expects; a later one is a new ROM that knows nothing.
		if (self->boots_seen) {
			emit_target target = {self, 0};
			patch_rom_booted(&self->patch, emit_to_engine, &target);
			for (int channel = 0; channel < GUI_NUM_MIDI_CHANNELS; channel++) {
				release_notes(self, channel);
			}
		}
		self->boots_seen = boots;
	}
	if (__atomic_exchange_n(&self->sync_pending, 0, __ATOMIC_ACQ_REL) || state_taken) {
		apply_filter(self);
		emit_target target = {self, 0};
		if (__atomic_exchange_n(&self->notes_off_pending, 0, __ATOMIC_ACQ_REL)) {
			patch_all_notes_off(&self->patch, emit_to_engine, &target);
		}
		patch_sync(&self->patch, &self->patch.rom, emit_to_engine, &target);
	}
	take_edits(self, process->out_events, true);
	// The VST3 wrapper puts notes before knobs and controllers of the same moment. A preset point
	// exactly on a note's start is meant for that note, so at each moment notes come last, in their
	// own order: a note that ends where it starts must not end first. Then the moment's notes go
	// out together, with their voices changed in one batch (D21).
	const clap_input_events_t *in = process->in_events;
	const uint32_t event_count = in->size(in);
	for (uint32_t first = 0; first < event_count;) {
		uint32_t end = first + 1;
		const uint32_t time = in->get(in, first)->time;
		while (end < event_count && in->get(in, end)->time == time) {
			end++;
		}
		for (int pass = 0; pass < 2; pass++) {
			for (uint32_t i = first; i < end; i++) {
				const clap_event_header_t *header = in->get(in, i);
				if (is_note(header) == (pass == 1)) {
					handle_event(self, header);
				}
			}
		}
		emit_target target = {self, time};
		patch_flush(&self->patch, emit_to_engine, &target);
		first = end;
	}
	patch_publish(&self->patch);
	if (self->knobs_moved) {
		self->knobs_moved = false;
		request_mark_dirty(self);
	}
	// A host may switch the output off. The emulator keeps its time anyway.
	const clap_audio_buffer_t *out = process->audio_outputs_count ? &process->audio_outputs[0] : NULL;
	bool has_output = out && out->data32 && out->channel_count > 0 && out->data32[0];
	float *left = has_output ? out->data32[0] : NULL;
	float *right = has_output && out->channel_count > 1 ? out->data32[1] : NULL;
	float scratch_left[4096], scratch_right[4096];
	uint32_t done = 0;
	while (done < process->frames_count) {
		uint32_t frames = process->frames_count - done;
		if ((!left || !right) && frames > 4096) {
			frames = 4096;
		}
		mdmi_engine_render(engine, left ? left + done : scratch_left, right ? right + done : scratch_right, frames);
		done += frames;
	}
	__atomic_store_n(&self->psg_times.clock, self->psg_times.clock + process->frames_count, __ATOMIC_RELEASE);
	return CLAP_PROCESS_CONTINUE;
}

static uint32_t audio_ports_count(const clap_plugin_t *plugin, bool is_input)
{
	(void)plugin;
	return is_input ? 0 : 1;
}

static bool audio_ports_get(const clap_plugin_t *plugin, uint32_t index, bool is_input, clap_audio_port_info_t *info)
{
	(void)plugin;
	if (is_input || index != 0) {
		return false;
	}
	memset(info, 0, sizeof(*info));
	info->id = 0;
	strncpy(info->name, "Main", sizeof(info->name) - 1);
	info->flags = CLAP_AUDIO_PORT_IS_MAIN;
	info->channel_count = 2;
	info->port_type = CLAP_PORT_STEREO;
	info->in_place_pair = CLAP_INVALID_ID;
	return true;
}

static const clap_plugin_audio_ports_t audio_ports_ext = {
	.count = audio_ports_count,
	.get = audio_ports_get,
};

static uint32_t note_ports_count(const clap_plugin_t *plugin, bool is_input)
{
	(void)plugin;
	return is_input ? 1 : 0;
}

static bool note_ports_get(const clap_plugin_t *plugin, uint32_t index, bool is_input, clap_note_port_info_t *info)
{
	(void)plugin;
	if (!is_input || index != 0) {
		return false;
	}
	memset(info, 0, sizeof(*info));
	info->id = 0;
	info->supported_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI;
	info->preferred_dialect = CLAP_NOTE_DIALECT_MIDI;
	strncpy(info->name, "MIDI In", sizeof(info->name) - 1);
	return true;
}

static const clap_plugin_note_ports_t note_ports_ext = {
	.count = note_ports_count,
	.get = note_ports_get,
};

static uint32_t latency_get(const clap_plugin_t *plugin)
{
	return mdmi_engine_latency(self_of(plugin)->engine);
}

static const clap_plugin_latency_t latency_ext = {
	.get = latency_get,
};

static bool render_has_hard_realtime(const clap_plugin_t *plugin)
{
	(void)plugin;
	return false;
}

static bool render_set(const clap_plugin_t *plugin, clap_plugin_render_mode mode)
{
	mdmi_engine_set_offline(self_of(plugin)->engine, mode == CLAP_RENDER_OFFLINE);
	return true;
}

static const clap_plugin_render_t render_ext = {
	.has_hard_realtime_requirement = render_has_hard_realtime,
	.set = render_set,
};

static uint32_t params_count(const clap_plugin_t *plugin)
{
	(void)plugin;
	return PATCH_NUM_IDS;
}

static bool params_get_info(const clap_plugin_t *plugin, uint32_t index, clap_param_info_t *info)
{
	(void)plugin;
	uint32_t id = patch_param_id_at(index);
	const patch_param_info *param = patch_param(id);
	if (!param) {
		return false;
	}
	memset(info, 0, sizeof(*info));
	info->id = id;
	info->flags = CLAP_PARAM_IS_AUTOMATABLE | (param->stepped ? CLAP_PARAM_IS_STEPPED : 0);
	patch_param_name(id, info->name, sizeof(info->name));
	info->min_value = 0;
	info->max_value = param->max;
	info->default_value = param->default_value;
	return true;
}

static bool params_get_value(const clap_plugin_t *plugin, clap_id id, double *value)
{
	if (!patch_param(id)) {
		return false;
	}
	*value = patch_get(&self_of(plugin)->patch, id);
	return true;
}

static bool params_value_to_text(const clap_plugin_t *plugin, clap_id id, double value, char *text, uint32_t size)
{
	if (!patch_param(id)) {
		return false;
	}
	patch_format(patch_bank_view(&self_of(plugin)->patch), id, (int)(value + 0.5), text, size);
	return true;
}

static bool params_text_to_value(const clap_plugin_t *plugin, clap_id id, const char *text, double *value)
{
	(void)plugin;
	char *end;
	double parsed = strtod(text, &end);
	if (!patch_param(id) || end == text) {
		return false;
	}
	*value = parsed;
	return true;
}

// Outside processing nothing can reach the ROM, so values wait for the next process call.
static void params_flush(const clap_plugin_t *plugin, const clap_input_events_t *in, const clap_output_events_t *out)
{
	mdmi_plugin *self = self_of(plugin);
	patch_take_incoming(&self->patch);
	const uint32_t count = in->size(in);
	for (uint32_t i = 0; i < count; i++) {
		const clap_event_header_t *header = in->get(in, i);
		if (header->space_id == CLAP_CORE_EVENT_SPACE_ID && header->type == CLAP_EVENT_PARAM_VALUE) {
			const clap_event_param_value_t *event = (const clap_event_param_value_t *)header;
			count_host_value(self, event->param_id);
			if (patch_set(&self->patch, event->param_id, (int)(event->value + 0.5))) {
				request_mark_dirty(self);
			}
		}
	}
	take_edits(self, out, false);
	apply_filter(self);
	patch_publish(&self->patch);
	__atomic_store_n(&self->sync_pending, 1, __ATOMIC_RELEASE);
}

static const clap_plugin_params_t params_ext = {
	.count = params_count,
	.get_info = params_get_info,
	.get_value = params_get_value,
	.value_to_text = params_value_to_text,
	.text_to_value = params_text_to_value,
	.flush = params_flush,
};

// Other DAC samples restart the core with an image that has them (D36). Before the first boot
// that is just the image it boots.
static void apply_dac(mdmi_plugin *self)
{
	if (self->dac.version == self->dac_applied) {
		return;
	}
	size_t size, built;
	const uint8_t *rom = mdmi_engine_builtin_rom(&size);
	uint8_t *image = dac_image(&self->dac, rom, size, &built);
	if (image) {
		self->dac_applied = self->dac.version;
		mdmi_engine_set_rom(self->engine, image, built);
		free(image);
	}
}

// After a project, a bank or a snapshot went to the model, or a file's voice did.
static void after_load(mdmi_plugin *self, bool knobs)
{
	apply_dac(self);
	if (self->active) {
		// The next process call or flush takes it. The host may not process until asked.
		if (self->host_params) {
			self->host_params->request_flush(self->host);
		}
	} else if (patch_take_incoming(&self->patch)) {
		apply_filter(self);
		__atomic_store_n(&self->sync_pending, 1, __ATOMIC_RELEASE);
	}
	if (knobs && self->host_params) {
		self->host_params->rescan(self->host, CLAP_PARAM_RESCAN_VALUES);
	}
}

static bool state_save(const clap_plugin_t *plugin, const clap_ostream_t *stream)
{
	mdmi_plugin *self = self_of(plugin);
	size_t size;
	uint8_t *buffer = state_build(&self->patch, &self->view, &size);
	if (!buffer) {
		return false;
	}
	size_t written = 0;
	while (written < size) {
		int64_t count = stream->write(stream, buffer + written, size - written);
		if (count <= 0) {
			break;
		}
		written += (size_t)count;
	}
	free(buffer);
	return written == size;
}

static bool state_load(const clap_plugin_t *plugin, const clap_istream_t *stream)
{
	mdmi_plugin *self = self_of(plugin);
	uint8_t *buffer = malloc(STATE_READ_MAX);
	if (!buffer) {
		return false;
	}
	size_t total = 0;
	int64_t count = 1;
	while (total < STATE_READ_MAX && count > 0) {
		count = stream->read(stream, buffer + total, STATE_READ_MAX - total);
		total += count > 0 ? (size_t)count : 0;
	}
	bool knobs;
	bool loaded = count >= 0 && state_read(&self->patch, buffer, total, &self->view, &knobs);
	free(buffer);
	if (!loaded) {
		return false;
	}
	after_load(self, knobs);
	return true;
}

static const clap_plugin_state_t state_ext = {
	.save = state_save,
	.load = state_load,
};

static bool gui_is_api_supported(const clap_plugin_t *plugin, const char *api, bool is_floating)
{
	(void)plugin;
	return !is_floating && !strcmp(api, CLAP_WINDOW_API_WIN32);
}

static bool gui_get_preferred_api(const clap_plugin_t *plugin, const char **api, bool *is_floating)
{
	(void)plugin;
	*api = CLAP_WINDOW_API_WIN32;
	*is_floating = false;
	return true;
}

static bool gui_ext_create(const clap_plugin_t *plugin, const char *api, bool is_floating)
{
	mdmi_plugin *self = self_of(plugin);
	if (self->gui || !gui_is_api_supported(plugin, api, is_floating)) {
		return false;
	}
	gui_source source = {
		.engine = self->engine,
		.patch = &self->patch,
		.notes = self->notes,
		.psg_times = &self->psg_times,
		.host_values = self->host_values,
		.version = plugin_descriptor.version,
		.page = &self->view.page,
		.channel = &self->view.channel,
		.all_envelopes = &self->view.envelopes,
		.loop_shadows = self->view.loops,
		.files = &self->files,
		.edit = gui_edit,
		.context = self,
	};
	self->gui = gui_create(&source);
	return self->gui != NULL;
}

static void gui_ext_destroy(const clap_plugin_t *plugin)
{
	mdmi_plugin *self = self_of(plugin);
	gui_destroy(self->gui);
	self->gui = NULL;
}

static bool gui_ext_set_scale(const clap_plugin_t *plugin, double scale)
{
	return gui_set_scale(self_of(plugin)->gui, scale);
}

static bool gui_ext_get_size(const clap_plugin_t *plugin, uint32_t *width, uint32_t *height)
{
	gui_get_size(self_of(plugin)->gui, width, height);
	return true;
}

static bool gui_can_resize(const clap_plugin_t *plugin)
{
	(void)plugin;
	return false;
}

static bool gui_get_resize_hints(const clap_plugin_t *plugin, clap_gui_resize_hints_t *hints)
{
	(void)plugin;
	(void)hints;
	return false;
}

static bool gui_adjust_size(const clap_plugin_t *plugin, uint32_t *width, uint32_t *height)
{
	gui_get_size(self_of(plugin)->gui, width, height);
	return true;
}

static bool gui_set_size(const clap_plugin_t *plugin, uint32_t width, uint32_t height)
{
	(void)plugin;
	(void)width;
	(void)height;
	return false;
}

static bool gui_ext_set_parent(const clap_plugin_t *plugin, const clap_window_t *window)
{
	return gui_set_parent(self_of(plugin)->gui, window->win32);
}

static bool gui_set_transient(const clap_plugin_t *plugin, const clap_window_t *window)
{
	(void)plugin;
	(void)window;
	return false;
}

static void gui_suggest_title(const clap_plugin_t *plugin, const char *title)
{
	(void)plugin;
	(void)title;
}

static bool gui_ext_show(const clap_plugin_t *plugin)
{
	return gui_show(self_of(plugin)->gui, true);
}

static bool gui_ext_hide(const clap_plugin_t *plugin)
{
	return gui_show(self_of(plugin)->gui, false);
}

static const clap_plugin_gui_t gui_ext = {
	.is_api_supported = gui_is_api_supported,
	.get_preferred_api = gui_get_preferred_api,
	.create = gui_ext_create,
	.destroy = gui_ext_destroy,
	.set_scale = gui_ext_set_scale,
	.get_size = gui_ext_get_size,
	.can_resize = gui_can_resize,
	.get_resize_hints = gui_get_resize_hints,
	.adjust_size = gui_adjust_size,
	.set_size = gui_set_size,
	.set_parent = gui_ext_set_parent,
	.set_transient = gui_set_transient,
	.suggest_title = gui_suggest_title,
	.show = gui_ext_show,
	.hide = gui_ext_hide,
};

// The arrow keys, from our patch of clap-wrapper: VST3 hosts give keys to the view, not to the
// window. True when the window took the key.
typedef struct {
	bool (*key)(const clap_plugin_t *plugin, bool up);
} mdmi_gui_key;

static bool gui_key_ext_key(const clap_plugin_t *plugin, bool up)
{
	mdmi_plugin *self = self_of(plugin);
	return self->gui && gui_key(self->gui, up);
}

static const mdmi_gui_key gui_key_ext = {gui_key_ext_key};

static const void *plugin_get_extension(const clap_plugin_t *plugin, const char *id)
{
	(void)plugin;
	if (!strcmp(id, "mdmi.gui-key")) {
		return &gui_key_ext;
	}
	if (!strcmp(id, CLAP_EXT_GUI)) {
		return &gui_ext;
	}
	if (!strcmp(id, CLAP_EXT_PARAMS)) {
		return &params_ext;
	}
	if (!strcmp(id, CLAP_EXT_STATE)) {
		return &state_ext;
	}
	if (!strcmp(id, CLAP_EXT_AUDIO_PORTS)) {
		return &audio_ports_ext;
	}
	if (!strcmp(id, CLAP_EXT_NOTE_PORTS)) {
		return &note_ports_ext;
	}
	if (!strcmp(id, CLAP_EXT_LATENCY)) {
		return &latency_ext;
	}
	if (!strcmp(id, CLAP_EXT_RENDER)) {
		return &render_ext;
	}
	return NULL;
}

static void plugin_on_main_thread(const clap_plugin_t *plugin)
{
	mdmi_plugin *self = self_of(plugin);
	if (__atomic_exchange_n(&self->dirty_pending, 0, __ATOMIC_ACQ_REL)) {
		if (self->host_state) {
			self->host_state->mark_dirty(self->host);
		}
	}
}

static const clap_plugin_t *factory_create_plugin(const clap_plugin_factory_t *factory, const clap_host_t *host, const char *plugin_id)
{
	(void)factory;
	if (!clap_version_is_compatible(host->clap_version) || strcmp(plugin_id, PLUGIN_ID)) {
		return NULL;
	}
	mdmi_plugin *self = calloc(1, sizeof(*self));
	if (!self) {
		return NULL;
	}
	self->host = host;
	for (int i = 0; i < PATCH_NUM_PSG_CHANNELS; i++) {
		self->psg_times.on[i] = self->psg_times.off[i] = GUI_NEVER;
	}
	self->plugin.desc = &plugin_descriptor;
	self->plugin.plugin_data = self;
	self->plugin.init = plugin_init;
	self->plugin.destroy = plugin_destroy;
	self->plugin.activate = plugin_activate;
	self->plugin.deactivate = plugin_deactivate;
	self->plugin.start_processing = plugin_start_processing;
	self->plugin.stop_processing = plugin_stop_processing;
	self->plugin.reset = plugin_reset;
	self->plugin.process = plugin_process;
	self->plugin.get_extension = plugin_get_extension;
	self->plugin.on_main_thread = plugin_on_main_thread;
	return &self->plugin;
}

static uint32_t factory_get_plugin_count(const clap_plugin_factory_t *factory)
{
	(void)factory;
	return 1;
}

static const clap_plugin_descriptor_t *factory_get_plugin_descriptor(const clap_plugin_factory_t *factory, uint32_t index)
{
	(void)factory;
	return index == 0 ? &plugin_descriptor : NULL;
}

static const clap_plugin_factory_t plugin_factory = {
	.get_plugin_count = factory_get_plugin_count,
	.get_plugin_descriptor = factory_get_plugin_descriptor,
	.create_plugin = factory_create_plugin,
};

bool mdmi_clap_init(const char *plugin_path)
{
	(void)plugin_path;
	patch_global_init();
	return mdmi_engine_global_init();
}

void mdmi_clap_deinit(void)
{
	mdmi_engine_global_deinit();
}

const void *mdmi_clap_get_factory(const char *factory_id)
{
	return strcmp(factory_id, CLAP_PLUGIN_FACTORY_ID) ? NULL : &plugin_factory;
}
