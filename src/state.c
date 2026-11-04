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
enum { BLOCK_VALUES = 1, BLOCK_VIEW = 2, BLOCK_NAMES = 3, BLOCK_BANK = 4, BLOCK_ENVELOPES = 5, BLOCK_PSG = 6 };
static_assert(sizeof(patch_instrument) == PATCH_NAME + PATCH_VOICE_PARAMS * 2, "a bank place has no padding");
static_assert(sizeof(patch_envelope) == PATCH_NAME + PATCH_ENVELOPE_STEPS + 1, "an envelope has no padding");

static uint8_t *put_block_head(uint8_t *out, uint32_t type, uint32_t length)
{
	memcpy(out, &type, sizeof(type));
	memcpy(out + sizeof(type), &length, sizeof(length));
	return out + BLOCK_HEAD;
}

uint8_t *state_build(mdmi_patch *patch, const state_view *view, size_t *size)
{
	const size_t most = sizeof(uint32_t) + 6 * BLOCK_HEAD + patch_save_size() + 2 + NAMES_BYTES + PSG_BYTES + PLACES_BYTES + ENVELOPES_BYTES;
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
		out = put_block_head(out + patch_save_size(), BLOCK_VIEW, 2);
		out[0] = (uint8_t)view->page;
		out[1] = (uint8_t)view->channel;
		out = put_block_head(out + 2, BLOCK_NAMES, NAMES_BYTES);
		patch_save_names(values, out);
		out = put_block_head(out + NAMES_BYTES, BLOCK_PSG, PSG_BYTES);
		memcpy(out, values->psg, PSG_BYTES);
		out += PSG_BYTES;
	}
	const patch_bank *bank = patch_bank_view(patch);
	out = put_block_head(out, BLOCK_BANK, PLACES_BYTES);
	memcpy(out, bank->places, PLACES_BYTES);
	out = put_block_head(out + PLACES_BYTES, BLOCK_ENVELOPES, ENVELOPES_BYTES);
	memcpy(out, bank->envelopes, ENVELOPES_BYTES);
	*size = (size_t)(out + ENVELOPES_BYTES - buffer);
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
	const uint8_t *values = NULL, *seen_view = NULL, *names = NULL, *bank = NULL, *envelopes = NULL, *psg = NULL;
	uint32_t values_length = 0;
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
		} else if (type == BLOCK_NAMES && length == NAMES_BYTES) {
			names = bytes + at;
		} else if (type == BLOCK_BANK && length == PLACES_BYTES) {
			bank = bytes + at;
		} else if (type == BLOCK_ENVELOPES && length == ENVELOPES_BYTES) {
			envelopes = bytes + at;
		} else if (type == BLOCK_PSG && length == PSG_BYTES) {
			psg = bytes + at;
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
	if (bank) {
		patch_bank *copy = malloc(sizeof(patch_bank));
		if (!copy) {
			return false;
		}
		patch_bank_rom(copy);
		memcpy(copy->places, bank, PLACES_BYTES);
		if (envelopes) {
			memcpy(copy->envelopes, envelopes, ENVELOPES_BYTES);
		}
		patch_load_bank(patch, copy);
		free(copy);
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
		patch_load(patch, values, values_length, names ? (const char (*)[PATCH_NAME])copied : NULL, psg ? channel_envelopes : NULL);
		*knobs = true;
	}
	if (seen_view) {
		view->page = seen_view[0];
		view->channel = seen_view[1] < PATCH_NUM_CHANNELS ? seen_view[1] : 0;
	}
	return true;
}
