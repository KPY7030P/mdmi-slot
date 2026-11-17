#ifndef MDMI_STATE_H
#define MDMI_STATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "dac.h"
#include "patch.h"

// A project is a version, then blocks of type, length and data: the knobs, the window's view, the
// names of the FM channels, the PSG channels' envelopes, the loops switched off, the channels' bases
// (D31), the bank and its envelopes, the DAC samples (D36), the arp rows (stage 20, block 10), the
// releases of loops that stop (stage 22b, block 11). A snapshot file is the
// same bytes, a bank file has only the bank and its places' rows (D25). Main thread only.
// The DAC samples can fill the ROM image's room, with their sources (D38).
#define STATE_READ_MAX (DAC_SOURCE_MAX + ROM_IMAGE_MAX + (1u << 20))

// A PSG loop switched off in the envelope editor: where it was, so switching it on puts it back
// there. An EEF has no switched-off loop, so the project keeps it with the view (D30). It goes when
// the channel's envelope gets another name.
typedef struct {
	bool kept;
	int start;
	// -1: to the envelope's end.
	int end;
	char name[PATCH_NAME];
} state_loop;

// A PSG loop's release kept while Note Off stops the loop (stage 22b, D54): the envelope plays
// without the steps after its loop, which the window shows after the loop's end, dim, and puts back
// when the release is chosen again. An EEF has no such steps, so the project keeps them with the
// view (block 11, as D30). They go when the channel's envelope gets another name.
typedef struct {
	bool stop;
	int count;
	uint8_t steps[PATCH_ENVELOPE_STEPS];
	char name[PATCH_NAME];
} state_tail;

// The window's page and channel and the loops switched off, saved with the state. envelopes has a
// bit per FM channel whose row on ALL shows the envelopes instead of the diagram (stage 11), the
// view block's third byte.
typedef struct {
	int page;
	int channel;
	uint8_t envelopes;
	// Stage 36 (D81): the ROM page's TV look (tv.h: 0 off, 1 RGB, 2 composite), the fourth byte.
	uint8_t tv;
	state_loop loops[PATCH_NUM_PSG_CHANNELS];
	state_tail tails[PATCH_NUM_PSG_CHANNELS];
	// The plugin's DAC samples, NULL where a state has none to give them (tests). A whole state
	// replaces them, with the ROM's if it has no block; the set's version then goes up if they changed.
	dac_set *dac;
} state_view;

// Without a view, only the bank. Freed by the caller.
uint8_t *state_build(mdmi_patch *patch, const state_view *view, size_t *size);
// Hands the model whatever blocks there are. False if nothing could be used; *knobs says whether
// there were values, and the view changes only if there was one.
bool state_read(mdmi_patch *patch, const uint8_t *bytes, size_t size, state_view *view, bool *knobs);
// A bank file of a bank outside the model, a library (stage 22c, D56): the same bytes state_build
// gives without a view. Freed by the caller.
uint8_t *state_bank_build(const patch_bank *bank, size_t *size);
// The bank of a bank file or a state into out, which starts as the ROM's. False if there is none.
bool state_bank_read(const uint8_t *bytes, size_t size, patch_bank *out);

#endif
