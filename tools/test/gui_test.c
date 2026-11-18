// Opens the plugin's window with no host around it, plays notes, and checks the pixels it draws.
//
//   gui_test <plugin.clap> <picture_prefix>
//
// Writes <prefix>-editor.bmp and <prefix>-rom.bmp. With MDMI_TEST_CRASH=segv it instead expects the
// window to report a dead engine.
#include <stdbool.h>
#include <stdint.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <ole2.h>
#include <shellapi.h>
#include <shlobj.h>
#include <clap/clap.h>

#define RATE 48000
#define BLOCK 128
#define WIDTH 720
#define HEIGHT 528
#define COLOR_PLAYING 0xAAEE22
#define COLOR_ERROR 0xEE2222
#define HEADER_H 28
#define TABLE_BOTTOM 480
#define BOOT_TIMEOUT_MS 30000
// Header buttons since stage 10: ALL 60-116, EDITOR 120-200, BANK 204-284, ROM 288-344.
#define BUTTON_ALL_X 88
#define BUTTON_EDITOR_X 160
#define BUTTON_ROM_X 316
#define BUTTON_Y 12
// Channel 4 in the rail, Op1 TL on the FM page, and the ids of Op1 TL on channels 3 and 4.
#define RAIL_X 100
#define RAIL_CH2_Y 104
#define RAIL_CH4_Y 184
#define OP1_TL_X 300
#define OP1_TL_Y 230
#define CH3_OP1_TL 1311
#define CH4_OP1_TL 1411
// The LFO Rate cell among the global knobs, and its id.
#define LFO_RATE_X 100
#define LFO_RATE_Y 484
#define LFO_RATE 3
// Master Volume at the strip's right end (stage 18a), and its id.
#define MASTER_X 660
#define MASTER 9
// The bank page's button, its third place in list A (stage 22c: rows from y 92), and channel 4's Preset.
#define BUTTON_BANK_X 244
#define BANK_ROW_X 400
#define BANK_ROW3_Y 142
#define CH4_PRESET 1400
#define COLOR_FM_DARK 0x222266
// The envelope editor of a PSG channel: channel 8 in the rail, the level lane, a row near its top
// (level 0), and UNDO.
#define RAIL_CH8_Y 350
#define ENV_X 296
#define ENV_RIGHT 702
#define ENV_LEVEL_Y 122
#define ENV_LEVEL_BOTTOM 234
// In the bright top of a bar at level 0: below it the body is dark (4A).
#define ENV_LOUD_Y 123
#define ACTION_UNDO_X 684
#define ENV_LENGTH_X 670
#define ENV_LENGTH_Y 317
#define ENV_LOOP_STRIP_Y 112
#define COLOR_PSG 0x22CCEE
#define COLOR_CELL 0x222222
// Stage 9: the rail's bars and the bank's scroll track.
#define RAIL_CH1_TOP 44
#define RAIL_CH3_TOP 124
#define RAIL_CH4_TOP 164
#define ROW_H 40
#define BAR_LEFT 54
#define BAR_RIGHT 226
#define CH3_SPECIAL_X 330
#define CH3_SPECIAL_Y 480
#define COLOR_LABEL 0xAAAAAA
#define COLOR_FM_LIGHT 0x8888EE
#define COLOR_TEXT 0xEEEEEE
#define SCROLL_X 506
#define SCROLL_TOP 92
#define SCROLL_BOTTOM 452
// Stage 10: the ALL page. Channel 2's row, its VOL and PAN R, its id, and the sound column.
#define ALL_CH2_Y 96
#define ALL_VOL_X 400
#define ALL_PAN_R_X 518
#define ALL_SOUND_X 290
#define ALL_SOUND_RIGHT 346
#define CH2_VOLUME 1201
// The DAC page (stage 12).
#define RAIL_CH6_Y 264
#define COLOR_AMBER 0xEECC66
#define COLOR_DAC 0xEE8822
#define COLOR_DAC_DARK 0x442200
#define DAC_SCROLL_X 696
#define DAC_LIST_Y 96
#define DAC_LIST_BOTTOM 396
#define DAC_FILE_X 580
#define DAC_RESET_X 650
// Note 40 is the 15th row when the list opens at note 26.
#define DAC_NOTE40_Y 376
// On EDITOR's grid note N is at 242 + N % 12 * 38, 96 + N / 12 * 18, 36x16.
#define GRID_NOTE40_X 394
#define COLOR_SHARP_ROM 0x2E1700
#define GRID_NOTE40_Y 150
// The panel's cells under the grid (D38).
#define DAC_ROW_A_Y 398
#define DAC_ROW_B_Y 422
#define DAC_TRIM_Y 295
#define DAC_LOOP_Y 310
#define DAC_GAIN_X 396
#define CH2_PAN 1202
#define COLOR_FM 0x4422EE
// Stage 24d (D62): channel 3 in Ch3 Special.
#define COLOR_SP 0xEE44AA
#define COLOR_SP_DARK 0x440022
#define COLOR_MARK 0xEECC66
#define FOOTER_TOP 504
// Stage 11: the FM page of v8. The operators' columns, the graph, the SSG button and its choice,
// a row's y in each column, and the ids of channel 4's operator 3 AR and RS, operator 1 DT, RS, AM and SSG.
// Stage 20, the arp row: its lane from x 264, 200 px wide, +24 at its top, 2 px a semitone; the
// loop's strip 13 px above the lane, the length cell at the row's right. On PSG under the envelope,
// on FM in the graph's place with the ARP switch right of FMS.
#define ARP_LX 264
#define ARP_PSG_LY 347
#define ARP_FM_LY 97
#define ARP_Y(ly, shift) ((ly) + 2 * (24 - (shift)))
#define ARP_STRIP(ly) ((ly) - 7)
#define ARP_LENGTH_X 495
// Stage 24d: the PSG row is 460 wide, its lane 378 from ARP_LX, its right column at 648.
#define PSG_COL(k) (ARP_LX + (k) * 378 / 4)
#define PSG_BAND (2 * (PSG_COL(1) - 1 - PSG_COL(0)))
#define PSG_TOPS (2 * (PSG_COL(4) - PSG_COL(0) - 4))
#define ARP_PSG_RX 648
#define ARP_PSG_LENGTH_X 673
#define ARP_LENGTH(ly) ((ly) + 11)
#define ARP_SWITCH_X 676
#define ARP_SWITCH_Y 92
#define ACTION_STORE_X 500
#define COLOR_LINE 0x444444
#define COLOR_HINT 0x666666
// Stage 21 (docs/design/ch3-ops.md): LFO and LFO RESET in the strip, the segments of CH3 NOTES,
// operator 2's pitch tag in its header and its editor under it (SEMI, FINE, FIX), and the ids.
#define LFO_ON_X 30
#define LFO_RESET_X 190
// Stage 22: three segments, OPS 376 w 34, 1 NOTE 412 w 38, CSM 452 w 34.
#define CH3_NOTES_ONE_X 431
#define CH3_NOTES_CSM_X 469
#define CH3_NOTES_Y 489
#define TAG2_X 408
#define TAG2_LEFT 384
#define TAG_Y 219
#define PITCH_LEFT 357
#define PITCH_TOP 227
#define PITCH_SEMI_X 414
#define PITCH_SEMI_Y 239
#define PITCH_FINE_Y 263
#define PITCH_FIX_X 383
#define PITCH_FIX_Y 287
#define CH3_SPECIAL 4
#define CH3_NOTES 10
#define LFO_RESET 11
#define CH3_OP2_SEMI 15
#define CH3_OP2_FINE 16
#define CH3_OP2_FIXED 17
#define CH3_OP2_TL 1322

static const int OP_X[4] = {242, 358, 474, 590};
#define OP_RIGHT_W 112
#define OP_AR_Y 291
#define OP_DT_Y 419
#define OP_RS_Y 444
#define GRAPH_LEFT 243
#define GRAPH_RIGHT 523
#define GRAPH_TOP 83
#define GRAPH_BOTTOM 205
// Operator 1's SSG button in its header, and its choice under the header: OFF, then the shape 8.
#define SSG_BUTTON_X 336
#define SSG_BUTTON_Y 219
#define SSG_PICK_Y 239
#define SSG_PICK_OFF_X 260
#define SSG_PICK_8_X 298
#define CH4_OP3_AR 1437
#define CH4_OP3_RS 1436
#define CH4_OP1_SSG 1421
#define CH4_OP1_DT 1413
#define CH4_OP1_RS 1414
#define CH4_OP1_AM 1420

static int failures;

#define CHECK(condition, ...) \
	do { \
		bool check_passed = (condition); \
		printf("  %s: ", check_passed ? "ok  " : "FAIL"); \
		printf(__VA_ARGS__); \
		printf("\n"); \
		if (!check_passed) failures++; \
	} while (0)

static const void *host_get_extension(const clap_host_t *host, const char *id)
{
	(void)host;
	(void)id;
	return NULL;
}

static void host_nop(const clap_host_t *host)
{
	(void)host;
}

static const clap_host_t host = {
	CLAP_VERSION_INIT, NULL, "gui_test", "", "", "1", host_get_extension, host_nop, host_nop, host_nop,
};

static uint32_t in_size(const clap_input_events_t *list)
{
	return list->ctx ? 1 : 0;
}

static const clap_event_header_t *in_get(const clap_input_events_t *list, uint32_t index)
{
	(void)index;
	return list->ctx;
}

// What the plugin told the host: gesture begins, values and ends, with the last id of each.
static int pushed[3];
static uint32_t pushed_id[3];
static double pushed_value;

static bool out_try_push(const clap_output_events_t *list, const clap_event_header_t *event)
{
	(void)list;
	int kind = event->type == CLAP_EVENT_PARAM_GESTURE_BEGIN ? 0 : event->type == CLAP_EVENT_PARAM_VALUE ? 1 : event->type == CLAP_EVENT_PARAM_GESTURE_END ? 2 : -1;
	if (kind == 1) {
		pushed_id[1] = ((const clap_event_param_value_t *)event)->param_id;
		pushed_value = ((const clap_event_param_value_t *)event)->value;
	} else if (kind >= 0) {
		pushed_id[kind] = ((const clap_event_param_gesture_t *)event)->param_id;
	}
	if (kind >= 0) {
		pushed[kind]++;
	}
	return true;
}

static const clap_plugin_t *plugin;
// Samples processed, the plugin's clock (stage 24f: where the FM cursor must be).
static uint64_t frames_run;
static float left[BLOCK], right[BLOCK];
static uint32_t pixels[WIDTH * HEIGHT];

// One block of audio, with an optional MIDI message, then whatever the window wants to do.
static void run_block(const void *event)
{
	float *channels[2] = {left, right};
	clap_audio_buffer_t out = {channels, NULL, 2, 0, 0};
	clap_input_events_t in = {(void *)event, in_size, in_get};
	clap_output_events_t out_events = {NULL, out_try_push};
	clap_process_t process = {0};
	process.frames_count = BLOCK;
	process.steady_time = -1;
	process.audio_outputs = &out;
	process.audio_outputs_count = 1;
	process.in_events = &in;
	process.out_events = &out_events;
	plugin->process(plugin, &process);
	frames_run += BLOCK;
	MSG message;
	while (PeekMessageA(&message, NULL, 0, 0, PM_REMOVE)) {
		DispatchMessageA(&message);
	}
	Sleep(BLOCK * 1000 / RATE);
}

static void run_ms(int ms)
{
	DWORD start = GetTickCount();
	while (GetTickCount() - start < (DWORD)ms) {
		run_block(NULL);
	}
}

static void send_midi(uint8_t status, uint8_t a, uint8_t b)
{
	clap_event_midi_t midi = {{sizeof(midi), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI, 0}, 0, {status, a, b}};
	run_block(&midi);
}

static void capture(HWND window)
{
	HDC screen = GetDC(NULL);
	BITMAPINFO info = {0};
	info.bmiHeader.biSize = sizeof(info.bmiHeader);
	info.bmiHeader.biWidth = WIDTH;
	info.bmiHeader.biHeight = -HEIGHT;
	info.bmiHeader.biPlanes = 1;
	info.bmiHeader.biBitCount = 32;
	void *bits = NULL;
	HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, NULL, 0);
	HDC dc = CreateCompatibleDC(screen);
	HGDIOBJ old = SelectObject(dc, bitmap);
	SendMessageA(window, WM_PRINTCLIENT, (WPARAM)dc, PRF_CLIENT);
	GdiFlush();
	memcpy(pixels, bits, sizeof(pixels));
	SelectObject(dc, old);
	DeleteObject(bitmap);
	DeleteDC(dc);
	ReleaseDC(NULL, screen);
}

static int count_color(int top, int bottom, uint32_t color)
{
	int count = 0;
	for (int i = top * WIDTH; i < bottom * WIDTH; i++) {
		count += (pixels[i] & 0xFFFFFF) == color;
	}
	return count;
}

static int count_color_in(int left, int right, int top, int bottom, uint32_t color)
{
	int count = 0;
	for (int y = top; y < bottom; y++) {
		for (int x = left; x < right; x++) {
			count += (pixels[y * WIDTH + x] & 0xFFFFFF) == color;
		}
	}
	return count;
}

static int count_not_black(int top, int bottom)
{
	int count = 0;
	for (int i = top * WIDTH; i < bottom * WIDTH; i++) {
		count += (pixels[i] & 0xFFFFFF) != 0;
	}
	return count;
}

static int count_not_black_in(int left, int right, int top, int bottom)
{
	int count = 0;
	for (int y = top; y < bottom; y++) {
		for (int x = left; x < right; x++) {
			count += (pixels[y * WIDTH + x] & 0xFFFFFF) != 0;
		}
	}
	return count;
}

static void save_bmp(const char *prefix, const char *name)
{
	char path[MAX_PATH];
	snprintf(path, sizeof(path), "%s-%s.bmp", prefix, name);
	FILE *f = fopen(path, "wb");
	if (!f) {
		return;
	}
	BITMAPINFOHEADER info = {0};
	info.biSize = sizeof(info);
	info.biWidth = WIDTH;
	info.biHeight = -HEIGHT;
	info.biPlanes = 1;
	info.biBitCount = 32;
	BITMAPFILEHEADER file = {0};
	file.bfType = 0x4D42;
	file.bfOffBits = sizeof(file) + sizeof(info);
	file.bfSize = file.bfOffBits + sizeof(pixels);
	fwrite(&file, sizeof(file), 1, f);
	fwrite(&info, sizeof(info), 1, f);
	fwrite(pixels, sizeof(pixels), 1, f);
	fclose(f);
}

static void click(HWND window, int x, int y)
{
	SendMessageA(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y));
	SendMessageA(window, WM_LBUTTONUP, 0, MAKELPARAM(x, y));
}

static void drag(HWND window, int x, int y, int to_y)
{
	SendMessageA(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y));
	for (int step = 1; step <= 4; step++) {
		SendMessageA(window, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(x, y + (to_y - y) * step / 4));
	}
	SendMessageA(window, WM_LBUTTONUP, 0, MAKELPARAM(x, to_y));
}

// A wheel turn over a point of the window: delta in 120ths of a notch, as Windows gives it.
static void wheel(HWND window, int x, int y, int delta)
{
	POINT screen = {x, y};
	ClientToScreen(window, &screen);
	SendMessageA(window, WM_MOUSEWHEEL, MAKEWPARAM(0, (short)delta), MAKELPARAM(screen.x, screen.y));
}

// A drag from one point to another in four moves (stage 22c: a row from one list to the other).
static void drag_to(HWND window, int x, int y, int to_x, int to_y)
{
	SendMessageA(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y));
	for (int step = 1; step <= 4; step++) {
		SendMessageA(window, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(x + (to_x - x) * step / 4, y + (to_y - y) * step / 4));
	}
	SendMessageA(window, WM_LBUTTONUP, 0, MAKELPARAM(to_x, to_y));
}

// The test's library folder (MDMI_BANKS) and the file NEW makes there (MDMI_TEST_NEW).
static char banks_dir[MAX_PATH], new_bank[MAX_PATH];

static size_t read_file(const char *path, uint8_t *bytes, size_t most)
{
	FILE *file = fopen(path, "rb");
	if (!file) {
		return 0;
	}
	const size_t size = fread(bytes, 1, most, file);
	fclose(file);
	return size;
}

// A sideways drag in four moves, as a hand makes it.
static void drag_x(HWND window, int x, int y, int to_x)
{
	SendMessageA(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y));
	for (int step = 1; step <= 4; step++) {
		SendMessageA(window, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(x + (to_x - x) * step / 4, y));
	}
	SendMessageA(window, WM_LBUTTONUP, 0, MAKELPARAM(to_x, y));
}

// The longest run of a colour on a row between left and right: its first x and its length.
static int longest_run(int y, int left, int right, uint32_t color, int *first)
{
	int best = 0, run = 0;
	*first = -1;
	for (int x = left; x < right; x++) {
		run = (pixels[y * WIDTH + x] & 0xFFFFFF) == color ? run + 1 : 0;
		if (run > best) {
			best = run;
			*first = x - run + 1;
		}
	}
	return best;
}

static int first_of(int y, int left, int right, uint32_t color)
{
	for (int x = left; x < right; x++) {
		if ((pixels[y * WIDTH + x] & 0xFFFFFF) == color) {
			return x;
		}
	}
	return -1;
}

// Runs of a colour on a row between left and right: the columns of a lane.
static int runs_of(int y, int left, int right, uint32_t color)
{
	int runs = 0;
	for (int x = left; x < right; x++) {
		runs += (pixels[y * WIDTH + x] & 0xFFFFFF) == color && (pixels[y * WIDTH + x - 1] & 0xFFFFFF) != color;
	}
	return runs;
}

static void double_click(HWND window, int x, int y)
{
	click(window, x, y);
	SendMessageA(window, WM_LBUTTONDBLCLK, MK_LBUTTON, MAKELPARAM(x, y));
	SendMessageA(window, WM_LBUTTONUP, 0, MAKELPARAM(x, y));
}

// The pitch editor's frame, its top line: all of it while the editor of operator 2 is open.
static int pitch_frame(void)
{
	return count_color_in(PITCH_LEFT, PITCH_LEFT + 114, PITCH_TOP, PITCH_TOP + 1, COLOR_LABEL);
}

static int get_value(clap_id id)
{
	const clap_plugin_params_t *params = plugin->get_extension(plugin, CLAP_EXT_PARAMS);
	double value = -1;
	params->get_value(plugin, id, &value);
	return (int)value;
}

// A project in memory, for the state extension.
static uint8_t project[1 << 20];
static size_t project_size, project_read;

static int64_t project_write(const clap_ostream_t *stream, const void *buffer, uint64_t size)
{
	(void)stream;
	if (project_size + size > sizeof(project)) {
		return -1;
	}
	memcpy(project + project_size, buffer, size);
	project_size += size;
	return (int64_t)size;
}

static int64_t project_take(const clap_istream_t *stream, void *buffer, uint64_t size)
{
	(void)stream;
	const size_t left = project_size - project_read;
	const size_t count = size < left ? size : left;
	memcpy(buffer, project + project_read, count);
	project_read += count;
	return (int64_t)count;
}

// Files dropped as from Explorer (stage 18): a data object holding only CF_HDROP, handed straight to
// the window's drop target.
typedef struct {
	IDataObject iface;
	HGLOBAL files;
} test_data;

static HRESULT STDMETHODCALLTYPE data_query(IDataObject *self, REFIID riid, void **out)
{
	if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDataObject)) {
		*out = self;
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE data_ref(IDataObject *self)
{
	(void)self;
	return 1;
}

static HRESULT STDMETHODCALLTYPE data_get(IDataObject *self, FORMATETC *format, STGMEDIUM *medium)
{
	if (format->cfFormat != CF_HDROP || !(format->tymed & TYMED_HGLOBAL)) {
		return DV_E_FORMATETC;
	}
	const HGLOBAL from = ((test_data *)self)->files;
	const SIZE_T size = GlobalSize(from);
	HGLOBAL copy = GlobalAlloc(GMEM_MOVEABLE, size);
	memcpy(GlobalLock(copy), GlobalLock(from), size);
	GlobalUnlock(copy);
	GlobalUnlock(from);
	medium->tymed = TYMED_HGLOBAL;
	medium->hGlobal = copy;
	medium->pUnkForRelease = NULL;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE data_query_get(IDataObject *self, FORMATETC *format)
{
	(void)self;
	return format->cfFormat == CF_HDROP ? S_OK : DV_E_FORMATETC;
}

static HRESULT STDMETHODCALLTYPE data_none(void)
{
	return E_NOTIMPL;
}

static IDataObjectVtbl data_vtable = {
	data_query, data_ref, data_ref, data_get, (void *)data_none, data_query_get, (void *)data_none,
	(void *)data_none, (void *)data_none, (void *)data_none, (void *)data_none, (void *)data_none,
};

// Drops one file at a point of the window; the effect the window chose.
static DWORD drop_file(HWND window, const char *path, int x, int y)
{
	wchar_t wide[MAX_PATH];
	const int chars = MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, MAX_PATH);
	const SIZE_T size = sizeof(DROPFILES) + (chars + 1) * sizeof(wchar_t);
	test_data data = {{&data_vtable}, GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, size)};
	DROPFILES *files = GlobalLock(data.files);
	files->pFiles = sizeof(DROPFILES);
	files->fWide = TRUE;
	memcpy((uint8_t *)files + sizeof(DROPFILES), wide, chars * sizeof(wchar_t));
	GlobalUnlock(data.files);
	IDropTarget *target = GetPropA(window, "OleDropTargetInterface");
	POINT screen = {x, y};
	ClientToScreen(window, &screen);
	const POINTL point = {screen.x, screen.y};
	DWORD effect = DROPEFFECT_COPY;
	if (target) {
		target->lpVtbl->DragEnter(target, &data.iface, MK_LBUTTON, point, &effect);
		effect = DROPEFFECT_COPY;
		target->lpVtbl->DragOver(target, MK_LBUTTON, point, &effect);
		const DWORD over = effect;
		effect = DROPEFFECT_COPY;
		if (over != DROPEFFECT_NONE) {
			target->lpVtbl->Drop(target, &data.iface, MK_LBUTTON, point, &effect);
		} else {
			target->lpVtbl->DragLeave(target);
		}
		effect = over;
	} else {
		effect = (DWORD)-1;
	}
	GlobalFree(data.files);
	return effect;
}

// A mono 16-bit WAV of a sine to drop.
static void write_wav(const char *path, uint32_t hz, uint32_t frames)
{
	FILE *f = fopen(path, "wb");
	const uint32_t data = frames * 2, riff = 36 + data, fmt = 16, rate = hz * 2;
	const uint16_t pcm = 1, mono = 1, align = 2, bits = 16;
	fwrite("RIFF", 1, 4, f);
	fwrite(&riff, 4, 1, f);
	fwrite("WAVEfmt ", 1, 8, f);
	fwrite(&fmt, 4, 1, f);
	fwrite(&pcm, 2, 1, f);
	fwrite(&mono, 2, 1, f);
	fwrite(&hz, 4, 1, f);
	fwrite(&rate, 4, 1, f);
	fwrite(&align, 2, 1, f);
	fwrite(&bits, 2, 1, f);
	fwrite("data", 1, 4, f);
	fwrite(&data, 4, 1, f);
	for (uint32_t i = 0; i < frames; i++) {
		const int16_t v = (int16_t)(12000 * sin(2 * 3.14159265358979 * 330 * i / hz));
		fwrite(&v, 2, 1, f);
	}
	fclose(f);
}

// A change of samples goes into the running core (D39): for a while the header must never say
// LOADING. The ms waited, or -1 if it did.
static long wait_applied(HWND window)
{
	const DWORD start = GetTickCount();
	while (GetTickCount() - start < 400) {
		run_ms(10);
		capture(window);
		if (count_color(0, HEADER_H, COLOR_AMBER) > 0) {
			return -1;
		}
	}
	return (long)(GetTickCount() - start);
}

int main(int argc, char **argv)
{
	if (argc != 3) {
		fprintf(stderr, "usage: %s <plugin.clap> <picture_prefix>\n", argv[0]);
		return 2;
	}
	const bool expect_crash = getenv("MDMI_TEST_CRASH") != NULL;
	// Stage 22c: the libraries in a folder of the test's, gone before it starts.
	snprintf(banks_dir, sizeof(banks_dir), "%s-banks", argv[2]);
	snprintf(new_bank, sizeof(new_bank), "%s\\First.mdbank", banks_dir);
	const char *const left[] = {"First.mdbank", "broken.mdbank", "First.mdbank.tmp"};
	for (size_t i = 0; i < sizeof(left) / sizeof(*left); i++) {
		char path[MAX_PATH];
		snprintf(path, sizeof(path), "%s\\%s", banks_dir, left[i]);
		SetFileAttributesA(path, FILE_ATTRIBUTE_NORMAL);
		DeleteFileA(path);
	}
	RemoveDirectoryA(banks_dir);
	SetEnvironmentVariableA("MDMI_BANKS", banks_dir);
	SetEnvironmentVariableA("MDMI_TEST_NEW", new_bank);
	static char env_banks[MAX_PATH + 16], env_new[MAX_PATH + 16];
	snprintf(env_banks, sizeof(env_banks), "MDMI_BANKS=%s", banks_dir);
	snprintf(env_new, sizeof(env_new), "MDMI_TEST_NEW=%s", new_bank);
	_putenv(env_banks);
	_putenv(env_new);
	HMODULE lib = LoadLibraryA(argv[1]);
	if (!lib) {
		fprintf(stderr, "LoadLibrary failed: %lu\n", GetLastError());
		return 1;
	}
	const clap_plugin_entry_t *entry = (const clap_plugin_entry_t *)GetProcAddress(lib, "clap_entry");
	if (!entry || !entry->init(argv[1])) {
		fprintf(stderr, "no clap_entry\n");
		return 1;
	}
	const clap_plugin_factory_t *factory = entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
	plugin = factory->create_plugin(factory, &host, factory->get_plugin_descriptor(factory, 0)->id);
	if (!plugin || !plugin->init(plugin) || !plugin->activate(plugin, RATE, BLOCK, BLOCK)) {
		fprintf(stderr, "plugin failed to start\n");
		return 1;
	}
	const clap_plugin_gui_t *gui = plugin->get_extension(plugin, CLAP_EXT_GUI);
	CHECK(gui != NULL, "plugin has a GUI");
	if (!gui) {
		return 1;
	}
	CHECK(gui->is_api_supported(plugin, CLAP_WINDOW_API_WIN32, false), "embedded Win32 window is supported");
	CHECK(gui->create(plugin, CLAP_WINDOW_API_WIN32, false), "window created");
	uint32_t width = 0, height = 0;
	gui->get_size(plugin, &width, &height);
	CHECK(width == WIDTH && height == HEIGHT, "size is %ux%u", width, height);

	HWND parent = CreateWindowExA(0, "STATIC", "gui_test", WS_POPUP, 0, 0, WIDTH, HEIGHT, NULL, NULL, NULL, NULL);
	clap_window_t window_ref = {CLAP_WINDOW_API_WIN32, {.win32 = parent}};
	CHECK(gui->set_parent(plugin, &window_ref), "window attached");
	CHECK(gui->show(plugin), "window shown");
	HWND window = FindWindowExA(parent, NULL, "MDMIPluginWindow", NULL);
	CHECK(window != NULL, "child window exists");
	if (!window) {
		return 1;
	}

	if (expect_crash) {
		DWORD start = GetTickCount();
		do {
			run_ms(100);
			capture(window);
		} while (count_color(0, HEADER_H, COLOR_ERROR) == 0 && GetTickCount() - start < BOOT_TIMEOUT_MS);
		CHECK(count_color(0, HEADER_H, COLOR_ERROR) > 0, "dead engine is shown in red");
		save_bmp(argv[2], "crash");
	} else {
		// RUNNING is drawn in the same colour as a sounding note, so this waits for the boot.
		DWORD start = GetTickCount();
		do {
			run_ms(100);
			capture(window);
		} while (count_color(0, HEADER_H, COLOR_PLAYING) == 0 && GetTickCount() - start < BOOT_TIMEOUT_MS);
		CHECK(count_color(0, HEADER_H, COLOR_PLAYING) > 0, "header says the engine runs after %lu ms", GetTickCount() - start);
		run_ms(500);

		capture(window);
		int quiet = count_color(HEADER_H, HEIGHT, COLOR_PLAYING);
		CHECK(quiet == 0, "nothing is lit before the first note");
		send_midi(0x90, 60, 110);
		send_midi(0x96, 69, 90);
		run_ms(200);
		capture(window);
		int playing = count_color(HEADER_H, HEIGHT, COLOR_PLAYING);
		CHECK(playing > 0, "notes light up the table (%d pixels)", playing);
		save_bmp(argv[2], "editor");

		click(window, BUTTON_ROM_X, BUTTON_Y);
		run_ms(300);
		capture(window);
		int rom_with_notes = count_not_black(HEADER_H, HEIGHT);
		CHECK(rom_with_notes > 1000, "ROM screen is drawn (%d pixels)", rom_with_notes);
		save_bmp(argv[2], "rom");
		// Stage 36 (D81): the TV switch. With no process calls the engine fills its ring and stops, so
		// the ROM's frame stands still: OFF, RGB, COMP and OFF again all draw the same frame.
		// check_tv.py compares the looks with the model; here: OFF is the frame at x2 as before, the
		// frame is the same after the looks (they only read it), and what a look costs.
		{
			Sleep(400);
			capture(window);
			static uint32_t off[WIDTH * HEIGHT];
			memcpy(off, pixels, sizeof(pixels));
			int blocks = 0;
			for (int y = 54; y < 54 + 448; y += 2) {
				for (int x = 40; x < 40 + 640; x += 2) {
					const uint32_t p = pixels[y * WIDTH + x];
					blocks += pixels[y * WIDTH + x + 1] != p || pixels[(y + 1) * WIDTH + x] != p || pixels[(y + 1) * WIDTH + x + 1] != p;
				}
			}
			save_bmp(argv[2], "tv-off");
			LARGE_INTEGER hz, t0, t1;
			QueryPerformanceFrequency(&hz);
			double ms[2] = {0, 0};
			static const char *const NAMES[2] = {"tv-rgb", "tv-comp"};
			int changed[2] = {0, 0};
			for (int look = 0; look < 2; look++) {
				// The click draws the window, the look worked out for every line of the frame.
				QueryPerformanceCounter(&t0);
				click(window, look ? 650 : 615, 40);
				QueryPerformanceCounter(&t1);
				capture(window);
				ms[look] = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)hz.QuadPart;
				save_bmp(argv[2], NAMES[look]);
				for (int i = 54 * WIDTH; i < (54 + 448) * WIDTH; i++) {
					changed[look] += pixels[i] != off[i];
				}
			}
			QueryPerformanceCounter(&t0);
			capture(window);
			QueryPerformanceCounter(&t1);
			const double cached = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)hz.QuadPart;
			click(window, 585, 40);
			QueryPerformanceCounter(&t0);
			capture(window);
			QueryPerformanceCounter(&t1);
			const double plain = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)hz.QuadPart;
			printf("    TV look: the window drawn with every line worked out, RGB %.1f ms, COMP %.1f ms; again from the kept lines %.1f ms; OFF %.1f ms\n", ms[0], ms[1], cached, plain);
			const int same = !memcmp(off + 54 * WIDTH, pixels + 54 * WIDTH, 448 * WIDTH * sizeof(uint32_t));
			CHECK(blocks == 0 && changed[0] > 1000 && changed[1] > 1000 && same,
				"TV switch: OFF draws the frame at x2 (%d uneven blocks), RGB and COMP draw it otherwise (%d, %d px), OFF again is the same frame bit for bit", blocks, changed[0], changed[1]);
		}
		static uint32_t before[WIDTH * HEIGHT];
		memcpy(before, pixels, sizeof(pixels));

		send_midi(0x80, 60, 0);
		send_midi(0x86, 69, 0);
		run_ms(500);
		capture(window);
		CHECK(memcmp(before + HEADER_H * WIDTH, pixels + HEADER_H * WIDTH, (HEIGHT - HEADER_H) * WIDTH * sizeof(uint32_t)) != 0, "ROM screen is live: it changes when notes stop");

		// Stage 24: the ROM's screen names each channel's sound where CC7B=00 and P=116 were, and
		// EXPORT writes the ROM for the SD card. The ROM's picture is at (40, 54), 16 px a tile; since
		// stage 24a the names are columns 7-22 of FM1's row 2.
		run_ms(3500);
		capture(window);
		save_bmp(argv[2], "rom-names");
		const int name_pixels = count_not_black_in(40 + 7 * 16, 40 + 23 * 16, 54 + 2 * 16, 54 + 3 * 16);
		CHECK(name_pixels > 200, "FM1's row names its sound, also long after the last change (%d px)", name_pixels);
		char export_path[MAX_PATH];
		snprintf(export_path, sizeof(export_path), "%s-export.bin", argv[2]);
		SetFileAttributesA(export_path, FILE_ATTRIBUTE_NORMAL);
		DeleteFileA(export_path);
		SetEnvironmentVariableA("MDMI_TEST_EXPORT", export_path);
		static char env_export[MAX_PATH + 24];
		snprintf(env_export, sizeof(env_export), "MDMI_TEST_EXPORT=%s", export_path);
		_putenv(env_export);
		click(window, 452 + 40, BUTTON_Y);
		run_ms(200);
		capture(window);
		save_bmp(argv[2], "rom-export");
		static uint8_t exported[(4u << 20) + 1];
		const size_t exported_size = read_file(export_path, exported, sizeof(exported));
		CHECK(exported_size && exported_size % (512u << 10) == 0 && count_color_in(0, WIDTH, FOOTER_TOP, HEIGHT, COLOR_TEXT) > 100,
			"EXPORT writes the ROM (%zu bytes) and the footer says where", exported_size);
		SetFileAttributesA(export_path, FILE_ATTRIBUTE_READONLY);
		click(window, 452 + 40, BUTTON_Y);
		run_ms(200);
		capture(window);
		static uint8_t again[(4u << 20) + 1];
		const size_t again_size = read_file(export_path, again, sizeof(again));
		CHECK(count_color_in(0, WIDTH, FOOTER_TOP, HEIGHT, COLOR_ERROR) > 100 && again_size == exported_size && !memcmp(again, exported, exported_size),
			"a read-only file: the footer says NOT WRITTEN in red, the file stays");
		SetFileAttributesA(export_path, FILE_ATTRIBUTE_NORMAL);
		DeleteFileA(export_path);

		// A file named in Cyrillic: the rail shows its name transliterated, not as gaps.
		click(window, BUTTON_EDITOR_X, BUTTON_Y);
		run_ms(100);
		capture(window);
		const int gm_name = count_color_in(60, 236, RAIL_CH1_TOP, RAIL_CH1_TOP + 20, COLOR_TEXT);
		char cyrillic[MAX_PATH];
		GetTempPathA(MAX_PATH, cyrillic);
		strcat(cyrillic, "\xD0\x91\xD0\xB0\xD1\x81.tfi");
		{
			static const uint8_t tfi[42] = {4, 0};
			wchar_t wide[MAX_PATH];
			MultiByteToWideChar(CP_UTF8, 0, cyrillic, -1, wide, MAX_PATH);
			FILE *f = _wfopen(wide, L"wb");
			fwrite(tfi, 1, sizeof(tfi), f);
			fclose(f);
		}
		drop_file(window, cyrillic, RAIL_X, RAIL_CH1_TOP + 10);
		run_ms(200);
		capture(window);
		save_bmp(argv[2], "cyrillic-name");
		const int bas_name = count_color_in(60, 236, RAIL_CH1_TOP, RAIL_CH1_TOP + 20, COLOR_TEXT);
		CHECK(bas_name > 0 && bas_name < gm_name, "a dropped Bas.tfi in Cyrillic draws as Bas (%d px, Grand Piano %d)", bas_name, gm_name);
		click(window, 656 + 28, BUTTON_Y);
		run_ms(200);
		{
			wchar_t wide[MAX_PATH];
			MultiByteToWideChar(CP_UTF8, 0, cyrillic, -1, wide, MAX_PATH);
			DeleteFileW(wide);
		}
		click(window, BUTTON_ROM_X, BUTTON_Y);
		run_ms(100);

		click(window, BUTTON_EDITOR_X, BUTTON_Y);
		run_ms(100);
		capture(window);
		CHECK(count_color(HEADER_H, HEIGHT, COLOR_PLAYING) == 0, "table goes dark after note off");

		// A chord on channel 4 borrows voices: a bracket joins the rows, between numbers and names.
		send_midi(0x93, 60, 110);
		send_midi(0x93, 64, 110);
		send_midi(0x93, 67, 110);
		run_ms(300);
		capture(window);
		int bracket = count_color_in(28, 54, HEADER_H, RAIL_CH4_Y + 200, COLOR_PLAYING);
		CHECK(bracket > 40, "a chord's borrowed voices are joined by a bracket (%d pixels)", bracket);
		// Stage 30: each borrowed row's arrowhead is a solid 4 x 7 at the name's height, tip at x 51;
		// the chord's own row a square of 9 x 9 on a spine of 1 px.
		const int heads = count_color_in(48, 52, HEADER_H, RAIL_CH4_Y + 200, COLOR_PLAYING);
		int square = 0;
		for (int y = RAIL_CH4_Y - 40; y < RAIL_CH4_Y + 40; y++) {
			int first;
			square += longest_run(y, 28, 50, COLOR_PLAYING, &first) >= 9;
		}
		CHECK(heads >= 2 * 16 && square == 9, "the chord's arrowheads are solid (%d px for two), its own row a square of 9 rows (%d)", heads, square);
		// Stage 24d: the source says ROM N, grey for the factory sound.
		CHECK(count_color_in(600, 708, 30, 48, COLOR_LABEL) > 20, "the source of a factory sound is grey");
		save_bmp(argv[2], "voices");
		send_midi(0x83, 60, 0);
		send_midi(0x83, 64, 0);
		send_midi(0x83, 67, 0);
		run_ms(300);
		capture(window);
		CHECK(count_color_in(28, 54, HEADER_H, RAIL_CH4_Y + 200, COLOR_PLAYING) == 0, "and the bracket goes with the chord");

		// Stage 9: the rail's bar shows how hard a held note was played, CC7 x velocity / 127.
		send_midi(0x90, 60, 127);
		run_ms(200);
		capture(window);
		const int hard = count_color_in(BAR_LEFT, BAR_RIGHT, RAIL_CH1_TOP + 25, RAIL_CH1_TOP + 29, COLOR_PLAYING);
		send_midi(0x80, 60, 0);
		send_midi(0x90, 60, 32);
		run_ms(200);
		capture(window);
		const int soft = count_color_in(BAR_LEFT, BAR_RIGHT, RAIL_CH1_TOP + 25, RAIL_CH1_TOP + 29, COLOR_PLAYING);
		send_midi(0x80, 60, 0);
		run_ms(200);
		capture(window);
		CHECK(hard > 2 * soft && soft > 0 && count_color_in(BAR_LEFT, BAR_RIGHT, RAIL_CH1_TOP + 25, RAIL_CH1_TOP + 29, COLOR_PLAYING) == 0,
			"velocity 127 lights %d px of channel 1's bar, 32 lights %d, none after note off", hard, soft);

		// A chord of seven on channel 4 has six voices: the brackets stay, its row says CUT in red.
		for (int key = 60; key < 67; key++) {
			send_midi(0x93, (uint8_t)key, (uint8_t)(20 + (key - 60) * 17));
		}
		run_ms(300);
		capture(window);
		save_bmp(argv[2], "cut");
		CHECK(count_color_in(28, 54, HEADER_H, RAIL_CH4_Y + 200, COLOR_PLAYING) > 40, "seven notes keep the bracket");
		CHECK(count_color_in(BAR_LEFT, BAR_RIGHT, RAIL_CH4_TOP + 20, RAIL_CH4_TOP + 39, COLOR_ERROR) > 0, "and channel 4's row says CUT");
		// Stage 24c: every bar ends at x 196 with a chord's owner, its CUT and the lent rows' CHn.
		{
			int ends[6], same = 0;
			for (int row = 0; row < 6; row++) {
				const uint32_t *line = pixels + (RAIL_CH1_TOP + row * ROW_H + 26) * WIDTH;
				ends[row] = -1;
				for (int x = BAR_LEFT; x < 202; x++) {
					ends[row] = (line[x] & 0xFFFFFF) != (line[200] & 0xFFFFFF) ? x : ends[row];
				}
				same += ends[row] == 196;
			}
			CHECK(same == 6, "the bars end at one x on every FM row (%d %d %d %d %d %d)", ends[0], ends[1], ends[2], ends[3], ends[4], ends[5]);
		}
		// Stage 24b: over the row the footer says why, in red. Without a real mouse the hover leaves as
		// soon as messages are pumped, so the frame is taken right after the move (WM_PRINTCLIENT draws
		// it afresh). Only red pixels are counted, not the text.
		SendMessageA(window, WM_MOUSEMOVE, 0, MAKELPARAM(100, RAIL_CH4_TOP + 10));
		capture(window);
		CHECK(count_color_in(0, WIDTH, FOOTER_TOP, HEIGHT, COLOR_ERROR) > 50, "over the cut row the footer says why no voice was free");
		save_bmp(argv[2], "cut-why");
		SendMessageA(window, WM_MOUSEMOVE, 0, MAKELPARAM(BUTTON_EDITOR_X, BUTTON_Y));
		// Each borrowed row's bar shows the velocity of the note on its voice, 20 to 122: counted in
		// segments, whose width depends on the text right of the bar, left of any text.
		int lit_rows = 0, fewest = 1000, most = 0;
		for (int row = 0; row < 6; row++) {
			const uint32_t *line = pixels + (RAIL_CH1_TOP + row * ROW_H + 26) * WIDTH;
			int segments = 0;
			for (int x = BAR_LEFT; x < 170; x++) {
				segments += (line[x] & 0xFFFFFF) == COLOR_PLAYING && (line[x - 1] & 0xFFFFFF) != COLOR_PLAYING;
			}
			lit_rows += segments > 0;
			fewest = segments > 0 && segments < fewest ? segments : fewest;
			most = segments > most ? segments : most;
		}
		CHECK(lit_rows == 6 && most - fewest >= 4, "all six rows light their bars, from %d to %d segments", fewest, most);
		for (int key = 60; key < 67; key++) {
			send_midi(0x83, (uint8_t)key, 0);
		}
		run_ms(300);

		// FM SP: channel 3's notes in four places, the lead (channel 3 itself) first and green,
		// operator 1 (MIDI 11) in the next place.
		click(window, CH3_SPECIAL_X, CH3_SPECIAL_Y);
		run_ms(300);
		send_midi(0x92, 60, 100);
		run_ms(200);
		capture(window);
		// A column at the right, where a one-note row has its note: the lead on top.
		const int column = 204, top = RAIL_CH3_TOP + 2;
		const int lead = count_color_in(column, 226, top, top + 8, COLOR_PLAYING), rest = count_color_in(column, 226, top + 9, top + 37, COLOR_PLAYING);
		send_midi(0x9A, 64, 100);
		run_ms(200);
		capture(window);
		save_bmp(argv[2], "special");
		// Stage 24c: a held operator is green like the lead.
		const int op1 = count_color_in(column, 226, top + 9, top + 17, COLOR_PLAYING);
		// The bar is as long as on a row with one note: its last segment ends short of the column.
		const int bar_past = count_color_in(column, 226, RAIL_CH3_TOP + 25, RAIL_CH3_TOP + 29, COLOR_SP);
		const int bar_light = count_color_in(BAR_LEFT, column, RAIL_CH3_TOP + 25, RAIL_CH3_TOP + 29, COLOR_SP) + count_color_in(BAR_LEFT, column, RAIL_CH3_TOP + 25, RAIL_CH3_TOP + 29, COLOR_PLAYING);
		// The row is light FM: the number in it, and 3 px between the name and the notes below.
		const int light_number = count_color_in(10, BAR_LEFT, RAIL_CH3_TOP, RAIL_CH3_TOP + 39, COLOR_SP);
		const int name_low = count_color_in(BAR_LEFT, 226, RAIL_CH3_TOP + 19, RAIL_CH3_TOP + 22, COLOR_TEXT);
		// Stage 24c: CH3 SPECIAL off and on again releases the ROM's keys; the column shows them let go.
		click(window, CH3_SPECIAL_X, CH3_SPECIAL_Y);
		run_ms(200);
		click(window, CH3_SPECIAL_X, CH3_SPECIAL_Y);
		run_ms(200);
		capture(window);
		const int still_green = count_color_in(column, 226, top, top + 37, COLOR_PLAYING);
		CHECK(still_green == 0 && count_color_in(column, 226, top, top + 37, COLOR_HINT) > 0, "after CH3 SPECIAL off and on the held notes show let go (%d green px)", still_green);
		send_midi(0x82, 60, 0);
		send_midi(0x8A, 64, 0);
		click(window, CH3_SPECIAL_X, CH3_SPECIAL_Y);
		run_ms(300);
		capture(window);
		const int plain_number = count_color_in(10, BAR_LEFT, RAIL_CH3_TOP, RAIL_CH3_TOP + 39, COLOR_SP);
		CHECK(lead > 0 && rest == 0 && op1 > 0, "FM SP: the lead's note is on top of the column (%d px, %d below), operator 1's next (%d px)", lead, rest, op1);
		CHECK(bar_past == 0 && bar_light > 4 * 100, "FM SP: the bar runs as on other rows (%d px lit, %d in the note column)", bar_light, bar_past);
		CHECK(light_number > 20 && plain_number == 0 && name_low == 0, "FM SP: channel 3's number is light FM (%d px, %d after), the name clear of the notes (%d px)", light_number, plain_number, name_low);
		// The mouse leaves the global cell, so its hover frame isn't part of what is compared.
		SendMessageA(window, WM_MOUSEMOVE, 0, MAKELPARAM(BUTTON_EDITOR_X, BUTTON_Y));
		run_ms(100);
		capture(window);
		memcpy(before, pixels, sizeof(pixels));

		// Closing and opening the window again, as a host does, with the sound running.
		click(window, BUTTON_ROM_X, BUTTON_Y);
		gui->hide(plugin);
		gui->destroy(plugin);
		run_ms(100);
		CHECK(gui->create(plugin, CLAP_WINDOW_API_WIN32, false), "window created again");
		CHECK(gui->set_parent(plugin, &window_ref) && gui->show(plugin), "window attached again");
		window = FindWindowExA(parent, NULL, "MDMIPluginWindow", NULL);
		run_ms(300);
		capture(window);
		CHECK(count_not_black(HEADER_H, HEIGHT) > 1000, "the ROM page is still open after reopening");

		// The table must not keep pieces of the ROM page.
		click(window, BUTTON_EDITOR_X, BUTTON_Y);
		run_ms(100);
		capture(window);
		CHECK(memcmp(before + HEADER_H * WIDTH, pixels + HEADER_H * WIDTH, (TABLE_BOTTOM - HEADER_H) * WIDTH * sizeof(uint32_t)) == 0, "the table looks the same after the ROM page");
		// Stage 37: BANK's page top of channel 3 in Ch3 Special is pink too, as its other pages (24d).
		click(window, CH3_SPECIAL_X, CH3_SPECIAL_Y);
		click(window, 10, RAIL_CH3_TOP + 10);
		click(window, BUTTON_BANK_X, BUTTON_Y);
		run_ms(100);
		capture(window);
		const int bank_pink = count_color_in(242, 702, HEADER_H, 80, COLOR_SP);
		click(window, BUTTON_EDITOR_X, BUTTON_Y);
		click(window, CH3_SPECIAL_X, CH3_SPECIAL_Y);
		run_ms(100);
		CHECK(bank_pink > 20, "BANK of channel 3 in Ch3 Special shows its number and square pink (%d px)", bank_pink);
		// Stage 37 (art-director, ui-concept.md): the channel's cells and the page end at x 702.
		capture(window);
		const int past_edge = count_not_black_in(702, 712, HEADER_H + 1, 80);
		const int at_edge = count_not_black_in(700, 702, HEADER_H + 1, 80);
		CHECK(past_edge == 0 && at_edge > 0, "the header's cells end at x 702 (%d px up to it, %d past it)", at_edge, past_edge);

		// Editing by mouse: the view is not a knob, a drag is one gesture on one channel's knob.
		memset(pushed, 0, sizeof(pushed));
		click(window, RAIL_X, RAIL_CH4_Y);
		run_ms(100);
		CHECK(pushed[0] + pushed[1] + pushed[2] == 0, "choosing channel 4 in the window tells the host nothing");
		const int ch3_before = get_value(CH3_OP1_TL), ch4_before = get_value(CH4_OP1_TL);
		drag(window, OP1_TL_X, OP1_TL_Y, OP1_TL_Y - 20);
		run_ms(100);
		CHECK(pushed[0] == 1 && pushed[2] == 1 && pushed_id[0] == CH4_OP1_TL && pushed_id[2] == CH4_OP1_TL, "a drag is one gesture on channel 4's Op1 TL");
		CHECK(pushed[1] > 0 && pushed_id[1] == CH4_OP1_TL && (int)pushed_value == get_value(CH4_OP1_TL), "the host is told each value");
		CHECK(get_value(CH4_OP1_TL) == ch4_before + 10 && get_value(CH3_OP1_TL) == ch3_before, "20 px up raises it by 10, from %d to %d, channel 3 untouched", ch4_before, get_value(CH4_OP1_TL));
		// Stage 22a: the wheel on TL, a notch up and one down.
		memset(pushed, 0, sizeof(pushed));
		wheel(window, OP1_TL_X, OP1_TL_Y, 120);
		run_ms(50);
		const int tl_up = get_value(CH4_OP1_TL);
		wheel(window, OP1_TL_X, OP1_TL_Y, -120);
		run_ms(50);
		CHECK(tl_up == ch4_before + 11 && get_value(CH4_OP1_TL) == ch4_before + 10 && pushed[0] == 2 && pushed[2] == 2 && pushed_id[0] == CH4_OP1_TL,
			"a wheel notch up on TL gives +1, one down -1, a gesture each (%d gestures)", pushed[0]);
		// Two notches quicker than the writer takes them: each steps from the one before.
		wheel(window, OP1_TL_X, OP1_TL_Y, 120);
		wheel(window, OP1_TL_X, OP1_TL_Y, 120);
		run_ms(50);
		const int quick = get_value(CH4_OP1_TL);
		wheel(window, OP1_TL_X, OP1_TL_Y, -240);
		run_ms(50);
		CHECK(quick == ch4_before + 12 && get_value(CH4_OP1_TL) == ch4_before + 10, "two quick notches give +2 (%+d), two down back", quick - ch4_before - 10);
		// Stage 24f: the cursor on the FM graph runs with the note, stays at Note Off while the key is held
		// past 1 s, then runs on through the release; x = 243 + sqrt(s / 2) * 280, s the graph's time.
		{
			SendMessageA(window, WM_MOUSEMOVE, 0, MAKELPARAM(BUTTON_EDITOR_X, BUTTON_Y));
			const uint64_t on = frames_run;
			send_midi(0x93, 60, 100);
			run_ms(500);
			capture(window);
			double s = (double)(frames_run - on) / RATE;
			int want = 243 + (int)(sqrt(s / 2) * 280);
			const int at_half = count_color_in(want - 1, want + 2, 83, 205, COLOR_PLAYING);
			save_bmp(argv[2], "fm-cursor");
			// Stage 35: past 1 s of the plugin's own clock, not of the wall's: under load a wall second
			// renders less, and the note wasn't held 1 s yet (parking of stage 27).
			for (DWORD start = GetTickCount(); frames_run - on < RATE * 14 / 10 && GetTickCount() - start < 5000;) {
				run_block(NULL);
			}
			capture(window);
			const int parked = count_color_in(439, 442, 83, 205, COLOR_PLAYING), off_lit = count_color_in(421, 438, 104, 114, COLOR_PLAYING);
			const uint64_t off = frames_run;
			send_midi(0x83, 60, 0);
			run_ms(350);
			capture(window);
			s = 1.0 + (double)(frames_run - off) / RATE;
			const int want_release = 243 + (int)(sqrt(s / 2) * 280);
			const int in_release = count_color_in(want_release - 1, want_release + 2, 83, 205, COLOR_PLAYING);
			for (DWORD start = GetTickCount(); frames_run - off < RATE * 15 / 10 && GetTickCount() - start < 5000;) {
				run_block(NULL);
			}
			capture(window);
			const int gone = count_color_in(243, 523, 83, 205, COLOR_PLAYING);
			CHECK(at_half >= 100 && parked >= 100 && off_lit > 0 && in_release >= 100 && gone == 0,
				"FM cursor: at x %d after %.2f s (%d px), parked at Note Off with OFF lit (%d px, %d), in the release at x %d (%d px), gone after 2 s (%d px)",
				want, (double)(off - on) / RATE, at_half, parked, off_lit, want_release, in_release, gone);
		}
		// Stage 10: TL now differs from what the channel got, its GM place (D31).
		SendMessageA(window, WM_MOUSEMOVE, 0, MAKELPARAM(OP1_TL_X, OP1_TL_Y));
		run_ms(100);
		capture(window);
		CHECK(count_color_in(OP1_TL_X - 60, OP1_TL_X + 60, OP1_TL_Y - 10, OP1_TL_Y + 12, COLOR_MARK) > 10, "the tweaked TL is amber, with a mark where the place's value is");
		CHECK(count_color_in(OP1_TL_X - 30, OP1_TL_X + 30, OP1_TL_Y - 8, OP1_TL_Y + 8, COLOR_FM_DARK) > 20 && count_color_in(OP1_TL_X - 30, OP1_TL_X + 30, OP1_TL_Y - 8, OP1_TL_Y + 8, COLOR_CELL) > 20,
			"the TL cell is a bar filled up to its value");
		save_bmp(argv[2], "fm");
		// The hover leaves at once without a real mouse; held down, the cell stays the one pointed at.
		SendMessageA(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(OP1_TL_X, OP1_TL_Y));
		run_ms(100);
		capture(window);
		SendMessageA(window, WM_LBUTTONUP, 0, MAKELPARAM(OP1_TL_X, OP1_TL_Y));
		run_ms(100);
		CHECK(count_color_in(0, WIDTH, FOOTER_TOP, HEIGHT, COLOR_MARK) > 50, "and the footer says TL against GM");
		save_bmp(argv[2], "fm-footer");
		// A value from the host marks its cell with a red dot until the mouse edits it, even one the knob
		// already has; the mouse on the cell takes the dot away (D32).
		clap_event_param_value_t from_host = {{sizeof(from_host), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0}, CH4_OP1_TL, NULL, -1, -1, -1, -1, get_value(CH4_OP1_TL)};
		// Within 200 ms of the window's gesture a value is Live's echo of it, no dot (D34).
		run_block(&from_host);
		run_ms(50);
		capture(window);
		CHECK(count_color_in(OP1_TL_X - 60, OP1_TL_X + 60, OP1_TL_Y - 2, OP1_TL_Y + 2, COLOR_ERROR) == 0, "a TL value right after the mouse's gesture is its echo: no dot");
		run_ms(300);
		run_block(&from_host);
		run_ms(100);
		capture(window);
		CHECK(count_color_in(OP1_TL_X - 60, OP1_TL_X + 60, OP1_TL_Y - 2, OP1_TL_Y + 2, COLOR_ERROR) == 9, "a TL value from the host puts a red dot on its cell");
		click(window, OP1_TL_X, OP1_TL_Y);
		run_ms(100);
		capture(window);
		CHECK(count_color_in(OP1_TL_X - 60, OP1_TL_X + 60, OP1_TL_Y - 2, OP1_TL_Y + 2, COLOR_ERROR) == 0, "a touch with the mouse takes it away");
		run_ms(300);
		run_block(&from_host);
		run_ms(2300);
		capture(window);
		CHECK(count_color_in(OP1_TL_X - 60, OP1_TL_X + 60, OP1_TL_Y - 2, OP1_TL_Y + 2, COLOR_ERROR) == 9, "a flat stretch of the lane, seconds without values, keeps it (D33)");
		memset(pushed, 0, sizeof(pushed));
		click(window, RAIL_X, RAIL_CH2_Y);
		run_ms(100);
		CHECK(pushed[0] + pushed[1] + pushed[2] == 0 && get_value(CH4_OP1_TL) == ch4_before + 10, "choosing another channel changes no knob");
		SendMessageA(window, WM_LBUTTONDBLCLK, MK_LBUTTON, MAKELPARAM(OP1_TL_X, OP1_TL_Y));
		click(window, RAIL_X, RAIL_CH4_Y);
		SendMessageA(window, WM_LBUTTONDBLCLK, MK_LBUTTON, MAKELPARAM(OP1_TL_X, OP1_TL_Y));
		run_ms(100);
		CHECK(get_value(CH4_OP1_TL) == ch4_before, "a double click returns it to the preset's value");

		// Stage 11, the FM page. A click on operator 3's lit RS segment changes no value but makes it
		// the operator last edited: its curve turns bright and its header is framed white.
		capture(window);
		static uint32_t graph_before[WIDTH * HEIGHT];
		memcpy(graph_before, pixels, sizeof(pixels));
		click(window, OP_X[2] + 22 + get_value(CH4_OP3_RS) * 13 + 6, OP_RS_Y);
		SendMessageA(window, WM_MOUSEMOVE, 0, MAKELPARAM(BUTTON_EDITOR_X, BUTTON_Y));
		run_ms(100);
		capture(window);
		save_bmp(argv[2], "fm-active");
		CHECK(count_color_in(OP_X[2], OP_X[2] + 70, 212, 213, COLOR_TEXT) > 60 && count_color_in(OP_X[0], OP_X[0] + 70, 212, 213, COLOR_TEXT) == 0
			&& count_color_in(OP_X[2] - 2, OP_X[2] - 1, 230, 450, COLOR_TEXT) == 0,
			"operator 3's header is framed, operator 1's no longer, no frame round the column");
		int graph_changed = 0;
		for (int y = GRAPH_TOP; y < GRAPH_BOTTOM; y++) {
			for (int x = GRAPH_LEFT; x < GRAPH_RIGHT; x++) {
				graph_changed += pixels[y * WIDTH + x] != graph_before[y * WIDTH + x];
			}
		}
		CHECK(graph_changed > 100 && count_color_in(GRAPH_LEFT, GRAPH_RIGHT, GRAPH_TOP, GRAPH_BOTTOM, COLOR_TEXT) > 100,
			"the graph draws operator 3's curve bright instead (%d px changed)", graph_changed);
		memset(pushed, 0, sizeof(pushed));
		drag(window, OP_X[2] + 60, OP_AR_Y, OP_AR_Y + 30);
		SendMessageA(window, WM_MOUSEMOVE, 0, MAKELPARAM(BUTTON_EDITOR_X, BUTTON_Y));
		run_ms(100);
		capture(window);
		CHECK(pushed[0] == 1 && pushed[2] == 1 && pushed_id[0] == CH4_OP3_AR && pushed_id[2] == CH4_OP3_AR, "a drag on operator 3's AR is one gesture on it");
		// The right edges of every row of a column meet: the last pixel inside, the next one black.
		bool edges = true;
		const int rows[9] = {238, 263, 291, 316, 341, 366, 391, OP_DT_Y, OP_RS_Y};
		for (int op = 0; op < 4; op++) {
			for (int r = 0; r < 9; r++) {
				const uint32_t inside = pixels[rows[r] * WIDTH + OP_X[op] + OP_RIGHT_W - 1] & 0xFFFFFF, past = pixels[rows[r] * WIDTH + OP_X[op] + OP_RIGHT_W] & 0xFFFFFF;
				if (!inside || past) {
					edges = false;
					printf("    column %d row %d: %06X then %06X\n", op + 1, r, (unsigned)inside, (unsigned)past);
				}
			}
		}
		CHECK(edges, "every row of every operator's column ends on the same right edge");
		// DT, RS and AM: a click is one gesture setting the segment's value; AM flips.
		const int am_before = get_value(CH4_OP1_AM);
		memset(pushed, 0, sizeof(pushed));
		click(window, OP_X[0] + 22 + 6, OP_DT_Y);
		run_ms(50);
		const int dt_gestures = pushed[0];
		click(window, OP_X[0] + 22 + 3 * 13 + 6, OP_RS_Y);
		run_ms(50);
		click(window, OP_X[0] + 90, OP_RS_Y);
		run_ms(100);
		CHECK(dt_gestures == 1 && pushed[0] == 3 && pushed[2] == 3 && get_value(CH4_OP1_DT) == 7 && get_value(CH4_OP1_RS) == 3 && get_value(CH4_OP1_AM) == !am_before,
			"a click on DT's first segment sets -3 (7), on RS's last 3, on AM flips it, one gesture each");
		// SSG of operator 1: the button in its header opens the choice under it, a shape sets it, the
		// button shows it.
		memset(pushed, 0, sizeof(pushed));
		click(window, SSG_BUTTON_X, SSG_BUTTON_Y);
		run_ms(100);
		capture(window);
		const int choice_open = count_color_in(OP_X[0] - 1, OP_X[0] + OP_RIGHT_W + 1, 226, 300, COLOR_LABEL);
		click(window, SSG_PICK_8_X, SSG_PICK_Y);
		run_ms(100);
		capture(window);
		save_bmp(argv[2], "fm-ssg");
		CHECK(choice_open > 40 && pushed[0] == 1 && pushed[2] == 1 && pushed_id[0] == CH4_OP1_SSG && get_value(CH4_OP1_SSG) == 8,
			"the SSG button opens the choice of shapes (%d px), a click on the first sets operator 1's SSG to 8 (%d, %d gestures)", choice_open, get_value(CH4_OP1_SSG), pushed[0]);
		CHECK(count_color_in(SSG_BUTTON_X - 16, SSG_BUTTON_X + 16, SSG_BUTTON_Y - 6, SSG_BUTTON_Y + 6, COLOR_MARK) > 10, "the SSG button draws the shape, amber against the preset's OFF");
		click(window, SSG_BUTTON_X, SSG_BUTTON_Y);
		run_ms(50);
		memset(pushed, 0, sizeof(pushed));
		click(window, GRAPH_LEFT + 20, GRAPH_BOTTOM - 20);
		run_ms(50);
		click(window, SSG_BUTTON_X, SSG_BUTTON_Y);
		run_ms(50);
		click(window, SSG_PICK_OFF_X, SSG_PICK_Y);
		run_ms(100);
		CHECK(pushed[0] == 1 && get_value(CH4_OP1_SSG) == 0, "a click beside the choice only closes it; OFF turns SSG off");
		// A double click on a shape: the second press lands on TL under the closed choice and must
		// leave it alone (review after 0.11.0).
		const int tl_before = get_value(CH4_OP1_TL);
		click(window, SSG_BUTTON_X, SSG_BUTTON_Y);
		run_ms(50);
		memset(pushed, 0, sizeof(pushed));
		click(window, SSG_PICK_8_X, SSG_PICK_Y);
		SendMessageA(window, WM_LBUTTONDBLCLK, MK_LBUTTON, MAKELPARAM(SSG_PICK_8_X, SSG_PICK_Y));
		SendMessageA(window, WM_LBUTTONUP, 0, MAKELPARAM(SSG_PICK_8_X, SSG_PICK_Y));
		run_ms(100);
		CHECK(pushed[0] == 1 && get_value(CH4_OP1_SSG) == 8 && get_value(CH4_OP1_TL) == tl_before,
			"a double click on a shape sets SSG once and leaves TL under it alone (%d gestures, TL %d to %d)", pushed[0], tl_before, get_value(CH4_OP1_TL));
		click(window, SSG_BUTTON_X, SSG_BUTTON_Y);
		run_ms(50);
		click(window, SSG_PICK_OFF_X, SSG_PICK_Y);
		run_ms(50);
		SendMessageA(window, WM_LBUTTONDBLCLK, MK_LBUTTON, MAKELPARAM(OP_X[2] + 60, OP_AR_Y));
		run_ms(50);
		click(window, LFO_RATE_X, LFO_RATE_Y);
		run_ms(100);
		const int first = get_value(LFO_RATE);
		for (int i = 0; i < 6; i++) {
			click(window, LFO_RATE_X, LFO_RATE_Y);
			run_ms(50);
		}
		// A quick second click reaches the window as a double click.
		SendMessageA(window, WM_LBUTTONDBLCLK, MK_LBUTTON, MAKELPARAM(LFO_RATE_X, LFO_RATE_Y));
		run_ms(100);
		CHECK(first == 1 && get_value(LFO_RATE) == 0, "a click steps LFO Rate to the next value, eight clicks go round to the first");

		// Stage 20: the ARP switch puts channel 4's row in the graph's place; a click on the empty row
		// draws a step in light FM, telling the host nothing.
		click(window, ARP_SWITCH_X, ARP_SWITCH_Y);
		SendMessageA(window, WM_MOUSEMOVE, 0, MAKELPARAM(BUTTON_EDITOR_X, BUTTON_Y));
		run_ms(100);
		capture(window);
		const int fm_row_empty = count_color_in(GRAPH_LEFT, GRAPH_RIGHT, GRAPH_TOP, GRAPH_BOTTOM, COLOR_FM_DARK) + count_color_in(GRAPH_LEFT, GRAPH_RIGHT, GRAPH_TOP, GRAPH_BOTTOM, COLOR_FM_LIGHT);
		const int fm_hint = count_color_in(ARP_LX, ARP_LX + 200, ARP_FM_LY + 30, ARP_FM_LY + 38, COLOR_HINT);
		memset(pushed, 0, sizeof(pushed));
		click(window, ARP_LX + 20, ARP_Y(ARP_FM_LY, 12));
		SendMessageA(window, WM_MOUSEMOVE, 0, MAKELPARAM(BUTTON_EDITOR_X, BUTTON_Y));
		run_ms(100);
		capture(window);
		save_bmp(argv[2], "fm-arp");
		const int fm_step = count_color_in(ARP_LX, ARP_LX + 49, ARP_Y(ARP_FM_LY, 12), ARP_Y(ARP_FM_LY, 12) + 2, COLOR_FM_LIGHT);
		CHECK(fm_row_empty == 0 && fm_hint > 50 && fm_step == 98 && pushed[0] + pushed[1] + pushed[2] == 0,
			"ARP shows channel 4's empty row instead of the graph (%d px of hint), a click draws +12 in light FM (%d px), the host told nothing", fm_hint, fm_step);
		// Channel 3: its row is dim in Ch3 Special and takes no mouse; it comes back as it was.
		click(window, RAIL_X, RAIL_CH3_TOP + 20);
		click(window, ARP_LX + 20, ARP_Y(ARP_FM_LY, 12));
		click(window, CH3_SPECIAL_X, CH3_SPECIAL_Y);
		run_ms(300);
		click(window, ARP_LX + 20, ARP_Y(ARP_FM_LY, 24));
		SendMessageA(window, WM_MOUSEMOVE, 0, MAKELPARAM(BUTTON_EDITOR_X, BUTTON_Y));
		run_ms(100);
		capture(window);
		save_bmp(argv[2], "fm-arp-dim");
		const int special_light = count_color_in(GRAPH_LEFT, GRAPH_RIGHT, GRAPH_TOP, GRAPH_BOTTOM, COLOR_FM_LIGHT);
		const int special_dim = count_color_in(ARP_LX, ARP_LX + 49, ARP_Y(ARP_FM_LY, 12), ARP_Y(ARP_FM_LY, 12) + 2, COLOR_LINE);
		click(window, CH3_SPECIAL_X, CH3_SPECIAL_Y);
		run_ms(300);
		capture(window);
		CHECK(special_light == 0 && special_dim == 98 && count_color_in(ARP_LX, ARP_LX + 49, ARP_Y(ARP_FM_LY, 12), ARP_Y(ARP_FM_LY, 12) + 2, COLOR_FM_LIGHT) == 98
			&& count_color_in(ARP_LX, ARP_LX + 49, ARP_Y(ARP_FM_LY, 24), ARP_Y(ARP_FM_LY, 24) + 2, COLOR_FM_LIGHT) == 0,
			"in Ch3 Special channel 3's row is dim (%d px light) and a click on it draws nothing; after it the row is back", special_light);
		// Stage 21: channel 3 by operators. The arp row is dim in Ch3 Special only with Ops.
		const int row_lit = count_color_in(ARP_LX, ARP_LX + 49, ARP_Y(ARP_FM_LY, 12), ARP_Y(ARP_FM_LY, 12) + 2, COLOR_FM_LIGHT);
		const int notes_dim_lit = count_color_in(376, 486, 480, 498, COLOR_FM);
		click(window, CH3_NOTES_ONE_X, CH3_NOTES_Y);
		click(window, TAG2_X, TAG_Y);
		SendMessageA(window, WM_MOUSEMOVE, 0, MAKELPARAM(BUTTON_EDITOR_X, BUTTON_Y));
		run_ms(100);
		capture(window);
		save_bmp(argv[2], "ch3-off");
		// Stage 22b: no tag at all with Ch3 Special off.
		const int tag_dim_text = count_color_in(TAG2_LEFT, TAG2_LEFT + 48, TAG_Y - 6, TAG_Y + 6, COLOR_LINE) + count_color_in(TAG2_LEFT, TAG2_LEFT + 48, TAG_Y - 6, TAG_Y + 6, COLOR_HINT);
		CHECK(get_value(CH3_NOTES) == 0 && notes_dim_lit == 0 && pitch_frame() == 0 && tag_dim_text == 0,
			"Ch3 Special off: CH3 NOTES is dim (%d px lit) and takes no click, no pitch tag (%d px) and the click opens nothing", notes_dim_lit, tag_dim_text);
		click(window, CH3_SPECIAL_X, CH3_SPECIAL_Y);
		run_ms(300);
		SendMessageA(window, WM_MOUSEMOVE, 0, MAKELPARAM(BUTTON_EDITOR_X, BUTTON_Y));
		run_ms(100);
		capture(window);
		const int ops_dim_row = count_color_in(ARP_LX, ARP_LX + 49, ARP_Y(ARP_FM_LY, 12), ARP_Y(ARP_FM_LY, 12) + 2, COLOR_SP);
		const int ops_lit = count_color_in(376, 410, 480, 498, COLOR_SP), special_on = count_color_in(298, 370, 480, 498, COLOR_SP);
		click(window, CH3_NOTES_ONE_X, CH3_NOTES_Y);
		run_ms(100);
		SendMessageA(window, WM_MOUSEMOVE, 0, MAKELPARAM(BUTTON_EDITOR_X, BUTTON_Y));
		run_ms(100);
		capture(window);
		save_bmp(argv[2], "ch3-one-note");
		const int one_row = count_color_in(ARP_LX, ARP_LX + 49, ARP_Y(ARP_FM_LY, 12), ARP_Y(ARP_FM_LY, 12) + 2, COLOR_SP);
		const int one_lit = count_color_in(412, 450, 480, 498, COLOR_SP);
		CHECK(special_on > 0 && ops_lit > 300 && get_value(CH3_NOTES) == 1 && one_lit > 300,
			"CH3 SPECIAL On in light FM (%d px), OPS lit (%d px), a click on 1 NOTE sets One Note (%d px)", special_on, ops_lit, one_lit);
		CHECK(row_lit == 98 && ops_dim_row == 0 && one_row == 98, "channel 3's arp row is dim with Ops (%d px light) and back with One Note (%d px)", ops_dim_row, one_row);
		// The rail's column of the operators' notes only with Ops.
		const int ops_column_x = 204, ops_top = RAIL_CH3_TOP + 2;
		send_midi(0x92, 60, 100);
		send_midi(0x9A, 64, 100);
		run_ms(200);
		capture(window);
		const int one_column = count_color_in(ops_column_x, 226, ops_top + 9, ops_top + 17, COLOR_HINT);
		double_click(window, CH3_NOTES_ONE_X, CH3_NOTES_Y);
		run_ms(200);
		capture(window);
		// Stage 24c: the switch released the ROM's keys, so operator 1's note shows let go, grey.
		const int ops_column = count_color_in(ops_column_x, 226, ops_top + 9, ops_top + 17, COLOR_HINT);
		send_midi(0x82, 60, 0);
		send_midi(0x8A, 64, 0);
		CHECK(get_value(CH3_NOTES) == 0 && one_column == 0 && ops_column > 0,
			"a double click on CH3 NOTES sets Ops; the rail shows operator 1's note with Ops (%d px), not with One Note (%d px)", ops_column, one_column);
		// Stage 22: the third segment sets CSM, lit alone; a double click goes back to Ops.
		click(window, CH3_NOTES_CSM_X, CH3_NOTES_Y);
		run_ms(100);
		capture(window);
		save_bmp(argv[2], "ch3-csm");
		const int csm_lit = count_color_in(452, 486, 480, 498, COLOR_SP), csm_others = count_color_in(376, 450, 480, 498, COLOR_SP);
		const int csm_value = get_value(CH3_NOTES);
		// Stage 22b (D57): operator 1's AR (row at y 281) has no FM bar in CSM, and has one again after.
		const int ar_in_csm = count_color_in(OP1_TL_X - 10, OP1_TL_X + 30, 282, 290, COLOR_SP_DARK);
		double_click(window, CH3_NOTES_CSM_X, CH3_NOTES_Y);
		run_ms(100);
		capture(window);
		const int ar_out = count_color_in(OP1_TL_X - 10, OP1_TL_X + 30, 282, 290, COLOR_SP_DARK);
		CHECK(csm_value == 2 && csm_lit > 300 && csm_others == 0 && get_value(CH3_NOTES) == 0,
			"a click on CSM sets CSM (%d), lit alone (%d px, the others %d), a double click Ops again", csm_value, csm_lit, csm_others);
		CHECK(ar_in_csm == 0 && ar_out > 0, "channel 3's AR is dim in CSM (%d px of FM bar) and lit after (%d px)", ar_in_csm, ar_out);

		// The pitch tag opens SEMI, FINE and FIX under its header; they exist only while it is open.
		const int tl_op2 = get_value(CH3_OP2_TL), fine_before = get_value(CH3_OP2_FINE);
		drag(window, PITCH_SEMI_X, PITCH_SEMI_Y, PITCH_SEMI_Y - 20);
		run_ms(50);
		const int closed_semi = get_value(CH3_OP2_SEMI), closed_tl = get_value(CH3_OP2_TL);
		double_click(window, PITCH_SEMI_X, PITCH_SEMI_Y);
		run_ms(50);
		CHECK(closed_semi == 48 && closed_tl != tl_op2 && get_value(CH3_OP2_TL) == tl_op2,
			"with the editor closed a drag there moves operator 2's TL (%d to %d), not SEMI", tl_op2, closed_tl);
		click(window, TAG2_X, TAG_Y);
		run_ms(50);
		capture(window);
		const int frame_open = pitch_frame();
		memset(pushed, 0, sizeof(pushed));
		drag(window, PITCH_SEMI_X, PITCH_SEMI_Y, PITCH_SEMI_Y - 60);
		drag(window, PITCH_SEMI_X, PITCH_FINE_Y, PITCH_FINE_Y + 30);
		click(window, PITCH_FIX_X, PITCH_FIX_Y);
		SendMessageA(window, WM_MOUSEMOVE, 0, MAKELPARAM(PITCH_SEMI_X, PITCH_SEMI_Y));
		run_ms(100);
		capture(window);
		save_bmp(argv[2], "ch3-pitch");
		const int semi = get_value(CH3_OP2_SEMI), fine = get_value(CH3_OP2_FINE), fixed = get_value(CH3_OP2_FIXED);
		const int tag_fixed = count_color_in(TAG2_LEFT, TAG2_LEFT + 48, TAG_Y - 6, TAG_Y + 6, COLOR_SP);
		// Stage 22a: 6 px a semitone, 3 px a cent.
		CHECK(frame_open == 114 && semi == 58 && fine == fine_before - 10 && fixed == 1 && pushed[0] == 3 && pushed_id[0] == CH3_OP2_FIXED,
			"a click on operator 2's tag opens its editor (%d px); 60 px drag SEMI to %+d, 30 px FINE to %+d, FIX turns on, %d gestures", frame_open, semi - 48, fine - 63, pushed[0]);
		CHECK(tag_fixed > 300, "with FIX the tag is filled light FM (%d px)", tag_fixed);
		// Stage 22a: a notch steps a cell by one, a gesture each; two half notches make one.
		memset(pushed, 0, sizeof(pushed));
		wheel(window, PITCH_SEMI_X, PITCH_SEMI_Y, 120);
		wheel(window, PITCH_SEMI_X, PITCH_FINE_Y, -60);
		const int half_fine = get_value(CH3_OP2_FINE);
		wheel(window, PITCH_SEMI_X, PITCH_FINE_Y, -60);
		run_ms(50);
		CHECK(get_value(CH3_OP2_SEMI) == semi + 1 && half_fine == fine && get_value(CH3_OP2_FINE) == fine - 1 && pushed[0] == 2 && pushed[2] == 2,
			"a notch up moves SEMI to %+d, two half notches down FINE to %+d, %d gestures", get_value(CH3_OP2_SEMI) - 48, get_value(CH3_OP2_FINE) - 63, pushed[0]);
		wheel(window, PITCH_SEMI_X, PITCH_SEMI_Y, 120 * 60);
		run_ms(50);
		CHECK(get_value(CH3_OP2_SEMI) == 96, "60 notches at once stop SEMI at +48 (%+d)", get_value(CH3_OP2_SEMI) - 48);
		double_click(window, PITCH_SEMI_X, PITCH_SEMI_Y);
		double_click(window, PITCH_SEMI_X, PITCH_FINE_Y);
		double_click(window, PITCH_FIX_X, PITCH_FIX_Y);
		run_ms(100);
		CHECK(get_value(CH3_OP2_SEMI) == 48 && get_value(CH3_OP2_FINE) == 63 && get_value(CH3_OP2_FIXED) == 0, "double clicks put SEMI, FINE and FIX back to 0, 0 and Off");
		click(window, GRAPH_LEFT + 100, GRAPH_BOTTOM + 40);
		run_ms(50);
		capture(window);
		CHECK(pitch_frame() == 0 && get_value(CH3_OP2_TL) == tl_op2, "a click elsewhere only closes the editor");
		click(window, RAIL_X, RAIL_CH1_TOP + 20);
		run_ms(50);
		click(window, TAG2_X, TAG_Y);
		run_ms(50);
		capture(window);
		save_bmp(argv[2], "ch3-ch1");
		const int ch1_frame = pitch_frame();
		drag(window, PITCH_SEMI_X, PITCH_SEMI_Y, PITCH_SEMI_Y - 20);
		CHECK(ch1_frame == 0 && get_value(CH3_OP2_SEMI) == 48, "channel 1's page has no tag and no editor: the same click opens nothing, the drag moves no SEMI");
		double_click(window, PITCH_SEMI_X, PITCH_SEMI_Y);
		click(window, RAIL_X, RAIL_CH3_TOP + 20);
		run_ms(50);

		// LFO RESET: a click steps it, a double click puts Off back; with the LFO off it is dim and takes no click.
		click(window, LFO_RESET_X, LFO_RATE_Y);
		run_ms(50);
		const int reset_one = get_value(LFO_RESET);
		double_click(window, LFO_RESET_X, LFO_RATE_Y);
		run_ms(50);
		const int reset_off = get_value(LFO_RESET);
		click(window, LFO_ON_X, LFO_RATE_Y);
		run_ms(50);
		click(window, LFO_RESET_X, LFO_RATE_Y);
		SendMessageA(window, WM_MOUSEMOVE, 0, MAKELPARAM(LFO_RESET_X, LFO_RATE_Y));
		run_ms(50);
		capture(window);
		save_bmp(argv[2], "ch3-lfo-off");
		const int reset_dim = get_value(LFO_RESET), dim_label = count_color_in(158, 222, 470, 477, COLOR_LINE);
		click(window, LFO_ON_X, LFO_RATE_Y);
		run_ms(50);
		CHECK(reset_one == 1 && reset_off == 0 && reset_dim == 0 && dim_label > 20 && get_value(2) == 1,
			"LFO RESET: a click is Ch 1, a double click Off; with the LFO off its label is dim (%d px) and a click does nothing", dim_label);
		click(window, CH3_SPECIAL_X, CH3_SPECIAL_Y);
		run_ms(300);
		CHECK(get_value(CH3_SPECIAL) == 0, "Ch3 Special off again");
		click(window, ACTION_UNDO_X, BUTTON_Y);
		click(window, RAIL_X, RAIL_CH4_Y);
		run_ms(100);

		// The overview (stage 10): VOL on a row is one gesture on that row's channel, PAN a click,
		// every FM row draws its algorithm and the sound opens the channel's page.
		click(window, BUTTON_ALL_X, BUTTON_Y);
		run_ms(100);
		memset(pushed, 0, sizeof(pushed));
		const int vol_before = get_value(CH2_VOLUME);
		drag(window, ALL_VOL_X, ALL_CH2_Y, ALL_CH2_Y + 20);
		run_ms(100);
		CHECK(pushed[0] == 1 && pushed[2] == 1 && pushed_id[0] == CH2_VOLUME && pushed_id[1] == CH2_VOLUME && get_value(CH2_VOLUME) == vol_before - 10,
			"VOL dragged on channel 2's row of ALL is one gesture on its Volume, %d to %d", vol_before, get_value(CH2_VOLUME));
		click(window, ALL_PAN_R_X, ALL_CH2_Y);
		run_ms(100);
		CHECK(get_value(CH2_PAN) == 2, "a click on R pans channel 2 right");
		// Master Volume (stage 18a): on ALL too, a drag of 10 px is 4 dB, a double click puts 0 dB back.
		memset(pushed, 0, sizeof(pushed));
		drag(window, MASTER_X, LFO_RATE_Y, LFO_RATE_Y + 10);
		run_ms(100);
		const int master_dragged = get_value(MASTER);
		SendMessageA(window, WM_LBUTTONDBLCLK, MK_LBUTTON, MAKELPARAM(MASTER_X, LFO_RATE_Y));
		SendMessageA(window, WM_LBUTTONUP, 0, MAKELPARAM(MASTER_X, LFO_RATE_Y));
		run_ms(100);
		CHECK(master_dragged == 560 && pushed_id[0] == MASTER && get_value(MASTER) == 600,
			"MASTER on ALL drags to %d (560 is -4 dB) and a double click sets it back to %d (600 is 0 dB)", master_dragged, get_value(MASTER));
		capture(window);
		CHECK(count_color_in(ALL_SOUND_X - 48, ALL_SOUND_RIGHT, RAIL_CH1_TOP, RAIL_CH1_TOP + 6 * ROW_H, COLOR_FM) > 6 * 50, "the FM rows draw their algorithms");
		// Stage 24d (from stage 10's parking): a double click on ALL's VOL sets the preset's value.
		{
			const int vol_before = get_value(1201);
			drag(window, ALL_VOL_X, ALL_CH2_Y, ALL_CH2_Y + 40);
			run_ms(50);
			const int vol_dragged = get_value(1201);
			double_click(window, ALL_VOL_X, ALL_CH2_Y);
			SendMessageA(window, WM_LBUTTONUP, 0, MAKELPARAM(ALL_VOL_X, ALL_CH2_Y));
			run_ms(50);
			// Channel 2's GM preset has VOL 127; earlier steps left the knob elsewhere.
			CHECK(vol_dragged < vol_before && get_value(1201) == 127, "a double click on ALL's VOL of channel 2 sets its preset's %d (dragged to %d)", get_value(1201), vol_dragged);
		}
		CHECK(count_color_in(ALL_SOUND_X - 48, ALL_SOUND_RIGHT, RAIL_CH8_Y - 30, RAIL_CH8_Y + 10, COLOR_PSG) > 0, "the PSG rows draw their envelopes");
		// Stage 11: a click on an FM row's sound puts that row's envelopes in place of its diagram, a
		// second click brings the diagram back; other rows keep theirs.
		click(window, ALL_SOUND_X, ALL_CH2_Y);
		run_ms(100);
		capture(window);
		save_bmp(argv[2], "all-envelopes");
		// Stage 24f: a note on channel 2 puts the FM cursor on its envelopes, x = 242 + sqrt(s / 2) * 104.
		{
			const uint64_t on = frames_run;
			send_midi(0x91, 60, 100);
			run_ms(400);
			capture(window);
			const double s = (double)(frames_run - on) / RATE;
			const int want = 242 + (int)(sqrt(s / 2) * 104);
			const int line = count_color_in(want - 1, want + 2, RAIL_CH1_TOP + ROW_H + 2, RAIL_CH1_TOP + ROW_H + 37, COLOR_PLAYING);
			send_midi(0x81, 60, 0);
			run_ms(100);
			CHECK(line >= 30, "ALL: channel 2's envelopes have the FM cursor at x %d after %.2f s (%d px)", want, s, line);
		}
		const int row2_boxes = count_color_in(ALL_SOUND_X - 48, ALL_SOUND_RIGHT, RAIL_CH1_TOP + ROW_H, RAIL_CH1_TOP + 2 * ROW_H, COLOR_FM);
		const int row2_curves = count_color_in(ALL_SOUND_X - 48, ALL_SOUND_RIGHT, RAIL_CH1_TOP + ROW_H, RAIL_CH1_TOP + 2 * ROW_H, COLOR_FM_LIGHT);
		const int row1_boxes = count_color_in(ALL_SOUND_X - 48, ALL_SOUND_RIGHT, RAIL_CH1_TOP, RAIL_CH1_TOP + ROW_H, COLOR_FM);
		click(window, ALL_SOUND_X, ALL_CH2_Y);
		run_ms(100);
		capture(window);
		CHECK(row2_boxes == 0 && row2_curves > 50 && row1_boxes > 50 && count_color_in(ALL_SOUND_X - 48, ALL_SOUND_RIGHT, RAIL_CH1_TOP + ROW_H, RAIL_CH1_TOP + 2 * ROW_H, COLOR_FM) > 50,
			"a click on channel 2's diagram shows its envelopes (%d px of carriers), channel 1 keeps its diagram, a second click brings it back", row2_curves);
		// A held note on channel 8 puts the envelope's cursor on its row.
		send_midi(0x97, 72, 100);
		run_ms(300);
		capture(window);
		CHECK(count_color_in(ALL_SOUND_X - 48, ALL_SOUND_RIGHT, RAIL_CH8_Y - 10, RAIL_CH8_Y + 30, COLOR_PLAYING) > 20, "a held note on channel 8 draws the envelope's cursor");
		// Stage 24e (D60): the OUT box shows channel 8's output, 2 periods of its square, standing still.
		{
			int top = -1;
			for (int y = RAIL_CH8_Y - 30; y < RAIL_CH8_Y + 30 && top < 0; y++) {
				top = count_color_in(533, 611, y, y + 1, COLOR_PSG) > 0 ? y : -1;
			}
			save_bmp(argv[2], "all-out");
			// Stage 35 (D80, architect): 7 frames over about 1 s, each 2 crests (runs of 4 px or more;
			// slivers at the right edge don't count) starting on the rising edge; the first crest moves by
			// 1 px at most, the edge's fraction of a sample (2.4 samples a pixel).
			int crests_low = 99, crests_high = 0, first_low = 9999, first_high = -1;
			for (int k = 0; k < 7; k++) {
				if (k) {
					run_ms(150);
					capture(window);
				}
				int crests = 0, first = -1;
				for (int x = 533; top >= 0 && x < 611; x++) {
					if ((pixels[top * WIDTH + x] & 0xFFFFFF) == COLOR_PSG && (pixels[top * WIDTH + x - 1] & 0xFFFFFF) != COLOR_PSG) {
						int end = x;
						while (end < 611 && (pixels[top * WIDTH + end] & 0xFFFFFF) == COLOR_PSG) {
							end++;
						}
						if (end - x >= 4) {
							first = first < 0 ? x : first;
							crests++;
						}
					}
				}
				crests_low = crests < crests_low ? crests : crests_low;
				crests_high = crests > crests_high ? crests : crests_high;
				first_low = first >= 0 && first < first_low ? first : first_low;
				first_high = first > first_high ? first : first_high;
			}
			CHECK(crests_low == 2 && crests_high == 2 && first_high >= 0 && first_high - 533 <= 1 && first_high - first_low <= 1,
				"channel 8's OUT box draws 2 periods from a rising edge in 7 frames over 1 s (crests %d..%d, first crest x %d..%d)", crests_low, crests_high, first_low, first_high);
		}
		// A chord: the picture holds by its lowest note, C3 under C4. Each column's top in two frames 120 ms
		// apart; the shift that matches them best is under 2 px (the sound decays, so tops move a little).
		{
			send_midi(0x90, 48, 110);
			send_midi(0x90, 60, 110);
			run_ms(300);
			int tops[2][78];
			for (int k = 0; k < 2; k++) {
				capture(window);
				for (int x = 0; x < 78; x++) {
					tops[k][x] = -1;
					for (int y = RAIL_CH1_TOP + 5; y < RAIL_CH1_TOP + 34 && tops[k][x] < 0; y++) {
						tops[k][x] = (pixels[y * WIDTH + 533 + x] & 0xFFFFFF) == COLOR_FM_LIGHT ? y : -1;
					}
				}
				run_ms(120);
			}
			send_midi(0x80, 48, 0);
			send_midi(0x80, 60, 0);
			run_ms(100);
			int best_shift = 99, best = 1 << 30, drawn = 0;
			for (int x = 0; x < 78; x++) {
				drawn += tops[0][x] >= 0;
			}
			for (int d = -20; d <= 20; d++) {
				int cost = 0, n = 0;
				for (int x = 20; x < 58; x++) {
					if (tops[0][x] >= 0 && tops[1][x + d] >= 0) {
						cost += abs(tops[0][x] - tops[1][x + d]);
						n++;
					}
				}
				cost = n ? cost * 38 / n : 1 << 30;
				if (cost < best) {
					best = cost;
					best_shift = d;
				}
			}
			CHECK(drawn > 60 && abs(best_shift) < 2, "a chord's OUT box holds by its lowest note: %d columns drawn, 120 ms later it matches best shifted by %d px", drawn, best_shift);
		}
		save_bmp(argv[2], "all");
		send_midi(0x87, 72, 0);
		run_ms(200);
		capture(window);
		CHECK(count_color_in(533, 611, RAIL_CH8_Y - 30, RAIL_CH8_Y + 30, COLOR_PSG) == 0, "without a held note the OUT box is empty");
		send_midi(0x97, 72, 100);
		run_ms(100);
		click(window, ALL_SOUND_X, RAIL_CH8_Y);
		run_ms(100);
		capture(window);
		CHECK(count_color_in(ENV_X, ENV_RIGHT, ENV_LEVEL_Y, ENV_LEVEL_BOTTOM, COLOR_PSG) > 0, "a click on channel 8's sound opens its envelope page");
		CHECK(count_color_in(ENV_X, ENV_RIGHT, ENV_LEVEL_Y, ENV_LEVEL_BOTTOM, COLOR_PLAYING) > 50, "the held note's cursor runs on the envelope editor too");
		send_midi(0x87, 72, 0);
		click(window, RAIL_X, RAIL_CH4_Y);
		run_ms(100);

		// The bank: a click on a place sets the selected channel's Preset, as one gesture.
		click(window, BUTTON_BANK_X, BUTTON_Y);
		run_ms(100);
		memset(pushed, 0, sizeof(pushed));
		click(window, BANK_ROW_X, BANK_ROW3_Y);
		run_ms(100);
		CHECK(pushed[0] == 1 && pushed[2] == 1 && pushed_id[0] == CH4_PRESET && get_value(CH4_PRESET) == 2, "a click on place 3 of the bank sets channel 4's Preset to it");
		capture(window);
		CHECK(count_color_in(BANK_ROW_X - 150, BANK_ROW_X + 290, BANK_ROW3_Y - 10, BANK_ROW3_Y + 9, COLOR_FM_DARK) > 1000, "and lights its row");
		CHECK(count_color_in(SCROLL_X, SCROLL_X + 6, SCROLL_TOP, SCROLL_BOTTOM, COLOR_FM_LIGHT) > 0, "the other channels' places are marked on the scroll track");
		save_bmp(argv[2], "bank");
		// Stage 22a: the wheel over the list still scrolls it, no knob moves: place 3's row goes up and out.
		{
			memset(pushed, 0, sizeof(pushed));
			wheel(window, BANK_ROW_X, BANK_ROW3_Y, -120 * 2);
			run_ms(50);
			capture(window);
			const int lit_there = count_color_in(BANK_ROW_X - 150, BANK_ROW_X + 290, BANK_ROW3_Y - 10, BANK_ROW3_Y + 9, COLOR_FM_DARK);
			wheel(window, BANK_ROW_X, BANK_ROW3_Y, 120 * 2);
			run_ms(50);
			capture(window);
			CHECK(lit_there < 1000 && count_color_in(BANK_ROW_X - 150, BANK_ROW_X + 290, BANK_ROW3_Y - 10, BANK_ROW3_Y + 9, COLOR_FM_DARK) > 1000 && pushed[0] + pushed[1] + pushed[2] == 0,
				"the wheel over the bank list scrolls it (%d px lit after, back after up), no knob moves", lit_there);
		}
		// Arrows from the host's view (our clap-wrapper patch) step the place, each press one gesture,
		// also when presses come faster than the model takes them; past the first place nothing moves.
		typedef struct {
			bool (*key)(const clap_plugin_t *plugin, bool up);
		} gui_key_ext;
		const gui_key_ext *keys = plugin->get_extension(plugin, "mdmi.gui-key");
		CHECK(keys && keys->key(plugin, false) && keys->key(plugin, false), "the bank page takes arrow keys");
		run_ms(100);
		CHECK(get_value(CH4_PRESET) == 4, "two arrows down at once move channel 4 to place 5");
		for (int i = 0; i < 6; i++) {
			keys->key(plugin, true);
		}
		run_ms(100);
		CHECK(get_value(CH4_PRESET) == 0, "arrows up stop at place 1");

		// Stage 22c (D56): libraries beside the plugin, here the test's folder in MDMI_BANKS, not there yet.
		{
			const int b_x = 600, b_row1_y = 102, a_row5_y = 92 + 4 * 20;
			capture(window);
			CHECK(count_color_in(528, 592, 180, 204, COLOR_LINE) > 100 && count_color_in(528, 690, 100, 116, COLOR_LABEL) > 50, "no folder: list B says so and offers NEW");
			save_bmp(argv[2], "bank-no-folder");
			// The last message (4 s of the window's timer) goes first: it comes before hints.
			for (int tick = 0; tick < 5 * 31; tick++) {
				SendMessageA(window, WM_TIMER, 1, 0);
			}
			SendMessageA(window, WM_MOUSEMOVE, 0, MAKELPARAM(600, 300));
			capture(window);
			CHECK(count_color_in(0, WIDTH, FOOTER_TOP, HEIGHT, COLOR_LABEL) > 100 && count_color_in(528, 690, 80, 88, COLOR_HINT) == 0, "the footer over list B says where the folder is expected");
			click(window, 560, 190);
			run_ms(50);
			capture(window);
			CHECK(GetFileAttributesA(new_bank) != INVALID_FILE_ATTRIBUTES && count_color_in(528, 690, 80, 88, COLOR_HINT) > 30, "NEW makes the folder and an empty library, open in list B");
			static uint8_t empty[1 << 17], now[1 << 17];
			const size_t empty_size = read_file(new_bank, empty, sizeof(empty));
			// Place 3 of the ROM bank dragged into the library.
			drag_to(window, BANK_ROW_X, BANK_ROW3_Y, b_x, 200);
			run_ms(50);
			capture(window);
			size_t size = read_file(new_bank, now, sizeof(now));
			CHECK(size == empty_size && memcmp(now, empty, size) && count_color_in(528, 690, 95, 110, COLOR_TEXT) > 50, "a ROM place dragged into the library is written to its file and listed");
			save_bmp(argv[2], "bank-library");
			click(window, ACTION_UNDO_X, BUTTON_Y);
			run_ms(50);
			size = read_file(new_bank, now, sizeof(now));
			CHECK(size == empty_size && !memcmp(now, empty, size), "UNDO writes the place back");
			click(window, ACTION_UNDO_X, BUTTON_Y);
			run_ms(50);
			size = read_file(new_bank, now, sizeof(now));
			CHECK(size == empty_size && memcmp(now, empty, size), "and again brings it");
			// A press on the library's sound plays it on channel 4; the Preset stays.
			// Channel 4 on place 1 first, so the tried sound is not the place it already plays.
			click(window, BANK_ROW_X, 102);
			run_ms(100);
			const int preset = get_value(CH4_PRESET);
			click(window, b_x, b_row1_y);
			run_ms(100);
			capture(window);
			CHECK(get_value(CH4_PRESET) == preset && count_color_in(524, 698, 92, 111, COLOR_FM_DARK) > 1000, "a library's sound tried on channel 4 lights its row, Preset stays");
			save_bmp(argv[2], "bank-tried");
			// Into the ROM bank's place 5: its name turns from GM grey to white.
			const int gm = count_color_in(290, 446, a_row5_y + 3, a_row5_y + 17, COLOR_TEXT);
			drag_to(window, b_x, b_row1_y, BANK_ROW_X, a_row5_y + 10);
			run_ms(100);
			capture(window);
			CHECK(gm == 0 && count_color_in(290, 446, a_row5_y + 3, a_row5_y + 17, COLOR_TEXT) > 50, "the library's sound dragged into place 5 of the ROM bank");
			click(window, ACTION_UNDO_X, BUTTON_Y);
			run_ms(100);
			// CLEAR on the hovered row empties the library's place.
			SendMessageA(window, WM_MOUSEMOVE, 0, MAKELPARAM(b_x, b_row1_y));
			capture(window);
			CHECK(count_color_in(654, 694, 94, 109, COLOR_LINE) > 20, "the hovered library row shows CLEAR");
			click(window, 674, b_row1_y);
			run_ms(50);
			size = read_file(new_bank, now, sizeof(now));
			CHECK(size == empty_size && !memcmp(now, empty, size), "CLEAR empties the place in the file");
			capture(window);
			save_bmp(argv[2], "bank-cleared");
			// A library that can't be written: nothing changes, the footer says why.
			SetFileAttributesA(new_bank, FILE_ATTRIBUTE_READONLY);
			drag_to(window, BANK_ROW_X, BANK_ROW3_Y, b_x, 200);
			run_ms(50);
			capture(window);
			size = read_file(new_bank, now, sizeof(now));
			CHECK(size == empty_size && !memcmp(now, empty, size) && count_color_in(0, WIDTH, FOOTER_TOP, HEIGHT, COLOR_ERROR) > 50, "a read-only library is refused in the footer, unchanged");
			SetFileAttributesA(new_bank, FILE_ATTRIBUTE_NORMAL);
			// Stage 37: the folder's only library opened in list A leaves list B with none to show: it
			// says NO OTHER (frame bank-no-other) and offers NEW, as with none at all.
			click(window, 300, 66);
			run_ms(50);
			// A's choice lists the libraries first, then the ROM bank.
			click(window, 300, 92 + 10);
			run_ms(50);
			capture(window);
			save_bmp(argv[2], "bank-no-other");
			const int a_library = count_color_in(242, 500, 92, 112, COLOR_TEXT) + count_color_in(242, 500, 92, 112, COLOR_LABEL);
			CHECK(count_color_in(528, 592, 180, 204, COLOR_LINE) > 100 && count_color_in(528, 690, 100, 116, COLOR_LABEL) > 50 && a_library > 0,
				"the only library open in list A: list B shows no other and offers NEW");
			click(window, 300, 66);
			run_ms(50);
			click(window, 300, 92 + 20 + 10);
			run_ms(50);
			// The choice of list B: NEW, then the libraries by name; a broken file opens as CAN'T READ.
			char broken[MAX_PATH];
			snprintf(broken, sizeof(broken), "%s\\broken.mdbank", banks_dir);
			FILE *file = fopen(broken, "wb");
			fputs("not a bank", file);
			fclose(file);
			click(window, b_x, 66);
			run_ms(50);
			capture(window);
			CHECK(count_color_in(520, 708, 56, 76, COLOR_TEXT) > 1000, "a click on list B's bar opens the choice");
			save_bmp(argv[2], "bank-choice");
			click(window, 400, 92 + 20 + 10);
			run_ms(50);
			capture(window);
			CHECK(count_color_in(528, 690, 100, 116, COLOR_ERROR) > 50 && count_color_in(0, WIDTH, FOOTER_TOP, HEIGHT, COLOR_ERROR) > 50, "a broken file opens as CAN'T READ");
			click(window, b_x, 66);
			run_ms(50);
			click(window, 400, 92 + 40 + 10);
			run_ms(50);
			capture(window);
			CHECK(count_color_in(528, 690, 80, 88, COLOR_HINT) > 30, "and the library comes back by the choice");
			DeleteFileA(broken);
		}
		click(window, BUTTON_EDITOR_X, BUTTON_Y);
		run_ms(100);
		CHECK(!keys->key(plugin, true), "the editor page leaves arrows to the host");
		run_ms(100);

		// Stage 8: drawing channel 8's envelope. A fast drag across the level lane fills every
		// column it passed, tells the host nothing (D19), and UNDO puts the envelope back.
		click(window, RAIL_X, RAIL_CH8_Y);
		run_ms(100);
		// Stage 22a: the wheel on a PSG knob, channel 8's VOL: a notch up +1, far down stops at 0, back.
		{
			const uint32_t ch8_volume = 1801;
			const int vol_x = 242 + 50, vol_y = 56 + 10, /* VOL at x 242, the first row at y 56 */ was = get_value(ch8_volume);
			memset(pushed, 0, sizeof(pushed));
			wheel(window, vol_x, vol_y, -120);
			run_ms(50);
			const int down = get_value(ch8_volume);
			wheel(window, vol_x, vol_y, -120 * 200);
			run_ms(50);
			const int vol_floor = get_value(ch8_volume);
			wheel(window, vol_x, vol_y, 120 * was);
			run_ms(50);
			CHECK(was > 0 && down == was - 1 && vol_floor == 0 && get_value(ch8_volume) == was && pushed_id[0] == ch8_volume,
				"the wheel on channel 8's VOL: a notch down %d to %d, far down stops at %d, back to %d", was, down, vol_floor, get_value(ch8_volume));
		}
		// The length cell dragged far up: the longest envelope, 3 or 4 px a column (0.8.1).
		memset(pushed, 0, sizeof(pushed));
		drag(window, ENV_LENGTH_X, ENV_LENGTH_Y, ENV_LENGTH_Y - 300);
		run_ms(200);
		capture(window);
		const int bars_before = count_color_in(ENV_X, ENV_RIGHT, ENV_LEVEL_Y, ENV_LEVEL_BOTTOM, COLOR_PSG);
		SendMessageA(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(ENV_X + 1, ENV_LOUD_Y));
		for (int x = ENV_X + 60; x <= 705; x += 60) {
			SendMessageA(window, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(x, ENV_LOUD_Y));
		}
		SendMessageA(window, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(705, ENV_LOUD_Y));
		SendMessageA(window, WM_LBUTTONUP, 0, MAKELPARAM(705, ENV_LOUD_Y));
		run_ms(200);
		capture(window);
		save_bmp(argv[2], "envelope");
		int loud = 0, past = 0;
		// Loud columns counted as runs of the colour: 122 frames with the loop's marks, and each of the
		// loop's two lines joins a pair.
		for (int x = ENV_X; x < ENV_RIGHT; x++) {
			loud += (pixels[ENV_LOUD_Y * WIDTH + x] & 0xFFFFFF) == COLOR_PSG && (pixels[ENV_LOUD_Y * WIDTH + x - 1] & 0xFFFFFF) != COLOR_PSG;
		}
		past = count_color_in(ENV_RIGHT, 712, ENV_LEVEL_Y, ENV_LEVEL_BOTTOM, COLOR_PSG);
		// A bar, then a gap, and the last frame at the lane's end, the same at every length.
		const uint32_t gap = pixels[ENV_LOUD_Y * WIDTH + ENV_X + 2] & 0xFFFFFF;
		const uint32_t last = pixels[ENV_LOUD_Y * WIDTH + ENV_RIGHT - 2] & 0xFFFFFF;
		CHECK(loud >= 120 && past == 0 && gap != COLOR_PSG && last == COLOR_PSG, "a fast drag to the right edge draws every column up to the limit (%d runs of 120) and the lane's end, nothing past it", loud);
		CHECK(pushed[0] + pushed[1] + pushed[2] == 0, "drawing tells the host nothing");
		click(window, ACTION_UNDO_X, BUTTON_Y);
		run_ms(200);
		capture(window);
		CHECK(count_color_in(ENV_X, ENV_RIGHT, ENV_LEVEL_Y, ENV_LEVEL_BOTTOM, COLOR_PSG) == bars_before, "UNDO puts back the envelope before the gesture");
		// A length gesture undoes as one.
		drag(window, ENV_LENGTH_X, ENV_LENGTH_Y, ENV_LENGTH_Y + 100);
		click(window, ACTION_UNDO_X, BUTTON_Y);
		run_ms(200);
		capture(window);
		CHECK(count_color_in(ENV_X, ENV_RIGHT, ENV_LEVEL_Y, ENV_LEVEL_BOTTOM, COLOR_PSG) == bars_before, "UNDO puts back the length before the gesture");
		// The loop's strip is its switch (4A): a click at its start turns the loop on or off, UNDO
		// puts it back, the host is told nothing.
		const int strip_before = count_color_in(ENV_X - 10, ENV_RIGHT + 10, ENV_LOOP_STRIP_Y - 6, ENV_LOOP_STRIP_Y + 8, COLOR_PSG);
		// With a loop, its body starts after the start tab and its line; without one, the switch is
		// at the strip's start.
		int body_x = ENV_X + 10;
		for (int x = ENV_X - 10; x < ENV_RIGHT; x++) {
			if ((pixels[ENV_LOOP_STRIP_Y * WIDTH + x] & 0xFFFFFF) == COLOR_PSG) {
				while ((pixels[ENV_LOOP_STRIP_Y * WIDTH + x] & 0xFFFFFF) == COLOR_PSG) {
					x++;
				}
				body_x = x + 1;
				break;
			}
		}
		memset(pushed, 0, sizeof(pushed));
		click(window, body_x, ENV_LOOP_STRIP_Y);
		run_ms(200);
		capture(window);
		save_bmp(argv[2], "envelope-loop");
		const int strip_after = count_color_in(ENV_X - 10, ENV_RIGHT + 10, ENV_LOOP_STRIP_Y - 6, ENV_LOOP_STRIP_Y + 8, COLOR_PSG);
		click(window, ACTION_UNDO_X, BUTTON_Y);
		run_ms(200);
		capture(window);
		CHECK(strip_after != strip_before && pushed[0] + pushed[1] + pushed[2] == 0
			&& count_color_in(ENV_X - 10, ENV_RIGHT + 10, ENV_LOOP_STRIP_Y - 6, ENV_LOOP_STRIP_Y + 8, COLOR_PSG) == strip_before,
			"a click on the loop's strip switches the loop (%d px of tabs, then %d), UNDO puts it back", strip_before, strip_after);
		// Off again: the loop's shadow stays where it was, and a click on it puts the loop back there,
		// not over the whole envelope (owner's note on 0.9.0).
		click(window, body_x, ENV_LOOP_STRIP_Y);
		run_ms(200);
		capture(window);
		const int shadow = count_color_in(ENV_X - 10, ENV_RIGHT + 10, ENV_LOOP_STRIP_Y - 6, ENV_LOOP_STRIP_Y + 8, COLOR_PSG);
		click(window, body_x, ENV_LOOP_STRIP_Y);
		run_ms(200);
		capture(window);
		save_bmp(argv[2], "envelope-loop-back");
		CHECK(shadow == strip_after && pushed[0] + pushed[1] + pushed[2] == 0
			&& count_color_in(ENV_X - 10, ENV_RIGHT + 10, ENV_LOOP_STRIP_Y - 6, ENV_LOOP_STRIP_Y + 8, COLOR_PSG) == strip_before,
			"a loop switched off and on comes back where it was (%d px of tabs, %d off, %d back)", strip_before, shadow,
			count_color_in(ENV_X - 10, ENV_RIGHT + 10, ENV_LOOP_STRIP_Y - 6, ENV_LOOP_STRIP_Y + 8, COLOR_PSG));
		// Stage 18b: the loop's body dragged moves the loop with its length; the host is told nothing.
		{
			const int first_before = first_of(ENV_LOOP_STRIP_Y, ENV_X - 10, ENV_RIGHT + 10, COLOR_PSG);
			const int to = first_before > ENV_X + 100 ? body_x - 80 : body_x + 80;
			memset(pushed, 0, sizeof(pushed));
			drag_x(window, body_x, ENV_LOOP_STRIP_Y, to);
			run_ms(200);
			capture(window);
			save_bmp(argv[2], "envelope-loop-moved");
			const int first_after = first_of(ENV_LOOP_STRIP_Y, ENV_X - 10, ENV_RIGHT + 10, COLOR_PSG);
			const int strip_moved = count_color_in(ENV_X - 10, ENV_RIGHT + 10, ENV_LOOP_STRIP_Y - 6, ENV_LOOP_STRIP_Y + 8, COLOR_PSG);
			CHECK(abs(first_after - first_before) > 40 && abs(strip_moved - strip_before) <= 14 && pushed[0] + pushed[1] + pushed[2] == 0,
				"a drag on the loop's body moves the loop (tab %d to %d), its length kept (%d px of tabs, %d before)", first_before, first_after, strip_moved,
				strip_before);
			click(window, ACTION_UNDO_X, BUTTON_Y);
			run_ms(200);
			capture(window);
			CHECK(first_of(ENV_LOOP_STRIP_Y, ENV_X - 10, ENV_RIGHT + 10, COLOR_PSG) == first_before, "UNDO puts the loop back");
		}
		// Stage 22b (D54): release or stop by a click on the loop's end tab or the strip after it.
		{
			// The end tab is the last of the strip's colour.
			const int strip_top = ENV_LOOP_STRIP_Y - 6, strip_bottom = ENV_LOOP_STRIP_Y + 8;
			int end_tab = -1;
			for (int x = ENV_RIGHT + 9; x >= ENV_X - 10 && end_tab < 0; x--) {
				end_tab = (pixels[ENV_LOOP_STRIP_Y * WIDTH + x] & 0xFFFFFF) == COLOR_PSG ? x - 4 : -1;
			}
			// Dragged to the lane's end: no release, a click on the tab changes nothing.
			drag_x(window, end_tab, ENV_LOOP_STRIP_Y, ENV_RIGHT + 20);
			run_ms(200);
			capture(window);
			int last = -1;
			for (int x = ENV_RIGHT + 9; x >= ENV_X - 10 && last < 0; x--) {
				last = (pixels[ENV_LOOP_STRIP_Y * WIDTH + x] & 0xFFFFFF) == COLOR_PSG ? x : -1;
			}
			const int strip_none = count_color_in(ENV_X - 10, ENV_RIGHT + 10, strip_top, strip_bottom, COLOR_PSG);
			const int lane_none = count_color_in(ENV_X, ENV_RIGHT, ENV_LEVEL_Y, ENV_LEVEL_BOTTOM, COLOR_PSG);
			click(window, last - 4, ENV_LOOP_STRIP_Y);
			// Away from the tab, which is lit under the mouse.
			SendMessageA(window, WM_MOUSEMOVE, 0, MAKELPARAM(100, 400));
			run_ms(200);
			capture(window);
			CHECK(count_color_in(ENV_X - 10, ENV_RIGHT + 10, strip_top, strip_bottom, COLOR_PSG) == strip_none && count_color_in(ENV_X, ENV_RIGHT, ENV_LEVEL_Y, ENV_LEVEL_BOTTOM, COLOR_PSG) == lane_none,
				"PSG, no release: a click on the end tab changes nothing");
			// 100 px back leaves a release, drawn loud so its bars show.
			drag_x(window, last - 4, ENV_LOOP_STRIP_Y, last - 104);
			run_ms(200);
			SendMessageA(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(last - 80, ENV_LOUD_Y));
			SendMessageA(window, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(ENV_RIGHT - 2, ENV_LOUD_Y));
			SendMessageA(window, WM_LBUTTONUP, 0, MAKELPARAM(ENV_RIGHT - 2, ENV_LOUD_Y));
			SendMessageA(window, WM_MOUSEMOVE, 0, MAKELPARAM(100, 400));
			run_ms(200);
			capture(window);
			last = -1;
			for (int x = ENV_RIGHT + 9; x >= ENV_X - 10 && last < 0; x--) {
				last = (pixels[ENV_LOOP_STRIP_Y * WIDTH + x] & 0xFFFFFF) == COLOR_PSG ? x : -1;
			}
			const int lit_release = count_color_in(last + 1, ENV_RIGHT, ENV_LEVEL_Y, ENV_LEVEL_BOTTOM, COLOR_PSG);
			memset(pushed, 0, sizeof(pushed));
			click(window, last - 4, ENV_LOOP_STRIP_Y);
			SendMessageA(window, WM_MOUSEMOVE, 0, MAKELPARAM(100, 400));
			run_ms(200);
			capture(window);
			save_bmp(argv[2], "envelope-stop");
			const int lit_stop = count_color_in(last + 1, ENV_RIGHT, ENV_LEVEL_Y, ENV_LEVEL_BOTTOM, COLOR_PSG);
			const int stop_frame = count_color_in(last + 1, ENV_RIGHT, ENV_LOOP_STRIP_Y - 6, ENV_LOOP_STRIP_Y - 5, COLOR_LINE);
			int last_stop = -1;
			for (int x = ENV_RIGHT + 9; x >= ENV_X - 10 && last_stop < 0; x--) {
				last_stop = (pixels[ENV_LOOP_STRIP_Y * WIDTH + x] & 0xFFFFFF) == COLOR_PSG ? x : -1;
			}
			CHECK(lit_release > 20 && lit_stop == 0 && stop_frame > 20 && last_stop == last && pushed[0] + pushed[1] + pushed[2] == 0,
				"PSG: a click on the end tab, stop: the release dims (%d px lit, was %d), outlined (%d px), the loop stays, the host told nothing", lit_stop, lit_release, stop_frame);
			click(window, last + 40, ENV_LOOP_STRIP_Y);
			SendMessageA(window, WM_MOUSEMOVE, 0, MAKELPARAM(100, 400));
			run_ms(200);
			capture(window);
			const int lit_back = count_color_in(last + 1, ENV_RIGHT, ENV_LEVEL_Y, ENV_LEVEL_BOTTOM, COLOR_PSG);
			CHECK(lit_back == lit_release, "PSG: a click on the strip after it, the release again (%d px)", lit_back);
			// Stop, the loop off and on again: it comes back with its release.
			click(window, last + 40, ENV_LOOP_STRIP_Y);
			run_ms(200);
			click(window, body_x, ENV_LOOP_STRIP_Y);
			run_ms(200);
			click(window, body_x, ENV_LOOP_STRIP_Y);
			SendMessageA(window, WM_MOUSEMOVE, 0, MAKELPARAM(100, 400));
			run_ms(200);
			capture(window);
			const int lit_again = count_color_in(last + 1, ENV_RIGHT, ENV_LEVEL_Y, ENV_LEVEL_BOTTOM, COLOR_PSG);
			CHECK(lit_again == lit_release, "PSG: stop, the loop off and on again: it comes back with its release (%d px)", lit_again);
		}
		// Stage 24d: a double click on a level column sets 0 dB (the top) in the first click's undo step.
		{
			SendMessageA(window, WM_MOUSEMOVE, 0, MAKELPARAM(100, 400));
			run_ms(100);
			capture(window);
			static uint32_t lane_before[(ENV_LEVEL_BOTTOM - ENV_LEVEL_Y) * (ENV_RIGHT - ENV_X)];
			for (int y = ENV_LEVEL_Y; y < ENV_LEVEL_BOTTOM; y++) {
				memcpy(lane_before + (y - ENV_LEVEL_Y) * (ENV_RIGHT - ENV_X), pixels + y * WIDTH + ENV_X, (ENV_RIGHT - ENV_X) * sizeof(uint32_t));
			}
			double_click(window, ENV_X + 3, ENV_LEVEL_Y + 70);
			SendMessageA(window, WM_LBUTTONUP, 0, MAKELPARAM(ENV_X + 3, ENV_LEVEL_Y + 70));
			SendMessageA(window, WM_MOUSEMOVE, 0, MAKELPARAM(100, 400));
			run_ms(200);
			capture(window);
			save_bmp(argv[2], "envelope-reset");
			const int top_lit = count_color_in(ENV_X, ENV_X + 4, ENV_LOUD_Y, ENV_LOUD_Y + 2, COLOR_PSG);
			click(window, ACTION_UNDO_X, BUTTON_Y);
			SendMessageA(window, WM_MOUSEMOVE, 0, MAKELPARAM(100, 400));
			run_ms(200);
			capture(window);
			int differ = 0;
			for (int y = ENV_LEVEL_Y; y < ENV_LEVEL_BOTTOM; y++) {
				differ += memcmp(lane_before + (y - ENV_LEVEL_Y) * (ENV_RIGHT - ENV_X), pixels + y * WIDTH + ENV_X, (ENV_RIGHT - ENV_X) * sizeof(uint32_t)) != 0;
			}
			CHECK(top_lit > 0 && differ == 0, "a double click on a level column sets it to 0 dB (%d px at the top), one UNDO gives back the column before the first press (%d rows differ)", top_lit, differ);
			// The same on a pitch column: a click high draws a shift, a double click there leaves none.
			const int pitch_top = ENV_LEVEL_BOTTOM + 6, pitch_bottom = pitch_top + 60;
			static uint32_t pitch_before[60 * (ENV_RIGHT - ENV_X)];
			for (int y = pitch_top; y < pitch_bottom; y++) {
				memcpy(pitch_before + (y - pitch_top) * (ENV_RIGHT - ENV_X), pixels + y * WIDTH + ENV_X, (ENV_RIGHT - ENV_X) * sizeof(uint32_t));
			}
			click(window, ENV_X + 3, pitch_top + 2);
			SendMessageA(window, WM_MOUSEMOVE, 0, MAKELPARAM(100, 400));
			run_ms(200);
			capture(window);
			const int shifted = count_color_in(ENV_X, ENV_X + 4, pitch_top, pitch_bottom, COLOR_PSG);
			click(window, ACTION_UNDO_X, BUTTON_Y);
			run_ms(100);
			double_click(window, ENV_X + 3, pitch_top + 2);
			SendMessageA(window, WM_LBUTTONUP, 0, MAKELPARAM(ENV_X + 3, pitch_top + 2));
			SendMessageA(window, WM_MOUSEMOVE, 0, MAKELPARAM(100, 400));
			run_ms(200);
			capture(window);
			const int reset = count_color_in(ENV_X, ENV_X + 4, pitch_top, pitch_bottom, COLOR_PSG);
			click(window, ACTION_UNDO_X, BUTTON_Y);
			SendMessageA(window, WM_MOUSEMOVE, 0, MAKELPARAM(100, 400));
			run_ms(200);
			capture(window);
			int pitch_differ = 0;
			for (int y = pitch_top; y < pitch_bottom; y++) {
				pitch_differ += memcmp(pitch_before + (y - pitch_top) * (ENV_RIGHT - ENV_X), pixels + y * WIDTH + ENV_X, (ENV_RIGHT - ENV_X) * sizeof(uint32_t)) != 0;
			}
			CHECK(shifted > reset && pitch_differ == 0, "a double click on a pitch column sets 0 (%d px of shift, %d after), one UNDO gives back the column (%d rows differ)", shifted, reset, pitch_differ);
			// The loop's strip: a double click switches the loop once, one UNDO switches it back.
			static uint32_t strip_before[16 * (ENV_RIGHT - ENV_X)];
			for (int y = 0; y < 16; y++) {
				memcpy(strip_before + y * (ENV_RIGHT - ENV_X), pixels + (ENV_LOOP_STRIP_Y - 6 + y) * WIDTH + ENV_X, (ENV_RIGHT - ENV_X) * sizeof(uint32_t));
			}
			double_click(window, ENV_X + 200, ENV_LOOP_STRIP_Y);
			SendMessageA(window, WM_LBUTTONUP, 0, MAKELPARAM(ENV_X + 200, ENV_LOOP_STRIP_Y));
			SendMessageA(window, WM_MOUSEMOVE, 0, MAKELPARAM(100, 400));
			run_ms(200);
			capture(window);
			int strip_changed = 0;
			for (int y = 0; y < 16; y++) {
				strip_changed += memcmp(strip_before + y * (ENV_RIGHT - ENV_X), pixels + (ENV_LOOP_STRIP_Y - 6 + y) * WIDTH + ENV_X, (ENV_RIGHT - ENV_X) * sizeof(uint32_t)) != 0;
			}
			click(window, ACTION_UNDO_X, BUTTON_Y);
			SendMessageA(window, WM_MOUSEMOVE, 0, MAKELPARAM(100, 400));
			run_ms(200);
			capture(window);
			int strip_back = 0;
			for (int y = 0; y < 16; y++) {
				strip_back += memcmp(strip_before + y * (ENV_RIGHT - ENV_X), pixels + (ENV_LOOP_STRIP_Y - 6 + y) * WIDTH + ENV_X, (ENV_RIGHT - ENV_X) * sizeof(uint32_t)) != 0;
			}
			CHECK(strip_changed > 0 && strip_back == 0, "a double click on the loop's strip switches it once (%d rows changed), one UNDO back (%d differ)", strip_changed, strip_back);
		}
		// Dragged far down: one frame, one column across the whole lane, drawn loud. Two frames
		// would draw 402 px.
		drag(window, ENV_LENGTH_X, ENV_LENGTH_Y, ENV_LENGTH_Y + 300);
		click(window, ENV_X + 100, ENV_LOUD_Y);
		run_ms(200);
		capture(window);
		save_bmp(argv[2], "envelope-short");
		const int wide = count_color_in(ENV_X, ENV_RIGHT, ENV_LOUD_Y, ENV_LOUD_Y + 1, COLOR_PSG);
		CHECK(wide >= 403 && pushed[0] + pushed[1] + pushed[2] == 0, "a length of one frame draws one column of %d px, the host told nothing", wide);

		// Stage 20, item 7: channel 8's arp row under the envelope. A click on the empty row makes four
		// steps of 0 looped from the first and draws the one under the mouse; a drag draws on.
		{
			const int ly = ARP_PSG_LY, strip = ARP_STRIP(ARP_PSG_LY);
			const int empty_hint = count_color_in(ARP_LX, PSG_COL(4), ly + 30, ly + 38, COLOR_HINT);
			// 0.22.0 divided by the zero steps here and took the host down: the mouse over the empty row.
			SendMessageA(window, WM_MOUSEMOVE, 0, MAKELPARAM(PSG_COL(1) + 20, ARP_Y(ly, 3)));
			run_ms(100);
			click(window, ARP_LX + 20, ARP_Y(ly, 12));
			SendMessageA(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(PSG_COL(1) + 20, ARP_Y(ly, 3)));
			SendMessageA(window, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(PSG_COL(2) + 20, ARP_Y(ly, 4)));
			SendMessageA(window, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(PSG_COL(3) + 20, ARP_Y(ly, 7)));
			SendMessageA(window, WM_LBUTTONUP, 0, MAKELPARAM(PSG_COL(3) + 20, ARP_Y(ly, 7)));
			SendMessageA(window, WM_MOUSEMOVE, 0, MAKELPARAM(BUTTON_EDITOR_X, BUTTON_Y));
			run_ms(200);
			capture(window);
			save_bmp(argv[2], "arp");
			const int tops = count_color_in(PSG_COL(0), PSG_COL(1) - 1, ARP_Y(ly, 12), ARP_Y(ly, 12) + 2, COLOR_PSG) + count_color_in(PSG_COL(1), PSG_COL(2) - 1, ARP_Y(ly, 3), ARP_Y(ly, 3) + 2, COLOR_PSG)
				+ count_color_in(PSG_COL(2), PSG_COL(3) - 1, ARP_Y(ly, 4), ARP_Y(ly, 4) + 2, COLOR_PSG) + count_color_in(PSG_COL(3), PSG_COL(4) - 1, ARP_Y(ly, 7), ARP_Y(ly, 7) + 2, COLOR_PSG);
			const int tab = count_color_in(ARP_LX - 9, ARP_LX - 1, strip - 6, strip + 6, COLOR_PSG);
			CHECK(empty_hint > 50 && tops == PSG_TOPS && tab == 8 * 12 && pushed[0] + pushed[1] + pushed[2] == 0,
				"a click on the empty row makes 4 looped steps, a drag draws +3 +4 +7 after the +12 (%d px of tops), the loop's tab at the start (%d px), the host told nothing", tops, tab);
			// The length up to the limit: 32 steps, the new ones copy the last, amber MAX; UNDO gives the 4 back.
			drag(window, ARP_PSG_LENGTH_X, ARP_LENGTH(ly), ARP_LENGTH(ly) - 200);
			SendMessageA(window, WM_MOUSEMOVE, 0, MAKELPARAM(BUTTON_EDITOR_X, BUTTON_Y));
			run_ms(200);
			capture(window);
			save_bmp(argv[2], "arp-long");
			const int sevens = runs_of(ARP_Y(ly, 7), ARP_LX, PSG_COL(4), COLOR_PSG);
			const int max = count_color_in(ARP_PSG_RX, ARP_PSG_RX + 52, ly + 1, ly + 32, COLOR_AMBER);
			click(window, ACTION_UNDO_X, BUTTON_Y);
			run_ms(200);
			capture(window);
			CHECK(sevens == 29 && max > 30 && runs_of(ARP_Y(ly, 7), ARP_LX, PSG_COL(4), COLOR_PSG) == 1,
				"the length dragged up stops at 32 steps (%d new ones at +7), MAX in amber (%d px); UNDO gives back 4", sevens, max);
			// The loop: a click on its body turns it off and leaves a shadow, a click on the shadow turns it
			// on there; a drag on the body moves its start; UNDO puts it back.
			click(window, ARP_LX + 130, strip);
			SendMessageA(window, WM_MOUSEMOVE, 0, MAKELPARAM(BUTTON_EDITOR_X, BUTTON_Y));
			run_ms(200);
			capture(window);
			save_bmp(argv[2], "arp-loop-off");
			const int off_tab = count_color_in(ARP_LX - 9, ARP_LX - 1, strip - 6, strip + 6, COLOR_PSG);
			const int shadow_line = count_color_in(ARP_LX, PSG_COL(4), strip - 6, strip - 5, COLOR_LINE);
			click(window, ARP_LX + 130, strip);
			SendMessageA(window, WM_MOUSEMOVE, 0, MAKELPARAM(BUTTON_EDITOR_X, BUTTON_Y));
			run_ms(200);
			capture(window);
			const int on_tab = count_color_in(ARP_LX - 9, ARP_LX - 1, strip - 6, strip + 6, COLOR_PSG);
			CHECK(off_tab == 0 && shadow_line > 190 && on_tab == 8 * 12, "a click on the loop turns it off, its shadow outlined (%d px), a click on that turns it on again", shadow_line);
			memset(pushed, 0, sizeof(pushed));
			drag_x(window, ARP_LX + 36, strip, ARP_LX + 36 + PSG_COL(2) - PSG_COL(0));
			SendMessageA(window, WM_MOUSEMOVE, 0, MAKELPARAM(BUTTON_EDITOR_X, BUTTON_Y));
			run_ms(200);
			capture(window);
			save_bmp(argv[2], "arp-loop-moved");
			const int moved_tab = count_color_in(PSG_COL(2) - 9, PSG_COL(2) - 1, strip - 6, strip + 6, COLOR_PSG);
			click(window, ACTION_UNDO_X, BUTTON_Y);
			run_ms(200);
			capture(window);
			CHECK(moved_tab == 8 * 12 && pushed[0] + pushed[1] + pushed[2] == 0 && count_color_in(ARP_LX - 9, ARP_LX - 1, strip - 6, strip + 6, COLOR_PSG) == 8 * 12,
				"a drag on the loop's body moves its start two steps on (%d px of tab there), the host told nothing; UNDO puts it back", moved_tab);
			// A double click on the +12: the first press draws +5, the second sets 0; one UNDO gives +12 back.
			click(window, ARP_LX + 20, ARP_Y(ly, 5));
			SendMessageA(window, WM_LBUTTONDBLCLK, MK_LBUTTON, MAKELPARAM(ARP_LX + 20, ARP_Y(ly, 5)));
			SendMessageA(window, WM_LBUTTONUP, 0, MAKELPARAM(ARP_LX + 20, ARP_Y(ly, 5)));
			SendMessageA(window, WM_MOUSEMOVE, 0, MAKELPARAM(BUTTON_EDITOR_X, BUTTON_Y));
			run_ms(200);
			capture(window);
			save_bmp(argv[2], "arp-zero");
			const int zero = count_color_in(PSG_COL(0), PSG_COL(1) - 1, ARP_Y(ly, 0), ARP_Y(ly, 0) + 2, COLOR_PSG);
			const int five = count_color_in(PSG_COL(0), PSG_COL(1) - 1, ARP_Y(ly, 5), ARP_Y(ly, 5) + 2, COLOR_PSG);
			click(window, ACTION_UNDO_X, BUTTON_Y);
			run_ms(200);
			capture(window);
			CHECK(zero == PSG_BAND && five == 0 && count_color_in(PSG_COL(0), PSG_COL(1) - 1, ARP_Y(ly, 12), ARP_Y(ly, 12) + 2, COLOR_PSG) == PSG_BAND,
				"a double click on a step sets it to 0, and one UNDO gives back the +12 from before the first press");
			// STORE: on with a row of the channel's own (its label grey), after it the channel is its place
			// as it is and STORE is off (its label dim).
			const int store_on = count_color_in(468, 532, 2, 26, COLOR_LABEL);
			click(window, ACTION_STORE_X, BUTTON_Y);
			SendMessageA(window, WM_MOUSEMOVE, 0, MAKELPARAM(BUTTON_EDITOR_X, BUTTON_Y));
			run_ms(200);
			capture(window);
			save_bmp(argv[2], "arp-store");
			const int store_off = count_color_in(468, 532, 2, 26, COLOR_LABEL);
			click(window, ACTION_UNDO_X, BUTTON_Y);
			run_ms(200);
			CHECK(store_on > 50 && store_off == 0 && count_color_in(468, 532, 2, 26, COLOR_LINE) > 50 && count_color_in(PSG_COL(0), PSG_COL(1) - 1, ARP_Y(ly, 12), ARP_Y(ly, 12) + 2, COLOR_PSG) == PSG_BAND,
				"STORE is on for channel 8's own row (%d px), off once the row is in its place, the row stays", store_on);
			// Stage 22b (D55): the row's switch at 470, 334, 52x12. ARP ON in PSG; a click turns the row
			// off, dim but kept (tops C_HINT, no PSG on the row), ARP OFF in grey; UNDO turns it on.
			const int switch_x = ARP_PSG_RX, switch_y = ly - 13;
			const int on_frame = count_color_in(switch_x, switch_x + 52, switch_y, switch_y + 12, COLOR_PSG);
			const int on_text = count_color_in(switch_x, switch_x + 52, switch_y, switch_y + 12, COLOR_TEXT);
			click(window, switch_x + 26, switch_y + 6);
			SendMessageA(window, WM_MOUSEMOVE, 0, MAKELPARAM(BUTTON_EDITOR_X, BUTTON_Y));
			run_ms(200);
			capture(window);
			save_bmp(argv[2], "arp-off");
			const int off_psg = count_color_in(242, 702, ly - 15, ly + 109, COLOR_PSG);
			const int off_tops = count_color_in(PSG_COL(0), PSG_COL(1) - 1, ARP_Y(ly, 12), ARP_Y(ly, 12) + 2, COLOR_HINT);
			const int off_label = count_color_in(switch_x, switch_x + 52, switch_y, switch_y + 12, COLOR_LABEL);
			const int dim_tab = count_color_in(ARP_LX - 9, ARP_LX - 1, strip - 6, strip + 6, COLOR_HINT);
			click(window, ACTION_UNDO_X, BUTTON_Y);
			run_ms(200);
			capture(window);
			const int undone = count_color_in(PSG_COL(0), PSG_COL(1) - 1, ARP_Y(ly, 12), ARP_Y(ly, 12) + 2, COLOR_PSG);
			CHECK(on_frame > 100 && on_text > 20 && off_psg == 0 && off_tops == PSG_BAND && off_label > 20 && dim_tab == 8 * 12 && undone == PSG_BAND,
				"ARP ON (%d px of frame); a click turns the row off: no PSG on it (%d px), its steps dim (%d px), ARP OFF; UNDO turns it on (%d px)", on_frame, off_psg, off_tops, undone);
			// Off, the row still draws: a click on the +12 column draws +5 in dim, and ON gives it its colour.
			click(window, switch_x + 26, switch_y + 6);
			click(window, ARP_LX + 20, ARP_Y(ly, 5));
			click(window, switch_x + 26, switch_y + 6);
			SendMessageA(window, WM_MOUSEMOVE, 0, MAKELPARAM(BUTTON_EDITOR_X, BUTTON_Y));
			run_ms(200);
			capture(window);
			const int drawn_on = count_color_in(PSG_COL(0), PSG_COL(1) - 1, ARP_Y(ly, 5), ARP_Y(ly, 5) + 2, COLOR_PSG);
			CHECK(drawn_on == PSG_BAND, "a row switched off is drawn on, and on again it plays what was drawn (%d px)", drawn_on);
			// Stage 24d: a double click on ARP ON turns the row off once, and one UNDO turns it on.
			double_click(window, switch_x + 26, switch_y + 6);
			SendMessageA(window, WM_LBUTTONUP, 0, MAKELPARAM(switch_x + 26, switch_y + 6));
			SendMessageA(window, WM_MOUSEMOVE, 0, MAKELPARAM(BUTTON_EDITOR_X, BUTTON_Y));
			run_ms(200);
			capture(window);
			const int twice_off = count_color_in(PSG_COL(0), PSG_COL(1) - 1, ARP_Y(ly, 5), ARP_Y(ly, 5) + 2, COLOR_HINT);
			click(window, ACTION_UNDO_X, BUTTON_Y);
			run_ms(200);
			capture(window);
			CHECK(twice_off == PSG_BAND && count_color_in(PSG_COL(0), PSG_COL(1) - 1, ARP_Y(ly, 5), ARP_Y(ly, 5) + 2, COLOR_PSG) == PSG_BAND,
				"a double click on ARP ON turns the row off once (%d px dim), one UNDO turns it on", twice_off);
			// An empty row has no switch: the length dragged to 0, ARP in grey, no frame; UNDO gives the steps back.
			drag(window, ARP_PSG_LENGTH_X, ARP_LENGTH(ly), ARP_LENGTH(ly) + 60);
			SendMessageA(window, WM_MOUSEMOVE, 0, MAKELPARAM(BUTTON_EDITOR_X, BUTTON_Y));
			run_ms(200);
			capture(window);
			const int empty_frame = count_color_in(switch_x, switch_x + 52, switch_y, switch_y + 12, COLOR_PSG) + count_color_in(switch_x, switch_x + 52, switch_y, switch_y + 12, COLOR_LINE);
			const int empty_label = count_color_in(switch_x, switch_x + 52, switch_y, switch_y + 12, COLOR_LABEL);
			click(window, ACTION_UNDO_X, BUTTON_Y);
			run_ms(200);
			CHECK(empty_frame == 0 && empty_label > 10, "an empty row has no switch, only ARP (%d px)", empty_label);
		}

		// Stage 16 (D45): channel 10's page has the noise's TYPE and RATE; TONE 3 takes channel 9.
		click(window, RAIL_X, RAIL_CH8_Y + 80);
		run_ms(100);
		click(window, 607, 92);
		run_ms(200);
		capture(window);
		save_bmp(argv[2], "noise");
		CHECK(get_value(8) == 3 && count_color_in(575, 639, 83, 101, COLOR_PSG) > 500, "TONE 3 on channel 10's page sets Noise Rate and lights in PSG's colour");
		click(window, RAIL_X, RAIL_CH8_Y + 40);
		run_ms(200);
		capture(window);
		save_bmp(argv[2], "noise-taken");
		CHECK(count_color_in(242, 600, 86, 98, COLOR_AMBER) > 100, "channel 9's page says in amber that the noise takes it");
		click(window, RAIL_X, RAIL_CH8_Y + 80);
		run_ms(100);
		click(window, 550, 92);
		run_ms(200);
		CHECK(get_value(8) == 0, "HIGH puts the rate back");
		// Stage 20: channel 10's row moves the noise only on Tone 3. Drawn there, at KIT it is dim and a
		// click on it draws nothing; back on Tone 3 it is as it was.
		click(window, 607, 92);
		run_ms(200);
		click(window, ARP_LX + 20, ARP_Y(ARP_PSG_LY, 12));
		click(window, 674, 92);
		run_ms(200);
		click(window, ARP_LX + 20, ARP_Y(ARP_PSG_LY, 24));
		SendMessageA(window, WM_MOUSEMOVE, 0, MAKELPARAM(BUTTON_EDITOR_X, BUTTON_Y));
		run_ms(200);
		capture(window);
		save_bmp(argv[2], "arp-kit");
		const int kit_psg = count_color_in(242, 702, ARP_PSG_LY - 15, ARP_PSG_LY + 109, COLOR_PSG);
		const int kit_dim = count_color_in(PSG_COL(0), PSG_COL(1) - 1, ARP_Y(ARP_PSG_LY, 12), ARP_Y(ARP_PSG_LY, 12) + 2, COLOR_LINE);
		click(window, 607, 92);
		run_ms(200);
		capture(window);
		CHECK(get_value(8) == 3 && kit_psg == 0 && kit_dim == PSG_BAND && count_color_in(PSG_COL(0), PSG_COL(1) - 1, ARP_Y(ARP_PSG_LY, 12), ARP_Y(ARP_PSG_LY, 12) + 2, COLOR_PSG) == PSG_BAND
			&& count_color_in(PSG_COL(0), PSG_COL(1) - 1, ARP_Y(ARP_PSG_LY, 24), ARP_Y(ARP_PSG_LY, 24) + 2, COLOR_PSG) == 0,
			"at KIT channel 10's row is dim (%d px of PSG) and takes no drawing; on Tone 3 it is back", kit_psg);
		click(window, ACTION_UNDO_X, BUTTON_Y);
		click(window, 550, 92);
		run_ms(200);

		// Stage 12: with the DAC on, EDITOR of channel 6 is the grid of notes, BANK the list. A project
		// with a sample restarts the engine, and CLEAR gives the note back to the ROM.
		send_midi(0xB0, 78, 127);
		run_ms(800);
		click(window, RAIL_X, RAIL_CH6_Y);
		click(window, BUTTON_EDITOR_X, BUTTON_Y);
		run_ms(200);
		capture(window);
		save_bmp(argv[2], "dac");
		const int mapped = count_color_in(242, 700, 96, 294, COLOR_DAC_DARK) + count_color_in(242, 700, 96, 294, COLOR_SHARP_ROM);
		CHECK(mapped > 61 * 450 && count_color_in(242, 700, 96, 294, COLOR_DAC) < 200, "the grid has the ROM's 61 notes (%d px), no file yet", mapped);
		// Stage 24c (D59): two DAC notes held, two green lines of the column on channel 6's row.
		send_midi(0x95, 36, 100);
		send_midi(0x95, 38, 100);
		run_ms(60);
		capture(window);
		const int dac_two = count_color_in(200, 227, 244 + 2, 244 + 18, COLOR_PLAYING), dac_rest = count_color_in(200, 227, 244 + 20, 244 + 37, COLOR_PLAYING);
		save_bmp(argv[2], "dac-voices");
		send_midi(0x85, 36, 0);
		send_midi(0x85, 38, 0);
		run_ms(100);
		CHECK(dac_two > 20 && dac_rest == 0, "two held DAC notes fill two lines of the column (%d px, %d below)", dac_two, dac_rest);
		// Stage 24e: on ALL the DAC's row has VOL and PAN, the last sample and a green line where its voice is.
		click(window, BUTTON_ALL_X, BUTTON_Y);
		send_midi(0xB5, 10, 0);
		send_midi(0x95, 38, 100);
		run_ms(60);
		capture(window);
		save_bmp(argv[2], "all-dac");
		const int dac_sample_px = count_color_in(242, 346, 244 + 3, 244 + 36, COLOR_DAC), dac_line = count_color_in(242, 346, 244 + 2, 244 + 37, COLOR_PLAYING);
		const int dac_left = count_color_in(474, 491, 244 + 4, 244 + 24, COLOR_TEXT), dac_vol = count_color_in(354, 466, 244 + 4, 244 + 24, COLOR_DAC_DARK);
		send_midi(0x85, 38, 0);
		send_midi(0xB5, 10, 64);
		run_ms(100);
		click(window, BUTTON_EDITOR_X, BUTTON_Y);
		run_ms(100);
		capture(window);
		CHECK(dac_sample_px > 50 && dac_line > 10 && dac_left > 100 && dac_vol > 100,
			"ALL's DAC row: the sample (%d px), its voice's line (%d px), PAN L from CC 10 (%d px), VOL in the DAC's colour (%d px)", dac_sample_px, dac_line, dac_left, dac_vol);
		// Stage 20: the window still shows FM rows instead of graphs, but channel 6 on the DAC has no row.
		CHECK(count_color_in(242, 708, HEADER_H, 460, COLOR_FM_LIGHT) + count_color_in(242, 708, HEADER_H, 460, COLOR_FM_DARK) == 0, "the DAC's page draws no arp row");
		const clap_plugin_state_t *state = plugin->get_extension(plugin, CLAP_EXT_STATE);
		clap_ostream_t out_stream = {NULL, project_write};
		project_size = 0;
		CHECK(state && state->save(plugin, &out_stream), "the project saves");
		// A block of samples (D38): note 40, a head of 61 bytes (name, a source of 4410 frames at
		// 22050 Hz kept whole, rate AUTO picked 1, 4096 bytes), the source, then the bytes.
		const uint32_t type = 9, bytes = 4096, head = 61, frames = 4410, hz = 22050;
		const uint32_t length = 1 + 2 + head + frames * 2 + bytes;
		memcpy(project + project_size, &type, 4);
		memcpy(project + project_size + 4, &length, 4);
		uint8_t *block = project + project_size + 8;
		memset(block, 0, length);
		block[0] = 40;
		memcpy(block + 1, &head, 2);
		memcpy(block + 3, "ramp.wav", 8);
		memcpy(block + 3 + 32, &hz, 4);
		memcpy(block + 3 + 36, &frames, 4);
		memcpy(block + 3 + 44, &frames, 4);
		block[3 + 52] = 0xFF;
		block[3 + 53] = 1;
		block[3 + 55] = 1;
		memcpy(block + 3 + head - 4, &bytes, 4);
		for (uint32_t i = 0; i < frames; i++) {
			const int16_t v = (int16_t)(8000 * sin(2 * 3.14159265358979 * 220 * i / hz));
			memcpy(block + 3 + head + i * 2, &v, 2);
		}
		for (uint32_t i = 0; i < bytes; i++) {
			block[3 + head + frames * 2 + i] = (uint8_t)(i % 64);
		}
		project_size += 8 + length;
		project_read = 0;
		clap_istream_t in_stream = {NULL, project_take};
		CHECK(state->load(plugin, &in_stream), "a project with a sample loads");
		const long restart_ms = wait_applied(window);
		capture(window);
		save_bmp(argv[2], "dac-file");
		const int cell = count_color_in(GRID_NOTE40_X, GRID_NOTE40_X + 34, GRID_NOTE40_Y, GRID_NOTE40_Y + 16, COLOR_DAC);
		CHECK(restart_ms >= 0 && cell > 300, "a project with a sample goes into the running core, no LOADING (%ld ms), note 40's cell is a file's (%d px)", restart_ms, cell);
		// The panel prepares a file's sample (D38): a drag on GAIN is one restart, on mouse up.
		click(window, GRID_NOTE40_X + 17, GRID_NOTE40_Y + 8);
		run_ms(100);
		capture(window);
		save_bmp(argv[2], "dac-panel");
		const int tabs = count_color_in(234, 250, DAC_TRIM_Y, DAC_TRIM_Y + 14, COLOR_DAC) + count_color_in(694, 710, DAC_TRIM_Y, DAC_TRIM_Y + 14, COLOR_DAC);
		CHECK(tabs >= 2 * 8 * 14 && count_color_in(DAC_GAIN_X, DAC_GAIN_X + 100, DAC_ROW_B_Y, DAC_ROW_B_Y + 20, COLOR_DAC_DARK) == 0, "the trim's tabs stand at the wave's ends (%d px), no gain", tabs);
		// The start's tab dragged in by a quarter, one restart.
		SendMessageA(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(238, DAC_TRIM_Y + 7));
		SendMessageA(window, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(300, DAC_TRIM_Y + 7));
		SendMessageA(window, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(352, DAC_TRIM_Y + 7));
		SendMessageA(window, WM_LBUTTONUP, 0, MAKELPARAM(352, DAC_TRIM_Y + 7));
		const long trim_ms = wait_applied(window);
		run_ms(100);
		capture(window);
		save_bmp(argv[2], "dac-trim");
		const int moved = count_color_in(346, 358, DAC_TRIM_Y, DAC_TRIM_Y + 14, COLOR_DAC);
		CHECK(trim_ms >= 0 && moved >= 8 * 14, "the start's tab dragged to a quarter trims there, no restart (%ld ms)", trim_ms);
		// As Windows sends a double click: down, up, double click, up. The first click moves nothing.
		SendMessageA(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(352, DAC_TRIM_Y + 7));
		SendMessageA(window, WM_LBUTTONUP, 0, MAKELPARAM(352, DAC_TRIM_Y + 7));
		run_ms(100);
		capture(window);
		const bool still = count_color(0, HEADER_H, COLOR_AMBER) == 0 && count_color_in(346, 358, DAC_TRIM_Y, DAC_TRIM_Y + 14, COLOR_DAC) + count_color_in(346, 358, DAC_TRIM_Y, DAC_TRIM_Y + 14, COLOR_TEXT) >= 8 * 14;
		CHECK(still, "a click on a tab without moving changes nothing, no restart");
		SendMessageA(window, WM_LBUTTONDBLCLK, MK_LBUTTON, MAKELPARAM(352, DAC_TRIM_Y + 7));
		SendMessageA(window, WM_LBUTTONUP, 0, MAKELPARAM(352, DAC_TRIM_Y + 7));
		const long untrim_ms = wait_applied(window);
		capture(window);
		CHECK(untrim_ms >= 0 && count_color_in(234, 242, DAC_TRIM_Y, DAC_TRIM_Y + 14, COLOR_DAC) >= 8 * 14, "a double click on the tab puts the start back");
		// While the mouse is down nothing is prepared: the header stays RUNNING.
		SendMessageA(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(DAC_GAIN_X + 50, DAC_ROW_B_Y + 10));
		for (int step = 1; step <= 4; step++) {
			SendMessageA(window, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(DAC_GAIN_X + 50, DAC_ROW_B_Y + 10 - step * 10));
			run_ms(50);
		}
		capture(window);
		const bool running = count_color(0, HEADER_H, COLOR_AMBER) == 0 && count_color(0, HEADER_H, COLOR_PLAYING) > 0;
		SendMessageA(window, WM_LBUTTONUP, 0, MAKELPARAM(DAC_GAIN_X + 50, DAC_ROW_B_Y - 30));
		CHECK(running, "during the drag the engine keeps running");
		const long gain_ms = wait_applied(window);
		run_ms(100);
		capture(window);
		save_bmp(argv[2], "dac-gain");
		const int gain = count_color_in(DAC_GAIN_X, DAC_GAIN_X + 100, DAC_ROW_B_Y, DAC_ROW_B_Y + 20, COLOR_DAC_DARK);
		CHECK(gain_ms >= 0 && gain > 100, "a drag on GAIN prepares the sample on mouse up, no restart (%ld ms), the bar shows it (%d px)", gain_ms, gain);
		SendMessageA(window, WM_LBUTTONDBLCLK, MK_LBUTTON, MAKELPARAM(DAC_GAIN_X + 50, DAC_ROW_B_Y + 10));
		SendMessageA(window, WM_LBUTTONUP, 0, MAKELPARAM(DAC_GAIN_X + 50, DAC_ROW_B_Y + 10));
		const long back_ms = wait_applied(window);
		capture(window);
		CHECK(back_ms >= 0 && count_color_in(DAC_GAIN_X, DAC_GAIN_X + 100, DAC_ROW_B_Y, DAC_ROW_B_Y + 20, COLOR_DAC_DARK) == 0, "a double click puts GAIN back to 0");
		click(window, BUTTON_BANK_X, BUTTON_Y);
		run_ms(200);
		capture(window);
		save_bmp(argv[2], "dac-list");
		const int file = count_color_in(DAC_FILE_X, DAC_FILE_X + 24, DAC_NOTE40_Y, DAC_NOTE40_Y + 20, COLOR_DAC);
		CHECK(file > 10, "BANK lists the notes, 40 says FILE (%d px)", file);
		click(window, DAC_RESET_X, DAC_NOTE40_Y + 9);
		const long reset_ms = wait_applied(window);
		capture(window);
		CHECK(reset_ms >= 0 && count_color_in(DAC_FILE_X, DAC_FILE_X + 24, DAC_LIST_Y, DAC_LIST_BOTTOM, COLOR_DAC) == 0, "CLEAR gives note 40 the ROM's sample again, no restart (%ld ms)", reset_ms);
		// Stage 18: a WAV dropped on the open note's wave replaces its sound, an FM file is refused
		// there; LOOP and ONE-SHOT exclude each other again (stage 22b, D54 cancels D53).
		click(window, BUTTON_EDITOR_X, BUTTON_Y);
		run_ms(200);
		click(window, GRID_NOTE40_X + 17, GRID_NOTE40_Y + 8);
		run_ms(100);
		char wav_path[MAX_PATH], tfi_path[MAX_PATH];
		GetTempPathA(MAX_PATH, wav_path);
		strcpy(tfi_path, wav_path);
		strcat(wav_path, "mdmi_gui_test_wave.wav");
		strcat(tfi_path, "mdmi_gui_test_wave.tfi");
		write_wav(wav_path, 22050, 11025);
		{
			static const uint8_t tfi[42] = {4, 0};
			FILE *f = fopen(tfi_path, "wb");
			fwrite(tfi, 1, sizeof(tfi), f);
			fclose(f);
		}
		const int rom_cell = count_color_in(GRID_NOTE40_X, GRID_NOTE40_X + 34, GRID_NOTE40_Y, GRID_NOTE40_Y + 16, COLOR_DAC);
		const DWORD tfi_effect = drop_file(window, tfi_path, 470, 333);
		const DWORD wav_effect = drop_file(window, wav_path, 470, 333);
		const long wave_ms = wait_applied(window);
		capture(window);
		save_bmp(argv[2], "dac-wave-drop");
		const int wave_cell = count_color_in(GRID_NOTE40_X, GRID_NOTE40_X + 34, GRID_NOTE40_Y, GRID_NOTE40_Y + 16, COLOR_DAC);
		CHECK(tfi_effect == DROPEFFECT_NONE, "an FM file over the wave is refused");
		// Stage 23 (D74): no NOTES cell; AUDIO IN is a lamp, dark without a track on the input.
		{
			capture(window);
			const int before = count_color_in(242, 712, 56, 400, COLOR_DAC_DARK);
			click(window, 650, 66);
			run_ms(200);
			capture(window);
			CHECK(count_color_in(524, 596, 56, 76, COLOR_DAC_DARK) == 0 && count_color_in(600, 708, 56, 76, COLOR_DAC_DARK) == 0
					&& count_color_in(242, 712, 56, 400, COLOR_DAC_DARK) == before,
				"the DAC page has no NOTES switch, its AUDIO IN lamp is dark without input and takes no click");
		}
		CHECK(wav_effect == DROPEFFECT_COPY && wave_ms >= 0 && rom_cell < 300 && wave_cell > 300, "a WAV dropped on the wave replaces the open note's sound (cell %d px, was %d)", wave_cell, rom_cell);
		// Stage 30: LOOP has no switch of its own; the loop's row turns it on and off.
		{
			int sx;
			capture(window);
			const int button = count_color_in(620, 700, DAC_ROW_A_Y, DAC_ROW_A_Y + 20, COLOR_CELL);
			click(window, 660, DAC_ROW_A_Y + 10);
			run_ms(200);
			capture(window);
			CHECK(button == 0 && longest_run(DAC_LOOP_Y + 1, 242, 712, COLOR_DAC_DARK, &sx) < 20, "no LOOP switch over ONE-SHOT: its place takes no click (%d px of a cell)", button);
		}
		click(window, 400, DAC_LOOP_Y + 6);
		const long loop_ms = wait_applied(window);
		capture(window);
		save_bmp(argv[2], "dac-loop");
		int loop_x;
		const int loop_on = longest_run(DAC_LOOP_Y + 1, 242, 712, COLOR_DAC_DARK, &loop_x);
		const int shot_off = count_color_in(620, 700, DAC_ROW_B_Y, DAC_ROW_B_Y + 20, COLOR_DAC_DARK);
		CHECK(loop_ms >= 0 && loop_on > 300 && shot_off == 0, "a click on the loop's shadow turns LOOP on (%d px), ONE-SHOT stays off", loop_on);
		click(window, 660, DAC_ROW_B_Y + 10);
		wait_applied(window);
		capture(window);
		const int loop_off = longest_run(DAC_LOOP_Y + 1, 242, 712, COLOR_DAC_DARK, &loop_x);
		const int shot_on = count_color_in(620, 700, DAC_ROW_B_Y, DAC_ROW_B_Y + 20, COLOR_DAC_DARK);
		save_bmp(argv[2], "dac-loop-one-shot");
		CHECK(shot_on > 1000 && loop_off < 20, "ONE-SHOT lights and LOOP goes off (%d px, loop %d px)", shot_on, loop_off);
		click(window, 400, DAC_LOOP_Y + 6);
		wait_applied(window);
		capture(window);
		CHECK(count_color_in(620, 700, DAC_ROW_B_Y, DAC_ROW_B_Y + 20, COLOR_DAC_DARK) == 0 && longest_run(DAC_LOOP_Y + 1, 242, 712, COLOR_DAC_DARK, &loop_x) > 300,
			"and LOOP on again takes ONE-SHOT off");
		// Stage 18b: the loop's row. Its body (dark) runs from START to the last whole block; the end
		// tab dragged left leaves a release, the body dragged moves the loop with its length and stops
		// at the last whole block, a click turns LOOP off, a click on the shadow on again.
		{
			const int row = DAC_LOOP_Y + 1;
			int s0, s1, s2, s3, s4;
			const int l0 = longest_run(row, 242, 712, COLOR_DAC_DARK, &s0);
			drag_x(window, s0 + l0 + 5, DAC_LOOP_Y + 6, s0 + l0 + 5 - 200);
			const long end_ms = wait_applied(window);
			capture(window);
			save_bmp(argv[2], "dac-loop-release");
			const int l1 = longest_run(row, 242, 712, COLOR_DAC_DARK, &s1);
			CHECK(end_ms >= 0 && l1 < l0 - 170 && l1 > l0 - 230 && abs(s1 - s0) <= 2, "the loop's end tab dragged 200 px left: the body %d px, was %d", l1, l0);
			drag_x(window, s1 + l1 / 2, DAC_LOOP_Y + 6, s1 + l1 / 2 + 80);
			wait_applied(window);
			capture(window);
			const int l2 = longest_run(row, 242, 712, COLOR_DAC_DARK, &s2);
			CHECK(abs(l2 - l1) <= 3 && abs(s2 - s1 - 80) <= 12, "the body dragged 80 px moves the loop %d px, its length %d, was %d", s2 - s1, l2, l1);
			drag_x(window, s2 + l2 / 2, DAC_LOOP_Y + 6, s2 + l2 / 2 + 400);
			wait_applied(window);
			capture(window);
			save_bmp(argv[2], "dac-loop-moved");
			const int l3 = longest_run(row, 242, 712, COLOR_DAC_DARK, &s3);
			CHECK(abs(l3 - l1) <= 3 && s3 + l3 <= s0 + l0 + 3, "dragged far right it stops at the last whole block (ends %d, the whole %d), length %d", s3 + l3,
				s0 + l0, l3);
			drag_x(window, s3 + l3 / 2, DAC_LOOP_Y + 6, s3 + l3 / 2 - 600);
			wait_applied(window);
			capture(window);
			int s5;
			const int l5 = longest_run(row, 242, 712, COLOR_DAC_DARK, &s5);
			CHECK(abs(l5 - l1) <= 3 && abs(s5 - s0) <= 2, "dragged far left it stops at START (%d, START %d), length %d", s5, s0, l5);
			drag_x(window, s5 + l5 / 2, DAC_LOOP_Y + 6, s5 + l5 / 2 + (s3 - s5));
			wait_applied(window);
			capture(window);
			longest_run(row, 242, 712, COLOR_DAC_DARK, &s3);
			SendMessageA(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(s3 + l3 / 2, DAC_LOOP_Y + 6));
			SendMessageA(window, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(s3 + l3 / 2 + 2, DAC_LOOP_Y + 6));
			SendMessageA(window, WM_LBUTTONUP, 0, MAKELPARAM(s3 + l3 / 2 + 2, DAC_LOOP_Y + 6));
			wait_applied(window);
			capture(window);
			save_bmp(argv[2], "dac-loop-shadow");
			const int shadow = longest_run(row, 242, 712, COLOR_DAC_DARK, &s4);
			CHECK(shadow < 20, "a click within 3 px on the body turns LOOP off, the row shows its shadow");
			click(window, s3 + l3 / 2, DAC_LOOP_Y + 6);
			wait_applied(window);
			capture(window);
			const int l4 = longest_run(row, 242, 712, COLOR_DAC_DARK, &s4);
			CHECK(abs(l4 - l3) <= 3 && abs(s4 - s3) <= 2,
				"a click on the shadow turns LOOP on where it was (%d px at %d)", l4, s4);
			// Stage 22b (D54): release or stop by a click on the loop's end tab or the row after it.
			// The wave after the loop's end is lit while the release plays, dim with stop.
			{
				const int wave_y = 323, wave_bottom = 355;
				// The loop ends on the last whole block: no release, the click changes nothing.
				const int lit_none = count_color_in(s4 + l4 + 4, 700, wave_y, wave_bottom, COLOR_DAC);
				click(window, s4 + l4 + 5, DAC_LOOP_Y + 6);
				wait_applied(window);
				capture(window);
				int s6;
				const int l6_same = longest_run(row, 242, 712, COLOR_DAC_DARK, &s6);
				CHECK(count_color_in(s4 + l4 + 4, 700, wave_y, wave_bottom, COLOR_DAC) == lit_none && l6_same == l4 && s6 == s4, "no release: a click on the end tab changes nothing");
				// The loop moved left leaves a release.
				drag_x(window, s4 + l4 / 2, DAC_LOOP_Y + 6, s4 + l4 / 2 - 100);
				wait_applied(window);
				capture(window);
				const int l6 = longest_run(row, 242, 712, COLOR_DAC_DARK, &s6);
				const int tab = s6 + l6 + 5, after = s6 + l6 + 4;
				const int lit_release = count_color_in(after, 700, wave_y, wave_bottom, COLOR_DAC);
				click(window, tab, DAC_LOOP_Y + 6);
				wait_applied(window);
				capture(window);
				save_bmp(argv[2], "dac-loop-stop");
				int s7;
				const int l7 = longest_run(row, 242, 712, COLOR_DAC_DARK, &s7);
				const int lit_stop = count_color_in(after, 700, wave_y, wave_bottom, COLOR_DAC);
				const int stop_frame = count_color_in(after + 8, 700, DAC_LOOP_Y, DAC_LOOP_Y + 1, COLOR_LINE);
				CHECK(lit_release > 300 && lit_stop < lit_release / 4 && l7 == l6 && s7 == s6 && stop_frame > 20,
					"a click on the end tab: stop, the release dims (%d px lit, was %d), its row outlined (%d px), the loop doesn't move", lit_stop, lit_release, stop_frame);
				click(window, after + 30, DAC_LOOP_Y + 6);
				wait_applied(window);
				capture(window);
				const int lit_back = count_color_in(after, 700, wave_y, wave_bottom, COLOR_DAC);
				CHECK(abs(lit_back - lit_release) < 20, "a click on the row after it: the release again (%d px lit)", lit_back);
				// Stop, then ONE-SHOT and LOOP again: the loop comes back with its release.
				click(window, after + 30, DAC_LOOP_Y + 6);
				wait_applied(window);
				click(window, 660, DAC_ROW_B_Y + 10);
				wait_applied(window);
				capture(window);
				int s_off;
				const bool shot_only = longest_run(row, 242, 712, COLOR_DAC_DARK, &s_off) < 20 && count_color_in(620, 700, DAC_ROW_B_Y, DAC_ROW_B_Y + 20, COLOR_DAC_DARK) > 1000;
				click(window, s6 + l6 / 2, DAC_LOOP_Y + 6);
				wait_applied(window);
				capture(window);
				const int lit_again = count_color_in(after, 700, wave_y, wave_bottom, COLOR_DAC);
				CHECK(shot_only && abs(lit_again - lit_release) < 20 && count_color_in(620, 700, DAC_ROW_B_Y, DAC_ROW_B_Y + 20, COLOR_DAC_DARK) == 0,
					"ONE-SHOT takes LOOP off; LOOP on again takes ONE-SHOT off and comes with its release (%d px lit)", lit_again);
				// A double click on the end tab: no release, in one undo step with the click before it.
				double_click(window, tab, DAC_LOOP_Y + 6);
				wait_applied(window);
				capture(window);
				int s8;
				const int l8 = longest_run(row, 242, 712, COLOR_DAC_DARK, &s8);
				click(window, ACTION_UNDO_X, BUTTON_Y);
				wait_applied(window);
				capture(window);
				int s9;
				const int l9 = longest_run(row, 242, 712, COLOR_DAC_DARK, &s9);
				const int lit_undone = count_color_in(after, 700, wave_y, wave_bottom, COLOR_DAC);
				CHECK(l8 > l6 + 50 && l9 == l6 && s9 == s6 && abs(lit_undone - lit_release) < 20,
					"a double click on the end tab: the loop to the last whole block (%d px, was %d); one UNDO gives back the loop with its release (%d px)", l8, l6, l9);
			}
			// Stage 30: under one block the row alone turns LOOP off, as the switch did (END dragged
			// next to START with LOOP on: the row frames TOO SHORT in amber, a click in it ends the loop).
			{
				const int trim_row = DAC_TRIM_Y + 6;
				int last = -1;
				for (int x = 242; x < WIDTH; x++) {
					last = (pixels[trim_row * WIDTH + x] & 0xFFFFFF) == COLOR_DAC ? x : last;
				}
				drag_x(window, last - 3, trim_row, 252);
				wait_applied(window);
				capture(window);
				save_bmp(argv[2], "dac-loop-too-short");
				const int amber = count_color_in(242, 712, DAC_LOOP_Y, DAC_LOOP_Y + 12, COLOR_AMBER);
				click(window, 246, DAC_LOOP_Y + 6);
				wait_applied(window);
				capture(window);
				const int amber_after = count_color_in(242, 712, DAC_LOOP_Y, DAC_LOOP_Y + 12, COLOR_AMBER);
				const int hint = count_color_in(242, 400, DAC_LOOP_Y, DAC_LOOP_Y + 12, COLOR_HINT);
				CHECK(last > 600 && amber >= 4 && amber_after == 0 && hint > 20,
					"under one block: TOO SHORT in amber (%d px), a click on it turns LOOP off and the row says why (%d px of hint)", amber, hint);
			}
		}
		DeleteFileA(wav_path);
		DeleteFileA(tfi_path);
		// Last, so its modes and notes touch no other check.
		click(window, BUTTON_ROM_X, BUTTON_Y);
		run_ms(500);
		// Stage 24a2's frames for the art-director: a beat on FM, PSG and the noise for about 1.5 s so
		// the ribbon has bars and holds, a fresh hit white, released notes dim.
		for (int step = 0; step < 12; step++) {
			const int ch = step % 3;
			send_midi((uint8_t)(0x90 | ch), (uint8_t)(48 + ch * 7), 100);
			send_midi(0x96, (uint8_t)(60 + (step & 3) * 2), 90);
			if (step % 2 == 0) {
				send_midi(0x99, 60, (uint8_t)(60 + step * 5));
			}
			run_ms(60);
			send_midi((uint8_t)(0x80 | ch), (uint8_t)(48 + ch * 7), 0);
			send_midi(0x86, (uint8_t)(60 + (step & 3) * 2), 0);
			send_midi(0x89, 60, 0);
			run_ms(60);
		}
		for (int ch = 3; ch < 5; ch++) {
			send_midi((uint8_t)(0x90 | ch), (uint8_t)(48 + ch * 5), (uint8_t)(40 + ch * 20));
		}
		for (int ch = 7; ch < 9; ch++) {
			send_midi((uint8_t)(0x90 | ch), (uint8_t)(57 + (ch - 6) * 12), (uint8_t)(50 + (ch - 6) * 35));
		}
		// Held long enough for their notes bright, their waves and a long hold line; then a fresh hit
		// on FM1 and the noise, caught while it is still white or near: its colour lands a VBlank after its note, white
		// 3 frames, then over 18 to the held colour.
		run_ms(700);
		send_midi(0x99, 60, 120);
		send_midi(0x90, 72, 110);
		run_ms(70);
		capture(window);
		save_bmp(argv[2], "rom-24a3-play");
		send_midi(0x80, 72, 0);
		send_midi(0x89, 60, 0);
		for (int ch = 3; ch < 5; ch++) {
			send_midi((uint8_t)(0x80 | ch), (uint8_t)(48 + ch * 5), 0);
		}
		for (int ch = 7; ch < 9; ch++) {
			send_midi((uint8_t)(0x80 | ch), (uint8_t)(57 + (ch - 6) * 12), 0);
		}
		run_ms(700);
		capture(window);
		save_bmp(argv[2], "rom-24a3-released");
		send_midi(0xB0, 78, 127);
		run_ms(300);
		for (int i = 0; i < 4; i++) {
			send_midi(0x95, (uint8_t)(36 + i * 2), 127);
			run_ms(150);
		}
		run_ms(200);
		capture(window);
		save_bmp(argv[2], "rom-24a3-dac");
		for (int i = 0; i < 4; i++) {
			send_midi(0x85, (uint8_t)(36 + i * 2), 0);
		}
		send_midi(0xB0, 78, 0);
		send_midi(0xB0, 80, 127);
		run_ms(300);
		// Ch3 OPS: operators 1-3 on MIDI 11-13 strike channel 3's ribbon in pink.
		send_midi(0x92, 60, 110);
		for (int op = 0; op < 3; op++) {
			run_ms(150);
			send_midi((uint8_t)(0x9A + op), (uint8_t)(64 + op * 3), 100);
		}
		run_ms(300);
		capture(window);
		save_bmp(argv[2], "rom-24a3-special");
		send_midi(0x82, 60, 0);
		for (int op = 0; op < 3; op++) {
			send_midi((uint8_t)(0x8A + op), (uint8_t)(64 + op * 3), 0);
		}
		send_midi(0xB0, 80, 0);
		run_ms(3000);
		// Stage 29's frames for the owner (rom-screen.md 11, art-director): FM 1-6 at velocities 127, 112,
		// 100, 80, 64, 30; velocity 64 at CC 7 127, 100, 64; 30 then 127 back to back; a decay at 0, 0.5,
		// 1, 2 s; Ch3 Special's squares over the same times.
		static const uint8_t VELOCITIES[6] = {127, 112, 100, 80, 64, 30};
		for (int ch = 0; ch < 6; ch++) {
			send_midi((uint8_t)(0x90 | ch), 60, VELOCITIES[ch]);
		}
		run_ms(120);
		capture(window);
		save_bmp(argv[2], "rom-29-velocity");
		for (int ch = 0; ch < 6; ch++) {
			send_midi((uint8_t)(0x80 | ch), 60, 0);
		}
		run_ms(1500);
		static const uint8_t VOLUMES[3] = {127, 100, 64};
		for (int ch = 0; ch < 3; ch++) {
			send_midi((uint8_t)(0xB0 | ch), 7, VOLUMES[ch]);
			send_midi((uint8_t)(0x90 | ch), 60, 64);
		}
		run_ms(120);
		capture(window);
		save_bmp(argv[2], "rom-29-cc7");
		for (int ch = 0; ch < 3; ch++) {
			send_midi((uint8_t)(0x80 | ch), 60, 0);
			send_midi((uint8_t)(0xB0 | ch), 7, 127);
		}
		run_ms(1500);
		send_midi(0x90, 60, 30);
		run_ms(300);
		send_midi(0x80, 60, 0);
		send_midi(0x90, 62, 127);
		run_ms(100);
		capture(window);
		save_bmp(argv[2], "rom-29-adjacent");
		static const int DECAY_MS[4] = {0, 500, 1000, 2000};
		for (int i = 0; i < 4; i++) {
			run_ms(i ? DECAY_MS[i] - DECAY_MS[i - 1] : 30);
			capture(window);
			char name[32];
			snprintf(name, sizeof(name), "rom-29-decay-%d", i);
			save_bmp(argv[2], name);
		}
		send_midi(0x80, 62, 0);
		run_ms(1500);
		send_midi(0xB0, 80, 127);
		run_ms(300);
		send_midi(0x92, 60, 110);
		for (int op = 0; op < 3; op++) {
			send_midi((uint8_t)(0x9A + op), (uint8_t)(64 + op * 3), 100);
		}
		// The first frame 100 ms in: OPS keys on later than a plain channel (the plugin's latency, the
		// moment's dealing and the ROM's frame), at 30 ms nothing sounds yet.
		for (int i = 0; i < 4; i++) {
			run_ms(i ? DECAY_MS[i] - DECAY_MS[i - 1] : 100);
			if (i == 3) {
				send_midi(0x8C, 70, 0);
				run_ms(300);
			}
			capture(window);
			char name[32];
			snprintf(name, sizeof(name), "rom-29-squares-%d", i);
			save_bmp(argv[2], name);
		}
		send_midi(0x82, 60, 0);
		for (int op = 0; op < 3; op++) {
			send_midi((uint8_t)(0x8A + op), (uint8_t)(64 + op * 3), 0);
		}
		send_midi(0xB0, 80, 0);
		run_ms(3000);
		click(window, BUTTON_EDITOR_X, BUTTON_Y);
		send_midi(0xB0, 78, 0);
		run_ms(800);

		// A host may destroy our parent before telling us, then attach the same view again.
		DestroyWindow(parent);
		run_ms(100);
		parent = CreateWindowExA(0, "STATIC", "gui_test", WS_POPUP, 0, 0, WIDTH, HEIGHT, NULL, NULL, NULL, NULL);
		window_ref.win32 = parent;
		CHECK(gui->set_parent(plugin, &window_ref) && gui->show(plugin), "window survives losing its parent");
		window = FindWindowExA(parent, NULL, "MDMIPluginWindow", NULL);
		CHECK(window != NULL, "a new window exists");
		run_ms(100);

		CHECK(gui->set_scale(plugin, 2.0), "scale accepted");
		gui->get_size(plugin, &width, &height);
		CHECK(width == WIDTH * 2 && height == HEIGHT * 2, "doubled size is %ux%u", width, height);
		gui->set_scale(plugin, 1.0);
	}

	gui->destroy(plugin);
	DestroyWindow(parent);
	plugin->deactivate(plugin);
	plugin->destroy(plugin);
	entry->deinit();
	printf("clean_shutdown=1\n");
	printf("%s: %d failed\n", failures ? "FAILED" : "PASSED", failures);
	fflush(stdout);
	return failures ? 1 : 0;
}
