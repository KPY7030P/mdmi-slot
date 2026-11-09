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
#define LFO_RATE_X 160
#define LFO_RATE_Y 484
#define LFO_RATE 3
// The bank page's button, its third place, and channel 4's Preset.
#define BUTTON_BANK_X 244
#define BANK_ROW_X 400
#define BANK_ROW3_Y 122
#define CH4_PRESET 1400
#define COLOR_FM_DARK 0x222266
// The envelope editor of a PSG channel: channel 8 in the rail, the level lane, a row near its top
// (level 0), and UNDO.
#define RAIL_CH8_Y 350
#define ENV_X 296
#define ENV_RIGHT 700
#define ENV_LEVEL_Y 122
#define ENV_LEVEL_BOTTOM 282
// In the bright top of a bar at level 0: below it the body is dark (4A).
#define ENV_LOUD_Y 123
#define ACTION_UNDO_X 684
#define ENV_LENGTH_X 670
#define ENV_LENGTH_Y 397
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
#define CH3_SPECIAL_X 400
#define CH3_SPECIAL_Y 480
#define COLOR_LABEL 0xAAAAAA
#define COLOR_FM_LIGHT 0x8888EE
#define COLOR_TEXT 0xEEEEEE
#define SCROLL_X 702
#define SCROLL_TOP 72
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
#define DAC_SCROLL_X 702
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
#define DAC_GAIN_X 396
#define CH2_PAN 1202
#define COLOR_FM 0x4422EE
#define COLOR_MARK 0xEECC66
#define FOOTER_TOP 504
// Stage 11: the FM page of v8. The operators' columns, the graph, the SSG button and its choice,
// a row's y in each column, and the ids of channel 4's operator 3 AR and RS, operator 1 DT, RS, AM and SSG.
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
		static uint32_t before[WIDTH * HEIGHT];
		memcpy(before, pixels, sizeof(pixels));

		send_midi(0x80, 60, 0);
		send_midi(0x86, 69, 0);
		run_ms(500);
		capture(window);
		CHECK(memcmp(before + HEADER_H * WIDTH, pixels + HEADER_H * WIDTH, (HEIGHT - HEADER_H) * WIDTH * sizeof(uint32_t)) != 0, "ROM screen is live: it changes when notes stop");

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
		CHECK(count_color_in(BAR_LEFT, BAR_RIGHT, RAIL_CH4_TOP + 20, RAIL_CH4_TOP + 30, COLOR_ERROR) > 0, "and channel 4's row says CUT");
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
		const int op1 = count_color_in(column, 226, top + 9, top + 17, COLOR_LABEL);
		// The bar is as long as on a row with one note: its last segment ends short of the column.
		const int bar_past = count_color_in(column, 226, RAIL_CH3_TOP + 25, RAIL_CH3_TOP + 29, COLOR_FM_LIGHT);
		const int bar_light = count_color_in(BAR_LEFT, column, RAIL_CH3_TOP + 25, RAIL_CH3_TOP + 29, COLOR_FM_LIGHT) + count_color_in(BAR_LEFT, column, RAIL_CH3_TOP + 25, RAIL_CH3_TOP + 29, COLOR_PLAYING);
		// The row is light FM: the number in it, and 3 px between the name and the notes below.
		const int light_number = count_color_in(10, BAR_LEFT, RAIL_CH3_TOP, RAIL_CH3_TOP + 39, COLOR_FM_LIGHT);
		const int name_low = count_color_in(BAR_LEFT, 226, RAIL_CH3_TOP + 19, RAIL_CH3_TOP + 22, COLOR_TEXT);
		send_midi(0x82, 60, 0);
		send_midi(0x8A, 64, 0);
		click(window, CH3_SPECIAL_X, CH3_SPECIAL_Y);
		run_ms(300);
		capture(window);
		const int plain_number = count_color_in(10, BAR_LEFT, RAIL_CH3_TOP, RAIL_CH3_TOP + 39, COLOR_FM_LIGHT);
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
		capture(window);
		CHECK(count_color_in(ALL_SOUND_X - 48, ALL_SOUND_RIGHT, RAIL_CH1_TOP, RAIL_CH1_TOP + 6 * ROW_H, COLOR_FM) > 6 * 50, "the FM rows draw their algorithms");
		CHECK(count_color_in(ALL_SOUND_X - 48, ALL_SOUND_RIGHT, RAIL_CH8_Y - 30, RAIL_CH8_Y + 10, COLOR_PSG) > 0, "the PSG rows draw their envelopes");
		// Stage 11: a click on an FM row's sound puts that row's envelopes in place of its diagram, a
		// second click brings the diagram back; other rows keep theirs.
		click(window, ALL_SOUND_X, ALL_CH2_Y);
		run_ms(100);
		capture(window);
		save_bmp(argv[2], "all-envelopes");
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
		save_bmp(argv[2], "all");
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
		click(window, BUTTON_EDITOR_X, BUTTON_Y);
		run_ms(100);
		CHECK(!keys->key(plugin, true), "the editor page leaves arrows to the host");
		run_ms(100);

		// Stage 8: drawing channel 8's envelope. A fast drag across the level lane fills every
		// column it passed, tells the host nothing (D19), and UNDO puts the envelope back.
		click(window, RAIL_X, RAIL_CH8_Y);
		run_ms(100);
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
		past = count_color_in(ENV_RIGHT, 708, ENV_LEVEL_Y, ENV_LEVEL_BOTTOM, COLOR_PSG);
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
		// Dragged far down: one frame, one column across the whole lane, drawn loud. Two frames
		// would draw 402 px.
		drag(window, ENV_LENGTH_X, ENV_LENGTH_Y, ENV_LENGTH_Y + 300);
		click(window, ENV_X + 100, ENV_LOUD_Y);
		run_ms(200);
		capture(window);
		save_bmp(argv[2], "envelope-short");
		const int wide = count_color_in(ENV_X, ENV_RIGHT, ENV_LOUD_Y, ENV_LOUD_Y + 1, COLOR_PSG);
		CHECK(wide >= 403 && pushed[0] + pushed[1] + pushed[2] == 0, "a length of one frame draws one column of %d px, the host told nothing", wide);

		// Stage 16 (D45): channel 10's page has the noise's TYPE and RATE; TONE 3 takes channel 9.
		click(window, RAIL_X, RAIL_CH8_Y + 80);
		run_ms(100);
		click(window, 669, 92);
		run_ms(200);
		capture(window);
		save_bmp(argv[2], "noise");
		CHECK(get_value(8) == 3 && count_color_in(633, 706, 83, 101, COLOR_PSG) > 500, "TONE 3 on channel 10's page sets Noise Rate and lights in PSG's colour");
		click(window, RAIL_X, RAIL_CH8_Y + 40);
		run_ms(200);
		capture(window);
		save_bmp(argv[2], "noise-taken");
		CHECK(count_color_in(332, 662, 442, 449, COLOR_AMBER) > 100, "channel 9's page says in amber that the noise takes it");
		click(window, RAIL_X, RAIL_CH8_Y + 80);
		run_ms(100);
		click(window, 614, 92);
		run_ms(200);
		CHECK(get_value(8) == 0, "HIGH puts the rate back");

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
		const int tabs = count_color_in(234, 250, DAC_TRIM_Y, DAC_TRIM_Y + 14, COLOR_DAC) + count_color_in(692, 708, DAC_TRIM_Y, DAC_TRIM_Y + 14, COLOR_DAC);
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
