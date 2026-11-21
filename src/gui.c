#define COBJMACROS
#include <ctype.h>
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
#include "names.h"
#include "tv.h"

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
// Stage 24d (D62): channel 3 in Ch3 Special Mode, everything of it that is FM's elsewhere.
#define C_SP 0xEE44AA
#define C_SP_DARK 0x440022

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
// STORE (stage 20) on EDITOR where ADD stands on BANK, their right edges together.
#define ACTION_STORE_X 468
#define ACTION_STORE_W 64
// EXPORT (stage 24) on ROM, where ADD and STORE stand on the other pages.
#define ACTION_EXPORT_X 452
#define ACTION_EXPORT_W 80
#define NAME_X 288
#define NAME_CHARS 23
#define ENVELOPE_NAME_CHARS 20
// The page's grid (stage 9, D29): four columns of 112 px. The channel's cells are one row, the same
// on FM, PSG and DAC (ui-variants-v8-fm); what belongs to the chip starts under them.
#define GRID_W 112
#define GRID_ROW1_Y 56
#define SOURCE_X 590
#define SOURCE_W 112
static const int GRID_X[4] = {242, 358, 474, 590};
#define RAIL_W 232
#define RAIL_FM_Y 44
#define RAIL_PSG_Y 300
#define GROUP_H 16
#define ROW_H 40
#define PAGE_X 242
// Stage 37 (art-director, ui-concept.md): every page ends at 702, where grid column 4 does.
#define PAGE_RIGHT 702
// Stage 38 (D83, art-director): MONO before PORTA in the channel's row; its overruled label.
#define MONO_X 566
#define MONO_W 30
#define C_MONO_OVERRULED 0x8888EE
#define GLOBALS_Y 460
#define MASTER_X 608
#define MASTER_W 104
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
// The bank page (stage 22c, D56, art-director's frame in the ROADMAP): two lists side by side, A the
// ROM bank or a library, B a library, a bar over each that chooses what it shows; one row per place,
// scrolled. The DAC's list keeps BANK_X to BANK_RIGHT.
#define BANK_X 242
#define BANK_BAR_Y 56
#define BANK_BAR_H 20
#define BANK_LABEL_Y 80
#define BANK_Y 92
#define BANK_ROW_H 20
#define BANK_ROWS 18
#define BANK_BOTTOM (BANK_Y + BANK_ROWS * BANK_ROW_H)
#define BANK_RIGHT 692
#define LIST_A_RIGHT 504
#define LIST_A_SCROLL 506
#define LIST_A_BAR_RIGHT 512
#define LIST_DIVIDER_X 515
#define LIST_B_X 520
#define LIST_B_BAR_RIGHT 702
#define LIST_EDGE 10
#define SCROLL_X 696
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
#define DAC_RESET_X 630
#define DAC_RESET_W 60
#define DAC_FREE_X 590
#define DAC_FREE_W 112
#define DAC_WAVE_COLUMNS 460
#define MESSAGE_TICKS (4 * TIMER_PER_SECOND)
#define ERROR_TICKS (10 * TIMER_PER_SECOND)
#define FLASH_TICKS 9

// Saved with the project, so the numbers stay.
// The envelope editor (stage 8, art-director's note; 4A in stage 9): one column per frame sharing the
// lane by the length (0.8.1), the loop's strip on top, a level lane, a pitch lane of 15 rows by value,
// the time axis with the length cell at its end. Every height follows from the strip's.
#define ENV_X 296
#define ENV_LANE_W 406
#define ENV_LOOP_Y 106
#define ENV_LOOP_H 14
// Stage 20 gives 90 px to the arp row under it: a level 7 px, a pitch row 4 (arpeggio.md, 4).
#define ENV_LEVEL_Y (ENV_LOOP_Y + 16)
#define ENV_LEVEL_ROW 7
#define ENV_BASE_Y (ENV_LEVEL_Y + 15 * ENV_LEVEL_ROW)
#define ENV_LEVEL_BOTTOM (ENV_LEVEL_Y + 16 * ENV_LEVEL_ROW)
#define ENV_PITCH_Y (ENV_LEVEL_BOTTOM + 6)
#define ENV_PITCH_ROW 4
#define ENV_PITCH_BOTTOM (ENV_PITCH_Y + 15 * ENV_PITCH_ROW)
#define ENV_CENTRE_Y (ENV_PITCH_Y + 7 * ENV_PITCH_ROW + 2)
#define ENV_AXIS_Y (ENV_PITCH_BOTTOM + 4)
#define ENV_LENGTH_X 650
#define ENV_LENGTH_Y (ENV_AXIS_Y + 3)
#define ENV_LENGTH_W 52
// The width of the switch drawn while there is no loop.
#define ENV_LOOP_OFF_W 36
// Pixels of a vertical drag on the length per frame.
#define ENV_LENGTH_PIXELS 2
// ENV_TAIL: the loop strip after the loop's end tab, where a click chooses release or stop (stage 22b).
enum { ENV_NONE, ENV_LEVEL, ENV_PITCH, ENV_LOOP_START, ENV_LOOP_END, ENV_LOOP, ENV_LENGTH, ENV_TAIL };

// The arp row (stage 20, docs/design/arpeggio.md): one widget the graph's size on both pages, in the
// graph's place on FM and under the envelope on PSG. Its lane holds -24..+24 at 2 px a semitone, the
// loop's strip over it, a column of the length and labels at its right.
#define ARP_W arp_w(g)
#define ARP_H GRAPH_H
#define ARP_PSG_Y 332
#define ARP_LANE_DX 22
// The lane and the right column follow the row's width (stage 24d): on FM 200 and 228 as before.
#define ARP_LANE_W (arp_w(g) - 82)
#define ARP_LANE_DY 15
#define ARP_LANE_H 98
#define ARP_STRIP_DY 2
#define ARP_STRIP_H 12
#define ARP_RIGHT_DX (arp_w(g) - 54)
#define ARP_LENGTH_DY 16
#define ARP_LENGTH_W 52
// Pixels of a vertical drag on the length per step: 128 px for all 32.
#define ARP_LENGTH_PIXELS 4
// The ARP switch of the FM page, right of AMS and FMS: the graph or the row.
#define ARP_SWITCH_X 650
#define ARP_SWITCH_W 52
// The column of the chip's lines right of the row on PSG.
#define PSG_NOTE_X 534
// What the mouse holds on the row. ARP_ZERO is a double click's 0, which waits for the mouse to go up.
// ARP_ON_OFF is the row's own switch at the right (stage 22b, D55), ARP_SWITCH the loop's.
enum { ARP_NONE, ARP_LANE, ARP_TAB, ARP_BODY, ARP_SHADOW, ARP_SWITCH, ARP_LENGTH, ARP_ZERO, ARP_ON_OFF };

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
// Stage 24e: the channel's live output (D60) where the source began; the source moved right.
#define ALL_OUT_X 532
#define ALL_OUT_W 80
#define ALL_OUT_H 31
#define ALL_SOURCE_X 616
enum { ACTION_LOAD, ACTION_SAVE, ACTION_UNDO, ACTION_ADD, ACTION_STORE, ACTION_EXPORT, ACTIONS };
static const int ACTION_X[ACTIONS] = {ACTION_LOAD_X, ACTION_SAVE_X, ACTION_UNDO_X, ACTION_ADD_X, ACTION_STORE_X, ACTION_EXPORT_X};
static const int ACTION_WIDTH[ACTIONS] = {ACTION_W, ACTION_W, ACTION_W, ACTION_W, ACTION_STORE_W, ACTION_EXPORT_W};

typedef struct {
	IDropTarget iface;
	struct mdmi_gui *gui;
} drop_target;

// What a list of BANK shows (stage 22c): the ROM bank, a library read from its file, a file that
// isn't a bank, or no library because the folder or its files aren't there (list B only).
enum { LIST_ROM, LIST_LIBRARY, LIST_UNREAD, LIST_NO_FOLDER, LIST_NO_LIBRARY };
typedef struct {
	int kind;
	char path[LIBRARY_PATH];
	// The file's name in the folder, and without .mdbank.
	char file[LIBRARY_NAME];
	char name[LIBRARY_NAME];
	// A library as read, the window's copy.
	patch_bank bank;
	// The rows: every place of the ROM bank, a library's places that aren't empty, of the list the
	// selected channel chooses from.
	int places[PATCH_BANK_SIZE];
	int count;
	// The first row shown of a library; the ROM bank's is bank_top.
	int top;
} bank_list;
// A row cell is dragged, a column cell is a global knob, a choice is one of a row of buttons (the
// algorithm, pan L C R), the source is the Preset drawn as where the sound comes from.
// SSG is the button that opens the choice of shapes over the graph (stage 11), PITCH the tag of a
// channel 3 operator's pitch that opens its editor (stage 21).
enum { CELL_ROW, CELL_COLUMN, CELL_CHOICE, CELL_SOURCE, CELL_SSG, CELL_PITCH };
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
	// Stage 24d (D62): while channel 3 in Ch3 Special is drawn, FM's colours are its pink.
	bool sp;
	// Stage 36 (D81): the TV look's lines kept from the last frame, made on first use.
	tv_cache *tv;

	cell cells[MAX_CELLS];
	int cell_count;
	int mouse_x, mouse_y;
	bool tracking;
	// The cell being dragged, -1 when none.
	uint32_t drag_id;
	bool dragging;
	// Shift held at the last press (stage 48: UNDO with Shift is redo).
	bool shift;
	// The last choice a press set (stage 42): a double click resets it only over that same button.
	bool chosen;
	uint32_t chosen_id;
	int chosen_value;
	// Stage 43 (channel-copy.md): a press on a rail row, past DRAG_SLOP a drag that copies its sound.
	int copy_from;
	int copy_x, copy_y;
	bool copy_moved;
	bool drag_moved;
	bool closing; // WM_DESTROY lets the press go: no click, no pair rule (review 49)
	int drag_y;
	// Parts of a wheel notch not yet spent on a cell (stage 22a), and the value the wheel last sent
	// while the model hasn't taken it yet: edits reach the model through the writer's queue, so
	// quick notches step from what was sent, not from what the model still shows.
	int wheel_rest;
	uint32_t wheel_rest_id;
	bool wheel_pending;
	uint32_t wheel_id;
	int wheel_value;
	int drag_start;
	int drag_value;

	uint32_t underruns_second_ago;
	uint32_t underruns_per_second;
	int ticks;

	// The first place the bank page shows, and what the page last showed it for.
	int bank_top;
	int bank_seen_channel;
	bool bank_was_open;
	// Stage 22c (D56): the library folder, its files, and what lists A and B show, read again when the
	// page opens, a bank is chosen, and after every action on one. own_banks when the host gives none.
	char lib_folder[LIBRARY_PATH];
	// Stage 24: the names last handed to the engine for the ROM's screen; EXPORT's dialog is open.
	rom_names screen_names;
	bool screen_names_sent;
	bool exporting;
	library_list lib_list;
	bank_list lists[2];
	gui_banks own_banks;
	// Names whose file couldn't be read when opened, which the choice shows in red.
	char lib_bad[4][LIBRARY_NAME];
	int lib_bad_next;
	// The open choice of a bank: for which list, -1 closed; its first row shown.
	int chooser;
	int chooser_top;
	// A press on a row of a list (-1: none): the place, where it was, and whether the mouse went past
	// DRAG_SLOP, which makes it a drag that copies.
	int row_list;
	int row_place;
	int row_x, row_y;
	bool row_moved;
	// The library place last tried on a channel, lit while the channel plays it.
	int tried_channel;
	int tried_place;
	char tried_path[LIBRARY_PATH];
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
	// The last click on the loop's end switched release and stop (stage 22b): a double click puts
	// that back and drops the release, in the click's undo step.
	bool dac_tail_clicked;
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
	// Stage 49: files offered past the 128 taken, counted in the message as extra.
	int file_beyond;
	int file_instruments;
	int file_envelopes;
	// WAV among the files, and the notes they would go to on the DAC page.
	int file_samples;
	int dac_drop_note;
	int dac_drop_count;
	// The drop aims at the open note's wave rather than a cell (stage 18).
	bool dac_drop_wave;
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
	// Stage 50: the loop shadow and kept release before the gesture, which its undo step puts back.
	state_loop env_loop_before;
	state_tail env_tail_before;
	bool env_changed;
	bool env_handed;
	bool env_told;
	int env_last_step;
	int env_last_value;
	// A drag on the length starts from these, so shortening and lengthening back keeps the frames.
	patch_shape env_start;
	int env_start_x;
	int env_start_y;
	// The press on the loop's strip moved past DRAG_SLOP: the span moves, it is no click (stage 18b).
	bool env_moved;
	// Note Off stops the loop and its release isn't played, the copy's choice while the gesture
	// lasts (stage 22b, D54): env_shape holds the release after the loop all the same.
	bool env_stop;

	// The arp row being drawn (stage 20), the same way: what is held, the window's copy, the row before
	// the gesture and at its start, the last step drawn. arp_handed stays after the gesture, so a
	// double click's 0 joins the first click's undo step. arp_view: the FM page shows the row instead
	// of the graph, one for the window, not in the project.
	int arp_drag;
	int arp_channel;
	patch_arp arp_row;
	patch_arp arp_before;
	patch_arp arp_start;
	bool arp_changed;
	bool arp_handed;
	bool arp_told;
	bool arp_moved;
	int arp_last_step;
	int arp_last_value;
	int arp_start_x;
	int arp_start_y;
	bool arp_view;

	// The FM page (stage 11): the operator last edited by the mouse, the operator whose choice of SSG
	// is open (-1 when closed), and whether ALL draws the FM rows' envelopes instead of diagrams. The
	// window's alone: no knob, no project.
	int last_op;
	int ssg_open;
	// Stage 49: the channel the choice of SSG was opened on; another closes it, however it came.
	int ssg_channel;
	// The operator of channel 3 whose pitch editor is open (stage 21, docs/design/ch3-ops.md), -1
	// when closed; at most one of it and the choice of SSG is open.
	int pitch_open;
	// The last press only closed the choice of SSG or the pitch editor: a double click's second press does nothing.
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
	// The list of BANK a flash is in (stage 22c), -1 for the ROM bank's rows as before; a place of -1
	// there is the whole list.
	int flash_list;
};

static LONG window_count;

static void fill(mdmi_gui *g, int x, int y, int w, int h, uint32_t color)
{
	if (g->sp) {
		color = color == C_FM_DARK ? C_SP_DARK : color == C_FM || color == C_FM_LIGHT ? C_SP : color;
	}
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

// UTF-8 names (Бас.tfi) draw transliterated, never as gaps (stage 24): the font is ASCII only.
static int text(mdmi_gui *g, int x, int y, const char *utf8, uint32_t color, int scale)
{
	char string[256];
	names_ascii(utf8, string, sizeof(string));
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

// Text on a fill of the channel's colour: black on channel 3's pink (D62), white on FM.
static uint32_t on_fill(const mdmi_gui *g)
{
	return g->sp ? C_BG : C_TEXT;
}

// Text ending at the right edge, cut to the room it has.
static void text_right(mdmi_gui *g, int right, int y, const char *string, uint32_t color, int scale, int room)
{
	char cut[64];
	names_ascii(string, cut, sizeof(cut));
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

void gui_note_name(int key, char *out, size_t size)
{
	static const char *const names[12] = {"C-", "C#", "D-", "D#", "E-", "F-", "F#", "G-", "G#", "A-", "A#", "B-"};
	// Always 3 characters (stage 30, rom-screen.md 12): octave -1 has '-' for its digit, C--.
	if (key < 12) {
		snprintf(out, size, "%s-", names[key % 12]);
	} else {
		snprintf(out, size, "%s%d", names[key % 12], key / 12 - 1);
	}
}

static void note_name(int key, char *out, size_t size)
{
	gui_note_name(key, out, size);
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
// Stage 24c (art-director): the column of notes where one channel sounds several without borrowing,
// Ch3 OPS's operators and the DAC's voices, in the note's place at the rail's right: 4 lines of 9 px,
// 4 characters wide (a name is 3 since stage 30). The name gives up two letters to keep clear of it.
#define NOTE_RIGHT 226
#define NOTE_ROOM 24
#define SPECIAL_NOTE_Y 2
#define SPECIAL_NOTE_PITCH 9
#define SPECIAL_NAME_CHARS 11
// The bar has the same 16 segments of 9 px on every row, whatever is written right of it or under it.
#define BAR_PITCH 9
static void level_bar(mdmi_gui *g, int y, int volume, int velocity, bool seen, bool held, uint32_t dark)
{
	// A note that came lights one segment at least.
	int lit = (volume * velocity * BAR_SEGMENTS + 127 * 127 / 2) / (127 * 127);
	lit = !seen ? 0 : lit < 1 ? 1 : lit;
	const int shown = (volume * BAR_SEGMENTS + 63) / 127;
	for (int k = 0; k < BAR_SEGMENTS; k++) {
		const uint32_t color = k < lit ? (held ? C_PLAYING : C_HINT) : k < shown ? dark : C_CELL;
		fill(g, BAR_X + k * BAR_PITCH, y, BAR_PITCH - 1, 4, color);
	}
}

// The line under the note (stage 24c): what goes with it, right to left, 6 px apart.
static void row_tag(mdmi_gui *g, int *right, int y, const char *tag, uint32_t color)
{
	*right -= (int)strlen(tag) * 6;
	text(g, *right, y, tag, color, 1);
	*right -= 6;
}

// One line of a row's column: a held note green, one let go that still sounds grey, none "---".
static void column_note(mdmi_gui *g, int y, int k, bool seen, int key, bool held)
{
	char buffer[8];
	if (seen) {
		note_name(key, buffer, sizeof(buffer));
	} else {
		snprintf(buffer, sizeof(buffer), "---");
	}
	text_right(g, NOTE_RIGHT, y + SPECIAL_NOTE_Y + k * SPECIAL_NOTE_PITCH, buffer, !seen ? C_LINE : held ? C_PLAYING : C_HINT, 1, NOTE_ROOM);
}

// One rail row (0A): the number x3 beside the name and the bar, the name x2 above with 3 px to the
// line below it, the bar and the note x1 on that line. Heights and row_y stay: drops, hover and brackets use them.
// Stage 43 (channel-copy.md): a rail row is being dragged past DRAG_SLOP.
static bool copy_dragged(const mdmi_gui *g)
{
	return g->copy_from >= 0 && g->copy_moved;
}

static int row_at(const mdmi_gui *g, int x, int y);

// A channel the dragged row's sound can go to: another of the same kind, not the DAC.
static bool copy_takes(mdmi_gui *g, int channel)
{
	const patch_values *values = patch_view(g->source.patch);
	return g->copy_from >= 0 && channel >= 0 && channel != g->copy_from && !is_dac(values, channel)
		&& channel_is_fm(channel) == channel_is_fm(g->copy_from);
}

// The row under the mouse the sound would go to, or -1.
static int copy_target(mdmi_gui *g)
{
	const int row = row_at(g, g->mouse_x, g->mouse_y);
	return copy_takes(g, row) ? row : -1;
}

static void draw_row(mdmi_gui *g, const patch_values *values, int channel)
{
	const int y = row_y(channel);
	const bool dac = is_dac(values, channel);
	const bool special = is_special(values, channel);
	const bool selected = *g->source.channel == channel;
	const uint32_t kind = dac ? C_DAC : special ? C_FM_LIGHT : channel_is_fm(channel) ? C_FM : C_PSG;
	const uint32_t dark = dac ? C_DAC_DARK : channel_is_fm(channel) ? C_FM_DARK : C_PSG_DARK;
	// While a file is over the window: the row it would go to is lit, rows it can't go to are dim. A rail
	// row dragged (stage 43) looks the same: rows of its kind framed, the one under the mouse lit.
	const bool copying = copy_dragged(g);
	const bool target = (g->file_over && g->drop_channel == channel) || (copying && copy_target(g) == channel);
	const bool dim = (g->file_over && !target && (g->file_count != 1 || !takes_kind(values, channel, g->file_kind)))
		|| (copying && channel != g->copy_from && !copy_takes(g, channel));
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
		snprintf(buffer, sizeof(buffer), "%.*s%s", special && patch_ch3_ops(values) ? SPECIAL_NAME_CHARS : 13, sound_name(values, channel), sound_changed(g, values, channel) ? "*" : "");
		text(g, BAR_X, y + 5, buffer, INK(taken ? C_HINT : C_TEXT), 2);
	}
#undef INK
	if ((g->file_over || (copying && channel != g->copy_from)) && !target && !dim) {
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
	// Stage 24c: the note at y+22 right of the bar, what goes with it a line below at y+31.
	const int line = y + 22, tags = y + 31, bar_y = y + 25;
	int right = NOTE_RIGHT;
	const uint32_t note = __atomic_load_n(&g->source.notes[channel], __ATOMIC_ACQUIRE);
	const bool seen = GUI_NOTE_SEEN(note);
	const int held = seen ? (int)GUI_NOTE_HELD(note) : 0;
	if (taken) {
		// No bar: the channel makes no sound. Its held notes are cut, as notes without a voice.
		text_right(g, NOTE_RIGHT, line, "TAKEN BY CH10", C_LABEL, 1, 13 * 6);
		if (held > 0) {
			snprintf(buffer, sizeof(buffer), "%d CUT", held);
			row_tag(g, &right, tags, buffer, C_ERROR);
		}
		return;
	}
	// Only with Ops (stage 21): in One Note MIDI 3 plays them all, a note as on any row.
	if (special && patch_ch3_ops(values)) {
		// The lead (operator 4, channel 3 itself) on top, operators 1-3 (MIDI 11-13) below it.
		static const int SOURCES[4] = {2, 10, 11, 12};
		for (int k = 0; k < 4; k++) {
			const uint32_t op = __atomic_load_n(&g->source.notes[SOURCES[k]], __ATOMIC_ACQUIRE);
			column_note(g, y, k, GUI_NOTE_SEEN(op), (int)GUI_NOTE_KEY(op), GUI_NOTE_SEEN(op) && GUI_NOTE_HELD(op));
		}
		level_bar(g, bar_y, values->channels[channel][PARAM_VOLUME], (int)GUI_NOTE_VELOCITY(note), seen, held > 0, volume_ink);
		return;
	}
	if (dac) {
		// PCM4's four voices, line k voice k, as the ROM deals them (D59). The DAC hears neither volume
		// nor velocity here: the block only says a sample sounds, green while its key is held.
		bool any_held = false, any_out = false;
		for (int v = 0; v < PATCH_DAC_VOICES; v++) {
			int key = 0;
			const int state = patch_dac_voice_state(g->source.patch, v, &key);
			column_note(g, y, v, state != PATCH_DAC_FREE, key, state == PATCH_DAC_HELD);
			any_held = any_held || state == PATCH_DAC_HELD;
			any_out = any_out || state == PATCH_DAC_OUT;
		}
		fill(g, BAR_X, bar_y, 16, 4, any_held ? C_PLAYING : any_out ? C_HINT : C_CELL);
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
		text_right(g, NOTE_RIGHT, line, buffer, C_PLAYING, 1, NOTE_ROOM);
		snprintf(buffer, sizeof(buffer), "CH%d", player + 1);
		row_tag(g, &right, tags, buffer, C_LABEL);
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
		text_right(g, NOTE_RIGHT, line, buffer, !seen ? C_LINE : held ? C_PLAYING : C_HINT, 1, NOTE_ROOM);
		if (own) {
			velocity = patch_voice_velocity(g->source.patch, channel);
		}
		if (held > 1) {
			snprintf(buffer, sizeof(buffer), "x%d", held);
			row_tag(g, &right, tags, buffer, C_PLAYING);
		}
		// Held notes without a voice: cut off by a tune, or more than the free voices (D20).
		const int muted = seen ? patch_muted_notes(g->source.patch, channel) : 0;
		if (muted > 0) {
			snprintf(buffer, sizeof(buffer), "%d CUT", muted);
			row_tag(g, &right, tags, buffer, C_ERROR);
		}
	}
	level_bar(g, bar_y, volume, velocity, bar_seen, bar_held, volume_ink);
}

// Brackets from a channel playing a chord to the rows whose voices it borrowed, between the big
// numbers and the names. Green like a held note: those voices sound. The bracket spanning fewer
// rows sits nearest the names, so nested ones don't cross.
#define LEND_LANE_X 44
#define LEND_LANE_STEP 7
#define LEND_TIP_X 48
// Stage 30 (ui-pass-2.md, its stage 30 section): a head on each branch; 0 is the variant without, for the owner's pick.
#define LEND_HEADS 1
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
		// Stage 24d (art-director): at the names' height, clear of the bars, so the head reads. Stage
		// 30: lines of 1 px, so the owner's square leads.
		fill(g, x, row_y(top[c]) + 12, 1, row_y(bottom[c]) - row_y(top[c]) + 1, C_PLAYING);
		for (int row = top[c]; row <= bottom[c]; row++) {
			if (row != c && player[row] != c) {
				continue;
			}
			const int y = row_y(row);
			if (row == c) {
				// Where the chord comes from: a square 9 x 9 on the spine, no arrow.
				fill(g, x - 4, y + 8, 9, 9, C_PLAYING);
				continue;
			}
			// Where it goes: a branch of 1 px with a solid head 4 x 7, its tip at x 51.
#if LEND_HEADS
			fill(g, x, y + 12, LEND_TIP_X - x, 1, C_PLAYING);
			for (int i = 0; i < 4; i++) {
				fill(g, LEND_TIP_X + i, y + 9 + i, 1, 7 - 2 * i, C_PLAYING);
			}
#else
			fill(g, x, y + 12, LEND_TIP_X + 4 - x, 1, C_PLAYING);
#endif
		}
	}
}

// Stage 44 (D88): a PSG channel's envelope speed, a global.
static bool is_env_speed(uint32_t id)
{
	return id >= GLOBAL_ENV_SPEED_BASE && id < GLOBAL_ENV_SPEED_BASE + PATCH_NUM_PSG_CHANNELS;
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

// Channel 3's AR in CSM: the ROM plays 31 whatever the voice holds (stage 22b, D57), so the cell is dim.
static bool csm_ar(const patch_values *values, uint32_t id)
{
	int channel, param;
	return patch_id_split(id, &channel, &param) && channel == 2 && param >= PARAM_OP_BASE
		&& (param - PARAM_OP_BASE) % PATCH_OP_PARAMS == OP_AR && patch_ch3_csm(values);
}

// Channel 3's operators in Ch3 Special (stage 21, docs/design/ch3-ops.md): a tag of each one's pitch
// in its header, and under the header the editor of SEMI, FINE and FIX, where the choice of SSG opens.
#define PITCH_TAG_X 26
#define PITCH_TAG_W 48
#define PITCH_EDIT_Y 227
#define PITCH_EDIT_H 72
#define PITCH_SEMI_Y 228
#define PITCH_FINE_Y 252
#define PITCH_FIX_Y 276
#define PITCH_FIX_W 50
// The strip's CH3 NOTES: its label on top, the segments under it.
#define CH3_NOTES_X 374
#define CH3_NOTES_W 114
#define CH3_NOTES_LABEL_H 14
#define CH3_NOTES_SEGMENT_Y (GLOBALS_Y + 20)
#define CH3_NOTES_SEGMENT_H 18
#define LFO_RESET_X 158
#define LFO_RESET_W 64
static const struct {
	const char *label;
	int x, w;
} CH3_NOTES_SEGMENTS[3] = {{"OPS", 376, 34}, {"1 NOTE", 412, 38}, {"CSM", 452, 34}};
#define CH3_NOTES_MODES 3

static bool is_ch3_op_global(uint32_t id)
{
	return id >= GLOBAL_CH3_OP_BASE && id < GLOBAL_CH3_OP_END;
}

// Stage 38 (D83): the FM channel whose MONO knob id is, or -1.
static int mono_channel_of(uint32_t id)
{
	return id >= GLOBAL_MONO_BASE && id < GLOBAL_MONO_BASE + PATCH_NUM_FM_CHANNELS ? (int)(id - GLOBAL_MONO_BASE) : -1;
}

// The channel of a PORTA ON knob of an FM channel, or -1.
static int porta_channel_of(uint32_t id)
{
	int channel, param;
	return patch_id_split(id, &channel, &param) && param == PARAM_PORTA_ON && channel >= 0 && channel < PATCH_NUM_FM_CHANNELS ? channel : -1;
}

static int ch3_field(int op, int field)
{
	return GLOBAL_CH3_OP(op, field);
}

// The mouse is in the open pitch editor, gaps between its cells included.
static bool in_pitch_editor(const mdmi_gui *g, int x, int y)
{
	return g->pitch_open >= 0 && x >= GRID_X[g->pitch_open] - 1 && x < GRID_X[g->pitch_open] + OP_W + 1 && y >= PITCH_EDIT_Y && y < PITCH_EDIT_Y + PITCH_EDIT_H;
}

// The note an operator plays with Fixed: C4 and its Semi, kept where the ROM keeps every operator.
static int ch3_fixed_key(const patch_values *values, int op)
{
	const int key = PATCH_CH3_FIXED_KEY + values->globals[ch3_field(op, PATCH_CH3_SEMI)] - PATCH_CH3_SEMI_MAX;
	return key < PATCH_CH3_LOWEST_KEY ? PATCH_CH3_LOWEST_KEY : key > PATCH_CH3_HIGHEST_KEY ? PATCH_CH3_HIGHEST_KEY : key;
}

// The tag's text: "+7", "+7 +10", or with Fixed the note, "G-4 +10".
static void ch3_pitch_text(const patch_values *values, int op, char *out, size_t size)
{
	const int semi = values->globals[ch3_field(op, PATCH_CH3_SEMI)] - PATCH_CH3_SEMI_MAX;
	const int fine = values->globals[ch3_field(op, PATCH_CH3_FINE)] - PATCH_CH3_FINE_MAX;
	char first[8];
	if (values->globals[ch3_field(op, PATCH_CH3_FIXED)]) {
		note_name(ch3_fixed_key(values, op), first, sizeof(first));
	} else {
		snprintf(first, sizeof(first), "%+d", semi);
	}
	if (fine) {
		snprintf(out, size, "%s %+d", first, fine);
	} else {
		snprintf(out, size, "%s", first);
	}
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
	if (g->pitch_open >= 0) {
		return g->pitch_open;
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
		// Stage 38 (art-director, ui-concept.md): TUNE and BEND give MONO its place before PORTA.
		{PARAM_VOLUME, "VOL", 242, 112}, {PARAM_FINE_TUNE, "TUNE", 422, 74}, {PARAM_BEND_RANGE, "BEND", 500, 62},
		{PARAM_PORTA_ON, "PORTA", 598, 36}, {PARAM_PORTA_TIME, "TIME", 638, 64},
	};
	static const char *const PANS[3] = {"L", "C", "R"};
	static const char *const DIGITS[8] = {"0", "1", "2", "3", "4", "5", "6", "7"};
	// Each as wide as what it shows (stage 21): CH3 NOTES, its segments, comes after CH3 SPECIAL.
	static const struct {
		int global;
		const char *label;
		int x, w;
	} GLOBAL_CELLS[] = {
		{GLOBAL_LFO_ON, "LFO", 8, 48}, {GLOBAL_LFO_RATE, "LFO RATE", 60, 94}, {GLOBAL_LFO_RESET, "LFO RESET", LFO_RESET_X, LFO_RESET_W},
		{GLOBAL_FILTER, "FILTER", 226, 68}, {GLOBAL_CH3_MODE, "CH3 SPECIAL", 298, 72}, {GLOBAL_DAC, "DAC", 492, 48},
		{GLOBAL_RETRIGGER, "RETRIGGER", 544, 60},
	};
	g->cell_count = 0;
	const int channel = *g->source.channel;
	const bool bank = *g->source.page == PAGE_BANK;
	// The pitch editor belongs to channel 3's FM page in Ch3 Special, and closes with it.
	if (channel != 2 || !is_special(values, channel) || *g->source.page != PAGE_EDITOR) {
		g->pitch_open = -1;
	}
	if (g->ssg_open >= 0 && channel != g->ssg_channel) {
		g->ssg_open = -1;
	}
	// Ahead of the column's rows it lies over, as the choice of SSG.
	if (g->pitch_open >= 0) {
		const int x = GRID_X[g->pitch_open];
		add_cell(g, x, PITCH_SEMI_Y, OP_W, OP_ROW_H, (uint32_t)ch3_field(g->pitch_open, PATCH_CH3_SEMI), "SEMI", CELL_ROW);
		add_cell(g, x, PITCH_FINE_Y, OP_W, OP_ROW_H, (uint32_t)ch3_field(g->pitch_open, PATCH_CH3_FINE), "FINE", CELL_ROW);
		add_cell(g, x, PITCH_FIX_Y, PITCH_FIX_W, OP_ROW_H, (uint32_t)ch3_field(g->pitch_open, PATCH_CH3_FIXED), "FIX", CELL_ROW);
		g->cells[g->cell_count - 1].look = LOOK_SWITCH;
	}
	if (*g->source.page == PAGE_ALL) {
		// Per row: VOL, PAN L C R on FM, the source dragged like the page's.
		for (int c = 0; c < PATCH_NUM_CHANNELS; c++) {
			const int y = row_y(c);
			// The DAC's row has VOL (D61, not MDMI canon) and PAN too, no source (stage 24e).
			add_cell(g, ALL_VOL_X, y + 4, GRID_W, CELL_H, PATCH_CHANNEL_ID(c, PARAM_VOLUME), "", CELL_ROW);
			if (patch_channel_has(c, PARAM_PAN)) {
				for (int pan = 0; pan < 3; pan++) {
					add_cell(g, ALL_PAN_X + pan * (ALL_PAN_W + 1), y + 4, ALL_PAN_W, CELL_H, PATCH_CHANNEL_ID(c, PARAM_PAN), PANS[pan], CELL_CHOICE);
					g->cells[g->cell_count - 1].choice = pan;
				}
			}
			if (!is_dac(values, c)) {
				add_cell(g, ALL_SOURCE_X, y + 4, PAGE_RIGHT - ALL_SOURCE_X, 22, PATCH_CHANNEL_ID(c, PARAM_PRESET), "PRESET", CELL_SOURCE);
			}
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
		// Stage 44 (D88, env-speed.md): the envelope's speed in PSG's PAN place, also on the noise.
		if (!channel_is_fm(channel)) {
			add_cell(g, GRID_X[1], GRID_ROW1_Y, 60, CELL_H, (uint32_t)GLOBAL_ENV_SPEED(channel - PATCH_NUM_FM_CHANNELS), "SPD", CELL_ROW);
		}
		// Stage 38 (D83): MONO, a global per FM channel, paired with PORTA (MONO_X, mono_pair_click).
		if (channel_is_fm(channel)) {
			add_cell(g, MONO_X, GRID_ROW1_Y, MONO_W, CELL_H, (uint32_t)GLOBAL_MONO(channel), "MONO", CELL_ROW);
			g->cells[g->cell_count - 1].look = LOOK_SWITCH;
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
			// AMS and FMS give the ARP switch its room (stage 20).
			add_cell(g, BLOCK_X, GRAPH_Y, 56, CELL_H, PATCH_CHANNEL_ID(channel, PARAM_AMS), "AMS", CELL_ROW);
			add_cell(g, BLOCK_X + 60, GRAPH_Y, 56, CELL_H, PATCH_CHANNEL_ID(channel, PARAM_FMS), "FMS", CELL_ROW);
			// FB sits in the diagram's corner over operator 1's loop: dragged, a click steps it.
			add_cell(g, feedback_x(values->channels[channel][PARAM_ALGORITHM]), DIAGRAM_Y + 2, 72, 13, PATCH_CHANNEL_ID(channel, PARAM_FEEDBACK), "FB", CELL_ROW);
			g->cells[g->cell_count - 1].look = LOOK_FEEDBACK;
			for (int algorithm = 0; algorithm < 8; algorithm++) {
				add_choice(g, BLOCK_X + algorithm * ALGORITHM_STEP, ALGORITHM_Y, ALGORITHM_W, ALGORITHM_H, PATCH_CHANNEL_ID(channel, PARAM_ALGORITHM), DIGITS[algorithm], algorithm, LOOK_BAR);
			}
			for (int op = 0; op < PATCH_OPS; op++) {
				const int x = GRID_X[op];
				add_cell(g, x + SSG_BUTTON_X, SSG_BUTTON_Y, SSG_BUTTON_W, SSG_BUTTON_H, op_id(channel, op, OP_SSG), "SSG", CELL_SSG);
				// Channel 3's pitch tag exists only in Ch3 Special (stage 22b).
				if (is_special(values, channel)) {
					add_cell(g, x + PITCH_TAG_X, SSG_BUTTON_Y, PITCH_TAG_W, SSG_BUTTON_H, (uint32_t)ch3_field(op, PATCH_CH3_SEMI), "PITCH", CELL_PITCH);
				}
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
		// LFO RESET does nothing with the LFO off: dim, drawn alone, no mouse (draw_globals_dim).
		if (GLOBAL_CELLS[i].global == GLOBAL_LFO_RESET && !values->globals[GLOBAL_LFO_ON]) {
			continue;
		}
		add_cell(g, GLOBAL_CELLS[i].x, GLOBALS_Y + 6, GLOBAL_CELLS[i].w, 32, (uint32_t)GLOBAL_CELLS[i].global, GLOBAL_CELLS[i].label, CELL_COLUMN);
	}
	// CH3 NOTES only means something in Ch3 Special; off it is dim the same way.
	if (values->globals[GLOBAL_CH3_MODE]) {
		for (int i = 0; i < CH3_NOTES_MODES; i++) {
			add_choice(g, CH3_NOTES_SEGMENTS[i].x, CH3_NOTES_SEGMENT_Y, CH3_NOTES_SEGMENTS[i].w, CH3_NOTES_SEGMENT_H, GLOBAL_CH3_NOTES, CH3_NOTES_SEGMENTS[i].label, i, LOOK_BAR);
		}
	}
	// The plugin's output level (stage 18a) at the strip's right end, under SAVE and UNDO on every page.
	add_cell(g, MASTER_X, GLOBALS_Y + 6, MASTER_W, 32, GLOBAL_MASTER, "MASTER", CELL_COLUMN);
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
				text(g, c->x + (c->w - 17) / 2, c->y + (c->h - 7) / 2, "OFF", on ? on_fill(g) : C_LABEL, 1);
			} else {
				ssg_icon(g, c->x + 6, c->y + 7, c->w - 12, 8, c->choice, on ? on_fill(g) : C_FM_LIGHT);
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
			// CH3 NOTES keeps all its segments small: 1 NOTE has no room for the big font (stage 21).
			const bool ch3_notes = c->id == GLOBAL_CH3_NOTES;
			const int scale = !psg && !ch3_notes && (int)strlen(c->label) * 12 - 2 <= c->w - 2 && c->h >= 16 ? 2 : 1;
			const int width = (int)strlen(c->label) * 6 * scale - scale;
			// CH3 NOTES, a global, is channel 3's in Ch3 Special: its chosen segment pink (D62).
			const bool sp = ch3_notes && is_special(values, 2);
			fill(g, c->x, c->y, c->w, c->h, lit ? (pan ? C_TEXT : psg ? C_PSG : sp ? C_SP : C_FM) : C_CELL);
			text(g, c->x + (c->w - width) / 2, c->y + (ch3_notes ? 6 : (c->h - 7 * scale) / 2), c->label, lit ? (pan || psg || sp ? C_BG : on_fill(g)) : dim ? (on ? C_HINT : C_LINE) : C_LABEL, scale);
		}
		if (changed && c->choice == base) {
			fill(g, c->x, c->y + c->h - 2, c->w, 2, C_AMBER);
		}
		return;
	}
	if (c->kind == CELL_PITCH) {
		// Drawn with its header, dim or not (draw_pitch_tag); the frame says the editor is open.
		if (g->pitch_open >= 0 && g->pitch_open == ((int)c->id - GLOBAL_CH3_OP_BASE) / PATCH_CH3_OP_FIELDS) {
			frame(g, c->x, c->y, c->w, c->h, C_TEXT);
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
		// Stage 38: MONO on while PORTA is on is overruled (PORTA glides): a frame in FM's colour.
		const int mono = mono_channel_of(c->id);
		if (mono >= 0 && value && values->channels[mono][PARAM_PORTA_ON]) {
			fill(g, c->x, c->y, c->w, c->h, C_BG);
			frame(g, c->x, c->y, c->w, c->h, C_FM);
			text(g, c->x + (c->w - width) / 2, c->y + (c->h - 7) / 2, c->label, C_MONO_OVERRULED, 1);
			return;
		}
		fill(g, c->x, c->y, c->w, c->h, value ? lit : C_BG);
		if (!value) {
			frame(g, c->x, c->y, c->w, c->h, held ? C_TEXT : C_LINE);
		}
		text(g, c->x + (c->w - width) / 2, c->y + (c->h - 7) / 2, c->label, changed ? C_AMBER : value ? on_fill(g) : C_LABEL, 1);
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
		// Where the sound comes from: place N of the ROM bank on FM and PSG alike (stage 24d), grey
		// while it holds the factory sound, white once the owner replaced it; a file dropped on the
		// channel says FILE over the place the next preset step goes back to.
		const patch_bank *bank = patch_bank_view(g->source.patch);
		const bool factory = channel_is_fm(channel) ? patch_place_is_gm(bank, value) : patch_envelope_is_rom(bank, value);
		snprintf(buffer, sizeof(buffer), "ROM %d", value + 1);
		const uint32_t ink = held || !factory ? C_TEXT : C_LABEL;
		const int right = c->x + c->w - 2;
		if (plays_own_file(g, values, channel)) {
			text_right(g, right, c->y + 2, "FILE", C_TEXT, 1, c->w);
			text_right(g, right, c->y + 12, buffer, ink, 1, c->w);
		} else {
			text_right(g, right, c->y + 4, buffer, ink, 2, c->w);
		}
		if (held) {
			frame(g, c->x, c->y, c->w, c->h, C_TEXT);
		}
		return;
	}
	// The cell's own channel: ALL shows them all.
	// Channel 3's operator pitch is FM's, though a global (stage 21).
	const bool ch3_op = is_ch3_op_global(c->id);
	const bool fm = (channel >= 0 && channel_is_fm(channel)) || ch3_op;
	patch_format(patch_bank_view(g->source.patch), c->id, value, buffer, sizeof(buffer));
	// Channel 3's AR in CSM: dim, still editable (stage 22b, D57).
	const bool forced = csm_ar(values, c->id);
	const uint32_t dark = held ? C_LINE : forced ? C_LINE : channel >= 0 && is_dac(values, channel) ? C_DAC_DARK : fm ? C_FM_DARK : C_PSG_DARK;
	const int max = patch_param(c->id)->max > 0 ? patch_param(c->id)->max : 1;
	if (c->id == GLOBAL_MASTER) {
		// Grey, as it belongs to no chip: a bar from the left, a tick at 0 dB as TUNE marks its middle.
		fill(g, c->x, c->y, c->w, c->h, C_CELL);
		fill(g, c->x, c->y, c->w * value / max, c->h, C_LINE);
		fill(g, c->x + c->w * PATCH_MASTER_UNITY / max, c->y, 1, c->h, C_LABEL);
	} else if (c->kind == CELL_COLUMN) {
		fill(g, c->x, c->y, c->w, c->h, held ? C_LINE : C_CELL);
	} else {
		// A channel's cell is a bar filled up to its value, as in the mockup; TUNE from its middle.
		fill(g, c->x, c->y, c->w, c->h, C_CELL);
		const int at = c->w * value / max;
		if ((channel >= 0 && param == PARAM_FINE_TUNE) || ch3_op || is_env_speed(c->id)) {
			const int middle = c->w * patch_param(c->id)->default_value / max;
			fill(g, c->x + (at < middle ? at : middle), c->y, at < middle ? middle - at : at - middle, c->h, dark);
			fill(g, c->x + middle, c->y, 1, c->h, C_LINE);
		} else {
			fill(g, c->x, c->y, at, c->h, dark);
		}
	}
	const uint32_t label = held ? C_TEXT : forced ? C_LINE : changed ? C_AMBER : C_LABEL;
	uint32_t shown = forced && !held ? C_HINT : C_TEXT;
	if (c->id == GLOBAL_DAC && value) {
		shown = C_DAC;
	}
	// On in channel 3's pink, as its row and number in the rail (stage 21, D62).
	if (c->id == GLOBAL_CH3_MODE && value) {
		shown = C_SP;
	}
	// No limiter: above 0 dB the plugin can clip, so amber; silence is dim.
	if (c->id == GLOBAL_MASTER) {
		shown = value > PATCH_MASTER_UNITY ? C_AMBER : value == 0 ? C_HINT : C_TEXT;
		if (value == 0) {
			snprintf(buffer, sizeof(buffer), "-INF");
		}
	}
	if (c->kind == CELL_COLUMN) {
		text(g, c->x + 4, c->y + 4, c->label, label, 1);
		// A value without room for its unit drops it, as in a row: 19926 for 19926 Hz.
		char *unit = strchr(buffer, ' ');
		if ((int)strlen(buffer) > (c->w - 8) / 12 && unit) {
			*unit = 0;
		}
		buffer[(c->w - 8) / 12] = 0;
		text(g, c->x + 4, c->y + 14, buffer, shown, 2);
	} else {
		// Stage 49 (ui-concept.md): TIME pads 2 px, not 4, so 100-127 fit at size 2 in its 64 px.
		const int pad = channel >= 0 && param == PARAM_PORTA_TIME ? 2 : 4;
		int label_end = text(g, c->x + pad, c->y + (c->h - 7) / 2, c->label, label, 1);
		// Stage 44 (env-speed.md): x1.00 fits its 60 px only small; the bar and the footer say the rest.
		if (is_env_speed(c->id)) {
			text_right(g, c->x + c->w - 3, c->y + (c->h - 7) / 2, buffer, shown, 1, c->x + c->w - 4 - label_end);
			if (held) {
				frame(g, c->x, c->y, c->w, c->h, C_TEXT);
			}
			return;
		}
		// A value without room for its unit drops it: +0 for +0 ct.
		// TIME's value ends its last glyph's blank at the cell's edge: drawn to 699, 3 px after the label.
		const int right = pad == 2 ? c->x + c->w : c->x + c->w - 4;
		const int room = right - label_end - (pad == 2 ? 2 : 6);
		char *space = strchr(buffer, ' ');
		if ((int)strlen(buffer) * 12 > room && space) {
			*space = 0;
		}
		text_right(g, right, c->y + (c->h - 14) / 2, buffer, shown, 2, room);
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
	int channel = -1, param = -1;
	patch_id_split(c->id, &channel, &param);
	g->sp = is_special(values, 2) && (channel == 2 || is_ch3_op_global(c->id));
	draw_cell_body(g, c, values);
	g->sp = false;
	// The pitch tag has no dot of its own: its editor's cells do (docs/design/ch3-ops.md).
	if (c->kind == CELL_PITCH || !g->source.host_values || !patch_id_split(c->id, &channel, &param) || !__atomic_load_n(&g->source.host_values[GUI_HOST_SLOT(channel, param)], __ATOMIC_RELAXED)) {
		return;
	}
	// A row of choices has its dot on its last button.
	if (c->look == LOOK_SHAPE || (c + 1 < g->cells + g->cell_count && c[1].id == c->id)) {
		return;
	}
	// Stage 49 (ui-concept.md): TIME's value reaches its right edge, so its dot goes top left.
	const bool time = channel >= 0 && param == PARAM_PORTA_TIME;
	fill(g, time ? c->x + 1 : c->x + c->w - 4, c->y + 1, 3, 3, C_ERROR);
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
			text(g, x + (OP_BOX - 10) / 2, y + (OP_BOX - 14) / 2, digit, on_fill(g), 2);
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

// Stage 49: steps of the open PSG channel's envelope in ms at its SPD (the ROM's own table, D88);
// at x1 the same as frame_ms.
static int env_ms(mdmi_gui *g, int steps)
{
	const int psg = *g->source.channel - PATCH_NUM_FM_CHANNELS;
	const long rate = psg >= 0 ? patch_env_rate(patch_get(g->source.patch, (uint32_t)GLOBAL_ENV_SPEED(psg))) : PATCH_ENV_RATE_ONE;
	return (int)(((long)steps * 1000 * PATCH_ENV_RATE_ONE + 30 * rate) / (60 * rate));
}

static int loop_end_of(const patch_shape *shape)
{
	return shape->loop_end < 0 ? shape->count : shape->loop_end;
}

// A loop's span [*start, *end) moved by delta with its length, held inside [low, high]: the one
// gesture both loop editors share (stage 18b), in their own units.
static void shift_span(long *start, long *end, long delta, long low, long high)
{
	const long length = *end - *start;
	long to = *start + delta;
	to = to > high - length ? high - length : to;
	to = to < low ? low : to;
	*start = to;
	*end = to + length;
}

// Columns share the whole lane by the length (0.8.1), their edges rounded down, so the lane ends at
// the page's right edge at every length and widths differ by a pixel at most.
static int lane_column_x(int left, int width, int count, int column)
{
	return left + column * width / count;
}

static int column_x(int count, int column)
{
	return lane_column_x(ENV_X, ENV_LANE_W, count, column);
}

// The gap pixel before a frame, where the loop lines stand.
static int boundary_x(int count, int boundary)
{
	return column_x(count, boundary) - 1;
}

// The boundary nearest to x, and the column under it (count past the lane's end, -1 before it).
static int lane_boundary_at(int left, int width, int count, int x)
{
	int best = 0;
	for (int b = 1; b <= count; b++) {
		if (abs(lane_column_x(left, width, count, b) - 1 - x) < abs(lane_column_x(left, width, count, best) - 1 - x)) {
			best = b;
		}
	}
	return best;
}

static int boundary_at(int count, int x)
{
	return lane_boundary_at(ENV_X, ENV_LANE_W, count, x);
}

static int lane_column_at(int left, int width, int count, int x)
{
	int column = -1;
	// No columns in an empty lane: the column edges divide by count.
	while (count > 0 && column < count && lane_column_x(left, width, count, column + 1) <= x) {
		column++;
	}
	return column;
}

static int column_at(int count, int x)
{
	return lane_column_at(ENV_X, ENV_LANE_W, count, x);
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

// A loop with steps after its end: a release that Note Off plays, or not with stop (stage 22b, D54).
static bool has_tail(const patch_shape *shape)
{
	return shape->loop_start >= 0 && shape->loop_end >= 0 && shape->loop_end < shape->count;
}

// The channel's envelope as the editor shows it: while Note Off stops its loop, the release the
// view keeps follows the loop's end (stage 22b), *stop then true. The envelope itself ends there.
static void channel_shape(mdmi_gui *g, int channel, patch_shape *shape, bool *stop)
{
	const patch_envelope *envelope = patch_envelope_view(g->source.patch, channel);
	patch_shape_of(envelope, shape);
	*stop = false;
	const int psg = channel - PATCH_NUM_FM_CHANNELS;
	if (psg < 0 || !g->source.tails) {
		return;
	}
	const state_tail *tail = &g->source.tails[psg];
	if (!tail->stop || strcmp(tail->name, envelope->name) != 0 || shape->loop_start < 0 || shape->loop_end >= 0) {
		return;
	}
	// The loop's end mark takes a step's room.
	const int end = shape->count, room = PATCH_ENVELOPE_STEPS - 2;
	const int count = tail->count < room - end ? tail->count : room - end;
	if (count <= 0) {
		return;
	}
	memcpy(shape->frames + end, tail->steps, (size_t)count);
	shape->loop_end = end;
	shape->count = end + count;
	*stop = true;
}

// What the editor shows: the window's copy while a gesture lasts, the channel's envelope otherwise.
static const patch_shape *shown_shape(mdmi_gui *g, patch_shape *scratch)
{
	const int channel = *g->source.channel;
	if (g->env_drag != ENV_NONE && g->env_channel == channel) {
		return &g->env_shape;
	}
	bool stop;
	channel_shape(g, channel, scratch, &stop);
	return scratch;
}

// Whether the release the editor shows isn't played (stage 22b).
static bool shown_stop(mdmi_gui *g, const patch_shape *shown)
{
	if (g->env_drag != ENV_NONE && g->env_channel == *g->source.channel) {
		return g->env_stop && has_tail(shown);
	}
	patch_shape scratch;
	bool stop;
	channel_shape(g, *g->source.channel, &scratch, &stop);
	return stop;
}

// What plays of a shape: with stop, it ends at the loop's end.
static void played_shape(const patch_shape *shape, bool stop, patch_shape *played)
{
	*played = *shape;
	if (stop && has_tail(shape)) {
		played->count = shape->loop_end;
		played->loop_end = -1;
	}
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
		if (x >= start - 9 && x <= start) {
			return ENV_LOOP_START;
		}
		if (x >= end && x <= end + 9) {
			return ENV_LOOP_END;
		}
		// After the end tab to the lane's end: the release's or stop's switch (stage 22b).
		return x > start && x < end ? ENV_LOOP : has_tail(shape) && x > end + 9 && x < ENV_X + ENV_LANE_W ? ENV_TAIL : ENV_NONE;
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
	const int psg = g->env_channel - PATCH_NUM_FM_CHANNELS;
	g->env_loop_before = g->source.loop_shadows[psg];
	if (g->source.tails) {
		g->env_tail_before = g->source.tails[psg];
	}
	channel_shape(g, g->env_channel, &g->env_shape, &g->env_stop);
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
	// With stop the envelope ends at the loop's end and the view keeps the release (stage 22b): the
	// ROM, an EEF, the bank and STORE get what plays. Anything else drops the stop.
	patch_shape played;
	const bool stop = g->env_stop && has_tail(&g->env_shape);
	played_shape(&g->env_shape, stop, &played);
	if (g->source.tails) {
		state_tail *tail = &g->source.tails[g->env_channel - PATCH_NUM_FM_CHANNELS];
		tail->stop = stop;
		tail->count = stop ? g->env_shape.count - g->env_shape.loop_end : 0;
		if (stop) {
			memcpy(tail->steps, g->env_shape.frames + g->env_shape.loop_end, (size_t)tail->count);
			snprintf(tail->name, sizeof(tail->name), "%s", g->env_before.name);
		}
	}
	patch_shape_to_envelope(&played, &envelope);
	files_draw_envelope(g->source.files, g->env_channel, g->env_handed ? NULL : &g->env_before, &envelope, &g->env_loop_before, g->source.tails ? &g->env_tail_before : NULL);
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
		const int value = g->env_drag == ENV_LEVEL ? clamp((y - ENV_LEVEL_Y) / ENV_LEVEL_ROW, 0, 15) : clamp((y - ENV_PITCH_Y) / ENV_PITCH_ROW, 0, 14);
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
	} else if (g->env_drag == ENV_LOOP_START || g->env_drag == ENV_LOOP_END) {
		// As the body: the press changes nothing, past DRAG_SLOP the tab follows the mouse, so a
		// click on the end tab chooses release or stop without moving the loop (stage 22b).
		g->env_moved = g->env_moved || abs(x - g->env_start_x) >= DRAG_SLOP;
		if (g->env_moved && g->env_drag == ENV_LOOP_START) {
			shape->loop_start = clamp(boundary, 0, loop_end_of(shape) - 1);
		} else if (g->env_moved) {
			const int end = clamp(boundary, shape->loop_start + 1, shape->count);
			shape->loop_end = end >= shape->count ? -1 : end;
		}
	} else if (g->env_drag == ENV_LOOP) {
		// The body held: past DRAG_SLOP the whole loop moves with its length; before, nothing.
		g->env_moved = g->env_moved || abs(x - g->env_start_x) >= DRAG_SLOP;
		if (g->env_moved && g->env_start.loop_start >= 0) {
			long start = g->env_start.loop_start, end = loop_end_of(&g->env_start);
			shift_span(&start, &end, boundary - boundary_at(shape->count, g->env_start_x), 0, shape->count);
			shape->loop_start = (int)start;
			shape->loop_end = end >= shape->count ? -1 : (int)end;
		}
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

// Stage 24d: a double click on a column sets its default, the level 0 dB (0) or the pitch 0, in the
// undo step of the click before it, as arp_zero does.
static void envelope_reset(mdmi_gui *g, int x, int y, int part)
{
	const bool again = g->env_handed && g->env_channel == *g->source.channel;
	const patch_envelope before = g->env_before;
	const state_loop loop_before = g->env_loop_before;
	const state_tail tail_before = g->env_tail_before;
	envelope_take(g);
	if (again) {
		g->env_before = before;
		g->env_loop_before = loop_before;
		g->env_tail_before = tail_before;
		g->env_handed = true;
	}
	patch_shape *shape = &g->env_shape;
	if (shape->count <= 0) {
		return;
	}
	const int step = clamp(column_at(shape->count, x), 0, shape->count - 1);
	(void)y;
	const uint8_t was = shape->frames[step];
	// PITCH_CODES[7] is the code of no shift.
	shape->frames[step] = with_value(was, part, part == ENV_LEVEL ? 0 : 7);
	g->env_changed = shape->frames[step] != was;
	envelope_hand(g);
}

// A click on the loop's strip, the mouse let go within DRAG_SLOP: the loop's switch (4A).
static void envelope_switch_loop(mdmi_gui *g)
{
	patch_shape *shape = &g->env_shape;
	{
		const int psg = g->env_channel - PATCH_NUM_FM_CHANNELS;
		// The shadow belongs to the envelope it was switched off on.
		if (strcmp(g->source.loop_shadows[psg].name, g->env_before.name) != 0) {
			g->source.loop_shadows[psg].kept = false;
		}
		int start, end;
		if (shape->loop_start >= 0) {
			// Off: both marks go, the window keeps where they were. The release plays on again
			// as steps of the envelope: a loop always comes back with its release (stage 22b).
			g->env_stop = false;
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
	}
}

// A press on the editor: the start of a gesture on a lane, a tab or the loop's strip, which
// changes nothing until it moves or is let go (stage 18b).
static bool envelope_down(mdmi_gui *g, int x, int y)
{
	patch_shape scratch;
	const int part = envelope_part_at(g, shown_shape(g, &scratch), x, y);
	if (part == ENV_NONE) {
		return false;
	}
	envelope_take(g);
	g->env_drag = part;
	g->env_start = g->env_shape;
	g->env_start_x = x;
	g->env_start_y = y;
	g->env_moved = false;
	if (g->window) {
		SetCapture(g->window);
	}
	envelope_move(g, x, y);
	return true;
}

static void envelope_up(mdmi_gui *g)
{
	if (g->env_drag == ENV_LOOP && !g->env_moved) {
		envelope_switch_loop(g);
	}
	// A click on the end tab or the strip after it: release or stop (stage 22b, D54). Without a
	// release there is nothing to choose.
	if ((g->env_drag == ENV_TAIL || g->env_drag == ENV_LOOP_END) && !g->env_moved && has_tail(&g->env_shape)) {
		g->env_stop = !g->env_stop;
		g->env_changed = true;
	}
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
	// The release after the loop, played or not (stage 22b).
	const bool tail = has_tail(shape), stop = tail && shown_stop(g, shape);
	const int tail_ms = tail ? env_ms(g, shape->count - shape->loop_end) : 0;
	if ((part == ENV_LEVEL || part == ENV_PITCH) && step >= 0 && step < shape->count) {
		if (part == ENV_PITCH && noise_without_pitch(patch_view(g->source.patch), *g->source.channel)) {
			snprintf(hint, size, "CH10: SHIFT WORKS ONLY WITH RATE TONE 3");
			return true;
		}
		if (stop && step >= shape->loop_end) {
			snprintf(hint, size, "STEP %d/%d NOT PLAYED: NOTE OFF STOPS", step + 1, shape->count);
			return true;
		}
		const int level = shape->frames[step] & 15;
		pitch_text(shape->frames[step], pitch, sizeof(pitch));
		char db[12];
		snprintf(db, sizeof(db), level == 15 ? "OFF" : "%d DB", -2 * level);
		snprintf(hint, size, "STEP %d/%d  %d MS  LEVEL %s  PITCH %s", step + 1, shape->count, env_ms(g, step), db, pitch);
	} else if (part == ENV_LOOP_START || part == ENV_LOOP_END) {
		const int end = loop_end_of(shape);
		if (g->env_drag != ENV_NONE && g->env_moved) {
			snprintf(hint, size, "LOOP %d-%d: %d STEPS, %d MS", shape->loop_start + 1, end, end - shape->loop_start, env_ms(g, end - shape->loop_start));
		} else if (part == ENV_LOOP_START) {
			snprintf(hint, size, "LOOP START: DRAG BETWEEN STEPS");
		} else if (!tail) {
			snprintf(hint, size, "LOOP END. NO RELEASE: ALWAYS STOP");
		} else {
			snprintf(hint, size, stop ? "LOOP END, STOP. CLICK: RELEASE %d MS" : "LOOP END, RELEASE %d MS. CLICK: STOP", tail_ms);
		}
	} else if (part == ENV_TAIL) {
		snprintf(hint, size, stop ? "NOTE OFF: STOP. CLICK: RELEASE %d MS" : "NOTE OFF: RELEASE %d MS. CLICK: STOP", tail_ms);
	} else if (part == ENV_LENGTH && shape->count >= room) {
		snprintf(hint, size, "MAX %d BYTES, LOOP MARKS TAKE %d", PATCH_ENVELOPE_STEPS, PATCH_ENVELOPE_STEPS - room);
	} else if (part == ENV_LENGTH) {
		snprintf(hint, size, g->env_drag != ENV_NONE ? "LENGTH %d STEPS, %d MS" : "LENGTH %d STEPS, %d MS: DRAG UP OR DOWN", shape->count, env_ms(g, shape->count));
	} else if (part == ENV_LOOP && shape->loop_start >= 0) {
		snprintf(hint, size, g->env_moved ? "LOOP %d-%d: RELEASE TO KEEP" : "LOOP %d-%d: CLICK OFF, DRAG TO MOVE", shape->loop_start + 1, loop_end_of(shape));
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

// Stop's sign (stage 22b): a 5x5 square in the middle of a 7x7 cell.
static void stop_sign(mdmi_gui *g, int x, int y, uint32_t color)
{
	fill(g, x + 1, y + 1, 5, 5, color);
}

// The sign fit_word falls back to: true and false still mean loop and release.
enum { SIGN_RELEASE, SIGN_LOOP, SIGN_STOP };

// A word in the room it has: the full word, the short one, the sign, then nothing (4A, v3 L1).
static void fit_word(mdmi_gui *g, int x, int room, int y, const char *word, const char *shorter, int sign, uint32_t color, bool centred)
{
	const bool loop = sign == SIGN_LOOP;
	const char *fits = (int)strlen(word) * 6 + 4 <= room ? word : shorter && (int)strlen(shorter) * 6 + 4 <= room ? shorter : NULL;
	if (fits) {
		const int w = (int)strlen(fits) * 6 - 1;
		text(g, centred ? x + (room - w) / 2 : x, y, fits, color, 1);
	} else if (room >= 11) {
		const int left = centred ? x + (room - 7) / 2 : x;
		if (loop) {
			loop_sign(g, left, y, color);
		} else if (sign == SIGN_STOP) {
			stop_sign(g, left, y, color);
		} else {
			release_sign(g, left, y, color);
		}
	}
}

// A value by the mouse, framed, so drawing needs no look down at the footer; held inside its lane.
static void value_tag(mdmi_gui *g, int centre, int y, const char *value, int left, int right)
{
	const int w = (int)strlen(value) * 6 + 5;
	const int x = clamp(centre - w / 2, left, right - w);
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
	// So does the release kept while the loop stops (stage 22b).
	if (g->source.tails && g->source.tails[psg].stop && g->env_drag == ENV_NONE && strcmp(g->source.tails[psg].name, values->psg[psg].name) != 0) {
		g->source.tails[psg].stop = false;
	}
	patch_shape scratch;
	const patch_shape *shape = shown_shape(g, &scratch);
	const int room = patch_shape_room(shape), end = loop_end_of(shape), count = shape->count;
	const bool loop = shape->loop_start >= 0, idle = g->env_drag == ENV_NONE;
	// The release after the loop, dim while Note Off stops the loop.
	const bool tail = has_tail(shape), stop = tail && shown_stop(g, shape);
	const int over = idle ? envelope_part_at(g, shape, g->mouse_x, g->mouse_y) : ENV_NONE;
	int kept_start, kept_end;
	const bool kept = parked_loop(g, shape, &kept_start, &kept_end);
	char buffer[64];

	text_right(g, 292, ENV_LEVEL_Y, "0DB", C_HINT, 1, 20);
	text_right(g, 292, ENV_LEVEL_Y + 49, "-14", C_HINT, 1, 20);
	text_right(g, 292, ENV_LEVEL_Y + 96, "-28", C_HINT, 1, 20);
	text_right(g, 292, ENV_BASE_Y + 1, "OFF", C_HINT, 1, 20);
	text(g, PAGE_X, ENV_PITCH_Y, "PITCH", noise ? C_LINE : C_LABEL, 1);
	if (!noise) {
		static const struct {
			const char *label;
			int y;
		} PITCH_LABELS[] = {{"+5", 0}, {"+1", 15}, {"0", 27}, {"-1", 39}, {"-5", 53}};
		for (size_t i = 0; i < sizeof(PITCH_LABELS) / sizeof(*PITCH_LABELS); i++) {
			text_right(g, 292, ENV_PITCH_Y + PITCH_LABELS[i].y, PITCH_LABELS[i].label, C_HINT, 1, 20);
		}
	}

	// One column per frame, filling the whole lane at every length: the loop on a lighter ground, a
	// grid of levels behind, a dark body with a bright top.
	const int hover = over == ENV_LEVEL || over == ENV_PITCH ? column_at(count, g->mouse_x) : -1;
	// Stage 41 (psg-playing-column.md): the column the note is at lights up, as W17 on the ROM screen;
	// never in a release not played.
	patch_shape played;
	played_shape(shape, stop, &played);
	const int playing = envelope_frame(g, &played, psg, mdmi_engine_get_stats(g->source.engine).sample_rate);
	for (int i = 0; i < count; i++) {
		const int x = column_x(count, i), w = column_x(count, i + 1) - x;
		const bool in_loop = loop && i >= shape->loop_start && i < end;
		// A release not played (stage 22b): there, but as dim as can still be read.
		const bool unplayed = stop && i >= end;
		const bool lit = i == playing && !unplayed;
		const uint32_t back = lit ? C_PSG_DARK : i == hover ? C_LINE : in_loop ? C_CELL : C_BG;
		fill(g, x, ENV_LEVEL_Y, w, ENV_LEVEL_BOTTOM - ENV_LEVEL_Y, back);
		fill(g, x, ENV_PITCH_Y, w, ENV_PITCH_BOTTOM - ENV_PITCH_Y, back);
		for (int k = 1; k < 15; k++) {
			fill(g, x, ENV_LEVEL_Y + ENV_LEVEL_ROW * k, w, 1, lit || in_loop || i == hover ? C_LINE : C_CELL);
		}
		const int level = shape->frames[i] & 15, top = ENV_LEVEL_Y + ENV_LEVEL_ROW * level;
		if (level < 15) {
			fill(g, x, top + 3, w - 1, ENV_BASE_Y - top - 3, unplayed ? C_CELL : lit ? C_PSG : C_PSG_DARK);
			fill(g, x, top, w - 1, 3, unplayed ? C_LINE : lit ? C_TEXT : C_PSG);
		}
		const int row = pitch_row(shape->frames[i]), pitch_top = ENV_PITCH_Y + row * ENV_PITCH_ROW;
		const uint32_t pitch_ink = noise || unplayed ? C_LINE : lit ? C_TEXT : C_PSG;
		if (row < 7) {
			fill(g, x, pitch_top, w - 1, ENV_CENTRE_Y - pitch_top, pitch_ink);
		} else if (row > 7) {
			fill(g, x, ENV_CENTRE_Y + 1, w - 1, pitch_top + ENV_PITCH_ROW - ENV_CENTRE_Y - 1, pitch_ink);
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
		if (tail) {
			// The release's or stop's switch after the end tab (stage 22b, dac-page.md): stop has the
			// shadow's outline, "lies there, isn't played".
			const int rx = end_x + 9, rw = ENV_X + ENV_LANE_W - rx, tail_x = end_x + 12;
			const bool on = over == ENV_TAIL;
			if ((stop || on) && rw >= 3) {
				frame(g, rx, ENV_LOOP_Y, rw, ENV_LOOP_H, on ? C_TEXT : C_LINE);
			}
			fit_word(g, tail_x, ENV_X + ENV_LANE_W - tail_x, ENV_LOOP_Y + 3, stop ? "STOP" : "RELEASE", stop ? NULL : "REL", stop ? SIGN_STOP : SIGN_RELEASE, on ? C_TEXT : C_LABEL, false);
		}
		fill(g, start_x, ENV_LOOP_Y, 1, ENV_PITCH_BOTTOM - ENV_LOOP_Y, C_PSG);
		fill(g, end_x, ENV_LOOP_Y, 1, ENV_PITCH_BOTTOM - ENV_LOOP_Y, C_PSG);
		// A held tab is amber once it moves; pressed for a click, it reads as under the mouse.
		const int held = idle ? ENV_NONE : g->env_drag;
		fill(g, start_x - 8, ENV_LOOP_Y, 8, ENV_LOOP_H, held == ENV_LOOP_START ? (g->env_moved ? C_AMBER : C_TEXT) : over == ENV_LOOP_START ? C_TEXT : C_PSG);
		fill(g, end_x + 1, ENV_LOOP_Y, 8, ENV_LOOP_H, held == ENV_LOOP_END ? (g->env_moved ? C_AMBER : C_TEXT) : over == ENV_LOOP_END ? C_TEXT : C_PSG);
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
			snprintf(buffer, sizeof(buffer), "%d", env_ms(g, f));
			text(g, x - (int)strlen(buffer) * 3, ENV_AXIS_Y + 14, buffer, C_HINT, 1);
		}
	}
	fill(g, lane_end, ENV_AXIS_Y, 1, 3, C_LABEL);
	// Stage 49 (ui-concept.md): at x1/2 a step is two frames; the row counts steps, the MS row follows SPD.
	text_right(g, 286, ENV_AXIS_Y + 4, "STEP", C_HINT, 1, 40);
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

	// The value by the mouse: the level over its bar, the loop's steps by a tab.
	const int part = idle ? over : g->env_drag;
	const int step = idle ? hover : g->env_last_step;
	if (part == ENV_LEVEL && step >= 0 && step < count) {
		const int level = shape->frames[step] & 15;
		const int centre = (column_x(count, step) + column_x(count, step + 1)) / 2;
		snprintf(buffer, sizeof(buffer), level == 15 ? "OFF" : "%dDB", -2 * level);
		// Over the bar, or inside it where the bar reaches the top.
		const int above = ENV_LEVEL_Y + ENV_LEVEL_ROW * level - 16;
		value_tag(g, centre, above >= ENV_LEVEL_Y ? above : ENV_LEVEL_Y + ENV_LEVEL_ROW * level + 5, buffer, ENV_X, ENV_X + ENV_LANE_W);
	} else if (loop && (part == ENV_LOOP_START || part == ENV_LOOP_END)) {
		snprintf(buffer, sizeof(buffer), "%d-%d", shape->loop_start + 1, end);
		value_tag(g, boundary_x(count, part == ENV_LOOP_START ? shape->loop_start : end), ENV_LEVEL_Y + 2, buffer, ENV_X, ENV_X + ENV_LANE_W);
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

// Stage 24f: where an FM channel's last note is on the graph's time, in seconds of the graph, or -1.
// Held, it runs up to Note Off at 1 s and stays there; released, it goes on from Note Off into the
// release, also when the key was let go before 1 s, as the graph's release starts there (art-director).
static double fm_cursor_seconds(const mdmi_gui *g, int channel)
{
	const gui_psg_times *times = g->source.psg_times;
	const unsigned rate = mdmi_engine_get_stats(g->source.engine).sample_rate;
	if (!times || rate == 0 || channel < 0 || channel >= PATCH_NUM_FM_CHANNELS) {
		return -1;
	}
	const uint64_t on = __atomic_load_n(&times->fm_on[channel], __ATOMIC_ACQUIRE);
	const uint64_t off = __atomic_load_n(&times->fm_off[channel], __ATOMIC_ACQUIRE);
	const uint64_t now = __atomic_load_n(&times->clock, __ATOMIC_ACQUIRE);
	if (on == GUI_NEVER || on > now) {
		return -1;
	}
	double s;
	if (off == GUI_NEVER || off < on || off > now) {
		s = (double)(now - on) / rate;
		s = s < PATCH_FM_OFF_SECONDS ? s : PATCH_FM_OFF_SECONDS;
	} else {
		s = PATCH_FM_OFF_SECONDS + (double)(now - off) / rate;
	}
	return s < PATCH_FM_SECONDS ? s : -1;
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
	const double cursor = fm_cursor_seconds(g, channel);
	const bool parked = cursor == PATCH_FM_OFF_SECONDS;
	text(g, off - 19, GRAPH_Y + 24, "OFF", parked ? C_PLAYING : C_HINT, 1);
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
	// Stage 24f: the last note's time, a green line over everything, a dot where it meets the active curve.
	if (cursor >= 0) {
		const int x = x0 + curve_x(cursor, w);
		fill(g, x, y0, 1, h, C_PLAYING);
		if (active >= 0) {
			const uint16_t *low, *high;
			curve_of(g, values, channel, active, &low, &high);
			const int column = (x - x0) * CURVE_COLUMNS / w;
			const int y = y0 + low[column < CURVE_COLUMNS ? column : CURVE_COLUMNS - 1] * (h - 1) / PATCH_FM_SILENT;
			fill(g, x - 1, y - 1, 3, 3, C_PLAYING);
		}
	}
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
// The loop's row under the trim strip (stage 18b, docs/design/dac-page.md): what repeats.
#define DAC_LOOP_Y 310
#define DAC_LOOP_H 12
#define WAVE_Y 323
#define WAVE_W DAC_WAVE_COLUMNS
#define WAVE_H 32
#define DAC_BUTTON_Y 358
#define DAC_LINE1_Y 377
#define DAC_LINE2_Y 387
#define DAC_REPLACE_X 498
#define DAC_CLEAR_X 602
#define DAC_BUTTON_W 100
#define DAC_BUTTON_H 24
#define DAC_DEFAULT_NOTE 36
#define DAC_MODE_Y 56
#define DAC_MODE_H 20
#define DAC_AUDIO_X 590
#define DAC_AUDIO_W 112
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
	// Stage 37: a project read against the whole space can hold more than a change may fill.
	const uint32_t room = rom_image_room(), used_all = dac_used(dac_of(g)), used = used_all < room ? used_all : room;
	fill(g, DAC_FREE_X, 30, DAC_FREE_W, 22, C_CELL);
	fill(g, DAC_FREE_X, 30, (int)((uint64_t)(room - used) * DAC_FREE_W / room), 22, C_DAC_DARK);
	text(g, DAC_FREE_X + 4, 38, "FREE", C_LABEL, 1);
	snprintf(buffer, sizeof(buffer), "%.1f MB", (room - used) / 1048576.0);
	text_right(g, DAC_FREE_X + DAC_FREE_W - 4, 34, buffer, (room - used) * 10 < room ? C_AMBER : C_TEXT, 2, 84);
	// A lamp, not a switch (stage 23, D74): lit while a track's audio reaches the DAC through DAC In,
	// which plays together with the samples. This page shows only while the DAC is on.
	const bool heard = mdmi_engine_get_stats(g->source.engine).input_signal;
	if (heard) {
		fill(g, DAC_AUDIO_X, DAC_MODE_Y, DAC_AUDIO_W, DAC_MODE_H, C_DAC_DARK);
	}
	frame(g, DAC_AUDIO_X, DAC_MODE_Y, DAC_AUDIO_W, DAC_MODE_H, heard ? C_DAC : C_CELL);
	text(g, DAC_AUDIO_X + (DAC_AUDIO_W - 8 * 12) / 2, DAC_MODE_Y + 3, "AUDIO IN", heard ? C_TEXT : C_LINE, 2);
	fill(g, RAIL_W + 1, 79, GUI_WIDTH - RAIL_W - 1, 1, C_CELL);
}

// What the ROM does with samples (hardware-rom.md, section 9), the same on both pages.
static void dac_warnings(mdmi_gui *g, const patch_values *values, int y)
{
	const bool tuned = values->channels[PATCH_NUM_FM_CHANNELS - 1][PARAM_FINE_TUNE] != 64;
	const int player = patch_voice_player(g->source.patch, PATCH_NUM_FM_CHANNELS - 1);
	const bool lent = player >= 0 && player != PATCH_NUM_FM_CHANNELS - 1;
	fill(g, BANK_X, y, PAGE_RIGHT - BANK_X, 1, C_CELL);
	text(g, BANK_X, y + 5, "4 VOICES: A 5TH NOTE CUTS THE OLDEST, LOUD ONES CLIP. VELOCITY IS VOLUME.", C_HINT, 1);
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
	// The loop row's tabs and its body, or the shadow while LOOP is off (stage 18b).
	DACC_LOOP_START, DACC_LOOP_END, DACC_LOOP_BODY,
	// The loop row after the loop's end, where a click chooses release or stop (stage 22b).
	DACC_LOOP_TAIL,
	// The loop row of a sample under one block with LOOP off: the footer says why, a click does nothing
	// (stage 30: what the dim LOOP switch said).
	DACC_LOOP_SHORT,
};
#define DAC_ROW_A_Y 398
// The rate's words in RATE's row (stage 19).
#define DAC_RATE_TEXT_X 274
#define DAC_RATE_TEXT_W 139
#define DAC_ROW_B_Y 422
#define DAC_CELL_H 20

static const struct {
	int x, y, w;
	const char *label;
} DAC_CONTROLS[] = {
	// START and END are the trim strip's tabs, no cells.
	{0, 0, 0, "START"}, {0, 0, 0, "END"}, {242, DAC_ROW_B_Y, 84, "PITCH"}, {328, DAC_ROW_B_Y, 62, "CT"},
	{396, DAC_ROW_B_Y, 100, "GAIN"}, {498, DAC_ROW_B_Y, 62, "NORMALIZE"}, {566, DAC_ROW_B_Y, 52, "FADE"}, {620, DAC_ROW_B_Y, 82, "ONE-SHOT"},
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

// The marks the loop's row shows: those that play, or with LOOP off where they would (the shadow).
static void shown_marks(const dac_sample *own, const dac_edit *edit, dac_marks *m)
{
	dac_edit on = *edit;
	on.loop = true;
	dac_marks_of(own, &on, m);
}

// FADE does nothing to a loop that has no release, or one Note Off stops (stage 22b): it has no end.
static bool fade_unused(const dac_sample *own, const dac_edit *edit)
{
	dac_marks m;
	dac_marks_of(own, edit, &m);
	return m.loops && !m.release;
}

// Where a click chooses release or stop on the loop's row: from the loop's end to the release's,
// at least 20 px wide, short of the page's edge (stage 22b, dac-page.md).
static int tail_zone_end(int le, int end)
{
	const int right = end - 2 > le + 19 ? end - 2 : le + 19;
	return right < 699 ? right : 699;
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
	if (y >= DAC_LOOP_Y && y < DAC_LOOP_Y + DAC_LOOP_H) {
		const dac_edit *edit = shown_edit(g, own);
		dac_marks m;
		shown_marks(own, edit, &m);
		if (m.too_short) {
			if (edit->loop && x >= wave_x_of(own, edit->start) && x < wave_x_of(own, edit->end)) {
				return DACC_LOOP_BODY;
			}
			return edit->loop || x < WAVE_X || x >= WAVE_X + WAVE_W ? DACC_NONE : DACC_LOOP_SHORT;
		}
		const int ls = wave_x_of(own, m.frame_loop_start), le = wave_x_of(own, m.frame_loop_end);
		if (edit->loop && x >= ls - TRIM_TAB_W && x < ls) {
			return DACC_LOOP_START;
		}
		if (edit->loop && x >= le && x < le + TRIM_TAB_W) {
			return DACC_LOOP_END;
		}
		if (x >= ls && x < le) {
			return DACC_LOOP_BODY;
		}
		// Release or stop, only with something after the loop; LOADING takes no click there.
		return edit->loop && m.tail && engine_booted(g) && x >= le && x <= tail_zone_end(le, wave_x_of(own, edit->end)) ? DACC_LOOP_TAIL : DACC_NONE;
	}
	if (y >= WAVE_Y && y < WAVE_Y + WAVE_H) {
		const dac_edit *edit = shown_edit(g, own);
		const int start = wave_x_of(own, edit->start), end = wave_x_of(own, edit->end);
		const bool near_start = abs(x - start) <= 4, near_end = abs(x - end) <= 4;
		if (near_start && near_end) {
			return x < (start + end) / 2 ? DACC_MARK_START : DACC_MARK_END;
		}
		if (near_start || near_end) {
			return near_start ? DACC_MARK_START : DACC_MARK_END;
		}
		// The loop's lines, where no trim line is near.
		dac_marks m;
		dac_marks_of(own, edit, &m);
		if (!m.loops) {
			return DACC_NONE;
		}
		const int ls = wave_x_of(own, m.frame_loop_start), le = wave_x_of(own, m.frame_loop_end) - 1;
		const bool near_ls = abs(x - ls) <= 4, near_le = abs(x - le) <= 4;
		if (near_ls && near_le) {
			return x < (ls + le) / 2 ? DACC_LOOP_START : DACC_LOOP_END;
		}
		return near_ls ? DACC_LOOP_START : near_le ? DACC_LOOP_END : DACC_NONE;
	}
	for (int c = 0; c < DACC_RATE_AUTO; c++) {
		if (in_box(x, y, DAC_CONTROLS[c].x, DAC_CONTROLS[c].y, DAC_CONTROLS[c].w, DAC_CELL_H)) {
			return c;
		}
	}
	// The rate's words answer the mouse with the footer only.
	return in_box(x, y, DAC_RATE_TEXT_X, DAC_ROW_A_Y, DAC_RATE_TEXT_W, DAC_CELL_H) ? DACC_RATE_AUTO : DACC_NONE;
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
		const bool unused = fade_unused(own, edit);
		const bool over = booted && !unused && g->dac_drag == DACC_NONE && in_box(g->mouse_x, g->mouse_y, x, y, w, DAC_CELL_H);
		const char *word = edit->fade ? "FADE" : "NO FADE";
		// A loop with no release has no end to fade: the word stays, too dim to read as a choice
		// (stage 18); with a release FADE is the release's end (18b).
		text(g, x + (w - (int)strlen(word) * 6) / 2, y + 7, word, unused ? C_CELL : !booted ? C_LINE : over ? C_TEXT : edit->fade ? C_LINE : C_AMBER, 1);
	}
	// LOOP is the loop row's own click (stage 30): no switch of its own.
	switch_cell(g, DACC_ONE_SHOT, edit->one_shot, booted);
	// RATE: words, not cells, since PCM4 has one rate for all voices (stage 19, dac-page.md).
	snprintf(buffer, sizeof(buffer), "%.1f KHZ FOR ALL VOICES", ROM_PCM4_HZ / 1000);
	text(g, DAC_RATE_TEXT_X, DAC_ROW_A_Y + 7, buffer, C_HINT, 1);
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

// A WAV held over the wave goes to the open note: the BANK list's drop frame, the wave under it (stage 18).
static void draw_wave_drop(mdmi_gui *g)
{
	if (g->file_over && g->dac_drop_wave && g->dac_drop_count > 0) {
		frame(g, WAVE_X, WAVE_Y, WAVE_W, WAVE_H, C_AMBER);
		frame(g, WAVE_X + 1, WAVE_Y + 1, WAVE_W - 2, WAVE_H - 2, C_AMBER);
	}
}

// The loop's row (stage 18b, docs/design/dac-page.md): hollow tabs outside the loop's ends, its body
// with its length, RELEASE after it, solid lines in the row and dotted down the wave. With LOOP off,
// an outline where it would be; too short, a word why.
static void draw_loop_row(mdmi_gui *g, const dac_sample *own, const dac_edit *edit, const dac_marks *m, int over)
{
	const int start = wave_x_of(own, edit->start), end = wave_x_of(own, edit->end);
	const bool booted = engine_booted(g);
	const bool body_held = g->dac_drag == DACC_LOOP_BODY && g->dac_moved;
	char a[16], words[32];
	if (m->too_short) {
		if (edit->loop) {
			frame(g, start, DAC_LOOP_Y, end - start, DAC_LOOP_H, C_AMBER);
			fit_word(g, start + 1, end - start - 2, DAC_LOOP_Y + 3, "TOO SHORT", NULL, true, C_AMBER, true);
		} else {
			text(g, WAVE_X, DAC_LOOP_Y + 3, "TOO SHORT TO LOOP", C_HINT, 1);
		}
		return;
	}
	const int ls = wave_x_of(own, m->frame_loop_start), le = wave_x_of(own, m->frame_loop_end);
	ms_text(ms_of(own, m->frame_loop_end - m->frame_loop_start), a, sizeof(a));
	snprintf(words, sizeof(words), "LOOP %s MS", a);
	if (!edit->loop) {
		const uint32_t ink = body_held ? C_AMBER : over == DACC_LOOP_BODY && booted ? C_TEXT : C_LINE;
		frame(g, ls, DAC_LOOP_Y, le - ls, DAC_LOOP_H, ink);
		fit_word(g, ls + 1, le - ls - 2, DAC_LOOP_Y + 3, "LOOP", NULL, true, ink == C_LINE ? C_LABEL : ink, true);
		return;
	}
	fill(g, ls + 1, DAC_LOOP_Y, le - ls - 2, DAC_LOOP_H, booted ? C_DAC_DARK : C_CELL);
	if (body_held || (over == DACC_LOOP_BODY && booted)) {
		frame(g, ls + 1, DAC_LOOP_Y, le - ls - 2, DAC_LOOP_H, body_held ? C_AMBER : C_TEXT);
	}
	fit_word(g, ls + 1, le - ls - 2, DAC_LOOP_Y + 3, words, "LOOP", true, body_held ? C_AMBER : booted ? C_DAC : C_LINE, true);
	if (m->tail) {
		// Release or stop (stage 22b): stop has the shadow's outline, "lies there, isn't played".
		const bool stop = !m->release, on = over == DACC_LOOP_TAIL && booted;
		const int rx = le + TRIM_TAB_W, rw = end - 2 - rx;
		const uint32_t ink = !booted ? C_CELL : on ? C_TEXT : C_LINE;
		if ((stop || on) && rw >= 3) {
			frame(g, rx, DAC_LOOP_Y, rw, DAC_LOOP_H, ink);
		}
		fit_word(g, le + TRIM_TAB_W + 3, end - 2 - (le + TRIM_TAB_W + 3), DAC_LOOP_Y + 3, stop ? "STOP" : "RELEASE", stop ? NULL : "REL", stop ? SIGN_STOP : SIGN_RELEASE,
			!booted ? C_LINE : on ? C_TEXT : C_LABEL, false);
	}
	// Hollow tabs, solid when the mouse is on them; the lines take their colour.
	for (int which = 0; which < 2; which++) {
		const int c = which ? DACC_LOOP_END : DACC_LOOP_START;
		const int tab = which ? le : ls - TRIM_TAB_W, line = which ? le - 1 : ls;
		// A held tab is amber once it moves; pressed for a click, as under the mouse (stage 22b).
		const uint32_t ink = !booted ? C_LINE : (g->dac_drag == c && g->dac_moved) || body_held ? C_AMBER : over == c ? C_TEXT : C_DAC;
		if (ink == C_DAC || ink == C_LINE) {
			fill(g, tab, DAC_LOOP_Y, TRIM_TAB_W, DAC_LOOP_H, booted ? C_DAC_DARK : C_CELL);
			frame(g, tab, DAC_LOOP_Y, TRIM_TAB_W, DAC_LOOP_H, ink);
		} else {
			fill(g, tab, DAC_LOOP_Y, TRIM_TAB_W, DAC_LOOP_H, ink);
		}
		fill(g, line, DAC_LOOP_Y, 1, DAC_LOOP_H, ink);
		for (int y = WAVE_Y; y < WAVE_Y + WAVE_H; y += 2) {
			fill(g, line, y, 1, 1, ink);
		}
	}
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
		draw_wave_drop(g);
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
	// With LOOP and no release the part after the last whole block isn't played: it reads as
	// trimmed (stage 18); with stop too, all after the loop (stage 22b). Too short to loop, it all
	// reads dim.
	dac_marks marks = {0};
	if (edit) {
		shown_marks(own, edit, &marks);
	}
	const bool looping = edit && edit->loop, short_loop = looping && marks.too_short;
	const int played = !edit ? end : short_loop ? start : looping ? wave_x_of(own, marks.frame_end) : end;
	if (edit) {
		fill(g, start, WAVE_Y, played - start, WAVE_H, C_CELL);
	}
	fill(g, WAVE_X, middle, WAVE_W, 1, C_LINE);
	for (int column = 0; column < WAVE_W; column++) {
		const int x = WAVE_X + column;
		const bool kept = x >= start && x < played;
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
		draw_loop_row(g, own, edit, &marks, over);
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
	draw_wave_drop(g);
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
	case DACC_LOOP_START:
	case DACC_LOOP_END:
	case DACC_LOOP_BODY: {
		// From where the marks stood at the press; they snap to blocks as they are drawn (stage 18b).
		dac_marks m, all;
		shown_marks(own, from, &m);
		dac_edit whole = *from;
		whole.loop_start = whole.loop_end = 0;
		shown_marks(own, &whole, &all);
		long ls = m.frame_loop_start, le = m.frame_loop_end;
		if (g->dac_drag == DACC_LOOP_BODY) {
			shift_span(&ls, &le, shift, from->start, all.frame_loop_end);
			// The length holds: where an intro isn't allowed yet, the loop stays at START.
			dac_edit moved = *from;
			moved.loop_start = (uint32_t)ls;
			moved.loop_end = (uint32_t)le;
			dac_marks after;
			shown_marks(own, &moved, &after);
			if (after.loop_end - after.loop_start != m.loop_end - m.loop_start) {
				le = (long)from->start + (le - ls);
				ls = from->start;
			}
		} else if (g->dac_drag == DACC_LOOP_START) {
			ls += shift;
			ls = ls < (long)from->start ? (long)from->start : ls >= le ? le - 1 : ls;
		} else {
			le += shift;
			le = le > (long)from->end ? (long)from->end : le <= ls ? ls + 1 : le;
		}
		work->loop_start = (uint32_t)ls;
		work->loop_end = (uint32_t)le;
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
static void dac_apply_to(mdmi_gui *g, int note, const dac_edit *edit, bool join)
{
	const dac_sample *own = panel_sample(g, note);
	if (!own || dac_same_edit(edit, &own->edit)) {
		return;
	}
	char message[FILES_MESSAGE];
	const bool done = files_edit_sample(g->source.files, note, edit, join, message, sizeof(message));
	say(g, message, done ? C_AMBER : C_ERROR, done ? MESSAGE_TICKS : ERROR_TICKS);
}

static void dac_apply(mdmi_gui *g, int note, const dac_edit *edit)
{
	dac_apply_to(g, note, edit, false);
}

// A press on the panel: a drag begins, or a switch, a rate or NORMALIZE applies at once.
static bool dac_panel_down(mdmi_gui *g, int x, int y)
{
	const int c = dac_control_at(g, x, y);
	const dac_sample *own = panel_sample(g, g->dac_selected);
	g->dac_tail_clicked = false;
	if (c == DACC_NONE || !own || !engine_booted(g)) {
		return c != DACC_NONE;
	}
	dac_edit edit = own->edit;
	if ((c == DACC_FADE && fade_unused(own, &edit)) || c == DACC_LOOP_SHORT || (c >= DACC_RATE_AUTO && c < DACC_RATE_END)) {
		return true;
	}
	if (c == DACC_FADE || c == DACC_ONE_SHOT || c == DACC_NORMALIZE) {
		if (c == DACC_FADE) {
			edit.fade = !edit.fade;
		} else if (c == DACC_ONE_SHOT) {
			// ONE-SHOT and LOOP exclude each other: a loop would never stop (stage 18; D54 cancels
			// D53). A loop always comes back with its release: either switch drops stop (22b).
			edit.one_shot = !edit.one_shot;
			edit.loop = edit.loop && !edit.one_shot;
			edit.stop = false;
		} else {
			edit.gain = (int16_t)dac_normalize_gain(own);
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
	case DACC_LOOP_START:
		// No intro.
		edit.loop_start = 0;
		break;
	case DACC_LOOP_END:
		// No release: the loop runs to the last whole block. The first click switched release and
		// stop; that goes back, and the change joins the click's undo step (stage 22b).
		edit.loop_end = 0;
		if (g->dac_tail_clicked) {
			g->dac_tail_clicked = false;
			edit.stop = !edit.stop;
			dac_apply_to(g, g->dac_selected, &edit, true);
			return true;
		}
		break;
	case DACC_LOOP_BODY:
	case DACC_LOOP_TAIL:
	case DACC_LOOP_SHORT:
		// Its first click switched the loop, or the release and stop, already: one undo step (stage 24d).
		return true;
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

// The footer over the loop's row: the tab's place, the loop's length, why a mark stops (stage 18b).
static void loop_hint(const dac_sample *own, const dac_edit *edit, int c, bool held, char *out, size_t size)
{
	dac_marks m;
	shown_marks(own, edit, &m);
	char a[16], b[16];
	const double block_ms = ROM_SAMPLE_BLOCK * 1000.0 / ROM_PCM4_HZ;
	if (m.too_short) {
		ms_text(ms_of(own, edit->end - edit->start), a, sizeof(a));
		ms_text(block_ms, b, sizeof(b));
		snprintf(out, size, "TOO SHORT: %s MS KEPT, LOOP NEEDS %s", a, b);
		return;
	}
	const bool shortest = m.loop_end - m.loop_start == 1;
	if (c == DACC_LOOP_START) {
		// An intro wanted where the loop ends too early for the ROM to catch it.
		const bool refused = edit->loop_start > edit->start && !m.loop_start && m.loop_end < m.least_with_intro && edit->loop_start >= edit->start + (m.frame_loop_end - edit->start) / (2 * m.loop_end);
		ms_text(ms_of(own, m.frame_loop_start - edit->start), a, sizeof(a));
		ms_text(block_ms * m.least_with_intro, b, sizeof(b));
		if (held && refused) {
			snprintf(out, size, "INTRO NEEDS LOOP END AT %s MS OR LATER", b);
		} else if (held && shortest) {
			snprintf(out, size, "LOOP: SHORTEST IS ONE BLOCK, %.1f MS", block_ms);
		} else {
			snprintf(out, size, held ? "LOOP START %s MS. RELEASE TO APPLY" : "LOOP START %s MS. DOUBLE CLICK: NO INTRO", a);
		}
	} else if (c == DACC_LOOP_END) {
		ms_text(ms_of(own, m.frame_loop_end - edit->start), a, sizeof(a));
		if (held && shortest) {
			snprintf(out, size, "LOOP: SHORTEST IS ONE BLOCK, %.1f MS", block_ms);
		} else if (held) {
			snprintf(out, size, "LOOP END %s MS. RELEASE TO APPLY", a);
		} else if (!m.tail) {
			snprintf(out, size, "LOOP END %s MS, NO RELEASE: ALWAYS STOP", a);
		} else {
			// Release or stop by a click (stage 22b); a double click's no release is in README.
			snprintf(out, size, m.release ? "LOOP END %s MS, RELEASE. CLICK: STOP" : "LOOP END %s MS, STOP. CLICK: RELEASE", a);
		}
	} else if (c == DACC_LOOP_TAIL) {
		ms_text(ms_of(own, m.frame_tail_end - m.frame_loop_end), a, sizeof(a));
		snprintf(out, size, m.release ? "NOTE OFF: RELEASE %s MS. CLICK: STOP" : "NOTE OFF: STOP. CLICK: RELEASE %s MS", a);
	} else {
		ms_text(ms_of(own, m.frame_loop_end - m.frame_loop_start), a, sizeof(a));
		ms_text(ms_of(own, m.frame_loop_start - edit->start), b, sizeof(b));
		if (held) {
			snprintf(out, size, "LOOP %s MS AT %s MS. RELEASE TO APPLY", a, b);
		} else {
			snprintf(out, size, edit->loop ? "LOOP %s MS: CLICK OFF, DRAG TO MOVE" : "LOOP %s MS IS OFF. CLICK: ON, DRAG: MOVE", a);
		}
	}
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
		snprintf(out, size, "PITCH %+d ST: %.2f S. DOUBLE CLICK: 0", semitones, dac_cost(own, edit) / ROM_PCM4_HZ);
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
	case DACC_FADE: {
		dac_marks fade_marks;
		dac_marks_of(own, edit, &fade_marks);
		const char *unused = fade_marks.tail ? "FADE: NOT USED, LOOP STOPS AT NOTE OFF" : "FADE: NOT USED BY A LOOP WITHOUT RELEASE";
		snprintf(out, size, "%s", fade_unused(own, edit) ? unused : edit->fade ? "FADE: LAST 4 MS FADE OUT. CLICK: OFF" : "NO FADE: THE END MAY CLICK. CLICK: ON");
		break;
	}
	case DACC_LOOP_START:
	case DACC_LOOP_END:
	case DACC_LOOP_BODY:
	case DACC_LOOP_TAIL:
	case DACC_LOOP_SHORT:
		// A tab pressed for a click says what the click does until it moves (stage 22b).
		loop_hint(own, edit, c, held && g->dac_moved, out, size);
		break;
	case DACC_ONE_SHOT:
		snprintf(out, size, edit->loop ? "ONE-SHOT: PLAYS TO THE END, LOOP GOES OFF" : edit->one_shot ? "ONE-SHOT: NOTE OFF DOES NOT CUT IT" : "ONE-SHOT OFF: NOTE OFF CUTS THE SAMPLE");
		break;
	default:
		// The rate's words (stage 19): PCM4 plays all four voices at one rate.
		snprintf(out, size, "RATE IS FIXED: 4 VOICES SHARE %.1f KHZ", ROM_PCM4_HZ / 1000);
		break;
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
	fill(g, BANK_X, TRIM_Y, WAVE_W, WAVE_Y - TRIM_Y, C_BG);
	draw_wave(g, note, has ? &sample : NULL, own, edit);
	// The note's name big, its number first on the line below (owner, D37).
	char key[8];
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
		// While a gesture lasts, what the edit would make. One rate for all (stage 19), shown in RATE.
		const double seconds = (edit ? dac_cost(own, edit) : sample.length) / ROM_PCM4_HZ;
		snprintf(buffer, sizeof(buffer), "  %.2f S", seconds);
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
			: "4 VOICES: A 5TH NOTE CUTS THE OLDEST, LOUD ONES CLIP. VELOCITY IS VOLUME.", lent || tuned ? C_AMBER : C_HINT, 1);
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
			snprintf(buffer, sizeof(buffer), "%.2f S", sample.length / ROM_PCM4_HZ);
			text_right(g, 520, y + 7, buffer, C_LABEL, 1, 60);
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
	char key[8];
	note_name(note, key, sizeof(key));
	(void)grid;
	if (!note_sample(g, note, &sample, &name, &file)) {
		snprintf(out, size, "%d %s EMPTY: DROP A WAV HERE", note, key);
		return;
	}
	// The footer holds about 42 characters before the version: a file's name goes last, cut at its end.
	if (file) {
		snprintf(out, size, "%d %s %.2f S %.60s", note, key, sample.length / ROM_PCM4_HZ, name);
	} else {
		snprintf(out, size, "%d %s %s %.2f S, ROM", note, key, name, sample.length / ROM_PCM4_HZ);
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
		const double left = (double)rom_image_room() - (double)dac_used(dac_of(g)), free = left > 0 ? left : 0;
		snprintf(out, size, "FREE %.2f MB: %.0f S OF SAMPLES", free / 1048576.0, free / ROM_PCM4_HZ);
		return true;
	}
	const int note = grid ? grid_note_at(x, y) : dac_note_at(g, x, y);
	const bool replace = grid && in_box(x, y, DAC_REPLACE_X, DAC_BUTTON_Y, DAC_BUTTON_W, DAC_BUTTON_H);
	const bool clear = grid && in_box(x, y, DAC_CLEAR_X, DAC_BUTTON_Y, DAC_BUTTON_W, DAC_BUTTON_H);
	const bool wave = grid && in_box(x, y, WAVE_X, WAVE_Y, WAVE_W, WAVE_H);
	if (in_box(x, y, DAC_AUDIO_X, DAC_MODE_Y, DAC_AUDIO_W, DAC_MODE_H)) {
		snprintf(out, size, mdmi_engine_get_stats(g->source.engine).input_signal ? "AUDIO IN: A TRACK IN THE DAC. FILTER UP FOR ITS GRIT"
			: "AUDIO IN: NO SIGNAL. LIVE: TRACK'S AUDIO TO DAC IN-MDMI");
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

// The arp row is on the open page: a PSG channel's editor, or the FM page showing it.
static bool arp_shown(const mdmi_gui *g)
{
	const int channel = *g->source.channel;
	if (*g->source.page != PAGE_EDITOR) {
		return false;
	}
	return !channel_is_fm(channel) || (g->arp_view && !is_dac(patch_view(g->source.patch), channel));
}

// The row's top left corner: the graph's on FM, under the envelope on PSG.
// Stage 24d: the row is as wide as the page on PSG (owner), on FM in the graph's place.
#define ARP_PSG_W 460
static int arp_w(const mdmi_gui *g)
{
	return channel_is_fm(*g->source.channel) ? GRAPH_W : ARP_PSG_W;
}

static void arp_corner(const mdmi_gui *g, int *bx, int *by)
{
	const bool fm = channel_is_fm(*g->source.channel);
	*bx = fm ? GRAPH_X : PAGE_X;
	*by = fm ? GRAPH_Y : ARP_PSG_Y;
}

// What the row shows: the window's copy while a gesture lasts, the channel's row otherwise.
static const patch_arp *shown_arp(mdmi_gui *g)
{
	const int channel = *g->source.channel;
	if (g->arp_drag != ARP_NONE && g->arp_channel == channel) {
		return &g->arp_row;
	}
	return patch_arp_view(g->source.patch, channel);
}

// A shift's 2 px band in the lane, +24 at the top.
static int arp_y(int ly, int shift)
{
	return ly + 2 * (PATCH_ARP_SHIFT - shift);
}

static int arp_x(const mdmi_gui *g, int lx, int count, int step)
{
	return lane_column_x(lx, ARP_LANE_W, count, step);
}

static void shift_text(int shift, char *out, size_t size)
{
	snprintf(out, size, shift > 0 ? "+%d" : "%d", shift);
}

// The length cell, the row's switch, the loop's tab, body or shadow, the loop's switch, or the lane
// under the mouse. A dim row takes no mouse; an empty one only its lane and length.
static int arp_part_at(mdmi_gui *g, const patch_arp *row, int x, int y)
{
	if (!arp_shown(g) || !patch_arp_works(patch_view(g->source.patch), *g->source.channel)) {
		return ARP_NONE;
	}
	int bx, by;
	arp_corner(g, &bx, &by);
	const int lx = bx + ARP_LANE_DX, ly = by + ARP_LANE_DY, end = lx + ARP_LANE_W, count = row->count;
	if (in_box(x, y, bx + ARP_RIGHT_DX, by + ARP_LENGTH_DY, ARP_LENGTH_W, CELL_H)) {
		return ARP_LENGTH;
	}
	if (count && in_box(x, y, bx + ARP_RIGHT_DX, by + ARP_STRIP_DY, ARP_LENGTH_W, ARP_STRIP_H)) {
		return ARP_ON_OFF;
	}
	if (y >= by + ARP_STRIP_DY && y < by + ARP_STRIP_DY + ARP_STRIP_H) {
		if (!count) {
			return ARP_NONE;
		}
		if (row->loop >= 0) {
			const int ls = arp_x(g, lx, count, row->loop) - 1;
			return x >= ls - 9 && x <= ls ? ARP_TAB : x > ls && x < end ? ARP_BODY : ARP_NONE;
		}
		if (row->shadow_kept) {
			return x >= arp_x(g, lx, count, row->shadow) - 10 && x < end ? ARP_SHADOW : ARP_NONE;
		}
		return x >= lx && x < lx + ENV_LOOP_OFF_W ? ARP_SWITCH : ARP_NONE;
	}
	return x >= lx && x < end && y >= ly && y < ly + ARP_LANE_H ? ARP_LANE : ARP_NONE;
}

// The ARP switch of the FM page under the mouse.
static bool over_arp_switch(const mdmi_gui *g, int x, int y)
{
	return *g->source.page == PAGE_EDITOR && takes_fm(patch_view(g->source.patch), *g->source.channel) && in_box(x, y, ARP_SWITCH_X, GRAPH_Y, ARP_SWITCH_W, CELL_H);
}

// The row (arpeggio.md, 1): the scale, a column per step sharing the lane by the row's length, bars
// from the zero line, the loop's strip with one tab at its start, the length at the right.
static void draw_arp(mdmi_gui *g, const patch_values *values, int bx, int by)
{
	const int channel = *g->source.channel;
	const bool fm = channel_is_fm(channel), dim = !patch_arp_works(values, channel);
	const uint32_t ink = dim ? C_LINE : fm ? C_FM_LIGHT : C_PSG, dark = dim ? C_CELL : fm ? C_FM_DARK : C_PSG_DARK;
	const patch_arp *row = shown_arp(g);
	const int count = row->count, lx = bx + ARP_LANE_DX, ly = by + ARP_LANE_DY, rx = bx + ARP_RIGHT_DX, end = lx + ARP_LANE_W;
	const int sy = by + ARP_STRIP_DY;
	const bool idle = g->arp_drag == ARP_NONE;
	const int over = idle && !g->dragging && !g->file_over ? arp_part_at(g, row, g->mouse_x, g->mouse_y) : ARP_NONE;
	const int hover = over == ARP_LANE && count ? lane_column_at(lx, ARP_LANE_W, count, g->mouse_x) : -1;
	const int lit = hover >= 0 ? hover : g->arp_drag == ARP_LANE || g->arp_drag == ARP_ZERO ? g->arp_last_step : -1;
	// Switched off (D55): dim but alive, every part takes the mouse as always. A row dim because the
	// channel can't play it stays that.
	const bool off = !dim && count && row->off;
	char buffer[16];

	fill(g, bx, by, ARP_W, ARP_H, C_BG);
	frame(g, bx, by, ARP_W, ARP_H, C_CELL);
	static const struct {
		const char *label;
		int y;
	} SCALE[] = {{"+24", 0}, {"+12", 22}, {"0", 46}, {"-12", 70}, {"-24", 91}};
	for (size_t i = 0; i < sizeof(SCALE) / sizeof(*SCALE); i++) {
		text_right(g, bx + 20, ly + SCALE[i].y, SCALE[i].label, dim ? C_LINE : C_HINT, 1, 20);
	}

	// The ground: the loop lighter, the column under the mouse lighter still, the octaves' lines.
	for (int i = 0; i < count; i++) {
		const int x = arp_x(g, lx, count, i), w = arp_x(g, lx, count, i + 1) - x;
		const bool in_loop = !off && row->loop >= 0 && i >= row->loop;
		fill(g, x, ly, w, ARP_LANE_H, off ? (i == lit ? C_CELL : C_BG) : i == hover ? C_LINE : in_loop ? C_CELL : C_BG);
		fill(g, x, ly + 24, w, 1, in_loop || i == hover ? C_LINE : C_CELL);
		fill(g, x, ly + 72, w, 1, in_loop || i == hover ? C_LINE : C_CELL);
	}
	if (!count) {
		// Empty: the ghost of four columns where a drawing lands.
		fill(g, lx, ly + 24, ARP_LANE_W, 1, C_CELL);
		fill(g, lx, ly + 72, ARP_LANE_W, 1, C_CELL);
		for (int k = 1; k < 4; k++) {
			for (int y = ly; y < ly + ARP_LANE_H; y += 2) {
				fill(g, lx + k * ARP_LANE_W / 4, y, 1, 1, C_CELL);
			}
		}
		text(g, lx + (ARP_LANE_W - 65) / 2, ly + 30, "DRAW AN ARP", dim ? C_LINE : C_HINT, 1);
	}
	// The zero line reads stronger than the octaves'.
	fill(g, lx, ly + 48, ARP_LANE_W, 2, C_LINE);
	for (int i = 0; i < count; i++) {
		const int x = arp_x(g, lx, count, i), w = arp_x(g, lx, count, i + 1) - x;
		const int shift = row->steps[i], top = arp_y(ly, shift);
		if (shift > 0) {
			fill(g, x, top + 2, w - 1, ly + 48 - top - 2, off ? C_CELL : dark);
		} else if (shift < 0) {
			fill(g, x, ly + 50, w - 1, top - ly - 50, off ? C_CELL : dark);
		}
		// A step of 0 is a real step: its top on the zero line.
		fill(g, x, top, w - 1, 2, off ? (i == lit ? C_LABEL : C_HINT) : ink);
		if (w > 19) {
			shift_text(shift, buffer, sizeof(buffer));
			text(g, x + (w - 1 - ((int)strlen(buffer) * 6 - 1)) / 2, by + 114, buffer, dim ? C_LINE : i == lit ? C_TEXT : off ? C_HINT : C_LABEL, 1);
		}
	}

	// The loop's strip: one tab at the start, the loop runs to the last step.
	if (count && row->loop >= 0) {
		const int ls = arp_x(g, lx, count, row->loop) - 1;
		const bool moved = g->arp_drag == ARP_BODY && g->arp_moved, held = moved || g->arp_drag == ARP_TAB;
		const uint32_t loop_ink = off ? C_HINT : ink;
		fill(g, ls + 1, sy, end - ls - 1, ARP_STRIP_H, over == ARP_BODY ? C_LINE : C_CELL);
		fit_word(g, ls + 1, end - ls - 1, sy + 3, "LOOP", NULL, true, moved ? C_AMBER : loop_ink, true);
		fill(g, ls, sy, 1, ly + ARP_LANE_H - sy, held ? C_AMBER : loop_ink);
		fill(g, ls - 8, sy, 8, ARP_STRIP_H, held ? C_AMBER : over == ARP_TAB ? C_TEXT : loop_ink);
	} else if (count && row->shadow_kept) {
		// The loop switched off: an outline where it started, no tab, no line.
		const int ss = arp_x(g, lx, count, row->shadow) - 1;
		const bool moved = g->arp_drag == ARP_SHADOW && g->arp_moved;
		const uint32_t color = moved ? C_AMBER : over == ARP_SHADOW ? C_TEXT : C_LINE;
		frame(g, ss, sy, end - ss, ARP_STRIP_H, color);
		fit_word(g, ss + 1, end - ss - 2, sy + 3, "LOOP", NULL, true, moved ? C_AMBER : over == ARP_SHADOW ? C_TEXT : dim ? C_LINE : C_LABEL, true);
	} else if (count) {
		frame(g, lx, sy, ENV_LOOP_OFF_W, ARP_STRIP_H, over == ARP_SWITCH ? C_TEXT : C_LINE);
		text(g, lx + 3, sy + 3, "LOOP", over == ARP_SWITCH ? C_TEXT : dim ? C_LINE : C_LABEL, 1);
		text(g, lx + 44, sy + 3, "ONCE, THEN HOLDS", C_HINT, 1);
	}

	// The column at the right: the row's switch, or ARP when it is empty; the length, dragged like the
	// envelope's, and what it means.
	if (count) {
		const bool on = !row->off, lit_switch = over == ARP_ON_OFF;
		fill(g, rx, sy, ARP_LENGTH_W, ARP_STRIP_H, on && !dim ? dark : C_BG);
		frame(g, rx, sy, ARP_LENGTH_W, ARP_STRIP_H, dim ? C_CELL : lit_switch ? C_TEXT : on ? ink : C_LINE);
		text(g, rx + 3, by + 5, on ? "ARP ON" : "ARP OFF", dim ? C_LINE : on ? C_TEXT : C_LABEL, 1);
	} else {
		text(g, rx, by + 4, "ARP", dim ? C_LINE : C_LABEL, 1);
	}
	const bool held = g->arp_drag == ARP_LENGTH, full = count >= PATCH_ARP_STEPS;
	const int cy = by + ARP_LENGTH_DY;
	fill(g, rx, cy, ARP_LENGTH_W, CELL_H, dim ? C_BG : held ? C_LINE : count ? dark : C_CELL);
	if (dim || held || over == ARP_LENGTH) {
		frame(g, rx, cy, ARP_LENGTH_W, CELL_H, dim ? C_CELL : held ? C_TEXT : C_LINE);
	}
	snprintf(buffer, sizeof(buffer), "%d", count);
	text_right(g, rx + 48, cy + 3, buffer, dim ? C_LINE : full ? C_AMBER : count ? C_TEXT : C_LABEL, 2, ARP_LENGTH_W - 4);
	text(g, rx, by + 39, full ? "MAX" : count ? "STEPS" : "NO ARP", full && !dim ? C_AMBER : C_HINT, 1);
	if (count) {
		snprintf(buffer, sizeof(buffer), "%d MS", frame_ms(count));
		text(g, rx, by + 51, buffer, dim ? C_LINE : C_LABEL, 1);
	}
	text(g, rx, by + 100, "CONSOLE:", C_HINT, 1);
	text(g, rx, by + 110, "UNTESTED", C_HINT, 1);

	// The value by the mouse: over the top at 0 and up, under it below 0, the other side if no room.
	if (!dim && lit >= 0 && lit < count) {
		const int shift = row->steps[lit], top = arp_y(ly, shift);
		int y = shift >= 0 ? top - 15 : top + 5;
		if (y < ly || y + 13 > ly + ARP_LANE_H) {
			y = shift >= 0 ? top + 5 : top - 15;
		}
		shift_text(shift, buffer, sizeof(buffer));
		value_tag(g, (arp_x(g, lx, count, lit) + arp_x(g, lx, count, lit + 1)) / 2, y, buffer, lx, end);
	}
}

// The FM page's switch between the graph and the row, with the row's length.
static void draw_arp_switch(mdmi_gui *g, const patch_values *values)
{
	const patch_arp *row = patch_arp_view(g->source.patch, *g->source.channel);
	const int channel = *g->source.channel, count = row->count;
	const bool over = !g->dragging && !g->file_over && g->arp_drag == ARP_NONE && over_arp_switch(g, g->mouse_x, g->mouse_y);
	fill(g, ARP_SWITCH_X, GRAPH_Y, ARP_SWITCH_W, CELL_H, g->arp_view ? C_FM : C_BG);
	if (over || !g->arp_view) {
		frame(g, ARP_SWITCH_X, GRAPH_Y, ARP_SWITCH_W, CELL_H, over ? C_TEXT : C_LINE);
	}
	text(g, ARP_SWITCH_X + 4, GRAPH_Y + 6, "ARP", g->arp_view ? on_fill(g) : C_LABEL, 1);
	char buffer[8];
	snprintf(buffer, sizeof(buffer), count ? "%d" : "-", count);
	text_right(g, ARP_SWITCH_X + 48, GRAPH_Y + 3, buffer, !count || row->off || !patch_arp_works(values, channel) ? C_LINE : g->arp_view ? on_fill(g) : C_TEXT, 2, 26);
}

// The channel's row becomes the window's copy, and what undo puts back.
static void arp_take(mdmi_gui *g)
{
	g->arp_channel = *g->source.channel;
	g->arp_before = *patch_arp_view(g->source.patch, g->arp_channel);
	g->arp_row = g->arp_start = g->arp_before;
	g->arp_changed = g->arp_handed = false;
	g->arp_last_step = -1;
}

// The copy goes to the model as a drawn envelope does: at most once per window frame and at the
// end of the gesture, the host told nothing (D19).
static void arp_hand(mdmi_gui *g)
{
	if (!g->arp_changed) {
		return;
	}
	patch_arp row = g->arp_row;
	patch_arp_clean(&row);
	files_draw_arp(g->source.files, g->arp_channel, g->arp_handed ? NULL : &g->arp_before, &row);
	g->arp_handed = true;
	g->arp_changed = false;
	if (!g->arp_told) {
		say(g, "Drawn arp plays from the next note", C_TEXT, MESSAGE_TICKS);
		g->arp_told = true;
	}
}

// An empty row starts as 4 steps of 0 looped from the first, its shadow gone (arpeggio.md, 2).
static void arp_begin(patch_arp *row)
{
	*row = PATCH_ARP_EMPTY;
	row->count = 4;
	row->loop = 0;
}

static void arp_move(mdmi_gui *g, int x, int y)
{
	int bx, by;
	arp_corner(g, &bx, &by);
	const int lx = bx + ARP_LANE_DX, ly = by + ARP_LANE_DY;
	patch_arp *row = &g->arp_row;
	const patch_arp was = *row;
	if (g->arp_drag == ARP_LANE) {
		if (!row->count) {
			arp_begin(row);
		}
		// Whole semitones by y, above the lane +24 and below it -24; jumped columns on the line between.
		const int step = clamp(lane_column_at(lx, ARP_LANE_W, row->count, x), 0, row->count - 1);
		const int value = clamp(PATCH_ARP_SHIFT - (y - ly) / 2, -PATCH_ARP_SHIFT, PATCH_ARP_SHIFT);
		const int from = g->arp_last_step < 0 ? step : g->arp_last_step;
		const int last = g->arp_last_step < 0 ? value : g->arp_last_value;
		const int direction = step >= from ? 1 : -1;
		for (int i = from;; i += direction) {
			row->steps[i] = (int8_t)(step == from ? value : last + (value - last) * (i - from) / (step - from));
			if (i == step) {
				break;
			}
		}
		g->arp_last_step = step;
		g->arp_last_value = value;
	} else if (g->arp_drag == ARP_TAB) {
		row->loop = (int8_t)clamp(lane_boundary_at(lx, ARP_LANE_W, row->count, x), 0, row->count - 1);
	} else if (g->arp_drag == ARP_BODY || g->arp_drag == ARP_SHADOW) {
		// Held (18b): past DRAG_SLOP the loop's start, or the shadow, follows the mouse by boundaries;
		// before, nothing. The loop ends at the last step, so its length changes.
		g->arp_moved = g->arp_moved || abs(x - g->arp_start_x) >= DRAG_SLOP;
		if (g->arp_moved) {
			long start = g->arp_drag == ARP_BODY ? g->arp_start.loop : g->arp_start.shadow, end = start + 1;
			shift_span(&start, &end, lane_boundary_at(lx, ARP_LANE_W, row->count, x) - lane_boundary_at(lx, ARP_LANE_W, row->count, g->arp_start_x), 0, row->count);
			if (g->arp_drag == ARP_BODY) {
				row->loop = (int8_t)start;
			} else {
				row->shadow = (uint8_t)start;
			}
		}
	} else if (g->arp_drag == ARP_LENGTH) {
		// Up is longer, from the gesture's start, so steps cut on the way come back; new steps copy
		// the last. From empty the row starts looped from the first step, as a drawing does.
		const int dy = g->arp_start_y - y;
		*row = g->arp_start;
		const int length = clamp(row->count + (abs(dy) < DRAG_SLOP ? 0 : dy / ARP_LENGTH_PIXELS), 0, PATCH_ARP_STEPS);
		if (!row->count && length) {
			arp_begin(row);
			row->count = 0;
		}
		for (; row->count < length; row->count++) {
			row->steps[row->count] = row->count ? row->steps[row->count - 1] : 0;
		}
		row->count = (uint8_t)length;
		patch_arp_clean(row);
	}
	g->arp_changed |= memcmp(&was, row, sizeof(was)) != 0;
}

// A press on the row starts a gesture; on the strip's body, a shadow or the switch it changes
// nothing until it moves or is let go (18b).
static bool arp_down(mdmi_gui *g, int x, int y)
{
	const int part = arp_part_at(g, shown_arp(g), x, y);
	if (part == ARP_NONE) {
		return false;
	}
	arp_take(g);
	g->arp_drag = part;
	g->arp_start_x = x;
	g->arp_start_y = y;
	g->arp_moved = false;
	if (g->window) {
		SetCapture(g->window);
	}
	arp_move(g, x, y);
	return true;
}

// A double click on a column: 0, in the undo step of the click before it, so undo gives back what
// was there before the first press. False off a column.
static bool arp_zero(mdmi_gui *g, int x, int y)
{
	const patch_arp *row = shown_arp(g);
	if (arp_part_at(g, row, x, y) != ARP_LANE || !row->count) {
		return false;
	}
	const bool again = g->arp_handed && g->arp_channel == *g->source.channel;
	const patch_arp before = g->arp_before;
	arp_take(g);
	if (again) {
		g->arp_before = before;
		g->arp_handed = true;
	}
	int bx, by;
	arp_corner(g, &bx, &by);
	const int step = clamp(lane_column_at(bx + ARP_LANE_DX, ARP_LANE_W, g->arp_row.count, x), 0, g->arp_row.count - 1);
	g->arp_changed = g->arp_row.steps[step] != 0;
	g->arp_row.steps[step] = 0;
	g->arp_last_step = step;
	g->arp_drag = ARP_ZERO;
	if (g->window) {
		SetCapture(g->window);
	}
	return true;
}

// The mouse let go: a click within DRAG_SLOP on the body turns the loop off and keeps its shadow,
// on the shadow turns it on there, on the loop's switch loops the whole row; the row's switch
// turns the row off or on.
static void arp_up(mdmi_gui *g)
{
	patch_arp *row = &g->arp_row;
	if (!g->arp_moved && g->arp_drag == ARP_BODY) {
		row->shadow_kept = 1;
		row->shadow = (uint8_t)row->loop;
		row->loop = -1;
		g->arp_changed = true;
	} else if (!g->arp_moved && g->arp_drag == ARP_SHADOW) {
		row->loop = (int8_t)row->shadow;
		row->shadow_kept = 0;
		g->arp_changed = true;
	} else if (g->arp_drag == ARP_SWITCH) {
		row->loop = 0;
		g->arp_changed = true;
	} else if (g->arp_drag == ARP_ON_OFF) {
		// The row's switch (D55): its steps stay, one undo step.
		row->off = row->off ? 0 : 1;
		g->arp_changed = true;
	}
	arp_hand(g);
	g->arp_drag = ARP_NONE;
	if (GetCapture() == g->window) {
		ReleaseCapture();
	}
}

// What the footer says about the row and the FM page's switch: the gesture under way, or what is
// under the mouse.
static bool arp_hint(mdmi_gui *g, char *hint, size_t size)
{
	const int channel = *g->source.channel;
	const int x = g->mouse_x, y = g->mouse_y;
	if (g->arp_drag == ARP_NONE && over_arp_switch(g, x, y)) {
		const patch_arp *view = patch_arp_view(g->source.patch, channel);
		const int count = view->count;
		if (g->arp_view) {
			snprintf(hint, size, "CLICK: BACK TO THE ENVELOPE GRAPH");
		} else {
			snprintf(hint, size, !count ? "NO ARP. CLICK TO DRAW ONE" : view->off ? "ARP %d STEPS, OFF. CLICK TO EDIT" : "ARP %d STEPS. CLICK TO EDIT", count);
		}
		return true;
	}
	if (!arp_shown(g)) {
		return false;
	}
	int bx, by;
	arp_corner(g, &bx, &by);
	if (!patch_arp_works(patch_view(g->source.patch), channel)) {
		if (!in_box(x, y, bx, by, ARP_W, ARP_H)) {
			return false;
		}
		snprintf(hint, size, channel == 2 ? "OPS MODE: ARP DOES NOTHING ON CH3" : "CH10: ARP WORKS ONLY WITH RATE TONE 3");
		return true;
	}
	const patch_arp *row = shown_arp(g);
	const int count = row->count;
	const bool held = g->arp_drag != ARP_NONE;
	const int part = held ? g->arp_drag : arp_part_at(g, row, x, y);
	char shift[8];
	if (part == ARP_LANE || part == ARP_ZERO) {
		const int step = held ? g->arp_last_step : lane_column_at(bx + ARP_LANE_DX, ARP_LANE_W, count, x);
		if (!count) {
			snprintf(hint, size, "NO ARP: DRAW HERE FOR 4 LOOPED STEPS");
			return true;
		}
		if (step < 0 || step >= count) {
			return false;
		}
		shift_text(row->steps[step], shift, sizeof(shift));
		if (held) {
			snprintf(hint, size, row->off ? "STEP %d/%d: %s SEMITONES, ARP OFF" : "STEP %d/%d: %s SEMITONES", step + 1, count, shift);
		} else if (row->off) {
			snprintf(hint, size, "STEP %d/%d: %s, ARP OFF. DOUBLE CLICK: 0", step + 1, count, shift);
		} else {
			snprintf(hint, size, "STEP %d/%d: %s AT %d MS. DOUBLE CLICK: 0", step + 1, count, shift, frame_ms(step));
		}
	} else if (part == ARP_ON_OFF && count) {
		snprintf(hint, size, row->off ? "ARP OFF: PLAYS AS NO ARP. CLICK: ON" : "ARP ON. CLICK: OFF, STEPS STAY HERE");
	} else if (part == ARP_LENGTH && held) {
		snprintf(hint, size, count ? "ARP %d STEPS, %d MS" : "0 STEPS: NO ARP", count, frame_ms(count));
	} else if (part == ARP_LENGTH) {
		snprintf(hint, size, !count ? "NO ARP: DRAG UP TO ADD STEPS" : count >= PATCH_ARP_STEPS ? "MAX %d STEPS, %d MS: DRAG DOWN" : "ARP %d STEPS, %d MS: DRAG UP OR DOWN", count, frame_ms(count));
	} else if (part == ARP_TAB && row->loop >= 0) {
		if (held) {
			snprintf(hint, size, "LOOP %d-%d: %d STEPS, %d MS", row->loop + 1, count, count - row->loop, frame_ms(count - row->loop));
		} else {
			snprintf(hint, size, "LOOP FROM STEP %d: DRAG BETWEEN STEPS", row->loop + 1);
		}
	} else if (part == ARP_BODY && row->loop >= 0) {
		snprintf(hint, size, held && g->arp_moved ? "LOOP %d-%d: RELEASE TO KEEP" : "LOOP %d-%d: CLICK OFF, DRAG TO MOVE", row->loop + 1, count);
	} else if (part == ARP_SHADOW && row->shadow_kept) {
		snprintf(hint, size, "LOOP %d-%d IS OFF: CLICK TO TURN IT ON", row->shadow + 1, count);
	} else if (part == ARP_SWITCH) {
		snprintf(hint, size, "ONCE, HOLDS LAST STEP. CLICK: LOOP ALL");
	} else {
		return false;
	}
	return true;
}

static void replace_sample(mdmi_gui *g, int note);

// Channel 3's pitch tag of an operator in its header (stage 21): what it plays against the key, or
// the note it stays at with Fixed, lit then. Dim outside Ch3 Special.
static void draw_pitch_tag(mdmi_gui *g, const patch_values *values, int op)
{
	const int x = GRID_X[op] + PITCH_TAG_X, y = SSG_BUTTON_Y;
	const bool dim = !is_special(values, 2);
	const bool fixed = values->globals[ch3_field(op, PATCH_CH3_FIXED)] != 0;
	const bool zero = values->globals[ch3_field(op, PATCH_CH3_SEMI)] == PATCH_CH3_SEMI_MAX && values->globals[ch3_field(op, PATCH_CH3_FINE)] == PATCH_CH3_FINE_MAX;
	char buffer[16];
	ch3_pitch_text(values, op, buffer, sizeof(buffer));
	uint32_t ground = C_BG, ink = zero ? C_HINT : C_TEXT;
	if (fixed) {
		ground = dim ? C_CELL : C_FM_LIGHT;
		ink = dim ? C_HINT : C_BG;
	} else if (dim) {
		ink = C_LINE;
	}
	fill(g, x, y, PITCH_TAG_W, SSG_BUTTON_H, ground);
	const int width = (int)strlen(buffer) * 6 - 1;
	text(g, x + (PITCH_TAG_W - width) / 2, y + (SSG_BUTTON_H - 7) / 2, buffer, ink, 1);
}

// The open pitch editor over its column's rows, its cells drawn over it (stage 21).
static void draw_pitch_editor(mdmi_gui *g, const patch_values *values)
{
	const int op = g->pitch_open, x = GRID_X[op];
	fill(g, x - 1, PITCH_EDIT_Y, OP_W + 2, PITCH_EDIT_H, C_BG);
	frame(g, x - 1, PITCH_EDIT_Y, OP_W + 2, PITCH_EDIT_H, C_LABEL);
	for (int i = 0; i < g->cell_count; i++) {
		if (is_ch3_op_global(g->cells[i].id) && g->cells[i].kind == CELL_ROW) {
			draw_cell(g, &g->cells[i], values);
		}
	}
	// What FIX gives: the note it stays at, or that the operator follows the key.
	if (value_of(g, values, (uint32_t)ch3_field(op, PATCH_CH3_FIXED))) {
		char buffer[8];
		note_name(ch3_fixed_key(values, op), buffer, sizeof(buffer));
		text_right(g, x + 108, PITCH_FIX_Y + (OP_ROW_H - 14) / 2, buffer, C_TEXT, 2, 54);
	} else {
		text(g, x + 54 + (58 - 47) / 2, PITCH_FIX_Y + (OP_ROW_H - 7) / 2, "FROM KEY", C_HINT, 1);
	}
}

// The strip's cells that take no mouse while they mean nothing (stage 21): LFO RESET with the LFO
// off, CH3 NOTES without Ch3 Special. Their values stay and show; the label of CH3 NOTES always.
static void draw_globals_dim(mdmi_gui *g, const patch_values *values)
{
	const int y = GLOBALS_Y + 6;
	char buffer[16];
	if (!values->globals[GLOBAL_LFO_ON]) {
		fill(g, LFO_RESET_X, y, LFO_RESET_W, 32, C_BG);
		frame(g, LFO_RESET_X, y, LFO_RESET_W, 32, C_CELL);
		text(g, LFO_RESET_X + 4, y + 4, "LFO RESET", C_LINE, 1);
		patch_format(patch_bank_view(g->source.patch), GLOBAL_LFO_RESET, values->globals[GLOBAL_LFO_RESET], buffer, sizeof(buffer));
		text(g, LFO_RESET_X + 4, y + 14, buffer, C_HINT, 2);
	}
	const bool dim = !values->globals[GLOBAL_CH3_MODE];
	fill(g, CH3_NOTES_X, y, CH3_NOTES_W, CH3_NOTES_LABEL_H, C_CELL);
	text(g, CH3_NOTES_X + 4, y + 4, "CH3 NOTES", dim ? C_LINE : C_LABEL, 1);
	if (!dim) {
		return;
	}
	for (int i = 0; i < CH3_NOTES_MODES; i++) {
		const bool on = values->globals[GLOBAL_CH3_NOTES] == i;
		const int width = (int)strlen(CH3_NOTES_SEGMENTS[i].label) * 6 - 1;
		fill(g, CH3_NOTES_SEGMENTS[i].x, CH3_NOTES_SEGMENT_Y, CH3_NOTES_SEGMENTS[i].w, CH3_NOTES_SEGMENT_H, C_CELL);
		text(g, CH3_NOTES_SEGMENTS[i].x + (CH3_NOTES_SEGMENTS[i].w - width) / 2, CH3_NOTES_SEGMENT_Y + 6, CH3_NOTES_SEGMENTS[i].label, on ? C_HINT : C_LINE, 1);
	}
}

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
		// PSG has no pan: its place has the envelope's speed (stage 44, a cell). Nor MONO (stage 38): a PSG channel plays one note anyway; its place dim like PAN's.
		frame(g, MONO_X, GRID_ROW1_Y, MONO_W, CELL_H, C_CELL);
		text(g, MONO_X + (MONO_W - 23) / 2, GRID_ROW1_Y + 6, "MONO", C_LINE, 1);
		// The row reaches the right edge (stage 24d): the chip's caveats moved, the noise's to the footer.
		if (channel == PATCH_NUM_CHANNELS - 1) {
			const bool kit = values->globals[GLOBAL_NOISE_RATE] == PATCH_NOISE_RATE_KIT;
			fill(g, 242, NOISE_ROW_Y, 32, CELL_H, C_CELL);
			text(g, 246, NOISE_ROW_Y + 6, "TYPE", kit ? C_LINE : C_LABEL, 1);
			fill(g, 402, NOISE_ROW_Y, 32, CELL_H, C_CELL);
			text(g, 406, NOISE_ROW_Y + 6, "RATE", C_LABEL, 1);
			if (noise_without_pitch(values, channel)) {
				// The places of TUNE, BEND, PORTA and TIME, dim like PAN's: no pitch off Tone 3.
				static const struct {
					const char *label;
					int x, w;
				} NO_PITCH[] = {{"TUNE", 422, 74}, {"BEND", 500, 62}, {"PORTA", 598, 36}, {"TIME", 638, 64}};
				for (size_t i = 0; i < sizeof(NO_PITCH) / sizeof(*NO_PITCH); i++) {
					fill(g, NO_PITCH[i].x, GRID_ROW1_Y, NO_PITCH[i].w, CELL_H, C_BG);
					frame(g, NO_PITCH[i].x, GRID_ROW1_Y, NO_PITCH[i].w, CELL_H, C_CELL);
					text(g, NO_PITCH[i].x + 4, GRID_ROW1_Y + 6, NO_PITCH[i].label, C_LINE, 1);
				}
			}
		} else if (noise_takes(values, channel)) {
			text(g, 242, 88, "TAKEN BY CH10 NOISE, TONE 3 OR KIT: CH9 NOTES ARE SILENT", C_AMBER, 1);
		}
		draw_envelope(g, values);
		draw_arp(g, values, PAGE_X, ARP_PSG_Y);
		return;
	}
	const int algorithm = value_of(g, values, PATCH_CHANNEL_ID(channel, PARAM_ALGORITHM));
	draw_diagram(g, algorithm, value_of(g, values, PATCH_CHANNEL_ID(channel, PARAM_FEEDBACK)));
	const int active = active_op(g);
	if (g->arp_view) {
		draw_arp(g, values, GRAPH_X, GRAPH_Y);
	} else {
		draw_graph(g, values, channel, active);
	}
	draw_arp_switch(g, values);
	for (int op = 0; op < PATCH_OPS; op++) {
		const int x = GRID_X[op];
		const bool carrier = CARRIERS[algorithm] >> op & 1;
		// Channel 3's header gives its word to the pitch tag (stage 21): the carrier shows by its fill.
		if (channel == 2) {
			snprintf(buffer, sizeof(buffer), "OP%d", op + 1);
		} else {
			snprintf(buffer, sizeof(buffer), "OP%d %s", op + 1, carrier ? "CARRIER" : "MOD");
		}
		// The active operator's header framed white; no frame round the column (after 0.11.0).
		if (carrier) {
			fill(g, x, OP_HEADER_Y, OP_W, OP_HEADER_H, C_FM);
		} else {
			frame(g, x, OP_HEADER_Y, OP_W, OP_HEADER_H, C_FM_LIGHT);
		}
		if (op == active) {
			frame(g, x, OP_HEADER_Y, OP_W, OP_HEADER_H, C_TEXT);
		}
		text(g, x + 4, OP_HEADER_Y + 4, buffer, carrier ? on_fill(g) : op == active ? C_TEXT : C_FM_LIGHT, 1);
		// Channel 3's pitch tags only in Ch3 Special (stage 22b): off, its header is as the others'.
		if (channel == 2 && values->globals[GLOBAL_CH3_MODE]) {
			draw_pitch_tag(g, values, op);
		}
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
	// So does the pitch editor (stage 21).
	const bool in_pitch = in_pitch_editor(g, g->mouse_x, g->mouse_y);
	for (int i = 0; i < g->cell_count; i++) {
		if (in_choice && g->cells[i].look != LOOK_SHAPE) {
			continue;
		}
		if (in_pitch && !(is_ch3_op_global(g->cells[i].id) && g->cells[i].kind == CELL_ROW)) {
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

// STORE is there on EDITOR of a channel with a sound of its own: FM 1-6 and PSG 7-10, not the DAC.
static bool shows_store(const mdmi_gui *g)
{
	return *g->source.page == PAGE_EDITOR && takes_files(patch_view(g->source.patch), *g->source.channel);
}

// ADD is there only on the bank page.
static int action_at(const mdmi_gui *g, int x, int y)
{
	for (int action = 0; action < ACTIONS; action++) {
		if ((action == ACTION_ADD && *g->source.page != PAGE_BANK) || (action == ACTION_STORE && !shows_store(g))
			|| (action == ACTION_EXPORT && *g->source.page != PAGE_ROM)) {
			continue;
		}
		if (y >= BUTTON_Y && y < BUTTON_Y + BUTTON_H && x >= ACTION_X[action] && x < ACTION_X[action] + ACTION_WIDTH[action]) {
			return action;
		}
	}
	return -1;
}

// What a press of the button under the mouse would do, for the footer.
static bool tv_hint(mdmi_gui *g, char *hint, size_t size);

// Stage 38 (art-director, ui-concept.md): over MONO and PORTA of the channel's row, what the pair does.
static bool mono_hint(mdmi_gui *g, char *hint, size_t size)
{
	if (g->file_over || g->dragging || *g->source.page == PAGE_ALL || *g->source.page == PAGE_ROM || *g->source.page == PAGE_BANK
		|| g->mouse_y < GRID_ROW1_Y || g->mouse_y >= GRID_ROW1_Y + CELL_H) {
		return false;
	}
	const patch_values *values = patch_view(g->source.patch);
	const int channel = *g->source.channel;
	const bool fm = channel_is_fm(channel);
	const bool over_mono = g->mouse_x >= MONO_X && g->mouse_x < MONO_X + MONO_W;
	const bool over_porta = g->mouse_x >= 598 && g->mouse_x < 634;
	if (is_dac(values, channel) || (!over_mono && !over_porta)) {
		return false;
	}
	const bool mono = fm && values->globals[GLOBAL_MONO(channel)], porta = values->channels[channel][PARAM_PORTA_ON];
	const char *text = NULL;
	if (over_mono && !fm) {
		text = "MONO: FM ONLY. PSG PLAYS ONE NOTE ANYWAY";
	} else if (over_mono && channel == 2 && is_special(values, 2)) {
		text = "CH3 SPECIAL: MONO DOES NOTHING ON CH3";
	} else if (over_mono) {
		text = mono && porta ? "PORTA WINS: GLIDES. CLICK: MONO, NO GLIDE" : mono ? "MONO ON: EVERY NOTE ATTACKS. CLICK: OFF" : "MONO OFF. CLICK: ONE VOICE, NEW ATTACK";
	} else if (fm) {
		text = porta && mono ? "PORTA WINS OVER MONO. CLICK: MONO ONLY" : porta ? "PORTA ON: NOTES GLIDE. CLICK: OFF" : "PORTA OFF. CLICK: ONE VOICE, NOTES GLIDE";
	} else {
		return false;
	}
	snprintf(hint, size, "%s", text);
	return true;
}

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
	} else if (action == ACTION_STORE) {
		const int place = (values->channels[channel][PARAM_PRESET] & (PATCH_BANK_SIZE - 1)) + 1;
		if (!files_can_store(g->source.files, channel)) {
			snprintf(hint, size, "STORE: nothing, CH%d is bank %d as is", channel + 1, place);
		} else {
			snprintf(hint, size, patch_arp_view(g->source.patch, channel)->count ? "STORE: CH%d and its arp into bank %d" : "STORE: CH%d into bank %d", channel + 1, place);
		}
	} else if (action == ACTION_SAVE && takes_files(values, channel) && patch_arp_view(g->source.patch, channel)->count) {
		// Files have no row (D44): the footer says so before it is lost.
		snprintf(hint, size, envelopes ? "SAVE: CH%d as EEF w/o arp, bank, snapshot" : "SAVE: CH%d as VGI w/o arp, bank, snapshot", channel + 1);
	} else if (action == ACTION_SAVE) {
		snprintf(hint, size, envelopes ? "SAVE: CH%d as EEF, the bank or a snapshot" : takes_fm(values, channel) ? "SAVE: CH%d as VGI, the bank or a snapshot" : "SAVE: the bank or a snapshot", channel + 1);
	} else if (action == ACTION_UNDO && (undo || files_redo_text(g->source.files))) {
		const char *redo = files_redo_text(g->source.files);
		if (undo && redo) {
			snprintf(hint, size, "UNDO: %.36s. SHIFT: %.36s", undo, redo);
		} else if (undo) {
			snprintf(hint, size, "UNDO: %s", undo);
		} else {
			snprintf(hint, size, "SHIFT+CLICK: REDO %s", redo);
		}
	} else if (action == ACTION_EXPORT) {
		snprintf(hint, size, "EXPORT: bank, DAC, names to a ROM for SD");
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
	// Stage 22b (D57): channel 3's AR is the ROM's 31 in CSM; the voice keeps its own for later.
	if (csm_ar(values, c->id)) {
		char kept[24];
		patch_format(patch_bank_view(g->source.patch), c->id, value_of(g, values, c->id), kept, sizeof(kept));
		snprintf(hint, size, "AR 31 IN CSM, THE VOICE KEEPS %s", kept);
		return true;
	}
	const int base = patch_base_value(values, channel, param), value = value_of(g, values, c->id);
	if (base < 0 || base == value) {
		return false;
	}
	const patch_bank *bank = patch_bank_view(g->source.patch);
	const char *from = plays_own_file(g, values, channel) ? "FILE" : "ROM";
	char now[24], was[24];
	patch_format(bank, c->id, value, now, sizeof(now));
	patch_format(bank, c->id, base, was, sizeof(was));
	snprintf(hint, size, "%s %s, %s %s", c->kind == CELL_CHOICE ? "ALG" : c->label, now, from, was);
	return true;
}

// Stage 24d: channel 10's TYPE and RATE row under the mouse, what was the chip's line beside the arp row.
static bool noise_hint(mdmi_gui *g, char *hint, size_t size)
{
	if (g->file_over || *g->source.page != PAGE_EDITOR || *g->source.channel != PATCH_NUM_CHANNELS - 1) {
		return false;
	}
	if (g->mouse_x < 242 || g->mouse_x >= PAGE_RIGHT || g->mouse_y < NOISE_ROW_Y || g->mouse_y >= NOISE_ROW_Y + CELL_H) {
		return false;
	}
	snprintf(hint, size, "TYPE AND RATE: NOT CHECKED ON CONSOLE");
	return true;
}

// Master Volume or Retrigger under the mouse (stage 49): say that the console has no such knob. The
// knob's id, or 0.
static uint32_t plugin_only_hint(mdmi_gui *g)
{
	const int over = g->file_over ? -1 : hovered(g);
	const uint32_t id = over >= 0 ? g->cells[over].id : 0;
	return id == GLOBAL_MASTER || id == GLOBAL_RETRIGGER ? id : 0;
}

// A rail row with cut notes under the mouse (stage 24b): why no voice was free. The voices that never
// lend are named by cause (D69: Ch3 Special, the DAC); the others play notes of their own, a Porta
// channel's too while it plays.
static bool cut_hint(mdmi_gui *g, char *hint, size_t size)
{
	if (g->file_over || g->dragging || *g->source.page == PAGE_ROM) {
		return false;
	}
	const int channel = rail_channel_at(g->mouse_x, g->mouse_y);
	const int muted = channel >= 0 && channel_is_fm(channel) ? patch_muted_notes(g->source.patch, channel) : 0;
	if (muted <= 0) {
		return false;
	}
	const patch_values *values = patch_view(g->source.patch);
	static const char *const CAUSE[] = {"", "PORTA", "SP", "DAC"};
	char causes[3][24] = {"", "", ""};
	for (int voice = 0; voice < PATCH_NUM_FM_CHANNELS; voice++) {
		const int reason = patch_lend_reason(values, voice);
		if (reason != PATCH_ALONE_NO && reason <= (int)(sizeof(causes) / sizeof(*causes))) {
			char *list = causes[reason - 1];
			snprintf(list + strlen(list), sizeof(causes[0]) - strlen(list), " %d", voice + 1);
		}
	}
	// Short enough for the footer beside a long build label: "1 CUT: SP 3, DAC 6".
	int at = snprintf(hint, size, "%d CUT", muted);
	bool first = true;
	for (int reason = PATCH_ALONE_PORTA; reason <= PATCH_ALONE_DAC && at > 0 && (size_t)at < size; reason++) {
		if (causes[reason - 1][0]) {
			at += snprintf(hint + at, size - (size_t)at, "%s %s%s", first ? ":" : ",", CAUSE[reason], causes[reason - 1]);
			first = false;
		}
	}
	if (first && at > 0 && (size_t)at < size) {
		snprintf(hint + at, size - (size_t)at, ": ALL 6 VOICES PLAY");
	}
	return true;
}

// Stage 21's cells under the mouse, dim ones too (docs/design/ch3-ops.md, part 3).
static bool ch3_hint(mdmi_gui *g, char *hint, size_t size)
{
	if (g->file_over || g->dragging || *g->source.page == PAGE_ROM) {
		return false;
	}
	const patch_values *values = patch_view(g->source.patch);
	const int x = g->mouse_x, y = g->mouse_y;
	if (in_box(x, y, CH3_NOTES_X, GLOBALS_Y + 6, CH3_NOTES_W, 32)) {
		int mode = values->globals[GLOBAL_CH3_NOTES];
		for (int i = 0; i < CH3_NOTES_MODES; i++) {
			mode = in_box(x, y, CH3_NOTES_SEGMENTS[i].x, CH3_NOTES_SEGMENT_Y, CH3_NOTES_SEGMENTS[i].w, CH3_NOTES_SEGMENT_H) ? i : mode;
		}
		snprintf(hint, size, "%s", !values->globals[GLOBAL_CH3_MODE] ? "CH3 NOTES: ONLY WITH CH3 SPECIAL ON"
			: mode == PATCH_CH3_CSM    ? "CSM: MIDI 3 SETS RATE, EXACT TO B-4"
			: mode == PATCH_CH3_ONE_NOTE ? "1 NOTE: MIDI 3 PLAYS ALL 4 OPERATORS" : "OPS: MIDI 11-13 AND 3 PLAY OPERATORS 1-4");
		return true;
	}
	if (in_box(x, y, LFO_RESET_X, GLOBALS_Y + 6, LFO_RESET_W, 32)) {
		const int reset = values->globals[GLOBAL_LFO_RESET];
		if (!values->globals[GLOBAL_LFO_ON]) {
			snprintf(hint, size, "LFO RESET: ONLY WITH LFO ON");
		} else if (!reset) {
			snprintf(hint, size, "LFO RESET OFF. CLICK: RESET ON CH1 NOTES");
		} else {
			snprintf(hint, size, "EACH CH%d NOTE RESTARTS THE SHARED LFO", reset);
		}
		return true;
	}
	if (*g->source.page != PAGE_EDITOR || *g->source.channel != 2) {
		return false;
	}
	const int over = hovered(g);
	const uint32_t id = over >= 0 ? g->cells[over].id : 0;
	if (over >= 0 && is_ch3_op_global(id) && g->cells[over].kind == CELL_ROW) {
		const int op = ((int)id - GLOBAL_CH3_OP_BASE) / PATCH_CH3_OP_FIELDS, field = ((int)id - GLOBAL_CH3_OP_BASE) % PATCH_CH3_OP_FIELDS;
		char note[8];
		note_name(ch3_fixed_key(values, op), note, sizeof(note));
		if (field == PATCH_CH3_SEMI) {
			snprintf(hint, size, "OP%d SEMI: -48 TO +48 ST, DOUBLE CLICK 0", op + 1);
		} else if (field == PATCH_CH3_FINE) {
			snprintf(hint, size, "OP%d FINE: -63 TO +63 CT, DOUBLE CLICK 0", op + 1);
		} else if (values->globals[id]) {
			snprintf(hint, size, "FIXED: OP%d STAYS AT %s ON ANY KEY", op + 1, note);
		} else {
			snprintf(hint, size, "OP%d FOLLOWS THE KEY. CLICK: FIX TO %s", op + 1, note);
		}
		return true;
	}
	// The tags, each in its header; with Ch3 Special off there are none (stage 22b).
	for (int op = 0; op < PATCH_OPS && values->globals[GLOBAL_CH3_MODE]; op++) {
		if (!in_box(x, y, GRID_X[op] + PITCH_TAG_X, SSG_BUTTON_Y, PITCH_TAG_W, SSG_BUTTON_H) || in_pitch_editor(g, x, y)) {
			continue;
		}
		const int semi = values->globals[ch3_field(op, PATCH_CH3_SEMI)] - PATCH_CH3_SEMI_MAX;
		const int fine = values->globals[ch3_field(op, PATCH_CH3_FINE)] - PATCH_CH3_FINE_MAX;
		const int midi = patch_ch3_ops(values) && op < PATCH_OPS - 1 ? 11 + op : 3;
		char pitch[32], cents[16] = "";
		if (fine) {
			snprintf(cents, sizeof(cents), " %+d CT", fine);
		}
		if (values->globals[ch3_field(op, PATCH_CH3_FIXED)]) {
			char note[8];
			note_name(ch3_fixed_key(values, op), note, sizeof(note));
			snprintf(pitch, sizeof(pitch), "FIXED %s%s", note, cents);
		} else if (semi || !fine) {
			snprintf(pitch, sizeof(pitch), "KEY %+d ST%s", semi, cents);
		} else {
			snprintf(pitch, sizeof(pitch), "KEY%s", cents);
		}
		snprintf(hint, size, "OP%d ON MIDI %d: %s", op + 1, midi, pitch);
		return true;
	}
	return false;
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

// Stage 22c: the lists of BANK under the mouse, defined with them.
static bool bank_hint(mdmi_gui *g, char *hint, size_t size, uint32_t *color);
// Stage 24e: ALL's OUT box and the DAC's VOL, defined with ALL.
static bool out_hint(mdmi_gui *g, char *hint, size_t size);

// Stage 44 (env-speed.md): SPD under the mouse or dragged, with the step's length.
static bool env_speed_hint(mdmi_gui *g, char *hint, size_t size)
{
	const int index = g->dragging ? -1 : hovered(g);
	const uint32_t id = g->dragging ? g->drag_id : index >= 0 ? g->cells[index].id : 0;
	if (!is_env_speed(id) || *g->source.page != PAGE_EDITOR) {
		return false;
	}
	const double speed = patch_env_speed(patch_get(g->source.patch, id));
	snprintf(hint, size, "ENV SPEED X%.2f: A STEP %.0f MS%s", speed, 1000.0 / 59.92 / speed, g->dragging ? "" : ". DOUBLE CLICK: X1");
	return true;
}

// One line, by what matters most: a dead engine, a gesture under way or the button under the mouse,
// what just happened, what the mouse is over, then the engine's numbers.
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
	uint32_t color = C_LABEL;
	if (stats.failed) {
		if (stats.boot_error) {
			snprintf(buffer, sizeof(buffer), "ENGINE FAILED: %s %u", stats.boot_stage ? stats.boot_stage : "", stats.boot_error);
		} else {
			snprintf(buffer, sizeof(buffer), "ENGINE FAILED: RELOAD DEVICE");
		}
		footer_text(g, buffer, C_ERROR);
	} else if (g->file_over) {
		footer_text(g, g->drop_hint, g->drop_hint_color);
	} else if (copy_dragged(g)) {
		// Stage 43 (channel-copy.md): what letting go would do.
		const int to = copy_target(g), over = row_at(g, g->mouse_x, g->mouse_y);
		const bool fm = channel_is_fm(g->copy_from);
		if (to >= 0) {
			snprintf(buffer, sizeof(buffer), "COPY CH%d TO CH%d, REPLACES %.13s", g->copy_from + 1, to + 1, sound_name(patch_view(g->source.patch), to));
			footer_text(g, buffer, C_AMBER);
		} else if (over == g->copy_from) {
			footer_text(g, fm ? "DROP ON ANOTHER FM ROW" : "DROP ON ANOTHER PSG ROW", C_LABEL);
		} else if (over >= 0) {
			footer_text(g, fm ? "FM GOES TO FM 1-6" : "PSG GOES TO PSG 7-10", C_LABEL);
		} else {
			footer_text(g, "LET GO OUTSIDE A ROW TO CANCEL", C_LABEL);
		}
	} else if (action_hint(g, buffer, sizeof(buffer))) {
		footer_text(g, buffer, C_LABEL);
	} else if (tv_hint(g, buffer, sizeof(buffer))) {
		footer_text(g, buffer, C_LABEL);
	} else if (g->row_moved && bank_hint(g, buffer, sizeof(buffer), &color)) {
		footer_text(g, buffer, color);
	} else if (g->env_drag != ENV_NONE && envelope_hint(g, buffer, sizeof(buffer))) {
		footer_text(g, buffer, C_LABEL);
	} else if (g->arp_drag != ARP_NONE && arp_hint(g, buffer, sizeof(buffer))) {
		footer_text(g, buffer, C_LABEL);
	} else if (g->message_ticks > 0) {
		// Stage 49 (ui-concept.md): what just happened, then what the mouse is over, CUT first.
		footer_text(g, g->message, g->message_color);
	} else if (cut_hint(g, buffer, sizeof(buffer))) {
		footer_text(g, buffer, C_ERROR);
	} else if (env_speed_hint(g, buffer, sizeof(buffer))) {
		footer_text(g, buffer, C_LABEL);
	} else if (plugin_only_hint(g) == GLOBAL_MASTER) {
		footer_text(g, "MASTER: PLUGIN ONLY, NOT ON THE CONSOLE", C_LABEL);
	} else if (plugin_only_hint(g) == GLOBAL_RETRIGGER) {
		footer_text(g, "RETRIGGER: PLUGIN ONLY, NOT ON THE CONSOLE", C_LABEL);
	} else if (ch3_hint(g, buffer, sizeof(buffer))) {
		footer_text(g, buffer, C_LABEL);
	} else if (bank_hint(g, buffer, sizeof(buffer), &color)) {
		footer_text(g, buffer, color);
	} else if (out_hint(g, buffer, sizeof(buffer))) {
		footer_text(g, buffer, C_LABEL);
	} else if (noise_hint(g, buffer, sizeof(buffer))) {
		footer_text(g, buffer, C_LABEL);
	} else if (dac_hint(g, buffer, sizeof(buffer))) {
		footer_text(g, buffer, C_LABEL);
	} else if (base_hint(g, buffer, sizeof(buffer))) {
		footer_text(g, buffer, C_AMBER);
	} else if (mono_hint(g, buffer, sizeof(buffer))) {
		footer_text(g, buffer, C_LABEL);
	} else if (envelope_hint(g, buffer, sizeof(buffer))) {
		footer_text(g, buffer, C_LABEL);
	} else if (arp_hint(g, buffer, sizeof(buffer))) {
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

static void scroll_bank(mdmi_gui *g, int top)
{
	g->bank_top = top < 0 ? 0 : top > BANK_TOP_MAX ? BANK_TOP_MAX : top;
}

// Stage 22c (D56): the two lists of BANK. Geometry of list A (0) and B (1).
static int list_x(int list)
{
	return list ? LIST_B_X : BANK_X;
}

static int list_right(int list)
{
	return list ? BANK_RIGHT : LIST_A_RIGHT;
}

static int list_scroll_x(int list)
{
	return list ? SCROLL_X : LIST_A_SCROLL;
}

static int list_bar_right(int list)
{
	return list ? LIST_B_BAR_RIGHT : LIST_A_BAR_RIGHT;
}

static gui_banks *banks_of(mdmi_gui *g)
{
	return g->source.banks ? g->source.banks : &g->own_banks;
}

static int *list_top(mdmi_gui *g, int list)
{
	return g->lists[list].kind == LIST_ROM ? &g->bank_top : &g->lists[list].top;
}

static void scroll_list(mdmi_gui *g, int list, int top)
{
	const int most = g->lists[list].count - BANK_ROWS;
	int *at = list_top(g, list);
	*at = top > most ? most : top;
	*at = *at < 0 ? 0 : *at;
}

static const patch_bank *list_bank(const mdmi_gui *g, int list)
{
	return g->lists[list].kind == LIST_ROM ? patch_bank_view(g->source.patch) : &g->lists[list].bank;
}

static bool listed(const library_list *list, const char *name)
{
	for (int i = 0; i < list->count; i++) {
		if (!strcmp(list->names[i], name)) {
			return true;
		}
	}
	return false;
}

static bool is_bad(const mdmi_gui *g, const char *name)
{
	for (int i = 0; i < 4; i++) {
		if (!strcmp(g->lib_bad[i], name)) {
			return true;
		}
	}
	return false;
}

static void stem_of(const char *file, char *out, size_t size)
{
	const char *dot = strrchr(file, '.');
	snprintf(out, size, "%.*s", dot && dot != file ? (int)(dot - file) : (int)strlen(file), file);
}

static void open_list(mdmi_gui *g, int list, const char *name)
{
	bank_list *l = &g->lists[list];
	l->path[0] = l->name[0] = l->file[0] = 0;
	l->top = 0;
	if (!*name) {
		l->kind = list ? (g->lib_list.found ? LIST_NO_LIBRARY : LIST_NO_FOLDER) : LIST_ROM;
		return;
	}
	snprintf(l->file, sizeof(l->file), "%s", name);
	library_path(g->lib_folder, name, l->path, sizeof(l->path));
	stem_of(name, l->name, sizeof(l->name));
	if (files_read_library(l->path, &l->bank)) {
		l->kind = LIST_LIBRARY;
		for (int i = 0; i < 4; i++) {
			if (!strcmp(g->lib_bad[i], name)) {
				g->lib_bad[i][0] = 0;
			}
		}
		return;
	}
	l->kind = LIST_UNREAD;
	if (!is_bad(g, name)) {
		snprintf(g->lib_bad[g->lib_bad_next], LIBRARY_NAME, "%s", name);
		g->lib_bad_next = (g->lib_bad_next + 1) % 4;
	}
}

// The folder and both lists read again. A file gone leaves its list to the ROM bank (A) or the first
// library by name that A doesn't show (B). The rows stay where they were when the same bank comes back.
static void load_lists(mdmi_gui *g)
{
	library_folder(g->lib_folder, sizeof(g->lib_folder));
	library_scan(g->lib_folder, &g->lib_list);
	gui_banks *banks = banks_of(g);
	if (*banks->left && !listed(&g->lib_list, banks->left)) {
		banks->left[0] = 0;
	}
	if (*banks->right && (!listed(&g->lib_list, banks->right) || !strcmp(banks->right, banks->left))) {
		banks->right[0] = 0;
	}
	const char *right = banks->right;
	for (int i = 0; !*right && i < g->lib_list.count; i++) {
		if (strcmp(g->lib_list.names[i], banks->left)) {
			right = g->lib_list.names[i];
		}
	}
	const char *names[2] = {banks->left, right};
	for (int list = 0; list < 2; list++) {
		char name[LIBRARY_NAME];
		stem_of(names[list], name, sizeof(name));
		const bool same = g->lists[list].kind == LIST_LIBRARY && *names[list] && !strcmp(g->lists[list].name, name);
		const int top = g->lists[list].top;
		open_list(g, list, names[list]);
		g->lists[list].top = same ? top : 0;
	}
}

// The rows each list shows for the selected channel's list: FM voices or envelopes.
static void list_rows(mdmi_gui *g, bool envelopes)
{
	for (int list = 0; list < 2; list++) {
		bank_list *l = &g->lists[list];
		l->count = 0;
		for (int place = 0; place < PATCH_BANK_SIZE; place++) {
			if (l->kind == LIST_ROM || (l->kind == LIST_LIBRARY && !files_place_empty(&l->bank, envelopes, place))) {
				l->places[l->count++] = place;
			}
		}
		scroll_list(g, list, *list_top(g, list));
	}
}

// The list whose rows or scroll track are at x, y, or -1.
static int list_under(int x, int y)
{
	if (y < BANK_Y || y >= BANK_BOTTOM) {
		return -1;
	}
	return x >= BANK_X && x < LIST_A_BAR_RIGHT ? 0 : x >= LIST_B_X && x < LIST_B_BAR_RIGHT ? 1 : -1;
}

static int bar_under(int x, int y)
{
	if (y < BANK_BAR_Y || y >= BANK_BAR_Y + BANK_BAR_H) {
		return -1;
	}
	return x >= BANK_X && x < LIST_A_BAR_RIGHT ? 0 : x >= LIST_B_X && x < LIST_B_BAR_RIGHT ? 1 : -1;
}

// The place of a list's row at x, y, or -1.
static int list_place_at(mdmi_gui *g, int list, int x, int y)
{
	if (x < list_x(list) || x >= list_right(list) || y < BANK_Y || y >= BANK_BOTTOM) {
		return -1;
	}
	const int row = *list_top(g, list) + (y - BANK_Y) / BANK_ROW_H;
	return row < g->lists[list].count ? g->lists[list].places[row] : -1;
}

// A place of the ROM bank at x, y while list A shows it, for dropped files.
static int bank_place_at(mdmi_gui *g, int x, int y)
{
	return g->lists[0].kind == LIST_ROM ? list_place_at(g, 0, x, y) : -1;
}

static files_spot spot_of(const mdmi_gui *g, int list, int place)
{
	return (files_spot){g->lists[list].kind == LIST_ROM ? NULL : g->lists[list].path, place};
}

static int first_free(const bank_list *l, bool envelopes)
{
	for (int place = 0; place < PATCH_BANK_SIZE; place++) {
		if (files_place_empty(&l->bank, envelopes, place)) {
			return place;
		}
	}
	return -1;
}

// The library row under the mouse has CLEAR at its right.
static bool over_clear(int list, int x, int y)
{
	const int left = list_right(list) - 44;
	return x >= left && x < left + 40 && (y - BANK_Y) % BANK_ROW_H >= 2 && (y - BANK_Y) % BANK_ROW_H < 17;
}

static bool shows_new(const mdmi_gui *g, int list)
{
	return g->lists[list].kind == LIST_NO_FOLDER || g->lists[list].kind == LIST_NO_LIBRARY;
}

static bool over_new(const mdmi_gui *g, int list, int x, int y)
{
	return shows_new(g, list) && in_box(x, y, list_x(list) + 8, 180, 64, 24);
}

// Text in lines of at most chars, broken after a space or a separator. Returns the y after it.
static int text_wrap(mdmi_gui *g, int x, int y, const char *string, uint32_t color, int chars)
{
	while (*string) {
		int length = (int)strlen(string);
		if (length > chars) {
			length = chars;
			for (int i = chars; i > chars / 2; i--) {
				if (string[i - 1] == ' ' || string[i - 1] == '\\' || string[i - 1] == '/') {
					length = i;
					break;
				}
			}
		}
		char line[128];
		snprintf(line, sizeof(line), "%.*s", length, string);
		text(g, x, y, line, color, 1);
		string += length;
		y += 10;
	}
	return y;
}

static void triangle(mdmi_gui *g, int x, int y, bool up, uint32_t color)
{
	for (int i = 0; i < 4; i++) {
		fill(g, x + i, up ? y + 3 - i : y + i, 7 - 2 * i, 1, color);
	}
}

static void draw_list_bar(mdmi_gui *g, int list, bool envelopes)
{
	const bank_list *l = &g->lists[list];
	const int x = list_x(list), right = list_bar_right(list), w = right - x;
	const bool open = g->chooser == list, rom = l->kind == LIST_ROM;
	const bool over = !open && !g->file_over && g->row_list < 0 && g->chooser < 0 && bar_under(g->mouse_x, g->mouse_y) == list;
	if (open) {
		fill(g, x, BANK_BAR_Y, w, BANK_BAR_H, C_TEXT);
	} else if (rom) {
		fill(g, x, BANK_BAR_Y, w, BANK_BAR_H, envelopes ? C_PSG_DARK : C_FM_DARK);
		fill(g, x, BANK_BAR_Y, 4, BANK_BAR_H, envelopes ? C_PSG : C_FM);
	} else {
		fill(g, x, BANK_BAR_Y, w, BANK_BAR_H, C_CELL);
	}
	char label[64];
	snprintf(label, sizeof(label), "%.*s", (w - 30) / 12, rom ? "ROM BANK" : shows_new(g, list) ? "NO LIBRARY" : l->name);
	text(g, x + 10, BANK_BAR_Y + 3, label, open ? C_BG : shows_new(g, list) ? C_LABEL : C_TEXT, 2);
	if (rom && !open) {
		text_right(g, right - 22, BANK_BAR_Y + 7, "PLAYS", envelopes ? C_PSG : C_FM_LIGHT, 1, 60);
	}
	triangle(g, right - 14, BANK_BAR_Y + 8, open, open ? C_BG : rom ? C_TEXT : C_LABEL);
	if (over) {
		frame(g, x, BANK_BAR_Y, w, BANK_BAR_H, C_TEXT);
	}
}

// The scroll track of a list, its thumb by the rows.
static void draw_list_track(mdmi_gui *g, int list)
{
	const int x = list_scroll_x(list), count = g->lists[list].count;
	fill(g, x, BANK_Y, 6, SCROLL_H, C_CELL);
	if (count > BANK_ROWS) {
		const bool over = g->mouse_x >= x && g->mouse_x < x + 6 && g->mouse_y >= BANK_Y && g->mouse_y < BANK_BOTTOM;
		fill(g, x, BANK_Y + *list_top(g, list) * SCROLL_H / count, 6, SCROLL_H * BANK_ROWS / count, over ? C_LABEL : C_LINE);
	}
}

static uint32_t list_frame_color(const mdmi_gui *g, int list)
{
	return g->flash_ticks > 0 && g->flash_list == list && g->flash_place < 0 && g->flash_color == C_ERROR ? C_ERROR : 0;
}

static void double_frame(mdmi_gui *g, int list, uint32_t color)
{
	const int x = list_x(list) - 2, w = list_scroll_x(list) + 8 - x;
	frame(g, x, BANK_Y - 2, w, SCROLL_H + 4, color);
	frame(g, x + 1, BANK_Y - 1, w - 2, SCROLL_H + 2, color);
}

// The list a row is dragged into, and where the copy would go, -1 when it goes nowhere.
static int drag_target(mdmi_gui *g, bool envelopes, int *place)
{
	const int to = list_under(g->mouse_x, g->mouse_y);
	*place = -1;
	if (!g->row_moved || to < 0 || to == g->row_list) {
		return -1;
	}
	if (g->lists[to].kind == LIST_ROM) {
		*place = list_place_at(g, to, g->mouse_x, g->mouse_y);
	} else if (g->lists[to].kind == LIST_LIBRARY) {
		*place = first_free(&g->lists[to], envelopes);
	}
	return to;
}

// The ROM bank in list A: its places, the channels' dots, the selected channel's place lit (D23).
static void draw_rom_list(mdmi_gui *g, const patch_values *values, bool envelopes)
{
	const int channel = *g->source.channel;
	const uint32_t kind = envelopes ? C_PSG : C_FM, dark = envelopes ? C_PSG_DARK : C_FM_DARK, light = envelopes ? C_PSG : C_FM_LIGHT;
	const int first_channel = envelopes ? PATCH_NUM_FM_CHANNELS : 0, last_channel = envelopes ? PATCH_NUM_CHANNELS : PATCH_NUM_FM_CHANNELS;
	const int n = last_channel - first_channel;
	const int preset = values->channels[channel][PARAM_PRESET];
	const patch_bank *bank = patch_bank_view(g->source.patch);
	char buffer[64];
	text(g, 250, BANK_LABEL_Y, "PRG", C_HINT, 1);
	text(g, 290, BANK_LABEL_Y, envelopes ? "PSG ENVELOPE" : "FM VOICE", light, 1);
	for (int c = first_channel; c < last_channel; c++) {
		snprintf(buffer, sizeof(buffer), "%d", c + 1);
		text(g, LIST_A_RIGHT - (n - (c - first_channel)) * 8, BANK_LABEL_Y, buffer, c == channel ? C_TEXT : is_dac(values, c) ? C_LINE : C_HINT, 1);
	}
	const bool own = plays_own_file(g, values, channel);
	const bool idle = !g->file_over && g->row_list < 0 && g->chooser < 0;
	const int hover = idle ? list_place_at(g, 0, g->mouse_x, g->mouse_y) : -1;
	const bool aimed = g->file_over && (g->drop_place >= 0 || g->drop_add);
	int dragged_to;
	const bool dragged = drag_target(g, envelopes, &dragged_to) == 0;
	for (int row = 0; row < BANK_ROWS; row++) {
		const int place = g->bank_top + row, y = BANK_Y + row * BANK_ROW_H;
		const bool selected = preset == place;
		const bool target = (aimed && place >= g->drop_first && place < g->drop_first + g->drop_lit) || (dragged && place == dragged_to);
		const bool flash = g->flash_ticks > 0 && g->flash_list < 0 && place >= g->flash_place && place < g->flash_place + g->flash_count;
		const bool from_file = envelopes ? !patch_envelope_is_rom(bank, place) : !patch_place_is_gm(bank, place);
		if (target) {
			fill(g, BANK_X, y, LIST_A_RIGHT - BANK_X, BANK_ROW_H - 1, C_AMBER);
		} else if (flash && g->flash_color == C_LINE) {
			fill(g, BANK_X, y, LIST_A_RIGHT - BANK_X, BANK_ROW_H - 1, C_LINE);
		} else if (selected) {
			if (!own) {
				fill(g, BANK_X + 4, y, LIST_A_RIGHT - BANK_X - 4, BANK_ROW_H - 1, dark);
			}
			fill(g, BANK_X, y, 4, BANK_ROW_H - 1, kind);
		}
		fill(g, BANK_X, y + BANK_ROW_H - 1, LIST_A_RIGHT - BANK_X, 1, place % 8 == 7 ? C_LINE : C_CELL);
		snprintf(buffer, sizeof(buffer), "%d", place + 1);
		text_right(g, 284, y + 3, buffer, target ? C_BG : selected ? C_TEXT : C_LABEL, 2, 40);
		snprintf(buffer, sizeof(buffer), "%.13s", place_name(bank, envelopes, place));
		text(g, 290, y + 3, buffer, target ? C_BG : from_file ? C_TEXT : C_LABEL, 2);
		for (int c = first_channel; c < last_channel; c++) {
			if (!takes_files(values, c) || values->channels[c][PARAM_PRESET] != place) {
				continue;
			}
			const uint32_t color = target ? C_BG : c == channel ? C_TEXT : light;
			const int x = LIST_A_RIGHT - (n - (c - first_channel)) * 8;
			if (plays_own_file(g, values, c)) {
				frame(g, x, y + 7, 6, 6, color);
			} else {
				fill(g, x, y + 7, 6, 6, color);
			}
		}
		if (flash && g->flash_color == C_ERROR) {
			frame(g, BANK_X, y, LIST_A_RIGHT - BANK_X, BANK_ROW_H - 1, C_ERROR);
		} else if (g->row_list == 0 && g->row_moved && g->row_place == place) {
			frame(g, BANK_X, y, LIST_A_RIGHT - BANK_X, BANK_ROW_H - 1, C_TEXT);
			frame(g, BANK_X + 1, y + 1, LIST_A_RIGHT - BANK_X - 2, BANK_ROW_H - 3, C_TEXT);
		} else if (hover == place) {
			frame(g, BANK_X, y, LIST_A_RIGHT - BANK_X, BANK_ROW_H - 1, C_TEXT);
		}
	}
	draw_list_track(g, 0);
	// Every channel of the list marks its place on the track, the selected one wide, so places out of
	// view are found without scrolling.
	for (int c = first_channel; c < last_channel; c++) {
		if (c != channel && takes_files(values, c)) {
			fill(g, LIST_A_SCROLL, BANK_Y + values->channels[c][PARAM_PRESET] * SCROLL_H / PATCH_BANK_SIZE, 3, 2, light);
		}
	}
	fill(g, LIST_A_SCROLL, BANK_Y + preset * SCROLL_H / PATCH_BANK_SIZE, 6, 3, kind);
	// The lit places show on the track too, where the list doesn't reach.
	if (aimed && g->drop_lit > 0) {
		const int h = g->drop_lit * SCROLL_H / PATCH_BANK_SIZE;
		fill(g, LIST_A_SCROLL, BANK_Y + g->drop_first * SCROLL_H / PATCH_BANK_SIZE, 6, h < 3 ? 3 : h, C_AMBER);
	}
}

// The library place last tried on the selected channel, while the channel plays it.
static bool is_tried(mdmi_gui *g, const patch_values *values, int list, int place)
{
	const int channel = *g->source.channel;
	const bank_list *l = &g->lists[list];
	return g->tried_channel == channel && g->tried_place == place && !strcmp(g->tried_path, l->path) && plays_own_file(g, values, channel)
		&& !strcmp(sound_name(values, channel), place_name(&l->bank, !channel_is_fm(channel), place));
}

// A library: its sounds by place, no numbers, grey but for the one the channel plays (D56).
static void draw_library_list(mdmi_gui *g, const patch_values *values, int list, bool envelopes)
{
	const bank_list *l = &g->lists[list];
	const int x = list_x(list), right = list_right(list), chars = list ? 13 : 20;
	const uint32_t kind = envelopes ? C_PSG : C_FM, dark = envelopes ? C_PSG_DARK : C_FM_DARK;
	char buffer[160];
	if (l->kind == LIST_UNREAD) {
		text(g, x + 8, 100, "CAN'T READ", C_ERROR, 2);
		const int y = text_wrap(g, x + 8, 124, "not an MDMI bank", C_LABEL, (right - x - 16) / 6);
		text_wrap(g, x + 8, y + 2, "Nothing is written to this file", C_HINT, (right - x - 16) / 6);
	} else if (shows_new(g, list)) {
		// Stage 37: the folder's only library open on the left is no "no libraries" (22c).
		const bool one_left = l->kind == LIST_NO_LIBRARY && g->lib_list.count == 1 && g->lists[0].kind == LIST_LIBRARY;
		text(g, x + 8, 100, l->kind == LIST_NO_FOLDER ? "NO FOLDER" : one_left ? "NO OTHER" : "NO LIBRARIES", C_LABEL, 2);
		int y = text_wrap(g, x + 8, 124, g->lib_folder, C_HINT, 28 < (right - x - 16) / 6 ? 28 : (right - x - 16) / 6);
		text_wrap(g, x + 8, y + 2, "Libraries are .mdbank files in this folder.", C_HINT, (right - x - 16) / 6);
		const bool over = !g->file_over && g->chooser < 0 && over_new(g, list, g->mouse_x, g->mouse_y);
		frame(g, x + 8, 180, 64, 24, over ? C_TEXT : C_LINE);
		text(g, x + 8 + 14, 185, "NEW", over ? C_TEXT : C_LABEL, 2);
	} else if (l->count == 0) {
		snprintf(buffer, sizeof(buffer), "0 %s, 128 FREE", envelopes ? "ENVELOPES" : "VOICES");
		text(g, x + 8, BANK_LABEL_Y, buffer, C_HINT, 1);
		text(g, x + 8, 100, "EMPTY", C_LABEL, 2);
		text(g, x + 8, 124, "Drag a sound here", C_HINT, 1);
	} else {
		snprintf(buffer, sizeof(buffer), "%d %s, %d FREE", l->count, envelopes ? "ENVELOPES" : "VOICES", PATCH_BANK_SIZE - l->count);
		text(g, x + 8, BANK_LABEL_Y, buffer, C_HINT, 1);
	}
	const bool idle = !g->file_over && g->row_list < 0 && g->chooser < 0;
	const int hover = idle && l->kind == LIST_LIBRARY ? list_place_at(g, list, g->mouse_x, g->mouse_y) : -1;
	for (int row = 0; l->kind == LIST_LIBRARY && row < BANK_ROWS && l->top + row < l->count; row++) {
		const int place = l->places[l->top + row], y = BANK_Y + row * BANK_ROW_H;
		const bool flash = g->flash_ticks > 0 && g->flash_list == list && g->flash_place == place;
		if (flash && g->flash_color == C_LINE) {
			fill(g, x, y, right - x, BANK_ROW_H - 1, C_LINE);
		} else if (is_tried(g, values, list, place)) {
			fill(g, x + 4, y, right - x - 4, BANK_ROW_H - 1, dark);
			fill(g, x, y, 4, BANK_ROW_H - 1, kind);
		}
		fill(g, x, y + BANK_ROW_H - 1, right - x, 1, C_CELL);
		const int room = hover == place ? (right - 44 - x - 12) / 12 : chars;
		snprintf(buffer, sizeof(buffer), "%.*s", room < chars ? room : chars, place_name(&l->bank, envelopes, place));
		text(g, x + 8, y + 3, buffer, C_TEXT, 2);
		if (flash && g->flash_color == C_ERROR) {
			frame(g, x, y, right - x, BANK_ROW_H - 1, C_ERROR);
		} else if (g->row_list == list && g->row_moved && g->row_place == place) {
			frame(g, x, y, right - x, BANK_ROW_H - 1, C_TEXT);
			frame(g, x + 1, y + 1, right - x - 2, BANK_ROW_H - 3, C_TEXT);
		} else if (hover == place) {
			frame(g, x, y, right - x, BANK_ROW_H - 1, C_TEXT);
			const bool clear = over_clear(list, g->mouse_x, g->mouse_y);
			frame(g, right - 44, y + 2, 40, 15, clear ? C_TEXT : C_LINE);
			text(g, right - 44 + 5, y + 2 + 4, "CLEAR", clear ? C_TEXT : C_LABEL, 1);
		}
	}
	draw_list_track(g, list);
	int place;
	if (drag_target(g, envelopes, &place) == list && l->kind == LIST_LIBRARY) {
		double_frame(g, list, place >= 0 ? C_AMBER : C_LINE);
	}
	if (list_frame_color(g, list)) {
		double_frame(g, list, C_ERROR);
	}
}

// The choice of a bank, over both lists: NEW, the ROM bank for list A, the libraries by name.
enum { CHOICE_NEW = -2, CHOICE_ROM = -1, CHOICE_MORE = LIBRARY_MAX };
static int choice_count(const mdmi_gui *g)
{
	return 1 + (g->chooser == 0) + g->lib_list.count + (g->lib_list.total > g->lib_list.count);
}

// What row i of the choice is: CHOICE_NEW, CHOICE_ROM, a library's index, or CHOICE_MORE.
static int choice_of(const mdmi_gui *g, int i)
{
	if (i == 0) {
		return CHOICE_NEW;
	}
	if (g->chooser == 0 && i == 1) {
		return CHOICE_ROM;
	}
	const int library = i - 1 - (g->chooser == 0);
	return library < g->lib_list.count ? library : CHOICE_MORE;
}

static int choice_at(const mdmi_gui *g, int x, int y)
{
	if (x < BANK_X || x >= BANK_RIGHT || y < BANK_Y || y >= BANK_BOTTOM) {
		return -3;
	}
	const int i = g->chooser_top + (y - BANK_Y) / BANK_ROW_H;
	return i < choice_count(g) ? choice_of(g, i) : -3;
}

// Whether a list shows a library's file; the one open in the other list can't be chosen here.
static bool list_shows(const mdmi_gui *g, int list, int library)
{
	return *g->lists[list].file && !strcmp(g->lists[list].file, g->lib_list.names[library]);
}

static bool open_here(const mdmi_gui *g, int library)
{
	return list_shows(g, g->chooser, library);
}

static bool open_elsewhere(const mdmi_gui *g, int library)
{
	return list_shows(g, 1 - g->chooser, library);
}

static void draw_chooser(mdmi_gui *g, bool envelopes)
{
	// Over the lists and their frames.
	fill(g, BANK_X - 2, BANK_LABEL_Y, LIST_B_BAR_RIGHT + 4 - BANK_X, BANK_BOTTOM + 3 - BANK_LABEL_Y, C_BG);
	text(g, 250, BANK_LABEL_Y, g->chooser ? "RIGHT LIST: A LIBRARY" : "LEFT LIST: ROM BANK OR A LIBRARY", C_HINT, 1);
	char buffer[160];
	const int path_room = (BANK_RIGHT - 460) / 6;
	const int length = (int)strlen(g->lib_folder);
	snprintf(buffer, sizeof(buffer), "%s", length > path_room ? g->lib_folder + length - path_room : g->lib_folder);
	text_right(g, BANK_RIGHT, BANK_LABEL_Y, buffer, C_HINT, 1, path_room * 6);
	const int count = choice_count(g);
	const int over = choice_at(g, g->mouse_x, g->mouse_y);
	for (int row = 0; row < BANK_ROWS && g->chooser_top + row < count; row++) {
		const int y = BANK_Y + row * BANK_ROW_H, choice = choice_of(g, g->chooser_top + row);
		bool clickable = true;
		fill(g, BANK_X, y + BANK_ROW_H - 1, BANK_RIGHT - BANK_X, 1, C_CELL);
		if (choice == CHOICE_NEW) {
			frame(g, 250, y + 2, 56, 15, over == choice ? C_TEXT : C_LINE);
			text(g, 250 + 19, y + 6, "NEW", C_LABEL, 1);
			text(g, 314, y + 6, "empty library, named in the Windows save dialog", C_HINT, 1);
		} else if (choice == CHOICE_ROM) {
			fill(g, BANK_X, y, 4, BANK_ROW_H - 1, envelopes ? C_PSG : C_FM);
			text(g, 250, y + 3, "ROM BANK", C_TEXT, 2);
			if (g->lists[0].kind == LIST_ROM) {
				text_right(g, BANK_RIGHT - 6, y + 7, "OPEN", C_LABEL, 1, 40);
			}
		} else if (choice == CHOICE_MORE) {
			clickable = false;
			snprintf(buffer, sizeof(buffer), "%d of %d libraries shown, by name", g->lib_list.count, g->lib_list.total);
			text(g, 250, y + 7, buffer, C_HINT, 1);
		} else {
			char name[LIBRARY_NAME];
			stem_of(g->lib_list.names[choice], name, sizeof(name));
			const bool here = open_here(g, choice), elsewhere = open_elsewhere(g, choice), bad = is_bad(g, g->lib_list.names[choice]);
			clickable = !elsewhere;
			if (here) {
				fill(g, BANK_X + 4, y, BANK_RIGHT - BANK_X - 4, BANK_ROW_H - 1, C_CELL);
				fill(g, BANK_X, y, 4, BANK_ROW_H - 1, C_TEXT);
			}
			snprintf(buffer, sizeof(buffer), "%.30s", name);
			text(g, 250, y + 3, buffer, elsewhere ? C_LINE : bad ? C_ERROR : C_TEXT, 2);
			const char *note = elsewhere ? (g->chooser ? "IN LEFT" : "IN RIGHT") : bad ? "CAN'T READ" : here ? "OPEN" : "";
			text_right(g, BANK_RIGHT - 6, y + 7, note, elsewhere ? C_LINE : bad ? C_ERROR : C_LABEL, 1, 70);
		}
		if (clickable && over == choice) {
			frame(g, BANK_X, y, BANK_RIGHT - BANK_X, BANK_ROW_H - 1, C_TEXT);
		}
	}
	fill(g, SCROLL_X, BANK_Y, 6, SCROLL_H, C_CELL);
	if (count > BANK_ROWS) {
		fill(g, SCROLL_X, BANK_Y + g->chooser_top * SCROLL_H / count, 6, SCROLL_H * BANK_ROWS / count, C_LINE);
	}
}

// The page of the selected channel's list (D28): list A, list B, the choice of a bank over them.
static void draw_bank(mdmi_gui *g, const patch_values *values)
{
	const int channel = *g->source.channel;
	const bool envelopes = !channel_is_fm(channel);
	if (!g->bank_was_open) {
		load_lists(g);
		g->chooser = -1;
	}
	list_rows(g, envelopes);
	const int preset = values->channels[channel][PARAM_PRESET];
	// Opening the page or choosing another channel brings its place into view; a program change doesn't.
	if (takes_files(values, channel) && (!g->bank_was_open || g->bank_seen_channel != channel) && (preset < g->bank_top || preset >= g->bank_top + BANK_ROWS)) {
		scroll_bank(g, preset - 3);
	}
	g->bank_was_open = true;
	g->bank_seen_channel = channel;

	// Stage 37: channel 3 in Ch3 Special is pink here too, as on its other pages (24d).
	g->sp = channel == 2 && is_special(values, 2);
	draw_page_top(g, values);
	g->sp = false;
	if (is_dac(values, channel)) {
		draw_dac_list(g, values);
		return;
	}
	for (int list = 0; list < 2; list++) {
		draw_list_bar(g, list, envelopes);
	}
	fill(g, LIST_DIVIDER_X, BANK_BAR_Y, 1, BANK_BOTTOM - BANK_BAR_Y, C_LINE);
	if (g->lists[0].kind == LIST_ROM) {
		draw_rom_list(g, values, envelopes);
	} else {
		draw_library_list(g, values, 0, envelopes);
	}
	// While files of the list's kind are dragged over the page, list B is one ADD field into the ROM
	// bank (D26), when list A shows it.
	const bool sounds_over = g->file_over && (envelopes ? g->file_envelopes : g->file_instruments) > 0 && g->lists[0].kind == LIST_ROM;
	if (sounds_over) {
		const bool full = files_add_place(g->source.files, envelopes) >= PATCH_BANK_SIZE;
		const uint32_t edge = full ? C_LINE : C_AMBER;
		fill(g, LIST_B_X, BANK_LABEL_Y, LIST_B_BAR_RIGHT - LIST_B_X, BANK_BOTTOM - BANK_LABEL_Y, C_BG);
		fill(g, LIST_B_X, BANK_Y, LIST_B_BAR_RIGHT - LIST_B_X, SCROLL_H, g->drop_add ? C_AMBER : C_BG);
		text(g, (LIST_B_X + LIST_B_BAR_RIGHT) / 2 - 18, BANK_Y + SCROLL_H / 2 - 7, "ADD", g->drop_add ? C_BG : edge, 2);
		double_frame(g, 0, C_AMBER);
		double_frame(g, 1, edge);
	} else {
		draw_library_list(g, values, 1, envelopes);
	}
	if (g->chooser >= 0) {
		draw_chooser(g, envelopes);
	}
	// The dragged sound's name at the mouse.
	if (g->row_list >= 0 && g->row_moved) {
		char buffer[32];
		snprintf(buffer, sizeof(buffer), "%.13s", place_name(list_bank(g, g->row_list), envelopes, g->row_place));
		fill(g, g->mouse_x + 8, g->mouse_y - 10, (int)strlen(buffer) * 12 + 8, 20, C_TEXT);
		text(g, g->mouse_x + 12, g->mouse_y - 7, buffer, C_BG, 2);
	}
}

// The footer over the lists of BANK: what a click or a drop would do.
static bool bank_hint(mdmi_gui *g, char *hint, size_t size, uint32_t *color)
{
	const patch_values *values = patch_view(g->source.patch);
	const int channel = *g->source.channel;
	if (*g->source.page != PAGE_BANK || is_dac(values, channel) || g->file_over) {
		return false;
	}
	const bool envelopes = !channel_is_fm(channel);
	const int x = g->mouse_x, y = g->mouse_y;
	*color = C_LABEL;
	if (g->row_list >= 0 && g->row_moved) {
		const char *name = place_name(list_bank(g, g->row_list), envelopes, g->row_place);
		int place;
		const int to = drag_target(g, envelopes, &place), over = list_under(x, y);
		if (over == g->row_list) {
			snprintf(hint, size, "Drop on the other list");
		} else if (to < 0) {
			return false;
		} else if (g->lists[to].kind == LIST_ROM && place >= 0) {
			*color = C_AMBER;
			snprintf(hint, size, "%s > slot %d, replaces %s", name, place + 1, place_name(list_bank(g, to), envelopes, place));
		} else if (g->lists[to].kind == LIST_LIBRARY && place < 0) {
			snprintf(hint, size, "%s is full to 128: clear a sound first", g->lists[to].name);
		} else if (g->lists[to].kind == LIST_LIBRARY) {
			*color = C_AMBER;
			snprintf(hint, size, "%s > ADD to %s, %d free", name, g->lists[to].name, PATCH_BANK_SIZE - g->lists[to].count);
		} else {
			return false;
		}
		return true;
	}
	if (g->row_list >= 0 || g->chooser >= 0) {
		return false;
	}
	const int list = list_under(x, y), bar = bar_under(x, y);
	if ((list == 1 || bar == 1) && g->lists[1].kind == LIST_NO_FOLDER) {
		snprintf(hint, size, "No folder %s", g->lib_folder);
		return true;
	}
	const int place = list >= 0 ? list_place_at(g, list, x, y) : -1;
	if (place < 0) {
		return false;
	}
	const patch_bank *bank = list_bank(g, list);
	char steps[32] = "";
	if (envelopes) {
		const patch_envelope *envelope = &bank->envelopes[place];
		const int length = patch_envelope_length(envelope);
		snprintf(steps, sizeof(steps), ", %d steps%s", length, memchr(envelope->steps, PATCH_EEF_LOOP_START, (size_t)length) ? ", loop" : "");
	}
	if (g->lists[list].kind == LIST_ROM) {
		snprintf(hint, size, "%d %s: click plays on CH%d, drag copies%s", place + 1, place_name(bank, envelopes, place), channel + 1, steps);
	} else if (over_clear(list, x, y)) {
		snprintf(hint, size, "CLEAR: %s leaves %s", place_name(bank, envelopes, place), g->lists[list].name);
	} else {
		snprintf(hint, size, "%s: click plays on CH%d, drag copies%s", place_name(bank, envelopes, place), channel + 1, steps);
	}
	return true;
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
			text(g, x + 3, y + 2, digit, on_fill(g), 1);
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
// The column at `playing` (or none at -1) lights up as in the editor (stage 41).
static void draw_mini_envelope(mdmi_gui *g, int x, int y, int w, int h, const patch_shape *shape, int playing)
{
	const int n = shape->count > 0 ? shape->count : 1;
	if (shape->loop_start >= 0) {
		const int from = mini_edge(x, w, n, shape->loop_start);
		fill(g, from, y, mini_edge(x, w, n, loop_end_of(shape)) - from, h, C_CELL);
	}
	// The playing column last: with more frames than pixels the next frame shares its x and would cover it.
	for (int i = 0; i <= shape->count; i++) {
		const int frame = i < shape->count ? i : playing;
		const bool lit = frame == playing;
		if (frame < 0 || frame >= shape->count || (lit && i < shape->count)) {
			continue;
		}
		const int level = shape->frames[frame] & 15;
		int width = mini_edge(x, w, n, frame + 1) - mini_edge(x, w, n, frame) - (w / n >= 3 ? 1 : 0);
		width = width < 1 ? 1 : width;
		if (lit) {
			fill(g, mini_edge(x, w, n, frame), y, width, h, C_PSG_DARK);
		}
		if (level >= 15) {
			continue;
		}
		const int top = y + ((h - 3) * level + 7) / 15;
		fill(g, mini_edge(x, w, n, frame), top, width, y + h - top, lit ? C_PSG : C_PSG_DARK);
		fill(g, mini_edge(x, w, n, frame), top, width, 2, lit ? C_TEXT : C_PSG);
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
	// Stage 44 (D88): the frames the envelope went at the speeds sent; at x1 the time since Note On.
	const uint64_t since = __atomic_load_n(&times->env_since[psg], __ATOMIC_ACQUIRE);
	const uint64_t at = __atomic_load_n(&times->env_at[psg], __ATOMIC_ACQUIRE) + (now > since ? now - since : 0) * __atomic_load_n(&times->env_rate[psg], __ATOMIC_ACQUIRE);
	const double scale = PSG_FRAME_HZ / ((double)PATCH_ENV_RATE_ONE * rate);
	const long total = (long)((double)at * scale);
	if (off == GUI_NEVER || off < on) {
		return patch_shape_position(shape, total, -1);
	}
	const uint64_t env_off = __atomic_load_n(&times->env_off[psg], __ATOMIC_ACQUIRE);
	const long held = (long)((double)(env_off == GUI_NEVER || env_off > at ? at : env_off) * scale);
	return patch_shape_position(shape, held, total - held);
}

// Stage 24e (D60): the lowest key the channel sounds now, its own and its chord's voices, or -1.
static int lowest_key(mdmi_gui *g, const patch_values *values, int channel)
{
	const uint32_t note = __atomic_load_n(&g->source.notes[channel], __ATOMIC_ACQUIRE);
	int lowest = GUI_NOTE_SEEN(note) && GUI_NOTE_HELD(note) ? (int)GUI_NOTE_KEY(note) : -1;
	if (channel_is_fm(channel) && !(channel == 2 && patch_ch3_ops(values))) {
		lowest = -1;
		for (int voice = 0; voice < PATCH_NUM_FM_CHANNELS; voice++) {
			if (patch_voice_player(g->source.patch, voice) == channel) {
				const int key = patch_voice_key(g->source.patch, voice);
				lowest = lowest < 0 || key < lowest ? key : lowest;
			}
		}
	}
	return lowest;
}

// Where the picture of a channel's output starts and how long it is, in samples back from the end of
// `count`: 2 periods of the lowest key, from the last rising crossing of its fundamental that leaves
// room for them, so a held note stands still (art-director's frame, build/stage24e). The noise has a
// fixed 5 ms.
static bool out_span(mdmi_gui *g, const patch_values *values, int channel, const mdmi_waves *waves, unsigned rate, double *start, int *span, int *key)
{
	*key = lowest_key(g, values, channel);
	if (*key < 0 || !waves || waves->count < 64 || rate == 0) {
		return false;
	}
	const float *s = waves->samples[channel];
	const int count = (int)waves->count;
	if (channel == PATCH_NUM_CHANNELS - 1) {
		*span = (int)(rate * 5 / 1000);
		*span = *span < count ? *span : count;
		*start = count - *span;
		return true;
	}
	const double cents = values->channels[channel][PARAM_FINE_TUNE] - 64;
	const double hz = 440.0 * pow(2.0, (*key - 69) / 12.0 + cents / 1200.0);
	const double period = rate / hz;
	*span = (int)(2 * period + 0.5);
	if (*span < 2 || *span + period + 1 > count) {
		return false;
	}
	// Stage 35 (D80): the start is a rising zero crossing of the lowest key's fundamental, its phase
	// measured each frame: one bin at the key's frequency over whole periods of the last ~4096 samples,
	// with a Hann window, on the outputs' own sample count. The waveform's own crossings won't do: a
	// decaying chord's move, merge and vanish (C3 + C4 of a piano jumped up to 15 px). Over whole periods
	// of the lowest key, its harmonics (a chord's octave too) add nothing to the bin, and decay changes
	// its size, not its phase. No state, so nothing wanders from frame to frame.
	const uint64_t first = waves->end - (uint64_t)count;
	int periods = (int)(4096 / period);
	periods = periods < 2 ? 2 : periods;
	const int n = (int)(periods * period);
	if (n > count) {
		return false;
	}
	const int i0 = count - n;
	const double turn = 2 * 3.14159265358979323846;
	double re = 0, im = 0;
	for (int i = i0; i < count; i++) {
		const double w = 0.5 - 0.5 * cos(turn * (i - i0) / n);
		const double angle = turn * fmod((double)(first + (uint64_t)i), period) / period;
		re += s[i] * w * cos(angle);
		im -= s[i] * w * sin(angle);
	}
	// The fundamental is about cos(2 pi t / period + theta): it rises through zero where that angle is
	// -pi / 2, at t = period * (-1/4 - theta / 2 pi), every period.
	const double theta = atan2(im, re);
	double rise = fmod(period * (-0.25 - theta / turn) - fmod((double)first, period), period);
	rise = rise < 0 ? rise + period : rise;
	const double limit = count - *span - 1;
	*start = rise + floor((limit - rise) / period) * period;
	*start = *start < 0 ? 0 : *start;
	return true;
}

// The OUT box of an FM or PSG row (stage 24e, D60): the channel's output now, at one scale, the dark
// colour a pixel wider under the light one as a phosphor's glow; only its frame and middle line when
// the channel holds no note. Channel 3 in Ch3 Special is pink through g->sp.
#define OUT_FULL 0.25f
// Stage 47 (D89): FM and PSG rows each at their chip's loudest as it holds, measured by reg_test --peaks
// (FM 0.12-0.14 at TL 0, PSG 0.042 at volume 0, noise 0.047; a note's first edge is higher and is
// cut at the box), so a channel turned up fills its box; the DAC keeps OUT_FULL.
#define OUT_FULL_FM 0.13f
#define OUT_FULL_PSG 0.045f
// A live trace in a box of the row (stage 24e, D60; stage 39 for the DAC): span samples of s from start
// over the box's inner columns, min and max a column, at one scale (OUT_FULL), the dark colour a pixel
// wider under the light one.
#define TRACE_W_MAX 128
static void draw_trace(mdmi_gui *g, int box_x, int box_w, int y, const float *s, int count, double start, int span, float full, uint32_t light, uint32_t dark)
{
	const int x0 = box_x + 1, w = box_w - 2 < TRACE_W_MAX ? box_w - 2 : TRACE_W_MAX, mid = y + 19;
	int tops[TRACE_W_MAX], bottoms[TRACE_W_MAX];
	for (int k = 0; k < w; k++) {
		const double a = start + (double)k * span / w, b = start + (double)(k + 1) * span / w;
		float lo, hi;
		if (b - a < 1) {
			const int i = (int)a;
			const double f = a - i;
			const float v = i + 1 < count ? (float)(s[i] + (s[i + 1] - s[i]) * f) : s[i];
			lo = hi = v;
		} else {
			lo = hi = s[(int)a];
			for (int i = (int)a; i < (int)b && i < count; i++) {
				lo = s[i] < lo ? s[i] : lo;
				hi = s[i] > hi ? s[i] : hi;
			}
		}
		int top = mid - (int)(hi / full * 13), bottom = mid - (int)(lo / full * 13);
		top = top < mid - 14 ? mid - 14 : top > mid + 14 ? mid + 14 : top;
		bottom = bottom < mid - 14 ? mid - 14 : bottom > mid + 14 ? mid + 14 : bottom;
		// Joined to the column before, so a step is a line, not a gap.
		if (k > 0) {
			top = bottoms[k - 1] < top ? bottoms[k - 1] : top;
			bottom = tops[k - 1] > bottom ? tops[k - 1] : bottom;
		}
		tops[k] = top;
		bottoms[k] = bottom;
	}
	for (int pass = 0; pass < 2; pass++) {
		for (int k = 0; k < w; k++) {
			const int top = tops[k] - (pass == 0), bottom = bottoms[k] + (pass == 0);
			fill(g, x0 + k, top, 1, bottom - top + 1, pass == 0 ? dark : light);
		}
	}
}

static void draw_out(mdmi_gui *g, const patch_values *values, int channel, int y, const mdmi_waves *waves, unsigned rate)
{
	fill(g, ALL_OUT_X, y + 4, ALL_OUT_W, ALL_OUT_H, C_BG);
	frame(g, ALL_OUT_X, y + 4, ALL_OUT_W, ALL_OUT_H, C_CELL);
	fill(g, ALL_OUT_X + 1, y + 19, ALL_OUT_W - 2, 1, C_CELL);
	double start;
	int span, key;
	if (!out_span(g, values, channel, waves, rate, &start, &span, &key)) {
		return;
	}
	const bool fm = channel_is_fm(channel);
	draw_trace(g, ALL_OUT_X, ALL_OUT_W, y, waves->samples[channel], (int)waves->count, start, span, fm ? OUT_FULL_FM : OUT_FULL_PSG, fm ? C_FM_LIGHT : C_PSG, fm ? C_FM_DARK : C_PSG_DARK);
}

// Stage 39 (art-director, ui-concept.md): the DAC row's live view, bit 6 of the ALL mask: the last
// 20 ms of channel 6's output in the sample picture's place, no anchor (the DAC has no pitch), at
// OUT_FULL as the other rows; silence is the frame and the middle line.
#define DAC_LIVE_BIT 6
#define DAC_LIVE_MS 20
static bool dac_live(const mdmi_gui *g)
{
	return *g->source.all_envelopes >> DAC_LIVE_BIT & 1;
}

// Whether the DAC's live output reaches a pixel, over the window drawn; the window's start and span.
static bool dac_live_span(const mdmi_waves *waves, unsigned rate, double *start, int *span)
{
	const int channel = PATCH_NUM_FM_CHANNELS - 1;
	if (!waves || rate == 0 || waves->count < 64) {
		return false;
	}
	*span = (int)(rate * DAC_LIVE_MS / 1000);
	*span = *span < (int)waves->count ? *span : (int)waves->count;
	*start = (double)((int)waves->count - *span);
	float peak = 0;
	for (int i = (int)*start; i < (int)waves->count; i++) {
		const float v = fabsf(waves->samples[channel][i]);
		peak = v > peak ? v : peak;
	}
	return peak >= OUT_FULL / 13;
}

static void draw_dac_live(mdmi_gui *g, int y, const mdmi_waves *waves, unsigned rate)
{
	fill(g, ALL_SOUND_X, y + 4, ALL_SOUND_W, ALL_OUT_H, C_BG);
	frame(g, ALL_SOUND_X, y + 4, ALL_SOUND_W, ALL_OUT_H, C_CELL);
	fill(g, ALL_SOUND_X + 1, y + 19, ALL_SOUND_W - 2, 1, C_CELL);
	double start;
	int span;
	if (dac_live_span(waves, rate, &start, &span)) {
		draw_trace(g, ALL_SOUND_X, ALL_SOUND_W, y, waves->samples[PATCH_NUM_FM_CHANNELS - 1], (int)waves->count, start, span, OUT_FULL, C_DAC, C_DAC_DARK);
	}
}

// The DAC's row on ALL (stage 24e): the last sample struck, from its start's tab to its end's, its loop
// on a grey ground, and a line for each sounding voice (D59): green on this sample, grey on another.
static void draw_all_dac(mdmi_gui *g, int y, const mdmi_waves *waves, unsigned rate)
{
	const int x0 = ALL_SOUND_X, w = ALL_SOUND_W, mid = y + 19;
	const uint32_t note = __atomic_load_n(&g->source.notes[PATCH_NUM_FM_CHANNELS - 1], __ATOMIC_ACQUIRE);
	rom_sample sample = {0};
	const char *name = NULL;
	bool file = false;
	const int key = GUI_NOTE_SEEN(note) ? (int)GUI_NOTE_KEY(note) : -1;
	const bool shown = key >= 0 && note_sample(g, key, &sample, &name, &file) && sample.length > 0;
	if (dac_live(g)) {
		// Stage 39: the live output instead of the sample; the sample's name stays.
		draw_dac_live(g, y, waves, rate);
		if (shown) {
			text_right(g, PAGE_RIGHT - 2, y + 8, name ? name : "", C_DAC, 2, 170);
		}
		return;
	}
	fill(g, x0, y + 36, w, 1, C_LINE);
	if (shown && sample.flags & (ROM_SAMPLE_LOOP | ROM_SAMPLE_MARKS)) {
		const uint32_t blocks = sample.length / ROM_SAMPLE_BLOCK;
		const uint32_t ls = sample.flags & ROM_SAMPLE_MARKS ? sample.loop_start : 0, le = sample.flags & ROM_SAMPLE_MARKS ? sample.loop_end : blocks;
		if (blocks > 0 && le > ls) {
			fill(g, x0 + (int)(ls * w / blocks), y + 3, (int)((le - ls) * w / blocks), 33, C_CELL);
		}
	}
	fill(g, x0, mid, w, 1, C_DAC_DARK);
	if (shown) {
		for (int k = 0; k < w; k++) {
			const uint32_t from = (uint32_t)((uint64_t)sample.length * k / w), to = (uint32_t)((uint64_t)sample.length * (k + 1) / w);
			int lo = 0, hi = 0;
			for (uint32_t i = from; i < to || i == from; i++) {
				lo = sample.bytes[i] < lo ? sample.bytes[i] : lo;
				hi = sample.bytes[i] > hi ? sample.bytes[i] : hi;
			}
			const int top = mid - hi * 15 / 128, bottom = mid - lo * 15 / 128;
			fill(g, x0 + k, top, 1, bottom - top + 1, C_DAC);
		}
		text_right(g, PAGE_RIGHT - 2, y + 8, name ? name : "", C_DAC, 2, 170);
	}
	const uint64_t now = g->source.psg_times ? __atomic_load_n(&g->source.psg_times->clock, __ATOMIC_ACQUIRE) : 0;
	for (int pass = 0; pass < 2; pass++) {
		for (int v = 0; v < PATCH_DAC_VOICES; v++) {
			int voice_key = 0;
			if (patch_dac_voice_state(g->source.patch, v, &voice_key) == PATCH_DAC_FREE) {
				continue;
			}
			const double place = patch_dac_voice_place(g->source.patch, v, now, rate);
			const bool mine = shown && voice_key == key;
			if (place < 0 || mine != (pass == 1)) {
				continue;
			}
			const int x = x0 + (int)(place * (w - 1));
			if (mine) {
				fill(g, x - 1, y + 2, 3, 35, C_BG);
			}
			fill(g, x, y + 2, 1, 35, mine ? C_PLAYING : C_HINT);
		}
	}
}

// What the footer says over ALL's OUT box, and over the DAC's VOL (stage 24e).
static bool out_hint(mdmi_gui *g, char *hint, size_t size)
{
	if (g->file_over || g->dragging || *g->source.page != PAGE_ALL) {
		return false;
	}
	const patch_values *values = patch_view(g->source.patch);
	const int channel = rail_channel_at(0, g->mouse_y);
	if (channel < 0) {
		return false;
	}
	if (is_dac(values, channel) && in_box(g->mouse_x, g->mouse_y, ALL_VOL_X, row_y(channel) + 4, GRID_W, CELL_H)) {
		snprintf(hint, size, "DAC VOL: SCALES NEW NOTES, NOT MDMI CANON");
		return true;
	}
	if (is_dac(values, channel) && in_box(g->mouse_x, g->mouse_y, ALL_SOUND_X, row_y(channel), ALL_SOUND_W, ROW_H)) {
		// Stage 39 (art-director): what the picture shows and what a click does.
		const uint32_t note = __atomic_load_n(&g->source.notes[PATCH_NUM_FM_CHANNELS - 1], __ATOMIC_ACQUIRE);
		double start;
		int span;
		if (dac_live(g)) {
			snprintf(hint, size, dac_live_span(mdmi_engine_latest_waves(g->source.engine), mdmi_engine_get_stats(g->source.engine).sample_rate, &start, &span)
				? "CH6 DAC OUT: LAST 20 MS. CLICK: SAMPLE" : "CH6 DAC OUT: SILENT. CLICK: SAMPLE");
		} else {
			snprintf(hint, size, GUI_NOTE_SEEN(note) ? "CH6 DAC: LAST SAMPLE. CLICK: LIVE OUT" : "CH6 DAC: NO SAMPLE YET. CLICK: LIVE OUT");
		}
		return true;
	}
	if (is_dac(values, channel) || !in_box(g->mouse_x, g->mouse_y, ALL_OUT_X, row_y(channel) + 4, ALL_OUT_W, ALL_OUT_H)) {
		return false;
	}
	const int key = lowest_key(g, values, channel);
	if (channel == PATCH_NUM_CHANNELS - 1) {
		snprintf(hint, size, "CH10 OUT: LAST 5 MS");
	} else if (key >= 0) {
		char name[8];
		note_name(key, name, sizeof(name));
		snprintf(hint, size, "CH%d OUT: 2 PERIODS OF %s", channel + 1, name);
	} else {
		snprintf(hint, size, "CH%d OUT: SILENT, NO NOTE HELD", channel + 1);
	}
	return true;
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
	} else if (channel_is_fm(channel) && values->globals[GLOBAL_MONO(channel)]) {
		// Stage 38: what sounds; with PORTA on, PORTA (it wins).
		text(g, x, y, "MONO", C_LABEL, 1);
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
	text(g, ALL_OUT_X, HEADER_H + 4, "OUT", C_HINT, 1);
	text(g, ALL_OUT_X, RAIL_PSG_Y - GROUP_H + 4, "OUT", C_HINT, 1);
	const mdmi_waves *waves = mdmi_engine_latest_waves(g->source.engine);
	for (int channel = 0; channel < PATCH_NUM_CHANNELS; channel++) {
		const int y = row_y(channel);
		// Channel 3 in Ch3 Special in its pink (D62); draw_editor puts FM's colours back after the page.
		g->sp = channel == 2 && is_special(values, 2);
		fill(g, RAIL_W + 1, y + ROW_H - 1, GUI_WIDTH - RAIL_W - 1, 1, C_CELL);
		if (is_dac(values, channel)) {
			draw_all_dac(g, y, waves, rate);
			continue;
		}
		draw_out(g, values, channel, y, waves, rate);
		if (channel_is_fm(channel)) {
			const int16_t *v = values->channels[channel];
			// The diagram or, after a click on it, the four envelopes in small, each row its own.
			if (*g->source.all_envelopes >> channel & 1) {
				draw_curves(g, values, channel, ALL_SOUND_X, y + 3, ALL_SOUND_W, 33, -1);
				fill(g, ALL_SOUND_X, y + 36, ALL_SOUND_W, 1, C_LINE);
				// Stage 24f: the last note's time, as the PSG rows have it.
				const double cursor = fm_cursor_seconds(g, channel);
				if (cursor >= 0) {
					fill(g, ALL_SOUND_X + curve_x(cursor, ALL_SOUND_W), y + 2, 1, 35, C_PLAYING);
				}
			} else {
				draw_mini_diagram(g, ALL_SOUND_X, y, v[PARAM_ALGORITHM], v[PARAM_FEEDBACK], is_special(values, channel) ? C_FM_LIGHT : C_FM);
			}
		} else {
			// Where the note is in what plays, its column lit (stage 41), reckoned as the editor does
			// (stage 49): a release kept while the loop stops is not played, so never lit.
			patch_shape shape, played;
			bool stop;
			channel_shape(g, channel, &shape, &stop);
			played_shape(&shape, stop, &played);
			draw_mini_envelope(g, ALL_SOUND_X, y + 3, ALL_SOUND_W, 33, &played, envelope_frame(g, &played, channel - PATCH_NUM_FM_CHANNELS, rate));
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
		g->sp = false;
	} else {
		g->bank_was_open = false;
		g->sp = *g->source.channel == 2 && is_special(values, 2);
		draw_page(g, values);
		g->sp = false;
	}
	for (int channel = 0; channel < PATCH_NUM_CHANNELS; channel++) {
		g->sp = channel == 2 && is_special(values, 2);
		draw_row(g, values, channel);
		g->sp = false;
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
	if (g->pitch_open >= 0) {
		g->sp = is_special(values, 2);
		draw_pitch_editor(g, values);
		g->sp = false;
	}
	draw_globals_dim(g, values);
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
	// Stage 36 (D81): the TV look reads the frame and writes the window's pixels, nothing else. Its
	// model is H40's pixel clock: another width draws as OFF.
	const int look = *g->source.tv_look;
	if (look != TV_OFF && rom->width == 320 && width == 320 * ROM_SCALE && height == (int)rom->height * ROM_SCALE) {
		if (!g->tv) {
			g->tv = tv_cache_new();
		}
		if (g->tv) {
			tv_draw(g->tv, look, rom->pixels, MDMI_FRAME_MAX_WIDTH, (int)rom->height, g->pixels + top * GUI_WIDTH + left, GUI_WIDTH);
			return;
		}
	}
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
static void action_button(mdmi_gui *g, int action, const char *label, bool enabled)
{
	const bool over = enabled && !g->file_over && action_at(g, g->mouse_x, g->mouse_y) == action;
	const int x = ACTION_X[action], w = ACTION_WIDTH[action];
	fill(g, x, BUTTON_Y, w, BUTTON_H, C_BG);
	frame(g, x, BUTTON_Y, w, BUTTON_H, !enabled ? C_CELL : over ? C_TEXT : C_LINE);
	const int width = (int)strlen(label) * 12;
	text(g, x + (w - width) / 2, BUTTON_Y + 5, label, !enabled ? C_LINE : over ? C_TEXT : C_LABEL, 2);
}

// The emulator's ROM screen names what the channels play (stage 24): handed to the engine when they
// change, while the page shows it.
static void send_screen_names(mdmi_gui *g)
{
	rom_names *names = malloc(sizeof(*names));
	if (names && files_screen_names(g->source.files, names) && (!g->screen_names_sent || memcmp(names, &g->screen_names, sizeof(*names)))
		&& mdmi_engine_set_names(g->source.engine, names)) {
		g->screen_names = *names;
		g->screen_names_sent = true;
	}
	free(names);
}

// Stage 36 (D81, docs/design/tv-look.md 1): the ROM page's TV switch between the header and the
// picture: the label TV, then OFF, RGB and COMP.
#define TV_SW_Y 32
#define TV_SW_H 18
static const int TV_CELL_X[4] = {542, 568, 604, 640}, TV_CELL_W[4] = {24, 34, 34, 40};
static const char *const TV_CELL_TEXT[4] = {"TV", "OFF", "RGB", "COMP"};

// The cell of the TV switch at x, y: 0 the label, 1-3 a look, -1 none.
static int tv_cell_at(int x, int y)
{
	for (int i = 0; i < 4 && y >= TV_SW_Y && y < TV_SW_Y + TV_SW_H; i++) {
		if (x >= TV_CELL_X[i] && x < TV_CELL_X[i] + TV_CELL_W[i]) {
			return i;
		}
	}
	return -1;
}

static void draw_tv_switch(mdmi_gui *g)
{
	const int over = g->file_over ? -1 : tv_cell_at(g->mouse_x, g->mouse_y);
	for (int i = 0; i < 4; i++) {
		const bool chosen = i > 0 && i - 1 == *g->source.tv_look;
		const uint32_t back = chosen ? C_TEXT : i > 0 && i == over ? C_LINE : C_CELL;
		const uint32_t ink = chosen ? C_BG : i > 0 && i == over ? C_TEXT : C_LABEL;
		fill(g, TV_CELL_X[i], TV_SW_Y, TV_CELL_W[i], TV_SW_H, back);
		text(g, TV_CELL_X[i] + (TV_CELL_W[i] - (int)strlen(TV_CELL_TEXT[i]) * 6 + 1) / 2, TV_SW_Y + 6, TV_CELL_TEXT[i], ink, 1);
	}
}

static bool tv_hint(mdmi_gui *g, char *hint, size_t size)
{
	if (g->file_over || *g->source.page != PAGE_ROM) {
		return false;
	}
	static const char *const HINTS[4] = {"TV OFF: ROM PIXELS AS THEY ARE", "TV OFF: ROM PIXELS AS THEY ARE", "RGB: CONSOLE ON A CRT BY SCART", "COMP: COMPOSITE TV, BLUR AND RAINBOW"};
	const int cell = tv_cell_at(g->mouse_x, g->mouse_y);
	if (cell < 0) {
		return false;
	}
	snprintf(hint, size, "%s", HINTS[cell]);
	return true;
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
	action_button(g, ACTION_LOAD, "LOAD", true);
	action_button(g, ACTION_SAVE, "SAVE", true);
	action_button(g, ACTION_UNDO, "UNDO", files_undo_text(g->source.files) != NULL || files_redo_text(g->source.files) != NULL);
	if (page == PAGE_BANK) {
		action_button(g, ACTION_ADD, "ADD", files_add_place(g->source.files, !channel_is_fm(*g->source.channel)) < PATCH_BANK_SIZE);
	}
	if (shows_store(g)) {
		action_button(g, ACTION_STORE, "STORE", files_can_store(g->source.files, *g->source.channel));
	}
	// Stage 24e: the outputs' last samples are copied only while ALL shows them.
	mdmi_engine_watch_waves(g->source.engine, page == PAGE_ALL);
	if (rom) {
		action_button(g, ACTION_EXPORT, "EXPORT", !g->exporting);
		send_screen_names(g);
		g->cell_count = 0;
		draw_rom(g);
		draw_tv_switch(g);
	} else {
		draw_editor(g);
	}
	draw_footer(g);
	// Stage 43: the dragged channel's name at the mouse, as a dragged BANK row's.
	if (copy_dragged(g) && !rom && g->mouse_x >= 0) {
		char buffer[32];
		snprintf(buffer, sizeof(buffer), "%.13s", sound_name(patch_view(g->source.patch), g->copy_from));
		fill(g, g->mouse_x + 8, g->mouse_y - 10, (int)strlen(buffer) * 12 + 8, 20, C_TEXT);
		text(g, g->mouse_x + 12, g->mouse_y - 7, buffer, C_BG, 2);
	}
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
// Channel 3's operator Semi and Fine take wider steps than their ranges give (stage 22a): a
// semitone or a cent has to be hit exactly.
static int drag_value(uint32_t id, int start, int max, int dy)
{
	int value;
	const int field = is_ch3_op_global(id) ? (int)(id - GLOBAL_CH3_OP_BASE) % PATCH_CH3_OP_FIELDS : -1;
	if (field == PATCH_CH3_SEMI || field == PATCH_CH3_FINE) {
		value = start + dy / (field == PATCH_CH3_SEMI ? 6 : 3);
	} else if (max > 127) {
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
	g->flash_list = -1;
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
static void put_places(mdmi_gui *g, const char *const *paths, int count, int beyond, bool envelopes, int first)
{
	char message[FILES_MESSAGE];
	if (first < 0) {
		first = files_add_place(g->source.files, envelopes);
	}
	int skipped;
	const int loaded = files_put_places(g->source.files, paths, count, beyond, envelopes, first, &skipped, message, sizeof(message));
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
	int beyond = 0;
	for (; *name; name += wcslen(name) + 1) {
		beyond++;
	}
	free(buffer);

	const char *paths[PATCH_BANK_SIZE];
	for (int i = 0; i < count; i++) {
		paths[i] = g->file_paths[i];
	}
	if (!bank || (count == 1 && !add && files_kind(paths[0]) != (envelopes ? FILES_ENVELOPE : FILES_INSTRUMENT))) {
		open_file(g, paths[0], channel);
	} else if (add) {
		put_places(g, paths, count, beyond, envelopes, -1);
	} else if (!takes_files(values, channel)) {
		say(g, "Select another CH to choose a slot", C_ERROR, ERROR_TICKS);
	} else {
		put_places(g, paths, count, beyond, envelopes, values->channels[channel][PARAM_PRESET]);
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
	// Stage 37: with a default extension the dialog adds the chosen type's own (it follows the type
	// list) before it asks whether to replace a file; adding it after the dialog replaced one unasked.
	dialog.lpstrDefExt = envelope ? L"eef" : L"vgi";
	dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
	if (!GetSaveFileNameW(&dialog)) {
		return;
	}
	const int chosen = dialog.nFilterIndex >= 1 && dialog.nFilterIndex <= 3 ? (int)dialog.nFilterIndex - 1 : 1;
	char given[MAX_PATH * 3], utf8[MAX_PATH * 3 + 8], message[FILES_MESSAGE];
	to_utf8(path, given, sizeof(given));
	int kind;
	if (files_save_path(given, chosen, envelope, utf8, sizeof(utf8), &kind)) {
		// Stage 49: the dialog asked about the name without the extension added here.
		wchar_t question[MAX_PATH * 2 + 64];
		wchar_t written[MAX_PATH * 2];
		MultiByteToWideChar(CP_UTF8, 0, utf8, -1, written, MAX_PATH * 2);
		const wchar_t *slash = wcsrchr(written, L'\\');
		_snwprintf(question, sizeof(question) / sizeof(*question), L"%s already exists.\nDo you want to replace it?", slash ? slash + 1 : written);
		question[sizeof(question) / sizeof(*question) - 1] = 0;
		if (MessageBoxW(g->window, question, L"Save", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
			return;
		}
	}
	bool done = files_save(g->source.files, utf8, kind, sound ? channel : -1, message, sizeof(message));
	say(g, message, done ? C_TEXT : C_ERROR, done ? MESSAGE_TICKS : ERROR_TICKS);
}

// EXPORT: the ROM for the SD card, named in the save dialog, or in MDMI_TEST_EXPORT for tests.
static void export_dialog(mdmi_gui *g)
{
	char path[MAX_PATH * 3 + 8], message[FILES_MESSAGE];
	const char *test = getenv("MDMI_TEST_EXPORT");
	if (test && *test) {
		snprintf(path, sizeof(path), "%s", test);
	} else {
		wchar_t file[MAX_PATH] = L"MDMI Slot.bin";
		OPENFILENAMEW dialog = {0};
		dialog.lStructSize = sizeof(dialog);
		dialog.hwndOwner = g->window;
		dialog.lpstrFilter = L"Mega Drive ROM (*.bin)\0*.bin\0";
		dialog.lpstrFile = file;
		dialog.nMaxFile = MAX_PATH;
		dialog.lpstrDefExt = L"bin";
		dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
		g->exporting = true;
		refresh(g);
		const bool chosen = GetSaveFileNameW(&dialog);
		g->exporting = false;
		if (!chosen) {
			return;
		}
		to_utf8(file, path, MAX_PATH * 3);
	}
	const bool done = files_export(g->source.files, path, message, sizeof(message));
	say(g, message, !done ? C_ERROR : strstr(message, "not in ROM") ? C_AMBER : C_TEXT, done ? MESSAGE_TICKS : ERROR_TICKS);
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
	} else if (action == ACTION_EXPORT) {
		export_dialog(g);
	} else if (action == ACTION_STORE) {
		// Off when the channel is its place as it is: the mouse does nothing there.
		if (files_can_store(g->source.files, *g->source.channel) && files_store(g->source.files, *g->source.channel, message, sizeof(message))) {
			say(g, message, C_TEXT, MESSAGE_TICKS);
		}
	} else if (action == ACTION_UNDO) {
		message[0] = 0;
		// Stage 48 (D90): Shift takes the step ahead again. Up and Down start from the Preset as it is.
		g->key_channel = -1;
		if (!g->shift && !files_undo_text(g->source.files) && files_redo_text(g->source.files)) {
			say(g, "Nothing to undo. SHIFT+CLICK: REDO", C_TEXT, MESSAGE_TICKS);
		} else if (g->shift ? files_redo(g->source.files, message, sizeof(message)) : files_undo(g->source.files, message, sizeof(message))) {
			say(g, message, C_TEXT, MESSAGE_TICKS);
		} else if (!strncmp(message, "Can't undo", 10)) {
			say(g, message, C_ERROR, ERROR_TICKS);
		}
		// A library's place may have gone back in its file (stage 22c).
		if (*g->source.page == PAGE_BANK) {
			load_lists(g);
		}
	}
	refresh(g);
}

// Stage 22c (D56): what the lists of BANK do. A flash on a library's place, or on the whole list.
static void flash_list(mdmi_gui *g, int list, int place, bool done)
{
	flash(g, -1, place, 1, done);
	g->flash_list = list;
}

// Whether path is a file right in the library folder; / and \ count alike, case doesn't.
static bool in_folder(const char *folder, const char *path)
{
	size_t parent = strlen(path);
	while (parent > 0 && path[parent - 1] != '\\' && path[parent - 1] != '/') {
		parent--;
	}
	size_t length = strlen(folder);
	while (length > 0 && (folder[length - 1] == '\\' || folder[length - 1] == '/')) {
		length--;
	}
	if (parent == 0 || parent - 1 != length) {
		return false;
	}
	for (size_t i = 0; i < length; i++) {
		const char a = folder[i] == '/' ? '\\' : folder[i], b = path[i] == '/' ? '\\' : path[i];
		if (tolower((unsigned char)a) != tolower((unsigned char)b)) {
			return false;
		}
	}
	return true;
}

// NEW: an empty library named in the save dialog, opened in the list it was asked from. Tests name
// the file in MDMI_TEST_NEW instead of the dialog.
static void new_library(mdmi_gui *g, int list)
{
	char message[FILES_MESSAGE], path[LIBRARY_PATH + 8];
	if (!library_make_folder(g->lib_folder)) {
		snprintf(message, sizeof(message), "Can't make the folder %s", g->lib_folder);
		say(g, message, C_ERROR, ERROR_TICKS);
		return;
	}
	const char *test = getenv("MDMI_TEST_NEW");
	if (test && *test) {
		snprintf(path, LIBRARY_PATH, "%s", test);
	} else {
		wchar_t file[MAX_PATH] = L"", folder[LIBRARY_PATH];
		MultiByteToWideChar(CP_UTF8, 0, g->lib_folder, -1, folder, LIBRARY_PATH);
		OPENFILENAMEW dialog = {0};
		dialog.lStructSize = sizeof(dialog);
		dialog.hwndOwner = g->window;
		dialog.lpstrFilter = L"Bank (*.mdbank)\0*.mdbank\0";
		dialog.lpstrFile = file;
		dialog.nMaxFile = MAX_PATH;
		dialog.lpstrInitialDir = folder;
		dialog.lpstrDefExt = L"mdbank";
		dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
		if (!GetSaveFileNameW(&dialog)) {
			return;
		}
		to_utf8(file, path, LIBRARY_PATH);
	}
	if (files_kind(path) != FILES_BANK) {
		strcat(path, ".mdbank");
	}
	if (!files_new_library(path, message, sizeof(message))) {
		say(g, message, C_ERROR, ERROR_TICKS);
		flash_list(g, list, -1, false);
		return;
	}
	if (!in_folder(g->lib_folder, path)) {
		say(g, "Saved outside MDMI Banks: not a library", C_AMBER, MESSAGE_TICKS);
		return;
	}
	const char *name = path + strlen(path);
	while (name > path && name[-1] != '\\' && name[-1] != '/') {
		name--;
	}
	gui_banks *banks = banks_of(g);
	snprintf(list ? banks->right : banks->left, LIBRARY_NAME, "%s", name);
	load_lists(g);
	say(g, message, C_AMBER, MESSAGE_TICKS);
}

static void open_chooser(mdmi_gui *g, int list)
{
	load_lists(g);
	g->chooser = list;
	g->chooser_top = 0;
	if (g->lib_list.total > g->lib_list.count) {
		char message[FILES_MESSAGE];
		snprintf(message, sizeof(message), "%d of %d libraries shown, by name", g->lib_list.count, g->lib_list.total);
		say(g, message, C_AMBER, MESSAGE_TICKS);
	}
}

static void chooser_down(mdmi_gui *g, int x, int y)
{
	const int choice = choice_at(g, x, y), list = g->chooser;
	gui_banks *banks = banks_of(g);
	if (choice == CHOICE_MORE || (choice >= 0 && open_elsewhere(g, choice))) {
		return;
	}
	if (x >= SCROLL_X && x < SCROLL_X + 6 && y >= BANK_Y && y < BANK_BOTTOM) {
		const int most = choice_count(g) - BANK_ROWS;
		g->chooser_top += y < BANK_Y + g->chooser_top * SCROLL_H / choice_count(g) ? -BANK_ROWS : BANK_ROWS;
		g->chooser_top = g->chooser_top > most ? most : g->chooser_top;
		g->chooser_top = g->chooser_top < 0 ? 0 : g->chooser_top;
		return;
	}
	g->chooser = -1;
	if (choice == CHOICE_NEW) {
		new_library(g, list);
	} else if (choice == CHOICE_ROM) {
		banks->left[0] = 0;
		load_lists(g);
	} else if (choice >= 0) {
		snprintf(list ? banks->right : banks->left, LIBRARY_NAME, "%s", g->lib_list.names[choice]);
		load_lists(g);
		if (g->lists[list].kind == LIST_UNREAD) {
			char message[FILES_MESSAGE];
			snprintf(message, sizeof(message), "%s: not an MDMI bank", g->lists[list].name);
			say(g, message, C_ERROR, ERROR_TICKS);
		}
	}
}

// A row dragged onto the other list: into the ROM bank's place under the mouse, or a library's first
// empty one, so nothing of a library is replaced by a miss (art-director's frame).
static void drop_row(mdmi_gui *g)
{
	const bool envelopes = !channel_is_fm(*g->source.channel);
	int place;
	const int to = drag_target(g, envelopes, &place);
	if (to < 0) {
		return;
	}
	char message[FILES_MESSAGE];
	if (g->lists[to].kind == LIST_LIBRARY && place < 0) {
		snprintf(message, sizeof(message), "%s is full to 128: clear a sound first", g->lists[to].name);
		say(g, message, C_LABEL, MESSAGE_TICKS);
		return;
	}
	if (place < 0) {
		return;
	}
	// Into a library the place is chosen in its file as it is now: another window may have filled
	// the one this shows as free (review of stage 22c).
	const bool rom = g->lists[to].kind == LIST_ROM;
	const files_spot from = spot_of(g, g->row_list, g->row_place), into = spot_of(g, to, rom ? place : -1);
	const bool done = files_copy_place(g->source.files, envelopes, &from, &into, &place, message, sizeof(message));
	say(g, message, done ? C_AMBER : C_ERROR, done ? MESSAGE_TICKS : ERROR_TICKS);
	load_lists(g);
	list_rows(g, envelopes);
	if (rom) {
		flash(g, -1, place, 1, done);
		return;
	}
	flash_list(g, to, done ? place : -1, done);
	for (int row = 0; done && row < g->lists[to].count; row++) {
		if (g->lists[to].places[row] == place && (row < g->lists[to].top || row >= g->lists[to].top + BANK_ROWS)) {
			scroll_list(g, to, row - 3);
		}
	}
}

// The library place a row shows still holds that sound in its file; otherwise the lists are read
// again and the footer says so (review of stage 22c: another window may have changed it).
static bool still_shown(mdmi_gui *g, int list, int place, bool envelopes)
{
	char name[PATCH_NAME];
	snprintf(name, sizeof(name), "%s", place_name(&g->lists[list].bank, envelopes, place));
	load_lists(g);
	list_rows(g, envelopes);
	const bank_list *l = &g->lists[list];
	if (l->kind == LIST_LIBRARY && !files_place_empty(&l->bank, envelopes, place) && !strcmp(place_name(&l->bank, envelopes, place), name)) {
		return true;
	}
	char message[FILES_MESSAGE];
	snprintf(message, sizeof(message), "%s changed elsewhere: shown again", name);
	say(g, message, C_LABEL, MESSAGE_TICKS);
	return false;
}

// A click on a row, the mouse up where it went down: a ROM bank place becomes the channel's Preset,
// a library's sound goes to the channel as a dropped file (D25). A drag does neither, so the sound
// the channel had and its undo step stay (review of stage 22c).
static void click_row(mdmi_gui *g, int list, int place)
{
	const int channel = *g->source.channel;
	const bool envelopes = !channel_is_fm(channel);
	if (g->lists[list].kind == LIST_ROM) {
		if (place != patch_view(g->source.patch)->channels[channel][PARAM_PRESET]) {
			files_preset_step(g->source.files, channel, place);
		}
		edit_once(g, PATCH_CHANNEL_ID(channel, PARAM_PRESET), place);
		return;
	}
	if (g->lists[list].kind != LIST_LIBRARY || !still_shown(g, list, place, envelopes)) {
		return;
	}
	char message[FILES_MESSAGE];
	const files_spot spot = spot_of(g, list, place);
	if (files_try_place(g->source.files, channel, &spot, message, sizeof(message))) {
		g->tried_channel = channel;
		g->tried_place = place;
		snprintf(g->tried_path, sizeof(g->tried_path), "%s", g->lists[list].path);
	} else {
		say(g, message, C_ERROR, ERROR_TICKS);
	}
}

// A press on BANK's lists, their bars or the choice. True when the page took it.
static bool bank_down(mdmi_gui *g, int x, int y)
{
	const int channel = *g->source.channel;
	const bool envelopes = !channel_is_fm(channel);
	list_rows(g, envelopes);
	if (g->chooser >= 0) {
		if (bar_under(x, y) >= 0 || choice_at(g, x, y) != -3 || (x >= SCROLL_X && x < SCROLL_X + 6)) {
			chooser_down(g, x, y);
		} else {
			g->chooser = -1;
		}
		return true;
	}
	const int bar = bar_under(x, y);
	if (bar >= 0) {
		open_chooser(g, bar);
		return true;
	}
	char message[FILES_MESSAGE];
	for (int list = 0; list < 2; list++) {
		if (over_new(g, list, x, y)) {
			new_library(g, list);
			return true;
		}
		const int place = list_place_at(g, list, x, y);
		if (place >= 0) {
			if (g->lists[list].kind == LIST_LIBRARY && over_clear(list, x, y)) {
				if (!still_shown(g, list, place, envelopes)) {
					return true;
				}
				const files_spot spot = spot_of(g, list, place);
				const bool done = files_clear_place(g->source.files, envelopes, &spot, message, sizeof(message));
				say(g, message, done ? C_AMBER : C_ERROR, done ? MESSAGE_TICKS : ERROR_TICKS);
				if (!done) {
					flash_list(g, list, place, false);
				}
				load_lists(g);
				return true;
			}
			// What the press does waits for the mouse to go up: a click or a drag (click_row, drop_row).
			g->row_list = list;
			g->row_place = place;
			g->row_x = x;
			g->row_y = y;
			g->row_moved = false;
			SetCapture(g->window);
			return true;
		}
		const int track = list_scroll_x(list);
		if (x >= track && x < track + 6 && y >= BANK_Y && y < BANK_BOTTOM && g->lists[list].count > BANK_ROWS) {
			const int top = *list_top(g, list);
			const int thumb = BANK_Y + top * SCROLL_H / g->lists[list].count;
			scroll_list(g, list, top + (y < thumb ? -BANK_ROWS : BANK_ROWS));
			return true;
		}
	}
	return x >= BANK_X && y >= BANK_BAR_Y && y < BANK_BOTTOM;
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
	// The pitch editor open: its cells work as any, a click anywhere else only closes it (stage 21).
	if (g->pitch_open >= 0 && !in_pitch_editor(g, x, y)) {
		g->press_eaten = true;
		g->pitch_open = -1;
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
		// Stage 36: a look of the TV switch, a view like the page (no undo, no knob).
		const int cell = g->file_over ? -1 : tv_cell_at(x, y);
		if (cell > 0) {
			*g->source.tv_look = (uint8_t)(cell - 1);
			refresh(g);
		}
		return;
	}
	int channel = rail_channel_at(x, y);
	if (channel >= 0) {
		*g->source.channel = channel;
		// Stage 43: the press may become a drag that copies the sound; never from the DAC.
		if (!is_dac(patch_view(g->source.patch), channel) && !g->file_over) {
			g->copy_from = channel;
			g->copy_x = x;
			g->copy_y = y;
			g->copy_moved = false;
			if (g->window) {
				SetCapture(g->window);
			}
		}
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
	if (*g->source.page == PAGE_BANK && !is_dac(patch_view(g->source.patch), *g->source.channel) && bank_down(g, x, y)) {
		refresh(g);
		return;
	}
	g->mouse_x = x;
	g->mouse_y = y;
	if (over_arp_switch(g, x, y)) {
		g->arp_view = !g->arp_view;
		refresh(g);
		return;
	}
	if (arp_down(g, x, y) || (is_envelope_page(g) && envelope_down(g, x, y))) {
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
			if (x >= ALL_SOUND_X && x < ALL_SOUND_X + ALL_SOUND_W && is_dac(patch_view(g->source.patch), row)) {
				// Stage 39: the DAC's picture switches between the sample and the live output.
				*g->source.all_envelopes ^= (uint8_t)(1 << DAC_LIVE_BIT);
			} else if (x >= ALL_SOUND_X && x < ALL_SOUND_X + ALL_SOUND_W && takes_fm(patch_view(g->source.patch), row)) {
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
		g->ssg_channel = *g->source.channel;
		refresh(g);
		return;
	}
	if (c->kind == CELL_PITCH) {
		g->pitch_open = g->last_op = ((int)c->id - GLOBAL_CH3_OP_BASE) / PATCH_CH3_OP_FIELDS;
		refresh(g);
		return;
	}
	if (op_of(c->id) >= 0) {
		g->last_op = op_of(c->id);
	}
	if (c->kind == CELL_CHOICE) {
		edit_once(g, c->id, c->choice);
		g->chosen = true;
		g->chosen_id = c->id;
		g->chosen_value = c->choice;
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
	if (g->copy_from >= 0) {
		g->copy_moved = g->copy_moved || abs(x - g->copy_x) >= DRAG_SLOP || abs(y - g->copy_y) >= DRAG_SLOP;
		refresh(g);
		return;
	}
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
	if (g->arp_drag != ARP_NONE) {
		arp_move(g, x, y);
		return;
	}
	if (g->row_list >= 0) {
		g->row_moved = g->row_moved || abs(x - g->row_x) >= DRAG_SLOP || abs(y - g->row_y) >= DRAG_SLOP;
		refresh(g);
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
	int value = drag_value(g->drag_id, g->drag_start, patch_param(g->drag_id)->max, dy);
	if (value != g->drag_value) {
		g->drag_value = value;
		edit(g, GUI_EDIT_SET, g->drag_id, value);
	}
}

// Stage 38 (D83): the other of the pair MONO | PORTA of an FM channel's knob, or 0.
static uint32_t pair_other(uint32_t id)
{
	const int mono = mono_channel_of(id), porta = porta_channel_of(id);
	return mono >= 0 ? PATCH_CHANNEL_ID(mono, PARAM_PORTA_ON) : porta >= 0 ? (uint32_t)GLOBAL_MONO(porta) : 0;
}

// The pair's one rule, for a click, a drag and the wheel alike (stage 49): one of MONO and PORTA
// turned on turns the other off.
static void pair_rule(mdmi_gui *g, uint32_t id, int value)
{
	const uint32_t other = pair_other(id);
	if (other && value && patch_get(g->source.patch, other)) {
		edit_once(g, other, 0);
	}
}

static void on_up(mdmi_gui *g)
{
	if (g->copy_from >= 0) {
		const int from = g->copy_from, to = copy_dragged(g) ? copy_target(g) : -1;
		g->copy_from = -1;
		g->copy_moved = false;
		if (GetCapture() == g->window) {
			ReleaseCapture();
		}
		char message[FILES_MESSAGE] = "";
		if (to >= 0 && files_copy_channel(g->source.files, from, to, message, sizeof(message))) {
			*g->source.channel = to;
			flash(g, to, -1, 1, true);
			char said[FILES_MESSAGE + 32];
			snprintf(said, sizeof(said), "%s. UNDO TAKES IT BACK", message);
			say(g, said, C_AMBER, MESSAGE_TICKS);
		}
		refresh(g);
		return;
	}
	if (g->row_list >= 0) {
		if (g->row_moved) {
			drop_row(g);
		} else {
			click_row(g, g->row_list, g->row_place);
		}
		g->row_list = -1;
		g->row_moved = false;
		if (GetCapture() == g->window) {
			ReleaseCapture();
		}
		refresh(g);
		return;
	}
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
		dac_edit edit = g->dac_work;
		const int note = g->dac_drag_note;
		if (g->dac_drag == DACC_LOOP_BODY && !g->dac_moved) {
			// A click on the loop's row: the body turns LOOP off, the shadow on (stage 18b), which
			// takes ONE-SHOT off (stage 18) and comes back with the release (22b).
			edit = g->dac_drag_from;
			edit.loop = !edit.loop;
			edit.one_shot = edit.one_shot && !edit.loop;
			edit.stop = false;
		} else if ((g->dac_drag == DACC_LOOP_END || g->dac_drag == DACC_LOOP_TAIL) && !g->dac_moved) {
			// On the loop's end tab or after it: release or stop (stage 22b, D54); nothing to choose
			// without a release.
			const dac_sample *own = panel_sample(g, note);
			dac_marks m = {0};
			if (own) {
				dac_marks_of(own, &g->dac_drag_from, &m);
			}
			edit = g->dac_drag_from;
			if (m.tail) {
				edit.stop = !edit.stop;
				g->dac_tail_clicked = true;
			}
		}
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
	if (g->arp_drag != ARP_NONE) {
		arp_up(g);
		return;
	}
	if (!g->dragging) {
		return;
	}
	// A click without moving steps a short range round, which flips a switch.
	const int max = patch_param(g->drag_id)->max;
	// Stage 38 (art-director): MONO and PORTA are a pair, at most one the mode. A click on an unlit one,
	// or on an overruled MONO, makes it the mode and turns the other off; on the lit one, turns it off.
	const int mono = mono_channel_of(g->drag_id);
	const bool other_on = pair_other(g->drag_id) && patch_get(g->source.patch, pair_other(g->drag_id));
	if (!g->drag_moved && max + 1 <= 8) {
		const bool overruled = mono >= 0 && g->drag_start && other_on;
		g->drag_value = overruled ? 1 : g->drag_start < max ? g->drag_start + 1 : 0;
		edit(g, GUI_EDIT_SET, g->drag_id, g->drag_value);
	}
	edit(g, GUI_EDIT_END, g->drag_id, 0);
	// A lit PORTA clicked while MONO is set turns PORTA off only, so this is for turning one on; a
	// drag keeps the pair's rule as a click does (stage 49).
	if (!g->closing) {
		pair_rule(g, g->drag_id, g->drag_value);
	}
	g->dragging = false;
	if (GetCapture() == g->window) {
		ReleaseCapture();
	}
}

// Back to the value of the channel's preset, or the default for global knobs. The preset itself stays.
// Stage 42 (wheel-double-click.md): a double click on a choice the first click set puts back the
// preset's value, or a global's default. Windows takes a click on the next button, a few pixels on, as
// the second of a double click: then it chooses that button, as a click.
static void choice_reset(mdmi_gui *g, int index, int x, int y)
{
	const uint32_t id = g->cells[index].id;
	// The model may not have the first click's value yet: what that click chose is kept for this.
	if (!g->chosen || g->chosen_id != id || g->chosen_value != g->cells[index].choice) {
		on_down(g, x, y);
		return;
	}
	g->chosen = false;
	int channel, param;
	patch_id_split(id, &channel, &param);
	const int value = channel < 0 ? patch_param(id)->default_value
		: patch_preset_value(patch_bank_view(g->source.patch), channel, param, patch_get(g->source.patch, PATCH_CHANNEL_ID(channel, PARAM_PRESET)));
	if (value != g->cells[index].choice) {
		edit_once(g, id, value);
	}
}

static void on_double_click(mdmi_gui *g, int x, int y)
{
	g->mouse_x = x;
	g->mouse_y = y;
	// The first press set or closed the choice of SSG; what is under the mouse now is another cell.
	if (g->press_eaten) {
		g->press_eaten = false;
		return;
	}
	// Stage 48: a quick second click on UNDO is one more step back (or ahead with Shift), not lost.
	if (action_at(g, x, y) == ACTION_UNDO) {
		on_down(g, x, y);
		return;
	}
	// The global strip is the same on every page: a double click there puts a knob back on any of them.
	const bool globals = y >= GLOBALS_Y && y < FOOTER_Y;
	if (!globals && dac_editor(g, patch_view(g->source.patch))) {
		if (dac_panel_double_click(g, x, y)) {
			refresh(g);
		} else {
			on_down(g, x, y);
		}
		return;
	}
	// On the arp row a column's second press sets 0; the row's ON/OFF and the ARP view's switch took
	// the first click, one undo step (stage 24d); elsewhere on the row it is a press as the first was.
	if (!globals && (over_arp_switch(g, x, y) || arp_part_at(g, shown_arp(g), x, y) != ARP_NONE)) {
		const int part = arp_part_at(g, shown_arp(g), x, y);
		if (!over_arp_switch(g, x, y) && part != ARP_ON_OFF && part != ARP_SWITCH && part != ARP_BODY && part != ARP_SHADOW && !arp_zero(g, x, y)) {
			on_down(g, x, y);
		}
		refresh(g);
		return;
	}
	// Stage 24d: on the envelope a column's second press sets its default, in the click's undo step;
	// on the loop's strip, its tabs and the release the first click did it all; LENGTH steps again.
	patch_shape scratch;
	const int env_part = !globals && is_envelope_page(g) ? envelope_part_at(g, shown_shape(g, &scratch), x, y) : ENV_NONE;
	if (env_part != ENV_NONE) {
		if (env_part == ENV_LEVEL || env_part == ENV_PITCH) {
			envelope_reset(g, x, y, env_part);
		} else if (env_part == ENV_LENGTH) {
			on_down(g, x, y);
		}
		refresh(g);
		return;
	}
	// On ALL a quick second click on a row's sound switches again, as the first did.
	if (!globals && *g->source.page == PAGE_ALL && x >= ALL_SOUND_X && x < ALL_SOUND_X + ALL_SOUND_W && row_at(g, x, y) >= 0) {
		on_down(g, x, y);
		return;
	}
	// ALL's VOL takes a double click as on EDITOR, the preset's value (stage 24d, from stage 10's
	// parking); on PAN the first click chose the side, and the second does nothing more.
	int index = *g->source.page == PAGE_EDITOR || *g->source.page == PAGE_ALL || globals ? hovered(g) : -1;
	if (index >= 0 && *g->source.page == PAGE_ALL && !globals) {
		int channel = -1, param = -1;
		patch_id_split(g->cells[index].id, &channel, &param);
		if (channel >= 0 && param == PARAM_VOLUME) {
			edit_once(g, g->cells[index].id, patch_preset_value(patch_bank_view(g->source.patch), channel, param, patch_get(g->source.patch, PATCH_CHANNEL_ID(channel, PARAM_PRESET))));
		} else if (g->cells[index].kind == CELL_CHOICE) {
			choice_reset(g, index, x, y);
		}
		return;
	}
	// The choice of SSG and its button take a quick second click as a click.
	if (g->ssg_open >= 0 || (index >= 0 && (g->cells[index].kind == CELL_SSG || g->cells[index].kind == CELL_PITCH))) {
		on_down(g, x, y);
		return;
	}
	// Stage 21's cells take a double click as their default on every page: CH3 NOTES OPS, LFO RESET
	// Off, and SEMI, FINE, FIX of the pitch editor 0 and Off (docs/design/ch3-ops.md).
	if (index >= 0 && (g->cells[index].id == GLOBAL_CH3_NOTES || g->cells[index].id == GLOBAL_LFO_RESET || is_ch3_op_global(g->cells[index].id))) {
		edit_once(g, g->cells[index].id, patch_param(g->cells[index].id)->default_value);
		return;
	}
	if (index < 0) {
		return;
	}
	// Stage 42: PAN, ALG, DT, RS and the noise's choice go back to the preset's (or default) value.
	if (g->cells[index].kind == CELL_CHOICE) {
		choice_reset(g, index, x, y);
		return;
	}
	const uint32_t id = g->cells[index].id;
	int channel, param;
	patch_id_split(id, &channel, &param);
	if (channel >= 0 && param == PARAM_PRESET) {
		return;
	}
	// Stage 38: the second press on MONO or PORTA does nothing; a second step could drop the mode.
	if (mono_channel_of(id) >= 0 || porta_channel_of(id) >= 0) {
		return;
	}
	// Where a global's click steps, a quick second click is one more step on EDITOR, as ever; elsewhere
	// the strip took no double click before, and a second step of DAC would reset the chips twice. A
	// channel's short cells (FB, AMS, FMS, AM) take the preset's value as the rest do (stage 42).
	if (channel < 0 && patch_param(id)->max + 1 <= 8 && *g->source.page != PAGE_EDITOR) {
		return;
	}
	if (channel < 0 && patch_param(id)->max + 1 <= 8) {
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
	g->file_count = g->file_beyond = g->file_instruments = g->file_envelopes = g->file_samples = 0;
	g->file_paths[0][0] = g->file_name[0] = g->file_stem[0] = 0;
	FORMATETC format = {CF_HDROP, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
	STGMEDIUM medium;
	if (FAILED(IDataObject_GetData(data, &format, &medium))) {
		return;
	}
	HDROP drop = GlobalLock(medium.hGlobal);
	if (drop) {
		const int offered = (int)DragQueryFileW(drop, 0xFFFFFFFF, NULL, 0);
		int i = 0;
		for (; i < offered && g->file_count < PATCH_BANK_SIZE; i++) {
			wchar_t path[MAX_PATH];
			if (DragQueryFileW(drop, (UINT)i, path, MAX_PATH)) {
				char *utf8 = g->file_paths[g->file_count++];
				to_utf8(path, utf8, sizeof(g->file_paths[0]));
				g->file_instruments += files_kind(utf8) == FILES_INSTRUMENT;
				g->file_envelopes += files_kind(utf8) == FILES_ENVELOPE;
				g->file_samples += files_kind(utf8) == FILES_SAMPLE;
			}
		}
		g->file_beyond = offered - i;
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
	g->dac_drop_wave = false;
	const patch_values *values = patch_view(g->source.patch);
	if (g->file_samples > 0) {
		// WAV go only to the DAC page's notes, one after another from the row (stage 12), or from the
		// open note when dropped on its wave (stage 18).
		g->dac_drop_wave = dac_editor(g, values) && g->dac_selected >= 0 && in_box(x, y, WAVE_X, WAVE_Y, WAVE_W, WAVE_H);
		const int note = g->dac_drop_wave ? g->dac_selected : dac_editor(g, values) ? grid_note_at(x, y) : dac_bank(g, values) ? dac_note_at(g, x, y) : -1;
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
	// Stage 22c: files go into the ROM bank only, list A's places or list B as one ADD field.
	const bool in_list = page == PAGE_BANK && y >= BANK_Y && y < BANK_BOTTOM && x >= BANK_X;
	const bool rom_shown = g->lists[0].kind == LIST_ROM;
	const bool add = in_list && rom_shown && x >= LIST_B_X && x < LIST_B_BAR_RIGHT;
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
	} else if (in_list && !rom_shown) {
		hint(g, C_LABEL, "Files go to the ROM bank: choose ROM BANK on the left");
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
		put_places(g, paths, g->file_count, g->file_beyond, g->drop_envelopes, g->drop_add ? -1 : g->drop_place);
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
	const int from = pending ? g->key_to : playing;
	int place = from + (up ? -1 : 1);
	place = place < 0 ? 0 : place >= PATCH_BANK_SIZE ? PATCH_BANK_SIZE - 1 : place;
	if (!pending) {
		g->key_channel = channel;
		g->key_from = playing;
	}
	g->key_to = place;
	if (place != from) {
		files_preset_step(g->source.files, channel, place);
	}
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
	const int wheel_x = client.x / g->scale, wheel_y = client.y / g->scale;
	const bool lists = *g->source.page == PAGE_BANK && !is_dac(patch_view(g->source.patch), *g->source.channel);
	if (lists && g->chooser >= 0 && wheel_x >= BANK_X) {
		const int most = choice_count(g) - BANK_ROWS, top = g->chooser_top - delta / WHEEL_DELTA * WHEEL_ROWS;
		g->chooser_top = top > most ? most : top;
		g->chooser_top = g->chooser_top < 0 ? 0 : g->chooser_top;
		refresh(g);
	} else if (lists && list_under(wheel_x, wheel_y) >= 0) {
		// Stage 22c: the list under the mouse, during a drag too.
		const int list = list_under(wheel_x, wheel_y);
		scroll_list(g, list, *list_top(g, list) - delta / WHEEL_DELTA * WHEEL_ROWS);
		refresh(g);
	} else if (dac_bank(g, patch_view(g->source.patch)) && client.x / g->scale >= BANK_X) {
		scroll_dac(g, g->dac_top - delta / WHEEL_DELTA * WHEEL_ROWS);
		refresh(g);
	} else {
		// Stage 22a: a notch over a numeric cell steps it by one, a whole change to the host each.
		// Smooth wheels send parts of a notch; they add up to one.
		g->mouse_x = client.x / g->scale;
		g->mouse_y = client.y / g->scale;
		// Not while anything is dragged or a file is over the window (stage 42, wheel-double-click.md).
		const bool busy = g->dragging || g->row_list >= 0 || g->env_drag != ENV_NONE || g->arp_drag != ARP_NONE || g->dac_drag != DACC_NONE
			|| g->grid_from >= 0 || g->file_over || g->copy_from >= 0;
		// Stage 42: over a channel's row of the rail, or its PRESET, the wheel steps the Preset. ROM shows
		// neither the rail nor the channel page's cells: there the wheel does nothing.
		const int rail = busy || *g->source.page == PAGE_ROM ? -1 : rail_channel_at(g->mouse_x, g->mouse_y);
		const int index = busy || rail >= 0 || *g->source.page == PAGE_ROM ? -1 : hovered(g);
		uint32_t id;
		if (rail >= 0 && !is_dac(patch_view(g->source.patch), rail)) {
			id = PATCH_CHANNEL_ID(rail, PARAM_PRESET);
		} else if (index >= 0 && (g->cells[index].kind == CELL_ROW || g->cells[index].kind == CELL_COLUMN || g->cells[index].kind == CELL_SOURCE)) {
			id = g->cells[index].id;
		} else {
			g->wheel_rest = 0;
			return;
		}
		// A Preset steps like the bank list it names: the wheel down goes down the list, to the next place.
		if (rail >= 0 || g->cells[index].kind == CELL_SOURCE) {
			delta = -delta;
		}
		// Parts of a notch count for one cell only.
		if (id != g->wheel_rest_id) {
			g->wheel_rest = 0;
			g->wheel_rest_id = id;
		}
		g->wheel_rest += delta;
		const int notches = g->wheel_rest / WHEEL_DELTA;
		g->wheel_rest -= notches * WHEEL_DELTA;
		if (!notches) {
			return;
		}
		const int max = patch_param(id)->max;
		const int now = g->wheel_pending && g->wheel_id == id ? g->wheel_value : patch_get(g->source.patch, id);
		const int value = now + notches < 0 ? 0 : now + notches > max ? max : now + notches;
		if (value != now) {
			int channel, param;
			// Stage 48 (D90): a Preset step is an UNDO step, the channel's sound before it kept.
			if (patch_id_split(id, &channel, &param) && channel >= 0 && param == PARAM_PRESET) {
				files_preset_step(g->source.files, channel, value);
			}
			edit_once(g, id, value);
			pair_rule(g, id, value);
			g->wheel_pending = true;
			g->wheel_id = id;
			g->wheel_value = value;
		}
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
		if (g->wheel_pending && patch_get(g->source.patch, g->wheel_id) == g->wheel_value) {
			g->wheel_pending = false;
		}
		if (g->env_drag != ENV_NONE) {
			envelope_hand(g);
		}
		if (g->arp_drag != ARP_NONE) {
			arp_hand(g);
		}
		// A row dragged near the top or bottom of the other list moves that list a row a tick (stage 22c).
		if (g->row_list >= 0 && g->row_moved) {
			const int list = list_under(g->mouse_x, g->mouse_y);
			if (list >= 0 && list != g->row_list) {
				const int step = g->mouse_y < BANK_Y + LIST_EDGE ? -1 : g->mouse_y >= BANK_BOTTOM - LIST_EDGE ? 1 : 0;
				scroll_list(g, list, *list_top(g, list) + step);
			}
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
		// A press let go here is no click (stage 49): a row or a switch held stays as it was.
		g->row_list = -1;
		g->drag_moved = true;
		g->closing = true;
		on_up(g);
		g->closing = false;
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
		g->shift = (wparam & MK_SHIFT) != 0;
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
		if (!g->dragging && g->copy_from < 0) {
			g->mouse_x = g->mouse_y = -1;
		}
		return 0;
	case WM_CAPTURECHANGED:
	case WM_RBUTTONDOWN:
		// Stage 43: a copy's drag lost or cancelled by the right button copies nothing.
		if (g->copy_from >= 0) {
			g->copy_from = -1;
			g->copy_moved = false;
			if (message == WM_RBUTTONDOWN && GetCapture() == g->window) {
				ReleaseCapture();
			}
			refresh(g);
			return 0;
		}
		if (message == WM_RBUTTONDOWN) {
			return DefWindowProcA(window, message, wparam, lparam);
		}
		on_up(g);
		return 0;
	case WM_LBUTTONUP:
		on_up(g);
		return 0;
	case WM_LBUTTONDBLCLK:
		g->shift = (wparam & MK_SHIFT) != 0;
		on_double_click(g, x, y);
		return 0;
	case WM_SETCURSOR:
		// The trim marks are the window's one sideways drag.
		if (LOWORD(lparam) == HTCLIENT && dac_editor(g, patch_view(g->source.patch))) {
			const int c = g->dac_drag != DACC_NONE ? g->dac_drag : dac_control_at(g, g->mouse_x, g->mouse_y);
			if (c == DACC_MARK_START || c == DACC_MARK_END || c == DACC_LOOP_START || c == DACC_LOOP_END) {
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
	g->pitch_open = -1;
	g->dac_selected = DAC_DEFAULT_NOTE;
	g->dac_drag = DACC_NONE;
	g->wave_note = -1;
	g->grid_from = -1;
	g->rom_copy_note = -1;
	g->bank_seen_channel = g->drop_channel = g->drop_place = g->flash_channel = g->flash_place = -1;
	g->chooser = g->row_list = g->tried_channel = g->flash_list = -1;
	g->copy_from = -1;
	mdmi_engine_watch_frames(g->source.engine, true);
	return g;
}

void gui_destroy(mdmi_gui *g)
{
	if (!g) {
		return;
	}
	mdmi_engine_watch_frames(g->source.engine, false);
	mdmi_engine_watch_waves(g->source.engine, false);
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
	tv_cache_free(g->tv);
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
