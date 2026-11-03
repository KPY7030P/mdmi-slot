#ifndef MDMI_STATE_H
#define MDMI_STATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "patch.h"

// A project is a version, then blocks of type, length and data: the knobs, the window's view, the
// names of the FM channels and the bank. A snapshot file is the same bytes, a bank file has only the
// bank (D25). Main thread only.
#define STATE_READ_MAX 65536

// The window's page and channel, saved with the state.
typedef struct {
	int page;
	int channel;
} state_view;

// Without a view, only the bank. Freed by the caller.
uint8_t *state_build(mdmi_patch *patch, const state_view *view, size_t *size);
// Hands the model whatever blocks there are. False if nothing could be used; *knobs says whether
// there were values, and the view changes only if there was one.
bool state_read(mdmi_patch *patch, const uint8_t *bytes, size_t size, state_view *view, bool *knobs);

#endif
