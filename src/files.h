#ifndef MDMI_FILES_H
#define MDMI_FILES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "library.h"
#include "patch.h"
#include "state.h"

// What the window does with files, on the main thread: an instrument onto a channel or a place of
// the bank, a bank or a snapshot onto everything, saving them, and undo with its history (D25, D90). An
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

// UNDO_ARP: a drawn row, only the row comes back (review of stage 20: a voice's knobs moved since stay).
// UNDO_LIBRARY: a place of a library file, written back to the file (stage 22c).
// UNDO_PRESET: a Preset step made in the window, the channel's whole sound before it (stage 48).
enum { UNDO_NONE, UNDO_CHANNEL, UNDO_PLACE, UNDO_ENVELOPE_PLACE, UNDO_STATE, UNDO_SAMPLES, UNDO_ARP, UNDO_LIBRARY, UNDO_PRESET };

// A place of a bank the window shows (stage 22c, D56): of the ROM bank, the one that plays, when
// library is NULL, otherwise of the library file at that path.
typedef struct {
	const char *library;
	int place;
} files_spot;

// One step of UNDO (D25, a history since stage 48, D90): what an action replaced, put back by the
// same command. Applying a step leaves in it what it replaced in turn, which is the redo step.
typedef struct {
	int kind;
	int target;
	patch_instrument instrument;
	patch_envelope envelope;
	// The channel's base with it (D31).
	patch_instrument base_instrument;
	patch_envelope base_envelope;
	// The row of the channel or the place, and of the channel's base (stage 20).
	patch_arp arp;
	patch_arp base_arp;
	// A place record of STORE: the channel whose base it changed, which undo gives back with the
	// place, or -1.
	int channel;
	uint8_t *state;
	size_t state_size;
	// The DAC samples before the last change of them, swapped back by undo.
	dac_set dac;
	// UNDO_LIBRARY: the file, and whether the place is of its envelopes; the place is target.
	bool envelopes;
	char library[LIBRARY_PATH];
	char text[FILES_MESSAGE];
	// Stage 43 (D87): a channel copied onto a PSG channel also took the source's loop shadow and kept
	// release; undo puts the target's back.
	bool view;
	state_loop loop;
	state_tail tail;
	// UNDO_PRESET (stage 48): the values before a Preset step made in the window; only the channel
	// target's part of them comes back, its Preset with it.
	patch_values *values;
} files_step;

// Stage 48 (D90): steps back and steps ahead, the oldest dropped first.
#define FILES_UNDO_DEPTH 32
// What heavy steps (whole states, sample sets) may hold together, each way.
#define FILES_UNDO_BYTES (64u << 20)

typedef struct {
	files_host host;
	// The step UNDO takes next (kind UNDO_NONE when there is none), the older ones, and the steps an
	// UNDO undid, for redo; a new action empties those.
	files_step now;
	files_step *back;
	int back_count;
	files_step *ahead;
	int ahead_count;
	// Set while a step is applied: the step it leaves goes into now, the history stays.
	bool replaying;
	// Stage 24: the bank and samples the signature was last reckoned for, and the signature.
	patch_bank *signed_bank;
	uint32_t signed_dac;
	uint16_t signature;
	// False when EXPORT would refuse that bank.
	bool has_signature;
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
// A drawn arp row for any channel 1-10, the same way (stage 20): the voice or envelope and the base
// stay. Hand it at most once per window frame and at mouse up; before is the row before the gesture
// on its first call, NULL after, and undo puts it back. The host is told nothing (D19).
void files_draw_arp(mdmi_files *files, int channel, const patch_arp *before, const patch_arp *arp);
// STORE (stage 20): the channel's whole sound (voice or envelope, name, row) into its Preset's
// place, one undo step that puts the place and the channel's base back. The channel then plays its
// place as it is. False when it does already: files_can_store says which.
bool files_can_store(mdmi_files *files, int channel);
bool files_store(mdmi_files *files, int channel, char *message, size_t size);
bool files_undo(mdmi_files *files, char *message, size_t size);
// Stage 48 (D90): the step the last UNDO took back, done again; false when there is none.
bool files_redo(mdmi_files *files, char *message, size_t size);
// Before a Preset step the window makes (the wheel, a click on a bank place, Up and Down): the
// channel's sound before it becomes an undo step. Steps one after another on the same channel, with
// no edit of its sound between them, are one step.
void files_preset_step(mdmi_files *files, int channel);
// A project the host loaded: no step goes back or ahead over it.
void files_forget(mdmi_files *files);
// WAV files onto the DAC's notes one after another from first (stage 12, D36), in the order given;
// the core restarts with them. The set's room is checked for all of them first: if they don't fit,
// or one can't be read, nothing changes. Returns how many went in.
int files_put_samples(mdmi_files *files, const char *const *paths, int count, int first, char *message, size_t size);
// Prepares a note's sample from its source another way (D38): one undo step. A sample of the ROM's
// becomes the project's own, a copy of its bytes, at its first change. join: the change goes into
// the undo step of the edit just before it, a double click's second half (stage 22b).
bool files_edit_sample(mdmi_files *files, int note, const dac_edit *edit, bool join, char *message, size_t size);
// A sample to another note (owner after 0.12.0): swapped with what that note played, or copied
// there. The ROM's samples go as copies of their bytes. One undo step. 1 if it went.
int files_move_sample(mdmi_files *files, int from, int to, bool copy, char *message, size_t size);
// CLEAR: a file goes and the ROM's sample comes back; on a note of the ROM's the sample is removed,
// and on a removed one it comes back.
bool files_reset_sample(mdmi_files *files, int note, char *message, size_t size);
// Libraries (stage 22c, D56). A library is a bank file; a place of it is empty when it is the ROM's
// GM preset, or envelope, with an empty row (D26). The file is read again before every change and
// written whole at once: to a file beside it, then put in its place.
bool files_read_library(const char *path, patch_bank *bank);
bool files_place_empty(const patch_bank *bank, bool envelopes, int place);
// A new library, every place empty, over a file that is there (the save dialog has asked).
bool files_new_library(const char *path, char *message, size_t size);
// Stage 43 (D87): channel from's sound onto channel to of the same kind, as a dropped file goes: the
// voice or envelope with its name, the row, the base (so the target shows the source's `*`) and, on
// PSG, the loop shadow and kept release. The channels' knobs stay; the host is told nothing (D19). One
// undo step. False, with a message, for the same channel or another kind.
bool files_copy_channel(mdmi_files *files, int from, int to, char *message, size_t size);
// A place's sound, name and row onto the channel, as a dropped file goes (D25): Preset stays.
bool files_try_place(mdmi_files *files, int channel, const files_spot *from, char *message, size_t size);
// A place's sound, name and row over another's, of the same list: FM voices, or envelopes. One undo
// step, which writes a library back. A library's place < 0 is its first empty place in the file as it
// is now, where the sound doesn't read as empty (a GM sound on its own GM place would); landed, when
// not NULL, says where the sound went.
bool files_copy_place(mdmi_files *files, bool envelopes, const files_spot *from, const files_spot *to, int *landed, char *message, size_t size);
// A library's place emptied.
bool files_clear_place(mdmi_files *files, bool envelopes, const files_spot *spot, char *message, size_t size);
// What undo would put back, or NULL.
const char *files_undo_text(const mdmi_files *files);
const char *files_redo_text(const mdmi_files *files);
// EXPORT (stage 24): the ROM for the SD card, with the ROM bank, the DAC samples, the names and the
// signature, written beside the file and then put in its place, so a refusal leaves it as it was. Not
// remembered by the project, no undo. The message names the channels that play a sound the bank
// doesn't hold, which the file can't play by its program numbers.
bool files_export(mdmi_files *files, const char *path, char *message, size_t size);
// The names the emulator's ROM screen shows (stage 24): the bank's, where a channel's service
// numbers carry its own sound's name, and the signature EXPORT would write now. False when out of
// memory.
bool files_screen_names(mdmi_files *files, rom_names *names);

#endif
