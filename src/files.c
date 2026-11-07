#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "files.h"
#include "instrument.h"

#define INSTRUMENT_MAX 64

static bool is_psg(int channel)
{
	return channel >= PATCH_NUM_FM_CHANNELS && channel < PATCH_NUM_CHANNELS;
}

void files_init(mdmi_files *files, const files_host *host)
{
	memset(files, 0, sizeof(*files));
	files->host = *host;
}

void files_free(mdmi_files *files)
{
	free(files->undo_state);
	files->undo_state = NULL;
	files->undo = UNDO_NONE;
	dac_clear(&files->undo_dac);
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

// The whole file, if it is no larger than the most a state can be. Freed by the caller.
static uint8_t *read_all(const char *path, size_t *size)
{
	FILE *file = open_utf8(path, L"rb");
	if (!file) {
		return NULL;
	}
	uint8_t *bytes = malloc(STATE_READ_MAX + 1);
	if (bytes) {
		*size = fread(bytes, 1, STATE_READ_MAX + 1, file);
	}
	fclose(file);
	if (bytes && *size > STATE_READ_MAX) {
		free(bytes);
		return NULL;
	}
	return bytes;
}

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
static void remember(mdmi_files *files, int undo, int target, const patch_instrument *instrument, const patch_envelope *envelope, uint8_t *state, size_t state_size)
{
	free(files->undo_state);
	dac_clear(&files->undo_dac);
	files->undo = undo;
	files->undo_target = target;
	files->undo_state = state;
	files->undo_state_size = state_size;
	if (instrument) {
		files->undo_instrument = *instrument;
	}
	if (envelope) {
		files->undo_envelope = *envelope;
	}
	const char *name = instrument ? instrument->name : envelope ? envelope->name : "";
	switch (undo) {
	case UNDO_CHANNEL:
		snprintf(files->undo_text, sizeof(files->undo_text), "CH%d back to %s", target + 1, name);
		break;
	case UNDO_PLACE:
	case UNDO_ENVELOPE_PLACE:
		snprintf(files->undo_text, sizeof(files->undo_text), "slot %d back to %s", target + 1, name);
		break;
	default:
		snprintf(files->undo_text, sizeof(files->undo_text), target == FILES_BANK ? "the bank back" : "everything back");
		break;
	}
}

// Undo puts the channel's base back with its sound (D31).
static void remember_base(mdmi_files *files, int channel)
{
	patch_base_view(files->host.patch, channel, &files->undo_base_instrument, &files->undo_base_envelope);
}

// A file is the channel's base as well; undo gives the one the channel had.
static void put_on_channel(mdmi_files *files, int channel, const patch_instrument *instrument, const patch_instrument *base)
{
	patch_instrument before;
	channel_instrument(files->host.patch, channel, &before);
	patch_instrument before_base;
	patch_base_view(files->host.patch, channel, &before_base, NULL);
	patch_load_voice(files->host.patch, channel, instrument, base);
	remember(files, UNDO_CHANNEL, channel, &before, NULL, NULL, 0);
	files->undo_base_instrument = before_base;
	files->host.handed(files->host.context, false);
}

static void put_envelope_on_channel(mdmi_files *files, int channel, const patch_envelope *envelope, const patch_envelope *base)
{
	patch_envelope before;
	channel_envelope(files->host.patch, channel, &before);
	patch_envelope before_base;
	patch_base_view(files->host.patch, channel, NULL, &before_base);
	patch_load_envelope(files->host.patch, channel, envelope, base);
	remember(files, UNDO_CHANNEL, channel, NULL, &before, NULL, 0);
	files->undo_base_envelope = before_base;
	files->host.handed(files->host.context, false);
}

// A drawing keeps the base: what the channel got stays what it is compared with.
void files_draw_envelope(mdmi_files *files, int channel, const patch_envelope *before, const patch_envelope *envelope)
{
	if (before) {
		remember_base(files, channel);
	}
	patch_load_envelope(files->host.patch, channel, envelope, NULL);
	if (before) {
		remember(files, UNDO_CHANNEL, channel, NULL, before, NULL, 0);
		snprintf(files->undo_text, sizeof(files->undo_text), "CH%d envelope before drawing", channel + 1);
	}
	files->host.handed(files->host.context, false);
}

static void put_in_place(mdmi_files *files, int place, const patch_instrument *instrument)
{
	patch_instrument before = patch_bank_view(files->host.patch)->places[place];
	patch_load_place(files->host.patch, place, instrument);
	remember(files, UNDO_PLACE, place, &before, NULL, NULL, 0);
	files->host.handed(files->host.context, false);
}

static void put_envelope_in_place(mdmi_files *files, int place, const patch_envelope *envelope)
{
	patch_envelope before = patch_bank_view(files->host.patch)->envelopes[place];
	patch_load_envelope_place(files->host.patch, place, envelope);
	remember(files, UNDO_ENVELOPE_PLACE, place, NULL, &before, NULL, 0);
	files->host.handed(files->host.context, false);
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
		put_envelope_on_channel(files, channel, &envelope, &envelope);
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
		put_on_channel(files, channel, &instrument, &instrument);
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

int files_put_places(mdmi_files *files, const char *const *paths, int count, bool envelopes, int first, int *skipped, char *message, size_t size)
{
	if (skipped) {
		*skipped = count;
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
			bank->envelopes[place++] = envelope;
		} else if (!envelopes && read_instrument(sorted[i], &instrument, error, sizeof(error))) {
			bank->places[place++] = instrument;
		} else if (unread++ == 0) {
			snprintf(why, sizeof(why), "%s", error);
			first_unread = base_of(sorted[i]);
		}
	}
	const int loaded = place - first;
	if (loaded == 0) {
		if (first >= PATCH_BANK_SIZE) {
			snprintf(message, size, envelopes ? "Envelopes full to 128" : "Bank is full to 128");
		} else if (count == 1) {
			snprintf(message, size, "%s", why);
		} else {
			snprintf(message, size, "Nothing loaded: %d files not read", count);
		}
	} else if (loaded == 1 && envelopes) {
		const patch_envelope envelope = bank->envelopes[first];
		put_envelope_in_place(files, first, &envelope);
		snprintf(message, size, "Loaded %s to slot %d", envelope.name, first + 1);
	} else if (loaded == 1) {
		const patch_instrument instrument = bank->places[first];
		put_in_place(files, first, &instrument);
		snprintf(message, size, "Loaded %s to slot %d", instrument.name, first + 1);
	} else {
		size_t before_size;
		uint8_t *before = state_build(files->host.patch, NULL, &before_size);
		patch_load_bank(files->host.patch, bank);
		if (before) {
			remember(files, UNDO_STATE, FILES_BANK, NULL, NULL, before, before_size);
			snprintf(files->undo_text, sizeof(files->undo_text), envelopes ? "envelopes before %d files" : "bank before %d files", loaded);
		}
		files->host.handed(files->host.context, false);
		snprintf(message, size, "Loaded %d to slots %d-%d", loaded, first + 1, place);
	}
	if (loaded > 0) {
		const size_t used = strlen(message);
		if (unread) {
			snprintf(message + used, size - used, ", skipped %d: %s", unread + no_room, first_unread);
		} else if (no_room) {
			snprintf(message + used, size - used, ", %d didn't fit", no_room);
		}
	}
	if (skipped) {
		*skipped = unread + no_room;
	}
	free(sorted);
	free(bank);
	return loaded;
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
	for (int note = 0; note < ROM_IMAGE_NOTES; note++) {
		files->undo_dac.notes[note] = dac_copy(dac->notes[note]);
	}
	snprintf(files->undo_text, sizeof(files->undo_text), "%s", text);
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
	uint32_t used = dac_used(dac);
	int loaded = 0;
	for (; loaded < count; loaded++) {
		const int note = first + loaded;
		const char *name = base_of(paths[loaded]);
		size_t length = 0;
		uint8_t *bytes = files_kind(paths[loaded]) == FILES_SAMPLE ? read_all(paths[loaded], &length) : NULL;
		const char *error = "not a WAV file";
		read[loaded] = bytes ? dac_from_wav(bytes, length, name, &error) : NULL;
		free(bytes);
		if (!read[loaded]) {
			snprintf(message, size, "%s: %s", name, error);
			break;
		}
		if (dac->notes[note]) {
			used -= rom_image_cost(dac->notes[note]->sample.length);
		}
		used += rom_image_cost(read[loaded]->sample.length);
		if (used > rom_image_room()) {
			snprintf(message, size, "No room: %.2f MB more than the ROM has", (used - rom_image_room()) / 1048576.0);
			break;
		}
	}
	if (loaded < count) {
		for (int i = 0; i <= loaded && i < count; i++) {
			free(read[i]);
		}
		return 0;
	}
	char text[FILES_MESSAGE];
	if (count == 1) {
		snprintf(text, sizeof(text), "note %d back before %.40s", first, read[0]->name);
		snprintf(message, size, "%.40s > note %d, the engine restarts", read[0]->name, first);
	} else {
		snprintf(text, sizeof(text), "notes %d-%d back before %d WAV", first, first + count - 1, count);
		snprintf(message, size, "%d WAV > notes %d-%d, the engine restarts", count, first, first + count - 1);
	}
	remember_samples(files, text);
	for (int i = 0; i < count; i++) {
		dac_put(dac, first + i, read[i]);
	}
	files->host.handed(files->host.context, false);
	return count;
}

bool files_reset_sample(mdmi_files *files, int note, char *message, size_t size)
{
	dac_set *dac = files->host.view->dac;
	if (!dac || note < 0 || note >= ROM_IMAGE_NOTES || !dac->notes[note]) {
		return false;
	}
	char text[FILES_MESSAGE];
	snprintf(text, sizeof(text), "note %d back to %.40s", note, dac->notes[note]->name);
	remember_samples(files, text);
	dac_put(dac, note, NULL);
	snprintf(message, size, "Note %d plays the ROM's sample again, the engine restarts", note);
	files->host.handed(files->host.context, false);
	return true;
}

bool files_undo(mdmi_files *files, char *message, size_t size)
{
	const int undo = files->undo, target = files->undo_target;
	if (undo == UNDO_NONE) {
		return false;
	}
	snprintf(message, size, "Undone: %s", files->undo_text);
	if (undo == UNDO_CHANNEL && is_psg(target)) {
		patch_envelope back = files->undo_envelope, base = files->undo_base_envelope;
		put_envelope_on_channel(files, target, &back, &base);
	} else if (undo == UNDO_CHANNEL) {
		patch_instrument back = files->undo_instrument, base = files->undo_base_instrument;
		put_on_channel(files, target, &back, &base);
	} else if (undo == UNDO_ENVELOPE_PLACE) {
		patch_envelope back = files->undo_envelope;
		put_envelope_in_place(files, target, &back);
	} else if (undo == UNDO_PLACE) {
		patch_instrument back = files->undo_instrument;
		put_in_place(files, target, &back);
	} else if (undo == UNDO_SAMPLES) {
		// The samples before and now trade places, so a second undo brings the change back.
		dac_set *dac = files->host.view->dac;
		for (int note = 0; note < ROM_IMAGE_NOTES; note++) {
			dac_sample *now = dac->notes[note];
			dac->notes[note] = files->undo_dac.notes[note];
			files->undo_dac.notes[note] = now;
		}
		dac->version++;
		files->host.handed(files->host.context, false);
	} else {
		uint8_t *back = files->undo_state;
		files->undo_state = NULL;
		bool done = put_state(files, target, back, files->undo_state_size);
		free(back);
		if (!done) {
			files->undo = UNDO_NONE;
			return false;
		}
	}
	return true;
}

const char *files_undo_text(const mdmi_files *files)
{
	return files->undo == UNDO_NONE ? NULL : files->undo_text;
}
