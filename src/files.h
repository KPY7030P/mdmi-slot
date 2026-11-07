#ifndef MDMI_FILES_H
#define MDMI_FILES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "patch.h"
#include "state.h"

// What the window does with files, on the main thread: an instrument onto a channel or a place of
// the bank, a bank or a snapshot onto everything, saving them, and one step of undo (D25). An
// envelope does on a PSG channel and the bank's envelopes what an instrument does on FM (D28).
// A WAV goes to a note of the DAC (stage 12).
enum { FILES_INSTRUMENT, FILES_BANK, FILES_SNAPSHOT, FILES_ENVELOPE, FILES_SAMPLE };
#define FILES_MESSAGE 96

typedef struct {
	mdmi_patch *patch;
	state_view *view;
	// After the model got something to take; knobs if a whole state came, which the host re-reads.
	void (*handed)(void *context, bool knobs);
	void *context;
} files_host;

enum { UNDO_NONE, UNDO_CHANNEL, UNDO_PLACE, UNDO_ENVELOPE_PLACE, UNDO_STATE, UNDO_SAMPLES };

typedef struct {
	files_host host;
	// What the last file replaced, put back by the same command; undoing keeps what it replaced, so
	// a second undo brings it back.
	int undo;
	int undo_target;
	patch_instrument undo_instrument;
	patch_envelope undo_envelope;
	// The channel's base with it (D31).
	patch_instrument undo_base_instrument;
	patch_envelope undo_base_envelope;
	uint8_t *undo_state;
	size_t undo_state_size;
	// The DAC samples before the last change of them, swapped back by undo.
	dac_set undo_dac;
	char undo_text[FILES_MESSAGE];
} mdmi_files;

void files_init(mdmi_files *files, const files_host *host);
void files_free(mdmi_files *files);
// What a file is by its name, or -1.
int files_kind(const char *path);
// An instrument goes to the channel, a bank or a snapshot to everything. The message says what
// happened, or why nothing did. Paths are UTF-8.
bool files_open(mdmi_files *files, const char *path, int channel, char *message, size_t size);
// Instruments, or envelopes, into the bank one after another in the order of their names, from the
// place first, or from files_add_place when first < 0 (D26). Files that can't be read, are of the
// other kind, or come after place 128 are skipped and counted in the message. One undo step puts
// the bank back. Returns how many went in.
int files_put_places(mdmi_files *files, const char *const *paths, int count, bool envelopes, int first, int *skipped, char *message, size_t size);
// The place after the last one that differs from GM, or from the ROM's envelope; PATCH_BANK_SIZE
// when that is the last.
int files_add_place(const mdmi_files *files, bool envelopes);
bool files_save(mdmi_files *files, const char *path, int kind, int channel, char *message, size_t size);
// A drawn envelope for a PSG channel, the way a dropped EEF goes (stage 8). The first of a gesture
// gives what the channel had before it, which undo puts back.
void files_draw_envelope(mdmi_files *files, int channel, const patch_envelope *before, const patch_envelope *envelope);
bool files_undo(mdmi_files *files, char *message, size_t size);
// WAV files onto the DAC's notes one after another from first (stage 12, D36), in the order given;
// the core restarts with them. The set's room is checked for all of them first: if they don't fit,
// or one can't be read, nothing changes. Returns how many went in.
int files_put_samples(mdmi_files *files, const char *const *paths, int count, int first, char *message, size_t size);
// Prepares a note's sample from its source another way (D38): one undo step. A sample of the ROM's
// becomes the project's own, a copy of its bytes, at its first change.
bool files_edit_sample(mdmi_files *files, int note, const dac_edit *edit, char *message, size_t size);
// A sample to another note (owner after 0.12.0): swapped with what that note played, or copied
// there. The ROM's samples go as copies of their bytes. One undo step. 1 if it went.
int files_move_sample(mdmi_files *files, int from, int to, bool copy, char *message, size_t size);
// CLEAR: a file goes and the ROM's sample comes back; on a note of the ROM's the sample is removed,
// and on a removed one it comes back.
bool files_reset_sample(mdmi_files *files, int note, char *message, size_t size);
// What undo would put back, or NULL.
const char *files_undo_text(const mdmi_files *files);

#endif
