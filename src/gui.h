#ifndef MDMI_GUI_H
#define MDMI_GUI_H

#include <stdbool.h>
#include <stdint.h>
#include "engine.h"
#include "files.h"
#include "patch.h"

// The plugin's window: everything is drawn by our own code into a pixel buffer. All of this runs
// on the host's main thread. The window reads the model's snapshot and asks for changes through edit().
#define GUI_WIDTH 720
#define GUI_HEIGHT 528
#define GUI_NUM_MIDI_CHANNELS 16

// What a MIDI channel is playing, written by the audio thread as one atomic value.
#define GUI_NOTE_KEY(v) ((v) & 0x7F)
#define GUI_NOTE_VELOCITY(v) (((v) >> 8) & 0x7F)
#define GUI_NOTE_HELD(v) (((v) >> 16) & 0xFF)
#define GUI_NOTE_SEEN(v) (((v) >> 24) & 1)
#define GUI_NOTE_PACK(key, velocity, held) \
	((uint32_t)(key) | ((uint32_t)(velocity) << 8) | ((uint32_t)(held) << 16) | (1u << 24))

// What edit() asks for: a gesture around one or more values, as a host records a knob.
enum { GUI_EDIT_BEGIN, GUI_EDIT_SET, GUI_EDIT_END };

// A PSG loop switched off in the envelope editor: where it was, so switching it on puts it back
// there. The sound has no loop, so only the plugin instance holds this: it outlives the window, not
// the project. It goes when the channel's envelope gets another name.
typedef struct {
	bool kept;
	int start;
	// -1: to the envelope's end.
	int end;
	char name[PATCH_NAME];
} gui_loop_shadow;

typedef struct {
	mdmi_engine *engine;
	mdmi_patch *patch;
	const uint32_t *notes;
	const char *version;
	// Which page and channel are open. They belong to the plugin instance, so they outlive the
	// window and are saved with the project. Only a view: no knob depends on them.
	int *page;
	int *channel;
	// PATCH_NUM_PSG_CHANNELS of them, the plugin's like page and channel, but not saved.
	gui_loop_shadow *loop_shadows;
	// What the window does with dropped and opened files.
	mdmi_files *files;
	void (*edit)(void *context, int kind, uint32_t id, int value);
	void *context;
} gui_source;

typedef struct mdmi_gui mdmi_gui;

mdmi_gui *gui_create(const gui_source *source);
void gui_destroy(mdmi_gui *gui);
bool gui_set_parent(mdmi_gui *gui, void *parent_hwnd);
bool gui_show(mdmi_gui *gui, bool visible);
bool gui_set_scale(mdmi_gui *gui, double scale);
void gui_get_size(const mdmi_gui *gui, uint32_t *width, uint32_t *height);
// An arrow key from the host; false when the window doesn't want it there.
bool gui_key(mdmi_gui *gui, bool up);

#endif
