#ifndef MDMI_GUI_H
#define MDMI_GUI_H

#include <stdbool.h>
#include <stdint.h>
#include "engine.h"
#include "patch.h"

// The plugin's window: everything is drawn by our own code into a pixel buffer. All of this runs
// on the host's main thread. The window only reads; nothing here changes the sound.
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

#ifdef MDMI_E3
// Experiment E3: two cells that send real gestures to the host, and what Program Changes arrived.
#define GUI_E3_CELLS 2
typedef struct {
	void *context;
	const char *names[GUI_E3_CELLS];
	int (*value)(void *context, int cell);
	void (*begin)(void *context, int cell);
	void (*change)(void *context, int cell, int value);
	void (*end)(void *context, int cell);
	const uint32_t *programs_seen;
	const uint32_t *last_program;
} gui_e3;
#endif

typedef struct {
	mdmi_engine *engine;
	const mdmi_patch *patch;
	const uint32_t *notes;
	const char *version;
	// Which page is open. Belongs to the plugin instance, so it outlives the window.
	int *page;
#ifdef MDMI_E3
	gui_e3 e3;
#endif
} gui_source;

typedef struct mdmi_gui mdmi_gui;

mdmi_gui *gui_create(const gui_source *source);
void gui_destroy(mdmi_gui *gui);
bool gui_set_parent(mdmi_gui *gui, void *parent_hwnd);
bool gui_show(mdmi_gui *gui, bool visible);
bool gui_set_scale(mdmi_gui *gui, double scale);
void gui_get_size(const mdmi_gui *gui, uint32_t *width, uint32_t *height);

#endif
