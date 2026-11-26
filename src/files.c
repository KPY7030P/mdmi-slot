#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "engine.h"
#include "files.h"
#include "rom_map.h"
#include "instrument.h"
#include "names.h"

#define INSTRUMENT_MAX 64

static bool is_psg(int channel)
{
	return channel >= PATCH_NUM_FM_CHANNELS && channel < PATCH_NUM_CHANNELS;
}

void files_init(mdmi_files *files, const files_host *host)
{
	memset(files, 0, sizeof(*files));
	files->host = *host;
	files->back = calloc(FILES_UNDO_DEPTH, sizeof(files_step));
	files->ahead = calloc(FILES_UNDO_DEPTH, sizeof(files_step));
}

// What a step holds of its own: a state, sample copies, values.
static void step_free(files_step *step)
{
	free(step->state);
	step->state = NULL;
	dac_clear(&step->dac);
	free(step->values);
	step->values = NULL;
	step->kind = UNDO_NONE;
}

static void steps_free(files_step *steps, int *count)
{
	for (int i = 0; i < *count; i++) {
		step_free(&steps[i]);
	}
	*count = 0;
}

// What a step holds of its own in memory: a whole state, copies of samples (their sources too), values.
static size_t step_bytes(const files_step *step)
{
	size_t bytes = step->state_size + (step->values ? sizeof(*step->values) : 0);
	for (int note = 0; note < ROM_IMAGE_NOTES; note++) {
		const dac_sample *sample = step->dac.notes[note];
		bytes += sample ? sizeof(*sample) + sample->sample.length + sample->source_frames * sizeof(int16_t) : 0;
	}
	return bytes;
}

// A step onto a stack; a full stack drops its oldest. The steps back hold one less: now is the 32nd.
// Heavy steps (whole states, sample sets) drop the oldest sooner: the stack keeps FILES_UNDO_BYTES at
// most, the newest step always (review of stage 48).
static void push_step(files_step *steps, int *count, int limit, files_step *step)
{
	if (!steps) {
		step_free(step);
		return;
	}
	if (*count == limit) {
		step_free(&steps[0]);
		memmove(steps, steps + 1, (size_t)(limit - 1) * sizeof(files_step));
		(*count)--;
	}
	steps[(*count)++] = *step;
	memset(step, 0, sizeof(*step));
	size_t bytes = 0;
	for (int i = 0; i < *count; i++) {
		bytes += step_bytes(&steps[i]);
	}
	while (*count > 1 && bytes > FILES_UNDO_BYTES) {
		bytes -= step_bytes(&steps[0]);
		step_free(&steps[0]);
		memmove(steps, steps + 1, (size_t)(*count - 1) * sizeof(files_step));
		memset(&steps[*count - 1], 0, sizeof(files_step));
		(*count)--;
	}
}

static void pop_step(files_step *steps, int *count, files_step *into)
{
	memset(into, 0, sizeof(*into));
	if (steps && *count > 0) {
		*into = steps[--*count];
		memset(&steps[*count], 0, sizeof(files_step));
	}
}

void files_forget(mdmi_files *files)
{
	step_free(&files->now);
	steps_free(files->back, &files->back_count);
	steps_free(files->ahead, &files->ahead_count);
}

void files_free(mdmi_files *files)
{
	files_forget(files);
	free(files->back);
	free(files->ahead);
	files->back = files->ahead = NULL;
	free(files->signed_bank);
	files->signed_bank = NULL;
}

static const char *extension_of(const char *path)
{
	const char *base = path;
	for (const char *c = path; *c; c++) {
		if (*c == '/' || *c == '\\') {
			base = c + 1;
		}
	}
	const char *dot = strrchr(base, '.');
	return dot ? dot + 1 : "";
}

// A name in a message as the footer shows it: its transliteration cut by whole letters (stage 60), so a
// Russian name gets as many letters as a Latin one, not half of them as a cut by bytes gave.
static const char *letters(const char *name, char *out, size_t size, int chars)
{
	names_fit(name, out, size, chars);
	return out;
}

static const char *base_of(const char *path)
{
	const char *base = path;
	for (const char *c = path; *c; c++) {
		if (*c == '/' || *c == '\\') {
			base = c + 1;
		}
	}
	return base;
}

int files_kind(const char *path)
{
	const char *extension = extension_of(path);
	if (!_stricmp(extension, "tfi") || !_stricmp(extension, "vgi") || !_stricmp(extension, "tyi")) {
		return FILES_INSTRUMENT;
	}
	if (!_stricmp(extension, "mdbank")) {
		return FILES_BANK;
	}
	if (!_stricmp(extension, "mdsnap")) {
		return FILES_SNAPSHOT;
	}
	if (!_stricmp(extension, "eef")) {
		return FILES_ENVELOPE;
	}
	if (!_stricmp(extension, "wav")) {
		return FILES_SAMPLE;
	}
	return -1;
}

static FILE *open_utf8(const char *path, const wchar_t *mode)
{
	wchar_t wide[MAX_PATH * 2];
	if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, MAX_PATH * 2)) {
		return NULL;
	}
	return _wfopen(wide, mode);
}

// The whole file, if it is no larger than most. Freed by the caller.
static uint8_t *read_up_to(const char *path, size_t most, size_t *size)
{
	FILE *file = open_utf8(path, L"rb");
	if (!file) {
		return NULL;
	}
	uint8_t *bytes = malloc(most + 1);
	if (bytes) {
		*size = fread(bytes, 1, most + 1, file);
	}
	fclose(file);
	if (bytes && *size > most) {
		free(bytes);
		return NULL;
	}
	return bytes;
}

static uint8_t *read_all(const char *path, size_t *size)
{
	return read_up_to(path, STATE_READ_MAX, size);
}

// A WAV is read whole before it is turned into the driver's format: 96 kHz stereo 32-bit floats for
// the 236 s the ROM could hold at most at 8 kHz would be 180 MB, so this is a size no sample needs.
#define WAV_READ_MAX (192u << 20)

static bool write_all(const char *path, const uint8_t *bytes, size_t size)
{
	FILE *file = open_utf8(path, L"wb");
	if (!file) {
		return false;
	}
	bool written = fwrite(bytes, 1, size, file) == size;
	return fclose(file) == 0 && written;
}

// What a channel has now, as an instrument.
static void channel_instrument(mdmi_patch *patch, int channel, patch_instrument *out)
{
	const patch_values *values = patch_view(patch);
	memcpy(out->name, values->names[channel], PATCH_NAME);
	memcpy(out->voice, values->channels[channel] + PARAM_ALGORITHM, sizeof(out->voice));
}

static void channel_envelope(mdmi_patch *patch, int channel, patch_envelope *out)
{
	*out = *patch_envelope_view(patch, channel);
}

// What undo puts back: an instrument or an envelope, whose name is the text's.
// A new action's step goes on top of the history and empties what redo had; a step being applied
// leaves its own in place of the one it was (stage 48, D90).
static void remember(mdmi_files *files, int undo, int target, const patch_instrument *instrument, const patch_envelope *envelope, uint8_t *state, size_t state_size)
{
	if (files->replaying || files->now.kind == UNDO_NONE) {
		step_free(&files->now);
	} else {
		push_step(files->back, &files->back_count, FILES_UNDO_DEPTH - 1, &files->now);
	}
	if (!files->replaying) {
		steps_free(files->ahead, &files->ahead_count);
	}
	files->now.kind = undo;
	files->now.target = target;
	files->now.channel = -1;
	files->now.state = state;
	files->now.state_size = state_size;
	files->now.view = false;
	files->now.drawing = 0;
	files->now.went_count = 0;
	files->now.went_seen = false;
	if (instrument) {
		files->now.instrument = *instrument;
	}
	if (envelope) {
		files->now.envelope = *envelope;
	}
	const char *name = instrument ? instrument->name : envelope ? envelope->name : "";
	switch (undo) {
	case UNDO_CHANNEL:
		snprintf(files->now.text, sizeof(files->now.text), "CH%d back to %s", target + 1, name);
		break;
	case UNDO_PLACE:
	case UNDO_ENVELOPE_PLACE:
		snprintf(files->now.text, sizeof(files->now.text), "slot %d back to %s", target + 1, name);
		break;
	default:
		snprintf(files->now.text, sizeof(files->now.text), target == FILES_BANK ? "the bank back" : "everything back");
		break;
	}
}

// Undo puts the channel's base back with its sound (D31), and the row of each (stage 20). Read before
// remember, which keeps the step before untouched.
static void channel_base(mdmi_files *files, int channel, patch_instrument *instrument, patch_envelope *envelope, patch_arp *arp)
{
	patch_base_view(files->host.patch, channel, instrument, envelope);
	patch_arp_base_view(files->host.patch, channel, arp);
}

// A file is the channel's base as well, and brings an empty row: files have none (D44). Undo gives
// the channel, its base and their rows back as they were.
static void put_on_channel(mdmi_files *files, int channel, const patch_instrument *instrument, const patch_instrument *base, const patch_arp *arp, const patch_arp *arp_base)
{
	patch_instrument before;
	channel_instrument(files->host.patch, channel, &before);
	const patch_arp before_arp = *patch_arp_view(files->host.patch, channel);
	patch_instrument before_base;
	patch_envelope unused;
	patch_arp before_arp_base;
	channel_base(files, channel, &before_base, &unused, &before_arp_base);
	patch_load_voice(files->host.patch, channel, instrument, base);
	patch_load_arp(files->host.patch, channel, arp, arp_base);
	remember(files, UNDO_CHANNEL, channel, &before, NULL, NULL, 0);
	files->now.base_instrument = before_base;
	files->now.arp = before_arp;
	files->now.base_arp = before_arp_base;
	files->host.handed(files->host.context, false);
}

static void put_envelope_on_channel(mdmi_files *files, int channel, const patch_envelope *envelope, const patch_envelope *base, const patch_arp *arp, const patch_arp *arp_base)
{
	patch_envelope before;
	channel_envelope(files->host.patch, channel, &before);
	const patch_arp before_arp = *patch_arp_view(files->host.patch, channel);
	patch_instrument unused;
	patch_envelope before_base;
	patch_arp before_arp_base;
	channel_base(files, channel, &unused, &before_base, &before_arp_base);
	patch_load_envelope(files->host.patch, channel, envelope, base);
	patch_load_arp(files->host.patch, channel, arp, arp_base);
	remember(files, UNDO_CHANNEL, channel, NULL, &before, NULL, 0);
	files->now.base_envelope = before_base;
	files->now.arp = before_arp;
	files->now.base_arp = before_arp_base;
	files->host.handed(files->host.context, false);
}

// A drawing keeps the base: what the channel got stays what it is compared with. The loop shadow and
// kept release before the gesture go with its step, as with a Preset step (stage 50).
void files_draw_envelope(mdmi_files *files, int channel, const patch_envelope *before, const patch_envelope *envelope, const state_loop *loop, const state_tail *tail)
{
	patch_instrument unused;
	patch_envelope base;
	patch_arp arp, base_arp;
	if (before) {
		channel_base(files, channel, &unused, &base, &base_arp);
		arp = *patch_arp_view(files->host.patch, channel);
	}
	patch_load_envelope(files->host.patch, channel, envelope, NULL);
	if (before) {
		remember(files, UNDO_CHANNEL, channel, NULL, before, NULL, 0);
		files->now.base_envelope = base;
		files->now.arp = arp;
		files->now.base_arp = base_arp;
		files->now.drawing = 1;
		if (loop && tail && files->host.view) {
			files->now.view = true;
			files->now.loop = *loop;
			files->now.tail = *tail;
		}
		snprintf(files->now.text, sizeof(files->now.text), "CH%d envelope before drawing", channel + 1);
	}
	files->host.handed(files->host.context, false);
}

void files_draw_arp(mdmi_files *files, int channel, const patch_arp *before, const patch_arp *arp)
{
	if (channel < 0 || channel >= PATCH_NUM_CHANNELS) {
		return;
	}
	if (before) {
		remember(files, UNDO_ARP, channel, NULL, NULL, NULL, 0);
		files->now.arp = *before;
		snprintf(files->now.text, sizeof(files->now.text), "CH%d arp before drawing", channel + 1);
	}
	patch_load_arp(files->host.patch, channel, arp, NULL);
	files->host.handed(files->host.context, false);
}

// A place gets a sound and its row; undo puts back the place as it was.
static void put_in_place(mdmi_files *files, int place, const patch_instrument *instrument, const patch_arp *arp)
{
	const patch_instrument before = patch_bank_view(files->host.patch)->places[place];
	const patch_arp before_arp = patch_bank_view(files->host.patch)->place_arps[place];
	patch_load_place(files->host.patch, place, instrument, arp);
	remember(files, UNDO_PLACE, place, &before, NULL, NULL, 0);
	files->now.arp = before_arp;
	files->host.handed(files->host.context, false);
}

static void put_envelope_in_place(mdmi_files *files, int place, const patch_envelope *envelope, const patch_arp *arp)
{
	const patch_envelope before = patch_bank_view(files->host.patch)->envelopes[place];
	const patch_arp before_arp = patch_bank_view(files->host.patch)->envelope_arps[place];
	patch_load_envelope_place(files->host.patch, place, envelope, arp);
	remember(files, UNDO_ENVELOPE_PLACE, place, NULL, &before, NULL, 0);
	files->now.arp = before_arp;
	files->host.handed(files->host.context, false);
}

// STORE and its undo (stage 20): the place gets a sound and row, and the channel keeps its own with
// a new base. The place and the base as they were are remembered, so undo is the same swap back.
static void swap_place(mdmi_files *files, int channel, int place, const patch_instrument *instrument, const patch_envelope *envelope, const patch_arp *arp, const patch_instrument *base, const patch_envelope *base_envelope, const patch_arp *base_arp)
{
	mdmi_patch *patch = files->host.patch;
	const bool psg = is_psg(channel);
	const patch_bank *bank = patch_bank_view(patch);
	const patch_instrument place_was = bank->places[place];
	const patch_envelope envelope_was = bank->envelopes[place];
	const patch_arp arp_was = psg ? bank->envelope_arps[place] : bank->place_arps[place];
	const patch_arp row = *patch_arp_view(patch, channel);
	patch_instrument base_was;
	patch_envelope base_envelope_was;
	patch_arp base_arp_was;
	channel_base(files, channel, &base_was, &base_envelope_was, &base_arp_was);
	if (psg) {
		patch_envelope sound;
		channel_envelope(patch, channel, &sound);
		patch_load_envelope_place(patch, place, envelope, arp);
		patch_load_envelope(patch, channel, &sound, base_envelope);
	} else {
		patch_instrument sound;
		channel_instrument(patch, channel, &sound);
		patch_load_place(patch, place, instrument, arp);
		patch_load_voice(patch, channel, &sound, base);
	}
	patch_load_arp(patch, channel, &row, base_arp);
	// After STORE the channel plays its place as it is, so it goes back to the place's number when the
	// ROM can play it there; after its undo the row decides again.
	patch_choose_program_again(patch, channel);
	remember(files, psg ? UNDO_ENVELOPE_PLACE : UNDO_PLACE, place, psg ? NULL : &place_was, psg ? &envelope_was : NULL, NULL, 0);
	files->now.arp = arp_was;
	files->now.base_instrument = base_was;
	files->now.base_envelope = base_envelope_was;
	files->now.base_arp = base_arp_was;
	files->now.channel = channel;
	files->host.handed(files->host.context, false);
}

bool files_can_store(mdmi_files *files, int channel)
{
	if (channel < 0 || channel >= PATCH_NUM_CHANNELS) {
		return false;
	}
	mdmi_patch *patch = files->host.patch;
	const patch_arp row = *patch_arp_view(patch, channel);
	const int place = patch_view(patch)->channels[channel][PARAM_PRESET] & (PATCH_BANK_SIZE - 1);
	const patch_bank *bank = patch_bank_view(patch);
	if (is_psg(channel)) {
		patch_envelope sound;
		channel_envelope(patch, channel, &sound);
		return !patch_arp_same(&row, &bank->envelope_arps[place]) || memcmp(sound.steps, bank->envelopes[place].steps, sizeof(sound.steps)) || strcmp(sound.name, bank->envelopes[place].name);
	}
	patch_instrument sound;
	channel_instrument(patch, channel, &sound);
	return !patch_arp_same(&row, &bank->place_arps[place]) || memcmp(sound.voice, bank->places[place].voice, sizeof(sound.voice)) || strcmp(sound.name, bank->places[place].name);
}

bool files_store(mdmi_files *files, int channel, char *message, size_t size)
{
	if (channel < 0 || channel >= PATCH_NUM_CHANNELS) {
		return false;
	}
	mdmi_patch *patch = files->host.patch;
	const int place = patch_view(patch)->channels[channel][PARAM_PRESET] & (PATCH_BANK_SIZE - 1);
	if (!files_can_store(files, channel)) {
		snprintf(message, size, "CH%d is bank %d as is", channel + 1, place + 1);
		return false;
	}
	const patch_arp row = *patch_arp_view(patch, channel);
	patch_instrument instrument = {0};
	patch_envelope envelope = {0};
	if (is_psg(channel)) {
		channel_envelope(patch, channel, &envelope);
	} else {
		channel_instrument(patch, channel, &instrument);
	}
	swap_place(files, channel, place, &instrument, &envelope, &row, &instrument, &envelope, &row);
	snprintf(message, size, "CH%d stored in bank %d", channel + 1, place + 1);
	return true;
}

// A bank, or everything. False with the state untouched if the bytes are no state.
static bool put_state(mdmi_files *files, int kind, const uint8_t *bytes, size_t size)
{
	size_t before_size;
	uint8_t *before = state_build(files->host.patch, kind == FILES_SNAPSHOT ? files->host.view : NULL, &before_size);
	bool knobs;
	state_view view = *files->host.view;
	if (!before || !state_read(files->host.patch, bytes, size, &view, &knobs)) {
		free(before);
		return false;
	}
	*files->host.view = view;
	remember(files, UNDO_STATE, kind, NULL, NULL, before, before_size);
	files->host.handed(files->host.context, knobs);
	return true;
}

// An instrument file read whole, or why it can't be.
static bool read_instrument(const char *path, patch_instrument *instrument, char *message, size_t size)
{
	const char *name = base_of(path);
	if (files_kind(path) != FILES_INSTRUMENT) {
		snprintf(message, size, "%s: not TFI, VGI or TYI", name);
		return false;
	}
	size_t length = 0;
	uint8_t *bytes = read_all(path, &length);
	if (!bytes) {
		snprintf(message, size, "%s: can't be read", name);
		return false;
	}
	char error[FILES_MESSAGE];
	const bool done = instrument_parse(path, bytes, length, instrument, error, sizeof(error));
	if (!done) {
		snprintf(message, size, "%s: %s", name, error);
	}
	free(bytes);
	return done;
}

static bool read_envelope(const char *path, patch_envelope *envelope, char *message, size_t size)
{
	const char *name = base_of(path);
	if (files_kind(path) != FILES_ENVELOPE) {
		snprintf(message, size, "%s: not an EEF envelope", name);
		return false;
	}
	size_t length = 0;
	uint8_t *bytes = read_all(path, &length);
	if (!bytes) {
		snprintf(message, size, "%s: can't be read", name);
		return false;
	}
	char error[FILES_MESSAGE];
	const bool done = instrument_parse_eef(path, bytes, length, envelope, error, sizeof(error));
	if (!done) {
		snprintf(message, size, "%s: %s", name, error);
	}
	free(bytes);
	return done;
}

bool files_open(mdmi_files *files, const char *path, int channel, char *message, size_t size)
{
	const char *name = base_of(path);
	const int kind = files_kind(path);
	if (kind < 0) {
		snprintf(message, size, "%s: not TFI, VGI, TYI or EEF", name);
		return false;
	}
	if (kind == FILES_ENVELOPE) {
		if (!is_psg(channel)) {
			snprintf(message, size, "Select CH7-10 for an EEF file");
			return false;
		}
		patch_envelope envelope;
		if (!read_envelope(path, &envelope, message, size)) {
			return false;
		}
		const patch_arp none = PATCH_ARP_EMPTY;
		put_envelope_on_channel(files, channel, &envelope, &envelope, &none, &none);
		snprintf(message, size, "Loaded %s to CH%d", envelope.name, channel + 1);
		return true;
	}
	if (kind == FILES_INSTRUMENT) {
		if (channel < 0 || channel >= PATCH_NUM_FM_CHANNELS) {
			snprintf(message, size, "Select CH1-6 for an FM file");
			return false;
		}
		patch_instrument instrument;
		if (!read_instrument(path, &instrument, message, size)) {
			return false;
		}
		const patch_arp none = PATCH_ARP_EMPTY;
		put_on_channel(files, channel, &instrument, &instrument, &none, &none);
		snprintf(message, size, "Loaded %s to CH%d", instrument.name, channel + 1);
		return true;
	}
	size_t length = 0;
	uint8_t *bytes = read_all(path, &length);
	if (!bytes) {
		snprintf(message, size, "%s: can't be read", name);
		return false;
	}
	const bool done = put_state(files, kind, bytes, length);
	snprintf(message, size, done ? "Loaded %s" : "%s: not an MDMI bank or snapshot", name);
	free(bytes);
	return done;
}

int files_add_place(const mdmi_files *files, bool envelopes)
{
	const patch_bank *bank = patch_bank_view(files->host.patch);
	int place = PATCH_BANK_SIZE;
	while (place > 0 && (envelopes ? patch_envelope_is_rom(bank, place - 1) : patch_place_is_gm(bank, place - 1))) {
		place--;
	}
	return place;
}

static int by_name(const void *a, const void *b)
{
	return _stricmp(base_of(*(const char *const *)a), base_of(*(const char *const *)b));
}

int files_put_places(mdmi_files *files, const char *const *paths, int count, int beyond, bool envelopes, int first, int *skipped, char *message, size_t size)
{
	if (skipped) {
		*skipped = count + beyond;
	}
	if (first < 0) {
		first = files_add_place(files, envelopes);
	}
	const char **sorted = malloc(sizeof(*sorted) * (size_t)count);
	patch_bank *bank = malloc(sizeof(*bank));
	if (!sorted || !bank || count < 1) {
		free(sorted);
		free(bank);
		snprintf(message, size, "Nothing to load");
		return false;
	}
	memcpy(sorted, paths, sizeof(*sorted) * (size_t)count);
	qsort(sorted, (size_t)count, sizeof(*sorted), by_name);
	*bank = *patch_bank_view(files->host.patch);

	int place = first, unread = 0, no_room = 0;
	char why[FILES_MESSAGE] = "";
	const char *first_unread = "";
	for (int i = 0; i < count; i++) {
		char error[FILES_MESSAGE];
		patch_instrument instrument;
		patch_envelope envelope;
		if (place >= PATCH_BANK_SIZE) {
			no_room++;
		} else if (envelopes && read_envelope(sorted[i], &envelope, error, sizeof(error))) {
			bank->envelope_arps[place] = PATCH_ARP_EMPTY;
			bank->envelopes[place++] = envelope;
		} else if (!envelopes && read_instrument(sorted[i], &instrument, error, sizeof(error))) {
			bank->place_arps[place] = PATCH_ARP_EMPTY;
			bank->places[place++] = instrument;
		} else if (unread++ == 0) {
			snprintf(why, sizeof(why), "%s", error);
			first_unread = base_of(sorted[i]);
		}
	}
	const int loaded = place - first;
	// Stage 49 (ui-concept.md): files with no place, past 128 or past the bank's end, are extra;
	// those not read are counted apart, the first one named when no file is extra.
	const int extra = beyond + no_room;
	if (loaded == 0) {
		if (first >= PATCH_BANK_SIZE) {
			snprintf(message, size, envelopes ? "Envelopes full to 128" : "Bank is full to 128");
		} else if (count == 1 && !beyond) {
			snprintf(message, size, "%s", why);
		} else {
			snprintf(message, size, "Nothing loaded");
		}
	} else if (loaded == 1 && envelopes) {
		const patch_envelope envelope = bank->envelopes[first];
		put_envelope_in_place(files, first, &envelope, NULL);
		snprintf(message, size, "Loaded %s to slot %d", envelope.name, first + 1);
	} else if (loaded == 1) {
		const patch_instrument instrument = bank->places[first];
		put_in_place(files, first, &instrument, NULL);
		snprintf(message, size, "Loaded %s to slot %d", instrument.name, first + 1);
	} else {
		size_t before_size;
		uint8_t *before = state_build(files->host.patch, NULL, &before_size);
		patch_load_bank(files->host.patch, bank);
		if (before) {
			remember(files, UNDO_STATE, FILES_BANK, NULL, NULL, before, before_size);
			snprintf(files->now.text, sizeof(files->now.text), envelopes ? "envelopes before %d files" : "bank before %d files", loaded);
		}
		files->host.handed(files->host.context, false);
		snprintf(message, size, "Loaded %d to slots %d-%d", loaded, first + 1, place);
	}
	if (loaded > 0 || (first < PATCH_BANK_SIZE && (count > 1 || beyond))) {
		size_t used = strlen(message);
		if (extra && used < size) {
			snprintf(message + used, size - used, ", %d extra", extra);
			used = strlen(message);
		}
		if (unread && used < size) {
			snprintf(message + used, size - used, extra ? ", %d unread" : ", %d unread: %s", unread, first_unread);
		}
	}
	if (skipped) {
		*skipped = unread + extra;
	}
	free(sorted);
	free(bank);
	return loaded;
}

bool files_save_path(const char *chosen, int type, bool envelope, char *out, size_t size, int *kind)
{
	// The name's extension says what the file is when it is ours, since loading goes by it; without
	// one, the chosen type's extension is added.
	const int KINDS[3] = {envelope ? FILES_ENVELOPE : FILES_INSTRUMENT, FILES_BANK, FILES_SNAPSHOT};
	const char *const EXTENSIONS[3] = {envelope ? ".eef" : ".vgi", ".mdbank", ".mdsnap"};
	type = type >= 0 && type <= 2 ? type : 1;
	const char *dot = strrchr(base_of(chosen), '.');
	const bool ours = dot && (!_stricmp(dot, EXTENSIONS[0]) || files_kind(chosen) == FILES_BANK || files_kind(chosen) == FILES_SNAPSHOT);
	snprintf(out, size, "%s%s", chosen, ours ? "" : EXTENSIONS[type]);
	*kind = ours ? files_kind(out) : KINDS[type];
	// Stage 49: "Bass v1.2" looks to Windows as if it had an extension, so the dialog asked about
	// that name, not the one written.
	FILE *there = ours ? NULL : open_utf8(out, L"rb");
	if (there) {
		fclose(there);
	}
	return there != NULL;
}

bool files_save(mdmi_files *files, const char *path, int kind, int channel, char *message, size_t size)
{
	const char *name = base_of(path);
	bool written;
	if (kind == FILES_ENVELOPE) {
		if (!is_psg(channel)) {
			snprintf(message, size, "Select CH7-10 to save an envelope");
			return false;
		}
		patch_envelope envelope;
		channel_envelope(files->host.patch, channel, &envelope);
		uint8_t eef[PATCH_ENVELOPE_STEPS + 1];
		written = write_all(path, eef, instrument_write_eef(&envelope, eef));
	} else if (kind == FILES_INSTRUMENT) {
		if (channel < 0 || channel >= PATCH_NUM_FM_CHANNELS) {
			snprintf(message, size, "Select CH1-6 to save an instrument");
			return false;
		}
		patch_instrument instrument;
		channel_instrument(files->host.patch, channel, &instrument);
		uint8_t vgi[INSTRUMENT_VGI_SIZE];
		instrument_write_vgi(&instrument, vgi);
		written = write_all(path, vgi, sizeof(vgi));
	} else {
		size_t length;
		uint8_t *bytes = state_build(files->host.patch, kind == FILES_SNAPSHOT ? files->host.view : NULL, &length);
		written = bytes && write_all(path, bytes, length);
		free(bytes);
	}
	snprintf(message, size, written ? "Saved %s" : "%s: can't be written", name);
	return written;
}

// Undo of the samples keeps a whole copy of them as they were.
static void remember_samples(mdmi_files *files, const char *text)
{
	remember(files, UNDO_SAMPLES, 0, NULL, NULL, NULL, 0);
	const dac_set *dac = files->host.view->dac;
	bool whole = true;
	for (int note = 0; note < ROM_IMAGE_NOTES; note++) {
		files->now.dac.notes[note] = dac_copy(dac->notes[note]);
		files->now.dac.removed[note] = dac->removed[note];
		whole = whole && (files->now.dac.notes[note] || !dac->notes[note]);
	}
	snprintf(files->now.text, sizeof(files->now.text), "%s", text);
	// Out of memory: no step, rather than one whose undo would take samples away.
	if (!whole) {
		step_free(&files->now);
	}
}

int files_put_samples(mdmi_files *files, const char *const *paths, int count, int first, char *message, size_t size)
{
	dac_set *dac = files->host.view->dac;
	if (count > ROM_IMAGE_NOTES - first) {
		count = ROM_IMAGE_NOTES - first;
	}
	if (!dac || count <= 0) {
		snprintf(message, size, "No note for the WAV");
		return 0;
	}
	dac_sample *read[ROM_IMAGE_NOTES] = {0};
	uint32_t used = dac_used(dac), sources = dac_sources(dac);
	int loaded = 0;
	for (; loaded < count; loaded++) {
		const int note = first + loaded;
		const char *name = base_of(paths[loaded]);
		size_t length = 0;
		const bool wav = files_kind(paths[loaded]) == FILES_SAMPLE;
		uint8_t *bytes = wav ? read_up_to(paths[loaded], WAV_READ_MAX, &length) : NULL;
		const char *error = !wav ? "not a WAV file" : "can't be read, or larger than 192 MB";
		read[loaded] = bytes ? dac_from_wav(bytes, length, name, &error) : NULL;
		free(bytes);
		if (!read[loaded]) {
			snprintf(message, size, "%s: %s", name, error);
			break;
		}
		if (dac->notes[note]) {
			used -= rom_image_cost(dac->notes[note]->sample.length);
			sources -= dac->notes[note]->source_frames * (uint32_t)sizeof(int16_t);
		}
		used += rom_image_cost(read[loaded]->sample.length);
		sources += read[loaded]->source_frames * (uint32_t)sizeof(int16_t);
		if (used > rom_image_room()) {
			snprintf(message, size, "No room: %.2f MB more than the ROM has", (used - rom_image_room()) / 1048576.0);
			break;
		}
		if (sources > DAC_SOURCE_MAX) {
			snprintf(message, size, "No room: the project keeps 16 MB of sounds");
			break;
		}
	}
	if (loaded < count) {
		for (int i = 0; i <= loaded && i < count; i++) {
			dac_free(read[i]);
		}
		return 0;
	}
	char text[FILES_MESSAGE];
	if (count == 1) {
		char name[48];
		letters(read[0]->name, name, sizeof(name), 40);
		snprintf(text, sizeof(text), "note %d back before %s", first, name);
		snprintf(message, size, "%s > note %d", name, first);
	} else {
		snprintf(text, sizeof(text), "notes %d-%d back before %d WAV", first, first + count - 1, count);
		snprintf(message, size, "%d WAV > notes %d-%d", count, first, first + count - 1);
	}
	remember_samples(files, text);
	for (int i = 0; i < count; i++) {
		dac_put(dac, first + i, read[i]);
	}
	files->host.handed(files->host.context, false);
	return count;
}

static dac_sample *note_content(const dac_set *dac, int note, bool *empty);

bool files_edit_sample(mdmi_files *files, int note, const dac_edit *edit, bool join, char *message, size_t size)
{
	dac_set *dac = files->host.view->dac;
	if (!dac || note < 0 || note >= ROM_IMAGE_NOTES) {
		return false;
	}
	// A sample of the ROM's becomes the project's own at its first change (owner after 0.12.0).
	const dac_sample *now = dac->notes[note];
	bool empty;
	dac_sample *changed = now ? dac_copy(now) : note_content(dac, note, &empty);
	if (!changed || !changed->source) {
		dac_free(changed);
		return false;
	}
	if (!dac_prepare(changed, edit)) {
		dac_free(changed);
		snprintf(message, size, "Note %d: the ROM has no room for that", note);
		return false;
	}
	const uint32_t was = now ? rom_image_cost(now->sample.length) : 0, was_source = now ? now->source_frames * (uint32_t)sizeof(int16_t) : 0;
	if (dac_used(dac) - was + rom_image_cost(changed->sample.length) > rom_image_room()
		|| dac_sources(dac) - was_source + changed->source_frames * (uint32_t)sizeof(int16_t) > DAC_SOURCE_MAX) {
		dac_free(changed);
		snprintf(message, size, "Note %d: the ROM has no room for that", note);
		return false;
	}
	char text[FILES_MESSAGE];
	snprintf(text, sizeof(text), "note %d before the change", note);
	if (!join || files->now.kind != UNDO_SAMPLES || files->ahead_count) {
		remember_samples(files, text);
	}
	dac_put(dac, note, changed);
	snprintf(message, size, "Note %d changed", note);
	files->host.handed(files->host.context, false);
	return true;
}

bool files_reset_sample(mdmi_files *files, int note, char *message, size_t size)
{
	dac_set *dac = files->host.view->dac;
	if (!dac || note < 0 || note >= ROM_IMAGE_NOTES) {
		return false;
	}
	const bool rom_has = ROM_SAMPLE_NAMES[note][0] != 0;
	char text[FILES_MESSAGE];
	if (dac->notes[note]) {
		// A file goes, the ROM's sample or silence comes back.
		char name[48];
		snprintf(text, sizeof(text), "note %d back to %s", note, letters(dac->notes[note]->name, name, sizeof(name), 40));
		remember_samples(files, text);
		if (dac->removed[note]) {
			dac_remove(dac, note);
		} else {
			dac_put(dac, note, NULL);
		}
		snprintf(message, size, rom_has ? "Note %d plays the ROM's %s again" : "Note %d is empty again%s", note, rom_has ? ROM_SAMPLE_NAMES[note] : "");
	} else if (rom_has && !dac->removed[note]) {
		snprintf(text, sizeof(text), "note %d back to %s", note, ROM_SAMPLE_NAMES[note]);
		remember_samples(files, text);
		dac_remove(dac, note);
		snprintf(message, size, "Note %d is empty: %s removed", note, ROM_SAMPLE_NAMES[note]);
	} else if (rom_has) {
		snprintf(text, sizeof(text), "note %d empty again", note);
		remember_samples(files, text);
		dac_put(dac, note, NULL);
		snprintf(message, size, "Note %d plays the ROM's %s again", note, ROM_SAMPLE_NAMES[note]);
	} else {
		return false;
	}
	files->host.handed(files->host.context, false);
	return true;
}

// What a note plays, as a sample that can go to another note: its file's, a copy of the ROM's, or
// none. *empty says the note plays nothing.
static dac_sample *note_content(const dac_set *dac, int note, bool *empty)
{
	*empty = false;
	if (dac->notes[note]) {
		return dac_copy(dac->notes[note]);
	}
	size_t size;
	const uint8_t *rom = mdmi_engine_builtin_rom(&size);
	rom_sample sample;
	if (dac->removed[note] || !rom_image_sample(rom, size, note, &sample)) {
		*empty = true;
		return NULL;
	}
	return dac_from_rom(&sample, ROM_SAMPLE_NAMES[note]);
}

// A note gets what another played, or nothing: then empty, the ROM's sample there removed.
static void put_content(dac_set *dac, int note, dac_sample *sample)
{
	if (sample) {
		dac_put(dac, note, sample);
	} else if (ROM_SAMPLE_NAMES[note][0]) {
		dac_remove(dac, note);
	} else {
		dac_put(dac, note, NULL);
	}
}

static uint32_t cost_of(const dac_sample *sample)
{
	return sample ? rom_image_cost(sample->sample.length) : 0;
}

static uint32_t source_of(const dac_sample *sample)
{
	return sample ? sample->source_frames * (uint32_t)sizeof(int16_t) : 0;
}

int files_move_sample(mdmi_files *files, int from, int to, bool copy, char *message, size_t size)
{
	dac_set *dac = files->host.view->dac;
	if (!dac || from < 0 || to < 0 || from >= ROM_IMAGE_NOTES || to >= ROM_IMAGE_NOTES || from == to) {
		return 0;
	}
	bool from_empty, to_empty = false;
	dac_sample *moving = note_content(dac, from, &from_empty);
	dac_sample *back = copy ? NULL : note_content(dac, to, &to_empty);
	if (!moving || (!copy && !back && !to_empty)) {
		dac_free(moving);
		dac_free(back);
		return 0;
	}
	// Room: the two notes' own samples after, against now. Swapped, the note left gets what the
	// other played, a copy of the ROM's included; copied, it keeps its own.
	const dac_sample *from_after = copy ? dac->notes[from] : back;
	const uint32_t used = dac_used(dac) - cost_of(dac->notes[from]) - cost_of(dac->notes[to]) + cost_of(from_after) + cost_of(moving);
	const uint32_t sources = dac_sources(dac) - source_of(dac->notes[from]) - source_of(dac->notes[to]) + source_of(from_after) + source_of(moving);
	if (used > rom_image_room() || sources > DAC_SOURCE_MAX) {
		dac_free(moving);
		dac_free(back);
		snprintf(message, size, "No room in the ROM for that");
		return 0;
	}
	char text[FILES_MESSAGE];
	snprintf(text, sizeof(text), copy ? "note %d before the copy" : "notes %d and %d before the swap", to, from);
	remember_samples(files, text);
	char name[48];
	snprintf(message, size, copy ? "%s copied from %d to %d" : "%s from %d swapped with %d", letters(moving->name, name, sizeof(name), 30), from, to);
	put_content(dac, to, moving);
	if (!copy) {
		put_content(dac, from, back);
	}
	files->host.handed(files->host.context, false);
	return 1;
}

// A whole file written beside the old one, then put in its place, so a write that fails halfway
// leaves the old file as it was.
static bool write_replace(const char *path, const uint8_t *bytes, size_t size)
{
	char temporary[LIBRARY_PATH + 8];
	snprintf(temporary, sizeof(temporary), "%s.tmp", path);
	wchar_t from[LIBRARY_PATH + 8], to[LIBRARY_PATH];
	if (!MultiByteToWideChar(CP_UTF8, 0, temporary, -1, from, LIBRARY_PATH + 8) || !MultiByteToWideChar(CP_UTF8, 0, path, -1, to, LIBRARY_PATH)) {
		return false;
	}
	if (!write_all(temporary, bytes, size)) {
		DeleteFileW(from);
		return false;
	}
	if (!MoveFileExW(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
		DeleteFileW(from);
		return false;
	}
	return true;
}

// The name a library goes by: its file's, without .mdbank.
static void library_name(const char *path, char *out, size_t size)
{
	const char *base = base_of(path);
	const char *dot = strrchr(base, '.');
	snprintf(out, size, "%.*s", dot && dot != base ? (int)(dot - base) : (int)strlen(base), base);
}

bool files_read_library(const char *path, patch_bank *bank)
{
	size_t length = 0;
	uint8_t *bytes = read_all(path, &length);
	const bool read = bytes && state_bank_read(bytes, length, bank);
	free(bytes);
	return read;
}

static bool write_library(const char *path, const patch_bank *bank)
{
	size_t length;
	uint8_t *bytes = state_bank_build(bank, &length);
	const bool written = bytes && write_replace(path, bytes, length);
	free(bytes);
	return written;
}

// What a place holds: the voice or the envelope by the list, and the row.
typedef struct {
	patch_instrument instrument;
	patch_envelope envelope;
	patch_arp arp;
} place_sound;

static void get_place(const patch_bank *bank, bool envelopes, int place, place_sound *out)
{
	memset(out, 0, sizeof(*out));
	if (envelopes) {
		out->envelope = bank->envelopes[place];
		out->arp = bank->envelope_arps[place];
	} else {
		out->instrument = bank->places[place];
		out->arp = bank->place_arps[place];
	}
}

static void set_place(patch_bank *bank, bool envelopes, int place, const place_sound *sound)
{
	if (envelopes) {
		bank->envelopes[place] = sound->envelope;
		bank->envelope_arps[place] = sound->arp;
	} else {
		bank->places[place] = sound->instrument;
		bank->place_arps[place] = sound->arp;
	}
}

bool files_place_empty(const patch_bank *bank, bool envelopes, int place)
{
	place &= PATCH_BANK_SIZE - 1;
	const patch_arp *arp = envelopes ? &bank->envelope_arps[place] : &bank->place_arps[place];
	return arp->count == 0 && (envelopes ? patch_envelope_is_rom(bank, place) : patch_place_is_gm(bank, place));
}

static const char *sound_name(const place_sound *sound, bool envelopes)
{
	return envelopes ? sound->envelope.name : sound->instrument.name;
}

static bool read_spot(mdmi_files *files, bool envelopes, const files_spot *spot, place_sound *out, char *message, size_t size)
{
	if (spot->place < 0 || spot->place >= PATCH_BANK_SIZE) {
		snprintf(message, size, "No such slot");
		return false;
	}
	if (!spot->library) {
		get_place(patch_bank_view(files->host.patch), envelopes, spot->place, out);
		return true;
	}
	patch_bank *bank = malloc(sizeof(*bank));
	if (!bank || !files_read_library(spot->library, bank)) {
		char name[LIBRARY_NAME];
		library_name(spot->library, name, sizeof(name));
		snprintf(message, size, "%s: can't be read as a bank", name);
		free(bank);
		return false;
	}
	get_place(bank, envelopes, spot->place, out);
	free(bank);
	return true;
}

// The library file read again, one place changed, the file written whole. before gets what the
// place had. Nothing changes if the file can't be read as a bank or written.
// *place < 0: the first empty place of the fresh file where the sound doesn't read as empty (a GM
// sound on its own GM place would), set to where it went.
static bool write_library_place(const char *path, bool envelopes, int *place, const place_sound *sound, place_sound *before, bool *was_empty, char *message, size_t size)
{
	char name[LIBRARY_NAME];
	library_name(path, name, sizeof(name));
	patch_bank *bank = malloc(sizeof(*bank));
	if (!bank || !files_read_library(path, bank)) {
		snprintf(message, size, "%s: can't be read as a bank, not changed", name);
		free(bank);
		return false;
	}
	for (int free_place = 0; *place < 0 && free_place < PATCH_BANK_SIZE; free_place++) {
		if (!files_place_empty(bank, envelopes, free_place)) {
			continue;
		}
		get_place(bank, envelopes, free_place, before);
		set_place(bank, envelopes, free_place, sound);
		if (!files_place_empty(bank, envelopes, free_place)) {
			*place = free_place;
		}
		set_place(bank, envelopes, free_place, before);
	}
	if (*place < 0) {
		char shown[48];
		snprintf(message, size, "%s is full to 128: clear a sound first", letters(name, shown, sizeof(shown), 40));
		free(bank);
		return false;
	}
	get_place(bank, envelopes, *place, before);
	*was_empty = files_place_empty(bank, envelopes, *place);
	set_place(bank, envelopes, *place, sound);
	const bool written = write_library(path, bank);
	free(bank);
	if (!written) {
		snprintf(message, size, "Can't write %s: read-only or in use, nothing changed", name);
	}
	return written;
}

static void remember_library(mdmi_files *files, bool envelopes, const char *path, int place, const place_sound *before, bool was_empty)
{
	remember(files, UNDO_LIBRARY, place, envelopes ? NULL : &before->instrument, envelopes ? &before->envelope : NULL, NULL, 0);
	files->now.arp = before->arp;
	files->now.envelopes = envelopes;
	snprintf(files->now.library, sizeof(files->now.library), "%s", path);
	char name[LIBRARY_NAME];
	library_name(path, name, sizeof(name));
	// Stage 37: a place that was empty goes back to empty, not to the GM sound it holds then (22c).
	char shown[48];
	snprintf(files->now.text, sizeof(files->now.text), "%s %d back to %s", letters(name, shown, sizeof(shown), 40), place + 1, was_empty ? "empty" : sound_name(before, envelopes));
}

// A sound into a place of the ROM bank or of a library, with its undo step.
// A library's place < 0 is its first free one; *landed says where the sound went.
static bool put_spot(mdmi_files *files, bool envelopes, const files_spot *to, const place_sound *sound, int *landed, char *message, size_t size)
{
	*landed = to->place;
	if (!to->library) {
		if (envelopes) {
			put_envelope_in_place(files, to->place, &sound->envelope, &sound->arp);
		} else {
			put_in_place(files, to->place, &sound->instrument, &sound->arp);
		}
		return true;
	}
	place_sound before;
	bool was_empty;
	if (!write_library_place(to->library, envelopes, landed, sound, &before, &was_empty, message, size)) {
		return false;
	}
	remember_library(files, envelopes, to->library, *landed, &before, was_empty);
	return true;
}

static void spot_name(const files_spot *spot, char *out, size_t size)
{
	char name[LIBRARY_NAME];
	if (spot->library) {
		library_name(spot->library, name, sizeof(name));
		char shown[48];
		snprintf(out, size, "%s %d", letters(name, shown, sizeof(shown), 40), spot->place + 1);
	} else {
		snprintf(out, size, "bank %d", spot->place + 1);
	}
}

bool files_new_library(const char *path, char *message, size_t size)
{
	char name[LIBRARY_NAME];
	library_name(path, name, sizeof(name));
	patch_bank *bank = calloc(1, sizeof(*bank));
	if (bank) {
		patch_bank_rom(bank);
	}
	const bool written = bank && write_library(path, bank);
	free(bank);
	snprintf(message, size, written ? "New library %s" : "%s: can't be written", name);
	return written;
}

// The target's loop shadow and kept release for the source's (or undo's), the target's kept in their place.
static void swap_view(mdmi_files *files, int channel, const state_loop *loop, const state_tail *tail)
{
	state_view *view = files->host.view;
	const int psg = channel - PATCH_NUM_FM_CHANNELS;
	const state_loop old_loop = view->loops[psg];
	const state_tail old_tail = view->tails[psg];
	view->loops[psg] = *loop;
	view->tails[psg] = *tail;
	files->now.view = true;
	files->now.loop = old_loop;
	files->now.tail = old_tail;
}

bool files_copy_channel(mdmi_files *files, int from, int to, char *message, size_t size)
{
	if (from < 0 || to < 0 || from >= PATCH_NUM_CHANNELS || to >= PATCH_NUM_CHANNELS || from == to) {
		snprintf(message, size, "Drop on another channel");
		return false;
	}
	if (is_psg(from) != is_psg(to)) {
		snprintf(message, size, is_psg(from) ? "PSG goes to PSG 7-10" : "FM goes to FM 1-6");
		return false;
	}
	// Channel 6 plays the DAC's samples while it is on: no voice to give or take (D87).
	const int dac = PATCH_NUM_FM_CHANNELS - 1;
	if ((from == dac || to == dac) && patch_view(files->host.patch)->globals[GLOBAL_DAC]) {
		snprintf(message, size, "CH6 plays the DAC");
		return false;
	}
	patch_instrument base;
	patch_envelope base_envelope;
	patch_base_view(files->host.patch, from, &base, &base_envelope);
	patch_arp arp = *patch_arp_view(files->host.patch, from), arp_base;
	patch_arp_base_view(files->host.patch, from, &arp_base);
	if (is_psg(to)) {
		patch_envelope envelope;
		channel_envelope(files->host.patch, from, &envelope);
		put_envelope_on_channel(files, to, &envelope, &base_envelope, &arp, &arp_base);
		if (files->host.view) {
			const int psg = from - PATCH_NUM_FM_CHANNELS;
			const state_loop loop = files->host.view->loops[psg];
			const state_tail tail = files->host.view->tails[psg];
			swap_view(files, to, &loop, &tail);
		}
	} else {
		patch_instrument instrument;
		channel_instrument(files->host.patch, from, &instrument);
		put_on_channel(files, to, &instrument, &base, &arp, &arp_base);
	}
	snprintf(message, size, "CH%d now plays CH%d's sound", to + 1, from + 1);
	return true;
}

bool files_try_place(mdmi_files *files, int channel, const files_spot *from, char *message, size_t size)
{
	if (channel < 0 || channel >= PATCH_NUM_CHANNELS) {
		return false;
	}
	const bool envelopes = is_psg(channel);
	place_sound sound;
	if (!read_spot(files, envelopes, from, &sound, message, size)) {
		return false;
	}
	if (envelopes) {
		put_envelope_on_channel(files, channel, &sound.envelope, &sound.envelope, &sound.arp, &sound.arp);
	} else {
		put_on_channel(files, channel, &sound.instrument, &sound.instrument, &sound.arp, &sound.arp);
	}
	snprintf(message, size, "Trying %s on CH%d", sound_name(&sound, envelopes), channel + 1);
	return true;
}

bool files_copy_place(mdmi_files *files, bool envelopes, const files_spot *from, const files_spot *to, int *landed, char *message, size_t size)
{
	if (to->place >= PATCH_BANK_SIZE || (to->place < 0 && !to->library)) {
		snprintf(message, size, "No such slot");
		return false;
	}
	const bool same_bank = from->library && to->library ? !_stricmp(from->library, to->library) : !from->library && !to->library;
	if (same_bank && (from->place == to->place || to->place < 0)) {
		snprintf(message, size, "Same slot");
		return false;
	}
	place_sound sound;
	int went;
	if (!read_spot(files, envelopes, from, &sound, message, size) || !put_spot(files, envelopes, to, &sound, &went, message, size)) {
		return false;
	}
	if (landed) {
		*landed = went;
	}
	char where[64];
	const files_spot there = {to->library, went};
	spot_name(&there, where, sizeof(where));
	snprintf(message, size, "%s to %s", sound_name(&sound, envelopes), where);
	return true;
}

bool files_clear_place(mdmi_files *files, bool envelopes, const files_spot *spot, char *message, size_t size)
{
	if (!spot->library || spot->place < 0 || spot->place >= PATCH_BANK_SIZE) {
		return false;
	}
	patch_bank *rom = calloc(1, sizeof(*rom));
	if (!rom) {
		return false;
	}
	patch_bank_rom(rom);
	place_sound empty;
	get_place(rom, envelopes, spot->place, &empty);
	free(rom);
	int landed;
	if (!put_spot(files, envelopes, spot, &empty, &landed, message, size)) {
		return false;
	}
	char where[64];
	spot_name(spot, where, sizeof(where));
	snprintf(message, size, "%s emptied", where);
	return true;
}

// Stage 48 (D90): one channel's part of the values, its Preset with it.
static void copy_channel(patch_values *to, const patch_values *from, int channel)
{
	memcpy(to->channels[channel], from->channels[channel], sizeof(to->channels[channel]));
	if (is_psg(channel)) {
		to->psg[channel - PATCH_NUM_FM_CHANNELS] = from->psg[channel - PATCH_NUM_FM_CHANNELS];
		to->psg_base[channel - PATCH_NUM_FM_CHANNELS] = from->psg_base[channel - PATCH_NUM_FM_CHANNELS];
	} else {
		memcpy(to->names[channel], from->names[channel], PATCH_NAME);
		memcpy(to->base[channel], from->base[channel], sizeof(to->base[channel]));
	}
	to->arps[channel] = from->arps[channel];
	to->arp_bases[channel] = from->arp_bases[channel];
}

static bool same_channel(const patch_values *a, const patch_values *b, int channel)
{
	if (!a || !b) {
		return false;
	}
	patch_values *mixed = malloc(sizeof(*mixed));
	if (!mixed) {
		return false;
	}
	*mixed = *a;
	copy_channel(mixed, b, channel);
	const bool same = !memcmp(mixed, a, sizeof(*a));
	free(mixed);
	return same;
}

static const char *channel_name(const patch_values *values, int channel)
{
	return is_psg(channel) ? values->psg[channel - PATCH_NUM_FM_CHANNELS].name : values->names[channel];
}

// The values as they are, to keep in a Preset step.
static patch_values *values_now(mdmi_files *files)
{
	patch_values *values = malloc(sizeof(*values));
	if (values) {
		*values = *patch_view(files->host.patch);
	}
	return values;
}

// A Preset step's record: the values, and on PSG the loop shadow and kept release.
static void remember_preset(mdmi_files *files, int channel, patch_values *values)
{
	remember(files, UNDO_PRESET, channel, NULL, NULL, NULL, 0);
	files->now.values = values;
	if (is_psg(channel) && files->host.view) {
		files->now.view = true;
		files->now.loop = files->host.view->loops[channel - PATCH_NUM_FM_CHANNELS];
		files->now.tail = files->host.view->tails[channel - PATCH_NUM_FM_CHANNELS];
	}
	snprintf(files->now.text, sizeof(files->now.text), "CH%d back to %s", channel + 1, channel_name(values, channel));
}

void files_preset_step(mdmi_files *files, int channel, int place)
{
	if (channel < 0 || channel >= PATCH_NUM_CHANNELS) {
		return;
	}
	const patch_values *values = patch_view(files->host.patch);
	files_step *run = &files->now;
	// The next step of a run: the channel plays, untouched, a place the run's steps went to and not
	// one before the last it was seen on, or no step has reached the model yet (a fast wheel). On any
	// other place the host stepped it (stage 50): undo goes back to where the host put it, not over it.
	if (run->kind == UNDO_PRESET && run->target == channel && !files->ahead_count) {
		int seen = -1;
		if (!patch_sound_changed(values, is_psg(channel) ? &values->psg[channel - PATCH_NUM_FM_CHANNELS] : NULL, channel)) {
			for (int k = 0; k < run->went_count && seen < 0; k++) {
				seen = run->went[k] == values->channels[channel][PARAM_PRESET] ? k : -1;
			}
		}
		if (seen >= 0 || (!run->went_seen && same_channel(values, run->values, channel))) {
			if (seen >= 0) {
				memmove(run->went, run->went + seen, (size_t)(run->went_count - seen) * sizeof(run->went[0]));
				run->went_count -= seen;
				run->went_seen = true;
			}
			if (run->went_count == FILES_RUN_WENT) {
				memmove(run->went, run->went + 1, (size_t)(FILES_RUN_WENT - 1) * sizeof(run->went[0]));
				run->went_count--;
			}
			run->went[run->went_count++] = (int16_t)place;
			return;
		}
	}
	patch_values *before = values_now(files);
	if (before) {
		remember_preset(files, channel, before);
		files->now.went[0] = (int16_t)place;
		files->now.went_count = 1;
	}
}

// What a step changes, done, leaving in now what it replaced: the step that takes it back.
static bool apply_step(mdmi_files *files, char *message, size_t size)
{
	const int undo = files->now.kind, target = files->now.target;
	const patch_arp arp = files->now.arp, base_arp = files->now.base_arp;
	const int store_channel = files->now.channel;
	if (undo == UNDO_ARP) {
		// The row alone, the one now kept for redo.
		const patch_arp now = *patch_arp_view(files->host.patch, target);
		patch_load_arp(files->host.patch, target, &arp, NULL);
		remember(files, UNDO_ARP, target, NULL, NULL, NULL, 0);
		files->now.arp = now;
		snprintf(files->now.text, sizeof(files->now.text), "CH%d arp as drawn", target + 1);
		files->host.handed(files->host.context, false);
	} else if (undo == UNDO_LIBRARY) {
		// The place as it was written back, and what it has now kept for redo.
		const bool envelopes = files->now.envelopes;
		place_sound back = {.instrument = files->now.instrument, .envelope = files->now.envelope, .arp = arp}, now;
		char path[LIBRARY_PATH];
		snprintf(path, sizeof(path), "%s", files->now.library);
		char why[FILES_MESSAGE];
		int place = target;
		bool now_empty;
		if (!write_library_place(path, envelopes, &place, &back, &now, &now_empty, why, sizeof(why))) {
			char name[LIBRARY_NAME];
			library_name(path, name, sizeof(name));
			wchar_t wide[LIBRARY_PATH];
			const bool gone = MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, LIBRARY_PATH) && GetFileAttributesW(wide) == INVALID_FILE_ATTRIBUTES;
			if (gone) {
				char shown[48];
				snprintf(message, size, "Can't undo: %s is gone", letters(name, shown, sizeof(shown), 40));
			} else {
				snprintf(message, size, "Can't undo: %s", why);
			}
			return false;
		}
		remember_library(files, envelopes, path, target, &now, now_empty);
	} else if (undo == UNDO_CHANNEL && is_psg(target)) {
		patch_envelope back = files->now.envelope, base = files->now.base_envelope;
		const bool view = files->now.view && files->host.view;
		const state_loop loop = files->now.loop;
		const state_tail tail = files->now.tail;
		const int drawing = files->now.drawing;
		put_envelope_on_channel(files, target, &back, &base, &arp, &base_arp);
		// A copy's loop shadow and kept release go back with it (stage 43), and a drawing's (stage 50).
		if (view) {
			swap_view(files, target, &loop, &tail);
		}
		// What the step left puts back the drawing, or again the envelope before it (stage 50).
		if (drawing) {
			files->now.drawing = 3 - drawing;
			snprintf(files->now.text, sizeof(files->now.text), drawing == 1 ? "CH%d envelope as drawn" : "CH%d envelope before drawing", target + 1);
		}
	} else if (undo == UNDO_CHANNEL) {
		patch_instrument back = files->now.instrument, base = files->now.base_instrument;
		put_on_channel(files, target, &back, &base, &arp, &base_arp);
	} else if ((undo == UNDO_PLACE || undo == UNDO_ENVELOPE_PLACE) && store_channel >= 0
		&& (patch_view(files->host.patch)->channels[store_channel][PARAM_PRESET] & (PATCH_BANK_SIZE - 1)) == target) {
		// STORE: the place and the channel's base go back together, the channel keeps its sound.
		patch_instrument back = files->now.instrument, base = files->now.base_instrument;
		patch_envelope back_envelope = files->now.envelope, base_envelope = files->now.base_envelope;
		swap_place(files, store_channel, target, &back, &back_envelope, &arp, &base, &base_envelope, &base_arp);
	} else if (undo == UNDO_ENVELOPE_PLACE) {
		patch_envelope back = files->now.envelope;
		put_envelope_in_place(files, target, &back, &arp);
	} else if (undo == UNDO_PLACE) {
		patch_instrument back = files->now.instrument;
		put_in_place(files, target, &back, &arp);
	} else if (undo == UNDO_PRESET) {
		// The channel's part of the values before the step, as one whole: the host reads its Preset
		// again (D19), and the sound never plays the place in between.
		patch_values *back = files->now.values, *next = values_now(files), *was = values_now(files);
		files->now.values = NULL;
		const bool view = files->now.view && files->host.view;
		const state_loop loop = files->now.loop;
		const state_tail tail = files->now.tail;
		if (!back || !next || !was) {
			free(back);
			free(next);
			free(was);
			files->now.kind = UNDO_NONE;
			return false;
		}
		copy_channel(next, back, target);
		patch_load_values(files->host.patch, next);
		remember_preset(files, target, was);
		// The run goes on from the place the step put back, as if its steps had come there.
		files->now.went[0] = next->channels[target][PARAM_PRESET];
		files->now.went_count = 1;
		files->now.went_seen = true;
		if (view) {
			swap_view(files, target, &loop, &tail);
		}
		free(back);
		free(next);
		files->host.handed(files->host.context, true);
	} else if (undo == UNDO_SAMPLES) {
		// The samples before and now trade places, so redo brings the change back.
		dac_set *dac = files->host.view->dac;
		for (int note = 0; note < ROM_IMAGE_NOTES; note++) {
			dac_sample *now = dac->notes[note];
			dac->notes[note] = files->now.dac.notes[note];
			files->now.dac.notes[note] = now;
			const bool removed = dac->removed[note];
			dac->removed[note] = files->now.dac.removed[note];
			files->now.dac.removed[note] = removed;
		}
		dac->version++;
		files->host.handed(files->host.context, false);
	} else {
		uint8_t *back = files->now.state;
		files->now.state = NULL;
		bool done = put_state(files, target, back, files->now.state_size);
		free(back);
		if (!done) {
			files->now.kind = UNDO_NONE;
			return false;
		}
	}
	return true;
}

// Stage 48 (D90): UNDO applies the step on top and keeps what it left for redo; the step under it
// comes up. Redo the other way round.
bool files_undo(mdmi_files *files, char *message, size_t size)
{
	if (files->now.kind == UNDO_NONE) {
		return false;
	}
	char text[FILES_MESSAGE];
	snprintf(text, sizeof(text), "%s", files->now.text);
	files->replaying = true;
	const bool done = apply_step(files, message, size);
	files->replaying = false;
	if (done) {
		snprintf(message, size, "Undone: %s", text);
		push_step(files->ahead, &files->ahead_count, FILES_UNDO_DEPTH, &files->now);
	}
	if (done || files->now.kind == UNDO_NONE) {
		step_free(&files->now);
		pop_step(files->back, &files->back_count, &files->now);
	}
	return done;
}

bool files_redo(mdmi_files *files, char *message, size_t size)
{
	if (!files->ahead || !files->ahead_count) {
		return false;
	}
	files_step step;
	pop_step(files->ahead, &files->ahead_count, &step);
	char text[FILES_MESSAGE];
	snprintf(text, sizeof(text), "%s", step.text);
	if (files->now.kind != UNDO_NONE) {
		push_step(files->back, &files->back_count, FILES_UNDO_DEPTH - 1, &files->now);
	}
	step_free(&files->now);
	files->now = step;
	files->replaying = true;
	const bool done = apply_step(files, message, size);
	files->replaying = false;
	if (done) {
		snprintf(message, size, "Redone: %s", text);
	} else {
		// Not done (a library gone or locked): the step stays ahead, and the one before comes back.
		if (files->now.kind != UNDO_NONE) {
			push_step(files->ahead, &files->ahead_count, FILES_UNDO_DEPTH, &files->now);
		}
		step_free(&files->now);
		pop_step(files->back, &files->back_count, &files->now);
	}
	return done;
}

const char *files_undo_text(const mdmi_files *files)
{
	return files->now.kind == UNDO_NONE ? NULL : files->now.text;
}

const char *files_redo_text(const mdmi_files *files)
{
	return files->ahead && files->ahead_count ? files->ahead[files->ahead_count - 1].text : NULL;
}

// Stage 24. The ROM bank in the fork's tables: an envelope as it plays (the bank holds no shadow or
// kept release, D30, D54), a row switched off as no row (D55).
static void rom_bank_of(const patch_bank *bank, rom_bank *out)
{
	memset(out, 0, sizeof(*out));
	for (int place = 0; place < PATCH_BANK_SIZE; place++) {
		patch_voice_fields(bank->places[place].voice, out->voices[place]);
		memcpy(out->envelopes[place], bank->envelopes[place].steps, ROM_ENVELOPE_BYTES);
		for (int psg = 0; psg < 2; psg++) {
			const patch_arp *arp = patch_arp_played(psg ? &bank->envelope_arps[place] : &bank->place_arps[place]);
			rom_arp *row = &out->arps[psg][place];
			row->count = arp->count;
			row->loop = arp->loop < 0 ? ROM_ARP_NO_LOOP : (uint8_t)arp->loop;
			memcpy(row->steps, arp->steps, sizeof(row->steps));
		}
	}
}

// A name as the ROM's screen shows it: ASCII, cut to the row.
#define SCREEN_NAME_CHARS 23
static void screen_name(const char *name, char *out)
{
	char ascii[PATCH_NAME * 2];
	names_ascii(name, ascii, sizeof(ascii));
	memset(out, 0, ROM_NAME_BYTES);
	memcpy(out, ascii, strnlen(ascii, SCREEN_NAME_CHARS));
}

static void bank_names(const patch_bank *bank, rom_names *names)
{
	memset(names, 0, sizeof(*names));
	for (int place = 0; place < PATCH_BANK_SIZE; place++) {
		screen_name(bank->places[place].name, names->names[0][place]);
		screen_name(bank->envelopes[place].name, names->names[1][place]);
	}
}

// The image for the SD card before rom_image_for_sd, with its signature written. NULL with a reason.
static uint8_t *export_image(mdmi_files *files, size_t *built, uint16_t *signature, const char **why)
{
	const patch_bank *bank = patch_bank_view(files->host.patch);
	rom_bank *tables = malloc(sizeof(*tables));
	rom_names *names = malloc(sizeof(*names));
	uint8_t *image = NULL;
	*why = "out of memory";
	if (tables && names) {
		rom_bank_of(bank, tables);
		bank_names(bank, names);
		size_t size;
		const uint8_t *rom = mdmi_engine_builtin_rom(&size);
		const dac_set *dac = files->host.view->dac;
		const rom_sample *samples[ROM_IMAGE_NOTES];
		for (int note = 0; note < ROM_IMAGE_NOTES; note++) {
			samples[note] = dac->notes[note] ? &dac->notes[note]->sample : NULL;
		}
		image = rom_image_build(rom, size, samples, dac->removed, tables, names, built);
		if (!image) {
			*why = rom_image_check(rom, size) ? "over 4 MB with the samples" : "the ROM's tables are not the plugin's";
		} else {
			*signature = rom_image_signature(image, *built);
			char text[ROM_SIGNATURE_BYTES + 1];
			snprintf(text, sizeof(text), "BANK %04X", *signature);
			memcpy(image + rom_image_names_at(), text, strlen(text));
		}
	}
	free(tables);
	free(names);
	return image;
}

// The end of a path in at most chars of its transliteration, by whole letters (stage 60).
static void path_end(const char *path, char *out, size_t size, int chars)
{
	char tail[MAX_PATH * 3];
	out[0] = 0;
	for (size_t at = strlen(path); at-- > 0;) {
		if (((unsigned char)path[at] & 0xC0) == 0x80) {
			continue;
		}
		names_ascii(path + at, tail, sizeof(tail));
		if ((int)strlen(tail) > chars) {
			break;
		}
		snprintf(out, size, "%s", tail);
	}
}

bool files_export(mdmi_files *files, const char *path, int chars, char *message, size_t size)
{
	size_t built;
	uint16_t signature;
	const char *why;
	uint8_t *image = export_image(files, &built, &signature, &why);
	if (image && !rom_image_for_sd(&image, &built)) {
		why = "out of memory";
		free(image);
		image = NULL;
	}
	if (!image) {
		snprintf(message, size, "NOT WRITTEN: %s", why);
		return false;
	}
	const bool written = write_replace(path, image, built);
	free(image);
	if (!written) {
		snprintf(message, size, "NOT WRITTEN: %s can't be written", base_of(path));
		return false;
	}
	// Channels the file can't play by their program numbers: a sound the bank doesn't hold.
	char channels[64] = "";
	int count = 0, last = -1;
	for (int channel = 0; channel < PATCH_NUM_CHANNELS; channel++) {
		if (files_can_store(files, channel)) {
			const size_t at = strlen(channels);
			snprintf(channels + at, sizeof(channels) - at, "%sCH%d", count ? " " : "", channel + 1);
			count++;
			last = channel;
		}
	}
	const double mb = built / 1048576.0;
	// Stage 60: the footer's chars hold the head and the path's end, the file's name whole if it can.
	// The head gets shorter by steps until it does: the sound's name goes, then the channels' list.
	if (chars > (int)size - 1) {
		chars = (int)size - 1;
	}
	char shown[MAX_PATH * 3];
	names_ascii(path, shown, sizeof(shown));
	const int length = (int)strlen(shown), wanted = (int)strlen(base_of(shown)) + 4;
	char heads[2][FILES_MESSAGE];
	int steps = 1;
	if (count == 1) {
		const patch_values *values = patch_view(files->host.patch);
		char name[24];
		letters(is_psg(last) ? patch_envelope_view(files->host.patch, last)->name : values->names[last], name, sizeof(name), 20);
		snprintf(heads[0], sizeof(heads[0]), "%s %s not in ROM. %.1f MB: ", channels, name, mb);
		snprintf(heads[1], sizeof(heads[1]), "%s not in ROM. %.1f MB: ", channels, mb);
		steps = 2;
	} else if (count) {
		snprintf(heads[0], sizeof(heads[0]), "%s not in ROM. %.1f MB: ", channels, mb);
		snprintf(heads[1], sizeof(heads[1]), "%d CH not in ROM. %.1f MB: ", count, mb);
		steps = 2;
	} else {
		snprintf(heads[0], sizeof(heads[0]), "ROM %.1f MB: ", mb);
	}
	int step = 0;
	while (step < steps - 1 && (int)strlen(heads[step]) + (length < wanted ? length : wanted) > chars) {
		step++;
	}
	const char *head = heads[step];
	// The path is cut at its start, so its end, the file's name, shows.
	const int room = chars - (int)strlen(head);
	if (length > room && room > 3) {
		char end[FILES_MESSAGE];
		path_end(path, end, sizeof(end), room - 3);
		snprintf(message, size, "%s...%s", head, end);
	} else {
		snprintf(message, size, "%s%s", head, shown);
	}
	return true;
}

bool files_screen_names(mdmi_files *files, rom_names *names)
{
	const patch_bank *bank = patch_bank_view(files->host.patch);
	const dac_set *dac = files->host.view->dac;
	if (!files->signed_bank || memcmp(files->signed_bank, bank, sizeof(*bank)) || files->signed_dac != dac->version) {
		size_t built;
		uint16_t signature = 0;
		const char *why;
		uint8_t *image = export_image(files, &built, &signature, &why);
		const bool exported = image != NULL;
		free(image);
		if (!files->signed_bank && !(files->signed_bank = malloc(sizeof(*bank)))) {
			return false;
		}
		// A bank EXPORT refuses has no signature, and isn't tried again until it changes.
		memcpy(files->signed_bank, bank, sizeof(*bank));
		files->signed_dac = dac->version;
		files->signature = signature;
		files->has_signature = exported;
	}
	bank_names(bank, names);
	if (files->has_signature) {
		snprintf(names->signature, sizeof(names->signature), "BANK %04X", files->signature);
	} else {
		snprintf(names->signature, sizeof(names->signature), "BANK ----");
	}
	// The plugin delivers a channel's own sound under its service numbers (D24, D28): there the
	// screen shows that sound's name, not the bank place's.
	const patch_values *values = patch_view(files->host.patch);
	for (int channel = 0; channel < PATCH_NUM_CHANNELS; channel++) {
		const bool psg = is_psg(channel);
		const int first = psg ? 120 + (channel - PATCH_NUM_FM_CHANNELS) * 2 : 116 + channel * 2;
		const char *name = psg ? patch_envelope_view(files->host.patch, channel)->name : values->names[channel];
		for (int i = 0; i < 2; i++) {
			screen_name(name, names->names[psg][first + i]);
		}
	}
	return true;
}
