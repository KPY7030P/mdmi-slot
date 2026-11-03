#ifndef MDMI_FILES_H
#define MDMI_FILES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "patch.h"
#include "state.h"

// What the window does with files, on the main thread: an instrument onto a channel or a place of
// the bank, a bank or a snapshot onto everything, saving them, and one step of undo (D25).
enum { FILES_INSTRUMENT, FILES_BANK, FILES_SNAPSHOT };
#define FILES_MESSAGE 96

typedef struct {
	mdmi_patch *patch;
	state_view *view;
	// After the model got something to take; knobs if a whole state came, which the host re-reads.
	void (*handed)(void *context, bool knobs);
	void *context;
} files_host;

enum { UNDO_NONE, UNDO_CHANNEL, UNDO_PLACE, UNDO_STATE };

typedef struct {
	files_host host;
	// What the last file replaced, put back by the same command; undoing keeps what it replaced, so
	// a second undo brings it back.
	int undo;
	int undo_target;
	patch_instrument undo_instrument;
	uint8_t *undo_state;
	size_t undo_state_size;
	char undo_text[FILES_MESSAGE];
} mdmi_files;

void files_init(mdmi_files *files, const files_host *host);
void files_free(mdmi_files *files);
// What a file is by its name, or -1.
int files_kind(const char *path);
// An instrument goes to the channel, or to the place of the bank when place >= 0; a bank or a
// snapshot to everything. The message says what happened, or why nothing did. Paths are UTF-8.
bool files_open(mdmi_files *files, const char *path, int channel, int place, char *message, size_t size);
bool files_save(mdmi_files *files, const char *path, int kind, int channel, char *message, size_t size);
bool files_undo(mdmi_files *files, char *message, size_t size);
// What undo would put back, or NULL.
const char *files_undo_text(const mdmi_files *files);

#endif
