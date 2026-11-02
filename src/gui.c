#define COBJMACROS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
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
#define C_TARGET 0xEEEE22
#define C_ERROR 0xEE2222

#define HEADER_H 32
#define FOOTER_Y 672
#define STATUS_Y 640
#define GROUP_H 24
#define ROW_H 56
#define ROM_SCALE 3
#define BUTTON_Y 4
#define BUTTON_W 92
#define BUTTON_H 24
#define BUTTON_EDITOR_X 80
#define BUTTON_ROM_X (BUTTON_EDITOR_X + BUTTON_W + 4)
#define VELOCITY_STEPS 15
#define E3_CELL_X 800
#define E3_CELL_W 152

enum { PAGE_EDITOR, PAGE_ROM };

typedef struct {
	IDropTarget iface;
	struct mdmi_gui *gui;
} drop_target;

struct mdmi_gui {
	gui_source source;
	HWND window;
	HWND parent;
	int scale;
	uint32_t *pixels;
	bool dirty;

	drop_target drop;
	bool drop_registered;
	bool ole_ready;
	bool dragging;
	bool drag_has_file;
	int drag_row;
	char drag_name[64];
	char message[128];
	bool message_is_warning;

#ifdef MDMI_E3
	int drag_cell;
	int drag_start_y;
	int drag_start_value;
#endif
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

static void frame_rect(mdmi_gui *g, int x, int y, int w, int h, int thickness, uint32_t color)
{
	fill(g, x, y, w, thickness, color);
	fill(g, x, y + h - thickness, w, thickness, color);
	fill(g, x, y, thickness, h, color);
	fill(g, x + w - thickness, y, thickness, h, color);
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
	text(g, x + (BUTTON_W - width) / 2, BUTTON_Y + 4, label, on ? C_BG : C_LABEL, 2);
}

static bool channel_is_fm(int channel)
{
	return channel < PATCH_NUM_FM_CHANNELS;
}

// The row a window position falls on, or -1.
static int row_at(int y)
{
	int top = HEADER_H + GROUP_H;
	if (y >= top && y < top + PATCH_NUM_FM_CHANNELS * ROW_H) {
		return (y - top) / ROW_H;
	}
	top += PATCH_NUM_FM_CHANNELS * ROW_H + GROUP_H;
	if (y >= top && y < top + (PATCH_NUM_CHANNELS - PATCH_NUM_FM_CHANNELS) * ROW_H) {
		return PATCH_NUM_FM_CHANNELS + (y - top) / ROW_H;
	}
	return -1;
}

static int row_y(int channel)
{
	if (channel_is_fm(channel)) {
		return HEADER_H + GROUP_H + channel * ROW_H;
	}
	return HEADER_H + 2 * GROUP_H + channel * ROW_H;
}

static bool drag_fits_channel(const mdmi_gui *g, int channel)
{
	const char *dot = strrchr(g->drag_name, '.');
	if (!g->drag_has_file || !dot) {
		return false;
	}
	if (!_stricmp(dot, ".tfi") || !_stricmp(dot, ".vgi") || !_stricmp(dot, ".tyi")) {
		return channel_is_fm(channel);
	}
	if (!_stricmp(dot, ".eef")) {
		return !channel_is_fm(channel);
	}
	return false;
}

static void note_name(int key, char *out, size_t size)
{
	static const char *const names[12] = {"C-", "C#", "D-", "D#", "E-", "F-", "F#", "G-", "G#", "A-", "A#", "B-"};
	snprintf(out, size, "%s%d", names[key % 12], key / 12 - 1);
}

static void draw_row(mdmi_gui *g, int channel)
{
	const mdmi_patch *patch = g->source.patch;
	const int y = row_y(channel);
	const bool dac = channel == PATCH_NUM_FM_CHANNELS - 1 && patch_get(patch, PARAM_DAC);
	const bool voice = channel > 0 && channel_is_fm(channel) && patch_get(patch, PARAM_POLYPHONY);
	const uint32_t kind = dac ? C_DAC : channel_is_fm(channel) ? C_FM : C_PSG;

	fill(g, 0, y, GUI_WIDTH, ROW_H - 1, C_BG);
	fill(g, 0, y + ROW_H - 1, GUI_WIDTH, 1, C_CELL);
	fill(g, 0, y, 8, ROW_H - 1, kind);

	char buffer[64];
	snprintf(buffer, sizeof(buffer), "%d", channel + 1);
	text(g, 20, y + 16, buffer, C_TEXT, 3);

	const char *tag = dac ? "DAC" : channel == PATCH_NUM_CHANNELS - 1 ? "NOISE"
		: !channel_is_fm(channel) ? "PSG"
		: channel == 2 && patch_get(patch, PARAM_CH3_MODE) ? "FM SP" : "FM";
	text(g, 76, y + 20, tag, dac ? C_DAC : channel_is_fm(channel) ? C_FM_LIGHT : C_PSG, 2);

	if (voice) {
		text(g, 156, y + 20, "voice of CH 1", C_HINT, 2);
	} else if (dac) {
		text(g, 156, y + 20, "Samples", C_LABEL, 2);
	} else {
		patch_format_preset(channel, patch_get_channel(patch, channel, PARAM_PRESET), buffer, sizeof(buffer));
		buffer[28] = 0;
		text(g, 156, y + 20, buffer, C_LABEL, 2);
	}

	if (!voice) {
		uint32_t note = __atomic_load_n(&g->source.notes[channel], __ATOMIC_ACQUIRE);
		if (GUI_NOTE_SEEN(note)) {
			const bool held = GUI_NOTE_HELD(note) > 0;
			note_name(GUI_NOTE_KEY(note), buffer, sizeof(buffer));
			text(g, 516, y + 16, buffer, held ? C_PLAYING : C_HINT, 3);
			int lit = held ? (GUI_NOTE_VELOCITY(note) * VELOCITY_STEPS + 126) / 127 : 0;
			for (int step = 0; step < VELOCITY_STEPS; step++) {
				fill(g, 600 + step * 8, y + 20, 6, 16, step < lit ? C_PLAYING : C_CELL);
			}
			if (GUI_NOTE_HELD(note) > 1) {
				snprintf(buffer, sizeof(buffer), "x%d", (int)GUI_NOTE_HELD(note));
				text(g, 732, y + 20, buffer, C_PLAYING, 2);
			}
		}
	}

#ifdef MDMI_E3
	const gui_e3 *e3 = &g->source.e3;
	int cell = channel == 3 ? 0 : channel == 6 ? 1 : -1;
	if (cell >= 0) {
		fill(g, E3_CELL_X, y + 8, E3_CELL_W, ROW_H - 17, g->drag_cell == cell ? C_FM_DARK : C_CELL);
		text(g, E3_CELL_X + 4, y + 11, e3->names[cell], C_LABEL, 1);
		snprintf(buffer, sizeof(buffer), "%d", e3->value(e3->context, cell));
		text(g, E3_CELL_X + 4, y + 24, buffer, C_TEXT, 2);
	}
#endif

	if (g->dragging && drag_fits_channel(g, channel)) {
		if (g->drag_row == channel) {
			frame_rect(g, 0, y, GUI_WIDTH, ROW_H - 1, 4, C_TARGET);
		} else {
			frame_rect(g, 0, y, GUI_WIDTH, ROW_H - 1, 2, C_TARGET);
		}
	}
}

static void draw_editor(mdmi_gui *g)
{
	fill(g, 0, HEADER_H, GUI_WIDTH, GROUP_H, C_CELL);
	text(g, 8, HEADER_H + 8, "YM2612", C_FM_LIGHT, 1);
	int psg_y = row_y(PATCH_NUM_FM_CHANNELS) - GROUP_H;
	fill(g, 0, psg_y, GUI_WIDTH, GROUP_H, C_CELL);
	text(g, 8, psg_y + 8, "SN76489", C_PSG, 1);
	for (int channel = 0; channel < PATCH_NUM_CHANNELS; channel++) {
		draw_row(g, channel);
	}

	mdmi_engine_stats stats = mdmi_engine_get_stats(g->source.engine);
	char buffer[128];
	snprintf(buffer, sizeof(buffer), "UNDERRUNS %u   DROPPED MIDI %u", stats.underruns, stats.dropped_midi);
	text(g, 8, STATUS_Y + 8, buffer, stats.underruns || stats.dropped_midi ? C_TARGET : C_HINT, 2);
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
	for (int y = 0; y < height; y++) {
		const uint32_t *source = frame->pixels + (y / ROM_SCALE) * MDMI_FRAME_MAX_WIDTH;
		uint32_t *line = g->pixels + (HEADER_H + y) * GUI_WIDTH + left;
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
		if (stats.boot_error) {
			snprintf(buffer, sizeof(buffer), "ENGINE FAILED: %s %u", stats.boot_stage ? stats.boot_stage : "", stats.boot_error);
		} else {
			snprintf(buffer, sizeof(buffer), "ENGINE FAILED: RELOAD DEVICE");
		}
		text(g, 280, 8, buffer, C_ERROR, 2);
	} else if (!stats.active) {
		text(g, 280, 8, "STOPPED", C_HINT, 2);
	} else if (!stats.booted) {
		text(g, 280, 8, "LOADING ENGINE", C_TARGET, 2);
	} else {
		text(g, 280, 8, "RUNNING", C_PLAYING, 2);
	}
	const int width = (int)strlen(g->source.version) * 6;
	text(g, GUI_WIDTH - width - 8, 12, g->source.version, C_HINT, 1);
}

static void draw_footer(mdmi_gui *g)
{
	fill(g, 0, FOOTER_Y, GUI_WIDTH, GUI_HEIGHT - FOOTER_Y, C_BG);
	fill(g, 0, FOOTER_Y, GUI_WIDTH, 1, C_LINE);
	char buffer[128];
	if (g->dragging) {
		if (!g->drag_has_file) {
			text(g, 8, FOOTER_Y + 10, g->message, C_TARGET, 1);
		} else if (g->drag_row >= 0 && drag_fits_channel(g, g->drag_row)) {
			snprintf(buffer, sizeof(buffer), "%s: drop on CH %d", g->drag_name, g->drag_row + 1);
			text(g, 8, FOOTER_Y + 8, buffer, C_TARGET, 2);
		} else {
			text(g, 8, FOOTER_Y + 8, g->drag_name, C_LABEL, 2);
		}
		return;
	}
	if (g->message[0]) {
		text(g, 8, FOOTER_Y + 10, g->message, g->message_is_warning ? C_TARGET : C_LABEL, 1);
	}
#ifdef MDMI_E3
	uint32_t seen = __atomic_load_n(g->source.e3.programs_seen, __ATOMIC_ACQUIRE);
	uint32_t last = __atomic_load_n(g->source.e3.last_program, __ATOMIC_ACQUIRE);
	snprintf(buffer, sizeof(buffer), "PGM SEEN %u  LAST CH %u = %u", seen, (last >> 8) + 1, last & 0xFF);
	text(g, 600, FOOTER_Y + 10, buffer, seen ? C_PLAYING : C_HINT, 1);
#endif
}

static void render(mdmi_gui *g)
{
	fill(g, 0, 0, GUI_WIDTH, HEADER_H, C_BG);
	fill(g, 0, HEADER_H - 1, GUI_WIDTH, 1, C_LINE);
	text(g, 8, 8, "MDMI", C_TEXT, 2);
	// While a file is dragged the table stays in view, because it holds the drop targets.
	const bool rom = *g->source.page == PAGE_ROM && !g->dragging;
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

// Drag and drop. Explorer offers CF_HDROP. Whatever else a host offers is listed in the footer,
// so that a browser that does not offer files can be told apart from one that offers them.
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

static bool read_file_name(IDataObject *data, char *out, size_t size)
{
	FORMATETC format = {CF_HDROP, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
	STGMEDIUM medium;
	if (FAILED(IDataObject_GetData(data, &format, &medium))) {
		return false;
	}
	bool ok = false;
	HDROP drop = GlobalLock(medium.hGlobal);
	if (drop) {
		wchar_t path[MAX_PATH];
		if (DragQueryFileW(drop, 0, path, MAX_PATH)) {
			const wchar_t *name = wcsrchr(path, L'\\');
			name = name ? name + 1 : path;
			size_t i = 0;
			for (; name[i] && i + 1 < size; i++) {
				out[i] = name[i] < 127 && name[i] >= 32 ? (char)name[i] : '?';
			}
			out[i] = 0;
			ok = true;
		}
		GlobalUnlock(medium.hGlobal);
	}
	ReleaseStgMedium(&medium);
	return ok;
}

static void describe_offered_formats(mdmi_gui *g, IDataObject *data)
{
	int used = snprintf(g->message, sizeof(g->message), "No file offered. Formats:");
	IEnumFORMATETC *formats = NULL;
	if (FAILED(IDataObject_EnumFormatEtc(data, DATADIR_GET, &formats)) || !formats) {
		return;
	}
	FORMATETC format;
	while (IEnumFORMATETC_Next(formats, 1, &format, NULL) == S_OK && used < (int)sizeof(g->message) - 1) {
		char name[48] = "";
		if (!GetClipboardFormatNameA(format.cfFormat, name, sizeof(name))) {
			snprintf(name, sizeof(name), "%u", format.cfFormat);
		}
		used += snprintf(g->message + used, sizeof(g->message) - (size_t)used, " %s", name);
		if (format.ptd) {
			CoTaskMemFree(format.ptd);
		}
	}
	IEnumFORMATETC_Release(formats);
}

static void update_drag(mdmi_gui *g, POINTL point, DWORD *effect)
{
	POINT client = {point.x, point.y};
	ScreenToClient(g->window, &client);
	g->drag_row = row_at(client.y / g->scale);
	*effect = g->drag_has_file && g->drag_row >= 0 && drag_fits_channel(g, g->drag_row) ? DROPEFFECT_COPY : DROPEFFECT_NONE;
	refresh(g);
}

static HRESULT STDMETHODCALLTYPE drop_enter(IDropTarget *self, IDataObject *data, DWORD keys, POINTL point, DWORD *effect)
{
	(void)keys;
	mdmi_gui *g = gui_of(self);
	g->dragging = true;
	g->message[0] = 0;
	g->drag_has_file = read_file_name(data, g->drag_name, sizeof(g->drag_name));
	if (!g->drag_has_file) {
		describe_offered_formats(g, data);
	}
	update_drag(g, point, effect);
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE drop_over(IDropTarget *self, DWORD keys, POINTL point, DWORD *effect)
{
	(void)keys;
	update_drag(gui_of(self), point, effect);
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE drop_leave(IDropTarget *self)
{
	mdmi_gui *g = gui_of(self);
	g->dragging = false;
	if (!g->drag_has_file) {
		// Keep the format list on screen after the drag: it is the point of the message.
		g->message_is_warning = true;
	}
	refresh(g);
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE drop_drop(IDropTarget *self, IDataObject *data, DWORD keys, POINTL point, DWORD *effect)
{
	(void)keys;
	(void)data;
	mdmi_gui *g = gui_of(self);
	update_drag(g, point, effect);
	g->dragging = false;
	if (*effect != DROPEFFECT_NONE) {
		snprintf(g->message, sizeof(g->message), "%s reached CH %d. Loading files arrives in stage 4.", g->drag_name, g->drag_row + 1);
		g->message_is_warning = false;
	}
	refresh(g);
	return S_OK;
}

static IDropTargetVtbl drop_vtable = {
	drop_query_interface, drop_add_ref, drop_release, drop_enter, drop_over, drop_leave, drop_drop,
};

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
#ifdef MDMI_E3
	int row = row_at(y);
	int cell = row == 3 ? 0 : row == 6 ? 1 : -1;
	if (*g->source.page == PAGE_EDITOR && cell >= 0 && x >= E3_CELL_X && x < E3_CELL_X + E3_CELL_W) {
		g->drag_cell = cell;
		g->drag_start_y = y;
		g->drag_start_value = g->source.e3.value(g->source.e3.context, cell);
		SetCapture(g->window);
		g->source.e3.begin(g->source.e3.context, cell);
	}
#endif
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
	case WM_LBUTTONDOWN:
		on_click(g, (short)LOWORD(lparam) / g->scale, (short)HIWORD(lparam) / g->scale);
		return 0;
#ifdef MDMI_E3
	case WM_MOUSEMOVE:
		if (g->drag_cell >= 0) {
			int value = g->drag_start_value + g->drag_start_y - (short)HIWORD(lparam) / g->scale;
			value = value < 0 ? 0 : value > 127 ? 127 : value;
			g->source.e3.change(g->source.e3.context, g->drag_cell, value);
		}
		return 0;
	case WM_LBUTTONUP:
		if (g->drag_cell >= 0) {
			g->source.e3.end(g->source.e3.context, g->drag_cell);
			g->drag_cell = -1;
			ReleaseCapture();
		}
		return 0;
#endif
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
	g->drag_row = -1;
#ifdef MDMI_E3
	g->drag_cell = -1;
#endif
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
		KillTimer(g->window, TIMER_ID);
		if (g->drop_registered) {
			RevokeDragDrop(g->window);
		}
		SetWindowLongPtrA(g->window, GWLP_USERDATA, 0);
		DestroyWindow(g->window);
		if (InterlockedDecrement(&window_count) == 0) {
			UnregisterClassA(WINDOW_CLASS, this_module());
		}
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
	g->ole_ready = SUCCEEDED(OleInitialize(NULL));
	HRESULT registered = RegisterDragDrop(g->window, &g->drop.iface);
	g->drop_registered = SUCCEEDED(registered);
	if (!g->drop_registered) {
		snprintf(g->message, sizeof(g->message), "Files can't be dropped here: error %08lX", (unsigned long)registered);
		g->message_is_warning = true;
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
