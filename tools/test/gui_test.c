// Opens the plugin's window with no host around it, plays notes, and checks the pixels it draws.
//
//   gui_test <plugin.clap> <picture_prefix>
//
// Writes <prefix>-editor.bmp and <prefix>-rom.bmp. With MDMI_TEST_CRASH=segv it instead expects the
// window to report a dead engine.
#include <stdbool.h>
#include <stdint.h>
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
#define BUTTON_EDITOR_X 100
#define BUTTON_ROM_X 180
#define BUTTON_Y 12
// Channel 4 in the rail, Op1 TL on the FM page, and the ids of Op1 TL on channels 3 and 4.
#define RAIL_X 100
#define RAIL_CH2_Y 104
#define RAIL_CH4_Y 184
#define OP1_TL_X 300
#define OP1_TL_Y 212
#define CH3_OP1_TL 1311
#define CH4_OP1_TL 1411
#define POLYPHONY_X 40
#define POLYPHONY_Y 484

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
static void run_block(const clap_event_midi_t *midi)
{
	float *channels[2] = {left, right};
	clap_audio_buffer_t out = {channels, NULL, 2, 0, 0};
	clap_input_events_t in = {(void *)midi, in_size, in_get};
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
		capture(window);
		save_bmp(argv[2], "fm");
		memset(pushed, 0, sizeof(pushed));
		click(window, RAIL_X, RAIL_CH2_Y);
		run_ms(100);
		CHECK(pushed[0] + pushed[1] + pushed[2] == 0 && get_value(CH4_OP1_TL) == ch4_before + 10, "choosing another channel changes no knob");
		SendMessageA(window, WM_LBUTTONDBLCLK, MK_LBUTTON, MAKELPARAM(OP1_TL_X, OP1_TL_Y));
		click(window, RAIL_X, RAIL_CH4_Y);
		SendMessageA(window, WM_LBUTTONDBLCLK, MK_LBUTTON, MAKELPARAM(OP1_TL_X, OP1_TL_Y));
		run_ms(100);
		CHECK(get_value(CH4_OP1_TL) == ch4_before, "a double click returns it to the preset's value");
		click(window, POLYPHONY_X, POLYPHONY_Y);
		run_ms(100);
		const int first = get_value(0);
		for (int i = 0; i < 5; i++) {
			click(window, POLYPHONY_X, POLYPHONY_Y);
			run_ms(50);
		}
		// A quick second click reaches the window as a double click.
		SendMessageA(window, WM_LBUTTONDBLCLK, MK_LBUTTON, MAKELPARAM(POLYPHONY_X, POLYPHONY_Y));
		run_ms(100);
		CHECK(first == 1 && get_value(0) == 0, "a click steps Polyphony to Ch 1, seven clicks go round to Off");

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
