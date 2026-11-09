#define COBJMACROS
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <commdlg.h>
#include <ole2.h>
#include <shellapi.h>
#include "gui.h"
#include "dac.h"
#include "rom_map.h"
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
// The page's grid (stage 9, D29): four columns of 112 px. The channel's cells are one row, the same
// on FM, PSG and DAC (ui-variants-v8-fm); what belongs to the chip starts under them.
#define GRID_W 112
#define GRID_ROW1_Y 56
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
// The FM page (stage 11, ui-variants-v8-fm): the graph of the four envelopes at the left, the block
// of AMS, FMS, the diagram and the algorithms at the right, the operators' columns under them.
#define GRAPH_X 242
#define GRAPH_Y 82
#define GRAPH_W 282
#define GRAPH_H 124
#define BLOCK_X 530
#define BLOCK_W 172
#define DIAGRAM_Y 106
#define DIAGRAM_H 78
#define ALGORITHM_Y 190
#define ALGORITHM_W 18
#define ALGORITHM_H 16
#define ALGORITHM_STEP 22
#define OP_W GRID_W
#define OP_HEADER_Y 212
#define OP_HEADER_H 14
#define OP_ROW_H 22
#define OP_BOTTOM 456
// DT and RS on one grid of segments: a label field, then segments of 12 px on a 13 px pitch.
#define SEG_X 22
#define SEG_PITCH 13
#define DRAG_SLOP 3
#define MAX_CELLS 160
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
// The DAC page (stage 12, docs/design/dac-page.md): a list of the 128 notes like the bank's.
#define DAC_Y 96
#define DAC_ROW_H 20
#define DAC_ROWS 15
#define DAC_SCROLL_H (DAC_ROWS * DAC_ROW_H)
#define DAC_TOP_MAX (ROM_IMAGE_NOTES - DAC_ROWS)
#define DAC_FIRST_ROM_NOTE 26
#define DAC_RESET_X 636
#define DAC_RESET_W 60
#define DAC_FREE_X 592
#define DAC_FREE_W 116
#define DAC_WAVE_COLUMNS 458
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
// An operator's fields in patch.h's order.
enum { OP_TL, OP_MUL, OP_DT, OP_RS, OP_AR, OP_DR, OP_SR, OP_SL, OP_RR, OP_AM, OP_SSG };

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
// SSG is the button that opens the choice of shapes over the graph (stage 11).
enum { CELL_ROW, CELL_COLUMN, CELL_CHOICE, CELL_SOURCE, CELL_SSG };
// How a cell looks, apart from what it does: a bar filled to its value, a segment of DT, RS or FB, a
// switch button (AM, PORTA), an SSG shape.
enum { LOOK_BAR, LOOK_SEGMENT, LOOK_SWITCH, LOOK_SHAPE, LOOK_FEEDBACK };

// A value the mouse can change. Rebuilt for each frame from what the page shows.
typedef struct {
	int x, y, w, h;
	uint32_t id;
	const char *label;
	int kind;
	int choice;
	int look;
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
	// The first note the DAC page shows, and whether it has been open.
	int dac_top;
	bool dac_was_open;
	// The note the DAC's grid shows below it, the window's alone (not in the project).
	int dac_selected;
	// The DAC panel's gesture (D38): what is dragged, on which note, the edit while it lasts and
	// where it began. Nothing reaches the sample before the mouse goes up.
	int dac_drag;
	int dac_drag_note;
	int dac_drag_x;
	int dac_drag_y;
	dac_edit dac_drag_from;
	dac_edit dac_work;
	// The mouse moved past DRAG_SLOP: before that a press changes nothing, so a double click resets.
	bool dac_moved;
	// A sample dragged on the grid (owner after 0.12.0): the note it came from (-1: none), whether
	// the mouse moved far enough, where it is now, where the press was.
	int grid_from;
	bool grid_moved;
	int grid_target;
	int grid_press_x;
	int grid_press_y;
	// The selected note's ROM sample as one of the project's, for the panel to show and change: made
	// when the note or the samples change. Changing it makes it the project's (owner after 0.12.0).
	dac_sample *rom_copy;
	int rom_copy_note;
	uint32_t rom_copy_version;
	// The selected sample's wave, min and max a column, and what they were made from.
	const void *wave_of;
	int wave_note;
	int16_t wave_low[DAC_WAVE_COLUMNS];
	int16_t wave_high[DAC_WAVE_COLUMNS];
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
	// WAV among the files, and the notes they would go to on the DAC page.
	int file_samples;
	int dac_drop_note;
	int dac_drop_count;
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

	// The FM page (stage 11): the operator last edited by the mouse, the operator whose choice of SSG
	// is open (-1 when closed), and whether ALL draws the FM rows' envelopes instead of diagrams. The
	// window's alone: no knob, no project.
	int last_op;
	int ssg_open;
	// The last press only closed the choice of SSG: a double click's second press does nothing.
	bool press_eaten;
	// Each FM operator's envelope as the graph's columns, kept until its values change.
	struct {
		bool ready;
		int16_t op[PATCH_OP_PARAMS];
		uint16_t low[GRAPH_W - 2], high[GRAPH_W - 2];
	} curves[PATCH_NUM_FM_CHANNELS][PATCH_OPS];

	// The footer's message, and the row or places a file just went to.
	char message[FILES_MESSAGE];
	uint32_t message_color;
	int message_ticks;
	int flash_channel;
	int flash_place;
	int flash_count;
	int flash_ticks;
	uint32_t flash_color;
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

// What a value is shown as: the ROM's DT 4 is -0, the same as 0; SSG 1-7 are off like 0.
static int shown_value(uint32_t id, int value)
{
	int channel, param;
	if (patch_id_split(id, &channel, &param) && channel >= 0 && param >= PARAM_OP_BASE) {
		const int field = (param - PARAM_OP_BASE) % PATCH_OP_PARAMS;
		if (field == OP_DT && value == 4) {
			return 0;
		}
		if (field == OP_SSG && value < 8) {
			return 0;
		}
	}
	return value;
}

static int value_of(mdmi_gui *g, const patch_values *values, uint32_t id)
{
	if (g->dragging && id == g->drag_id) {
		return g->drag_value;
	}
	int channel, param;
	patch_id_split(id, &channel, &param);
	return shown_value(id, channel < 0 ? values->globals[param] : values->channels[channel][param]);
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

// Stage 16 (D45): the noise runs on channel 9's tone (Tone 3, Kit), so channel 9 makes no sound of its own.
static bool noise_takes(const patch_values *values, int channel)
{
	return channel == PATCH_NUM_CHANNELS - 2 && values->globals[GLOBAL_NOISE_RATE] >= PATCH_NOISE_RATE_TONE3;
}

// On channel 10 pitch moves the noise only on Tone 3: a fixed rate has none, Kit's drums their own.
static bool noise_without_pitch(const patch_values *values, int channel)
{
	return channel == PATCH_NUM_CHANNELS - 1 && values->globals[GLOBAL_NOISE_RATE] != PATCH_NOISE_RATE_TONE3;
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
	const bool taken = noise_takes(values, channel);
	text(g, 10, y + 7, buffer, INK(special ? C_FM_LIGHT : taken ? C_LINE : C_TEXT), 3);
	if (dac) {
		text(g, BAR_X, y + 5, "DAC Samples", INK(C_TEXT), 2);
	} else {
		snprintf(buffer, sizeof(buffer), "%.*s%s", special ? SPECIAL_NAME_CHARS : 13, sound_name(values, channel), sound_changed(g, values, channel) ? "*" : "");
		text(g, BAR_X, y + 5, buffer, INK(taken ? C_HINT : C_TEXT), 2);
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
	if (taken) {
		// No bar: the channel makes no sound. Its held notes are cut, as notes without a voice.
		left -= 13 * 6;
		text(g, left, line, "TAKEN BY CH10", C_LABEL, 1);
		if (held > 0) {
			snprintf(buffer, sizeof(buffer), "%d CUT", held);
			left -= 6 + (int)strlen(buffer) * 6;
			text(g, left, line, buffer, C_ERROR, 1);
		}
		return;
	}
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
		g->cells[g->cell_count++] = (cell){x, y, w, h, id, label, kind, -1, LOOK_BAR};
	}
}

// One of a row of buttons, which sets its value.
static void add_choice(mdmi_gui *g, int x, int y, int w, int h, uint32_t id, const char *label, int choice, int look)
{
	add_cell(g, x, y, w, h, id, label, CELL_CHOICE);
	if (g->cell_count > 0) {
		g->cells[g->cell_count - 1].choice = choice;
		g->cells[g->cell_count - 1].look = look;
	}
}

// An operator's field in its column (v8): TL, MUL | AR, DR, SL, SR, RR as bars, nine rows of 22 px,
// 3 px apart inside a group and 6 between groups; DT and RS are segments.
static const struct {
	const char *label;
	int field;
	int y;
} OP_BARS[] = {
	{"TL", OP_TL, 228}, {"MUL", OP_MUL, 253},
	{"AR", OP_AR, 281}, {"DR", OP_DR, 306}, {"SL", OP_SL, 331}, {"SR", OP_SR, 356}, {"RR", OP_RR, 381},
};
#define OP_DT_Y 409
#define OP_RS_Y 434
// The ROM's DT 0-7 for the segments -3 to +3; 4 is -0 and shows as 0.
static const int DT_VALUES[7] = {7, 6, 5, 0, 1, 2, 3};
// Each operator's SSG is a button at its header's right end (art-director, after 0.11.0); a click
// opens the choice under the header in the same column, 3 x 3 cells over TL, MUL and AR.
#define SSG_BUTTON_X 78
#define SSG_BUTTON_Y (OP_HEADER_Y + 1)
#define SSG_BUTTON_W 32
#define SSG_BUTTON_H 12
#define SSG_PICK_Y 227
#define SSG_PICK_H 72
#define SSG_CELL_W 36
#define SSG_CELL_H 22

static uint32_t op_id(int channel, int op, int field)
{
	return PATCH_CHANNEL_ID(channel, PARAM_OP_BASE + op * PATCH_OP_PARAMS + field);
}

// The operator a cell of the FM page belongs to, or -1.
static int op_of(uint32_t id)
{
	int channel, param;
	if (!patch_id_split(id, &channel, &param) || channel < 0 || param < PARAM_OP_BASE) {
		return -1;
	}
	return (param - PARAM_OP_BASE) / PATCH_OP_PARAMS;
}

// The operator whose column the mouse is over, header included, or -1.
static int op_column_at(int x, int y)
{
	for (int op = 0; op < PATCH_OPS; op++) {
		if (x >= GRID_X[op] - 2 && x < GRID_X[op] + OP_W + 2 && y >= OP_HEADER_Y - 2 && y < OP_BOTTOM + 1) {
			return op;
		}
	}
	return -1;
}

// The operator the graph and the frame follow: the one whose SSG is being chosen, the column under the
// mouse, else the one last edited. A value from the host changes none of it.
static int active_op(const mdmi_gui *g)
{
	if (g->ssg_open >= 0) {
		return g->ssg_open;
	}
	const int over = g->dragging || g->file_over ? -1 : op_column_at(g->mouse_x, g->mouse_y);
	return over >= 0 ? over : g->last_op;
}

static int feedback_x(int algorithm);

// Channel 10's noise (stage 16, D45): TYPE and RATE as fields, each followed by its choices, in the
// empty band between the channel's cells and the envelope's loop strip, in the small font so no two
// labels run together. RATE's fixed rates go low to high as L M H (owner); at KIT the drums choose
// their type, so TYPE is dim.
#define NOISE_ROW_Y 82
static const struct {
	uint32_t global;
	const char *label;
	int value, x, w;
} NOISE_CHOICES[] = {
	{GLOBAL_NOISE_TYPE, "PERIODIC", PATCH_NOISE_PERIODIC, 276, 64},
	{GLOBAL_NOISE_TYPE, "WHITE", PATCH_NOISE_WHITE, 342, 50},
	{GLOBAL_NOISE_RATE, "L", PATCH_NOISE_RATE_LOW, 436, 44},
	{GLOBAL_NOISE_RATE, "M", PATCH_NOISE_RATE_MID, 482, 44},
	{GLOBAL_NOISE_RATE, "H", PATCH_NOISE_RATE_HIGH, 528, 44},
	{GLOBAL_NOISE_RATE, "TONE 3", PATCH_NOISE_RATE_TONE3, 574, 66},
	{GLOBAL_NOISE_RATE, "KIT", PATCH_NOISE_RATE_KIT, 642, 65},
};

// The cells of the open page and the global knobs, so drawing and the mouse agree.
static void layout(mdmi_gui *g, const patch_values *values)
{
	// One row of the channel's cells for every kind of channel (v8): x and width of each. PAN is
	// buttons; PSG has none, its place stays (draw_page).
	static const struct {
		int param;
		const char *label;
		int x, w;
	} CHANNEL_CELLS[] = {
		{PARAM_VOLUME, "VOL", 242, 112}, {PARAM_FINE_TUNE, "TUNE", 422, 96}, {PARAM_BEND_RANGE, "BEND", 522, 64},
		{PARAM_PORTA_ON, "PORTA", 590, 50}, {PARAM_PORTA_TIME, "TIME", 644, 64},
	};
	static const char *const PANS[3] = {"L", "C", "R"};
	static const char *const DIGITS[8] = {"0", "1", "2", "3", "4", "5", "6", "7"};
	static const struct {
		int global;
		const char *label;
	} GLOBAL_CELLS[] = {
		{GLOBAL_LFO_ON, "LFO"}, {GLOBAL_LFO_RATE, "LFO RATE"},
		{GLOBAL_FILTER, "FILTER"}, {GLOBAL_CH3_MODE, "CH3 SPECIAL"}, {GLOBAL_DAC, "DAC"}, {GLOBAL_RETRIGGER, "RETRIGGER"},
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
	} else if (is_dac(values, channel) && (*g->source.page == PAGE_EDITOR || bank)) {
		// On the DAC page only TUNE: it alone changes which sample a note plays.
		add_cell(g, CHANNEL_CELLS[1].x, GRID_ROW1_Y, CHANNEL_CELLS[1].w, CELL_H, PATCH_CHANNEL_ID(channel, PARAM_FINE_TUNE), CHANNEL_CELLS[1].label, CELL_ROW);
	} else if (!is_dac(values, channel)) {
		// The Preset is the source at the name line's right end, dragged like any cell.
		add_cell(g, SOURCE_X, 30, SOURCE_W, 22, PATCH_CHANNEL_ID(channel, PARAM_PRESET), "PRESET", CELL_SOURCE);
	}
	if (!is_dac(values, channel) && !bank && *g->source.page != PAGE_ALL) {
		// The choice of SSG first: it lies over its column's rows, and the first cell under the mouse wins.
		if (channel_is_fm(channel) && g->ssg_open >= 0) {
			for (int n = 0; n < 9; n++) {
				add_choice(g, GRID_X[g->ssg_open] + n % 3 * (SSG_CELL_W + 2), SSG_PICK_Y + 1 + n / 3 * (SSG_CELL_H + 2), SSG_CELL_W, SSG_CELL_H, op_id(channel, g->ssg_open, OP_SSG), "", n == 0 ? 0 : 7 + n, LOOK_SHAPE);
			}
		}
		for (size_t i = 0; i < sizeof(CHANNEL_CELLS) / sizeof(*CHANNEL_CELLS); i++) {
			// Channel 10 off Tone 3 has no pitch: only VOL is a cell, the rest dim (draw_page).
			if (noise_without_pitch(values, channel) && CHANNEL_CELLS[i].param != PARAM_VOLUME) {
				continue;
			}
			add_cell(g, CHANNEL_CELLS[i].x, GRID_ROW1_Y, CHANNEL_CELLS[i].w, CELL_H, PATCH_CHANNEL_ID(channel, CHANNEL_CELLS[i].param), CHANNEL_CELLS[i].label, CELL_ROW);
			if (CHANNEL_CELLS[i].param == PARAM_PORTA_ON) {
				g->cells[g->cell_count - 1].look = LOOK_SWITCH;
			}
		}
		if (patch_channel_has(channel, PARAM_PAN)) {
			for (int pan = 0; pan < 3; pan++) {
				add_choice(g, GRID_X[1] + pan * 20, GRID_ROW1_Y, 19, CELL_H, PATCH_CHANNEL_ID(channel, PARAM_PAN), PANS[pan], pan, LOOK_BAR);
			}
		}
		if (channel == PATCH_NUM_CHANNELS - 1) {
			for (size_t i = 0; i < sizeof(NOISE_CHOICES) / sizeof(*NOISE_CHOICES); i++) {
				add_choice(g, NOISE_CHOICES[i].x, NOISE_ROW_Y, NOISE_CHOICES[i].w, CELL_H, NOISE_CHOICES[i].global, NOISE_CHOICES[i].label, NOISE_CHOICES[i].value, LOOK_BAR);
			}
		}
		if (channel_is_fm(channel)) {
			add_cell(g, BLOCK_X, GRAPH_Y, 84, CELL_H, PATCH_CHANNEL_ID(channel, PARAM_AMS), "AMS", CELL_ROW);
			add_cell(g, BLOCK_X + 88, GRAPH_Y, 84, CELL_H, PATCH_CHANNEL_ID(channel, PARAM_FMS), "FMS", CELL_ROW);
			// FB sits in the diagram's corner over operator 1's loop: dragged, a click steps it.
			add_cell(g, feedback_x(values->channels[channel][PARAM_ALGORITHM]), DIAGRAM_Y + 2, 72, 13, PATCH_CHANNEL_ID(channel, PARAM_FEEDBACK), "FB", CELL_ROW);
			g->cells[g->cell_count - 1].look = LOOK_FEEDBACK;
			for (int algorithm = 0; algorithm < 8; algorithm++) {
				add_choice(g, BLOCK_X + algorithm * ALGORITHM_STEP, ALGORITHM_Y, ALGORITHM_W, ALGORITHM_H, PATCH_CHANNEL_ID(channel, PARAM_ALGORITHM), DIGITS[algorithm], algorithm, LOOK_BAR);
			}
			for (int op = 0; op < PATCH_OPS; op++) {
				const int x = GRID_X[op];
				add_cell(g, x + SSG_BUTTON_X, SSG_BUTTON_Y, SSG_BUTTON_W, SSG_BUTTON_H, op_id(channel, op, OP_SSG), "SSG", CELL_SSG);
				for (size_t i = 0; i < sizeof(OP_BARS) / sizeof(*OP_BARS); i++) {
					add_cell(g, x, OP_BARS[i].y, OP_W, OP_ROW_H, op_id(channel, op, OP_BARS[i].field), OP_BARS[i].label, CELL_ROW);
				}
				for (int k = 0; k < 7; k++) {
					add_choice(g, x + SEG_X + k * SEG_PITCH, OP_DT_Y, SEG_PITCH - 1, OP_ROW_H, op_id(channel, op, OP_DT), "DT", DT_VALUES[k], LOOK_SEGMENT);
				}
				for (int k = 0; k < 4; k++) {
					add_choice(g, x + SEG_X + k * SEG_PITCH, OP_RS_Y, SEG_PITCH - 1, OP_ROW_H, op_id(channel, op, OP_RS), "RS", k, LOOK_SEGMENT);
				}
				// AM takes the rest of RS's row, a switch a click flips.
				const int am = x + SEG_X + 4 * SEG_PITCH;
				add_cell(g, am, OP_RS_Y, x + OP_W - am, OP_ROW_H, op_id(channel, op, OP_AM), "AM", CELL_ROW);
				g->cells[g->cell_count - 1].look = LOOK_SWITCH;
			}
		}
	}
	for (int i = 0; i < (int)(sizeof(GLOBAL_CELLS) / sizeof(*GLOBAL_CELLS)); i++) {
		add_cell(g, 8 + i * 118, GLOBALS_Y + 6, 114, 32, (uint32_t)GLOBAL_CELLS[i].global, GLOBAL_CELLS[i].label, CELL_COLUMN);
	}
}

// A line of 1 px from one point to another.
static void line(mdmi_gui *g, int x0, int y0, int x1, int y1, uint32_t color)
{
	const int dx = abs(x1 - x0), dy = abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
	int e = dx - dy;
	for (;;) {
		fill(g, x0, y0, 1, 1, color);
		if (x0 == x1 && y0 == y1) {
			break;
		}
		const int e2 = 2 * e;
		if (e2 > -dy) {
			e -= dy;
			x0 += sx;
		}
		if (e2 < dx) {
			e += dx;
			y0 += sy;
		}
	}
}

// The shapes of SSG 8-15 in small, loud at the top: points in quarters of the width, 0 top, 4 bottom,
// ended by -1.
static const int8_t SSG_SHAPES[8][10][2] = {
	{{0, 0}, {1, 4}, {1, 0}, {2, 4}, {2, 0}, {3, 4}, {3, 0}, {4, 4}, {-1, 0}},
	{{0, 0}, {1, 4}, {4, 4}, {-1, 0}},
	{{0, 0}, {1, 4}, {2, 0}, {3, 4}, {4, 0}, {-1, 0}},
	{{0, 0}, {1, 4}, {1, 0}, {4, 0}, {-1, 0}},
	{{0, 4}, {1, 0}, {1, 4}, {2, 0}, {2, 4}, {3, 0}, {3, 4}, {4, 0}, {-1, 0}},
	{{0, 4}, {1, 0}, {4, 0}, {-1, 0}},
	{{0, 4}, {1, 0}, {2, 4}, {3, 0}, {4, 4}, {-1, 0}},
	{{0, 4}, {1, 0}, {1, 4}, {4, 4}, {-1, 0}},
};

static void ssg_icon(mdmi_gui *g, int x, int y, int w, int h, int ssg, uint32_t color)
{
	if (ssg < 8 || ssg > 15 || w < 2) {
		return;
	}
	const int8_t (*p)[2] = SSG_SHAPES[ssg - 8];
	for (int k = 1; k < 10 && p[k][0] >= 0; k++) {
		line(g, x + p[k - 1][0] * (w - 1) / 4, y + p[k - 1][1] * (h - 1) / 4, x + p[k][0] * (w - 1) / 4, y + p[k][1] * (h - 1) / 4, color);
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
	const int base_raw = channel >= 0 ? patch_base_value(values, channel, param) : -1;
	const int base = base_raw >= 0 ? shown_value(c->id, base_raw) : -1;
	const bool changed = base >= 0 && value != base;
	if (c->kind == CELL_CHOICE) {
		const bool on = value == c->choice;
		if (c->look == LOOK_SEGMENT) {
			// DT and RS: the value's segment lit, DT's middle (0) marked.
			const bool middle = c->choice == 0 && !strcmp(c->label, "DT");
			fill(g, c->x, c->y, c->w, c->h, on ? C_FM_LIGHT : middle ? C_LINE : C_CELL);
		} else if (c->look == LOOK_SHAPE) {
			// One of the choice of SSG: OFF or a shape, the one set filled, the one under the mouse grey.
			const bool over = g->mouse_x >= c->x && g->mouse_x < c->x + c->w && g->mouse_y >= c->y && g->mouse_y < c->y + c->h;
			fill(g, c->x, c->y, c->w, c->h, on ? C_FM_LIGHT : over ? C_LINE : C_CELL);
			if (c->choice == 0) {
				text(g, c->x + (c->w - 17) / 2, c->y + (c->h - 7) / 2, "OFF", on ? C_TEXT : C_LABEL, 1);
			} else {
				ssg_icon(g, c->x + 6, c->y + 7, c->w - 12, 8, c->choice, on ? C_TEXT : C_FM_LIGHT);
			}
			return;
		} else {
			// The algorithm lit in FM's colour, the pan in white as a switch. A label too wide for the
			// big font takes the small one.
			const bool pan = param == PARAM_PAN;
			// The noise's mode is the PSG's own choice, lit in its colour (stage 16).
			const bool psg = c->id == GLOBAL_NOISE_TYPE || c->id == GLOBAL_NOISE_RATE;
			// At Kit the drums choose their type: TYPE is dim, its choice kept for later.
			const bool dim = c->id == GLOBAL_NOISE_TYPE && values->globals[GLOBAL_NOISE_RATE] == PATCH_NOISE_RATE_KIT;
			const bool lit = on && !dim;
			const int scale = !psg && (int)strlen(c->label) * 12 - 2 <= c->w - 2 && c->h >= 16 ? 2 : 1;
			const int width = (int)strlen(c->label) * 6 * scale - scale;
			fill(g, c->x, c->y, c->w, c->h, lit ? (pan ? C_TEXT : psg ? C_PSG : C_FM) : C_CELL);
			text(g, c->x + (c->w - width) / 2, c->y + (c->h - 7 * scale) / 2, c->label, lit ? (pan || psg ? C_BG : C_TEXT) : dim ? (on ? C_HINT : C_LINE) : C_LABEL, scale);
		}
		if (changed && c->choice == base) {
			fill(g, c->x, c->y + c->h - 2, c->w, 2, C_AMBER);
		}
		return;
	}
	if (c->kind == CELL_SSG) {
		// An operator's SSG in its header: the shape, or a dim SSG when off.
		fill(g, c->x, c->y, c->w, c->h, C_BG);
		if (value >= 8) {
			ssg_icon(g, c->x + 4, c->y + 3, c->w - 8, 6, value, changed ? C_AMBER : C_TEXT);
		} else {
			text(g, c->x + (c->w - 17) / 2, c->y + (c->h - 7) / 2, c->label, changed ? C_AMBER : C_HINT, 1);
		}
		if (g->ssg_open >= 0 && op_of(c->id) == g->ssg_open) {
			frame(g, c->x, c->y, c->w, c->h, C_TEXT);
		}
		return;
	}
	if (c->look == LOOK_SWITCH) {
		// AM and PORTA: lit in the channel's colour when on, a click flips it.
		const uint32_t lit = channel >= 0 && !channel_is_fm(channel) ? C_PSG_DARK : C_FM;
		const int width = (int)strlen(c->label) * 6 - 1;
		fill(g, c->x, c->y, c->w, c->h, value ? lit : C_BG);
		if (!value) {
			frame(g, c->x, c->y, c->w, c->h, held ? C_TEXT : C_LINE);
		}
		text(g, c->x + (c->w - width) / 2, c->y + (c->h - 7) / 2, c->label, changed ? C_AMBER : value ? C_TEXT : C_LABEL, 1);
		return;
	}
	if (c->look == LOOK_FEEDBACK) {
		// FB over operator 1's loop: seven segments filled up to it, and the number.
		fill(g, c->x, c->y, c->w, c->h, C_BG);
		text(g, c->x + 2, c->y + 3, c->label, held ? C_TEXT : changed ? C_AMBER : C_LABEL, 1);
		for (int k = 0; k < 7; k++) {
			fill(g, c->x + 16 + k * 6, c->y + 2, 5, 9, k < value ? C_FM_LIGHT : C_CELL);
		}
		snprintf(buffer, sizeof(buffer), "%d", value);
		text(g, c->x + 16 + 7 * 6 + 4, c->y + 3, buffer, C_TEXT, 1);
		if (held) {
			frame(g, c->x, c->y, c->w, c->h, C_TEXT);
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
	const uint32_t dark = held ? C_LINE : fm ? C_FM_DARK : C_PSG_DARK;
	const int max = patch_param(c->id)->max > 0 ? patch_param(c->id)->max : 1;
	if (c->kind == CELL_COLUMN) {
		fill(g, c->x, c->y, c->w, c->h, held ? C_LINE : C_CELL);
	} else {
		// A channel's cell is a bar filled up to its value, as in the mockup; TUNE from its middle.
		fill(g, c->x, c->y, c->w, c->h, C_CELL);
		const int at = c->w * value / max;
		if (param == PARAM_FINE_TUNE) {
			const int middle = c->w * patch_param(c->id)->default_value / max;
			fill(g, c->x + (at < middle ? at : middle), c->y, at < middle ? middle - at : at - middle, c->h, dark);
			fill(g, c->x + middle, c->y, 1, c->h, C_LINE);
		} else {
			fill(g, c->x, c->y, at, c->h, dark);
		}
	}
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
		int label_end = text(g, c->x + 4, c->y + (c->h - 7) / 2, c->label, label, 1);
		// A value without room for its unit drops it: +0 for +0 ct.
		const int room = c->x + c->w - 4 - label_end - 6;
		char *space = strchr(buffer, ' ');
		if ((int)strlen(buffer) * 12 > room && space) {
			*space = 0;
		}
		text_right(g, c->x + c->w - 4, c->y + (c->h - 14) / 2, buffer, shown, 2, room);
		if (changed) {
			fill(g, c->x + (c->w - 2) * base / max, c->y + c->h - 4, 2, 4, C_AMBER);
		}
	}
	if (held) {
		frame(g, c->x, c->y, c->w, c->h, C_TEXT);
	}
}

// A knob the host sent a value since the mouse last touched it (D33): a red dot 3x3 at the cell's top right.
static void draw_cell(mdmi_gui *g, const cell *c, const patch_values *values)
{
	draw_cell_body(g, c, values);
	int channel, param;
	if (!g->source.host_values || !patch_id_split(c->id, &channel, &param) || !__atomic_load_n(&g->source.host_values[GUI_HOST_SLOT(channel, param)], __ATOMIC_RELAXED)) {
		return;
	}
	// A row of choices has its dot on its last button.
	if (c->look == LOOK_SHAPE || (c + 1 < g->cells + g->cell_count && c[1].id == c->id)) {
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

// The diagram in the block (v8): operators of 18 px on a grid of 38 by 21, so three rows keep their
// links apart (the mockup's 18 put rows on each other), the output bus 6 px over
// the frame's bottom, the feedback loop over operator 1.
#define OP_BOX 18
#define OP_HALF (OP_BOX / 2 - 1)
#define DIAGRAM_BUS (DIAGRAM_Y + 69)
#define DIAGRAM_RIGHT (BLOCK_X + 158)
static void op_xy(int algorithm, int op, int *x, int *y)
{
	*x = BLOCK_X + 14 + ALGORITHM_POS[algorithm][op][0] * 38;
	*y = DIAGRAM_Y + 13 + ALGORITHM_POS[algorithm][op][1] * 21;
}

// FB stands at the frame's top left, or top right when operator 1's loop is under it there.
static int feedback_x(int algorithm)
{
	return ALGORITHM_POS[algorithm][0][1] == 0 && ALGORITHM_POS[algorithm][0][0] < 2 ? BLOCK_X + BLOCK_W - 76 : BLOCK_X + 4;
}

static void draw_diagram(mdmi_gui *g, int algorithm, int feedback)
{
	fill(g, BLOCK_X, DIAGRAM_Y, BLOCK_W, DIAGRAM_H, C_BG);
	frame(g, BLOCK_X, DIAGRAM_Y, BLOCK_W, DIAGRAM_H, C_CELL);
	int leftmost = DIAGRAM_RIGHT;
	for (int link = 0; link < 3; link++) {
		int from = ALGORITHM_LINKS[algorithm][link][0], to = ALGORITHM_LINKS[algorithm][link][1];
		if (from < 0) {
			continue;
		}
		int fx, fy, tx, ty;
		op_xy(algorithm, from, &fx, &fy);
		op_xy(algorithm, to, &tx, &ty);
		if (fy == ty) {
			fill(g, fx + OP_BOX, fy + OP_HALF, tx - fx - OP_BOX, 2, C_LABEL);
		} else {
			fill(g, fx + OP_BOX, fy + OP_HALF, tx + OP_HALF - fx - OP_BOX + 2, 2, C_LABEL);
			if (ty > fy) {
				fill(g, tx + OP_HALF, fy + OP_HALF, 2, ty - fy - OP_HALF, C_LABEL);
			} else {
				fill(g, tx + OP_HALF, ty + OP_BOX, 2, fy + OP_HALF - ty - OP_BOX + 2, C_LABEL);
			}
		}
	}
	for (int op = 0; op < PATCH_OPS; op++) {
		int x, y;
		op_xy(algorithm, op, &x, &y);
		if (CARRIERS[algorithm] >> op & 1) {
			fill(g, x + OP_HALF, y + OP_BOX, 2, DIAGRAM_BUS - y - OP_BOX, C_LABEL);
			leftmost = x + OP_HALF < leftmost ? x + OP_HALF : leftmost;
		}
	}
	fill(g, leftmost, DIAGRAM_BUS, DIAGRAM_RIGHT - leftmost, 2, C_LABEL);
	int x, y;
	op_xy(algorithm, 0, &x, &y);
	const uint32_t loop = feedback ? C_LABEL : C_LINE;
	fill(g, x - 5, y - 4, 2, OP_HALF + 6, loop);
	fill(g, x - 5, y - 4, OP_BOX + 10, 2, loop);
	fill(g, x + OP_BOX + 3, y - 4, 2, OP_HALF + 6, loop);
	fill(g, x - 5, y + OP_HALF, 5, 2, loop);
	for (int op = 0; op < PATCH_OPS; op++) {
		op_xy(algorithm, op, &x, &y);
		char digit[2] = {(char)('1' + op), 0};
		if (CARRIERS[algorithm] >> op & 1) {
			fill(g, x, y, OP_BOX, OP_BOX, C_FM);
			text(g, x + (OP_BOX - 10) / 2, y + (OP_BOX - 14) / 2, digit, C_TEXT, 2);
		} else {
			fill(g, x, y, OP_BOX, OP_BOX, C_BG);
			frame(g, x, y, OP_BOX, OP_BOX, C_FM_LIGHT);
			frame(g, x + 1, y + 1, OP_BOX - 2, OP_BOX - 2, C_FM_LIGHT);
			text(g, x + (OP_BOX - 10) / 2, y + (OP_BOX - 14) / 2, digit, C_FM_LIGHT, 2);
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
	// Channel 3 in Ch3 Special Mode has its number light FM, as in the rail.
	text(g, PAGE_X, 30, buffer, is_special(values, channel) ? C_FM_LIGHT : noise_takes(values, channel) ? C_LINE : C_TEXT, 3);
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
	if (y >= ENV_PITCH_Y && y < ENV_PITCH_BOTTOM && noise_without_pitch(patch_view(g->source.patch), *g->source.channel)) {
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
		if (part == ENV_PITCH && noise_without_pitch(patch_view(g->source.patch), *g->source.channel)) {
			snprintf(hint, size, "CH10: SHIFT WORKS ONLY WITH RATE TONE 3");
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

static int envelope_frame(mdmi_gui *g, const patch_shape *shape, int psg, unsigned rate);

static void draw_envelope(mdmi_gui *g, const patch_values *values)
{
	const bool noise = noise_without_pitch(values, *g->source.channel);
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
	text(g, PAGE_X, ENV_PITCH_Y, "PITCH", noise ? C_LINE : C_LABEL, 1);
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
		const uint32_t pitch_ink = noise ? C_LINE : C_PSG;
		if (row < 7) {
			fill(g, x, pitch_top, w - 1, ENV_CENTRE_Y - pitch_top, pitch_ink);
		} else if (row > 7) {
			fill(g, x, ENV_CENTRE_Y + 1, w - 1, pitch_top + ENV_PITCH_ROW - ENV_CENTRE_Y - 1, pitch_ink);
		}
	}
	fill(g, ENV_X, ENV_BASE_Y, ENV_LANE_W - 1, 1, C_LINE);
	fill(g, ENV_X, ENV_CENTRE_Y, ENV_LANE_W - 1, 1, C_LINE);
	// Where the note is, the same green line as on ALL, down both lanes.
	const int playing = envelope_frame(g, shape, psg, mdmi_engine_get_stats(g->source.engine).sample_rate);
	if (playing >= 0 && playing < count) {
		const int x = column_x(count, playing), w = column_x(count, playing + 1) - x;
		fill(g, x + (w - 1) / 2, ENV_LEVEL_Y, 1, ENV_PITCH_BOTTOM - ENV_LEVEL_Y, C_PLAYING);
	}

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

// The graph's columns and the note its rates are drawn for (KS by RS, hardware-specialist): C4.
#define CURVE_COLUMNS (GRAPH_W - 2)
#define CURVE_KEYCODE 16

// An operator's envelope as the graph's columns, the values being dragged included.
static void curve_of(mdmi_gui *g, const patch_values *values, int channel, int op, const uint16_t **low, const uint16_t **high)
{
	int16_t now[PATCH_OP_PARAMS];
	for (int f = 0; f < PATCH_OP_PARAMS; f++) {
		const uint32_t id = op_id(channel, op, f);
		now[f] = g->dragging && g->drag_id == id ? (int16_t)g->drag_value : values->channels[channel][PARAM_OP_BASE + op * PATCH_OP_PARAMS + f];
	}
	if (!g->curves[channel][op].ready || memcmp(now, g->curves[channel][op].op, sizeof(now))) {
		memcpy(g->curves[channel][op].op, now, sizeof(now));
		patch_fm_envelope(now, CURVE_KEYCODE, CURVE_COLUMNS, g->curves[channel][op].low, g->curves[channel][op].high);
		g->curves[channel][op].ready = true;
	}
	*low = g->curves[channel][op].low;
	*high = g->curves[channel][op].high;
}

// The x of a time on the graph's axis, from its left edge, w px wide.
static int curve_x(double seconds, int w)
{
	return (int)(sqrt(seconds / PATCH_FM_SECONDS) * w);
}

// The four envelopes in a box w x h, loud at the top: the active one bright over a dark body, the
// others grey. Without an active one (ALL) carriers are light FM and modulators grey. Columns that
// span a range (a fast SSG) are drawn as the range.
static void draw_curves(mdmi_gui *g, const patch_values *values, int channel, int x0, int y0, int w, int h, int active)
{
	const int algorithm = values->channels[channel][PARAM_ALGORITHM] & 7;
	for (int pass = 0; pass < 3; pass++) {
		for (int op = 0; op < PATCH_OPS; op++) {
			// The active one's body first, then the others' lines, the active one's line on top.
			if ((pass == 1) == (op == active)) {
				continue;
			}
			const uint16_t *low, *high;
			curve_of(g, values, channel, op, &low, &high);
			const uint32_t color = op == active ? C_TEXT : active < 0 && CARRIERS[algorithm] >> op & 1 ? C_FM_LIGHT : C_HINT;
			int last_top = -1, last_bottom = -1;
			for (int k = 0; k < w; k++) {
				// The cached columns this one covers.
				const int from = k * CURVE_COLUMNS / w, to = (k + 1) * CURVE_COLUMNS / w > from ? (k + 1) * CURVE_COLUMNS / w : from + 1;
				int lo = PATCH_FM_SILENT, hi = 0;
				for (int c = from; c < to && c < CURVE_COLUMNS; c++) {
					lo = low[c] < lo ? low[c] : lo;
					hi = high[c] > hi ? high[c] : hi;
				}
				int top = y0 + lo * (h - 1) / PATCH_FM_SILENT, bottom = y0 + hi * (h - 1) / PATCH_FM_SILENT;
				if (pass == 0) {
					fill(g, x0 + k, top, 1, y0 + h - top, C_FM_DARK);
					continue;
				}
				// Joined to the column before, so a step is a line, not a gap.
				const int a = last_top >= 0 && last_bottom < top ? last_bottom : top;
				const int b = last_top >= 0 && last_top > bottom ? last_top : bottom;
				fill(g, x0 + k, a, 1, b - a + 1, color);
				last_top = top;
				last_bottom = bottom;
			}
		}
	}
}

// The graph of the FM page (v8): the four envelopes, Note Off dotted at 1 s with the active curve's
// number there, the time marks.
static void draw_graph(mdmi_gui *g, const patch_values *values, int channel, int active)
{
	const int x0 = GRAPH_X + 1, y0 = GRAPH_Y + 1, w = GRAPH_W - 2, h = GRAPH_H - 2;
	fill(g, GRAPH_X, GRAPH_Y, GRAPH_W, GRAPH_H, C_BG);
	frame(g, GRAPH_X, GRAPH_Y, GRAPH_W, GRAPH_H, C_CELL);
	const int off = x0 + curve_x(PATCH_FM_OFF_SECONDS, w);
	for (int y = GRAPH_Y + 22; y < GRAPH_Y + GRAPH_H - 2; y += 3) {
		fill(g, off, y, 1, 1, C_LINE);
	}
	draw_curves(g, values, channel, x0, y0, w, h, active);
	text(g, off - 19, GRAPH_Y + 24, "OFF", C_HINT, 1);
	// The active curve's number where it crosses Note Off (art-director, after 0.11.0).
	if (active >= 0) {
		const uint16_t *low, *high;
		curve_of(g, values, channel, active, &low, &high);
		int y = y0 + low[off - x0 < CURVE_COLUMNS ? off - x0 : CURVE_COLUMNS - 1] * (h - 1) / PATCH_FM_SILENT - 4;
		y = y < GRAPH_Y + 24 ? GRAPH_Y + 24 : y > GRAPH_Y + GRAPH_H - 12 ? GRAPH_Y + GRAPH_H - 12 : y;
		const char digit[2] = {(char)('1' + active), 0};
		fill(g, off + 3, y, 8, 9, C_BG);
		text(g, off + 4, y + 1, digit, C_TEXT, 1);
	}
	static const struct {
		double seconds;
		const char *label;
	} MARKS[3] = {{0.1, "100 MS"}, {0.5, "500 MS"}, {2.0, "2 S"}};
	for (int i = 0; i < 3; i++) {
		const int x = x0 + curve_x(MARKS[i].seconds, w - 1);
		fill(g, x, GRAPH_Y + GRAPH_H, 1, 3, C_HINT);
		if (i < 2) {
			text(g, x + 2, GRAPH_Y + GRAPH_H - 10, MARKS[i].label, C_LABEL, 1);
		} else {
			text_right(g, x - 2, GRAPH_Y + GRAPH_H - 10, MARKS[i].label, C_LABEL, 1, 40);
		}
	}
	text(g, x0 + 3, GRAPH_Y + GRAPH_H - 10, "C4", C_HINT, 1);
}

static const dac_set *dac_of(const mdmi_gui *g)
{
	return g->source.files->host.view->dac;
}

static bool engine_booted(const mdmi_gui *g)
{
	return mdmi_engine_get_stats(g->source.engine).booted;
}

// The DAC's two pages (stage 12, docs/design/dac-page.md): EDITOR of channel 6 while the DAC is on
// is the grid of notes, BANK the list of them.
static bool dac_editor(const mdmi_gui *g, const patch_values *values)
{
	return *g->source.page == PAGE_EDITOR && is_dac(values, *g->source.channel);
}

static bool dac_bank(const mdmi_gui *g, const patch_values *values)
{
	return *g->source.page == PAGE_BANK && is_dac(values, *g->source.channel);
}

// What a note plays: a file's sample, the ROM's, or nothing.
static bool note_sample(const mdmi_gui *g, int note, rom_sample *sample, const char **name, bool *file)
{
	const dac_sample *own = dac_of(g)->notes[note];
	*file = own != NULL;
	if (!own && dac_of(g)->removed[note]) {
		*name = ROM_SAMPLE_NAMES[note];
		return false;
	}
	if (own) {
		*sample = own->sample;
		*name = own->name;
		return true;
	}
	size_t size;
	const uint8_t *rom = mdmi_engine_builtin_rom(&size);
	*name = ROM_SAMPLE_NAMES[note];
	return rom_image_sample(rom, size, note, sample);
}

static bool rom_has(int note)
{
	return ROM_SAMPLE_NAMES[note][0] != 0;
}

static void rate_text(int rate, char *out, size_t size)
{
	static const char *const RATES[ROM_SAMPLE_RATES] = {"32", "22", "16", "13.4", "11", "8"};
	snprintf(out, size, "%s", RATES[rate]);
}

// A file's name without its folder and .wav, cut to the room.
static void sample_label(const char *path, char *out, size_t size, int chars)
{
	const char *base = path;
	for (const char *c = path; *c; c++) {
		base = *c == '\\' || *c == '/' ? c + 1 : base;
	}
	snprintf(out, size, "%s", base);
	char *dot = strrchr(out, '.');
	if (dot && !_stricmp(dot, ".wav")) {
		*dot = 0;
	}
	if ((int)strlen(out) > chars) {
		out[chars] = 0;
	}
}

// A name in a grid cell, 5 characters: whole if it fits, else the first letter of every word but the
// last, then the last word. Words part at _ - . and spaces, and where a capital follows a small letter;
// digits stay with the word before them.
static void dac_short(const char *name, char *out, size_t size)
{
	char plain[64];
	sample_label(name, plain, sizeof(plain), 63);
	const int length = (int)strlen(plain);
	int starts[32], count = 0;
	for (int i = 0; i < length && count < 32; i++) {
		const char c = plain[i], before = i ? plain[i - 1] : ' ';
		const bool separator = c == '_' || c == '-' || c == '.' || c == ' ';
		const bool after_separator = before == '_' || before == '-' || before == '.' || before == ' ';
		if (!separator && (after_separator || (c >= 'A' && c <= 'Z' && before >= 'a' && before <= 'z'))) {
			starts[count++] = i;
		}
	}
	char joined[64];
	int n = 0;
	for (int i = 0; i < length; i++) {
		if (plain[i] != '_' && plain[i] != '-' && plain[i] != '.' && plain[i] != ' ') {
			joined[n++] = plain[i];
		}
	}
	joined[n] = 0;
	if (n <= 5 || count < 2) {
		snprintf(out, size, "%.5s", joined);
		return;
	}
	char shortened[64];
	int k = 0;
	for (int w = 0; w < count - 1; w++) {
		shortened[k++] = plain[starts[w]];
	}
	for (int i = starts[count - 1]; i < length; i++) {
		if (plain[i] != '_' && plain[i] != '-' && plain[i] != '.' && plain[i] != ' ') {
			shortened[k++] = plain[i];
		}
	}
	shortened[k] = 0;
	snprintf(out, size, "%.5s", shortened);
}

#define GRID_CELL_X 242
#define GRID_CELL_Y 96
#define GRID_STEP_X 38
#define GRID_STEP_Y 18
#define GRID_CELL_W 36
// Sharps darker than naturals, for empty and ROM cells only (owner chose variant B of v12).
#define C_SHARP_EMPTY 0x161616
#define C_SHARP_ROM 0x2E1700
#define GRID_CELL_H 16
#define WAVE_X 242
// The trim's strip over the wave, as the PSG loop's: tabs at the kept part's ends (owner after D38).
#define TRIM_Y 295
#define TRIM_H 14
#define TRIM_TAB_W 8
#define WAVE_Y 311
#define WAVE_W DAC_WAVE_COLUMNS
#define WAVE_H 44
#define DAC_BUTTON_Y 358
#define DAC_LINE1_Y 377
#define DAC_LINE2_Y 387
#define DAC_REPLACE_X 496
#define DAC_CLEAR_X 600
#define DAC_BUTTON_W 100
#define DAC_BUTTON_H 24
#define DAC_DEFAULT_NOTE 36
#define DAC_MODE_Y 56
#define DAC_MODE_H 20
#define DAC_NOTES_X 524
#define DAC_NOTES_W 72
#define DAC_AUDIO_X 600
#define DAC_AUDIO_W 108
#define DAC_PANEL_TEXT_X 320

static int grid_note_at(int x, int y)
{
	if (x < GRID_CELL_X || x >= GRID_CELL_X + 12 * GRID_STEP_X || y < GRID_CELL_Y || y >= GRID_CELL_Y + 11 * GRID_STEP_Y) {
		return -1;
	}
	const int note = (y - GRID_CELL_Y) / GRID_STEP_Y * 12 + (x - GRID_CELL_X) / GRID_STEP_X;
	return note < ROM_IMAGE_NOTES ? note : -1;
}

static bool in_box(int x, int y, int left, int top, int w, int h)
{
	return x >= left && x < left + w && y >= top && y < top + h;
}

// The DAC's room left, at the source's place, and the rest of the top both pages share.
static void draw_dac_top(mdmi_gui *g)
{
	char buffer[32];
	text(g, NAME_X, 34, "DAC Samples", C_TEXT, 2);
	const uint32_t room = rom_image_room(), used = dac_used(dac_of(g));
	fill(g, DAC_FREE_X, 30, DAC_FREE_W, 22, C_CELL);
	fill(g, DAC_FREE_X, 30, (int)((uint64_t)(room - used) * DAC_FREE_W / room), 22, C_DAC_DARK);
	text(g, DAC_FREE_X + 4, 38, "FREE", C_LABEL, 1);
	snprintf(buffer, sizeof(buffer), "%.1f MB", (room - used) / 1048576.0);
	text_right(g, DAC_FREE_X + DAC_FREE_W - 4, 34, buffer, (room - used) * 10 < room ? C_AMBER : C_TEXT, 2, 84);
	// What channel 6 plays: the notes' samples, or one day a track's audio (stage 15, after E5).
	fill(g, DAC_NOTES_X, DAC_MODE_Y, DAC_NOTES_W, DAC_MODE_H, C_DAC_DARK);
	text(g, DAC_NOTES_X + (DAC_NOTES_W - 5 * 12) / 2, DAC_MODE_Y + 3, "NOTES", C_TEXT, 2);
	frame(g, DAC_AUDIO_X, DAC_MODE_Y, DAC_AUDIO_W, DAC_MODE_H, C_CELL);
	text(g, DAC_AUDIO_X + (DAC_AUDIO_W - 8 * 12) / 2, DAC_MODE_Y + 3, "AUDIO IN", C_LINE, 2);
	fill(g, RAIL_W + 1, 79, GUI_WIDTH - RAIL_W - 1, 1, C_CELL);
}

// What the ROM does with samples (hardware-rom.md, section 9), the same on both pages.
static void dac_warnings(mdmi_gui *g, const patch_values *values, int y)
{
	const bool tuned = values->channels[PATCH_NUM_FM_CHANNELS - 1][PARAM_FINE_TUNE] != 64;
	const int player = patch_voice_player(g->source.patch, PATCH_NUM_FM_CHANNELS - 1);
	const bool lent = player >= 0 && player != PATCH_NUM_FM_CHANNELS - 1;
	fill(g, BANK_X, y, BANK_RIGHT - BANK_X, 1, C_CELL);
	text(g, BANK_X, y + 5, "ONE SAMPLE AT A TIME. NO VOLUME. ALL NOTES/SOUND OFF DO NOT STOP IT.", C_HINT, 1);
	text(g, BANK_X, y + 15, "BEND OR TUNE OFF CENTRE PLAYS THE SAMPLE OF A NEIGHBOUR NOTE.", tuned ? C_AMBER : C_HINT, 1);
	text(g, BANK_X, y + 25, "POLYPHONY: A CHORD THAT REACHES CH6 PLAYS SAMPLES, NOT FM.", lent ? C_AMBER : C_HINT, 1);
	text(g, BANK_X, y + 35, "THE DAC SWITCH BELOW RESETS THE CHIP: ALL SOUNDING NOTES ARE CUT.", C_HINT, 1);
}

static int held_dac_note(const mdmi_gui *g)
{
	const uint32_t held = g->source.notes[PATCH_NUM_FM_CHANNELS - 1];
	return GUI_NOTE_HELD(held) ? (int)GUI_NOTE_KEY(held) : -1;
}

static bool drop_aims_at(const mdmi_gui *g, int note)
{
	return g->file_over && g->dac_drop_count > 0 && note >= g->dac_drop_note && note < g->dac_drop_note + g->dac_drop_count;
}

static void dac_button(mdmi_gui *g, int x, const char *label, bool enabled)
{
	const bool over = enabled && !g->file_over && in_box(g->mouse_x, g->mouse_y, x, DAC_BUTTON_Y, DAC_BUTTON_W, DAC_BUTTON_H);
	frame(g, x, DAC_BUTTON_Y, DAC_BUTTON_W, DAC_BUTTON_H, !enabled ? C_CELL : over ? C_TEXT : C_LINE);
	const int width = (int)strlen(label) * 12;
	text(g, x + (DAC_BUTTON_W - width) / 2, DAC_BUTTON_Y + 5, label, !enabled ? C_LINE : over ? C_TEXT : C_LABEL, 2);
}

// The DAC panel's settings (D38, docs/design/dac-page.md): cells of the selected file's edit, the
// trim marks on its wave. None is a knob of the host: a gesture changes the window's copy, and the
// mouse going up prepares the sample once and restarts the core.
enum {
	DACC_NONE = -1, DACC_START, DACC_END, DACC_PITCH, DACC_CENTS, DACC_GAIN, DACC_NORMALIZE, DACC_FADE, DACC_ONE_SHOT,
	DACC_RATE_AUTO, DACC_RATE_END = DACC_RATE_AUTO + ROM_SAMPLE_RATES + 1, DACC_MARK_START = DACC_RATE_END, DACC_MARK_END,
};
#define DAC_ROW_A_Y 398
#define DAC_ROW_B_Y 422
#define DAC_CELL_H 20

static const struct {
	int x, y, w;
	const char *label;
} DAC_CONTROLS[] = {
	// START and END are the trim strip's tabs, no cells.
	{0, 0, 0, "START"}, {0, 0, 0, "END"}, {242, DAC_ROW_B_Y, 84, "PITCH"}, {328, DAC_ROW_B_Y, 62, "CT"},
	{396, DAC_ROW_B_Y, 100, "GAIN"}, {498, DAC_ROW_B_Y, 62, "NORMALIZE"}, {566, DAC_ROW_B_Y, 52, "FADE"}, {620, DAC_ROW_B_Y, 80, "ONE-SHOT"},
	{274, DAC_ROW_A_Y, 32, "AUTO"}, {308, DAC_ROW_A_Y, 22, "32"}, {332, DAC_ROW_A_Y, 22, "22"}, {356, DAC_ROW_A_Y, 22, "16"},
	{380, DAC_ROW_A_Y, 28, "13.4"}, {410, DAC_ROW_A_Y, 22, "11"}, {434, DAC_ROW_A_Y, 22, "8"},
};

static bool gain_clips(const mdmi_gui *g, const dac_sample *own, const dac_edit *edit);

// What the panel shows and changes for a note: the project's sample, or a copy of the ROM's that
// becomes the project's when changed. NULL for an empty note.
static const dac_sample *panel_sample(const mdmi_gui *gui, int note)
{
	mdmi_gui *g = (mdmi_gui *)gui;
	const dac_set *dac = dac_of(g);
	if (note < 0 || note >= ROM_IMAGE_NOTES) {
		return NULL;
	}
	if (dac->notes[note]) {
		return dac->notes[note]->source ? dac->notes[note] : NULL;
	}
	if (g->rom_copy_note != note || g->rom_copy_version != dac->version) {
		dac_free(g->rom_copy);
		g->rom_copy = NULL;
		g->rom_copy_note = note;
		g->rom_copy_version = dac->version;
		size_t size;
		const uint8_t *rom = mdmi_engine_builtin_rom(&size);
		rom_sample sample;
		if (!dac->removed[note] && rom_image_sample(rom, size, note, &sample)) {
			g->rom_copy = dac_from_rom(&sample, ROM_SAMPLE_NAMES[note]);
		}
	}
	return g->rom_copy;
}
static void draw_wave(mdmi_gui *g, int note, const rom_sample *sample, const dac_sample *own, const dac_edit *edit);

static const dac_edit *shown_edit(const mdmi_gui *g, const dac_sample *own)
{
	return g->dac_drag != DACC_NONE && g->dac_drag_note == g->dac_selected ? &g->dac_work : &own->edit;
}

static int wave_x_of(const dac_sample *own, uint32_t frame)
{
	return WAVE_X + (int)((uint64_t)frame * WAVE_W / own->source_frames);
}

static int dac_control_at(const mdmi_gui *g, int x, int y)
{
	const dac_sample *own = panel_sample(g, g->dac_selected);
	if (!own || !own->source) {
		return DACC_NONE;
	}
	if (y >= TRIM_Y && y < TRIM_Y + TRIM_H) {
		const dac_edit *edit = shown_edit(g, own);
		const int start = wave_x_of(own, edit->start), end = wave_x_of(own, edit->end);
		if (x >= start - TRIM_TAB_W && x < start) {
			return DACC_MARK_START;
		}
		if (x >= end && x < end + TRIM_TAB_W) {
			return DACC_MARK_END;
		}
		return DACC_NONE;
	}
	if (y >= WAVE_Y && y < WAVE_Y + WAVE_H) {
		const dac_edit *edit = shown_edit(g, own);
		const int start = wave_x_of(own, edit->start), end = wave_x_of(own, edit->end);
		const bool near_start = abs(x - start) <= 4, near_end = abs(x - end) <= 4;
		if (near_start && near_end) {
			return x < (start + end) / 2 ? DACC_MARK_START : DACC_MARK_END;
		}
		return near_start ? DACC_MARK_START : near_end ? DACC_MARK_END : DACC_NONE;
	}
	for (int c = 0; c < (int)(sizeof(DAC_CONTROLS) / sizeof(*DAC_CONTROLS)); c++) {
		if (in_box(x, y, DAC_CONTROLS[c].x, DAC_CONTROLS[c].y, DAC_CONTROLS[c].w, DAC_CELL_H)) {
			return c;
		}
	}
	return DACC_NONE;
}

static void split_pitch(int pitch, int *semitones, int *cents)
{
	*semitones = (int)lround(pitch / 100.0);
	*cents = pitch - *semitones * 100;
}

static double ms_of(const dac_sample *own, uint32_t frames)
{
	return frames * 1000.0 / own->source_hz;
}

static void ms_text(double ms, char *out, size_t size)
{
	snprintf(out, size, ms < 10 ? "%.2f" : ms < 100 ? "%.1f" : "%.0f", ms);
}

// A cell of the panel: a bar from the left, or from the middle for a signed value, the value at its right.
static void setting_cell(mdmi_gui *g, int c, const char *value, double fill_from, double fill_to, bool lit, bool booted, uint32_t label_color)
{
	const int x = DAC_CONTROLS[c].x, y = DAC_CONTROLS[c].y, w = DAC_CONTROLS[c].w;
	const bool held = g->dac_drag == c || (c == DACC_START && g->dac_drag == DACC_MARK_START) || (c == DACC_END && g->dac_drag == DACC_MARK_END);
	fill(g, x, y, w, DAC_CELL_H, held ? C_LINE : C_CELL);
	if (lit && booted) {
		const int from = x + (int)(fill_from * w), to = x + (int)(fill_to * w);
		fill(g, from < to ? from : to, y, abs(to - from) + 1, DAC_CELL_H, C_DAC_DARK);
	}
	if (held) {
		frame(g, x, y, w, DAC_CELL_H, C_TEXT);
	}
	text(g, x + 4, y + 6, DAC_CONTROLS[c].label, booted ? label_color : C_LINE, 1);
	text_right(g, x + w - 4, y + 3, value, booted ? C_TEXT : C_LINE, 2, w - 8 - (int)strlen(DAC_CONTROLS[c].label) * 6);
}

static void switch_cell(mdmi_gui *g, int c, bool on, bool booted)
{
	const int x = DAC_CONTROLS[c].x, y = DAC_CONTROLS[c].y, w = DAC_CONTROLS[c].w;
	const bool over = booted && g->dac_drag == DACC_NONE && in_box(g->mouse_x, g->mouse_y, x, y, w, DAC_CELL_H);
	fill(g, x, y, w, DAC_CELL_H, on && booted ? C_DAC_DARK : C_CELL);
	frame(g, x, y, w, DAC_CELL_H, over ? C_TEXT : on && booted ? C_DAC : C_LINE);
	const int width = (int)strlen(DAC_CONTROLS[c].label) * 6;
	text(g, x + (w - width) / 2, y + 7, DAC_CONTROLS[c].label, !booted ? C_LINE : on ? C_TEXT : C_LABEL, 1);
}

static void draw_settings(mdmi_gui *g, const dac_sample *own, const dac_edit *edit, bool booted)
{
	char buffer[32];
	text(g, BANK_X, DAC_ROW_A_Y + 7, "RATE", booted ? C_LABEL : C_LINE, 1);
	int semitones, cents;
	split_pitch(edit->pitch, &semitones, &cents);
	snprintf(buffer, sizeof(buffer), "%+d", semitones);
	setting_cell(g, DACC_PITCH, buffer, 0.5, 0.5 + semitones / 48.0, semitones != 0, booted, C_LABEL);
	snprintf(buffer, sizeof(buffer), "%+d", cents);
	setting_cell(g, DACC_CENTS, buffer, 0.5, 0.5 + cents / 100.0, cents != 0, booted, C_LABEL);
	const bool clips = gain_clips(g, own, edit);
	snprintf(buffer, sizeof(buffer), "%+.1f", edit->gain / 10.0);
	setting_cell(g, DACC_GAIN, buffer, 0.5, 0.5 + edit->gain / 480.0, edit->gain != 0, booted, clips ? C_AMBER : C_LABEL);
	if (clips) {
		// The label says it: what the gain pushes past full scale is cut.
		fill(g, DAC_CONTROLS[DACC_GAIN].x + 4, DAC_CONTROLS[DACC_GAIN].y + 6, 24, 7, g->dac_drag == DACC_GAIN ? C_LINE : edit->gain ? C_DAC_DARK : C_CELL);
		text(g, DAC_CONTROLS[DACC_GAIN].x + 4, DAC_CONTROLS[DACC_GAIN].y + 6, "CLIP", C_AMBER, 1);
	}
	switch_cell(g, DACC_NORMALIZE, false, booted);
	// FADE is on almost always (owner): a dim word, and NO FADE in amber when it isn't.
	{
		const int x = DAC_CONTROLS[DACC_FADE].x, y = DAC_CONTROLS[DACC_FADE].y, w = DAC_CONTROLS[DACC_FADE].w;
		const bool over = booted && g->dac_drag == DACC_NONE && in_box(g->mouse_x, g->mouse_y, x, y, w, DAC_CELL_H);
		const char *word = edit->fade ? "FADE" : "NO FADE";
		text(g, x + (w - (int)strlen(word) * 6) / 2, y + 7, word, !booted ? C_LINE : over ? C_TEXT : edit->fade ? C_LINE : C_AMBER, 1);
	}
	switch_cell(g, DACC_ONE_SHOT, edit->one_shot, booted);
	// RATE: the one chosen lit, AUTO's pick dark under it.
	const int picked = dac_rate(own, edit);
	for (int c = DACC_RATE_AUTO; c < DACC_RATE_END; c++) {
		const int x = DAC_CONTROLS[c].x, y = DAC_CONTROLS[c].y, w = DAC_CONTROLS[c].w;
		const int rate = c - DACC_RATE_AUTO - 1;
		const bool chosen = rate == edit->rate, auto_pick = edit->rate == DAC_RATE_AUTO && rate == picked;
		const bool over = booted && g->dac_drag == DACC_NONE && in_box(g->mouse_x, g->mouse_y, x, y, w, DAC_CELL_H);
		fill(g, x, y, w, DAC_CELL_H, !booted ? C_CELL : chosen ? C_DAC : auto_pick ? C_DAC_DARK : over ? C_LINE : C_CELL);
		const int width = (int)strlen(DAC_CONTROLS[c].label) * 6;
		text(g, x + (w - width) / 2, y + 7, DAC_CONTROLS[c].label, !booted ? C_LINE : chosen ? C_BG : auto_pick ? C_TEXT : C_LABEL, 1);
	}
	// What the sample takes of the ROM.
	snprintf(buffer, sizeof(buffer), "%.1f", dac_cost(own, edit) / 1024.0);
	text_right(g, 520, DAC_ROW_A_Y + 3, buffer, g->dac_drag != DACC_NONE ? C_AMBER : C_TEXT, 2, 54);
	text(g, 524, DAC_ROW_A_Y + 7, "KB OF ROM", C_LABEL, 1);
}

// Whether the gain pushes the kept part past full scale.
// Whether the gain cuts the peaks: the prepared bytes say it, and while a gesture lasts the wave's
// columns of the kept part do.
static bool gain_clips(const mdmi_gui *g, const dac_sample *own, const dac_edit *edit)
{
	if (edit == &own->edit) {
		return own->clipped;
	}
	if (g->wave_of != (const void *)own->source) {
		return false;
	}
	const double gain = pow(10.0, edit->gain / 200.0);
	const int first = wave_x_of(own, edit->start) - WAVE_X, last = wave_x_of(own, edit->end) - WAVE_X;
	int peak = 0;
	for (int column = first; column < last && column < WAVE_W; column++) {
		peak = abs(g->wave_low[column]) > peak ? abs(g->wave_low[column]) : peak;
		peak = abs(g->wave_high[column]) > peak ? abs(g->wave_high[column]) : peak;
	}
	return peak * gain > 32767.5;
}

// The wave: a file's source whole, its kept part lit and the trim marks; the ROM's bytes as they are.
static void draw_wave(mdmi_gui *g, int note, const rom_sample *sample, const dac_sample *own, const dac_edit *edit)
{
	fill(g, WAVE_X, WAVE_Y, WAVE_W, WAVE_H, edit ? C_BG : C_CELL);
	const int middle = WAVE_Y + WAVE_H / 2, half = WAVE_H / 2;
	if (!sample) {
		fill(g, WAVE_X, middle, WAVE_W, 1, C_LINE);
		const char *empty = "EMPTY. DROP A WAV HERE OR PRESS REPLACE";
		text(g, WAVE_X + (WAVE_W - (int)strlen(empty) * 6) / 2, middle - 10, empty, C_HINT, 1);
		return;
	}
	// Min and max a column, made again only for another sample.
	const void *of = edit ? (const void *)own->source : (const void *)sample->bytes;
	if (g->wave_of != of || g->wave_note != note) {
		g->wave_of = of;
		g->wave_note = note;
		const uint32_t count = edit ? own->source_frames : sample->length;
		for (int column = 0; column < WAVE_W; column++) {
			uint32_t from = (uint32_t)((uint64_t)column * count / WAVE_W);
			uint32_t to = (uint32_t)((uint64_t)(column + 1) * count / WAVE_W);
			to = to > from ? to : from + 1;
			int low = 32767, high = -32768;
			for (uint32_t i = from; i < to && i < count; i++) {
				const int v = edit ? own->source[i] : sample->bytes[i] * 256;
				low = v < low ? v : low;
				high = v > high ? v : high;
			}
			g->wave_low[column] = (int16_t)(low > high ? 0 : low);
			g->wave_high[column] = (int16_t)(low > high ? 0 : high);
		}
	}
	const double gain = edit ? pow(10.0, edit->gain / 200.0) : 1.0;
	const int start = edit ? wave_x_of(own, edit->start) : WAVE_X, end = edit ? wave_x_of(own, edit->end) : WAVE_X + WAVE_W;
	if (edit) {
		fill(g, start, WAVE_Y, end - start, WAVE_H, C_CELL);
	}
	fill(g, WAVE_X, middle, WAVE_W, 1, C_LINE);
	for (int column = 0; column < WAVE_W; column++) {
		const int x = WAVE_X + column;
		const bool kept = x >= start && x < end;
		const double high = g->wave_high[column] * gain / 32768.0, low = g->wave_low[column] * gain / 32768.0;
		const bool cut = kept && (high > 1 || low < -1) && (edit != &own->edit || own->clipped);
		int top = middle - (int)(fmin(high, 1) * half), bottom = middle - (int)(fmax(low, -1) * half);
		top = top < WAVE_Y ? WAVE_Y : top;
		bottom = bottom >= WAVE_Y + WAVE_H ? WAVE_Y + WAVE_H - 1 : bottom;
		fill(g, x, top, 1, bottom - top + 1, cut ? C_AMBER : kept ? C_DAC : C_DAC_DARK);
	}
	if (edit) {
		// The strip: the kept part's body with its length, a tab outside each end as on the PSG loop,
		// the ends' times beside the tabs, lines from the tabs down the wave.
		const int over = g->dac_drag != DACC_NONE ? g->dac_drag : dac_control_at(g, g->mouse_x, g->mouse_y);
		char words[32], a[16];
		fill(g, start, TRIM_Y, end - start, TRIM_H, C_CELL);
		ms_text(ms_of(own, edit->end - edit->start), a, sizeof(a));
		snprintf(words, sizeof(words), "%s MS", a);
		if ((int)strlen(words) * 6 + 4 <= end - start) {
			text(g, start + (end - start - (int)strlen(words) * 6) / 2, TRIM_Y + 4, words, C_DAC, 1);
		}
		const uint32_t start_ink = g->dac_drag == DACC_MARK_START ? C_AMBER : over == DACC_MARK_START ? C_TEXT : C_DAC;
		const uint32_t end_ink = g->dac_drag == DACC_MARK_END ? C_AMBER : over == DACC_MARK_END ? C_TEXT : C_DAC;
		fill(g, start - TRIM_TAB_W, TRIM_Y, TRIM_TAB_W, TRIM_H, start_ink);
		fill(g, end, TRIM_Y, TRIM_TAB_W, TRIM_H, end_ink);
		fill(g, start, TRIM_Y, 1, WAVE_Y + WAVE_H - TRIM_Y, start_ink);
		fill(g, end - 1, TRIM_Y, 1, WAVE_Y + WAVE_H - TRIM_Y, end_ink);
		// A trimmed start says where it is, left of its tab when there is room.
		if (edit->start) {
			ms_text(ms_of(own, edit->start), a, sizeof(a));
			const int width = (int)strlen(a) * 6;
			if (start - TRIM_TAB_W - 3 - width >= WAVE_X) {
				text(g, start - TRIM_TAB_W - 3 - width, TRIM_Y + 4, a, C_LABEL, 1);
			}
		}
		if (edit->end < own->source_frames) {
			ms_text(ms_of(own, edit->end), a, sizeof(a));
			if (end + TRIM_TAB_W + 3 + (int)strlen(a) * 6 <= WAVE_X + WAVE_W) {
				text(g, end + TRIM_TAB_W + 3, TRIM_Y + 4, a, C_LABEL, 1);
			}
		}
	}
}

// What a gesture on the panel makes of the edit at this point.
static void dac_drag_to(mdmi_gui *g, int x, int y)
{
	const dac_sample *own = panel_sample(g, g->dac_drag_note);
	if (!own || !own->source) {
		return;
	}
	dac_edit *work = &g->dac_work;
	const dac_edit *from = &g->dac_drag_from;
	const int dy = g->dac_drag_y - y, dx = x - g->dac_drag_x;
	g->dac_moved = g->dac_moved || abs(dx) >= DRAG_SLOP || abs(dy) >= DRAG_SLOP;
	if (!g->dac_moved) {
		return;
	}
	// A tab moves by as much as the mouse did, from where it was.
	const long shift = (long)((int64_t)dx * own->source_frames / WAVE_W);
	const uint32_t gap = (uint32_t)(own->source_hz / 1000) + 1;
	int semitones, cents;
	split_pitch(from->pitch, &semitones, &cents);
	switch (g->dac_drag) {
	case DACC_START:
	case DACC_MARK_START: {
		long frame = g->dac_drag == DACC_START ? (long)from->start + dy : (long)from->start + shift;
		const long most = (long)work->end - (long)gap;
		work->start = (uint32_t)(frame < 0 ? 0 : frame > most ? (most > 0 ? most : 0) : frame);
		break;
	}
	case DACC_END:
	case DACC_MARK_END: {
		long frame = g->dac_drag == DACC_END ? (long)from->end + dy : (long)from->end + shift;
		const long least = (long)work->start + (long)gap;
		work->end = (uint32_t)(frame > (long)own->source_frames ? (long)own->source_frames : frame < least ? least : frame);
		break;
	}
	case DACC_PITCH: {
		int s = semitones + dy / 2;
		s = s < -24 ? -24 : s > 24 ? 24 : s;
		int p = s * 100 + cents;
		work->pitch = (int16_t)(p < -DAC_PITCH_MAX ? -DAC_PITCH_MAX : p > DAC_PITCH_MAX ? DAC_PITCH_MAX : p);
		break;
	}
	case DACC_CENTS: {
		int c = cents + dy / 2;
		c = c < -50 ? -50 : c > 50 ? 50 : c;
		int p = semitones * 100 + c;
		work->pitch = (int16_t)(p < -DAC_PITCH_MAX ? -DAC_PITCH_MAX : p > DAC_PITCH_MAX ? DAC_PITCH_MAX : p);
		break;
	}
	case DACC_GAIN: {
		int gain = from->gain + dy / 2 * 5;
		work->gain = (int16_t)(gain < -DAC_GAIN_MAX ? -DAC_GAIN_MAX : gain > DAC_GAIN_MAX ? DAC_GAIN_MAX : gain);
		break;
	}
	default:
		break;
	}
}

// The edit goes to the sample: prepared once, the core restarts (D38).
static void dac_apply(mdmi_gui *g, int note, const dac_edit *edit)
{
	const dac_sample *own = panel_sample(g, note);
	if (!own || (edit->start == own->edit.start && edit->end == own->edit.end && edit->pitch == own->edit.pitch && edit->gain == own->edit.gain
			&& edit->rate == own->edit.rate && edit->fade == own->edit.fade && edit->one_shot == own->edit.one_shot)) {
		return;
	}
	char message[FILES_MESSAGE];
	const bool done = files_edit_sample(g->source.files, note, edit, message, sizeof(message));
	say(g, message, done ? C_AMBER : C_ERROR, done ? MESSAGE_TICKS : ERROR_TICKS);
}

// A press on the panel: a drag begins, or a switch, a rate or NORMALIZE applies at once.
static bool dac_panel_down(mdmi_gui *g, int x, int y)
{
	const int c = dac_control_at(g, x, y);
	const dac_sample *own = panel_sample(g, g->dac_selected);
	if (c == DACC_NONE || !own || !engine_booted(g)) {
		return c != DACC_NONE;
	}
	dac_edit edit = own->edit;
	if (c == DACC_FADE || c == DACC_ONE_SHOT || c == DACC_NORMALIZE || (c >= DACC_RATE_AUTO && c < DACC_RATE_END)) {
		if (c == DACC_FADE) {
			edit.fade = !edit.fade;
		} else if (c == DACC_ONE_SHOT) {
			edit.one_shot = !edit.one_shot;
		} else if (c == DACC_NORMALIZE) {
			edit.gain = (int16_t)dac_normalize_gain(own);
		} else {
			edit.rate = (int8_t)(c - DACC_RATE_AUTO - 1);
		}
		dac_apply(g, g->dac_selected, &edit);
		return true;
	}
	g->dac_drag = c;
	g->dac_drag_note = g->dac_selected;
	g->dac_drag_x = x;
	g->dac_drag_y = y;
	g->dac_drag_from = edit;
	g->dac_work = edit;
	g->dac_moved = false;
	SetCapture(g->window);
	return true;
}

// A double click on a cell or a mark puts that value back: no trim, no pitch, no gain.
static bool dac_panel_double_click(mdmi_gui *g, int x, int y)
{
	const int c = dac_control_at(g, x, y);
	const dac_sample *own = panel_sample(g, g->dac_selected);
	if (c == DACC_NONE || !own) {
		return false;
	}
	if (!engine_booted(g)) {
		return true;
	}
	dac_edit edit = own->edit;
	int semitones, cents;
	split_pitch(edit.pitch, &semitones, &cents);
	switch (c) {
	case DACC_START:
		edit.start = 0;
		break;
	case DACC_END:
		edit.end = own->source_frames;
		break;
	case DACC_MARK_START:
		edit.start = 0;
		break;
	case DACC_MARK_END:
		edit.end = own->source_frames;
		break;
	case DACC_PITCH:
		edit.pitch = (int16_t)cents;
		break;
	case DACC_CENTS:
		edit.pitch = (int16_t)(semitones * 100);
		break;
	case DACC_GAIN:
		edit.gain = 0;
		break;
	default:
		return dac_panel_down(g, x, y);
	}
	dac_apply(g, g->dac_selected, &edit);
	return true;
}

// What the footer says about the panel under the mouse or the gesture.
static bool dac_panel_hint(mdmi_gui *g, char *out, size_t size)
{
	const int c = g->dac_drag != DACC_NONE ? g->dac_drag : dac_control_at(g, g->mouse_x, g->mouse_y);
	const dac_sample *own = panel_sample(g, g->dac_selected);
	if (c == DACC_NONE || !own || !own->source) {
		return false;
	}
	const dac_edit *edit = shown_edit(g, own);
	const bool held = g->dac_drag != DACC_NONE;
	char a[16], b[16];
	int semitones, cents;
	split_pitch(edit->pitch, &semitones, &cents);
	switch (c) {
	case DACC_START:
	case DACC_MARK_START:
		ms_text(ms_of(own, edit->start), a, sizeof(a));
		snprintf(out, size, held ? "START %s MS. RELEASE TO APPLY" : "START %s MS. DRAG. DOUBLE CLICK: 0", a);
		break;
	case DACC_END:
	case DACC_MARK_END:
		ms_text(ms_of(own, edit->end), a, sizeof(a));
		ms_text(ms_of(own, own->source_frames), b, sizeof(b));
		snprintf(out, size, held ? "END %s MS. RELEASE TO APPLY" : "END %s MS OF %s. DOUBLE CLICK: ALL", a, b);
		break;
	case DACC_PITCH:
		snprintf(out, size, "PITCH %+d ST: %.2f S. DOUBLE CLICK: 0", semitones, dac_cost(own, edit) / ROM_SAMPLE_HZ[dac_rate(own, edit)]);
		break;
	case DACC_CENTS:
		snprintf(out, size, "CENTS %+d, -50..+50. DOUBLE CLICK: 0", cents);
		break;
	case DACC_GAIN:
		snprintf(out, size, gain_clips(g, own, edit) ? "GAIN %+.1f DB: THE PEAKS CLIP" : "GAIN %+.1f DB. DOUBLE CLICK: 0", edit->gain / 10.0);
		break;
	case DACC_NORMALIZE:
		snprintf(out, size, "NORMALIZE: GAIN %+.1f DB, PEAK AT -0.5", dac_normalize_gain(own) / 10.0);
		break;
	case DACC_FADE:
		snprintf(out, size, edit->fade ? "FADE: LAST 4 MS FADE OUT. CLICK: OFF" : "NO FADE: THE END MAY CLICK. CLICK: ON");
		break;
	case DACC_ONE_SHOT:
		snprintf(out, size, edit->one_shot ? "ONE-SHOT: NOTE OFF DOES NOT CUT IT" : "ONE-SHOT OFF: NOTE OFF CUTS THE SAMPLE");
		break;
	default: {
		dac_edit other = *edit;
		other.rate = (int8_t)(c - DACC_RATE_AUTO - 1);
		const int rate = dac_rate(own, &other);
		rate_text(rate, a, sizeof(a));
		snprintf(out, size, other.rate == DAC_RATE_AUTO ? "AUTO: %s KHZ FOR THIS SAMPLE, %.1f KB" : "RATE %s KHZ: %.1f KB OF ROM", a, dac_cost(own, &other) / 1024.0);
		break;
	}
	}
	return true;
}

static bool is_sharp(int key)
{
	return key == 1 || key == 3 || key == 6 || key == 8 || key == 10;
}

// EDITOR of channel 6 with the DAC on: every note in a grid, the selected one's sample below.
static void draw_dac_grid(mdmi_gui *g, const patch_values *values)
{
	char buffer[64];
	draw_dac_top(g);
	const bool booted = engine_booted(g);
	const int playing = held_dac_note(g);
	const int hover = g->file_over ? -1 : grid_note_at(g->mouse_x, g->mouse_y);
	static const char *const KEYS[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
	for (int column = 0; column < 12; column++) {
		const int width = (int)strlen(KEYS[column]) * 6;
		text(g, GRID_CELL_X + column * GRID_STEP_X + (GRID_CELL_W - width) / 2, 84, KEYS[column], is_sharp(column) ? C_LINE : C_HINT, 1);
	}
	for (int note = 0; note < ROM_IMAGE_NOTES; note++) {
		const int x = GRID_CELL_X + note % 12 * GRID_STEP_X, y = GRID_CELL_Y + note / 12 * GRID_STEP_Y;
		rom_sample sample;
		const char *name;
		bool file;
		const bool has = note_sample(g, note, &sample, &name, &file);
		uint32_t back, ink;
		const char *shown = name;
		const bool sharp = is_sharp(note % 12);
		const bool moved_here = g->grid_from >= 0 && g->grid_moved && note == g->grid_target && note != g->grid_from;
		if (drop_aims_at(g, note)) {
			back = C_AMBER;
			ink = C_BG;
			shown = g->file_paths[note - g->dac_drop_note];
		} else if (moved_here) {
			// What a drag would put here.
			back = C_AMBER;
			ink = C_BG;
			rom_sample other;
			bool other_file;
			note_sample(g, g->grid_from, &other, &shown, &other_file);
		} else if (!booted && file) {
			back = sharp ? C_SHARP_EMPTY : C_CELL;
			ink = C_AMBER;
		} else if (note == playing) {
			back = C_PLAYING;
			ink = C_BG;
		} else if (!has) {
			back = sharp ? C_SHARP_EMPTY : C_CELL;
			ink = back;
		} else if (file) {
			back = C_DAC;
			ink = C_BG;
		} else {
			back = sharp ? C_SHARP_ROM : C_DAC_DARK;
			ink = C_LABEL;
		}
		fill(g, x, y, GRID_CELL_W, GRID_CELL_H, back);
		if (has || drop_aims_at(g, note) || moved_here) {
			dac_short(shown, buffer, sizeof(buffer));
			text(g, x + 3, y + 4, buffer, ink, 1);
		}
		if (note == g->dac_selected) {
			frame(g, x - 1, y - 1, GRID_CELL_W + 2, GRID_CELL_H + 2, C_TEXT);
			frame(g, x - 2, y - 2, GRID_CELL_W + 4, GRID_CELL_H + 4, C_TEXT);
		} else if (note == hover) {
			frame(g, x - 1, y - 1, GRID_CELL_W + 2, GRID_CELL_H + 2, C_LABEL);
		}
	}
	// The selected note's sample: a file's can be prepared (D38), the ROM's only replaced.
	const int note = g->dac_selected;
	rom_sample sample;
	const char *name;
	bool file;
	const bool has = note_sample(g, note, &sample, &name, &file);
	const dac_sample *own = panel_sample(g, note);
	const bool editable = own && own->source;
	const dac_edit *edit = editable ? shown_edit(g, own) : NULL;
	fill(g, BANK_X, TRIM_Y, WAVE_W, TRIM_H, C_BG);
	draw_wave(g, note, has ? &sample : NULL, own, edit);
	// The note's name big, its number first on the line below (owner, D37).
	char key[8], rate[8];
	note_name(note, key, sizeof(key));
	text(g, BANK_X, DAC_BUTTON_Y, key, note == playing ? C_PLAYING : C_TEXT, 3);
	if (has) {
		sample_label(name, buffer, sizeof(buffer), 14);
		text(g, DAC_PANEL_TEXT_X, DAC_BUTTON_Y, buffer, file ? C_TEXT : C_LABEL, 2);
	} else {
		text(g, DAC_PANEL_TEXT_X, DAC_BUTTON_Y, "EMPTY", C_HINT, 2);
	}
	snprintf(buffer, sizeof(buffer), "%d", note);
	text(g, DAC_PANEL_TEXT_X, DAC_LINE1_Y, buffer, C_TEXT, 1);
	const int after = DAC_PANEL_TEXT_X + (int)strlen(buffer) * 6;
	if (has) {
		// While a gesture lasts, what the edit would make.
		const int shown_rate = edit ? dac_rate(own, edit) : sample.rate;
		const double seconds = (edit ? dac_cost(own, edit) : sample.length) / ROM_SAMPLE_HZ[shown_rate];
		rate_text(shown_rate, rate, sizeof(rate));
		snprintf(buffer, sizeof(buffer), "  %.2f S  %s KHZ", seconds, rate);
		text(g, after, DAC_LINE1_Y, buffer, g->dac_drag != DACC_NONE ? C_AMBER : C_LABEL, 1);
		text(g, 448, DAC_LINE1_Y, !booted && file ? "LOADING" : file ? "FILE" : "ROM", !booted && file ? C_AMBER : file ? C_DAC : C_HINT, 1);
	}
	const bool removed = dac_of(g)->removed[note];
	if (file) {
		snprintf(buffer, sizeof(buffer), rom_has(note) ? "ROM HAD %s" : "ROM HAD NOTHING", ROM_SAMPLE_NAMES[note]);
	} else if (removed) {
		snprintf(buffer, sizeof(buffer), "ROM'S %s REMOVED", ROM_SAMPLE_NAMES[note]);
	} else {
		snprintf(buffer, sizeof(buffer), has ? "BUILT INTO THE ROM" : "ROM HAS NOTHING HERE");
	}
	text(g, DAC_PANEL_TEXT_X, DAC_LINE2_Y, buffer, C_HINT, 1);
	dac_button(g, DAC_REPLACE_X, "REPLACE", booted);
	dac_button(g, DAC_CLEAR_X, removed && !file ? "RESTORE" : "CLEAR", booted && (file || rom_has(note)));
	if (editable) {
		draw_settings(g, own, edit, booted);
		// One line of warnings: what matters now first.
		const bool tuned = values->channels[PATCH_NUM_FM_CHANNELS - 1][PARAM_FINE_TUNE] != 64;
		const int player = patch_voice_player(g->source.patch, PATCH_NUM_FM_CHANNELS - 1);
		const bool lent = player >= 0 && player != PATCH_NUM_FM_CHANNELS - 1;
		text(g, BANK_X, 448, lent ? "POLYPHONY: A CHORD THAT REACHES CH6 PLAYS SAMPLES, NOT FM."
			: tuned ? "BEND OR TUNE OFF CENTRE PLAYS THE SAMPLE OF A NEIGHBOUR NOTE."
			: "ONE SAMPLE AT A TIME. NO VOLUME. ALL NOTES/SOUND OFF DO NOT STOP IT.", lent || tuned ? C_AMBER : C_HINT, 1);
	} else {
		dac_warnings(g, values, 400);
	}
}

static int dac_note_at(const mdmi_gui *g, int x, int y)
{
	if (x < BANK_X || x >= BANK_RIGHT || y < DAC_Y || y >= DAC_Y + DAC_SCROLL_H) {
		return -1;
	}
	return g->dac_top + (y - DAC_Y) / DAC_ROW_H;
}

static void scroll_dac(mdmi_gui *g, int top)
{
	g->dac_top = top < 0 ? 0 : top > DAC_TOP_MAX ? DAC_TOP_MAX : top;
}

static bool over_reset(const mdmi_gui *g, int note, int x, int y)
{
	const int row_y = DAC_Y + (note - g->dac_top) * DAC_ROW_H;
	return note >= 0 && dac_of(g)->notes[note] && x >= DAC_RESET_X && x < DAC_RESET_X + DAC_RESET_W && y >= row_y + 2 && y < row_y + 17;
}

// BANK of channel 6 with the DAC on: every note as a row of the list, like the bank's places.
static void draw_dac_list(mdmi_gui *g, const patch_values *values)
{
	char buffer[64];
	if (!g->dac_was_open) {
		g->dac_was_open = true;
		scroll_dac(g, DAC_FIRST_ROM_NOTE);
	}
	draw_dac_top(g);
	const dac_set *dac = dac_of(g);
	const bool booted = engine_booted(g);
	text(g, 250, 84, "NOTE", C_HINT, 1);
	text(g, 310, 84, "SAMPLE", C_DAC, 1);
	text_right(g, 520, 84, "LENGTH", C_HINT, 1, 40);
	text(g, 528, 84, "RATE", C_HINT, 1);
	text(g, 580, 84, "FILE", C_HINT, 1);
	const int playing = held_dac_note(g);
	const int hover = g->file_over ? -1 : dac_note_at(g, g->mouse_x, g->mouse_y);
	for (int row = 0; row < DAC_ROWS; row++) {
		const int note = g->dac_top + row, y = DAC_Y + row * DAC_ROW_H;
		rom_sample sample;
		const char *name;
		bool file;
		const bool has = note_sample(g, note, &sample, &name, &file);
		const bool target = drop_aims_at(g, note);
		if (target) {
			fill(g, BANK_X, y, BANK_RIGHT - BANK_X, DAC_ROW_H - 1, C_AMBER);
		}
		if (note == playing) {
			fill(g, BANK_X, y, 4, DAC_ROW_H - 1, C_PLAYING);
		}
		fill(g, BANK_X, y + DAC_ROW_H - 1, BANK_RIGHT - BANK_X, 1, note % 12 == 11 ? C_LINE : C_CELL);
		snprintf(buffer, sizeof(buffer), "%d", note);
		text_right(g, 276, y + 3, buffer, target ? C_BG : note == playing ? C_PLAYING : has ? C_LABEL : C_HINT, 2, 30);
		note_name(note, buffer, sizeof(buffer));
		text(g, 282, y + 7, buffer, target ? C_BG : has ? C_HINT : C_LINE, 1);
		if (target) {
			sample_label(g->file_paths[note - g->dac_drop_note], buffer, sizeof(buffer), 14);
			text(g, 310, y + 3, buffer, C_BG, 2);
		} else if (has) {
			sample_label(name, buffer, sizeof(buffer), 14);
			text(g, 310, y + 3, buffer, !booted && file ? C_LINE : file ? C_TEXT : C_LABEL, 2);
			snprintf(buffer, sizeof(buffer), "%.2f S", sample.length / ROM_SAMPLE_HZ[sample.rate]);
			text_right(g, 520, y + 7, buffer, C_LABEL, 1, 60);
			rate_text(sample.rate, buffer, sizeof(buffer));
			strcat(buffer, "K");
			text(g, 528, y + 7, buffer, C_LABEL, 1);
		} else {
			text(g, 310, y + 7, "---", C_LINE, 1);
		}
		if (file && !target) {
			text(g, 580, y + 7, booted ? "FILE" : "LOADING", booted ? C_DAC : C_AMBER, 1);
			const bool over = booted && over_reset(g, note, g->mouse_x, g->mouse_y);
			frame(g, DAC_RESET_X, y + 2, DAC_RESET_W, 15, over ? C_TEXT : C_LINE);
			text(g, DAC_RESET_X + 15, y + 6, "CLEAR", over ? C_TEXT : C_LABEL, 1);
		}
		if (hover == note) {
			frame(g, BANK_X, y, BANK_RIGHT - BANK_X, DAC_ROW_H - 1, C_TEXT);
		}
	}
	// The track shows the whole map: notes with a sample, files, the note held, the drop.
	fill(g, SCROLL_X, DAC_Y, 6, DAC_SCROLL_H, C_CELL);
	for (int note = 0; note < ROM_IMAGE_NOTES; note++) {
		rom_sample sample;
		const char *name;
		bool file;
		if (note_sample(g, note, &sample, &name, &file)) {
			fill(g, SCROLL_X, DAC_Y + note * DAC_SCROLL_H / ROM_IMAGE_NOTES, 6, 3, C_DAC_DARK);
		}
	}
	const bool over_scroll = g->mouse_x >= SCROLL_X && g->mouse_x < SCROLL_X + 6 && g->mouse_y >= DAC_Y && g->mouse_y < DAC_Y + DAC_SCROLL_H;
	frame(g, SCROLL_X, DAC_Y + g->dac_top * DAC_SCROLL_H / ROM_IMAGE_NOTES, 6, DAC_SCROLL_H * DAC_ROWS / ROM_IMAGE_NOTES, over_scroll ? C_LABEL : C_LINE);
	for (int note = 0; note < ROM_IMAGE_NOTES; note++) {
		const int y = DAC_Y + note * DAC_SCROLL_H / ROM_IMAGE_NOTES;
		if (dac->notes[note]) {
			fill(g, SCROLL_X, y, 3, 2, C_DAC);
		}
		if (drop_aims_at(g, note)) {
			fill(g, SCROLL_X, y, 6, 3, C_AMBER);
		}
	}
	if (playing >= 0) {
		fill(g, SCROLL_X, DAC_Y + playing * DAC_SCROLL_H / ROM_IMAGE_NOTES, 6, 3, C_PLAYING);
	}
	dac_warnings(g, values, 400);
}

// What the footer says about a note.
static void note_hint(const mdmi_gui *g, int note, bool grid, char *out, size_t size)
{
	rom_sample sample;
	const char *name;
	bool file;
	char key[8], rate[8];
	note_name(note, key, sizeof(key));
	(void)grid;
	if (!note_sample(g, note, &sample, &name, &file)) {
		snprintf(out, size, "%d %s EMPTY: DROP A WAV HERE", note, key);
		return;
	}
	rate_text(sample.rate, rate, sizeof(rate));
	// The footer holds about 42 characters before the version: a file's name goes last, cut at its end.
	if (file) {
		snprintf(out, size, "%d %s %.2f S %s KHZ %.60s", note, key, sample.length / ROM_SAMPLE_HZ[sample.rate], rate, name);
	} else {
		snprintf(out, size, "%d %s %s %.2f S %s KHZ, ROM", note, key, name, sample.length / ROM_SAMPLE_HZ[sample.rate], rate);
	}
}

// What the footer says about the DAC's pages under the mouse.
static bool dac_hint(mdmi_gui *g, char *out, size_t size)
{
	const patch_values *values = patch_view(g->source.patch);
	const bool grid = dac_editor(g, values), list = dac_bank(g, values);
	if (!grid && !list) {
		return false;
	}
	const int x = g->mouse_x, y = g->mouse_y;
	if (grid && g->grid_from >= 0 && g->grid_moved) {
		const bool copy = GetKeyState(VK_CONTROL) < 0;
		if (g->grid_target < 0 || g->grid_target == g->grid_from) {
			snprintf(out, size, "%d: DROP ON A NOTE. OUTSIDE: CANCEL", g->grid_from);
		} else {
			snprintf(out, size, copy ? "COPY %d TO %d" : "SWAP %d AND %d. CTRL COPIES", g->grid_from, g->grid_target);
		}
		return true;
	}
	if (grid && engine_booted(g) && dac_panel_hint(g, out, size)) {
		return true;
	}
	if (in_box(x, y, DAC_FREE_X, 30, DAC_FREE_W, 22)) {
		const double free = rom_image_room() - dac_used(dac_of(g));
		snprintf(out, size, "FREE %.2f MB: %.0f S AT 32 KHZ, %.0f S AT 22 KHZ", free / 1048576.0, free / ROM_SAMPLE_HZ[0], free / ROM_SAMPLE_HZ[1]);
		return true;
	}
	const int note = grid ? grid_note_at(x, y) : dac_note_at(g, x, y);
	const bool replace = grid && in_box(x, y, DAC_REPLACE_X, DAC_BUTTON_Y, DAC_BUTTON_W, DAC_BUTTON_H);
	const bool clear = grid && in_box(x, y, DAC_CLEAR_X, DAC_BUTTON_Y, DAC_BUTTON_W, DAC_BUTTON_H);
	const bool wave = grid && in_box(x, y, WAVE_X, WAVE_Y, WAVE_W, WAVE_H);
	if (in_box(x, y, DAC_NOTES_X, DAC_MODE_Y, DAC_NOTES_W, DAC_MODE_H)) {
		snprintf(out, size, "NOTES: CH6 PLAYS THE SAMPLES BELOW");
		return true;
	}
	if (in_box(x, y, DAC_AUDIO_X, DAC_MODE_Y, DAC_AUDIO_W, DAC_MODE_H)) {
		snprintf(out, size, "AUDIO IN: CH6 PLAYS TRACK AUDIO. NOT YET");
		return true;
	}
	if (note < 0 && !replace && !clear && !wave) {
		return false;
	}
	if (!engine_booted(g)) {
		snprintf(out, size, "ENGINE RESTARTS: NO SOUND UNTIL LOADED");
		return true;
	}
	const int selected = g->dac_selected;
	char key[8];
	if (replace) {
		note_name(selected, key, sizeof(key));
		snprintf(out, size, "REPLACE %d %s FROM A WAV", selected, key);
	} else if (clear && dac_of(g)->notes[selected]) {
		snprintf(out, size, "CLEAR %d: BACK TO %s", selected, rom_has(selected) && !dac_of(g)->removed[selected] ? ROM_SAMPLE_NAMES[selected] : "EMPTY");
	} else if (clear && rom_has(selected) && dac_of(g)->removed[selected]) {
		snprintf(out, size, "RESTORE %d: THE ROM'S %s BACK", selected, ROM_SAMPLE_NAMES[selected]);
	} else if (clear && rom_has(selected)) {
		snprintf(out, size, "CLEAR %d: REMOVE THE ROM'S %s", selected, ROM_SAMPLE_NAMES[selected]);
	} else if (clear) {
		snprintf(out, size, "CLEAR: %d IS EMPTY", selected);
	} else if (wave) {
		note_hint(g, selected, true, out, size);
	} else if (!grid && over_reset(g, note, x, y)) {
		snprintf(out, size, "CLEAR %d: BACK TO %s", note, rom_has(note) && !dac_of(g)->removed[note] ? ROM_SAMPLE_NAMES[note] : "EMPTY");
	} else {
		note_hint(g, note, grid, out, size);
	}
	return true;
}

static void replace_sample(mdmi_gui *g, int note);

static void draw_page(mdmi_gui *g, const patch_values *values)
{
	const int channel = *g->source.channel;
	char buffer[64];
	draw_page_top(g, values);
	if (is_dac(values, channel)) {
		draw_dac_grid(g, values);
		return;
	}
	if (!channel_is_fm(channel)) {
		// PSG has no pan; its place stays so TUNE and BEND are where they are on FM.
		frame(g, GRID_X[1], GRID_ROW1_Y, 60, CELL_H, C_CELL);
		text(g, GRID_X[1] + 4, GRID_ROW1_Y + 6, "PAN", C_LINE, 1);
		text_right(g, GRID_X[1] + 56, GRID_ROW1_Y + 3, "-", C_LINE, 2, 20);
		text(g, PAGE_X, 442, channel == PATCH_NUM_CHANNELS - 1 ? "SN76489 NOISE" : "SN76489 SQUARE", C_HINT, 1);
		if (channel == PATCH_NUM_CHANNELS - 1) {
			const bool kit = values->globals[GLOBAL_NOISE_RATE] == PATCH_NOISE_RATE_KIT;
			fill(g, 242, NOISE_ROW_Y, 32, CELL_H, C_CELL);
			text(g, 246, NOISE_ROW_Y + 6, "TYPE", kit ? C_LINE : C_LABEL, 1);
			fill(g, 402, NOISE_ROW_Y, 32, CELL_H, C_CELL);
			text(g, 406, NOISE_ROW_Y + 6, "RATE", C_LABEL, 1);
			text(g, 332, 442, "TYPE AND RATE: NOT CHECKED ON CONSOLE", C_HINT, 1);
			if (noise_without_pitch(values, channel)) {
				// The places of TUNE, BEND, PORTA and TIME, dim like PAN's: no pitch off Tone 3.
				static const struct {
					const char *label;
					int x, w;
				} NO_PITCH[] = {{"TUNE", 422, 96}, {"BEND", 522, 64}, {"PORTA", 590, 50}, {"TIME", 644, 64}};
				for (size_t i = 0; i < sizeof(NO_PITCH) / sizeof(*NO_PITCH); i++) {
					fill(g, NO_PITCH[i].x, GRID_ROW1_Y, NO_PITCH[i].w, CELL_H, C_BG);
					frame(g, NO_PITCH[i].x, GRID_ROW1_Y, NO_PITCH[i].w, CELL_H, C_CELL);
					text(g, NO_PITCH[i].x + 4, GRID_ROW1_Y + 6, NO_PITCH[i].label, C_LINE, 1);
				}
			}
		} else if (noise_takes(values, channel)) {
			text(g, 332, 442, "TAKEN BY CH10 NOISE (TONE 3, KIT): CH9 NOTES ARE SILENT", C_AMBER, 1);
		}
		draw_envelope(g, values);
		return;
	}
	const int algorithm = value_of(g, values, PATCH_CHANNEL_ID(channel, PARAM_ALGORITHM));
	draw_diagram(g, algorithm, value_of(g, values, PATCH_CHANNEL_ID(channel, PARAM_FEEDBACK)));
	const int active = active_op(g);
	draw_graph(g, values, channel, active);
	for (int op = 0; op < PATCH_OPS; op++) {
		const int x = GRID_X[op];
		const bool carrier = CARRIERS[algorithm] >> op & 1;
		snprintf(buffer, sizeof(buffer), "OP%d %s", op + 1, carrier ? "CARRIER" : "MOD");
		// The active operator's header framed white; no frame round the column (after 0.11.0).
		if (carrier) {
			fill(g, x, OP_HEADER_Y, OP_W, OP_HEADER_H, C_FM);
		} else {
			frame(g, x, OP_HEADER_Y, OP_W, OP_HEADER_H, C_FM_LIGHT);
		}
		if (op == active) {
			frame(g, x, OP_HEADER_Y, OP_W, OP_HEADER_H, C_TEXT);
		}
		text(g, x + 4, OP_HEADER_Y + 4, buffer, carrier || op == active ? C_TEXT : C_FM_LIGHT, 1);
		// The label fields of DT and RS, amber when the value differs from the base (D31).
		static const struct {
			const char *label;
			int field, y;
		} FIELDS[2] = {{"DT", OP_DT, OP_DT_Y}, {"RS", OP_RS, OP_RS_Y}};
		for (int i = 0; i < 2; i++) {
			const uint32_t id = op_id(channel, op, FIELDS[i].field);
			const int base = patch_base_value(values, channel, PARAM_OP_BASE + op * PATCH_OP_PARAMS + FIELDS[i].field);
			const bool changed = base >= 0 && shown_value(id, base) != value_of(g, values, id);
			fill(g, x, FIELDS[i].y, SEG_X - 1, OP_ROW_H, C_CELL);
			text(g, x + 4, FIELDS[i].y + (OP_ROW_H - 7) / 2, FIELDS[i].label, changed ? C_AMBER : C_LABEL, 1);
		}
	}
}

static int hovered(const mdmi_gui *g)
{
	// The open choice of SSG covers the rows under it, gaps between its cells included.
	const bool in_choice = g->ssg_open >= 0 && g->mouse_x >= GRID_X[g->ssg_open] - 1 && g->mouse_x < GRID_X[g->ssg_open] + OP_W + 1
		&& g->mouse_y >= SSG_PICK_Y && g->mouse_y < SSG_PICK_Y + SSG_PICK_H;
	for (int i = 0; i < g->cell_count; i++) {
		if (in_choice && g->cells[i].look != LOOK_SHAPE) {
			continue;
		}
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
	} else if (dac_hint(g, buffer, sizeof(buffer))) {
		footer_text(g, buffer, C_LABEL);
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
	if (is_dac(values, channel)) {
		draw_dac_list(g, values);
		return;
	}
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
			// The diagram or, after a click on it, the four envelopes in small, each row its own.
			if (*g->source.all_envelopes >> channel & 1) {
				draw_curves(g, values, channel, ALL_SOUND_X, y + 3, ALL_SOUND_W, 33, -1);
				fill(g, ALL_SOUND_X, y + 36, ALL_SOUND_W, 1, C_LINE);
			} else {
				draw_mini_diagram(g, ALL_SOUND_X, y, v[PARAM_ALGORITHM], v[PARAM_FEEDBACK], is_special(values, channel) ? C_FM_LIGHT : C_FM);
			}
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
		if (g->cells[i].look != LOOK_SHAPE) {
			draw_cell(g, &g->cells[i], values);
		}
	}
	// The choice of SSG over its column's rows, so it goes last.
	if (g->ssg_open >= 0 && *g->source.page == PAGE_EDITOR && channel_is_fm(*g->source.channel)) {
		fill(g, GRID_X[g->ssg_open] - 1, SSG_PICK_Y, OP_W + 2, SSG_PICK_H, C_BG);
		frame(g, GRID_X[g->ssg_open] - 1, SSG_PICK_Y, OP_W + 2, SSG_PICK_H, C_LABEL);
		for (int i = 0; i < g->cell_count; i++) {
			if (g->cells[i].look == LOOK_SHAPE) {
				draw_cell(g, &g->cells[i], values);
			}
		}
	}
	// Hover last, so nothing covers it. ALL has no hover frames (owner after 0.11.0).
	const bool all = *g->source.page == PAGE_ALL;
	int over = g->dragging || g->file_over || all ? -1 : hovered(g);
	// A shape of the choice of SSG shows the mouse by its grey fill alone (owner after 0.11.0).
	if (over >= 0 && g->cells[over].look != LOOK_SHAPE) {
		const cell *c = &g->cells[over];
		frame(g, c->x, c->y, c->w, c->h, C_TEXT);
	}
	int row = g->dragging || g->file_over || all ? -1 : row_at(g, g->mouse_x, g->mouse_y);
	if (row >= 0) {
		frame(g, 0, row_y(row), RAIL_W, ROW_H, C_TEXT);
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

// A knob the mouse moves is the window's: its host mark goes at once, until the host sends it a
// value again (D33).
static void edit(mdmi_gui *g, int kind, uint32_t id, int value)
{
	int channel, param;
	if (kind != GUI_EDIT_END && g->source.host_values && patch_id_split(id, &channel, &param)) {
		__atomic_store_n(&g->source.host_values[GUI_HOST_SLOT(channel, param)], 0, __ATOMIC_RELAXED);
	}
	g->source.edit(g->source.context, kind, id, value);
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

// REPLACE on the DAC's grid: one WAV for the selected note (stage 12).
static void replace_sample(mdmi_gui *g, int note)
{
	wchar_t buffer[MAX_PATH * 2] = {0};
	OPENFILENAMEW dialog = {0};
	dialog.lStructSize = sizeof(dialog);
	dialog.hwndOwner = g->window;
	dialog.lpstrFilter = L"WAV (*.wav)\0*.wav\0All files\0*.*\0";
	dialog.lpstrFile = buffer;
	dialog.nMaxFile = MAX_PATH * 2;
	dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
	if (!GetOpenFileNameW(&dialog)) {
		return;
	}
	char path[MAX_PATH * 3];
	to_utf8(buffer, path, sizeof(path));
	const char *paths[1] = {path};
	char message[FILES_MESSAGE];
	const int loaded = files_put_samples(g->source.files, paths, 1, note, message, sizeof(message));
	say(g, message, loaded ? C_AMBER : C_ERROR, loaded ? MESSAGE_TICKS : ERROR_TICKS);
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
	// The choice of SSG open: a shape sets it, any other click only closes the choice.
	g->press_eaten = g->ssg_open >= 0;
	if (g->ssg_open >= 0) {
		g->mouse_x = x;
		g->mouse_y = y;
		const int index = hovered(g);
		if (index >= 0 && g->cells[index].look == LOOK_SHAPE) {
			edit_once(g, g->cells[index].id, g->cells[index].choice);
		}
		g->ssg_open = -1;
		refresh(g);
		return;
	}
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
	if (dac_editor(g, patch_view(g->source.patch))) {
		const int note = grid_note_at(x, y);
		const bool booted = engine_booted(g);
		if (dac_panel_down(g, x, y)) {
			refresh(g);
			return;
		}
		if (note >= 0) {
			g->dac_selected = note;
			// A press on a note that plays something may become a drag to another note.
			rom_sample sample;
			const char *name;
			bool file;
			if (booted && note_sample(g, note, &sample, &name, &file)) {
				g->grid_from = note;
				g->grid_moved = false;
				g->grid_target = note;
				g->grid_press_x = x;
				g->grid_press_y = y;
				SetCapture(g->window);
			}
		} else if (booted && in_box(x, y, DAC_REPLACE_X, DAC_BUTTON_Y, DAC_BUTTON_W, DAC_BUTTON_H)) {
			replace_sample(g, g->dac_selected);
		} else if (booted && in_box(x, y, DAC_CLEAR_X, DAC_BUTTON_Y, DAC_BUTTON_W, DAC_BUTTON_H)) {
			char message[FILES_MESSAGE];
			if (files_reset_sample(g->source.files, g->dac_selected, message, sizeof(message))) {
				say(g, message, C_AMBER, MESSAGE_TICKS);
			}
		}
		if (note >= 0 || (y >= DAC_BUTTON_Y && y < DAC_BUTTON_Y + DAC_BUTTON_H)) {
			refresh(g);
			return;
		}
	}
	if (dac_bank(g, patch_view(g->source.patch))) {
		const int note = dac_note_at(g, x, y);
		if (note >= 0 && over_reset(g, note, x, y) && engine_booted(g)) {
			char message[FILES_MESSAGE];
			if (files_reset_sample(g->source.files, note, message, sizeof(message))) {
				say(g, message, C_AMBER, MESSAGE_TICKS);
			}
		} else if (x >= SCROLL_X && x < SCROLL_X + 6 && y >= DAC_Y && y < DAC_Y + DAC_SCROLL_H) {
			scroll_dac(g, (y - DAC_Y) * ROM_IMAGE_NOTES / DAC_SCROLL_H - DAC_ROWS / 2);
		}
		if (note >= 0 || (x >= SCROLL_X && y >= DAC_Y && y < DAC_Y + DAC_SCROLL_H)) {
			refresh(g);
			return;
		}
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
			// The sound of a PSG row opens its page; of an FM row it switches that row between diagram
			// and envelopes (stage 11).
			if (x >= ALL_SOUND_X && x < ALL_SOUND_X + ALL_SOUND_W && takes_fm(patch_view(g->source.patch), row)) {
				*g->source.all_envelopes ^= (uint8_t)(1 << row);
			} else if (x >= ALL_SOUND_X && x < ALL_SOUND_X + ALL_SOUND_W) {
				*g->source.page = PAGE_EDITOR;
			}
			refresh(g);
		}
		return;
	}
	const cell *c = &g->cells[index];
	if (c->kind == CELL_SSG) {
		g->ssg_open = g->last_op = op_of(c->id);
		refresh(g);
		return;
	}
	if (op_of(c->id) >= 0) {
		g->last_op = op_of(c->id);
	}
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
	if (g->dac_drag != DACC_NONE) {
		dac_drag_to(g, x, y);
		refresh(g);
		return;
	}
	if (g->grid_from >= 0) {
		g->grid_moved = g->grid_moved || abs(x - g->grid_press_x) >= 4 || abs(y - g->grid_press_y) >= 4;
		g->grid_target = grid_note_at(x, y);
		refresh(g);
		return;
	}
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
	if (g->grid_from >= 0) {
		const int from = g->grid_from, to = g->grid_target;
		const bool moved = g->grid_moved;
		g->grid_from = -1;
		if (GetCapture() == g->window) {
			ReleaseCapture();
		}
		if (moved && to >= 0 && to != from) {
			char message[FILES_MESSAGE] = "";
			const bool copy = GetKeyState(VK_CONTROL) < 0;
			if (files_move_sample(g->source.files, from, to, copy, message, sizeof(message))) {
				g->dac_selected = to;
				say(g, message, C_AMBER, MESSAGE_TICKS);
			} else if (message[0]) {
				say(g, message, C_ERROR, ERROR_TICKS);
			}
		}
		refresh(g);
		return;
	}
	if (g->dac_drag != DACC_NONE) {
		const dac_edit edit = g->dac_work;
		const int note = g->dac_drag_note;
		g->dac_drag = DACC_NONE;
		if (GetCapture() == g->window) {
			ReleaseCapture();
		}
		dac_apply(g, note, &edit);
		refresh(g);
		return;
	}
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
	// The first press set or closed the choice of SSG; what is under the mouse now is another cell.
	if (g->press_eaten) {
		g->press_eaten = false;
		return;
	}
	if (dac_editor(g, patch_view(g->source.patch))) {
		if (dac_panel_double_click(g, x, y)) {
			refresh(g);
		} else {
			on_down(g, x, y);
		}
		return;
	}
	// A quick second press on the envelope draws as the first did.
	patch_shape scratch;
	if (is_envelope_page(g) && envelope_part_at(g, shown_shape(g, &scratch), x, y) != ENV_NONE) {
		on_down(g, x, y);
		return;
	}
	// On ALL a quick second click on a row's sound switches again, as the first did.
	if (*g->source.page == PAGE_ALL && x >= ALL_SOUND_X && x < ALL_SOUND_X + ALL_SOUND_W && row_at(g, x, y) >= 0) {
		on_down(g, x, y);
		return;
	}
	int index = *g->source.page == PAGE_EDITOR ? hovered(g) : -1;
	// The choice of SSG and its button take a quick second click as a click.
	if (g->ssg_open >= 0 || (index >= 0 && g->cells[index].kind == CELL_SSG)) {
		on_down(g, x, y);
		return;
	}
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
	g->file_count = g->file_instruments = g->file_envelopes = g->file_samples = 0;
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
				g->file_samples += files_kind(utf8) == FILES_SAMPLE;
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
	g->dac_drop_count = 0;
	const patch_values *values = patch_view(g->source.patch);
	if (g->file_samples > 0) {
		// WAV go only to the DAC page's notes, one after another from the row (stage 12).
		const int note = dac_editor(g, values) ? grid_note_at(x, y) : dac_bank(g, values) ? dac_note_at(g, x, y) : -1;
		if (!dac_editor(g, values) && !dac_bank(g, values)) {
			hint(g, C_LABEL, "WAV: open CH6 with DAC on");
		} else if (!engine_booted(g)) {
			hint(g, C_LABEL, "Wait: the engine is loading");
		} else if (g->file_samples != g->file_count) {
			hint(g, C_LABEL, "Only WAV files go to notes");
		} else if (note < 0) {
			hint(g, C_LABEL, "Drop the WAV on a note");
		} else {
			g->dac_drop_note = note;
			g->dac_drop_count = g->file_samples < ROM_IMAGE_NOTES - note ? g->file_samples : ROM_IMAGE_NOTES - note;
			if (g->dac_drop_count == 1) {
				hint(g, C_AMBER, "%s > note %d", g->file_stem, note);
			} else {
				hint(g, C_AMBER, "%d WAV > notes %d-%d%s", g->dac_drop_count, note, note + g->dac_drop_count - 1, g->file_samples > g->dac_drop_count ? ", the rest won't fit" : "");
			}
			return true;
		}
		return false;
	}
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
	if (fits && g->dac_drop_count > 0) {
		const char *paths[ROM_IMAGE_NOTES];
		for (int i = 0; i < g->dac_drop_count; i++) {
			paths[i] = g->file_paths[i];
		}
		char message[FILES_MESSAGE];
		const int loaded = files_put_samples(g->source.files, paths, g->dac_drop_count, g->dac_drop_note, message, sizeof(message));
		say(g, message, loaded ? C_AMBER : C_ERROR, loaded ? MESSAGE_TICKS : ERROR_TICKS);
		if (loaded) {
			g->dac_selected = g->dac_drop_note;
		}
	} else if (fits && g->drop_channel >= 0) {
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
	} else if (dac_bank(g, patch_view(g->source.patch)) && client.x / g->scale >= BANK_X) {
		scroll_dac(g, g->dac_top - delta / WHEEL_DELTA * WHEEL_ROWS);
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
	case WM_SETCURSOR:
		// The trim marks are the window's one sideways drag.
		if (LOWORD(lparam) == HTCLIENT && dac_editor(g, patch_view(g->source.patch))) {
			const int c = g->dac_drag != DACC_NONE ? g->dac_drag : dac_control_at(g, g->mouse_x, g->mouse_y);
			if (c == DACC_MARK_START || c == DACC_MARK_END) {
				SetCursor(LoadCursor(NULL, IDC_SIZEWE));
				return TRUE;
			}
		}
		return DefWindowProcA(window, message, wparam, lparam);
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
	g->ssg_open = -1;
	g->dac_selected = DAC_DEFAULT_NOTE;
	g->dac_drag = DACC_NONE;
	g->wave_note = -1;
	g->grid_from = -1;
	g->rom_copy_note = -1;
	g->bank_seen_channel = g->drop_channel = g->drop_place = g->flash_channel = g->flash_place = -1;
	mdmi_engine_watch_frames(g->source.engine, true);
	return g;
}

void gui_destroy(mdmi_gui *g)
{
	if (!g) {
		return;
	}
	mdmi_engine_watch_frames(g->source.engine, false);
	dac_free(g->rom_copy);
	g->rom_copy = NULL;
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
