#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "state.h"

#define STATE_VERSION 3
#define BLOCK_HEAD (2 * sizeof(uint32_t))
#define NAMES_BYTES (PATCH_NUM_FM_CHANNELS * PATCH_NAME)
#define PLACES_BYTES (PATCH_BANK_SIZE * sizeof(patch_instrument))
#define ENVELOPES_BYTES (PATCH_BANK_SIZE * sizeof(patch_envelope))
#define PSG_BYTES (PATCH_NUM_PSG_CHANNELS * sizeof(patch_envelope))
// The bank's envelopes and the PSG channels' came with D28 as blocks of their own, so the bank
// block keeps its length and older builds skip the new ones.
// The loops switched off in the editor came with D30, also as a block of their own: per PSG
// channel kept, start, end (255 for the envelope's end) and the envelope's name.
// The channels' bases came with D31, as a block of their own: the FM voices, then the PSG envelopes.
// The DAC samples came with D36, also as a block of their own, written only when there are any.
// The arp rows came with stage 20 as one block of fixed length: the FM places, the PSG places, the
// channels and the channels' bases, 36 bytes each: steps, loop step (255 none), loop switched off,
// its shadow's step, 32 shifts. A bank file has the block too, its channels' rows empty and unread.
// Stage 22b put the row switched off in bit 1 of the third byte (D55): the block keeps its length.
// The releases kept while a PSG loop stops at Note Off came with stage 22b (D54) as a block of their
// own, written with the view: per PSG channel stop, the steps' count, the steps, the envelope's name.
enum { BLOCK_VALUES = 1, BLOCK_VIEW = 2, BLOCK_NAMES = 3, BLOCK_BANK = 4, BLOCK_ENVELOPES = 5, BLOCK_PSG = 6, BLOCK_LOOPS = 7, BLOCK_BASES = 8, BLOCK_DAC = 9, BLOCK_ARPS = 10, BLOCK_TAILS = 11 };
#define PLACE_ARPS_BYTES (PATCH_BANK_SIZE * sizeof(patch_arp))
#define ARPS_BYTES (2 * PLACE_ARPS_BYTES + PATCH_CHANNEL_ARPS_BYTES)
#define LOOP_BYTES (3 + PATCH_NAME)
#define LOOPS_BYTES (PATCH_NUM_PSG_CHANNELS * LOOP_BYTES)
#define TAIL_BYTES (2 + PATCH_ENVELOPE_STEPS + PATCH_NAME)
#define TAILS_BYTES (PATCH_NUM_PSG_CHANNELS * TAIL_BYTES)
static_assert(sizeof(patch_instrument) == PATCH_NAME + PATCH_VOICE_PARAMS * 2, "a bank place has no padding");
static_assert(sizeof(patch_envelope) == PATCH_NAME + PATCH_ENVELOPE_STEPS + 1, "an envelope has no padding");
static_assert(sizeof(patch_arp) == 4 + PATCH_ARP_STEPS, "a row has no padding");

static uint8_t *put_block_head(uint8_t *out, uint32_t type, uint32_t length)
{
	memcpy(out, &type, sizeof(type));
	memcpy(out + sizeof(type), &length, sizeof(length));
	return out + BLOCK_HEAD;
}

uint8_t *state_build(mdmi_patch *patch, const state_view *view, size_t *size)
{
	const size_t dac_bytes = view && view->dac ? dac_block_size(view->dac) : 0;
	const size_t most = sizeof(uint32_t) + 11 * BLOCK_HEAD + patch_save_size() + 3 + NAMES_BYTES + PSG_BYTES + LOOPS_BYTES + TAILS_BYTES + PATCH_BASES_BYTES + PLACES_BYTES + ENVELOPES_BYTES + ARPS_BYTES + dac_bytes;
	uint8_t *buffer = malloc(most);
	if (!buffer) {
		return NULL;
	}
	const uint32_t version = STATE_VERSION;
	memcpy(buffer, &version, sizeof(version));
	uint8_t *out = buffer + sizeof(version);
	if (view) {
		const patch_values *values = patch_view(patch);
		out = put_block_head(out, BLOCK_VALUES, (uint32_t)patch_save_size());
		patch_save(values, out);
		// The third byte came with stage 11; older builds read the first two.
		out = put_block_head(out + patch_save_size(), BLOCK_VIEW, 3);
		out[0] = (uint8_t)view->page;
		out[1] = (uint8_t)view->channel;
		out[2] = view->envelopes;
		out = put_block_head(out + 3, BLOCK_NAMES, NAMES_BYTES);
		patch_save_names(values, out);
		out = put_block_head(out + NAMES_BYTES, BLOCK_PSG, PSG_BYTES);
		memcpy(out, values->psg, PSG_BYTES);
		out = put_block_head(out + PSG_BYTES, BLOCK_LOOPS, LOOPS_BYTES);
		for (int i = 0; i < PATCH_NUM_PSG_CHANNELS; i++) {
			const state_loop *loop = &view->loops[i];
			uint8_t *at = out + i * LOOP_BYTES;
			at[0] = loop->kept;
			at[1] = (uint8_t)loop->start;
			at[2] = loop->end < 0 ? 255 : (uint8_t)loop->end;
			memcpy(at + 3, loop->name, PATCH_NAME);
		}
		out = put_block_head(out + LOOPS_BYTES, BLOCK_TAILS, TAILS_BYTES);
		for (int i = 0; i < PATCH_NUM_PSG_CHANNELS; i++) {
			const state_tail *tail = &view->tails[i];
			uint8_t *at = out + i * TAIL_BYTES;
			const int count = tail->count < 0 ? 0 : tail->count > PATCH_ENVELOPE_STEPS ? PATCH_ENVELOPE_STEPS : tail->count;
			at[0] = tail->stop;
			at[1] = (uint8_t)count;
			memset(at + 2, 0, PATCH_ENVELOPE_STEPS);
			memcpy(at + 2, tail->steps, (size_t)count);
			memcpy(at + 2 + PATCH_ENVELOPE_STEPS, tail->name, PATCH_NAME);
		}
		out = put_block_head(out + TAILS_BYTES, BLOCK_BASES, PATCH_BASES_BYTES);
		patch_save_bases(values, out);
		out += PATCH_BASES_BYTES;
		if (dac_bytes) {
			out = put_block_head(out, BLOCK_DAC, (uint32_t)dac_bytes);
			dac_block_write(view->dac, out);
			out += dac_bytes;
		}
	}
	const patch_bank *bank = patch_bank_view(patch);
	out = put_block_head(out, BLOCK_BANK, PLACES_BYTES);
	memcpy(out, bank->places, PLACES_BYTES);
	out = put_block_head(out + PLACES_BYTES, BLOCK_ENVELOPES, ENVELOPES_BYTES);
	memcpy(out, bank->envelopes, ENVELOPES_BYTES);
	out = put_block_head(out + ENVELOPES_BYTES, BLOCK_ARPS, ARPS_BYTES);
	memcpy(out, bank->place_arps, PLACE_ARPS_BYTES);
	memcpy(out + PLACE_ARPS_BYTES, bank->envelope_arps, PLACE_ARPS_BYTES);
	if (view) {
		patch_save_arps(patch_view(patch), out + 2 * PLACE_ARPS_BYTES);
	} else {
		const patch_arp none = PATCH_ARP_EMPTY;
		for (int i = 0; i < 2 * PATCH_NUM_CHANNELS; i++) {
			memcpy(out + 2 * PLACE_ARPS_BYTES + i * sizeof(patch_arp), &none, sizeof(none));
		}
	}
	*size = (size_t)(out + ARPS_BYTES - buffer);
	return buffer;
}

// The bank goes first, so channels without names are named after it. Without envelopes, the bank
// has the ROM's and the PSG channels play their places: a state of an older build sounds as it did.
bool state_read(mdmi_patch *patch, const uint8_t *bytes, size_t size, state_view *view, bool *knobs)
{
	uint32_t version;
	*knobs = false;
	if (size < sizeof(version)) {
		return false;
	}
	memcpy(&version, bytes, sizeof(version));
	if (version != STATE_VERSION) {
		return false;
	}
	const uint8_t *values = NULL, *seen_view = NULL, *names = NULL, *bank = NULL, *envelopes = NULL, *psg = NULL, *loops = NULL, *bases = NULL, *samples = NULL, *arps = NULL, *tails = NULL;
	uint32_t values_length = 0, view_length = 0, samples_length = 0;
	for (size_t at = sizeof(version); at + BLOCK_HEAD <= size;) {
		uint32_t type, length;
		memcpy(&type, bytes + at, sizeof(type));
		memcpy(&length, bytes + at + sizeof(type), sizeof(length));
		at += BLOCK_HEAD;
		if (length > size - at) {
			return false;
		}
		if (type == BLOCK_VALUES) {
			values = bytes + at;
			values_length = length;
		} else if (type == BLOCK_VIEW && length >= 2) {
			seen_view = bytes + at;
			view_length = length;
		} else if (type == BLOCK_NAMES && length == NAMES_BYTES) {
			names = bytes + at;
		} else if (type == BLOCK_BANK && length == PLACES_BYTES) {
			bank = bytes + at;
		} else if (type == BLOCK_ENVELOPES && length == ENVELOPES_BYTES) {
			envelopes = bytes + at;
		} else if (type == BLOCK_PSG && length == PSG_BYTES) {
			psg = bytes + at;
		} else if (type == BLOCK_LOOPS && length == LOOPS_BYTES) {
			loops = bytes + at;
		} else if (type == BLOCK_BASES && length == PATCH_BASES_BYTES) {
			bases = bytes + at;
		} else if (type == BLOCK_ARPS && length == ARPS_BYTES) {
			arps = bytes + at;
		} else if (type == BLOCK_TAILS && length == TAILS_BYTES) {
			tails = bytes + at;
		} else if (type == BLOCK_DAC) {
			samples = bytes + at;
			samples_length = length;
		}
		// A block of a newer build is skipped.
		at += length;
	}
	if (!values && !bank) {
		return false;
	}
	if (values && values_length % 6) {
		return false;
	}
	// A whole state has its samples, or the ROM's. Read before anything changes, so damaged
	// samples leave the state untouched.
	dac_set read_samples = {0};
	if (values && view->dac && samples && !dac_block_read(&read_samples, samples, samples_length)) {
		return false;
	}
	if (bank) {
		patch_bank *copy = malloc(sizeof(patch_bank));
		if (!copy) {
			dac_clear(&read_samples);
			return false;
		}
		patch_bank_rom(copy);
		memcpy(copy->places, bank, PLACES_BYTES);
		if (envelopes) {
			memcpy(copy->envelopes, envelopes, ENVELOPES_BYTES);
		}
		// Without rows every place has none.
		if (arps) {
			memcpy(copy->place_arps, arps, PLACE_ARPS_BYTES);
			memcpy(copy->envelope_arps, arps + PLACE_ARPS_BYTES, PLACE_ARPS_BYTES);
		}
		patch_load_bank(patch, copy);
		free(copy);
	}
	if (values && view->dac) {
		dac_take(view->dac, &read_samples);
	}
	if (values) {
		char copied[PATCH_NUM_FM_CHANNELS][PATCH_NAME];
		if (names) {
			memcpy(copied, names, sizeof(copied));
		}
		patch_envelope channel_envelopes[PATCH_NUM_PSG_CHANNELS];
		if (psg) {
			memcpy(channel_envelopes, psg, PSG_BYTES);
		}
		patch_load(patch, values, values_length, names ? (const char (*)[PATCH_NAME])copied : NULL, psg ? channel_envelopes : NULL, bases, arps ? arps + 2 * PLACE_ARPS_BYTES : NULL);
		*knobs = true;
	}
	if (seen_view) {
		view->page = seen_view[0];
		view->channel = seen_view[1] < PATCH_NUM_CHANNELS ? seen_view[1] : 0;
		view->envelopes = view_length >= 3 ? seen_view[2] : 0;
		// A state of an older build has no loops switched off.
		memset(view->loops, 0, sizeof(view->loops));
		for (int i = 0; loops && i < PATCH_NUM_PSG_CHANNELS; i++) {
			const uint8_t *at = loops + i * LOOP_BYTES;
			state_loop *loop = &view->loops[i];
			loop->kept = at[0] != 0 && at[1] < PATCH_ENVELOPE_STEPS;
			loop->start = at[1];
			loop->end = at[2] == 255 ? -1 : at[2];
			memcpy(loop->name, at + 3, PATCH_NAME);
			loop->name[PATCH_NAME - 1] = '\0';
		}
		// Nor releases kept (before 0.22.3).
		memset(view->tails, 0, sizeof(view->tails));
		for (int i = 0; tails && i < PATCH_NUM_PSG_CHANNELS; i++) {
			const uint8_t *at = tails + i * TAIL_BYTES;
			state_tail *tail = &view->tails[i];
			tail->count = at[1] <= PATCH_ENVELOPE_STEPS ? at[1] : 0;
			tail->stop = at[0] != 0 && tail->count > 0;
			memcpy(tail->steps, at + 2, (size_t)tail->count);
			memcpy(tail->name, at + 2 + PATCH_ENVELOPE_STEPS, PATCH_NAME);
			tail->name[PATCH_NAME - 1] = '\0';
		}
	}
	return true;
}
