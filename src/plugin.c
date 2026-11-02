#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <clap/clap.h>
#include "engine.h"
#include "patch.h"

#define PLUGIN_ID "dev.mdmi-plugin.mega-drive"

static const char *const plugin_features[] = {
	CLAP_PLUGIN_FEATURE_INSTRUMENT,
	CLAP_PLUGIN_FEATURE_SYNTHESIZER,
	CLAP_PLUGIN_FEATURE_STEREO,
	NULL,
};

static const clap_plugin_descriptor_t plugin_descriptor = {
	.clap_version = CLAP_VERSION_INIT,
	.id = PLUGIN_ID,
	.name = "MDMI Mega Drive",
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
		fprintf(stderr, "stats underruns=%u dropped_midi=%u failed=%d booted=%d worst_wait=%.2fms worst_run=%.2fms@%llu underruns_at=%llu..%llu\n", stats.underruns, stats.dropped_midi, stats.failed, stats.booted,
			stats.worst_wait_ms, stats.worst_run_ms, (unsigned long long)stats.worst_run_pos, (unsigned long long)stats.first_underrun_pos, (unsigned long long)stats.last_underrun_pos);
	}
	mdmi_engine_destroy(self->engine);
	free(self);
}

static bool plugin_activate(const clap_plugin_t *plugin, double sample_rate, uint32_t min_frames, uint32_t max_frames)
{
	(void)min_frames;
	mdmi_plugin *self = self_of(plugin);
	__atomic_store_n(&self->sync_pending, 1, __ATOMIC_RELEASE);
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
	mdmi_engine_all_notes_off(self_of(plugin)->engine, 0);
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

static void handle_param(mdmi_plugin *self, const clap_event_param_value_t *event)
{
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
		break;
	}
	case CLAP_EVENT_MIDI: {
		const clap_event_midi_t *midi = (const clap_event_midi_t *)header;
		uint8_t status = midi->data[0];
		size_t length = (status & 0xF0) == 0xC0 || (status & 0xF0) == 0xD0 ? 2 : 3;
		if (status >= 0xF0) {
			length = status == 0xF2 ? 3 : status == 0xF1 || status == 0xF3 ? 2 : 1;
		}
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
	return PATCH_NUM_PARAMS;
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
	strncpy(info->name, param->name, sizeof(info->name) - 1);
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
	patch_format(&self_of(plugin)->patch, id, (int)(value + 0.5), text, size);
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
	(void)out;
	mdmi_plugin *self = self_of(plugin);
	const uint32_t count = in->size(in);
	for (uint32_t i = 0; i < count; i++) {
		const clap_event_header_t *header = in->get(in, i);
		if (header->space_id == CLAP_CORE_EVENT_SPACE_ID && header->type == CLAP_EVENT_PARAM_VALUE) {
			const clap_event_param_value_t *event = (const clap_event_param_value_t *)header;
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

static const void *plugin_get_extension(const clap_plugin_t *plugin, const char *id)
{
	(void)plugin;
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
