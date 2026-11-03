#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
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
#define C_TARGET 0xEEEE22
#define C_ERROR 0xEE2222

// The layout of docs/design/ui-concept.md, "Окно 720x528".
#define HEADER_H 28
#define BUTTON_Y 2
#define BUTTON_W 80
#define BUTTON_H 24
#define BUTTON_EDITOR_X 60
#define BUTTON_ROM_X (BUTTON_EDITOR_X + BUTTON_W + 4)
#define STATUS_X 236
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
#define DIAGRAM_Y 80
#define OP_W 112
#define OP_HEADER_Y 178
#define DRAG_SLOP 3
#define MAX_CELLS 80
#define TIMER_PER_SECOND (1000 / TIMER_MS)

enum { PAGE_EDITOR, PAGE_ROM };
enum { CELL_ROW, CELL_COLUMN, CELL_ALGORITHM };

// A value the mouse can change. Rebuilt for each frame from what the page shows.
typedef struct {
	int x, y, w, h;
	uint32_t id;
	const char *label;
	int kind;
	int algorithm;
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

static void button(mdmi_gui *g, int x, const char *label, bool on)
{
	fill(g, x, BUTTON_Y, BUTTON_W, BUTTON_H, on ? C_TEXT : C_CELL);
	int width = (int)strlen(label) * 12;
	text(g, x + (BUTTON_W - width) / 2, BUTTON_Y + 5, label, on ? C_BG : C_LABEL, 2);
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

static int value_of(mdmi_gui *g, const patch_values *values, uint32_t id)
{
	if (g->dragging && id == g->drag_id) {
		return g->drag_value;
	}
	int channel, param;
	patch_id_split(id, &channel, &param);
	return channel < 0 ? values->globals[param] : values->channels[channel][param];
}

static void draw_row(mdmi_gui *g, const patch_values *values, int channel)
{
	const int y = row_y(channel);
	const bool dac = is_dac(values, channel);
	const bool selected = *g->source.channel == channel;
	const uint32_t kind = dac ? C_DAC : channel_is_fm(channel) ? C_FM : C_PSG;
	const uint32_t dark = dac ? C_DAC_DARK : channel_is_fm(channel) ? C_FM_DARK : C_PSG_DARK;

	fill(g, 0, y, RAIL_W, ROW_H - 1, C_BG);
	fill(g, 0, y + ROW_H - 1, RAIL_W, 1, C_CELL);
	if (selected) {
		fill(g, 4, y, RAIL_W - 3, ROW_H - 1, dark);
	}
	fill(g, 0, y, 4, ROW_H - 1, kind);

	char buffer[64];
	snprintf(buffer, sizeof(buffer), "%d", channel + 1);
	text(g, 10, y + 8, buffer, C_TEXT, 3);

	const char *tag = dac ? "DAC" : channel == PATCH_NUM_CHANNELS - 1 ? "NOISE"
		: !channel_is_fm(channel) ? "PSG"
		: channel == 2 && values->globals[GLOBAL_CH3_MODE] ? "FM SP" : "FM";
	text(g, 54, y + 7, tag, dac ? C_DAC : channel_is_fm(channel) ? C_FM_LIGHT : C_PSG, 1);

	if (dac) {
		text(g, 54, y + 21, "Samples", C_LABEL, 2);
	} else {
		patch_format_preset(channel, values->channels[channel][PARAM_PRESET], buffer, sizeof(buffer));
		const char *name = strchr(buffer, ' ') ? strchr(buffer, ' ') + 1 : buffer;
		char cut[16];
		snprintf(cut, sizeof(cut), "%.14s", name);
		text(g, 54, y + 21, cut, C_LABEL, 2);
	}

	uint32_t note = __atomic_load_n(&g->source.notes[channel], __ATOMIC_ACQUIRE);
	// A row whose voice another channel's chord plays shows that note and whose it is, not its own
	// last note.
	const int player = patch_voice_player(g->source.patch, channel);
	const bool lent = player >= 0 && player != channel && !(GUI_NOTE_SEEN(note) && GUI_NOTE_HELD(note));
	if (lent) {
		note_name(patch_voice_key(g->source.patch, channel), buffer, sizeof(buffer));
		const int left = RAIL_W - 6 - (int)strlen(buffer) * 12;
		text(g, left, y + 3, buffer, C_PLAYING, 2);
		snprintf(buffer, sizeof(buffer), "CH%d", player + 1);
		text(g, left - 4 - (int)strlen(buffer) * 6, y + 7, buffer, C_LABEL, 1);
	} else if (GUI_NOTE_SEEN(note)) {
		const int held = (int)GUI_NOTE_HELD(note);
		note_name(GUI_NOTE_KEY(note), buffer, sizeof(buffer));
		int left = RAIL_W - 6 - (int)strlen(buffer) * 12;
		text(g, left, y + 3, buffer, held ? C_PLAYING : C_HINT, 2);
		if (held > 1) {
			snprintf(buffer, sizeof(buffer), "x%d", held);
			left -= 4 + (int)strlen(buffer) * 6;
			text(g, left, y + 7, buffer, C_PLAYING, 1);
		}
		// Held notes without a voice: cut off by a tune, or more than the free voices (D20).
		const int muted = patch_muted_notes(g->source.patch, channel);
		if (muted > 0) {
			snprintf(buffer, sizeof(buffer), "%d CUT", muted);
			text(g, left - 6 - (int)strlen(buffer) * 6, y + 7, buffer, C_ERROR, 1);
		}
	}
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
		g->cells[g->cell_count++] = (cell){x, y, w, h, id, label, kind, 0};
	}
}

// Operator fields in the order the page shows them: level and pitch, times, the rest.
static const struct {
	const char *label;
	int field;
	int y;
} OP_CELLS[PATCH_OP_PARAMS] = {
	{"TL", 0, 202}, {"MUL", 1, 224}, {"DT", 2, 246},
	{"AR", 4, 272}, {"DR", 5, 294}, {"SL", 7, 316}, {"SR", 6, 338}, {"RR", 8, 360},
	{"RS", 3, 386}, {"AM", 9, 408}, {"SSG", 10, 430},
};

// The cells of the open page and the global knobs, so drawing and the mouse agree.
static void layout(mdmi_gui *g, const patch_values *values)
{
	static const struct {
		int param;
		const char *label;
		int x, y;
	} CHANNEL_CELLS[] = {
		{PARAM_VOLUME, "VOL", 472, 58}, {PARAM_PAN, "PAN", 592, 58},
		{PARAM_FINE_TUNE, "TUNE", 472, 80}, {PARAM_BEND_RANGE, "BEND", 592, 80},
		{PARAM_PORTA_ON, "PORTA", 472, 102}, {PARAM_PORTA_TIME, "P.TIME", 592, 102},
		{PARAM_FEEDBACK, "FB", 472, 128}, {PARAM_AMS, "AMS", 592, 128},
		{PARAM_FMS, "FMS", 472, 150},
	};
	static const struct {
		int global;
		const char *label;
	} GLOBAL_CELLS[] = {
		{GLOBAL_LFO_ON, "LFO"}, {GLOBAL_LFO_RATE, "LFO RATE"},
		{GLOBAL_FILTER, "FILTER"}, {GLOBAL_CH3_MODE, "CH3 SPECIAL"}, {GLOBAL_DAC, "DAC"},
	};
	g->cell_count = 0;
	const int channel = *g->source.channel;
	if (!is_dac(values, channel)) {
		add_cell(g, 288, 32, 272, CELL_H, PATCH_CHANNEL_ID(channel, PARAM_PRESET), "PRESET", CELL_ROW);
		for (size_t i = 0; i < sizeof(CHANNEL_CELLS) / sizeof(*CHANNEL_CELLS); i++) {
			if (patch_channel_has(channel, CHANNEL_CELLS[i].param)) {
				add_cell(g, CHANNEL_CELLS[i].x, CHANNEL_CELLS[i].y, 116, CELL_H, PATCH_CHANNEL_ID(channel, CHANNEL_CELLS[i].param), CHANNEL_CELLS[i].label, CELL_ROW);
			}
		}
		if (channel_is_fm(channel)) {
			for (int algorithm = 0; algorithm < 8; algorithm++) {
				add_cell(g, PAGE_X + algorithm * 28, 58, 26, CELL_H, PATCH_CHANNEL_ID(channel, PARAM_ALGORITHM), "", CELL_ALGORITHM);
				g->cells[g->cell_count - 1].algorithm = algorithm;
			}
			for (int op = 0; op < PATCH_OPS; op++) {
				for (int i = 0; i < PATCH_OP_PARAMS; i++) {
					int param = PARAM_OP_BASE + op * PATCH_OP_PARAMS + OP_CELLS[i].field;
					add_cell(g, PAGE_X + op * 118, OP_CELLS[i].y, OP_W, CELL_H, PATCH_CHANNEL_ID(channel, param), OP_CELLS[i].label, CELL_ROW);
				}
			}
		}
	}
	for (int i = 0; i < (int)(sizeof(GLOBAL_CELLS) / sizeof(*GLOBAL_CELLS)); i++) {
		add_cell(g, 8 + i * 118, GLOBALS_Y + 6, 114, 32, (uint32_t)GLOBAL_CELLS[i].global, GLOBAL_CELLS[i].label, CELL_COLUMN);
	}
}

static void draw_cell(mdmi_gui *g, const cell *c, const patch_values *values)
{
	const bool held = g->dragging && g->drag_id == c->id;
	const int value = value_of(g, values, c->id);
	char buffer[64];
	if (c->kind == CELL_ALGORITHM) {
		const bool on = value == c->algorithm;
		fill(g, c->x, c->y, c->w, c->h, on ? C_FM : C_CELL);
		snprintf(buffer, sizeof(buffer), "%d", c->algorithm);
		text(g, c->x + 7, c->y + 2, buffer, on ? C_TEXT : C_LABEL, 2);
		return;
	}
	patch_format(c->id, value, buffer, sizeof(buffer));
	const bool fm = c->id >= PATCH_NUM_GLOBALS && channel_is_fm(*g->source.channel);
	uint32_t background = held ? C_LINE : c->kind == CELL_COLUMN ? C_CELL : fm ? C_FM_DARK : C_PSG_DARK;
	fill(g, c->x, c->y, c->w, c->h, background);
	const uint32_t label = held ? C_TEXT : C_LABEL;
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
	}
	if (held) {
		frame(g, c->x, c->y, c->w, c->h, C_TEXT);
	}
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

static void draw_page(mdmi_gui *g, const patch_values *values)
{
	const int channel = *g->source.channel;
	char buffer[64];
	fill(g, RAIL_W + 1, HEADER_H, GUI_WIDTH - RAIL_W - 1, GLOBALS_Y - HEADER_H, C_BG);
	snprintf(buffer, sizeof(buffer), "%d", channel + 1);
	text(g, PAGE_X, 30, buffer, C_TEXT, 3);
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
		text(g, PAGE_X, 62, channel == PATCH_NUM_CHANNELS - 1 ? "SN76489 NOISE" : "SN76489 SQUARE", C_HINT, 1);
		text(g, PAGE_X, 74, "ENVELOPE EDITOR: LATER STAGE", C_HINT, 1);
		return;
	}
	const int algorithm = value_of(g, values, PATCH_CHANNEL_ID(channel, PARAM_ALGORITHM));
	draw_diagram(g, algorithm, value_of(g, values, PATCH_CHANNEL_ID(channel, PARAM_FEEDBACK)));
	for (int op = 0; op < PATCH_OPS; op++) {
		const int x = PAGE_X + op * 118;
		const bool carrier = CARRIERS[algorithm] >> op & 1;
		snprintf(buffer, sizeof(buffer), "OP%d", op + 1);
		if (carrier) {
			fill(g, x, OP_HEADER_Y, OP_W, CELL_H, C_FM);
			text(g, x + 4, OP_HEADER_Y + 2, buffer, C_TEXT, 2);
			text_right(g, x + 108, OP_HEADER_Y + 6, "CARRIER", C_TEXT, 1, 60);
		} else {
			frame(g, x, OP_HEADER_Y, OP_W, CELL_H, C_FM_LIGHT);
			text(g, x + 4, OP_HEADER_Y + 2, buffer, C_FM_LIGHT, 2);
			text_right(g, x + 108, OP_HEADER_Y + 6, "MOD", C_FM_LIGHT, 1, 60);
		}
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

static void draw_status_line(mdmi_gui *g)
{
	fill(g, 0, FOOTER_Y, GUI_WIDTH, GUI_HEIGHT - FOOTER_Y, C_BG);
	mdmi_engine_stats stats = mdmi_engine_get_stats(g->source.engine);
	if (++g->ticks >= TIMER_PER_SECOND) {
		g->ticks = 0;
		g->underruns_per_second = stats.underruns - g->underruns_second_ago;
		g->underruns_second_ago = stats.underruns;
	}
	char buffer[160];
	snprintf(buffer, sizeof(buffer), "UNDERRUNS %u +%u/S  MIDI LOST %u  BLOCK %u @ %u  RUN %.1f WAIT %.1f MS  RING %u SHORT %u BURST %u",
		stats.underruns, g->underruns_per_second, stats.dropped_midi, stats.last_block, stats.sample_rate, stats.worst_run_ms, stats.worst_wait_ms,
		stats.fill_target, stats.worst_short, stats.longest_burst);
	text(g, 8, FOOTER_Y + 6, buffer, stats.underruns || stats.dropped_midi ? C_TARGET : C_HINT, 1);
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
	draw_page(g, values);
	for (int channel = 0; channel < PATCH_NUM_CHANNELS; channel++) {
		draw_row(g, values, channel);
	}
	draw_lends(g);
	fill(g, 0, GLOBALS_Y, GUI_WIDTH, FOOTER_Y - GLOBALS_Y, C_BG);
	fill(g, 0, GLOBALS_Y, GUI_WIDTH, 1, C_LINE);
	for (int i = 0; i < g->cell_count; i++) {
		draw_cell(g, &g->cells[i], values);
	}
	draw_status_line(g);
	// Hover last, so nothing covers it.
	int over = g->dragging ? -1 : hovered(g);
	if (over >= 0) {
		const cell *c = &g->cells[over];
		frame(g, c->x, c->y, c->w, c->h, C_TEXT);
	}
	int row = g->dragging ? -1 : rail_channel_at(g->mouse_x, g->mouse_y);
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
	char buffer[64];
	if (stats.failed) {
		int end = text(g, STATUS_X, 7, "ENGINE FAILED", C_ERROR, 2);
		if (stats.boot_error) {
			snprintf(buffer, sizeof(buffer), "%s %u", stats.boot_stage ? stats.boot_stage : "", stats.boot_error);
		} else {
			snprintf(buffer, sizeof(buffer), "RELOAD DEVICE");
		}
		text(g, end + 6, 11, buffer, C_ERROR, 1);
	} else if (!stats.active) {
		text(g, STATUS_X, 7, "STOPPED", C_HINT, 2);
	} else if (!stats.booted) {
		text(g, STATUS_X, 7, "LOADING", C_TARGET, 2);
	} else {
		text(g, STATUS_X, 7, "RUNNING", C_PLAYING, 2);
	}
	const int width = (int)strlen(g->source.version) * 6;
	text(g, GUI_WIDTH - width - 8, 11, g->source.version, C_HINT, 1);
}

static void render(mdmi_gui *g)
{
	fill(g, 0, 0, GUI_WIDTH, HEADER_H, C_BG);
	fill(g, 0, HEADER_H - 1, GUI_WIDTH, 1, C_LINE);
	text(g, 8, 7, "MDMI", C_TEXT, 2);
	const bool rom = *g->source.page == PAGE_ROM;
	button(g, BUTTON_EDITOR_X, "EDITOR", !rom);
	button(g, BUTTON_ROM_X, "ROM", rom);
	draw_status(g);
	if (rom) {
		g->cell_count = 0;
		draw_rom(g);
	} else {
		draw_editor(g);
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

static void edit(mdmi_gui *g, int kind, uint32_t id, int value)
{
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

static void on_down(mdmi_gui *g, int x, int y)
{
	if (y >= BUTTON_Y && y < BUTTON_Y + BUTTON_H) {
		if (x >= BUTTON_EDITOR_X && x < BUTTON_EDITOR_X + BUTTON_W) {
			*g->source.page = PAGE_EDITOR;
		} else if (x >= BUTTON_ROM_X && x < BUTTON_ROM_X + BUTTON_W) {
			*g->source.page = PAGE_ROM;
		}
		refresh(g);
		return;
	}
	if (*g->source.page != PAGE_EDITOR) {
		return;
	}
	int channel = rail_channel_at(x, y);
	if (channel >= 0) {
		*g->source.channel = channel;
		refresh(g);
		return;
	}
	g->mouse_x = x;
	g->mouse_y = y;
	int index = hovered(g);
	if (index < 0) {
		return;
	}
	const cell *c = &g->cells[index];
	if (c->kind == CELL_ALGORITHM) {
		edit_once(g, c->id, c->algorithm);
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
	int index = *g->source.page == PAGE_EDITOR ? hovered(g) : -1;
	if (index < 0 || g->cells[index].kind == CELL_ALGORITHM) {
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
		: patch_preset_value(channel, param, patch_get(g->source.patch, PATCH_CHANNEL_ID(channel, PARAM_PRESET)));
	edit_once(g, id, value);
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
		refresh(g);
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
