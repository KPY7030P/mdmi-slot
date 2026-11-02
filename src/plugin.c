#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <clap/clap.h>
#include "engine.h"
#include "gui.h"
#include "patch.h"

#ifdef MDMI_E3
// Experiment E3 (docs/ROADMAP.md): a separate test plugin with the parameter count the real one
// would have. Removed when the experiment has given its answers.
#define PLUGIN_ID "dev.mdmi-plugin.e3-test"
#define PLUGIN_NAME "MDMI E3 Test"
#define E3_FIRST_ID 1000
#define E3_EXTRA_PARAMS 298
#define E3_CELL_PROGRAM 1
#else
#define PLUGIN_ID "dev.mdmi-plugin.mega-drive"
#define PLUGIN_NAME "MDMI Mega Drive"
#endif

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

typedef struct {
	clap_plugin_t plugin;
	const clap_host_t *host;
	mdmi_engine *engine;
	const clap_host_params_t *host_params;
	const clap_host_state_t *host_state;
	mdmi_patch patch;
	uint32_t sync_pending;
	uint32_t rescan_pending;
	mdmi_gui *gui;
	int gui_page;
	uint32_t notes[GUI_NUM_MIDI_CHANNELS];
	uint8_t held[GUI_NUM_MIDI_CHANNELS];
#ifdef MDMI_E3
	int16_t e3_values[E3_EXTRA_PARAMS];
	uint32_t e3_begin[GUI_E3_CELLS];
	uint32_t e3_changed[GUI_E3_CELLS];
	uint32_t e3_end[GUI_E3_CELLS];
	int e3_sent_program;
	uint32_t e3_programs_seen;
	uint32_t e3_last_program;
#endif
} mdmi_plugin;

typedef struct {
	mdmi_engine *engine;
	uint32_t offset;
} emit_target;

static void emit_to_engine(void *context, const uint8_t *bytes, size_t length)
{
	emit_target *target = context;
	mdmi_engine_midi(target->engine, target->offset, bytes, length);
}

static void apply_filter(mdmi_plugin *self)
{
	mdmi_engine_set_filter(self->engine, patch_filter_hz(patch_get(&self->patch, PARAM_FILTER)));
}

// The host learns about knobs it didn't move itself on the main thread.
static void request_rescan(mdmi_plugin *self)
{
	__atomic_store_n(&self->rescan_pending, 1, __ATOMIC_RELEASE);
	self->host->request_callback(self->host);
}

static void release_notes(mdmi_plugin *self, int channel);

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
	mdmi_engine_destroy(self->engine);
	free(self);
}

static bool plugin_activate(const clap_plugin_t *plugin, double sample_rate, uint32_t min_frames, uint32_t max_frames)
{
	(void)min_frames;
	mdmi_plugin *self = self_of(plugin);
	__atomic_store_n(&self->sync_pending, 1, __ATOMIC_RELEASE);
	for (int channel = 0; channel < GUI_NUM_MIDI_CHANNELS; channel++) {
		release_notes(self, channel);
	}
	return mdmi_engine_activate(self->engine, sample_rate, max_frames);
}

static void plugin_deactivate(const clap_plugin_t *plugin)
{
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
	mdmi_engine_all_notes_off(self->engine, 0);
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
		if (bytes[1] == 120 || bytes[1] == 121 || bytes[1] == 123) {
			release_notes(self, channel);
		}
		break;
#ifdef MDMI_E3
	case 0xC0:
		__atomic_store_n(&self->e3_last_program, ((uint32_t)channel << 8) | bytes[1], __ATOMIC_RELEASE);
		__atomic_fetch_add(&self->e3_programs_seen, 1, __ATOMIC_ACQ_REL);
		break;
#endif
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
	emit_target target = {self->engine, time};
	bool knobs_changed = false;
	if (patch_track_midi(&self->patch, bytes, length, &knobs_changed)) {
		emit_to_engine(&target, bytes, length);
	} else {
		patch_sync(&self->patch, emit_to_engine, &target);
	}
	if (knobs_changed) {
		request_rescan(self);
	}
}

#ifdef MDMI_E3
static int e3_cell_param(int cell)
{
	return E3_FIRST_ID + cell;
}

static void e3_send_program(mdmi_plugin *self, uint32_t time, int value)
{
	const uint8_t message[2] = {0xC6, (uint8_t)value};
	mdmi_engine_midi(self->engine, time, message, sizeof(message));
	self->e3_sent_program = value;
}

static void e3_push_gesture(const clap_output_events_t *out, uint16_t type, int cell)
{
	clap_event_param_gesture_t event = {{sizeof(event), 0, CLAP_CORE_EVENT_SPACE_ID, type, 0}, (clap_id)e3_cell_param(cell)};
	out->try_push(out, &event.header);
}

// What the window's cells did since the last block, told to the host.
static void e3_send_gestures(mdmi_plugin *self, const clap_output_events_t *out)
{
	for (int cell = 0; cell < GUI_E3_CELLS; cell++) {
		if (__atomic_exchange_n(&self->e3_begin[cell], 0, __ATOMIC_ACQ_REL)) {
			e3_push_gesture(out, CLAP_EVENT_PARAM_GESTURE_BEGIN, cell);
		}
		if (__atomic_exchange_n(&self->e3_changed[cell], 0, __ATOMIC_ACQ_REL)) {
			clap_event_param_value_t event = {{sizeof(event), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0}, (clap_id)e3_cell_param(cell), NULL, -1, -1, -1, -1, __atomic_load_n(&self->e3_values[cell], __ATOMIC_ACQUIRE)};
			out->try_push(out, &event.header);
		}
		if (__atomic_exchange_n(&self->e3_end[cell], 0, __ATOMIC_ACQ_REL)) {
			e3_push_gesture(out, CLAP_EVENT_PARAM_GESTURE_END, cell);
		}
	}
}

static void e3_request_flush(mdmi_plugin *self)
{
	if (self->host_params) {
		self->host_params->request_flush(self->host);
	}
}

static int e3_cell_value(void *context, int cell)
{
	return __atomic_load_n(&((mdmi_plugin *)context)->e3_values[cell], __ATOMIC_ACQUIRE);
}

static void e3_cell_begin(void *context, int cell)
{
	mdmi_plugin *self = context;
	__atomic_store_n(&self->e3_begin[cell], 1, __ATOMIC_RELEASE);
	e3_request_flush(self);
}

static void e3_cell_change(void *context, int cell, int value)
{
	mdmi_plugin *self = context;
	__atomic_store_n(&self->e3_values[cell], (int16_t)value, __ATOMIC_RELEASE);
	__atomic_store_n(&self->e3_changed[cell], 1, __ATOMIC_RELEASE);
	e3_request_flush(self);
}

static void e3_cell_end(void *context, int cell)
{
	mdmi_plugin *self = context;
	__atomic_store_n(&self->e3_end[cell], 1, __ATOMIC_RELEASE);
	e3_request_flush(self);
}
#endif

static void handle_param(mdmi_plugin *self, const clap_event_param_value_t *event)
{
#ifdef MDMI_E3
	if (event->param_id >= E3_FIRST_ID) {
		int e3_value = (int)(event->value + 0.5);
		__atomic_store_n(&self->e3_values[event->param_id - E3_FIRST_ID], (int16_t)e3_value, __ATOMIC_RELEASE);
		if (event->param_id == (clap_id)e3_cell_param(E3_CELL_PROGRAM)) {
			e3_send_program(self, event->header.time, e3_value);
		}
		return;
	}
#endif
	int value = (int)(event->value + 0.5);
	if (patch_set(&self->patch, event->param_id, value)) {
		request_rescan(self);
	}
	if (event->param_id == PARAM_FILTER) {
		apply_filter(self);
	}
	emit_target target = {self->engine, event->header.time};
	patch_sync(&self->patch, emit_to_engine, &target);
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
		mdmi_engine_midi(self->engine, header->time, message, sizeof(message));
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
		mdmi_engine_midi(self->engine, header->time, sysex->buffer, sysex->size);
		break;
	}
	case CLAP_EVENT_PARAM_VALUE:
		handle_param(self, (const clap_event_param_value_t *)header);
		break;
	default:
		break;
	}
}

static clap_process_status plugin_process(const clap_plugin_t *plugin, const clap_process_t *process)
{
	mdmi_plugin *self = self_of(plugin);
	mdmi_engine *engine = self->engine;
	if (__atomic_exchange_n(&self->sync_pending, 0, __ATOMIC_ACQ_REL)) {
		emit_target target = {engine, 0};
		patch_sync(&self->patch, emit_to_engine, &target);
	}
	const uint32_t event_count = process->in_events->size(process->in_events);
	for (uint32_t i = 0; i < event_count; i++) {
		handle_event(self, process->in_events->get(process->in_events, i));
	}
#ifdef MDMI_E3
	e3_send_gestures(self, process->out_events);
	int e3_program = e3_cell_value(self, E3_CELL_PROGRAM);
	if (e3_program != self->e3_sent_program) {
		e3_send_program(self, 0, e3_program);
	}
#endif
	const clap_audio_buffer_t *out = &process->audio_outputs[0];
	float *left = out->data32[0];
	float *right = out->channel_count > 1 ? out->data32[1] : NULL;
	float scratch[4096];
	uint32_t done = 0;
	while (done < process->frames_count) {
		uint32_t frames = process->frames_count - done;
		if (!right && frames > 4096) {
			frames = 4096;
		}
		mdmi_engine_render(engine, left + done, right ? right + done : scratch, frames);
		done += frames;
	}
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
#ifdef MDMI_E3
	return PATCH_NUM_PARAMS + E3_EXTRA_PARAMS;
#else
	return PATCH_NUM_PARAMS;
#endif
}

static bool params_get_info(const clap_plugin_t *plugin, uint32_t index, clap_param_info_t *info)
{
	(void)plugin;
#ifdef MDMI_E3
	if (index >= PATCH_NUM_PARAMS) {
		uint32_t extra = index - PATCH_NUM_PARAMS;
		if (extra >= E3_EXTRA_PARAMS) {
			return false;
		}
		memset(info, 0, sizeof(*info));
		info->id = E3_FIRST_ID + extra;
		info->flags = CLAP_PARAM_IS_AUTOMATABLE | CLAP_PARAM_IS_STEPPED;
		if (extra < GUI_E3_CELLS) {
			snprintf(info->name, sizeof(info->name), "%s", extra == 0 ? "CH4 Op1 TL" : "CH7 Preset");
		} else {
			snprintf(info->name, sizeof(info->name), "E3 Param %u", extra);
		}
		info->max_value = 127;
		return true;
	}
#endif
	uint32_t id = patch_param_id_at(index);
	const patch_param_info *param = patch_param(id);
	if (!param) {
		return false;
	}
	memset(info, 0, sizeof(*info));
	info->id = id;
	info->flags = CLAP_PARAM_IS_AUTOMATABLE | (param->stepped ? CLAP_PARAM_IS_STEPPED : 0);
	strncpy(info->name, param->name, sizeof(info->name) - 1);
	info->min_value = 0;
	info->max_value = param->max;
	info->default_value = param->default_value;
	return true;
}

static bool params_get_value(const clap_plugin_t *plugin, clap_id id, double *value)
{
#ifdef MDMI_E3
	if (id >= E3_FIRST_ID && id < E3_FIRST_ID + E3_EXTRA_PARAMS) {
		*value = e3_cell_value(self_of(plugin), (int)(id - E3_FIRST_ID));
		return true;
	}
#endif
	if (!patch_param(id)) {
		return false;
	}
	*value = patch_get(&self_of(plugin)->patch, id);
	return true;
}

static bool params_value_to_text(const clap_plugin_t *plugin, clap_id id, double value, char *text, uint32_t size)
{
#ifdef MDMI_E3
	if (id >= E3_FIRST_ID) {
		snprintf(text, size, "%d", (int)(value + 0.5));
		return true;
	}
#endif
	if (!patch_param(id)) {
		return false;
	}
	patch_format(&self_of(plugin)->patch, id, (int)(value + 0.5), text, size);
	return true;
}

static bool params_text_to_value(const clap_plugin_t *plugin, clap_id id, const char *text, double *value)
{
	(void)plugin;
	char *end;
	double parsed = strtod(text, &end);
#ifdef MDMI_E3
	if (id >= E3_FIRST_ID && end != text) {
		*value = parsed;
		return true;
	}
#endif
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
#ifdef MDMI_E3
	e3_send_gestures(self, out);
#else
	(void)out;
#endif
	const uint32_t count = in->size(in);
	for (uint32_t i = 0; i < count; i++) {
		const clap_event_header_t *header = in->get(in, i);
		if (header->space_id == CLAP_CORE_EVENT_SPACE_ID && header->type == CLAP_EVENT_PARAM_VALUE) {
			const clap_event_param_value_t *event = (const clap_event_param_value_t *)header;
#ifdef MDMI_E3
			if (event->param_id >= E3_FIRST_ID) {
				__atomic_store_n(&self->e3_values[event->param_id - E3_FIRST_ID], (int16_t)(event->value + 0.5), __ATOMIC_RELEASE);
				continue;
			}
#endif
			if (patch_set(&self->patch, event->param_id, (int)(event->value + 0.5))) {
				request_rescan(self);
			}
		}
	}
	apply_filter(self);
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

static bool state_save(const clap_plugin_t *plugin, const clap_ostream_t *stream)
{
	uint8_t buffer[sizeof(uint32_t) + sizeof(patch_values)];
	patch_save(&self_of(plugin)->patch, buffer);
	size_t written = 0;
	while (written < sizeof(buffer)) {
		int64_t count = stream->write(stream, buffer + written, sizeof(buffer) - written);
		if (count <= 0) {
			return false;
		}
		written += (size_t)count;
	}
	return true;
}

static bool state_load(const clap_plugin_t *plugin, const clap_istream_t *stream)
{
	mdmi_plugin *self = self_of(plugin);
	uint8_t buffer[sizeof(uint32_t) + sizeof(patch_values)];
	size_t total = 0;
	while (total < sizeof(buffer)) {
		int64_t count = stream->read(stream, buffer + total, sizeof(buffer) - total);
		if (count < 0) {
			return false;
		}
		if (count == 0) {
			break;
		}
		total += (size_t)count;
	}
	if (!patch_load(&self->patch, buffer, total)) {
		return false;
	}
	apply_filter(self);
	__atomic_store_n(&self->sync_pending, 1, __ATOMIC_RELEASE);
	if (self->host_params) {
		self->host_params->rescan(self->host, CLAP_PARAM_RESCAN_VALUES);
	}
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
		.version = plugin_descriptor.version,
		.page = &self->gui_page,
#ifdef MDMI_E3
		.e3 = {
			.context = self,
			.names = {"CH4 OP1 TL", "CH7 PRESET"},
			.value = e3_cell_value,
			.begin = e3_cell_begin,
			.change = e3_cell_change,
			.end = e3_cell_end,
			.programs_seen = &self->e3_programs_seen,
			.last_program = &self->e3_last_program,
		},
#endif
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

static const void *plugin_get_extension(const clap_plugin_t *plugin, const char *id)
{
	(void)plugin;
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
	if (__atomic_exchange_n(&self->rescan_pending, 0, __ATOMIC_ACQ_REL)) {
		if (self->host_params) {
			self->host_params->rescan(self->host, CLAP_PARAM_RESCAN_VALUES);
		}
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
