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

#define HEADER_H 28
#define FOOTER_Y 504
#define STATUS_Y 480
#define GROUP_H 16
#define ROW_H 42
#define ROM_SCALE 2
#define BUTTON_Y 2
#define BUTTON_W 80
#define BUTTON_H 24
#define BUTTON_EDITOR_X 60
#define BUTTON_ROM_X (BUTTON_EDITOR_X + BUTTON_W + 4)
#define STATUS_X 236
#define VELOCITY_STEPS 15
#define TIMER_PER_SECOND (1000 / TIMER_MS)

enum { PAGE_EDITOR, PAGE_ROM };

struct mdmi_gui {
	gui_source source;
	HWND window;
	HWND parent;
	int scale;
	uint32_t *pixels;
	bool dirty;

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
	if (channel_is_fm(channel)) {
		return HEADER_H + GROUP_H + channel * ROW_H;
	}
	return HEADER_H + 2 * GROUP_H + channel * ROW_H;
}

static void note_name(int key, char *out, size_t size)
{
	static const char *const names[12] = {"C-", "C#", "D-", "D#", "E-", "F-", "F#", "G-", "G#", "A-", "A#", "B-"};
	snprintf(out, size, "%s%d", names[key % 12], key / 12 - 1);
}

static void draw_row(mdmi_gui *g, int channel)
{
	mdmi_patch *patch = g->source.patch;
	const int y = row_y(channel);
	const bool dac = channel == PATCH_NUM_FM_CHANNELS - 1 && patch_get(patch, PARAM_DAC);
	const bool voice = channel > 0 && channel_is_fm(channel) && patch_get(patch, PARAM_POLYPHONY);
	const uint32_t kind = dac ? C_DAC : channel_is_fm(channel) ? C_FM : C_PSG;

	fill(g, 0, y, GUI_WIDTH, ROW_H - 1, C_BG);
	fill(g, 0, y + ROW_H - 1, GUI_WIDTH, 1, C_CELL);
	fill(g, 0, y, 8, ROW_H - 1, kind);

	char buffer[64];
	snprintf(buffer, sizeof(buffer), "%d", channel + 1);
	text(g, 14, y + 9, buffer, C_TEXT, 3);

	const char *tag = dac ? "DAC" : channel == PATCH_NUM_CHANNELS - 1 ? "NOISE"
		: !channel_is_fm(channel) ? "PSG"
		: channel == 2 && patch_get(patch, PARAM_CH3_MODE) ? "FM SP" : "FM";
	text(g, 54, y + 13, tag, dac ? C_DAC : channel_is_fm(channel) ? C_FM_LIGHT : C_PSG, 2);

	if (voice) {
		text(g, 122, y + 13, "voice of CH 1", C_HINT, 2);
	} else if (dac) {
		text(g, 122, y + 13, "Samples", C_LABEL, 2);
	} else {
		patch_format_preset(channel, patch_get_channel(patch, channel, PARAM_PRESET), buffer, sizeof(buffer));
		buffer[21] = 0;
		text(g, 122, y + 13, buffer, C_LABEL, 2);
	}

	if (!voice) {
		uint32_t note = __atomic_load_n(&g->source.notes[channel], __ATOMIC_ACQUIRE);
		if (GUI_NOTE_SEEN(note)) {
			const bool held = GUI_NOTE_HELD(note) > 0;
			note_name(GUI_NOTE_KEY(note), buffer, sizeof(buffer));
			text(g, 382, y + 9, buffer, held ? C_PLAYING : C_HINT, 3);
			int lit = held ? (GUI_NOTE_VELOCITY(note) * VELOCITY_STEPS + 126) / 127 : 0;
			for (int step = 0; step < VELOCITY_STEPS; step++) {
				fill(g, 442 + step * 6, y + 13, 4, 16, step < lit ? C_PLAYING : C_CELL);
			}
			if (GUI_NOTE_HELD(note) > 1) {
				snprintf(buffer, sizeof(buffer), "x%d", (int)GUI_NOTE_HELD(note));
				text(g, 536, y + 13, buffer, C_PLAYING, 2);
			}
		}
	}
}

static void draw_editor(mdmi_gui *g)
{
	fill(g, 0, HEADER_H, GUI_WIDTH, FOOTER_Y - HEADER_H, C_BG);
	fill(g, 0, HEADER_H, GUI_WIDTH, GROUP_H, C_CELL);
	text(g, 8, HEADER_H + 4, "YM2612", C_FM_LIGHT, 1);
	int psg_y = row_y(PATCH_NUM_FM_CHANNELS) - GROUP_H;
	fill(g, 0, psg_y, GUI_WIDTH, GROUP_H, C_CELL);
	text(g, 8, psg_y + 4, "SN76489", C_PSG, 1);
	for (int channel = 0; channel < PATCH_NUM_CHANNELS; channel++) {
		draw_row(g, channel);
	}

	mdmi_engine_stats stats = mdmi_engine_get_stats(g->source.engine);
	char buffer[128];
	if (++g->ticks >= TIMER_PER_SECOND) {
		g->ticks = 0;
		g->underruns_per_second = stats.underruns - g->underruns_second_ago;
		g->underruns_second_ago = stats.underruns;
	}
	snprintf(buffer, sizeof(buffer), "UNDERRUNS %u +%u/S  MIDI LOST %u", stats.underruns, g->underruns_per_second, stats.dropped_midi);
	text(g, 8, STATUS_Y + 4, buffer, stats.underruns || stats.dropped_midi ? C_TARGET : C_HINT, 2);
	snprintf(buffer, sizeof(buffer), "BLOCK %u @ %u  RUN %.1f WAIT %.1f MS", stats.last_block, stats.sample_rate, stats.worst_run_ms, stats.worst_wait_ms);
	text(g, 450, STATUS_Y + 8, buffer, C_HINT, 1);
}

static void draw_rom(mdmi_gui *g)
{
	const mdmi_frame *frame = mdmi_engine_latest_frame(g->source.engine);
	fill(g, 0, HEADER_H, GUI_WIDTH, GUI_HEIGHT - HEADER_H, C_BG);
	int width = (int)frame->width * ROM_SCALE, height = (int)frame->height * ROM_SCALE;
	if (width > GUI_WIDTH) {
		width = GUI_WIDTH;
	}
	if (height > GUI_HEIGHT - HEADER_H) {
		height = GUI_HEIGHT - HEADER_H;
	}
	const int left = (GUI_WIDTH - width) / 2;
	const int top = HEADER_H + (GUI_HEIGHT - HEADER_H - height) / 2;
	for (int y = 0; y < height; y++) {
		const uint32_t *source = frame->pixels + (y / ROM_SCALE) * MDMI_FRAME_MAX_WIDTH;
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

static void draw_footer(mdmi_gui *g)
{
	fill(g, 0, FOOTER_Y, GUI_WIDTH, GUI_HEIGHT - FOOTER_Y, C_BG);
	fill(g, 0, FOOTER_Y, GUI_WIDTH, 1, C_LINE);
	const mdmi_engine_stats stats = mdmi_engine_get_stats(g->source.engine);
	char buffer[96];
	snprintf(buffer, sizeof(buffer), "RING %u  WORST SHORT %u  LONGEST BURST %u BLOCKS", stats.fill_target, stats.worst_short, stats.longest_burst);
	text(g, 8, FOOTER_Y + 8, buffer, C_HINT, 1);
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
		draw_rom(g);
	} else {
		draw_editor(g);
		draw_footer(g);
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

static void on_click(mdmi_gui *g, int x, int y)
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
}

static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
	mdmi_gui *g = (mdmi_gui *)GetWindowLongPtrA(window, GWLP_USERDATA);
	if (!g) {
		return DefWindowProcA(window, message, wparam, lparam);
	}
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
		KillTimer(window, TIMER_ID);
		SetWindowLongPtrA(window, GWLP_USERDATA, 0);
		g->window = NULL;
		InterlockedDecrement(&window_count);
		return 0;
	case WM_LBUTTONDOWN:
		on_click(g, (short)LOWORD(lparam) / g->scale, (short)HIWORD(lparam) / g->scale);
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
