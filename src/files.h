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
enum { FILES_INSTRUMENT, FILES_BANK, FILES_SNAPSHOT, FILES_ENVELOPE };
#define FILES_MESSAGE 96

typedef struct {
	mdmi_patch *patch;
	state_view *view;
	// After the model got something to take; knobs if a whole state came, which the host re-reads.
	void (*handed)(void *context, bool knobs);
	void *context;
} files_host;

enum { UNDO_NONE, UNDO_CHANNEL, UNDO_PLACE, UNDO_ENVELOPE_PLACE, UNDO_STATE };

typedef struct {
	files_host host;
	// What the last file replaced, put back by the same command; undoing keeps what it replaced, so
	// a second undo brings it back.
	int undo;
	int undo_target;
	patch_instrument undo_instrument;
	patch_envelope undo_envelope;
	uint8_t *undo_state;
	size_t undo_state_size;
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
// What undo would put back, or NULL.
const char *files_undo_text(const mdmi_files *files);

#endif
