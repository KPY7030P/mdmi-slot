#define COBJMACROS
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <commdlg.h>
#include <ole2.h>
#include <shellapi.h>
#include "gui.h"
#include "font5x7.h"

#define TIMER_ID 1
#define TIMER_MS 33
#define WINDOW_CLASS "MDMIPluginWindow"

// The 16 colours of docs/design/ui-concept.md, all real CRAM values.
#define C_BG 0x000000
#define C_CELL 0x222222
#define C_LINE 0x444444
#define C_HINT 0x666666
#define C_LABEL 0xAAAAAA
#define C_TEXT 0xEEEEEE
#define C_FM_DARK 0x222266
#define C_FM 0x4422EE
#define C_FM_LIGHT 0x8888EE
#define C_PSG_DARK 0x004466
#define C_PSG 0x22CCEE
#define C_DAC_DARK 0x442200
#define C_DAC 0xEE8822
#define C_PLAYING 0xAAEE22
#define C_AMBER 0xEECC66
#define C_ERROR 0xEE2222

// The layout of docs/design/ui-concept.md, "Окно 720x528".
#define HEADER_H 28
#define BUTTON_Y 2
#define BUTTON_W 80
#define BUTTON_SMALL_W 56
#define BUTTON_H 24
#define BUTTON_ALL_X 60
#define BUTTON_EDITOR_X (BUTTON_ALL_X + BUTTON_SMALL_W + 4)
#define BUTTON_BANK_X (BUTTON_EDITOR_X + BUTTON_W + 4)
#define BUTTON_ROM_X (BUTTON_BANK_X + BUTTON_W + 4)
#define STATUS_X (BUTTON_ROM_X + BUTTON_SMALL_W + 8)
#define ACTION_W 56
#define ACTION_ADD_X 476
#define ACTION_LOAD_X 536
#define ACTION_SAVE_X 596
#define ACTION_UNDO_X 656
#define NAME_X 288
#define NAME_CHARS 23
#define ENVELOPE_NAME_CHARS 20
// The page's grid (stage 9, D29): the channel's cells in four columns of 112 px, two rows, the same
// on FM, PSG and DAC; what belongs to the chip starts under them.
#define GRID_W 112
#define GRID_ROW1_Y 56
#define GRID_ROW2_Y 78
#define CHIP_Y 104
#define SOURCE_X 592
#define SOURCE_W 116
static const int GRID_X[4] = {242, 358, 474, 590};
#define RAIL_W 232
#define RAIL_FM_Y 44
#define RAIL_PSG_Y 300
#define GROUP_H 16
#define ROW_H 40
#define PAGE_X 242
#define PAGE_RIGHT 708
#define GLOBALS_Y 460
#define FOOTER_Y 504
#define ROM_SCALE 2
#define CELL_H 20
#define DIAGRAM_X 242
#define DIAGRAM_Y CHIP_Y
#define OP_W GRID_W
#define OP_HEADER_Y 202
#define OP_HEADER_H 14
#define ALGORITHM_X 474
#define ALGORITHM_Y 152
#define DRAG_SLOP 3
#define MAX_CELLS 80
#define TIMER_PER_SECOND (1000 / TIMER_MS)
// The bank page: one row per place, scrolled.
#define BANK_X 242
#define BANK_HEADER_Y 60
#define BANK_Y 72
#define BANK_ROW_H 20
#define BANK_ROWS 19
#define BANK_RIGHT 698
#define BANK_DOTS_X 616
// While files are dragged the rows left of this replace and the FILE and dots column adds (D26).
#define BANK_ADD_X 576
#define BANK_ADD_Y 56
#define SCROLL_X 702
#define SCROLL_H (BANK_ROWS * BANK_ROW_H)
#define BANK_TOP_MAX (PATCH_BANK_SIZE - BANK_ROWS)
#define WHEEL_ROWS 3
#define MESSAGE_TICKS (4 * TIMER_PER_SECOND)
#define ERROR_TICKS (10 * TIMER_PER_SECOND)
#define FLASH_TICKS 9

// Saved with the project, so the numbers stay.
// The envelope editor (stage 8, art-director's note; 4A in stage 9): one column per frame sharing the
// lane by the length (0.8.1), the loop's strip on top, a level lane, a pitch lane of 15 rows by value,
// the time axis with the length cell at its end. Every height follows from the strip's.
#define ENV_X 296
#define ENV_LANE_W 404
#define ENV_LOOP_Y 106
#define ENV_LOOP_H 14
#define ENV_LEVEL_Y (ENV_LOOP_Y + 16)
#define ENV_BASE_Y (ENV_LEVEL_Y + 150)
#define ENV_LEVEL_BOTTOM (ENV_LEVEL_Y + 160)
#define ENV_PITCH_Y (ENV_LEVEL_BOTTOM + 8)
#define ENV_PITCH_ROW 6
#define ENV_PITCH_BOTTOM (ENV_PITCH_Y + 15 * ENV_PITCH_ROW)
#define ENV_CENTRE_Y (ENV_PITCH_Y + 7 * ENV_PITCH_ROW + 3)
#define ENV_AXIS_Y (ENV_PITCH_BOTTOM + 4)
#define ENV_LENGTH_X 648
#define ENV_LENGTH_Y (ENV_AXIS_Y + 3)
#define ENV_LENGTH_W 52
// The width of the switch drawn while there is no loop.
#define ENV_LOOP_OFF_W 36
// Pixels of a vertical drag on the length per frame.
#define ENV_LENGTH_PIXELS 2
enum { ENV_NONE, ENV_LEVEL, ENV_PITCH, ENV_LOOP_START, ENV_LOOP_END, ENV_LOOP, ENV_LENGTH };

// The page is saved with the project: a new one goes last.
enum { PAGE_EDITOR, PAGE_ROM, PAGE_BANK, PAGE_ALL };

// The overview (stage 10, D29): its rows continue the rail's, its columns as in ui-variants-v2.
#define ALL_SOUND_X 242
#define ALL_SOUND_W 104
#define ALL_VOL_X 354
#define ALL_PAN_X 474
#define ALL_PAN_W 17
#define ALL_SOURCE_X 536
enum { ACTION_LOAD, ACTION_SAVE, ACTION_UNDO, ACTION_ADD, ACTIONS };

typedef struct {
	IDropTarget iface;
	struct mdmi_gui *gui;
} drop_target;
// A row cell is dragged, a column cell is a global knob, a choice is one of a row of buttons (the
// algorithm, pan L C R), the source is the Preset drawn as where the sound comes from.
enum { CELL_ROW, CELL_COLUMN, CELL_CHOICE, CELL_SOURCE };

// A value the mouse can change. Rebuilt for each frame from what the page shows.
typedef struct {
	int x, y, w, h;
	uint32_t id;
	const char *label;
	int kind;
	int choice;
} cell;

struct mdmi_gui {
	gui_source source;
	HWND window;
	HWND parent;
	int scale;
	uint32_t *pixels;

	cell cells[MAX_CELLS];
	int cell_count;
	int mouse_x, mouse_y;
	bool tracking;
	// The cell being dragged, -1 when none.
	uint32_t drag_id;
	bool dragging;
	bool drag_moved;
	int drag_y;
	int drag_start;
	int drag_value;

	uint32_t underruns_second_ago;
	uint32_t underruns_per_second;
	int ticks;

	// The first place the bank page shows, and what the page last showed it for.
	int bank_top;
	int bank_seen_channel;
	bool bank_was_open;
	// The arrows' last step: the channel, the place it played and the place asked for.
	int key_channel;
	int key_from;
	int key_to;

	// Files dragged over the window, and where they would go: a channel, places from a row, places
	// added after the last file, or nowhere. The first file's kind and names; instruments counts the
	// TFI, VGI and TYI among them, envelopes the EEF. drop_envelopes says which of the two go.
	drop_target drop;
	bool drop_registered;
	bool ole_ready;
	bool file_over;
	int file_kind;
	int file_count;
	int file_instruments;
	int file_envelopes;
	bool drop_envelopes;
	char file_paths[PATCH_BANK_SIZE][MAX_PATH * 3];
	char file_name[64];
	char file_stem[64];
	int drop_channel;
	int drop_place;
	bool drop_add;
	// The places lit for the drop.
	int drop_first;
	int drop_lit;
	char drop_hint[FILES_MESSAGE];
	uint32_t drop_hint_color;

	// The envelope being drawn (stage 8): what is dragged, the window's copy while the gesture lasts,
	// what the channel had before it, and the last column drawn, so a fast drag leaves no gaps.
	int env_drag;
	int env_channel;
	patch_shape env_shape;
	patch_envelope env_before;
	bool env_changed;
	bool env_handed;
	bool env_told;
	int env_last_step;
	int env_last_value;
	// A drag on the length starts from these, so shortening and lengthening back keeps the frames.
	patch_shape env_start;
	int env_start_y;

	// The footer's message, and the row or places a file just went to.
	char message[FILES_MESSAGE];
	uint32_t message_color;
	int message_ticks;
	int flash_channel;
	int flash_place;
	int flash_count;
	int flash_ticks;
	uint32_t flash_color;

	// The host's values seen so far per knob, and until when, in GetTickCount64 ms, each knob stays
	// marked (D32).
	uint32_t host_seen[GUI_HOST_SLOTS];
	ULONGLONG host_until[GUI_HOST_SLOTS];
};

static LONG window_count;

static void fill(mdmi_gui *g, int x, int y, int w, int h, uint32_t color)
{
	for (int row = y; row < y + h && row < GUI_HEIGHT; row++) {
		uint32_t *line = g->pixels + row * GUI_WIDTH;
		for (int col = x; col < x + w && col < GUI_WIDTH; col++) {
			line[col] = color;
		}
	}
}

static void frame(mdmi_gui *g, int x, int y, int w, int h, uint32_t color)
{
	fill(g, x, y, w, 1, color);
	fill(g, x, y + h - 1, w, 1, color);
	fill(g, x, y, 1, h, color);
	fill(g, x + w - 1, y, 1, h, color);
}

static int text(mdmi_gui *g, int x, int y, const char *string, uint32_t color, int scale)
{
	for (const char *c = string; *c; c++) {
		unsigned index = (unsigned char)*c - 32;
		if (index < 95) {
			for (int col = 0; col < 5; col++) {
				for (int row = 0; row < 8; row++) {
					if (FONT_5X7[index][col] >> row & 1) {
						fill(g, x + col * scale, y + row * scale, scale, scale, color);
					}
				}
			}
		}
		x += 6 * scale;
	}
	return x;
}

// Text ending at the right edge, cut to the room it has.
static void text_right(mdmi_gui *g, int right, int y, const char *string, uint32_t color, int scale, int room)
{
	char cut[64];
	snprintf(cut, sizeof(cut), "%s", string);
	int fit = room / (6 * scale);
	if ((int)strlen(cut) > fit) {
		cut[fit > 0 ? fit : 0] = 0;
	}
	text(g, right - (int)strlen(cut) * 6 * scale, y, cut, color, scale);
}

static void button(mdmi_gui *g, int x, int w, const char *label, bool on)
{
	fill(g, x, BUTTON_Y, w, BUTTON_H, on ? C_TEXT : C_CELL);
	int width = (int)strlen(label) * 12;
	text(g, x + (w - width) / 2, BUTTON_Y + 5, label, on ? C_BG : C_LABEL, 2);
}

static bool channel_is_fm(int channel)
{
	return channel < PATCH_NUM_FM_CHANNELS;
}

static int row_y(int channel)
{
	return channel_is_fm(channel) ? RAIL_FM_Y + channel * ROW_H : RAIL_PSG_Y + (channel - PATCH_NUM_FM_CHANNELS) * ROW_H;
}

static void note_name(int key, char *out, size_t size)
{
	static const char *const names[12] = {"C-", "C#", "D-", "D#", "E-", "F-", "F#", "G-", "G#", "A-", "A#", "B-"};
	snprintf(out, size, "%s%d", names[key % 12], key / 12 - 1);
}

static bool is_dac(const patch_values *values, int channel)
{
	return channel == PATCH_NUM_FM_CHANNELS - 1 && values->globals[GLOBAL_DAC];
}

// A channel an FM file can go to.
static bool takes_fm(const patch_values *values, int channel)
{
	return channel_is_fm(channel) && !is_dac(values, channel);
}

// A channel a file of its kind can go to: an FM file, or an EEF on PSG (D28).
static bool takes_files(const patch_values *values, int channel)
{
	return takes_fm(values, channel) || !channel_is_fm(channel);
}

static bool takes_kind(const patch_values *values, int channel, int kind)
{
	return kind == FILES_INSTRUMENT ? takes_fm(values, channel) : kind == FILES_ENVELOPE && !channel_is_fm(channel);
}

// The name of what the channel plays: its voice's or its envelope's.
static const char *sound_name(const patch_values *values, int channel)
{
	return channel_is_fm(channel) ? values->names[channel] : values->psg[channel - PATCH_NUM_FM_CHANNELS].name;
}

// The name of a place in the list a channel chooses from.
static const char *place_name(const patch_bank *bank, bool envelopes, int place)
{
	place &= PATCH_BANK_SIZE - 1;
	return envelopes ? bank->envelopes[place].name : bank->places[place].name;
}

// The channel plays a file dropped on it rather than its bank place.
static bool plays_own_file(mdmi_gui *g, const patch_values *values, int channel)
{
	const patch_bank *bank = patch_bank_view(g->source.patch);
	return strcmp(sound_name(values, channel), place_name(bank, !channel_is_fm(channel), values->channels[channel][PARAM_PRESET])) != 0;
}

static void say(mdmi_gui *g, const char *message, uint32_t color, int ticks)
{
	snprintf(g->message, sizeof(g->message), "%s", message);
	g->message_color = color;
	g->message_ticks = ticks;
}

static int value_of(mdmi_gui *g, const patch_values *values, uint32_t id)
{
	if (g->dragging && id == g->drag_id) {
		return g->drag_value;
	}
	int channel, param;
	patch_id_split(id, &channel, &param);
	return channel < 0 ? values->globals[param] : values->channels[channel][param];
}

// The channel's sound is tweaked against what it got, its bank place or a dropped file: a * after
// the name (D31). From the frame's own snapshot: a second patch_view in a frame could hand its
// buffer to the writer.
static bool sound_changed(mdmi_gui *g, const patch_values *values, int channel)
{
	(void)g;
	return takes_files(values, channel) && patch_sound_changed(values, channel_is_fm(channel) ? NULL : &values->psg[channel - PATCH_NUM_FM_CHANNELS], channel);
}

// Channel 3 in Ch3 Special Mode: its operators take notes of their own.
static bool is_special(const patch_values *values, int channel)
{
	return channel == 2 && values->globals[GLOBAL_CH3_MODE];
}

// The rail's level bar (stage 9): 16 segments, the channel's volume (CC7) in its dark colour, and
// the part a note gets, CC7 x velocity / 127, green while it is held and grey after. That is only
// what the plugin sent: the ROM has no velocity curve of its own (D29).
#define BAR_X 54
#define BAR_SEGMENTS 16
// FM SP's four notes in a column at the rail's right, one note's width: the bar ends where a
// one-note row's does. The name gives up two letters to keep clear of the column.
#define SPECIAL_NOTE_Y 2
#define SPECIAL_NOTE_PITCH 9
#define SPECIAL_NOTES_W 18
#define SPECIAL_NAME_CHARS 11
static void level_bar(mdmi_gui *g, int y, int right, int volume, int velocity, bool seen, bool held, uint32_t dark)
{
	int pitch = (right - BAR_X) / BAR_SEGMENTS;
	pitch = pitch < 3 ? 3 : pitch;
	// A note that came lights one segment at least.
	int lit = (volume * velocity * BAR_SEGMENTS + 127 * 127 / 2) / (127 * 127);
	lit = !seen ? 0 : lit < 1 ? 1 : lit;
	const int shown = (volume * BAR_SEGMENTS + 63) / 127;
	for (int k = 0; k < BAR_SEGMENTS; k++) {
		const uint32_t color = k < lit ? (held ? C_PLAYING : C_HINT) : k < shown ? dark : C_CELL;
		fill(g, BAR_X + k * pitch, y, pitch - 1, 4, color);
	}
}

// One rail row (0A): the number x3 beside the name and the bar, the name x2 above with 3 px to the
// line below it, the bar and the note x1 on that line. Heights and row_y stay: drops, hover and brackets use them.
static void draw_row(mdmi_gui *g, const patch_values *values, int channel)
{
	const int y = row_y(channel);
	const bool dac = is_dac(values, channel);
	const bool special = is_special(values, channel);
	const bool selected = *g->source.channel == channel;
	const uint32_t kind = dac ? C_DAC : special ? C_FM_LIGHT : channel_is_fm(channel) ? C_FM : C_PSG;
	const uint32_t dark = dac ? C_DAC_DARK : channel_is_fm(channel) ? C_FM_DARK : C_PSG_DARK;
	// While a file is over the window: the row it would go to is lit, rows it can't go to are dim.
	const bool target = g->file_over && g->drop_channel == channel;
	const bool dim = g->file_over && !target && (g->file_count != 1 || !takes_kind(values, channel, g->file_kind));
	const bool flash = g->flash_ticks > 0 && g->flash_channel == channel;
#define INK(color) (target ? C_BG : dim ? C_LINE : (color))

	fill(g, 0, y, RAIL_W, ROW_H - 1, target ? C_AMBER : flash && g->flash_color == C_LINE ? C_LINE : C_BG);
	fill(g, 0, y + ROW_H - 1, RAIL_W, 1, C_CELL);
	if (selected && !target && !flash) {
		fill(g, 4, y, RAIL_W - 3, ROW_H - 1, dark);
	}
	fill(g, 0, y, 4, ROW_H - 1, kind);

	char buffer[64];
	snprintf(buffer, sizeof(buffer), "%d", channel + 1);
	text(g, 10, y + 7, buffer, INK(special ? C_FM_LIGHT : C_TEXT), 3);
	if (dac) {
		text(g, BAR_X, y + 5, "Samples", INK(C_TEXT), 2);
	} else {
		snprintf(buffer, sizeof(buffer), "%.*s%s", special ? SPECIAL_NAME_CHARS : 13, sound_name(values, channel), sound_changed(g, values, channel) ? "*" : "");
		text(g, BAR_X, y + 5, buffer, INK(C_TEXT), 2);
	}
#undef INK
	if (g->file_over && !target && !dim) {
		frame(g, 0, y, RAIL_W, ROW_H - 1, C_AMBER);
		frame(g, 1, y + 1, RAIL_W - 2, ROW_H - 3, C_AMBER);
	} else if (flash && g->flash_color == C_ERROR) {
		frame(g, 0, y, RAIL_W, ROW_H - 1, C_ERROR);
		frame(g, 1, y + 1, RAIL_W - 2, ROW_H - 3, C_ERROR);
	}
	if (target) {
		return;
	}

	// The selected row's ground is the dark colour: its volume is drawn in the channel's own. Channel
	// 3 in Ch3 Special Mode is light FM all over: its stripe, number and volume.
	const uint32_t volume_ink = selected || special ? kind : dark;
	// The bottom line, right to left: the note at 226, then how many are held, then how many are cut.
	const int line = y + 22, bar_y = y + 25;
	int left = RAIL_W - 6;
	const uint32_t note = __atomic_load_n(&g->source.notes[channel], __ATOMIC_ACQUIRE);
	const bool seen = GUI_NOTE_SEEN(note);
	const int held = seen ? (int)GUI_NOTE_HELD(note) : 0;
	if (special) {
		// A column at the right where other rows have their note: the lead (operator 4, channel 3
		// itself) on top and green, operators 1-3 (MIDI 11-13) below it, dimmer. The bar stays as
		// long as on a row with one note (owner's note on 0.9.0).
		static const int SOURCES[4] = {2, 10, 11, 12};
		for (int k = 0; k < 4; k++) {
			const uint32_t op = __atomic_load_n(&g->source.notes[SOURCES[k]], __ATOMIC_ACQUIRE);
			const bool op_held = GUI_NOTE_SEEN(op) && GUI_NOTE_HELD(op);
			if (GUI_NOTE_SEEN(op)) {
				note_name((int)GUI_NOTE_KEY(op), buffer, sizeof(buffer));
			} else {
				snprintf(buffer, sizeof(buffer), "---");
			}
			text_right(g, left, y + SPECIAL_NOTE_Y + k * SPECIAL_NOTE_PITCH, buffer, !GUI_NOTE_SEEN(op) ? C_LINE : !op_held ? C_HINT : k == 0 ? C_PLAYING : C_LABEL, 1, SPECIAL_NOTES_W);
		}
		level_bar(g, bar_y, left - SPECIAL_NOTES_W - 6, values->channels[channel][PARAM_VOLUME], (int)GUI_NOTE_VELOCITY(note), seen, held > 0, volume_ink);
		return;
	}
	if (dac) {
		// The DAC hears neither volume nor velocity: the bar only says a sample was started.
		if (seen) {
			note_name((int)GUI_NOTE_KEY(note), buffer, sizeof(buffer));
			text_right(g, left, line, buffer, held ? C_PLAYING : C_HINT, 1, 40);
		}
		fill(g, BAR_X, bar_y, 16, 4, held ? C_PLAYING : C_CELL);
		return;
	}
	// A row whose voice another channel's chord plays shows that note and whose it is, with the
	// player's volume and that note's velocity.
	const bool fm = channel_is_fm(channel);
	const int player = fm ? patch_voice_player(g->source.patch, channel) : -1;
	const bool lent = player >= 0 && player != channel && !(seen && held);
	int volume = values->channels[channel][PARAM_VOLUME], velocity = (int)GUI_NOTE_VELOCITY(note);
	bool bar_seen = seen, bar_held = held > 0;
	if (lent) {
		note_name(patch_voice_key(g->source.patch, channel), buffer, sizeof(buffer));
		left -= (int)strlen(buffer) * 6;
		text(g, left, line, buffer, C_PLAYING, 1);
		snprintf(buffer, sizeof(buffer), "CH%d", player + 1);
		left -= 6 + (int)strlen(buffer) * 6;
		text(g, left, line, buffer, C_LABEL, 1);
		volume = values->channels[player][PARAM_VOLUME];
		velocity = patch_voice_velocity(g->source.patch, channel);
		bar_seen = bar_held = true;
	} else {
		// In a chord the last note went to a lent voice; the row shows the key its own voice plays.
		const bool own = held && player == channel;
		if (seen) {
			note_name(own ? patch_voice_key(g->source.patch, channel) : (int)GUI_NOTE_KEY(note), buffer, sizeof(buffer));
		} else {
			snprintf(buffer, sizeof(buffer), "---");
		}
		left -= (int)strlen(buffer) * 6;
		text(g, left, line, buffer, !seen ? C_LINE : held ? C_PLAYING : C_HINT, 1);
		if (own) {
			velocity = patch_voice_velocity(g->source.patch, channel);
		}
		if (held > 1) {
			snprintf(buffer, sizeof(buffer), "x%d", held);
			left -= 6 + (int)strlen(buffer) * 6;
			text(g, left, line, buffer, C_PLAYING, 1);
		}
		// Held notes without a voice: cut off by a tune, or more than the free voices (D20).
		const int muted = seen ? patch_muted_notes(g->source.patch, channel) : 0;
		if (muted > 0) {
			snprintf(buffer, sizeof(buffer), "%d CUT", muted);
			left -= 6 + (int)strlen(buffer) * 6;
			text(g, left, line, buffer, C_ERROR, 1);
		}
	}
	level_bar(g, bar_y, left - 6, volume, velocity, bar_seen, bar_held, volume_ink);
}

// Brackets from a channel playing a chord to the rows whose voices it borrowed, between the big
// numbers and the names. Green like a held note: those voices sound. The bracket spanning fewer
// rows sits nearest the names, so nested ones don't cross.
#define LEND_LANE_X 44
#define LEND_LANE_STEP 7
#define LEND_TIP_X 47
static void draw_lends(mdmi_gui *g)
{
	int top[PATCH_NUM_FM_CHANNELS], bottom[PATCH_NUM_FM_CHANNELS];
	bool lends[PATCH_NUM_FM_CHANNELS] = {false};
	int player[PATCH_NUM_FM_CHANNELS];
	for (int c = 0; c < PATCH_NUM_FM_CHANNELS; c++) {
		top[c] = bottom[c] = c;
	}
	for (int voice = 0; voice < PATCH_NUM_FM_CHANNELS; voice++) {
		int c = player[voice] = patch_voice_player(g->source.patch, voice);
		if (c >= 0 && c < PATCH_NUM_FM_CHANNELS && c != voice) {
			lends[c] = true;
			top[c] = voice < top[c] ? voice : top[c];
			bottom[c] = voice > bottom[c] ? voice : bottom[c];
		}
	}
	for (int c = 0; c < PATCH_NUM_FM_CHANNELS; c++) {
		if (!lends[c]) {
			continue;
		}
		int lane = 0;
		for (int other = 0; other < PATCH_NUM_FM_CHANNELS; other++) {
			int span = bottom[c] - top[c], other_span = bottom[other] - top[other];
			lane += other != c && lends[other] && (other_span < span || (other_span == span && other < c));
		}
		const int x = LEND_LANE_X - lane * LEND_LANE_STEP;
		const int y_top = row_y(top[c]) + ROW_H / 2, y_bottom = row_y(bottom[c]) + ROW_H / 2;
		fill(g, x, y_top, 2, y_bottom - y_top + 1, C_PLAYING);
		for (int row = top[c]; row <= bottom[c]; row++) {
			if (row != c && player[row] != c) {
				continue;
			}
			const int y = row_y(row) + ROW_H / 2;
			if (row == c) {
				// Where the chord comes from: a square on the spine, no arrow.
				fill(g, x - 2, y - 3, 6, 7, C_PLAYING);
				continue;
			}
			// Where it goes: a branch with an arrowhead into the row.
			fill(g, x, y, LEND_TIP_X - x, 1, C_PLAYING);
			for (int i = 0; i < 5; i++) {
				fill(g, LEND_TIP_X + i, y - 4 + i, 1, 9 - 2 * i, C_PLAYING);
			}
		}
	}
}

static void add_cell(mdmi_gui *g, int x, int y, int w, int h, uint32_t id, const char *label, int kind)
{
	if (g->cell_count < MAX_CELLS) {
		g->cells[g->cell_count++] = (cell){x, y, w, h, id, label, kind, -1};
	}
}

// Operator fields in the order the page shows them: level and pitch, times, the rest.
static const struct {
	const char *label;
	int field;
	int y;
} OP_CELLS[PATCH_OP_PARAMS] = {
	{"TL", 0, 220}, {"MUL", 1, 241}, {"DT", 2, 262},
	{"AR", 4, 287}, {"DR", 5, 308}, {"SL", 7, 329}, {"SR", 6, 350}, {"RR", 8, 371},
	{"RS", 3, 396}, {"AM", 9, 417}, {"SSG", 10, 438},
};

// The cells of the open page and the global knobs, so drawing and the mouse agree.
static void layout(mdmi_gui *g, const patch_values *values)
{
	// One row of the channel's cells for every kind of channel: the column and the row of each. PAN
	// is buttons; PSG has none, its place stays (draw_page).
	static const struct {
		int param;
		const char *label;
		int column, y;
	} CHANNEL_CELLS[] = {
		{PARAM_VOLUME, "VOL", 0, GRID_ROW1_Y}, {PARAM_FINE_TUNE, "TUNE", 2, GRID_ROW1_Y}, {PARAM_BEND_RANGE, "BEND", 3, GRID_ROW1_Y},
		{PARAM_PORTA_ON, "PORTA", 0, GRID_ROW2_Y}, {PARAM_PORTA_TIME, "P.TIME", 1, GRID_ROW2_Y},
	}, FM_CELLS[] = {
		{PARAM_FEEDBACK, "FB", 2, CHIP_Y}, {PARAM_AMS, "AMS", 3, CHIP_Y}, {PARAM_FMS, "FMS", 2, CHIP_Y + 22},
	};
	static const char *const PANS[3] = {"L", "C", "R"};
	static const char *const DIGITS[8] = {"0", "1", "2", "3", "4", "5", "6", "7"};
	static const struct {
		int global;
		const char *label;
	} GLOBAL_CELLS[] = {
		{GLOBAL_LFO_ON, "LFO"}, {GLOBAL_LFO_RATE, "LFO RATE"},
		{GLOBAL_FILTER, "FILTER"}, {GLOBAL_CH3_MODE, "CH3 SPECIAL"}, {GLOBAL_DAC, "DAC"},
	};
	g->cell_count = 0;
	const int channel = *g->source.channel;
	const bool bank = *g->source.page == PAGE_BANK;
	if (*g->source.page == PAGE_ALL) {
		// Per row: VOL, PAN L C R on FM, the source dragged like the page's.
		for (int c = 0; c < PATCH_NUM_CHANNELS; c++) {
			const int y = row_y(c);
			if (is_dac(values, c)) {
				continue;
			}
			add_cell(g, ALL_VOL_X, y + 4, GRID_W, CELL_H, PATCH_CHANNEL_ID(c, PARAM_VOLUME), "", CELL_ROW);
			if (patch_channel_has(c, PARAM_PAN)) {
				for (int pan = 0; pan < 3; pan++) {
					add_cell(g, ALL_PAN_X + pan * (ALL_PAN_W + 1), y + 4, ALL_PAN_W, CELL_H, PATCH_CHANNEL_ID(c, PARAM_PAN), PANS[pan], CELL_CHOICE);
					g->cells[g->cell_count - 1].choice = pan;
				}
			}
			add_cell(g, ALL_SOURCE_X, y + 4, PAGE_RIGHT - ALL_SOURCE_X, 22, PATCH_CHANNEL_ID(c, PARAM_PRESET), "PRESET", CELL_SOURCE);
		}
	} else if (!is_dac(values, channel)) {
		// The Preset is the source at the name line's right end, dragged like any cell.
		add_cell(g, SOURCE_X, 30, SOURCE_W, 22, PATCH_CHANNEL_ID(channel, PARAM_PRESET), "PRESET", CELL_SOURCE);
	}
	if (!is_dac(values, channel) && !bank && *g->source.page != PAGE_ALL) {
		for (size_t i = 0; i < sizeof(CHANNEL_CELLS) / sizeof(*CHANNEL_CELLS); i++) {
			add_cell(g, GRID_X[CHANNEL_CELLS[i].column], CHANNEL_CELLS[i].y, GRID_W, CELL_H, PATCH_CHANNEL_ID(channel, CHANNEL_CELLS[i].param), CHANNEL_CELLS[i].label, CELL_ROW);
		}
		if (patch_channel_has(channel, PARAM_PAN)) {
			for (int pan = 0; pan < 3; pan++) {
				add_cell(g, GRID_X[1] + 28 + pan * 28, GRID_ROW1_Y, 26, CELL_H, PATCH_CHANNEL_ID(channel, PARAM_PAN), PANS[pan], CELL_CHOICE);
				g->cells[g->cell_count - 1].choice = pan;
			}
		}
		if (channel_is_fm(channel)) {
			for (size_t i = 0; i < sizeof(FM_CELLS) / sizeof(*FM_CELLS); i++) {
				add_cell(g, GRID_X[FM_CELLS[i].column], FM_CELLS[i].y, GRID_W, CELL_H, PATCH_CHANNEL_ID(channel, FM_CELLS[i].param), FM_CELLS[i].label, CELL_ROW);
			}
			for (int algorithm = 0; algorithm < 8; algorithm++) {
				add_cell(g, ALGORITHM_X + algorithm * 29, ALGORITHM_Y, 26, CELL_H, PATCH_CHANNEL_ID(channel, PARAM_ALGORITHM), DIGITS[algorithm], CELL_CHOICE);
				g->cells[g->cell_count - 1].choice = algorithm;
			}
			for (int op = 0; op < PATCH_OPS; op++) {
				for (int i = 0; i < PATCH_OP_PARAMS; i++) {
					int param = PARAM_OP_BASE + op * PATCH_OP_PARAMS + OP_CELLS[i].field;
					add_cell(g, GRID_X[op], OP_CELLS[i].y, OP_W, CELL_H, PATCH_CHANNEL_ID(channel, param), OP_CELLS[i].label, CELL_ROW);
				}
			}
		}
	}
	for (int i = 0; i < (int)(sizeof(GLOBAL_CELLS) / sizeof(*GLOBAL_CELLS)); i++) {
		add_cell(g, 8 + i * 118, GLOBALS_Y + 6, 114, 32, (uint32_t)GLOBAL_CELLS[i].global, GLOBAL_CELLS[i].label, CELL_COLUMN);
	}
}

static void draw_cell_body(mdmi_gui *g, const cell *c, const patch_values *values)
{
	const bool held = g->dragging && g->drag_id == c->id;
	const int value = value_of(g, values, c->id);
	char buffer[64];
	int channel, param;
	patch_id_split(c->id, &channel, &param);
	// What the channel got has another value here (D31): amber, and a mark where the base's value is.
	const int base = channel >= 0 ? patch_base_value(values, channel, param) : -1;
	const bool changed = base >= 0 && value != base;
	if (c->kind == CELL_CHOICE) {
		// The algorithm lit in FM's colour, the pan in white as a switch.
		const bool on = value == c->choice, pan = param == PARAM_PAN;
		fill(g, c->x, c->y, c->w, c->h, on ? (pan ? C_TEXT : C_FM) : C_CELL);
		text(g, c->x + (c->w - 11) / 2, c->y + 2, c->label, on ? (pan ? C_BG : C_TEXT) : C_LABEL, 2);
		if (changed && c->choice == base) {
			fill(g, c->x, c->y + c->h - 2, c->w, 2, C_AMBER);
		}
		return;
	}
	if (c->kind == CELL_SOURCE) {
		// Where the sound comes from: a GM preset, a ROM envelope, or a place a file replaced; a file
		// dropped on the channel says FILE over the place the next preset step goes back to.
		const patch_bank *bank = patch_bank_view(g->source.patch);
		const bool from_rom = channel_is_fm(channel) ? patch_place_is_gm(bank, value) : patch_envelope_is_rom(bank, value);
		snprintf(buffer, sizeof(buffer), "%s %d", from_rom ? (channel_is_fm(channel) ? "GM" : "ROM") : "BANK", value + 1);
		const int right = c->x + c->w - 2;
		if (plays_own_file(g, values, channel)) {
			text_right(g, right, c->y + 2, "FILE", held ? C_TEXT : C_LABEL, 1, c->w);
			text_right(g, right, c->y + 12, buffer, held ? C_TEXT : C_LABEL, 1, c->w);
		} else {
			text_right(g, right, c->y + 4, buffer, held ? C_TEXT : C_LABEL, 2, c->w);
		}
		if (held) {
			frame(g, c->x, c->y, c->w, c->h, C_TEXT);
		}
		return;
	}
	// The cell's own channel: ALL shows them all.
	const bool fm = channel >= 0 && channel_is_fm(channel);
	patch_format(patch_bank_view(g->source.patch), c->id, value, buffer, sizeof(buffer));
	uint32_t background = held ? C_LINE : c->kind == CELL_COLUMN ? C_CELL : fm ? C_FM_DARK : C_PSG_DARK;
	fill(g, c->x, c->y, c->w, c->h, background);
	const uint32_t label = held ? C_TEXT : changed ? C_AMBER : C_LABEL;
	uint32_t shown = C_TEXT;
	if (c->id == GLOBAL_DAC && value) {
		shown = C_DAC;
	}
	if (c->kind == CELL_COLUMN) {
		text(g, c->x + 4, c->y + 4, c->label, label, 1);
		buffer[(c->w - 8) / 12] = 0;
		text(g, c->x + 4, c->y + 14, buffer, shown, 2);
	} else {
		int label_end = text(g, c->x + 4, c->y + 6, c->label, label, 1);
		text_right(g, c->x + c->w - 4, c->y + 2, buffer, shown, 2, c->x + c->w - 4 - label_end - 6);
		if (changed) {
			const int max = patch_param(c->id)->max;
			fill(g, c->x + (c->w - 2) * base / (max > 0 ? max : 1), c->y + c->h - 4, 2, 4, C_AMBER);
		}
	}
	if (held) {
		frame(g, c->x, c->y, c->w, c->h, C_TEXT);
	}
}

// A value from the host lately (D32): a red dot 3x3 at the cell's top right, on the last button
// of a row of choices.
static void draw_cell(mdmi_gui *g, const cell *c, const patch_values *values)
{
	draw_cell_body(g, c, values);
	int channel, param;
	if (!patch_id_split(c->id, &channel, &param) || g->host_until[GUI_HOST_SLOT(channel, param)] <= GetTickCount64()) {
		return;
	}
	if (c->kind == CELL_CHOICE && c->choice != patch_param(c->id)->max) {
		return;
	}
	fill(g, c->x + c->w - 4, c->y + 1, 3, 3, C_ERROR);
}

// Operator positions in the diagram (column, row), what modulates what, and which are carriers.
static const int8_t ALGORITHM_POS[8][4][2] = {
	{{0, 1}, {1, 1}, {2, 1}, {3, 1}},
	{{1, 0}, {1, 2}, {2, 1}, {3, 1}},
	{{2, 0}, {1, 1}, {2, 1}, {3, 1}},
	{{1, 1}, {2, 1}, {2, 2}, {3, 1}},
	{{0, 1}, {1, 1}, {2, 1}, {3, 1}},
	{{0, 0}, {1, 1}, {2, 1}, {3, 1}},
	{{0, 1}, {1, 1}, {2, 1}, {3, 1}},
	{{0, 1}, {1, 1}, {2, 1}, {3, 1}},
};
static const int8_t ALGORITHM_LINKS[8][3][2] = {
	{{0, 1}, {1, 2}, {2, 3}},
	{{0, 2}, {1, 2}, {2, 3}},
	{{0, 3}, {1, 2}, {2, 3}},
	{{0, 1}, {1, 3}, {2, 3}},
	{{0, 1}, {2, 3}, {-1, -1}},
	{{0, 1}, {0, 2}, {0, 3}},
	{{0, 1}, {-1, -1}, {-1, -1}},
	{{-1, -1}, {-1, -1}, {-1, -1}},
};
static const uint8_t CARRIERS[8] = {8, 8, 8, 8, 10, 14, 14, 15};

static void op_xy(int algorithm, int op, int *x, int *y)
{
	*x = DIAGRAM_X + 16 + ALGORITHM_POS[algorithm][op][0] * 52;
	*y = DIAGRAM_Y + 5 + ALGORITHM_POS[algorithm][op][1] * 26;
}

static void draw_diagram(mdmi_gui *g, int algorithm, int feedback)
{
	fill(g, DIAGRAM_X, DIAGRAM_Y, 222, 92, C_BG);
	frame(g, DIAGRAM_X, DIAGRAM_Y, 222, 92, C_CELL);
	const int bus = DIAGRAM_Y + 87;
	int leftmost = DIAGRAM_X + 214;
	for (int link = 0; link < 3; link++) {
		int from = ALGORITHM_LINKS[algorithm][link][0], to = ALGORITHM_LINKS[algorithm][link][1];
		if (from < 0) {
			continue;
		}
		int fx, fy, tx, ty;
		op_xy(algorithm, from, &fx, &fy);
		op_xy(algorithm, to, &tx, &ty);
		if (fy == ty) {
			fill(g, fx + 24, fy + 11, tx - fx - 24, 2, C_LABEL);
		} else {
			fill(g, fx + 24, fy + 11, tx + 11 - fx - 24 + 2, 2, C_LABEL);
			if (ty > fy) {
				fill(g, tx + 11, fy + 11, 2, ty - fy - 11, C_LABEL);
			} else {
				fill(g, tx + 11, ty + 24, 2, fy + 11 - ty - 24 + 2, C_LABEL);
			}
		}
	}
	for (int op = 0; op < PATCH_OPS; op++) {
		int x, y;
		op_xy(algorithm, op, &x, &y);
		if (CARRIERS[algorithm] >> op & 1) {
			fill(g, x + 11, y + 24, 2, bus - y - 24, C_LABEL);
			leftmost = x + 11 < leftmost ? x + 11 : leftmost;
		}
	}
	fill(g, leftmost, bus, DIAGRAM_X + 214 - leftmost, 2, C_LABEL);
	int x, y;
	op_xy(algorithm, 0, &x, &y);
	const uint32_t loop = feedback ? C_LABEL : C_LINE;
	fill(g, x - 6, y - 4, 2, 16, loop);
	fill(g, x - 6, y - 4, 36, 2, loop);
	fill(g, x + 29, y - 4, 2, 16, loop);
	fill(g, x - 6, y + 11, 6, 2, loop);
	for (int op = 0; op < PATCH_OPS; op++) {
		op_xy(algorithm, op, &x, &y);
		char digit[2] = {(char)('1' + op), 0};
		if (CARRIERS[algorithm] >> op & 1) {
			fill(g, x, y, 24, 24, C_FM);
			text(g, x + 6, y + 4, digit, C_TEXT, 2);
		} else {
			fill(g, x, y, 24, 24, C_BG);
			frame(g, x, y, 24, 24, C_FM_LIGHT);
			text(g, x + 6, y + 4, digit, C_FM_LIGHT, 2);
		}
	}
}

// The channel's number, the name of what it plays with its *, and the source at the right end (its
// cell): one line for the editor and the bank.
static void draw_page_top(mdmi_gui *g, const patch_values *values)
{
	const int channel = *g->source.channel;
	char buffer[64];
	fill(g, RAIL_W + 1, HEADER_H, GUI_WIDTH - RAIL_W - 1, GLOBALS_Y - HEADER_H, C_BG);
	snprintf(buffer, sizeof(buffer), "%d", channel + 1);
	text(g, PAGE_X, 30, buffer, C_TEXT, 3);
	if (takes_files(values, channel)) {
		snprintf(buffer, sizeof(buffer), "%.*s%s", NAME_CHARS, sound_name(values, channel), sound_changed(g, values, channel) ? "*" : "");
		text(g, NAME_X, 34, buffer, C_TEXT, 2);
	}
}

// The pitch lane's rows by value, +5 semitones at the top: the code each row draws.
static const uint8_t PITCH_CODES[15] = {7, 6, 5, 4, 3, 2, 1, 0, 8, 9, 10, 11, 12, 13, 14};

static int pitch_row(uint8_t frame)
{
	for (int row = 0; row < 15; row++) {
		if (PITCH_CODES[row] == frame >> 4) {
			return row;
		}
	}
	// Code F shifts nothing, as 0 does.
	return 7;
}

static void pitch_text(uint8_t frame, char *out, size_t size)
{
	static const char *const UP[8] = {"0", "+5C", "+10C", "+20C", "+50C", "+1 ST", "+2 ST", "+5 ST"};
	const int code = frame >> 4;
	snprintf(out, size, "%s", code >= 8 && code <= 14 ? UP[code - 7] : UP[code < 8 ? code : 0]);
	if (code >= 8 && code <= 14) {
		out[0] = '-';
	}
}

static int frame_ms(int frame)
{
	return (frame * 1000 + 30) / 60;
}

static int loop_end_of(const patch_shape *shape)
{
	return shape->loop_end < 0 ? shape->count : shape->loop_end;
}

// Columns share the whole lane by the length (0.8.1), their edges rounded down, so the lane ends at
// the page's right edge at every length and widths differ by a pixel at most.
static int column_x(int count, int column)
{
	return ENV_X + column * ENV_LANE_W / count;
}

// The gap pixel before a frame, where the loop lines stand.
static int boundary_x(int count, int boundary)
{
	return column_x(count, boundary) - 1;
}

// The boundary nearest to x, and the column under it (count past the lane's end, -1 before it).
static int boundary_at(int count, int x)
{
	int best = 0;
	for (int b = 1; b <= count; b++) {
		if (abs(column_x(count, b) - 1 - x) < abs(column_x(count, best) - 1 - x)) {
			best = b;
		}
	}
	return best;
}

static int column_at(int count, int x)
{
	int column = -1;
	while (column < count && column_x(count, column + 1) <= x) {
		column++;
	}
	return column;
}

static int clamp(int value, int low, int high)
{
	return value < low ? low : value > high ? high : value;
}

static bool over_length(int x, int y)
{
	return x >= ENV_LENGTH_X && x < ENV_LENGTH_X + ENV_LENGTH_W && y >= ENV_LENGTH_Y && y < ENV_LENGTH_Y + CELL_H;
}

static bool is_envelope_page(const mdmi_gui *g)
{
	return *g->source.page == PAGE_EDITOR && !channel_is_fm(*g->source.channel);
}

// What the editor shows: the window's copy while a gesture lasts, the channel's envelope otherwise.
static const patch_shape *shown_shape(mdmi_gui *g, patch_shape *scratch)
{
	const int channel = *g->source.channel;
	if (g->env_drag != ENV_NONE && g->env_channel == channel) {
		return &g->env_shape;
	}
	patch_shape_of(patch_envelope_view(g->source.patch, channel), scratch);
	return scratch;
}

// The loop switched off on this channel, fitted to the envelope's length now: false when the
// envelope has a loop or none was switched off.
static bool parked_loop(mdmi_gui *g, const patch_shape *shape, int *start, int *end)
{
	const int psg = *g->source.channel - PATCH_NUM_FM_CHANNELS;
	if (psg < 0 || shape->loop_start >= 0 || !g->source.loop_shadows[psg].kept) {
		return false;
	}
	*start = clamp(g->source.loop_shadows[psg].start, 0, shape->count - 1);
	*end = g->source.loop_shadows[psg].end < 0 ? shape->count : clamp(g->source.loop_shadows[psg].end, *start + 1, shape->count);
	return true;
}

// The tab, loop strip, lane or length cell under the mouse. The lanes hold the envelope's frames
// only. The strip is the loop's switch (4A): the loop's body between the tabs turns it off, its
// shadow turns it on again where it was, and with neither the switch at the strip's start loops the
// whole envelope.
static int envelope_part_at(mdmi_gui *g, const patch_shape *shape, int x, int y)
{
	const int count = shape->count;
	if (over_length(x, y)) {
		return ENV_LENGTH;
	}
	if (y >= ENV_LOOP_Y && y < ENV_LOOP_Y + ENV_LOOP_H) {
		int kept_start, kept_end;
		if (parked_loop(g, shape, &kept_start, &kept_end)) {
			// A shadow of one step is a few pixels: the tabs' room on both sides counts.
			return x >= boundary_x(count, kept_start) - 9 && x <= boundary_x(count, kept_end) + 9 ? ENV_LOOP : ENV_NONE;
		}
		if (shape->loop_start < 0) {
			return x >= ENV_X && x < ENV_X + ENV_LOOP_OFF_W ? ENV_LOOP : ENV_NONE;
		}
		const int start = boundary_x(count, shape->loop_start), end = boundary_x(count, loop_end_of(shape));
		return x >= start - 9 && x <= start ? ENV_LOOP_START : x >= end && x <= end + 9 ? ENV_LOOP_END : x > start && x < end ? ENV_LOOP : ENV_NONE;
	}
	if (x < ENV_X || x >= ENV_X + ENV_LANE_W) {
		return ENV_NONE;
	}
	return y >= ENV_LEVEL_Y && y < ENV_LEVEL_BOTTOM ? ENV_LEVEL : y >= ENV_PITCH_Y && y < ENV_PITCH_BOTTOM ? ENV_PITCH : ENV_NONE;
}

static uint8_t with_value(uint8_t frame, int lane, int value)
{
	return lane == ENV_LEVEL ? (uint8_t)((frame & 0xF0) | value) : (uint8_t)(PITCH_CODES[value] << 4 | (frame & 15));
}

// The channel's envelope becomes the window's copy, and what undo puts back.
static void envelope_take(mdmi_gui *g)
{
	g->env_channel = *g->source.channel;
	g->env_before = *patch_envelope_view(g->source.patch, g->env_channel);
	patch_shape_of(&g->env_before, &g->env_shape);
	g->env_changed = g->env_handed = false;
	g->env_last_step = -1;
}

// The copy goes to the model the way a dropped EEF does (D19, D28): at most once per window frame
// and at the end of the gesture, so stores don't pile up in the ROM.
static void envelope_hand(mdmi_gui *g)
{
	if (!g->env_changed) {
		return;
	}
	patch_envelope envelope = g->env_before;
	patch_shape_to_envelope(&g->env_shape, &envelope);
	files_draw_envelope(g->source.files, g->env_channel, g->env_handed ? NULL : &g->env_before, &envelope);
	g->env_handed = true;
	g->env_changed = false;
	if (!g->env_told) {
		say(g, "Drawn envelope plays from the next note", C_TEXT, MESSAGE_TICKS);
		g->env_told = true;
	}
}

static void envelope_move(mdmi_gui *g, int x, int y)
{
	patch_shape *shape = &g->env_shape;
	const patch_shape was = *shape;
	const int boundary = boundary_at(shape->count, x);
	if (g->env_drag == ENV_LEVEL || g->env_drag == ENV_PITCH) {
		const int step = clamp(column_at(shape->count, x), 0, shape->count - 1);
		const int value = g->env_drag == ENV_LEVEL ? clamp((y - ENV_LEVEL_Y) / 10, 0, 15) : clamp((y - ENV_PITCH_Y) / ENV_PITCH_ROW, 0, 14);
		// Columns the mouse jumped over get values on the line between.
		const int from = g->env_last_step < 0 ? step : g->env_last_step;
		const int last = g->env_last_step < 0 ? value : g->env_last_value;
		const int direction = step >= from ? 1 : -1;
		for (int i = from;; i += direction) {
			shape->frames[i] = with_value(shape->frames[i], g->env_drag, step == from ? value : last + (value - last) * (i - from) / (step - from));
			if (i == step) {
				break;
			}
		}
		g->env_last_step = step;
		g->env_last_value = value;
	} else if (g->env_drag == ENV_LOOP_START) {
		shape->loop_start = clamp(boundary, 0, loop_end_of(shape) - 1);
	} else if (g->env_drag == ENV_LOOP_END) {
		const int end = clamp(boundary, shape->loop_start + 1, shape->count);
		shape->loop_end = end >= shape->count ? -1 : end;
	} else if (g->env_drag == ENV_LENGTH) {
		// Up is longer, as on the other cells; from the gesture's start, so frames cut on the way come back.
		const int dy = g->env_start_y - y;
		*shape = g->env_start;
		const int length = clamp(shape->count + (abs(dy) < DRAG_SLOP ? 0 : dy / ENV_LENGTH_PIXELS), 1, patch_shape_room(shape));
		for (; shape->count < length; shape->count++) {
			shape->frames[shape->count] = shape->frames[shape->count - 1];
		}
		shape->count = length;
	}
	patch_envelope scratch;
	patch_shape_to_envelope(shape, &scratch);
	g->env_changed |= memcmp(&was, shape, sizeof(was)) != 0;
}

// A press on the editor: the loop's switch, or the start of a gesture on a lane or a tab.
static bool envelope_down(mdmi_gui *g, int x, int y)
{
	patch_shape scratch;
	const int part = envelope_part_at(g, shown_shape(g, &scratch), x, y);
	if (part == ENV_LOOP) {
		envelope_take(g);
		patch_shape *shape = &g->env_shape;
		const int psg = g->env_channel - PATCH_NUM_FM_CHANNELS;
		// The shadow belongs to the envelope it was switched off on.
		if (strcmp(g->source.loop_shadows[psg].name, g->env_before.name) != 0) {
			g->source.loop_shadows[psg].kept = false;
		}
		int start, end;
		if (shape->loop_start >= 0) {
			// Off: both marks go, the window keeps where they were.
			g->source.loop_shadows[psg].kept = true;
			g->source.loop_shadows[psg].start = shape->loop_start;
			g->source.loop_shadows[psg].end = shape->loop_end;
			snprintf(g->source.loop_shadows[psg].name, sizeof(g->source.loop_shadows[psg].name), "%s", g->env_before.name);
			shape->loop_start = shape->loop_end = -1;
		} else if (parked_loop(g, shape, &start, &end)) {
			// On again where it was.
			shape->loop_start = start;
			shape->loop_end = end >= shape->count ? -1 : end;
		} else {
			// On for the first time: the whole envelope.
			shape->loop_start = 0;
			shape->loop_end = -1;
		}
		patch_envelope scratch;
		patch_shape_to_envelope(shape, &scratch);
		g->env_changed = true;
		envelope_hand(g);
		return true;
	}
	if (part == ENV_NONE) {
		return false;
	}
	envelope_take(g);
	g->env_drag = part;
	g->env_start = g->env_shape;
	g->env_start_y = y;
	if (g->window) {
		SetCapture(g->window);
	}
	envelope_move(g, x, y);
	return true;
}

static void envelope_up(mdmi_gui *g)
{
	envelope_hand(g);
	g->env_drag = ENV_NONE;
	if (GetCapture() == g->window) {
		ReleaseCapture();
	}
}

// What the footer says about the editor: the gesture under way, or what is under the mouse.
static bool envelope_hint(mdmi_gui *g, char *hint, size_t size)
{
	if (!is_envelope_page(g)) {
		return false;
	}
	patch_shape scratch;
	const patch_shape *shape = shown_shape(g, &scratch);
	const int x = g->mouse_x, y = g->mouse_y, room = patch_shape_room(shape);
	const int part = g->env_drag != ENV_NONE ? g->env_drag : envelope_part_at(g, shape, x, y);
	const int step = g->env_drag != ENV_NONE ? g->env_last_step : column_at(shape->count, x);
	int kept_start, kept_end;
	char pitch[16];
	if ((part == ENV_LEVEL || part == ENV_PITCH) && step >= 0 && step < shape->count) {
		if (part == ENV_PITCH && *g->source.channel == PATCH_NUM_CHANNELS - 1) {
			snprintf(hint, size, "CH10: SHIFT CHANGES NOISE, NOT PITCH");
			return true;
		}
		const int level = shape->frames[step] & 15;
		pitch_text(shape->frames[step], pitch, sizeof(pitch));
		char db[12];
		snprintf(db, sizeof(db), level == 15 ? "OFF" : "%d DB", -2 * level);
		snprintf(hint, size, "STEP %d/%d  %d MS  LEVEL %s  PITCH %s", step + 1, shape->count, frame_ms(step), db, pitch);
	} else if (part == ENV_LOOP_START || part == ENV_LOOP_END) {
		const int end = loop_end_of(shape);
		if (g->env_drag != ENV_NONE) {
			snprintf(hint, size, "LOOP %d-%d: %d STEPS, %d MS", shape->loop_start + 1, end, end - shape->loop_start, frame_ms(end - shape->loop_start));
		} else {
			snprintf(hint, size, part == ENV_LOOP_START ? "LOOP START: DRAG BETWEEN STEPS" : "LOOP END: AFTER IT PLAYS ON NOTE OFF");
		}
	} else if (part == ENV_LENGTH && shape->count >= room) {
		snprintf(hint, size, "MAX %d BYTES, LOOP MARKS TAKE %d", PATCH_ENVELOPE_STEPS, PATCH_ENVELOPE_STEPS - room);
	} else if (part == ENV_LENGTH) {
		snprintf(hint, size, g->env_drag != ENV_NONE ? "LENGTH %d STEPS, %d MS" : "LENGTH %d STEPS, %d MS: DRAG UP OR DOWN", shape->count, frame_ms(shape->count));
	} else if (part == ENV_LOOP && shape->loop_start >= 0) {
		snprintf(hint, size, "LOOP %d-%d: CLICK TO TURN OFF", shape->loop_start + 1, loop_end_of(shape));
	} else if (part == ENV_LOOP && shape->count >= room && parked_loop(g, shape, &kept_start, &kept_end)) {
		snprintf(hint, size, "LOOP %d-%d: LAST STEPS CUT, MAX %d BYTES", kept_start + 1, kept_end, PATCH_ENVELOPE_STEPS);
	} else if (part == ENV_LOOP && parked_loop(g, shape, &kept_start, &kept_end)) {
		snprintf(hint, size, "LOOP %d-%d IS OFF: CLICK TO TURN IT ON", kept_start + 1, kept_end);
	} else if (part == ENV_LOOP) {
		snprintf(hint, size, shape->count >= room ? "LOOP: LAST STEP CUT, MAX %d BYTES" : "LOOP: CLICK TO LOOP ALL %d STEPS", shape->count >= room ? PATCH_ENVELOPE_STEPS : shape->count);
	} else {
		return false;
	}
	return true;
}

// A 7x7 sign for a word that doesn't fit: the loop's arrow, or release's falling steps.
static void loop_sign(mdmi_gui *g, int x, int y, uint32_t color)
{
	fill(g, x + 1, y, 5, 1, color);
	fill(g, x, y + 1, 1, 4, color);
	fill(g, x + 6, y + 1, 1, 4, color);
	fill(g, x + 1, y + 5, 4, 1, color);
	fill(g, x + 4, y + 4, 1, 3, color);
	fill(g, x + 3, y + 5, 3, 1, color);
}

static void release_sign(mdmi_gui *g, int x, int y, uint32_t color)
{
	for (int k = 0; k < 4; k++) {
		fill(g, x + k * 2, y + k * 2, 2, 1, color);
	}
	fill(g, x + 7, y + 6, 1, 1, color);
	fill(g, x, y, 1, 7, color);
	fill(g, x, y + 6, 8, 1, color);
}

// A word in the room it has: the full word, the short one, the sign, then nothing (4A, v3 L1).
static void fit_word(mdmi_gui *g, int x, int room, int y, const char *word, const char *shorter, bool loop, uint32_t color, bool centred)
{
	const char *fits = (int)strlen(word) * 6 + 4 <= room ? word : shorter && (int)strlen(shorter) * 6 + 4 <= room ? shorter : NULL;
	if (fits) {
		const int w = (int)strlen(fits) * 6 - 1;
		text(g, centred ? x + (room - w) / 2 : x, y, fits, color, 1);
	} else if (room >= 11) {
		const int left = centred ? x + (room - 7) / 2 : x;
		if (loop) {
			loop_sign(g, left, y, color);
		} else {
			release_sign(g, left, y, color);
		}
	}
}

// A value by the mouse, framed, so drawing needs no look down at the footer.
static void value_tag(mdmi_gui *g, int centre, int y, const char *value)
{
	const int w = (int)strlen(value) * 6 + 5;
	const int x = clamp(centre - w / 2, ENV_X, ENV_X + ENV_LANE_W - w);
	fill(g, x, y, w, 13, C_BG);
	frame(g, x, y, w, 13, C_LABEL);
	text(g, x + 3, y + 3, value, C_TEXT, 1);
}

static void draw_envelope(mdmi_gui *g, const patch_values *values)
{
	const bool noise = *g->source.channel == PATCH_NUM_CHANNELS - 1;
	// Another envelope came (a program change, a file): the shadow of the old one's loop goes.
	const int psg = *g->source.channel - PATCH_NUM_FM_CHANNELS;
	if (g->source.loop_shadows[psg].kept && g->env_drag == ENV_NONE && strcmp(g->source.loop_shadows[psg].name, values->psg[psg].name) != 0) {
		g->source.loop_shadows[psg].kept = false;
	}
	patch_shape scratch;
	const patch_shape *shape = shown_shape(g, &scratch);
	const int room = patch_shape_room(shape), end = loop_end_of(shape), count = shape->count;
	const bool loop = shape->loop_start >= 0, idle = g->env_drag == ENV_NONE;
	const int over = idle ? envelope_part_at(g, shape, g->mouse_x, g->mouse_y) : ENV_NONE;
	int kept_start, kept_end;
	const bool kept = parked_loop(g, shape, &kept_start, &kept_end);
	char buffer[64];

	text_right(g, 292, ENV_LEVEL_Y, "0DB", C_HINT, 1, 20);
	text_right(g, 292, ENV_LEVEL_Y + 70, "-14", C_HINT, 1, 20);
	text_right(g, 292, ENV_LEVEL_Y + 140, "-28", C_HINT, 1, 20);
	text_right(g, 292, ENV_BASE_Y + 2, "OFF", C_HINT, 1, 20);
	text(g, PAGE_X, ENV_PITCH_Y, noise ? "NOISE" : "PITCH", C_LABEL, 1);
	if (!noise) {
		static const struct {
			const char *label;
			int y;
		} PITCH_LABELS[] = {{"+5", 0}, {"+1", 24}, {"0", 42}, {"-1", 60}, {"-5", 84}};
		for (size_t i = 0; i < sizeof(PITCH_LABELS) / sizeof(*PITCH_LABELS); i++) {
			text_right(g, 292, ENV_PITCH_Y + PITCH_LABELS[i].y, PITCH_LABELS[i].label, C_HINT, 1, 20);
		}
	}

	// One column per frame, filling the whole lane at every length: the loop on a lighter ground, a
	// grid of levels behind, a dark body with a bright top.
	const int hover = over == ENV_LEVEL || over == ENV_PITCH ? column_at(count, g->mouse_x) : -1;
	for (int i = 0; i < count; i++) {
		const int x = column_x(count, i), w = column_x(count, i + 1) - x;
		const bool in_loop = loop && i >= shape->loop_start && i < end;
		const uint32_t back = i == hover ? C_LINE : in_loop ? C_CELL : C_BG;
		fill(g, x, ENV_LEVEL_Y, w, ENV_LEVEL_BOTTOM - ENV_LEVEL_Y, back);
		fill(g, x, ENV_PITCH_Y, w, ENV_PITCH_BOTTOM - ENV_PITCH_Y, back);
		for (int k = 1; k < 15; k++) {
			fill(g, x, ENV_LEVEL_Y + 10 * k, w, 1, in_loop || i == hover ? C_LINE : C_CELL);
		}
		const int level = shape->frames[i] & 15, top = ENV_LEVEL_Y + 10 * level;
		if (level < 15) {
			fill(g, x, top + 3, w - 1, ENV_BASE_Y - top - 3, C_PSG_DARK);
			fill(g, x, top, w - 1, 3, C_PSG);
		}
		const int row = pitch_row(shape->frames[i]), pitch_top = ENV_PITCH_Y + row * ENV_PITCH_ROW;
		if (row < 7) {
			fill(g, x, pitch_top, w - 1, ENV_CENTRE_Y - pitch_top, C_PSG);
		} else if (row > 7) {
			fill(g, x, ENV_CENTRE_Y + 1, w - 1, pitch_top + ENV_PITCH_ROW - ENV_CENTRE_Y - 1, C_PSG);
		}
	}
	fill(g, ENV_X, ENV_BASE_Y, ENV_LANE_W - 1, 1, C_LINE);
	fill(g, ENV_X, ENV_CENTRE_Y, ENV_LANE_W - 1, 1, C_LINE);

	// The loop's strip: its body is the switch, the tabs point outward so a loop of one step keeps
	// them apart, LOOP between them and RELEASE over the tail that plays after Note Off.
	if (loop) {
		const int start_x = boundary_x(count, shape->loop_start), end_x = boundary_x(count, end);
		const int body = end_x - start_x - 1;
		fill(g, start_x + 1, ENV_LOOP_Y, body, ENV_LOOP_H, over == ENV_LOOP ? C_LINE : C_CELL);
		fit_word(g, start_x + 1, body, ENV_LOOP_Y + 3, "LOOP", NULL, true, C_PSG, true);
		if (shape->loop_end >= 0) {
			const int tail_x = end_x + 12;
			fit_word(g, tail_x, ENV_X + ENV_LANE_W - tail_x, ENV_LOOP_Y + 3, "RELEASE", "REL", false, C_LABEL, false);
		}
		fill(g, start_x, ENV_LOOP_Y, 1, ENV_PITCH_BOTTOM - ENV_LOOP_Y, C_PSG);
		fill(g, end_x, ENV_LOOP_Y, 1, ENV_PITCH_BOTTOM - ENV_LOOP_Y, C_PSG);
		fill(g, start_x - 8, ENV_LOOP_Y, 8, ENV_LOOP_H, g->env_drag == ENV_LOOP_START ? C_AMBER : over == ENV_LOOP_START ? C_TEXT : C_PSG);
		fill(g, end_x + 1, ENV_LOOP_Y, 8, ENV_LOOP_H, g->env_drag == ENV_LOOP_END ? C_AMBER : over == ENV_LOOP_END ? C_TEXT : C_PSG);
	} else if (kept) {
		// The shadow of the loop switched off: an outline where it was, no tabs and no lines down the
		// lanes, the columns on the plain ground. A click puts it back.
		const int start_x = boundary_x(count, kept_start), end_x = boundary_x(count, kept_end);
		const uint32_t ink = over == ENV_LOOP ? C_TEXT : C_LINE;
		frame(g, start_x, ENV_LOOP_Y, end_x - start_x + 1, ENV_LOOP_H, ink);
		fit_word(g, start_x + 1, end_x - start_x - 1, ENV_LOOP_Y + 3, "LOOP", NULL, true, over == ENV_LOOP ? C_TEXT : C_LABEL, true);
		const int tail_x = end_x + 8;
		if (tail_x + 13 * 6 <= ENV_X + ENV_LANE_W) {
			text(g, tail_x, ENV_LOOP_Y + 3, "NOTE OFF CUTS", C_HINT, 1);
		}
	} else {
		frame(g, ENV_X, ENV_LOOP_Y, ENV_LOOP_OFF_W, ENV_LOOP_H, over == ENV_LOOP ? C_TEXT : C_LINE);
		text(g, ENV_X + 3, ENV_LOOP_Y + 3, "LOOP", over == ENV_LOOP ? C_TEXT : C_LABEL, 1);
		text(g, ENV_X + ENV_LOOP_OFF_W + 8, ENV_LOOP_Y + 3, "NOTE OFF CUTS", C_HINT, 1);
	}

	// The time axis under the lane: frames and ms at 60 Hz, labelled every L frames, the first of
	// 1, 3, 6, 15, 30 that leaves 32 px between labels, short of the length cell at the end.
	static const int STEPS[] = {1, 3, 6, 15, 30};
	int every = 30;
	for (size_t i = 0; i < sizeof(STEPS) / sizeof(*STEPS); i++) {
		if (STEPS[i] * ENV_LANE_W >= 32 * count) {
			every = STEPS[i];
			break;
		}
	}
	const int lane_end = ENV_X + ENV_LANE_W - 1;
	for (int f = 0; f < count; f++) {
		const int x = column_x(count, f);
		const bool labelled = f % every == 0;
		if (labelled || ENV_LANE_W >= 6 * count) {
			fill(g, x, ENV_AXIS_Y, 1, 3, labelled ? C_HINT : C_LINE);
		}
		if (labelled && x + 76 <= lane_end) {
			snprintf(buffer, sizeof(buffer), "%d", f);
			text(g, x - (int)strlen(buffer) * 3, ENV_AXIS_Y + 4, buffer, C_HINT, 1);
			snprintf(buffer, sizeof(buffer), "%d", frame_ms(f));
			text(g, x - (int)strlen(buffer) * 3, ENV_AXIS_Y + 14, buffer, C_HINT, 1);
		}
	}
	fill(g, lane_end, ENV_AXIS_Y, 1, 3, C_LABEL);
	text_right(g, 286, ENV_AXIS_Y + 4, "FRAME", C_HINT, 1, 40);
	text_right(g, 286, ENV_AXIS_Y + 14, "MS", C_HINT, 1, 40);
	// The length is the number at the axis' end, dragged like a value cell. At the limit, 124 bytes
	// with the loop marks, it turns amber and says MAX.
	const bool held = g->env_drag == ENV_LENGTH, full = count >= room;
	fill(g, ENV_LENGTH_X, ENV_LENGTH_Y, ENV_LENGTH_W, CELL_H, held ? C_LINE : C_PSG_DARK);
	if (held || over == ENV_LENGTH) {
		frame(g, ENV_LENGTH_X, ENV_LENGTH_Y, ENV_LENGTH_W, CELL_H, held ? C_TEXT : C_LINE);
	}
	snprintf(buffer, sizeof(buffer), "%d", count);
	text_right(g, ENV_LENGTH_X + ENV_LENGTH_W - 4, ENV_LENGTH_Y + 2, buffer, full ? C_AMBER : C_TEXT, 2, ENV_LENGTH_W - 4);
	if (full) {
		text_right(g, ENV_LENGTH_X - 4, ENV_LENGTH_Y + 6, "MAX", C_AMBER, 1, 20);
	}
	snprintf(buffer, sizeof(buffer), "%d MS", frame_ms(count));
	text_right(g, ENV_X + ENV_LANE_W, ENV_LENGTH_Y + CELL_H + 3, buffer, C_LABEL, 1, 60);

	// The value by the mouse: the level over its bar, the loop's steps by a tab.
	const int part = idle ? over : g->env_drag;
	const int step = idle ? hover : g->env_last_step;
	if (part == ENV_LEVEL && step >= 0 && step < count) {
		const int level = shape->frames[step] & 15;
		const int centre = (column_x(count, step) + column_x(count, step + 1)) / 2;
		snprintf(buffer, sizeof(buffer), level == 15 ? "OFF" : "%dDB", -2 * level);
		// Over the bar, or inside it where the bar reaches the top.
		const int above = ENV_LEVEL_Y + 10 * level - 16;
		value_tag(g, centre, above >= ENV_LEVEL_Y ? above : ENV_LEVEL_Y + 10 * level + 5, buffer);
	} else if (loop && (part == ENV_LOOP_START || part == ENV_LOOP_END)) {
		snprintf(buffer, sizeof(buffer), "%d-%d", shape->loop_start + 1, end);
		value_tag(g, boundary_x(count, part == ENV_LOOP_START ? shape->loop_start : end), ENV_LEVEL_Y + 2, buffer);
	}
}

static void draw_page(mdmi_gui *g, const patch_values *values)
{
	const int channel = *g->source.channel;
	char buffer[64];
	draw_page_top(g, values);
	if (is_dac(values, channel)) {
		static const char *const NOTES[] = {
			"Notes on MIDI channel 6 trigger drum samples.",
			"One sample at a time. Note off stops it.",
			"Volume, pan and pitch do nothing on samples.",
			"The sample page comes in a later stage.",
			"Turn DAC off below to get FM channel 6 back.",
		};
		text(g, PAGE_X, 62, "CH 6 PLAYS SAMPLES (DAC)", C_DAC, 2);
		for (size_t i = 0; i < sizeof(NOTES) / sizeof(*NOTES); i++) {
			text(g, PAGE_X, 90 + (int)i * 12, NOTES[i], C_LABEL, 1);
		}
		return;
	}
	if (!channel_is_fm(channel)) {
		// PSG has no pan; its place stays so TUNE and BEND are where they are on FM.
		frame(g, GRID_X[1], GRID_ROW1_Y, GRID_W, CELL_H, C_CELL);
		text(g, GRID_X[1] + 4, GRID_ROW1_Y + 6, "PAN", C_LINE, 1);
		text_right(g, GRID_X[1] + GRID_W - 4, GRID_ROW1_Y + 2, "-", C_LINE, 2, 20);
		text(g, PAGE_X, 442, channel == PATCH_NUM_CHANNELS - 1 ? "SN76489 NOISE" : "SN76489 SQUARE", C_HINT, 1);
		draw_envelope(g, values);
		return;
	}
	const int algorithm = value_of(g, values, PATCH_CHANNEL_ID(channel, PARAM_ALGORITHM));
	draw_diagram(g, algorithm, value_of(g, values, PATCH_CHANNEL_ID(channel, PARAM_FEEDBACK)));
	text(g, ALGORITHM_X, ALGORITHM_Y + 24, "ALGORITHM", C_HINT, 1);
	// The pan's label, left of its three buttons.
	fill(g, GRID_X[1], GRID_ROW1_Y, 26, CELL_H, C_FM_DARK);
	text(g, GRID_X[1] + 4, GRID_ROW1_Y + 6, "PAN", C_LABEL, 1);
	for (int op = 0; op < PATCH_OPS; op++) {
		const int x = GRID_X[op];
		const bool carrier = CARRIERS[algorithm] >> op & 1;
		snprintf(buffer, sizeof(buffer), "OP%d %s", op + 1, carrier ? "CARRIER" : "MOD");
		if (carrier) {
			fill(g, x, OP_HEADER_Y, OP_W, OP_HEADER_H, C_FM);
		} else {
			frame(g, x, OP_HEADER_Y, OP_W, OP_HEADER_H, C_FM_LIGHT);
		}
		text(g, x + 4, OP_HEADER_Y + 3, buffer, carrier ? C_TEXT : C_FM_LIGHT, 1);
	}
}

static int hovered(const mdmi_gui *g)
{
	for (int i = 0; i < g->cell_count; i++) {
		const cell *c = &g->cells[i];
		if (g->mouse_x >= c->x && g->mouse_x < c->x + c->w && g->mouse_y >= c->y && g->mouse_y < c->y + c->h) {
			return i;
		}
	}
	return -1;
}

static int rail_channel_at(int x, int y)
{
	if (x < 0 || x >= RAIL_W) {
		return -1;
	}
	for (int channel = 0; channel < PATCH_NUM_CHANNELS; channel++) {
		if (y >= row_y(channel) && y < row_y(channel) + ROW_H) {
			return channel;
		}
	}
	return -1;
}

// On ALL a row reaches the page's right edge; elsewhere only the rail's. One geometry for drops,
// hover and clicks (D29).
static int row_at(const mdmi_gui *g, int x, int y)
{
	if (*g->source.page == PAGE_ALL && x >= RAIL_W && x < PAGE_RIGHT) {
		return rail_channel_at(0, y);
	}
	return rail_channel_at(x, y);
}

// ADD is there only on the bank page.
static int action_at(const mdmi_gui *g, int x, int y)
{
	static const int X[ACTIONS] = {ACTION_LOAD_X, ACTION_SAVE_X, ACTION_UNDO_X, ACTION_ADD_X};
	for (int action = 0; action < ACTIONS; action++) {
		if (action == ACTION_ADD && *g->source.page != PAGE_BANK) {
			continue;
		}
		if (y >= BUTTON_Y && y < BUTTON_Y + BUTTON_H && x >= X[action] && x < X[action] + ACTION_W) {
			return action;
		}
	}
	return -1;
}

// What a press of the button under the mouse would do, for the footer.
static bool action_hint(mdmi_gui *g, char *hint, size_t size)
{
	const int action = g->file_over ? -1 : action_at(g, g->mouse_x, g->mouse_y);
	const int channel = *g->source.channel;
	const patch_values *values = patch_view(g->source.patch);
	const char *undo = files_undo_text(g->source.files);
	const bool envelopes = !channel_is_fm(channel);
	const int add = files_add_place(g->source.files, envelopes);
	if (action == ACTION_LOAD && *g->source.page == PAGE_BANK) {
		if (takes_files(values, channel)) {
			snprintf(hint, size, "LOAD: files from slot %d on, bank or snapshot", values->channels[channel][PARAM_PRESET] + 1);
		} else {
			snprintf(hint, size, "LOAD: select another CH for slots");
		}
	} else if (action == ACTION_ADD) {
		if (add < PATCH_BANK_SIZE) {
			snprintf(hint, size, "ADD: files to slots from %d", add + 1);
		} else {
			snprintf(hint, size, envelopes ? "ADD: envelopes full to 128" : "ADD: bank is full to 128");
		}
	} else if (action == ACTION_LOAD) {
		snprintf(hint, size, "LOAD: file to CH%d, bank or snapshot", channel + 1);
	} else if (action == ACTION_SAVE) {
		snprintf(hint, size, envelopes ? "SAVE: CH%d as EEF, the bank or a snapshot" : takes_fm(values, channel) ? "SAVE: CH%d as VGI, the bank or a snapshot" : "SAVE: the bank or a snapshot", channel + 1);
	} else if (action == ACTION_UNDO && undo) {
		snprintf(hint, size, "UNDO: %s", undo);
	} else {
		return false;
	}
	return true;
}

// The cell under the mouse against what the channel got: TL 57, FILE 34 (D31).
static bool base_hint(mdmi_gui *g, char *hint, size_t size)
{
	const int over = g->file_over ? -1 : hovered(g);
	if (over < 0) {
		return false;
	}
	const cell *c = &g->cells[over];
	int channel, param;
	if (!patch_id_split(c->id, &channel, &param) || channel < 0) {
		return false;
	}
	const patch_values *values = patch_view(g->source.patch);
	const int base = patch_base_value(values, channel, param), value = value_of(g, values, c->id);
	if (base < 0 || base == value) {
		return false;
	}
	const patch_bank *bank = patch_bank_view(g->source.patch);
	const char *from = plays_own_file(g, values, channel) ? "FILE" : patch_place_is_gm(bank, values->channels[channel][PARAM_PRESET]) ? "GM" : "BANK";
	char now[24], was[24];
	patch_format(bank, c->id, value, now, sizeof(now));
	patch_format(bank, c->id, base, was, sizeof(was));
	snprintf(hint, size, "%s %s, %s %s", c->kind == CELL_CHOICE ? "ALG" : c->label, now, from, was);
	return true;
}

// Where the build's version starts at the footer's right end; the rest of the line ends before it.
static int version_x(const mdmi_gui *g)
{
	return GUI_WIDTH - 8 - (int)strlen(g->source.version) * 6;
}

static void footer_text(mdmi_gui *g, const char *message, uint32_t color)
{
	char cut[FILES_MESSAGE];
	snprintf(cut, sizeof(cut), "%.*s", (version_x(g) - 16) / 12, message);
	text(g, 8, FOOTER_Y + 4, cut, color, 2);
}

// One line, by what matters most: a dead engine, a file being dragged, the button under the mouse,
// what just happened, then the engine's numbers.
static void draw_footer(mdmi_gui *g)
{
	fill(g, 0, FOOTER_Y, GUI_WIDTH, GUI_HEIGHT - FOOTER_Y, C_BG);
	mdmi_engine_stats stats = mdmi_engine_get_stats(g->source.engine);
	if (++g->ticks >= TIMER_PER_SECOND) {
		g->ticks = 0;
		g->underruns_per_second = stats.underruns - g->underruns_second_ago;
		g->underruns_second_ago = stats.underruns;
	}
	char buffer[160];
	if (stats.failed) {
		if (stats.boot_error) {
			snprintf(buffer, sizeof(buffer), "ENGINE FAILED: %s %u", stats.boot_stage ? stats.boot_stage : "", stats.boot_error);
		} else {
			snprintf(buffer, sizeof(buffer), "ENGINE FAILED: RELOAD DEVICE");
		}
		footer_text(g, buffer, C_ERROR);
	} else if (g->file_over) {
		footer_text(g, g->drop_hint, g->drop_hint_color);
	} else if (action_hint(g, buffer, sizeof(buffer))) {
		footer_text(g, buffer, C_LABEL);
	} else if (g->env_drag != ENV_NONE && envelope_hint(g, buffer, sizeof(buffer))) {
		footer_text(g, buffer, C_LABEL);
	} else if (g->message_ticks > 0) {
		footer_text(g, g->message, g->message_color);
	} else if (base_hint(g, buffer, sizeof(buffer))) {
		footer_text(g, buffer, C_AMBER);
	} else if (envelope_hint(g, buffer, sizeof(buffer))) {
		footer_text(g, buffer, C_LABEL);
	} else {
		snprintf(buffer, sizeof(buffer), "UNDERRUNS %u +%u/S MIDI LOST %u BLOCK %u @ %u RUN %.1f WAIT %.1f MS RING %u SHORT %u BURST %u",
			stats.underruns, g->underruns_per_second, stats.dropped_midi, stats.last_block, stats.sample_rate, stats.worst_run_ms, stats.worst_wait_ms,
			stats.fill_target, stats.worst_short, stats.longest_burst);
		buffer[(version_x(g) - 16) / 6] = 0;
		text(g, 8, FOOTER_Y + 8, buffer, stats.underruns || stats.dropped_midi ? C_AMBER : C_HINT, 1);
	}
	text(g, version_x(g), FOOTER_Y + 8, g->source.version, C_HINT, 1);
}

static int bank_place_at(const mdmi_gui *g, int x, int y)
{
	if (x < BANK_X || x >= BANK_RIGHT || y < BANK_Y || y >= BANK_Y + BANK_ROWS * BANK_ROW_H) {
		return -1;
	}
	return g->bank_top + (y - BANK_Y) / BANK_ROW_H;
}

static void scroll_bank(mdmi_gui *g, int top)
{
	g->bank_top = top < 0 ? 0 : top > BANK_TOP_MAX ? BANK_TOP_MAX : top;
}

// The places of the list the selected channel chooses from, FM voices or envelopes (D28), with its
// place lit and a dot for every channel of the list on it.
static void draw_bank(mdmi_gui *g, const patch_values *values)
{
	const int channel = *g->source.channel;
	const bool usable = takes_files(values, channel);
	const bool envelopes = !channel_is_fm(channel);
	const uint32_t kind = envelopes ? C_PSG : C_FM, dark = envelopes ? C_PSG_DARK : C_FM_DARK, light = envelopes ? C_PSG : C_FM_LIGHT;
	const int first_channel = envelopes ? PATCH_NUM_FM_CHANNELS : 0, last_channel = envelopes ? PATCH_NUM_CHANNELS : PATCH_NUM_FM_CHANNELS;
	const int preset = values->channels[channel][PARAM_PRESET];
	// Opening the page or choosing another channel brings its place into view; a program change doesn't.
	if (usable && (!g->bank_was_open || g->bank_seen_channel != channel) && (preset < g->bank_top || preset >= g->bank_top + BANK_ROWS)) {
		scroll_bank(g, preset - 3);
	}
	g->bank_was_open = true;
	g->bank_seen_channel = channel;

	draw_page_top(g, values);
	const patch_bank *bank = patch_bank_view(g->source.patch);
	text(g, 250, BANK_HEADER_Y, "PRG", C_HINT, 1);
	if (usable) {
		text(g, 298, BANK_HEADER_Y, envelopes ? "PSG ENVELOPE" : "FM VOICE", light, 1);
	} else {
		text(g, 298, BANK_HEADER_Y, "CH6 PLAYS SAMPLES. SELECT ANOTHER CH", C_LABEL, 1);
	}
	if (envelopes) {
		text(g, 544, BANK_HEADER_Y, "STEP", C_HINT, 1);
	}
	// While files of the list's kind are dragged the FILE and dots column becomes one ADD field.
	const bool sounds_over = g->file_over && (envelopes ? g->file_envelopes : g->file_instruments) > 0;
	const bool full = files_add_place(g->source.files, envelopes) >= PATCH_BANK_SIZE;
	char buffer[64];
	if (!sounds_over) {
		text(g, 580, BANK_HEADER_Y, "FILE", C_HINT, 1);
		for (int c = first_channel; c < last_channel; c++) {
			snprintf(buffer, sizeof(buffer), "%d", c + 1);
			text(g, BANK_DOTS_X + (c - first_channel) * 12, BANK_HEADER_Y, buffer, c == channel ? C_TEXT : is_dac(values, c) ? C_LINE : C_HINT, 1);
		}
	}
	const bool own = usable && plays_own_file(g, values, channel);
	const int hover = g->file_over ? -1 : bank_place_at(g, g->mouse_x, g->mouse_y);
	const bool aimed = g->file_over && (g->drop_place >= 0 || g->drop_add);
	for (int row = 0; row < BANK_ROWS; row++) {
		const int place = g->bank_top + row, y = BANK_Y + row * BANK_ROW_H;
		const bool selected = usable && preset == place;
		const bool target = aimed && place >= g->drop_first && place < g->drop_first + g->drop_lit;
		const bool flash = g->flash_ticks > 0 && place >= g->flash_place && place < g->flash_place + g->flash_count;
		const bool from_file = envelopes ? !patch_envelope_is_rom(bank, place) : !patch_place_is_gm(bank, place);
		if (target) {
			fill(g, BANK_X, y, BANK_ADD_X - BANK_X, BANK_ROW_H - 1, C_AMBER);
		} else if (flash && g->flash_color == C_LINE) {
			fill(g, BANK_X, y, BANK_RIGHT - BANK_X, BANK_ROW_H - 1, C_LINE);
		} else if (selected) {
			if (!own) {
				fill(g, BANK_X + 4, y, BANK_RIGHT - BANK_X - 4, BANK_ROW_H - 1, dark);
			}
			fill(g, BANK_X, y, 4, BANK_ROW_H - 1, kind);
		}
		fill(g, BANK_X, y + BANK_ROW_H - 1, BANK_RIGHT - BANK_X, 1, place % 8 == 7 ? C_LINE : C_CELL);
		snprintf(buffer, sizeof(buffer), "%d", place + 1);
		text_right(g, 286, y + 3, buffer, target ? C_BG : !usable ? C_LINE : selected ? C_TEXT : C_LABEL, 2, 40);
		snprintf(buffer, sizeof(buffer), "%.*s", envelopes ? ENVELOPE_NAME_CHARS : NAME_CHARS, place_name(bank, envelopes, place));
		text(g, 298, y + 3, buffer, target ? C_BG : !usable ? C_HINT : from_file ? C_TEXT : C_LABEL, 2);
		if (envelopes && !target) {
			// The steps the limit counts, and whether a held note keeps it going.
			const patch_envelope *envelope = &bank->envelopes[place];
			const int length = patch_envelope_length(envelope);
			snprintf(buffer, sizeof(buffer), "%d", length);
			text_right(g, 564, y + 7, buffer, selected ? C_TEXT : C_LABEL, 1, 24);
			if (memchr(envelope->steps, PATCH_EEF_LOOP_START, (size_t)length)) {
				text(g, 568, y + 7, "L", C_PSG, 1);
			}
		}
		if (from_file) {
			text(g, 580, y + 7, "FILE", light, 1);
		}
		for (int c = first_channel; c < last_channel; c++) {
			if (!takes_files(values, c) || values->channels[c][PARAM_PRESET] != place) {
				continue;
			}
			const uint32_t color = c == channel ? C_TEXT : light;
			const int x = BANK_DOTS_X + (c - first_channel) * 12;
			if (plays_own_file(g, values, c)) {
				frame(g, x, y + 7, 6, 6, color);
			} else {
				fill(g, x, y + 7, 6, 6, color);
			}
		}
		if (flash && g->flash_color == C_ERROR) {
			frame(g, BANK_X, y, BANK_RIGHT - BANK_X, BANK_ROW_H - 1, C_ERROR);
		} else if (hover == place) {
			frame(g, BANK_X, y, BANK_RIGHT - BANK_X, BANK_ROW_H - 1, usable ? C_TEXT : C_LINE);
		}
	}
	fill(g, SCROLL_X, BANK_Y, 6, SCROLL_H, C_CELL);
	const bool over_scroll = g->mouse_x >= SCROLL_X && g->mouse_x < SCROLL_X + 6 && g->mouse_y >= BANK_Y && g->mouse_y < BANK_Y + SCROLL_H;
	fill(g, SCROLL_X, BANK_Y + g->bank_top * SCROLL_H / PATCH_BANK_SIZE, 6, SCROLL_H * BANK_ROWS / PATCH_BANK_SIZE, over_scroll ? C_LABEL : C_LINE);
	// The lit places show on the track too, where the list doesn't reach.
	if (aimed && g->drop_lit > 0) {
		const int h = g->drop_lit * SCROLL_H / PATCH_BANK_SIZE;
		fill(g, SCROLL_X, BANK_Y + g->drop_first * SCROLL_H / PATCH_BANK_SIZE, 6, h < 3 ? 3 : h, C_AMBER);
	}
	// Every channel of the list marks its place on the track, the selected one wide, so places out of
	// view are found without scrolling.
	for (int c = first_channel; c < last_channel; c++) {
		if (c != channel && takes_files(values, c)) {
			fill(g, SCROLL_X, BANK_Y + values->channels[c][PARAM_PRESET] * SCROLL_H / PATCH_BANK_SIZE, 3, 2, light);
		}
	}
	if (usable) {
		fill(g, SCROLL_X, BANK_Y + preset * SCROLL_H / PATCH_BANK_SIZE, 6, 3, kind);
	}
	if (sounds_over) {
		// The field covers what the column shows, lit when the files would go there.
		const uint32_t edge = full ? C_LINE : C_AMBER;
		fill(g, 580, 72, 118, 380, g->drop_add ? C_AMBER : C_BG);
		text(g, 621, 255, "ADD", g->drop_add ? C_BG : edge, 2);
		frame(g, 240, 70, 336, 384, C_AMBER);
		frame(g, 241, 71, 334, 382, C_AMBER);
		frame(g, 578, 70, 122, 384, edge);
		frame(g, 579, 71, 120, 382, edge);
	}
}

// The algorithm in small for a row of ALL: operators 10 px with their number, carriers filled, the
// output bus below, the feedback loop over operator 1. Its own drawing: the FM page gets its own in
// stage 11.
#define MINI_OP 10
#define MINI_HALF (MINI_OP / 2)
#define MINI_BUS 37
#define MINI_RIGHT 64
static void mini_op_xy(int x0, int y0, int algorithm, int op, int *x, int *y)
{
	*x = x0 + 5 + ALGORITHM_POS[algorithm][op][0] * 15;
	*y = y0 + 3 + ALGORITHM_POS[algorithm][op][1] * 11;
}

static void draw_mini_diagram(mdmi_gui *g, int x0, int y0, int algorithm, int feedback, uint32_t carrier)
{
	const int bus = y0 + MINI_BUS;
	int left = x0 + MINI_RIGHT;
	for (int link = 0; link < 3; link++) {
		const int from = ALGORITHM_LINKS[algorithm][link][0], to = ALGORITHM_LINKS[algorithm][link][1];
		if (from < 0) {
			continue;
		}
		int fx, fy, tx, ty;
		mini_op_xy(x0, y0, algorithm, from, &fx, &fy);
		mini_op_xy(x0, y0, algorithm, to, &tx, &ty);
		if (fy == ty) {
			fill(g, fx + MINI_OP, fy + MINI_HALF, tx - fx - MINI_OP, 1, C_LABEL);
		} else {
			fill(g, fx + MINI_OP, fy + MINI_HALF, tx + MINI_HALF - fx - MINI_OP + 1, 1, C_LABEL);
			if (ty > fy) {
				fill(g, tx + MINI_HALF, fy + MINI_HALF, 1, ty - fy - MINI_HALF, C_LABEL);
			} else {
				fill(g, tx + MINI_HALF, ty + MINI_OP, 1, fy + MINI_HALF - ty - MINI_OP + 1, C_LABEL);
			}
		}
	}
	for (int op = 0; op < PATCH_OPS; op++) {
		int x, y;
		mini_op_xy(x0, y0, algorithm, op, &x, &y);
		if (CARRIERS[algorithm] >> op & 1) {
			fill(g, x + MINI_HALF, y + MINI_OP, 1, bus - y - MINI_OP, C_LABEL);
			left = x + MINI_HALF < left ? x + MINI_HALF : left;
		}
	}
	fill(g, left, bus, x0 + MINI_RIGHT - left, 1, C_LABEL);
	int x, y;
	mini_op_xy(x0, y0, algorithm, 0, &x, &y);
	const uint32_t loop = feedback ? C_LABEL : C_LINE;
	fill(g, x - 3, y - 3, 1, 8, loop);
	fill(g, x - 3, y - 3, MINI_OP + 6, 1, loop);
	fill(g, x + MINI_OP + 2, y - 3, 1, 8, loop);
	fill(g, x - 3, y + MINI_HALF, 3, 1, loop);
	for (int op = 0; op < PATCH_OPS; op++) {
		mini_op_xy(x0, y0, algorithm, op, &x, &y);
		const char digit[2] = {(char)('1' + op), 0};
		if (CARRIERS[algorithm] >> op & 1) {
			fill(g, x, y, MINI_OP, MINI_OP, carrier);
			text(g, x + 3, y + 2, digit, C_TEXT, 1);
		} else {
			fill(g, x, y, MINI_OP, MINI_OP, C_BG);
			frame(g, x, y, MINI_OP, MINI_OP, C_FM_LIGHT);
			text(g, x + 3, y + 2, digit, C_FM_LIGHT, 1);
		}
	}
	char buffer[16];
	snprintf(buffer, sizeof(buffer), "ALG %d", algorithm);
	text(g, x0 + 72, y0 + 8, buffer, C_LABEL, 1);
	snprintf(buffer, sizeof(buffer), "FB %d", feedback);
	text(g, x0 + 72, y0 + 22, buffer, C_LABEL, 1);
}

// Where frame i of n starts in a lane w px wide.
static int mini_edge(int x, int w, int n, int i)
{
	return x + i * w / n;
}

// The envelope in small, frame by frame: dark body, bright top, the loop on a grey ground.
static void draw_mini_envelope(mdmi_gui *g, int x, int y, int w, int h, const patch_shape *shape)
{
	const int n = shape->count > 0 ? shape->count : 1;
	if (shape->loop_start >= 0) {
		const int from = mini_edge(x, w, n, shape->loop_start);
		fill(g, from, y, mini_edge(x, w, n, loop_end_of(shape)) - from, h, C_CELL);
	}
	for (int i = 0; i < shape->count; i++) {
		const int level = shape->frames[i] & 15;
		if (level >= 15) {
			continue;
		}
		const int top = y + ((h - 3) * level + 7) / 15;
		int width = mini_edge(x, w, n, i + 1) - mini_edge(x, w, n, i) - (w / n >= 3 ? 1 : 0);
		width = width < 1 ? 1 : width;
		fill(g, mini_edge(x, w, n, i), top, width, y + h - top, C_PSG_DARK);
		fill(g, mini_edge(x, w, n, i), top, width, 2, C_PSG);
	}
	fill(g, x, y + h, w, 1, C_LINE);
}

// The frame a PSG channel's envelope plays now, reckoned from when its note started and stopped
// (D29), or -1. The ROM steps once a frame at 59.92 Hz.
#define PSG_FRAME_HZ 59.92
static int envelope_frame(mdmi_gui *g, const patch_shape *shape, int psg, unsigned rate)
{
	const gui_psg_times *times = g->source.psg_times;
	if (!times || rate == 0) {
		return -1;
	}
	const uint64_t on = __atomic_load_n(&times->on[psg], __ATOMIC_ACQUIRE);
	const uint64_t off = __atomic_load_n(&times->off[psg], __ATOMIC_ACQUIRE);
	const uint64_t now = __atomic_load_n(&times->clock, __ATOMIC_ACQUIRE);
	if (on == GUI_NEVER || on > now) {
		return -1;
	}
	const long total = (long)((double)(now - on) * PSG_FRAME_HZ / rate);
	if (off == GUI_NEVER || off < on) {
		return patch_shape_position(shape, total, -1);
	}
	const long held = (long)((double)((off < now ? off : now) - on) * PSG_FRAME_HZ / rate);
	return patch_shape_position(shape, held, total - held);
}

// TUNE, BEND and PORTA of a row, only those not at their default (1B).
static void draw_all_more(mdmi_gui *g, const patch_values *values, int channel, int y)
{
	const int16_t *v = values->channels[channel];
	char buffer[64];
	int x = ALL_VOL_X;
	if (v[PARAM_FINE_TUNE] != patch_param(PATCH_CHANNEL_ID(channel, PARAM_FINE_TUNE))->default_value) {
		snprintf(buffer, sizeof(buffer), "TUNE %+d", v[PARAM_FINE_TUNE] - 64);
		x = text(g, x, y, buffer, C_LABEL, 1) + 12;
	}
	if (v[PARAM_BEND_RANGE] != patch_param(PATCH_CHANNEL_ID(channel, PARAM_BEND_RANGE))->default_value) {
		snprintf(buffer, sizeof(buffer), "BEND %d", v[PARAM_BEND_RANGE]);
		x = text(g, x, y, buffer, C_LABEL, 1) + 12;
	}
	if (v[PARAM_PORTA_ON]) {
		snprintf(buffer, sizeof(buffer), "PORTA %d", v[PARAM_PORTA_TIME]);
		text(g, x, y, buffer, C_LABEL, 1);
	}
}

// The overview (stage 10): every channel's sound in small, VOL, PAN and source, on the rail's rows.
static void draw_all(mdmi_gui *g, const patch_values *values)
{
	const unsigned rate = mdmi_engine_get_stats(g->source.engine).sample_rate;
	fill(g, RAIL_W + 1, HEADER_H, GUI_WIDTH - RAIL_W - 1, GLOBALS_Y - HEADER_H, C_BG);
	fill(g, RAIL_W + 1, HEADER_H, GUI_WIDTH - RAIL_W - 1, GROUP_H, C_CELL);
	fill(g, RAIL_W + 1, RAIL_PSG_Y - GROUP_H, GUI_WIDTH - RAIL_W - 1, GROUP_H, C_CELL);
	text(g, ALL_VOL_X, HEADER_H + 4, "VOL", C_HINT, 1);
	text(g, ALL_PAN_X, HEADER_H + 4, "PAN", C_HINT, 1);
	text_right(g, PAGE_RIGHT - 2, HEADER_H + 4, "SOURCE", C_HINT, 1, 60);
	text(g, ALL_VOL_X, RAIL_PSG_Y - GROUP_H + 4, "VOL", C_HINT, 1);
	text_right(g, PAGE_RIGHT - 2, RAIL_PSG_Y - GROUP_H + 4, "SOURCE", C_HINT, 1, 60);
	for (int channel = 0; channel < PATCH_NUM_CHANNELS; channel++) {
		const int y = row_y(channel);
		fill(g, RAIL_W + 1, y + ROW_H - 1, GUI_WIDTH - RAIL_W - 1, 1, C_CELL);
		if (is_dac(values, channel)) {
			text(g, ALL_VOL_X, y + 14, "PLAYS SAMPLES (DAC)", C_DAC, 1);
			continue;
		}
		if (channel_is_fm(channel)) {
			const int16_t *v = values->channels[channel];
			draw_mini_diagram(g, ALL_SOUND_X, y, v[PARAM_ALGORITHM], v[PARAM_FEEDBACK], is_special(values, channel) ? C_FM_LIGHT : C_FM);
		} else {
			patch_shape shape;
			patch_shape_of(&values->psg[channel - PATCH_NUM_FM_CHANNELS], &shape);
			draw_mini_envelope(g, ALL_SOUND_X, y + 3, ALL_SOUND_W, 33, &shape);
			// Where the note is in it, a green line over the frame.
			const int frame = envelope_frame(g, &shape, channel - PATCH_NUM_FM_CHANNELS, rate);
			if (frame >= 0) {
				const int n = shape.count > 0 ? shape.count : 1;
				const int left = mini_edge(ALL_SOUND_X, ALL_SOUND_W, n, frame), right = mini_edge(ALL_SOUND_X, ALL_SOUND_W, n, frame + 1);
				fill(g, left + (right - left) / 2, y + 2, 1, 35, C_PLAYING);
			}
		}
		draw_all_more(g, values, channel, y + 28);
	}
}

static void draw_editor(mdmi_gui *g)
{
	const patch_values *values = patch_view(g->source.patch);
	layout(g, values);
	fill(g, 0, HEADER_H, RAIL_W, GROUP_H, C_CELL);
	text(g, 8, HEADER_H + 4, "YM2612", C_FM_LIGHT, 1);
	fill(g, 0, RAIL_PSG_Y - GROUP_H, RAIL_W, GROUP_H, C_CELL);
	text(g, 8, RAIL_PSG_Y - GROUP_H + 4, "SN76489", C_PSG, 1);
	fill(g, RAIL_W, HEADER_H, 1, GLOBALS_Y - HEADER_H, C_LINE);
	if (*g->source.page == PAGE_BANK) {
		draw_bank(g, values);
	} else if (*g->source.page == PAGE_ALL) {
		g->bank_was_open = false;
		draw_all(g, values);
	} else {
		g->bank_was_open = false;
		draw_page(g, values);
	}
	for (int channel = 0; channel < PATCH_NUM_CHANNELS; channel++) {
		draw_row(g, values, channel);
	}
	draw_lends(g);
	fill(g, 0, GLOBALS_Y, GUI_WIDTH, FOOTER_Y - GLOBALS_Y, C_BG);
	fill(g, 0, GLOBALS_Y, GUI_WIDTH, 1, C_LINE);
	for (int i = 0; i < g->cell_count; i++) {
		draw_cell(g, &g->cells[i], values);
	}
	// Hover last, so nothing covers it.
	int over = g->dragging || g->file_over ? -1 : hovered(g);
	if (over >= 0) {
		const cell *c = &g->cells[over];
		frame(g, c->x, c->y, c->w, c->h, C_TEXT);
	}
	int row = g->dragging || g->file_over ? -1 : row_at(g, g->mouse_x, g->mouse_y);
	if (row >= 0) {
		frame(g, 0, row_y(row), *g->source.page == PAGE_ALL ? GUI_WIDTH : RAIL_W, ROW_H, C_TEXT);
	}
}

static void draw_rom(mdmi_gui *g)
{
	const mdmi_frame *rom = mdmi_engine_latest_frame(g->source.engine);
	fill(g, 0, HEADER_H, GUI_WIDTH, GUI_HEIGHT - HEADER_H, C_BG);
	int width = (int)rom->width * ROM_SCALE, height = (int)rom->height * ROM_SCALE;
	if (width > GUI_WIDTH) {
		width = GUI_WIDTH;
	}
	if (height > GUI_HEIGHT - HEADER_H) {
		height = GUI_HEIGHT - HEADER_H;
	}
	const int left = (GUI_WIDTH - width) / 2;
	const int top = HEADER_H + (GUI_HEIGHT - HEADER_H - height) / 2;
	for (int y = 0; y < height; y++) {
		const uint32_t *source = rom->pixels + (y / ROM_SCALE) * MDMI_FRAME_MAX_WIDTH;
		uint32_t *line = g->pixels + (top + y) * GUI_WIDTH + left;
		for (int x = 0; x < width; x++) {
			line[x] = source[x / ROM_SCALE] & 0xFFFFFF;
		}
	}
}

static void draw_status(mdmi_gui *g)
{
	mdmi_engine_stats stats = mdmi_engine_get_stats(g->source.engine);
	// What failed is told in the footer.
	if (stats.failed) {
		text(g, STATUS_X, 7, "FAILED", C_ERROR, 2);
	} else if (!stats.active) {
		text(g, STATUS_X, 7, "STOPPED", C_HINT, 2);
	} else if (!stats.booted) {
		text(g, STATUS_X, 7, "LOADING", C_AMBER, 2);
	} else {
		text(g, STATUS_X, 7, "RUNNING", C_PLAYING, 2);
	}
}

// Buttons that do something, drawn unlike the page switches.
static void action_button(mdmi_gui *g, int action, int x, const char *label, bool enabled)
{
	const bool over = enabled && !g->file_over && action_at(g, g->mouse_x, g->mouse_y) == action;
	fill(g, x, BUTTON_Y, ACTION_W, BUTTON_H, C_BG);
	frame(g, x, BUTTON_Y, ACTION_W, BUTTON_H, !enabled ? C_CELL : over ? C_TEXT : C_LINE);
	const int width = (int)strlen(label) * 12;
	text(g, x + (ACTION_W - width) / 2, BUTTON_Y + 5, label, !enabled ? C_LINE : over ? C_TEXT : C_LABEL, 2);
}

static void render(mdmi_gui *g)
{
	fill(g, 0, 0, GUI_WIDTH, HEADER_H, C_BG);
	fill(g, 0, HEADER_H - 1, GUI_WIDTH, 1, C_LINE);
	text(g, 8, 7, "MDMI", C_TEXT, 2);
	const int page = *g->source.page;
	const bool rom = page == PAGE_ROM;
	button(g, BUTTON_ALL_X, BUTTON_SMALL_W, "ALL", page == PAGE_ALL);
	button(g, BUTTON_EDITOR_X, BUTTON_W, "EDITOR", page != PAGE_ROM && page != PAGE_BANK && page != PAGE_ALL);
	button(g, BUTTON_BANK_X, BUTTON_W, "BANK", page == PAGE_BANK);
	button(g, BUTTON_ROM_X, BUTTON_SMALL_W, "ROM", rom);
	draw_status(g);
	action_button(g, ACTION_LOAD, ACTION_LOAD_X, "LOAD", true);
	action_button(g, ACTION_SAVE, ACTION_SAVE_X, "SAVE", true);
	action_button(g, ACTION_UNDO, ACTION_UNDO_X, "UNDO", files_undo_text(g->source.files) != NULL);
	if (page == PAGE_BANK) {
		action_button(g, ACTION_ADD, ACTION_ADD_X, "ADD", files_add_place(g->source.files, !channel_is_fm(*g->source.channel)) < PATCH_BANK_SIZE);
	}
	if (rom) {
		g->cell_count = 0;
		draw_rom(g);
	} else {
		draw_editor(g);
	}
	draw_footer(g);
}

static void paint(mdmi_gui *g, HDC dc)
{
	BITMAPINFO info = {0};
	info.bmiHeader.biSize = sizeof(info.bmiHeader);
	info.bmiHeader.biWidth = GUI_WIDTH;
	info.bmiHeader.biHeight = -GUI_HEIGHT;
	info.bmiHeader.biPlanes = 1;
	info.bmiHeader.biBitCount = 32;
	info.bmiHeader.biCompression = BI_RGB;
	SetStretchBltMode(dc, COLORONCOLOR);
	StretchDIBits(dc, 0, 0, GUI_WIDTH * g->scale, GUI_HEIGHT * g->scale, 0, 0, GUI_WIDTH, GUI_HEIGHT, g->pixels, &info, DIB_RGB_COLORS, SRCCOPY);
}

static void refresh(mdmi_gui *g)
{
	render(g);
	if (g->window) {
		InvalidateRect(g->window, NULL, FALSE);
	}
}

// A knob the mouse moves is the window's: its host mark goes at once.
static void edit(mdmi_gui *g, int kind, uint32_t id, int value)
{
	int channel, param;
	if (kind != GUI_EDIT_END && patch_id_split(id, &channel, &param)) {
		g->host_until[GUI_HOST_SLOT(channel, param)] = 0;
	}
	g->source.edit(g->source.context, kind, id, value);
}

// Once a timer tick: a knob whose count moved is marked for two seconds from its last value.
#define HOST_MARK_MS 2000
static void host_tick(mdmi_gui *g)
{
	if (!g->source.host_values) {
		return;
	}
	const ULONGLONG now = GetTickCount64();
	for (int i = 0; i < GUI_HOST_SLOTS; i++) {
		const uint32_t count = __atomic_load_n(&g->source.host_values[i], __ATOMIC_RELAXED);
		if (count != g->host_seen[i]) {
			g->host_seen[i] = count;
			g->host_until[i] = now + HOST_MARK_MS;
		}
	}
}

// One whole change, as a click on a button.
static void edit_once(mdmi_gui *g, uint32_t id, int value)
{
	edit(g, GUI_EDIT_BEGIN, id, 0);
	edit(g, GUI_EDIT_SET, id, value);
	edit(g, GUI_EDIT_END, id, 0);
}

// How far the mouse moves for one step, so short ranges don't jump and long ones don't crawl.
static int drag_value(int start, int max, int dy)
{
	int value;
	if (max > 127) {
		value = start + dy * 4;
	} else {
		int pixels = max + 1 <= 8 ? 12 : max + 1 <= 32 ? 6 : 2;
		value = start + dy / pixels;
	}
	return value < 0 ? 0 : value > max ? max : value;
}

static void flash(mdmi_gui *g, int channel, int place, int count, bool done)
{
	g->flash_channel = channel;
	g->flash_place = place;
	g->flash_count = count;
	g->flash_color = done ? C_LINE : C_ERROR;
	g->flash_ticks = FLASH_TICKS;
}

// Opens a file onto a channel, or a bank or a snapshot, and says how it went.
static void open_file(mdmi_gui *g, const char *path, int channel)
{
	char message[FILES_MESSAGE];
	const patch_values *values = patch_view(g->source.patch);
	bool done;
	if (files_kind(path) == FILES_INSTRUMENT && channel_is_fm(channel) && is_dac(values, channel)) {
		snprintf(message, sizeof(message), "CH6 plays samples: DAC is on");
		done = false;
	} else {
		done = files_open(g->source.files, path, channel, message, sizeof(message));
	}
	say(g, message, done ? C_TEXT : C_ERROR, done ? MESSAGE_TICKS : ERROR_TICKS);
	if (files_kind(path) == FILES_INSTRUMENT || files_kind(path) == FILES_ENVELOPE) {
		flash(g, channel, -1, 0, done);
	}
}

// Instruments or envelopes into the bank from a place, or added after the last file when first < 0 (D26).
static void put_places(mdmi_gui *g, const char *const *paths, int count, bool envelopes, int first)
{
	char message[FILES_MESSAGE];
	if (first < 0) {
		first = files_add_place(g->source.files, envelopes);
	}
	int skipped;
	const int loaded = files_put_places(g->source.files, paths, count, envelopes, first, &skipped, message, sizeof(message));
	say(g, message, !loaded ? C_ERROR : skipped ? C_AMBER : C_TEXT, loaded && !skipped ? MESSAGE_TICKS : ERROR_TICKS);
	// The places filled, or the one the files were aimed at.
	flash(g, -1, first, loaded ? loaded : 1, loaded > 0);
}

static void to_utf8(const wchar_t *wide, char *out, size_t size)
{
	if (!WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, (int)size, NULL, NULL)) {
		out[0] = 0;
	}
}

#define DIALOG_CHARS 32768

// LOAD on the editor takes one file for the channel. On the bank page, and for ADD, several
// instruments go to places; a bank or a snapshot alone still loads whole.
static void load_dialog(mdmi_gui *g, bool add)
{
	const bool bank = add || *g->source.page == PAGE_BANK;
	const patch_values *values = patch_view(g->source.patch);
	const int channel = *g->source.channel;
	const bool envelopes = !channel_is_fm(channel);
	wchar_t *buffer = calloc(DIALOG_CHARS, sizeof(wchar_t));
	if (!buffer) {
		return;
	}
	OPENFILENAMEW dialog = {0};
	dialog.lStructSize = sizeof(dialog);
	dialog.hwndOwner = g->window;
	dialog.lpstrFilter = add && envelopes ? L"Envelopes (*.eef)\0*.eef\0All files\0*.*\0"
		: add ? L"Instruments (*.tfi;*.vgi;*.tyi)\0*.tfi;*.vgi;*.tyi\0All files\0*.*\0"
		: L"MDMI files (*.tfi;*.vgi;*.tyi;*.eef;*.mdbank;*.mdsnap)\0*.tfi;*.vgi;*.tyi;*.eef;*.mdbank;*.mdsnap\0All files\0*.*\0";
	dialog.lpstrFile = buffer;
	dialog.nMaxFile = DIALOG_CHARS;
	dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | (bank ? OFN_ALLOWMULTISELECT | OFN_EXPLORER : 0);
	if (!GetOpenFileNameW(&dialog)) {
		free(buffer);
		return;
	}
	// Several files come as the folder, then each name; one file as its whole path.
	const wchar_t *folder = buffer, *name = buffer + wcslen(buffer) + 1;
	int count = 0;
	if (!*name) {
		to_utf8(buffer, g->file_paths[count++], sizeof(g->file_paths[0]));
	}
	for (; *name && count < PATCH_BANK_SIZE; name += wcslen(name) + 1) {
		wchar_t path[MAX_PATH * 2];
		_snwprintf(path, MAX_PATH * 2, L"%s\\%s", folder, name);
		path[MAX_PATH * 2 - 1] = 0;
		to_utf8(path, g->file_paths[count++], sizeof(g->file_paths[0]));
	}
	free(buffer);

	const char *paths[PATCH_BANK_SIZE];
	for (int i = 0; i < count; i++) {
		paths[i] = g->file_paths[i];
	}
	if (!bank || (count == 1 && !add && files_kind(paths[0]) != (envelopes ? FILES_ENVELOPE : FILES_INSTRUMENT))) {
		open_file(g, paths[0], channel);
	} else if (add) {
		put_places(g, paths, count, envelopes, -1);
	} else if (!takes_files(values, channel)) {
		say(g, "Select another CH to choose a slot", C_ERROR, ERROR_TICKS);
	} else {
		put_places(g, paths, count, envelopes, values->channels[channel][PARAM_PRESET]);
	}
}

static void save_dialog(mdmi_gui *g)
{
	const int channel = *g->source.channel;
	const bool sound = takes_files(patch_view(g->source.patch), channel);
	const bool envelope = !channel_is_fm(channel);
	wchar_t path[MAX_PATH] = L"";
	if (sound) {
		// The instrument's or envelope's name, without what a file name can't hold.
		MultiByteToWideChar(CP_UTF8, 0, sound_name(patch_view(g->source.patch), channel), -1, path, MAX_PATH);
		for (wchar_t *c = path; *c; c++) {
			if (wcschr(L"\\/:*?\"<>|", *c)) {
				*c = L'_';
			}
		}
	}
	OPENFILENAMEW dialog = {0};
	dialog.lStructSize = sizeof(dialog);
	dialog.hwndOwner = g->window;
	dialog.lpstrFilter = envelope ? L"Envelope EEF (*.eef)\0*.eef\0Bank (*.mdbank)\0*.mdbank\0Snapshot (*.mdsnap)\0*.mdsnap\0"
		: L"Instrument VGI (*.vgi)\0*.vgi\0Bank (*.mdbank)\0*.mdbank\0Snapshot (*.mdsnap)\0*.mdsnap\0";
	dialog.nFilterIndex = sound ? 1 : 2;
	dialog.lpstrFile = path;
	dialog.nMaxFile = MAX_PATH;
	dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
	if (!GetSaveFileNameW(&dialog)) {
		return;
	}
	// The name's extension says what the file is when it is ours, since loading goes by it; without
	// one, the chosen type's extension is added.
	const int KINDS[3] = {envelope ? FILES_ENVELOPE : FILES_INSTRUMENT, FILES_BANK, FILES_SNAPSHOT};
	const char *const EXTENSIONS[3] = {envelope ? ".eef" : ".vgi", ".mdbank", ".mdsnap"};
	const int chosen = dialog.nFilterIndex >= 1 && dialog.nFilterIndex <= 3 ? (int)dialog.nFilterIndex - 1 : 1;
	char utf8[MAX_PATH * 3 + 8], message[FILES_MESSAGE];
	to_utf8(path, utf8, MAX_PATH * 3);
	const char *dot = strrchr(utf8, '.');
	const bool ours = dot && (!_stricmp(dot, EXTENSIONS[0]) || files_kind(utf8) == FILES_BANK || files_kind(utf8) == FILES_SNAPSHOT);
	if (!ours) {
		strcat(utf8, EXTENSIONS[chosen]);
	}
	const int kind = ours ? files_kind(utf8) : KINDS[chosen];
	bool done = files_save(g->source.files, utf8, kind, sound ? channel : -1, message, sizeof(message));
	say(g, message, done ? C_TEXT : C_ERROR, done ? MESSAGE_TICKS : ERROR_TICKS);
}

static void on_action(mdmi_gui *g, int action)
{
	char message[FILES_MESSAGE];
	if (action == ACTION_LOAD) {
		load_dialog(g, false);
	} else if (action == ACTION_ADD) {
		if (files_add_place(g->source.files, !channel_is_fm(*g->source.channel)) < PATCH_BANK_SIZE) {
			load_dialog(g, true);
		}
	} else if (action == ACTION_SAVE) {
		save_dialog(g);
	} else if (files_undo(g->source.files, message, sizeof(message))) {
		say(g, message, C_TEXT, MESSAGE_TICKS);
	}
	refresh(g);
}

static void on_down(mdmi_gui *g, int x, int y)
{
	if (action_at(g, x, y) >= 0) {
		on_action(g, action_at(g, x, y));
		return;
	}
	if (y >= BUTTON_Y && y < BUTTON_Y + BUTTON_H) {
		if (x >= BUTTON_ALL_X && x < BUTTON_ALL_X + BUTTON_SMALL_W) {
			*g->source.page = PAGE_ALL;
		} else if (x >= BUTTON_EDITOR_X && x < BUTTON_EDITOR_X + BUTTON_W) {
			*g->source.page = PAGE_EDITOR;
		} else if (x >= BUTTON_BANK_X && x < BUTTON_BANK_X + BUTTON_W) {
			*g->source.page = PAGE_BANK;
		} else if (x >= BUTTON_ROM_X && x < BUTTON_ROM_X + BUTTON_SMALL_W) {
			*g->source.page = PAGE_ROM;
		}
		refresh(g);
		return;
	}
	if (*g->source.page == PAGE_ROM) {
		return;
	}
	int channel = rail_channel_at(x, y);
	if (channel >= 0) {
		*g->source.channel = channel;
		refresh(g);
		return;
	}
	if (*g->source.page == PAGE_BANK) {
		const int place = bank_place_at(g, x, y);
		const int selected = *g->source.channel;
		if (place >= 0 && takes_files(patch_view(g->source.patch), selected)) {
			edit_once(g, PATCH_CHANNEL_ID(selected, PARAM_PRESET), place);
		} else if (x >= SCROLL_X && x < SCROLL_X + 6 && y >= BANK_Y && y < BANK_Y + SCROLL_H) {
			const int thumb = BANK_Y + g->bank_top * SCROLL_H / PATCH_BANK_SIZE;
			scroll_bank(g, g->bank_top + (y < thumb ? -BANK_ROWS : BANK_ROWS));
		}
		if (place >= 0 || x >= SCROLL_X) {
			refresh(g);
			return;
		}
	}
	g->mouse_x = x;
	g->mouse_y = y;
	if (is_envelope_page(g) && envelope_down(g, x, y)) {
		refresh(g);
		return;
	}
	int index = hovered(g);
	if (index < 0) {
		// A row of ALL selects its channel; its sound opens the channel's page.
		const int row = row_at(g, x, y);
		if (row >= 0) {
			*g->source.channel = row;
			if (x >= ALL_SOUND_X && x < ALL_SOUND_X + ALL_SOUND_W) {
				*g->source.page = PAGE_EDITOR;
			}
			refresh(g);
		}
		return;
	}
	const cell *c = &g->cells[index];
	if (c->kind == CELL_CHOICE) {
		edit_once(g, c->id, c->choice);
		return;
	}
	g->dragging = true;
	g->drag_moved = false;
	g->drag_id = c->id;
	g->drag_y = y;
	g->drag_start = g->drag_value = patch_get(g->source.patch, c->id);
	edit(g, GUI_EDIT_BEGIN, c->id, 0);
	// A touch alone is a change to the host, so Configure in Live picks the knob up.
	edit(g, GUI_EDIT_SET, c->id, g->drag_value);
	if (g->window) {
		SetCapture(g->window);
	}
	refresh(g);
}

static void on_move(mdmi_gui *g, int x, int y)
{
	g->mouse_x = x;
	g->mouse_y = y;
	if (g->env_drag != ENV_NONE) {
		envelope_move(g, x, y);
		return;
	}
	if (!g->dragging) {
		return;
	}
	const int dy = g->drag_y - y;
	if (abs(dy) >= DRAG_SLOP) {
		g->drag_moved = true;
	}
	if (!g->drag_moved) {
		return;
	}
	int value = drag_value(g->drag_start, patch_param(g->drag_id)->max, dy);
	if (value != g->drag_value) {
		g->drag_value = value;
		edit(g, GUI_EDIT_SET, g->drag_id, value);
	}
}

static void on_up(mdmi_gui *g)
{
	if (g->env_drag != ENV_NONE) {
		envelope_up(g);
		return;
	}
	if (!g->dragging) {
		return;
	}
	// A click without moving steps a short range round, which flips a switch.
	const int max = patch_param(g->drag_id)->max;
	if (!g->drag_moved && max + 1 <= 8) {
		g->drag_value = g->drag_start < max ? g->drag_start + 1 : 0;
		edit(g, GUI_EDIT_SET, g->drag_id, g->drag_value);
	}
	edit(g, GUI_EDIT_END, g->drag_id, 0);
	g->dragging = false;
	if (GetCapture() == g->window) {
		ReleaseCapture();
	}
}

// Back to the value of the channel's preset, or the default for global knobs. The preset itself stays.
static void on_double_click(mdmi_gui *g, int x, int y)
{
	g->mouse_x = x;
	g->mouse_y = y;
	// A quick second press on the envelope draws as the first did.
	patch_shape scratch;
	if (is_envelope_page(g) && envelope_part_at(g, shown_shape(g, &scratch), x, y) != ENV_NONE) {
		on_down(g, x, y);
		return;
	}
	int index = *g->source.page == PAGE_EDITOR ? hovered(g) : -1;
	if (index < 0 || g->cells[index].kind == CELL_CHOICE) {
		return;
	}
	const uint32_t id = g->cells[index].id;
	int channel, param;
	patch_id_split(id, &channel, &param);
	if (channel >= 0 && param == PARAM_PRESET) {
		return;
	}
	// Where a click steps, a quick second click is one more step.
	if (patch_param(id)->max + 1 <= 8) {
		on_down(g, x, y);
		on_up(g);
		return;
	}
	int value = channel < 0 ? patch_param(id)->default_value
		: patch_preset_value(patch_bank_view(g->source.patch), channel, param, patch_get(g->source.patch, PATCH_CHANNEL_ID(channel, PARAM_PRESET)));
	edit_once(g, id, value);
}

// Files dragged from Explorer (D13). The drop target lives as long as the window, so it counts no
// references.
static HRESULT STDMETHODCALLTYPE drop_query_interface(IDropTarget *self, REFIID riid, void **out)
{
	if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDropTarget)) {
		*out = self;
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE drop_add_ref(IDropTarget *self)
{
	(void)self;
	return 1;
}

static ULONG STDMETHODCALLTYPE drop_release(IDropTarget *self)
{
	(void)self;
	return 1;
}

static mdmi_gui *gui_of(IDropTarget *self)
{
	return ((drop_target *)self)->gui;
}

// The files offered, up to a bank of them, and how many are instruments and envelopes.
static void read_drop(mdmi_gui *g, IDataObject *data)
{
	g->file_count = g->file_instruments = g->file_envelopes = 0;
	g->file_paths[0][0] = g->file_name[0] = g->file_stem[0] = 0;
	FORMATETC format = {CF_HDROP, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
	STGMEDIUM medium;
	if (FAILED(IDataObject_GetData(data, &format, &medium))) {
		return;
	}
	HDROP drop = GlobalLock(medium.hGlobal);
	if (drop) {
		const int offered = (int)DragQueryFileW(drop, 0xFFFFFFFF, NULL, 0);
		for (int i = 0; i < offered && g->file_count < PATCH_BANK_SIZE; i++) {
			wchar_t path[MAX_PATH];
			if (DragQueryFileW(drop, (UINT)i, path, MAX_PATH)) {
				char *utf8 = g->file_paths[g->file_count++];
				to_utf8(path, utf8, sizeof(g->file_paths[0]));
				g->file_instruments += files_kind(utf8) == FILES_INSTRUMENT;
				g->file_envelopes += files_kind(utf8) == FILES_ENVELOPE;
			}
		}
		GlobalUnlock(medium.hGlobal);
	}
	ReleaseStgMedium(&medium);
	// The name shown is the first instrument's or envelope's, if there is one among the files.
	const char *named = g->file_paths[0];
	for (int i = 0; i < g->file_count; i++) {
		if (files_kind(g->file_paths[i]) == FILES_INSTRUMENT || files_kind(g->file_paths[i]) == FILES_ENVELOPE) {
			named = g->file_paths[i];
			break;
		}
	}
	const char *base = named;
	for (const char *c = named; *c; c++) {
		base = *c == '\\' || *c == '/' ? c + 1 : base;
	}
	snprintf(g->file_name, sizeof(g->file_name), "%s", base);
	snprintf(g->file_stem, sizeof(g->file_stem), "%s", base);
	char *dot = strrchr(g->file_stem, '.');
	if (dot) {
		*dot = 0;
	}
	g->file_kind = files_kind(g->file_paths[0]);
}

static void hint(mdmi_gui *g, uint32_t color, const char *format, ...)
{
	va_list args;
	va_start(args, format);
	vsnprintf(g->drop_hint, sizeof(g->drop_hint), format, args);
	va_end(args);
	g->drop_hint_color = color;
}

// Where the file would go at this point, and what the footer says about it.
static bool aim_drop(mdmi_gui *g, POINTL point)
{
	POINT client = {point.x, point.y};
	ScreenToClient(g->window, &client);
	const int x = client.x / g->scale, y = client.y / g->scale;
	g->mouse_x = x;
	g->mouse_y = y;
	g->drop_channel = g->drop_place = -1;
	g->drop_add = false;
	g->drop_lit = 0;
	const patch_values *values = patch_view(g->source.patch);
	const int page = *g->source.page;
	const bool many = g->file_count > 1;
	// The files go as envelopes when only those are there, or with both kinds when the bank shows
	// envelopes; the others are skipped (D28).
	const bool envelopes_open = !channel_is_fm(*g->source.channel);
	const bool envelopes = g->file_envelopes > 0 && (g->file_instruments == 0 || envelopes_open);
	g->drop_envelopes = envelopes;
	const int sounds = envelopes ? g->file_envelopes : g->file_instruments;
	// The words count only those: one TFI with a stray file is still "the file".
	const bool several = sounds > 1;
	if (!many && g->file_kind != FILES_INSTRUMENT && g->file_kind != FILES_ENVELOPE) {
		hint(g, C_LABEL, g->file_kind < 0 ? "%s: not TFI, VGI, TYI or EEF" : "%s: use LOAD", g->file_name);
		return false;
	}
	if (sounds == 0) {
		hint(g, C_LABEL, "No TFI, VGI, TYI or EEF in these files");
		return false;
	}
	if (page == PAGE_ROM) {
		hint(g, C_LABEL, "Open EDITOR or BANK to drop");
		return false;
	}
	const int channel = row_at(g, x, y);
	const bool in_list = page == PAGE_BANK && y >= BANK_ADD_Y && y < BANK_Y + BANK_ROWS * BANK_ROW_H;
	const bool add = in_list && x >= BANK_ADD_X && x < BANK_RIGHT + 2;
	const int place = in_list && !add ? bank_place_at(g, x, y) : -1;
	if (channel >= 0 && many) {
		hint(g, C_LABEL, "One file per channel");
	} else if (channel >= 0 && envelopes && channel_is_fm(channel)) {
		hint(g, C_LABEL, "EEF envelope: drop on CH7-10");
	} else if (channel >= 0 && !envelopes && !channel_is_fm(channel)) {
		hint(g, C_LABEL, "FM file: drop on CH1-6");
	} else if (channel >= 0 && is_dac(values, channel)) {
		hint(g, C_LABEL, "CH6 plays samples: DAC is on");
	} else if (channel >= 0) {
		g->drop_channel = channel;
		hint(g, C_AMBER, "%s > CH%d, replaces %s", g->file_stem, channel + 1, sound_name(values, channel));
		return true;
	} else if (page != PAGE_BANK) {
		hint(g, C_LABEL, many ? "Several files: open BANK to drop" : envelopes ? "%s: drop on CH7-10" : "%s: drop on CH1-6", g->file_name);
	} else if (envelopes != envelopes_open && (add || place >= 0)) {
		hint(g, C_LABEL, envelopes ? "EEF: select CH7-10 for envelopes" : "FM file: select CH1-6 for voices");
	} else if (add || place >= 0) {
		const int first = add ? files_add_place(g->source.files, envelopes) : place;
		if (first >= PATCH_BANK_SIZE) {
			hint(g, C_LABEL, envelopes ? "Envelopes full to 128: drop on a slot" : "Bank full to 128: drop on a slot");
			return false;
		}
		// Adding doesn't depend on the row, so the list can move to where the files will go.
		if (add && (first < g->bank_top || first >= g->bank_top + BANK_ROWS)) {
			scroll_bank(g, first - 3);
		}
		g->drop_add = add;
		g->drop_place = place;
		g->drop_first = first;
		g->drop_lit = sounds < PATCH_BANK_SIZE - first ? sounds : PATCH_BANK_SIZE - first;
		const int last = first + g->drop_lit;
		const char *to = add ? "ADD to " : "";
		if (!several) {
			hint(g, C_AMBER, add ? "%s > ADD to slot %d" : "%s > slot %d, replaces %s", g->file_stem, first + 1, place_name(patch_bank_view(g->source.patch), envelopes, first));
		} else if (sounds > g->drop_lit) {
			hint(g, C_AMBER, "%d files > %sslots %d-%d, %d won't fit", sounds, to, first + 1, last, sounds - g->drop_lit);
		} else {
			hint(g, C_AMBER, "%d files > %sslots %d-%d", sounds, to, first + 1, last);
		}
		return true;
	} else if (several) {
		hint(g, C_AMBER, "%d files: slot replaces, ADD column adds", sounds);
	} else {
		hint(g, C_AMBER, "%s: slot replaces, ADD column adds", g->file_name);
	}
	return false;
}

static DWORD drop_effect(DWORD allowed, bool fits)
{
	// Only an effect the source allows can complete the drop, and a move would let it delete the file.
	DWORD chosen = allowed & DROPEFFECT_COPY ? DROPEFFECT_COPY : allowed & DROPEFFECT_LINK;
	return fits ? chosen : DROPEFFECT_NONE;
}

static HRESULT STDMETHODCALLTYPE drop_enter(IDropTarget *self, IDataObject *data, DWORD keys, POINTL point, DWORD *effect)
{
	(void)keys;
	mdmi_gui *g = gui_of(self);
	read_drop(g, data);
	g->file_over = true;
	*effect = drop_effect(*effect, aim_drop(g, point));
	refresh(g);
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE drop_over(IDropTarget *self, DWORD keys, POINTL point, DWORD *effect)
{
	(void)keys;
	mdmi_gui *g = gui_of(self);
	*effect = drop_effect(*effect, aim_drop(g, point));
	refresh(g);
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE drop_leave(IDropTarget *self)
{
	mdmi_gui *g = gui_of(self);
	g->file_over = false;
	refresh(g);
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE drop_drop(IDropTarget *self, IDataObject *data, DWORD keys, POINTL point, DWORD *effect)
{
	(void)keys;
	(void)data;
	mdmi_gui *g = gui_of(self);
	const bool fits = aim_drop(g, point);
	*effect = drop_effect(*effect, fits);
	g->file_over = false;
	if (fits && g->drop_channel >= 0) {
		open_file(g, g->file_paths[0], g->drop_channel);
	} else if (fits) {
		const char *paths[PATCH_BANK_SIZE];
		for (int i = 0; i < g->file_count; i++) {
			paths[i] = g->file_paths[i];
		}
		put_places(g, paths, g->file_count, g->drop_envelopes, g->drop_add ? -1 : g->drop_place);
	} else {
		// What the footer said while the file was over the window stays there after it.
		say(g, g->drop_hint, C_ERROR, ERROR_TICKS);
	}
	refresh(g);
	return S_OK;
}

static IDropTargetVtbl drop_vtable = {
	drop_query_interface, drop_add_ref, drop_release, drop_enter, drop_over, drop_leave, drop_drop,
};

// Up and down step the selected channel's place on the bank page, as a click on the next row
// would, and the list moves only as far as it must. Keys come from the host through the view (our
// clap-wrapper patch), not as window messages: the window never has the focus.
bool gui_key(mdmi_gui *g, bool up)
{
	const int channel = *g->source.channel;
	const patch_values *values = patch_view(g->source.patch);
	if (!g->window || *g->source.page != PAGE_BANK || !takes_files(values, channel)) {
		return false;
	}
	// Until the model takes the last step, the next one goes on from where that one asked to go.
	const int playing = values->channels[channel][PARAM_PRESET];
	const bool pending = g->key_channel == channel && playing == g->key_from && g->key_to != g->key_from;
	int place = (pending ? g->key_to : playing) + (up ? -1 : 1);
	place = place < 0 ? 0 : place >= PATCH_BANK_SIZE ? PATCH_BANK_SIZE - 1 : place;
	if (!pending) {
		g->key_channel = channel;
		g->key_from = playing;
	}
	g->key_to = place;
	edit_once(g, PATCH_CHANNEL_ID(channel, PARAM_PRESET), place);
	if (place < g->bank_top) {
		scroll_bank(g, place);
	} else if (place >= g->bank_top + BANK_ROWS) {
		scroll_bank(g, place - BANK_ROWS + 1);
	}
	refresh(g);
	return true;
}

static void on_wheel(mdmi_gui *g, int delta, int screen_x, int screen_y)
{
	POINT client = {screen_x, screen_y};
	ScreenToClient(g->window, &client);
	if (*g->source.page == PAGE_BANK && client.x / g->scale >= BANK_X) {
		scroll_bank(g, g->bank_top - delta / WHEEL_DELTA * WHEEL_ROWS);
		refresh(g);
	}
}

static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
	mdmi_gui *g = (mdmi_gui *)GetWindowLongPtrA(window, GWLP_USERDATA);
	if (!g) {
		return DefWindowProcA(window, message, wparam, lparam);
	}
	const int x = (short)LOWORD(lparam) / g->scale, y = (short)HIWORD(lparam) / g->scale;
	switch (message) {
	case WM_TIMER:
		g->message_ticks -= g->message_ticks > 0;
		g->flash_ticks -= g->flash_ticks > 0;
		host_tick(g);
		if (g->env_drag != ENV_NONE) {
			envelope_hand(g);
		}
		refresh(g);
		return 0;
	case WM_MOUSEWHEEL:
		on_wheel(g, GET_WHEEL_DELTA_WPARAM(wparam), (short)LOWORD(lparam), (short)HIWORD(lparam));
		return 0;
	case WM_PAINT: {
		PAINTSTRUCT ps;
		HDC dc = BeginPaint(window, &ps);
		paint(g, dc);
		EndPaint(window, &ps);
		return 0;
	}
	case WM_PRINTCLIENT:
		render(g);
		paint(g, (HDC)wparam);
		return 0;
	case WM_ERASEBKGND:
		return 1;
	case WM_DESTROY:
		// Also reached when the host destroys our parent first. The next set_parent makes a new window.
		on_up(g);
		KillTimer(window, TIMER_ID);
		if (g->drop_registered) {
			RevokeDragDrop(window);
			g->drop_registered = false;
		}
		g->file_over = false;
		SetWindowLongPtrA(window, GWLP_USERDATA, 0);
		g->window = NULL;
		InterlockedDecrement(&window_count);
		return 0;
	case WM_LBUTTONDOWN:
		on_down(g, x, y);
		return 0;
	case WM_MOUSEMOVE:
		if (!g->tracking) {
			TRACKMOUSEEVENT track = {sizeof(track), TME_LEAVE, window, 0};
			g->tracking = TrackMouseEvent(&track) != 0;
		}
		on_move(g, x, y);
		return 0;
	case WM_MOUSELEAVE:
		g->tracking = false;
		if (!g->dragging) {
			g->mouse_x = g->mouse_y = -1;
		}
		return 0;
	case WM_LBUTTONUP:
	case WM_CAPTURECHANGED:
		on_up(g);
		return 0;
	case WM_LBUTTONDBLCLK:
		on_double_click(g, x, y);
		return 0;
	default:
		return DefWindowProcA(window, message, wparam, lparam);
	}
}

static HINSTANCE this_module(void)
{
	HMODULE module = NULL;
	GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)&this_module, &module);
	return module;
}

mdmi_gui *gui_create(const gui_source *source)
{
	mdmi_gui *g = calloc(1, sizeof(*g));
	if (!g) {
		return NULL;
	}
	g->pixels = calloc(GUI_WIDTH * GUI_HEIGHT, sizeof(uint32_t));
	if (!g->pixels) {
		free(g);
		return NULL;
	}
	g->source = *source;
	g->scale = 1;
	g->mouse_x = g->mouse_y = -1;
	g->key_channel = -1;
	g->bank_seen_channel = g->drop_channel = g->drop_place = g->flash_channel = g->flash_place = -1;
	// Values the host sent before the window opened mark nothing.
	for (int i = 0; g->source.host_values && i < GUI_HOST_SLOTS; i++) {
		g->host_seen[i] = __atomic_load_n(&g->source.host_values[i], __ATOMIC_RELAXED);
	}
	mdmi_engine_watch_frames(g->source.engine, true);
	return g;
}

void gui_destroy(mdmi_gui *g)
{
	if (!g) {
		return;
	}
	mdmi_engine_watch_frames(g->source.engine, false);
	if (g->window) {
		DestroyWindow(g->window);
	}
	if (window_count == 0) {
		UnregisterClassA(WINDOW_CLASS, this_module());
	}
	if (g->ole_ready) {
		OleUninitialize();
	}
	free(g->pixels);
	free(g);
}

bool gui_set_parent(mdmi_gui *g, void *parent_hwnd)
{
	g->parent = parent_hwnd;
	if (g->window) {
		SetParent(g->window, g->parent);
		return true;
	}
	HINSTANCE module = this_module();
	if (InterlockedIncrement(&window_count) == 1) {
		WNDCLASSA window_class = {0};
		window_class.style = CS_DBLCLKS;
		window_class.lpfnWndProc = window_proc;
		window_class.hInstance = module;
		window_class.lpszClassName = WINDOW_CLASS;
		window_class.hCursor = LoadCursor(NULL, IDC_ARROW);
		RegisterClassA(&window_class);
	}
	g->window = CreateWindowExA(0, WINDOW_CLASS, "MDMI", WS_CHILD | WS_CLIPSIBLINGS, 0, 0, GUI_WIDTH * g->scale, GUI_HEIGHT * g->scale, g->parent, NULL, module, NULL);
	if (!g->window) {
		InterlockedDecrement(&window_count);
		return false;
	}
	SetWindowLongPtrA(g->window, GWLP_USERDATA, (LONG_PTR)g);
	SetTimer(g->window, TIMER_ID, TIMER_MS, NULL);
	g->drop.iface.lpVtbl = &drop_vtable;
	g->drop.gui = g;
	// OleInitialize says S_FALSE when the host has done it already; both count towards uninitializing.
	if (!g->ole_ready) {
		g->ole_ready = SUCCEEDED(OleInitialize(NULL));
	}
	HRESULT registered = RegisterDragDrop(g->window, &g->drop.iface);
	g->drop_registered = SUCCEEDED(registered);
	if (!g->drop_registered) {
		char message[FILES_MESSAGE];
		snprintf(message, sizeof(message), "Files can't be dropped here: error %08lX", (unsigned long)registered);
		say(g, message, C_ERROR, ERROR_TICKS);
	}
	render(g);
	return true;
}

bool gui_show(mdmi_gui *g, bool visible)
{
	if (!g->window) {
		return false;
	}
	ShowWindow(g->window, visible ? SW_SHOWNA : SW_HIDE);
	mdmi_engine_watch_frames(g->source.engine, visible);
	return true;
}

bool gui_set_scale(mdmi_gui *g, double scale)
{
	int whole = (int)(scale + 0.25);
	g->scale = whole < 1 ? 1 : whole > 4 ? 4 : whole;
	if (g->window) {
		SetWindowPos(g->window, NULL, 0, 0, GUI_WIDTH * g->scale, GUI_HEIGHT * g->scale, SWP_NOZORDER | SWP_NOMOVE | SWP_NOACTIVATE);
	}
	return true;
}

void gui_get_size(const mdmi_gui *g, uint32_t *width, uint32_t *height)
{
	*width = (uint32_t)(GUI_WIDTH * g->scale);
	*height = (uint32_t)(GUI_HEIGHT * g->scale);
}
