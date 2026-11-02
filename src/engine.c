#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <avrt.h>
#include "engine.h"
#include "libretro.h"
#include "resampler.h"

extern const uint8_t mdmi_blob_engine[], mdmi_blob_engine_end[];
extern const uint8_t mdmi_blob_rom[], mdmi_blob_rom_end[];

#define BOOT_FRAMES 120
#define WARM_UP_CHANNELS 10
#define WARM_UP_NOTE_FRAMES 3
#define WARM_UP_SETTLE_FRAMES 60
#define RING_FRAMES (1 << 15)
#define MIDI_QUEUE_SIZE (1 << 13)
#define MIDI_CHUNK 7
#define CORE_BATCH_FRAMES 128
#define MARGIN_SECONDS 0.005
#define RENDER_WAIT_MS 1.0
// Measured from note event to first sound: the ROM parsing MIDI and the YM2612 attack.
#define ROM_REACTION_SECONDS 0.0025
#define STALE_TEMP_FILE_SECONDS (24 * 3600)
// One-pole high-pass that removes the DC offset of the YM2612 DAC.
#define DC_BLOCK_HZ 5.0

typedef struct {
	uint64_t target;
	uint8_t length;
	uint8_t bytes[MIDI_CHUNK];
} midi_chunk;

typedef void (*set_environment_fn)(retro_environment_t);
typedef void (*set_video_refresh_fn)(retro_video_refresh_t);
typedef void (*set_audio_sample_fn)(retro_audio_sample_t);
typedef void (*set_audio_sample_batch_fn)(retro_audio_sample_batch_t);
typedef void (*set_input_poll_fn)(retro_input_poll_t);
typedef void (*set_input_state_fn)(retro_input_state_t);
typedef void (*void_fn)(void);
typedef bool (*load_game_fn)(const struct retro_game_info *);
typedef void (*get_av_info_fn)(struct retro_system_av_info *);
typedef size_t (*midi_write_fn)(const uint8_t *, size_t);
typedef void (*set_audio_slices_fn)(uint8_t);
typedef uint32_t (*has_failed_fn)(void);
typedef void (*set_lowpass_fn)(double);

struct mdmi_engine {
	HANDLE thread;
	HANDLE wake;
	HMODULE dll;
	char dll_path[MAX_PATH];
	char rom_path[MAX_PATH];

	void_fn retro_run;
	midi_write_fn midi_write;
	void_fn test_fatal;
	void_fn test_freeze;
	has_failed_fn has_failed;
	set_lowpass_fn set_lowpass;
	void_fn retro_deinit;
	double core_rate;

	// Set on the main thread while the worker is parked.
	double sample_rate;
	uint32_t midi_delay;
	uint32_t latency;
	uint32_t fill_target;
	resampler *rs;
	float dc_coef;
	float dc_in[2], dc_out[2];
	bool dc_primed;
	bool thread_started;
	double worst_run_ms;
	double worst_wait_ms;
	const char *boot_stage;
	DWORD boot_error;
	uint64_t worst_run_pos;
	uint64_t first_underrun_pos;
	uint64_t last_underrun_pos;

	uint32_t quit;
	uint32_t active;
	uint32_t parked;
	uint32_t failed;
	uint32_t booted;
	uint32_t offline;
	uint32_t discard_audio;
	uint32_t filter_hz;
	uint32_t applied_filter_hz;
	uint32_t frames_seen;
	uint32_t underruns;
	uint32_t dropped_midi;
	uint32_t last_block;

	float *ring[2];
	uint64_t ring_read;
	uint64_t ring_write;

	midi_chunk *midi_queue;
	uint32_t midi_head;
	uint32_t midi_tail;

	// Triple buffer: the worker fills frames[frame_back], publishes it by swapping it with
	// frame_shared, and the main thread swaps frame_front for frame_shared when it is new.
	mdmi_frame *frames;
	uint32_t frame_back;
	uint32_t frame_front;
	uint32_t frame_shared;
	uint32_t frames_watched;
};

#define FRAME_NEW 4
#define FRAME_INDEX 3

static DWORD tls_engine = TLS_OUT_OF_INDEXES;
static PVOID crash_handler;
static LONG global_init_count;

static mdmi_engine *current_engine(void)
{
	return tls_engine == TLS_OUT_OF_INDEXES ? NULL : TlsGetValue(tls_engine);
}

// Crashes on the worker kill only that engine: the host keeps running and it renders silence.
static LONG CALLBACK on_exception(PEXCEPTION_POINTERS info)
{
	mdmi_engine *e = current_engine();
	if (!e) {
		return EXCEPTION_CONTINUE_SEARCH;
	}
	switch (info->ExceptionRecord->ExceptionCode) {
	case EXCEPTION_ACCESS_VIOLATION:
	case EXCEPTION_ILLEGAL_INSTRUCTION:
	case EXCEPTION_PRIV_INSTRUCTION:
	case EXCEPTION_INT_DIVIDE_BY_ZERO:
	case EXCEPTION_STACK_OVERFLOW:
		__atomic_store_n(&e->failed, 1, __ATOMIC_RELEASE);
		ExitThread(1);
	default:
		return EXCEPTION_CONTINUE_SEARCH;
	}
}

bool mdmi_engine_global_init(void)
{
	// Hosts may initialize a plugin entry more than once.
	if (InterlockedIncrement(&global_init_count) > 1) {
		return true;
	}
	tls_engine = TlsAlloc();
	if (tls_engine == TLS_OUT_OF_INDEXES) {
		return false;
	}
	crash_handler = AddVectoredExceptionHandler(1, on_exception);
	return crash_handler != NULL;
}

void mdmi_engine_global_deinit(void)
{
	if (InterlockedDecrement(&global_init_count) > 0) {
		return;
	}
	if (crash_handler) {
		RemoveVectoredExceptionHandler(crash_handler);
		crash_handler = NULL;
	}
	if (tls_engine != TLS_OUT_OF_INDEXES) {
		TlsFree(tls_engine);
		tls_engine = TLS_OUT_OF_INDEXES;
	}
}

static bool env_cb(unsigned cmd, void *data)
{
	switch (cmd) {
	case RETRO_ENVIRONMENT_GET_OVERSCAN:
		// Without overscan an NTSC frame is exactly the console's 320x224.
		*(bool *)data = false;
		return true;
	case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
		return true;
	case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
		*(const char **)data = NULL;
		return false;
	default:
		return false;
	}
}

static void video_cb(const void *data, unsigned width, unsigned height, size_t pitch)
{
	mdmi_engine *e = current_engine();
	e->frames_seen++;
	if (!data || !__atomic_load_n(&e->frames_watched, __ATOMIC_ACQUIRE)) {
		return;
	}
	mdmi_frame *frame = &e->frames[e->frame_back];
	frame->width = width < MDMI_FRAME_MAX_WIDTH ? width : MDMI_FRAME_MAX_WIDTH;
	frame->height = height < MDMI_FRAME_MAX_HEIGHT ? height : MDMI_FRAME_MAX_HEIGHT;
	const uint8_t *row = data;
	for (uint32_t y = 0; y < frame->height; y++, row += pitch) {
		memcpy(frame->pixels + y * MDMI_FRAME_MAX_WIDTH, row, frame->width * sizeof(uint32_t));
	}
	e->frame_back = __atomic_exchange_n(&e->frame_shared, e->frame_back | FRAME_NEW, __ATOMIC_ACQ_REL) & FRAME_INDEX;
}

static void audio_cb(int16_t left, int16_t right)
{
	(void)left;
	(void)right;
}

static size_t audio_batch_cb(const int16_t *data, size_t frames)
{
	mdmi_engine *e = current_engine();
	if (e->discard_audio) {
		return frames;
	}
	uint64_t write = e->ring_write;
	for (size_t i = 0; i < frames; i++) {
		float in[2];
		for (int ch = 0; ch < 2; ch++) {
			float sample = data[i * 2 + ch] / 32768.0f;
			if (!e->dc_primed) {
				e->dc_in[ch] = sample;
			}
			float out = sample - e->dc_in[ch] + e->dc_coef * e->dc_out[ch];
			e->dc_in[ch] = sample;
			e->dc_out[ch] = out;
			in[ch] = out;
		}
		e->dc_primed = true;
		float out_l[8], out_r[8];
		int count = resampler_push(e->rs, in[0], in[1], out_l, out_r, 8);
		for (int n = 0; n < count; n++) {
			e->ring[0][write % RING_FRAMES] = out_l[n];
			e->ring[1][write % RING_FRAMES] = out_r[n];
			write++;
		}
	}
	__atomic_store_n(&e->ring_write, write, __ATOMIC_RELEASE);
	return frames;
}

static void input_poll_cb(void) {}

static int16_t input_state_cb(unsigned port, unsigned device, unsigned index, unsigned id)
{
	(void)port;
	(void)device;
	(void)index;
	(void)id;
	return 0;
}

static bool write_file(const char *path, const uint8_t *data, size_t size)
{
	FILE *f = fopen(path, "wb");
	if (!f) {
		return false;
	}
	bool ok = fwrite(data, 1, size, f) == size;
	return fclose(f) == 0 && ok;
}

// Leftovers from crashed hosts. Loaded DLLs refuse deletion, and the age check protects
// a ROM another process has written but not opened yet.
static void clean_stale_temp_files(const char *dir)
{
	char pattern[MAX_PATH];
	snprintf(pattern, sizeof(pattern), "%s*", dir);
	WIN32_FIND_DATAA found;
	HANDLE find = FindFirstFileA(pattern, &found);
	if (find == INVALID_HANDLE_VALUE) {
		return;
	}
	FILETIME now_ft;
	GetSystemTimeAsFileTime(&now_ft);
	ULARGE_INTEGER now = {.LowPart = now_ft.dwLowDateTime, .HighPart = now_ft.dwHighDateTime};
	do {
		ULARGE_INTEGER written = {.LowPart = found.ftLastWriteTime.dwLowDateTime, .HighPart = found.ftLastWriteTime.dwHighDateTime};
		bool stale = now.QuadPart - written.QuadPart > (uint64_t)STALE_TEMP_FILE_SECONDS * 10000000ULL;
		if (stale && !(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
			char path[MAX_PATH];
			snprintf(path, sizeof(path), "%s%s", dir, found.cFileName);
			DeleteFileA(path);
		}
	} while (FindNextFileA(find, &found));
	FindClose(find);
}

static bool unpack_files(mdmi_engine *e)
{
	char dir[MAX_PATH];
	char temp[MAX_PATH];
	if (!GetTempPathA(sizeof(temp), temp)) {
		return false;
	}
	snprintf(dir, sizeof(dir), "%sMDMI-Plugin\\", temp);
	CreateDirectoryA(dir, NULL);
	clean_stale_temp_files(dir);

	// This runs on the engine's own thread, whose id no other engine has, whichever plugin file
	// of this process it comes from.
	snprintf(e->dll_path, sizeof(e->dll_path), "%sengine-%lu-%lu.dll", dir, GetCurrentProcessId(), GetCurrentThreadId());
	snprintf(e->rom_path, sizeof(e->rom_path), "%srom-%lu-%lu.bin", dir, GetCurrentProcessId(), GetCurrentThreadId());
	return write_file(e->dll_path, mdmi_blob_engine, (size_t)(mdmi_blob_engine_end - mdmi_blob_engine))
		&& write_file(e->rom_path, mdmi_blob_rom, (size_t)(mdmi_blob_rom_end - mdmi_blob_rom));
}

#define LOAD(var, type, name) \
	type var = (type)GetProcAddress(e->dll, name); \
	if (!var) { return false; }

static void run_frames(mdmi_engine *e, uint32_t frames)
{
	uint32_t target = e->frames_seen + frames;
	while (e->frames_seen < target) {
		e->retro_run();
	}
}

// The JIT compiles ROM code on first use, which can stall the emulation thread long enough
// to drop audio. Playing every channel during boot moves that compile out of the first real notes.
static void warm_up(mdmi_engine *e)
{
	for (uint8_t channel = 0; channel < WARM_UP_CHANNELS; channel++) {
		const uint8_t note_on[3] = {(uint8_t)(0x90 | channel), 60, 100};
		const uint8_t bend[3] = {(uint8_t)(0xE0 | channel), 0, 0x50};
		const uint8_t bend_center[3] = {(uint8_t)(0xE0 | channel), 0, 0x40};
		const uint8_t note_off[3] = {(uint8_t)(0x80 | channel), 60, 0};
		e->midi_write(note_on, sizeof(note_on));
		e->midi_write(bend, sizeof(bend));
		run_frames(e, WARM_UP_NOTE_FRAMES);
		e->midi_write(bend_center, sizeof(bend_center));
		e->midi_write(note_off, sizeof(note_off));
		run_frames(e, WARM_UP_NOTE_FRAMES);
	}
	run_frames(e, WARM_UP_SETTLE_FRAMES);
}

static double now_ms(void)
{
	LARGE_INTEGER freq, count;
	QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&count);
	return (double)count.QuadPart * 1000.0 / (double)freq.QuadPart;
}

static bool load_core(mdmi_engine *e)
{
	double t0 = now_ms();
	if (!unpack_files(e)) {
		e->boot_error = GetLastError();
		e->boot_stage = "unpack";
		return false;
	}
	double t1 = now_ms();
	e->dll = LoadLibraryA(e->dll_path);
	double t2 = now_ms();
	if (!e->dll) {
		e->boot_error = GetLastError();
		e->boot_stage = "loadlibrary";
		return false;
	}
	e->boot_stage = "core";
	LOAD(set_environment, set_environment_fn, "retro_set_environment")
	LOAD(set_video, set_video_refresh_fn, "retro_set_video_refresh")
	LOAD(set_sample, set_audio_sample_fn, "retro_set_audio_sample")
	LOAD(set_batch, set_audio_sample_batch_fn, "retro_set_audio_sample_batch")
	LOAD(set_poll, set_input_poll_fn, "retro_set_input_poll")
	LOAD(set_state, set_input_state_fn, "retro_set_input_state")
	LOAD(retro_init, void_fn, "retro_init")
	LOAD(load_game, load_game_fn, "retro_load_game")
	LOAD(get_av, get_av_info_fn, "retro_get_system_av_info")
	LOAD(set_slices, set_audio_slices_fn, "mdmi_set_audio_slices")
	LOAD(run, void_fn, "retro_run")
	LOAD(midi_write, midi_write_fn, "mdmi_midi_write")
	LOAD(has_failed, has_failed_fn, "mdmi_has_failed")
	LOAD(test_fatal, void_fn, "mdmi_test_fatal")
	LOAD(test_freeze, void_fn, "mdmi_test_freeze")
	LOAD(deinit, void_fn, "retro_deinit")
	LOAD(set_lowpass, set_lowpass_fn, "mdmi_set_lowpass")
	e->retro_run = run;
	e->midi_write = midi_write;
	e->has_failed = has_failed;
	e->test_fatal = test_fatal;
	e->test_freeze = test_freeze;
	e->retro_deinit = deinit;
	e->set_lowpass = set_lowpass;

	set_environment(env_cb);
	set_video(video_cb);
	set_sample(audio_cb);
	set_batch(audio_batch_cb);
	set_poll(input_poll_cb);
	set_state(input_state_cb);
	retro_init();

	struct retro_game_info game = {.path = e->rom_path};
	e->boot_stage = "load_game";
	if (!load_game(&game)) {
		return false;
	}
	struct retro_system_av_info av;
	get_av(&av);
	e->core_rate = av.timing.sample_rate;
	double t3 = now_ms();

	e->boot_stage = "frames";
	// Run through the ROM's startup with audio thrown away, then switch to fine-grained slices.
	e->discard_audio = 1;
	run_frames(e, BOOT_FRAMES);
	warm_up(e);
	e->discard_audio = 0;
	set_slices(1);
	if (getenv("MDMI_PLUGIN_STATS")) {
		fprintf(stderr, "boot unpack=%.0fms loadlibrary=%.0fms load_game=%.0fms frames=%.0fms\n", t1 - t0, t2 - t1, t3 - t2, now_ms() - t3);
	}
	__atomic_store_n(&e->booted, 1, __ATOMIC_RELEASE);
	return true;
}

static void pump_midi(mdmi_engine *e, uint64_t position)
{
	uint32_t tail = e->midi_tail;
	uint32_t head = __atomic_load_n(&e->midi_head, __ATOMIC_ACQUIRE);
	while (tail != head) {
		const midi_chunk *chunk = &e->midi_queue[tail % MIDI_QUEUE_SIZE];
		if (chunk->target > position) {
			break;
		}
		e->midi_write(chunk->bytes, chunk->length);
		tail++;
	}
	__atomic_store_n(&e->midi_tail, tail, __ATOMIC_RELEASE);
}

static DWORD WINAPI worker_main(LPVOID param)
{
	mdmi_engine *e = param;
	TlsSetValue(tls_engine, e);
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

	if (!load_core(e)) {
		__atomic_store_n(&e->failed, 1, __ATOMIC_RELEASE);
		return 1;
	}
	const char *crash_test = getenv("MDMI_TEST_CRASH");
	if (crash_test && !strcmp(crash_test, "fatal")) {
		e->test_fatal();
	} else if (crash_test && !strcmp(crash_test, "freeze")) {
		e->test_freeze();
	} else if (crash_test && !strcmp(crash_test, "segv")) {
		*(volatile int *)NULL = 1;
	}

	// Same scheduling class as the DAW's audio thread: at normal priorities the OS can preempt
	// this thread for longer than the buffer margin. Boot stays outside it, as it is CPU-heavy.
	DWORD mmcss_task = 0;
	HANDLE mmcss = AvSetMmThreadCharacteristicsA("Pro Audio", &mmcss_task);

	while (!__atomic_load_n(&e->quit, __ATOMIC_ACQUIRE)) {
		if (!__atomic_load_n(&e->active, __ATOMIC_ACQUIRE)) {
			__atomic_store_n(&e->parked, 1, __ATOMIC_RELEASE);
			WaitForSingleObject(e->wake, INFINITE);
			continue;
		}
		uint32_t filter_hz = __atomic_load_n(&e->filter_hz, __ATOMIC_ACQUIRE);
		if (filter_hz && filter_hz != e->applied_filter_hz) {
			e->set_lowpass(filter_hz);
			e->applied_filter_hz = filter_hz;
		}
		uint64_t write = e->ring_write;
		pump_midi(e, write);
		uint64_t read = __atomic_load_n(&e->ring_read, __ATOMIC_ACQUIRE);
		if (write - read >= e->fill_target) {
			double wait_start = now_ms();
			WaitForSingleObject(e->wake, 1);
			double wait_ms = now_ms() - wait_start;
			if (wait_ms > e->worst_wait_ms) {
				e->worst_wait_ms = wait_ms;
			}
			continue;
		}
		double run_start = now_ms();
		e->retro_run();
		double run_ms = now_ms() - run_start;
		if (run_ms > e->worst_run_ms) {
			e->worst_run_ms = run_ms;
			e->worst_run_pos = write;
		}
		if (e->has_failed()) {
			__atomic_store_n(&e->failed, 1, __ATOMIC_RELEASE);
			return 1;
		}
	}
	if (mmcss) {
		AvRevertMmThreadCharacteristics(mmcss);
	}
	e->retro_deinit();
	return 0;
}

mdmi_engine *mdmi_engine_create(void)
{
	mdmi_engine *e = calloc(1, sizeof(*e));
	if (!e) {
		return NULL;
	}
	e->parked = 1;
	e->wake = CreateEventA(NULL, FALSE, FALSE, NULL);
	e->rs = malloc(sizeof(resampler));
	e->ring[0] = calloc(RING_FRAMES, sizeof(float));
	e->ring[1] = calloc(RING_FRAMES, sizeof(float));
	e->midi_queue = calloc(MIDI_QUEUE_SIZE, sizeof(midi_chunk));
	e->frames = calloc(3, sizeof(mdmi_frame));
	if (!e->wake || !e->rs || !e->ring[0] || !e->ring[1] || !e->midi_queue || !e->frames) {
		mdmi_engine_destroy(e);
		return NULL;
	}
	e->frame_front = 0;
	e->frame_back = 1;
	e->frame_shared = 2;
	return e;
}

static bool worker_dead(const mdmi_engine *e)
{
	return !e->thread_started || __atomic_load_n(&e->failed, __ATOMIC_ACQUIRE)
		|| WaitForSingleObject(e->thread, 0) == WAIT_OBJECT_0;
}

void mdmi_engine_destroy(mdmi_engine *e)
{
	if (!e) {
		return;
	}
	bool clean_exit = true;
	if (e->thread_started) {
		__atomic_store_n(&e->quit, 1, __ATOMIC_RELEASE);
		SetEvent(e->wake);
		WaitForSingleObject(e->thread, 5000);
		DWORD code = STILL_ACTIVE;
		GetExitCodeThread(e->thread, &code);
		clean_exit = code == 0;
		if (code != STILL_ACTIVE) {
			CloseHandle(e->thread);
		}
	}
	// A core that died mid-run may have code still mapped or locks held, so it stays loaded.
	if (clean_exit) {
		if (e->dll) {
			FreeLibrary(e->dll);
		}
		if (e->dll_path[0]) {
			DeleteFileA(e->dll_path);
		}
	}
	if (e->rom_path[0]) {
		DeleteFileA(e->rom_path);
	}
	if (clean_exit) {
		if (e->wake) {
			CloseHandle(e->wake);
		}
		free(e->rs);
		free(e->ring[0]);
		free(e->ring[1]);
		free(e->midi_queue);
		free(e->frames);
		free(e);
	}
}

bool mdmi_engine_activate(mdmi_engine *e, double sample_rate, uint32_t max_block)
{
	if (e->thread_started && worker_dead(e)) {
		return false;
	}
	// The core's rate is constant for an NTSC machine; the worker confirms it while booting.
	const double core_rate = 53693175.0 / (7 * 6 * 24);
	e->sample_rate = sample_rate;
	resampler_init(e->rs, core_rate, sample_rate);
	e->dc_coef = (float)(1.0 - 2.0 * 3.14159265358979 * DC_BLOCK_HZ / core_rate);
	e->dc_primed = false;
	e->dc_out[0] = e->dc_out[1] = 0;

	uint32_t batch = (uint32_t)(CORE_BATCH_FRAMES * sample_rate / core_rate) + 2;
	uint32_t margin = (uint32_t)(sample_rate * MARGIN_SECONDS);
	e->fill_target = max_block + margin;
	e->midi_delay = e->fill_target + batch;
	e->latency = e->midi_delay + (uint32_t)(sample_rate * ROM_REACTION_SECONDS);

	e->ring_read = e->ring_write = 0;
	e->midi_head = e->midi_tail = 0;
	e->parked = 0;
	__atomic_store_n(&e->active, 1, __ATOMIC_RELEASE);

	if (!e->thread_started) {
		e->thread = CreateThread(NULL, 0, worker_main, e, 0, NULL);
		if (!e->thread) {
			e->active = 0;
			e->parked = 1;
			return false;
		}
		e->thread_started = true;
	} else {
		SetEvent(e->wake);
	}
	return true;
}

void mdmi_engine_deactivate(mdmi_engine *e)
{
	__atomic_store_n(&e->active, 0, __ATOMIC_RELEASE);
	if (!e->thread_started) {
		e->parked = 1;
		return;
	}
	SetEvent(e->wake);
	while (!worker_dead(e) && !__atomic_load_n(&e->parked, __ATOMIC_ACQUIRE)) {
		Sleep(1);
	}
	// The queue restarts on activation, so what the ROM was still due to receive goes in now.
	// Nothing plays while parked, so silence held notes for when the engine comes back.
	if (e->booted && !worker_dead(e)) {
		pump_midi(e, UINT64_MAX);
		for (uint8_t channel = 0; channel < 16; channel++) {
			const uint8_t all_notes_off[3] = {(uint8_t)(0xB0 | channel), 123, 0};
			e->midi_write(all_notes_off, sizeof(all_notes_off));
		}
	}
}

uint32_t mdmi_engine_latency(const mdmi_engine *e)
{
	return e->latency;
}

void mdmi_engine_set_filter(mdmi_engine *e, double cutoff_hz)
{
	__atomic_store_n(&e->filter_hz, (uint32_t)cutoff_hz, __ATOMIC_RELEASE);
}

void mdmi_engine_set_offline(mdmi_engine *e, bool offline)
{
	__atomic_store_n(&e->offline, offline, __ATOMIC_RELEASE);
}

mdmi_engine_stats mdmi_engine_get_stats(const mdmi_engine *e)
{
	mdmi_engine_stats stats = {
		.underruns = __atomic_load_n(&e->underruns, __ATOMIC_RELAXED),
		.dropped_midi = __atomic_load_n(&e->dropped_midi, __ATOMIC_RELAXED),
		.failed = e->thread_started && worker_dead(e) && !__atomic_load_n(&e->quit, __ATOMIC_ACQUIRE),
		.booted = __atomic_load_n(&e->booted, __ATOMIC_ACQUIRE) != 0,
		.worst_run_ms = e->worst_run_ms,
		.worst_wait_ms = e->worst_wait_ms,
		.boot_stage = e->boot_stage,
		.boot_error = e->boot_error,
		.worst_run_pos = e->worst_run_pos,
		.first_underrun_pos = e->first_underrun_pos,
		.last_underrun_pos = e->last_underrun_pos,
		.active = __atomic_load_n(&e->active, __ATOMIC_ACQUIRE) != 0,
		.last_block = __atomic_load_n(&e->last_block, __ATOMIC_RELAXED),
		.sample_rate = (uint32_t)e->sample_rate,
	};
	return stats;
}

void mdmi_engine_watch_frames(mdmi_engine *e, bool watching)
{
	__atomic_store_n(&e->frames_watched, watching, __ATOMIC_RELEASE);
}

const mdmi_frame *mdmi_engine_latest_frame(mdmi_engine *e)
{
	if (__atomic_load_n(&e->frame_shared, __ATOMIC_ACQUIRE) & FRAME_NEW) {
		e->frame_front = __atomic_exchange_n(&e->frame_shared, e->frame_front, __ATOMIC_ACQ_REL) & FRAME_INDEX;
	}
	return &e->frames[e->frame_front];
}

void mdmi_engine_midi(mdmi_engine *e, uint32_t sample_offset, const uint8_t *bytes, size_t length)
{
	uint64_t target = __atomic_load_n(&e->ring_read, __ATOMIC_RELAXED) + sample_offset + e->midi_delay;
	uint32_t head = e->midi_head;
	uint32_t tail = __atomic_load_n(&e->midi_tail, __ATOMIC_ACQUIRE);
	size_t chunks = (length + MIDI_CHUNK - 1) / MIDI_CHUNK;
	if (MIDI_QUEUE_SIZE - (head - tail) < chunks) {
		__atomic_fetch_add(&e->dropped_midi, 1, __ATOMIC_RELAXED);
		return;
	}
	while (length) {
		midi_chunk *chunk = &e->midi_queue[head % MIDI_QUEUE_SIZE];
		size_t count = length < MIDI_CHUNK ? length : MIDI_CHUNK;
		chunk->target = target;
		chunk->length = (uint8_t)count;
		memcpy(chunk->bytes, bytes, count);
		bytes += count;
		length -= count;
		head++;
	}
	__atomic_store_n(&e->midi_head, head, __ATOMIC_RELEASE);
}

void mdmi_engine_all_notes_off(mdmi_engine *e, uint32_t sample_offset)
{
	for (uint8_t channel = 0; channel < 16; channel++) {
		const uint8_t message[3] = {(uint8_t)(0xB0 | channel), 123, 0};
		mdmi_engine_midi(e, sample_offset, message, sizeof(message));
	}
}

void mdmi_engine_render(mdmi_engine *e, float *left, float *right, uint32_t frames)
{
	uint64_t read = e->ring_read;
	uint64_t available = __atomic_load_n(&e->ring_write, __ATOMIC_ACQUIRE) - read;
	SetEvent(e->wake);
	__atomic_store_n(&e->last_block, frames, __ATOMIC_RELAXED);

	bool offline = __atomic_load_n(&e->offline, __ATOMIC_ACQUIRE) != 0;
	if (available < frames && (offline || __atomic_load_n(&e->booted, __ATOMIC_ACQUIRE))) {
		// Offline rendering has no deadline. In real time a host with a large device buffer calls
		// several blocks in a row, ahead of its deadline, so a short wait beats dropping audio.
		double give_up = now_ms() + RENDER_WAIT_MS;
		while (available < frames && !worker_dead(e) && (offline || now_ms() < give_up)) {
			SwitchToThread();
			available = __atomic_load_n(&e->ring_write, __ATOMIC_ACQUIRE) - read;
		}
	}

	uint32_t count = available < frames ? (uint32_t)available : frames;
	if (count < frames && __atomic_load_n(&e->booted, __ATOMIC_ACQUIRE) && !__atomic_load_n(&e->failed, __ATOMIC_ACQUIRE) && read) {
		if (!__atomic_fetch_add(&e->underruns, 1, __ATOMIC_RELAXED)) {
			e->first_underrun_pos = read;
		}
		e->last_underrun_pos = read;
	}
	for (uint32_t i = 0; i < count; i++) {
		left[i] = e->ring[0][(read + i) % RING_FRAMES];
		right[i] = e->ring[1][(read + i) % RING_FRAMES];
	}
	for (uint32_t i = count; i < frames; i++) {
		left[i] = right[i] = 0;
	}
	__atomic_store_n(&e->ring_read, read + count, __ATOMIC_RELEASE);
	SetEvent(e->wake);
}
