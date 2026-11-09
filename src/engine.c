#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <avrt.h>
#include <pmmintrin.h>
#include <xmmintrin.h>
#include "engine.h"
#include "libretro.h"
#include "resampler.h"
#include "rom_image.h"

extern const uint8_t mdmi_blob_engine[], mdmi_blob_engine_end[];
extern const uint8_t mdmi_blob_rom[], mdmi_blob_rom_end[];

#define BOOT_FRAMES 120
#define WARM_UP_CHANNELS 10
#define WARM_UP_NOTE_FRAMES 3
#define WARM_UP_SETTLE_FRAMES 60
// The ring holds what the worker may write ahead of the reader, sized from the block at activation.
#define RING_MIN_FRAMES 4096
#define MIDI_QUEUE_SIZE (1 << 13)
#define MIDI_CHUNK 7
#define CORE_BATCH_FRAMES 128
#define MARGIN_SECONDS 0.005
// How long a block alone waits for the worker. A block of a burst, which the host renders ahead of its
// deadline, waits up to half its own length instead, if that is longer (D14).
#define RENDER_WAIT_MS 1.0
// A block that starts within this share of its own length after the last one ended is in a burst.
#define BURST_GAP_SHARE 0.5
// Measured from note event to first sound: the ROM parsing MIDI and the YM2612 attack.
#define ROM_REACTION_SECONDS 0.0025
#define STALE_TEMP_FILE_SECONDS (24 * 3600)
// One-pole high-pass that removes the DC offset of the YM2612 DAC.
#define DC_BLOCK_HZ 5.0
// The core's rate is constant for an NTSC machine.
#define CORE_RATE (53693175.0 / (7 * 6 * 24))
// The ring: Main, then each output (stage 17), left and right.
#define RING_CHANNELS (2 + 2 * MDMI_OUTPUTS)
#define OUT_CHANNELS (2 * MDMI_OUTPUTS)
#define NO_OWNER -1
// An owner whose voice hasn't keyed on in this long counts as a miss.
#define OWNER_STUCK_SECONDS 1.0

// An image for the next boot, handed from the main thread to the worker.
typedef struct {
	size_t size;
	uint8_t bytes[];
} rom_image;

enum { CHUNK_MIDI, CHUNK_OWNER, CHUNK_OWNER_NOW };

// A piece of a MIDI message, or which channel a voice's next note belongs to (CHUNK_OWNER: bytes
// hold the voice and the channel). Both go in one queue so they keep their order.
typedef struct {
	uint64_t target;
	uint8_t kind;
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
typedef size_t (*cart_compare_fn)(const uint8_t *, size_t, size_t, size_t);
typedef void (*write_ram_fn)(size_t, const uint8_t *, size_t);
typedef void (*set_lowpass_fn)(double);
typedef void (*voice_batch_fn)(const float *, const uint16_t *, size_t);
typedef void (*set_voice_batch_fn)(voice_batch_fn);
typedef uint8_t (*test_ym_read_fn)(uint8_t, uint8_t);
typedef uint8_t (*test_psg_volume_fn)(uint8_t);
typedef uint16_t (*test_psg_tone_fn)(uint8_t);

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
	test_ym_read_fn test_ym_read;
	test_psg_volume_fn test_psg_volume;
	test_psg_tone_fn test_psg_tone;
	cart_compare_fn cart_differs;
	cart_compare_fn write_cart;
	write_ram_fn write_ram;
	set_lowpass_fn set_lowpass;
	void_fn retro_deinit;

	// Set on the main thread while the worker is parked.
	double sample_rate;
	uint32_t midi_delay;
	uint32_t latency;
	uint32_t fill_target;
	resampler *rs;
	float dc_coef;
	float dc_in[MDMI_VOICES * 2], dc_out[MDMI_VOICES * 2];
	bool dc_primed;
	// The worker's: the core's voices of the batch it is about to mix, and which output each voice
	// plays into (D46). A pending owner takes the voice at its next key-on.
	float voices[CORE_BATCH_FRAMES * MDMI_VOICES * 2];
	uint16_t keyon[CORE_BATCH_FRAMES];
	uint32_t voice_frames;
	int8_t owner[MDMI_VOICES];
	int8_t pending_owner[MDMI_VOICES];
	uint64_t pending_since[MDMI_VOICES];
	uint64_t core_frames;
	uint32_t owner_misses;
	float owner_wait_ms;
	float owner_wait_round_ms;
	uint32_t voice_batch_misses;
	float voice_mix_error;
	float mix_diff_in[2], mix_diff_out[2];
	bool thread_started;
	double worst_run_ms;
	double worst_wait_ms;
	// Set by the main thread, taken by the worker: the core boots this image next (stage 12).
	rom_image *next_rom;
	uint32_t boots;
	double last_boot_ms;
	// Images written into the running core instead of a restart (D39).
	uint32_t patches;
	double last_patch_ms;
	const char *boot_stage;
	DWORD boot_error;
	uint64_t worst_run_pos;
	uint64_t first_underrun_pos;
	uint64_t last_underrun_pos;

	uint32_t quit;
	uint32_t active;
	// Deactivation asks with a new number and waits until the worker, parked, answers with it:
	// a flag the main thread also wrote could be read back stale while the worker still ran (audit 1.8).
	uint32_t park_request;
	uint32_t park_answer;
	uint32_t failed;
	uint32_t booted;
	uint32_t offline;
	// Tests only: the worker computes no further than the last block render asked for.
	uint32_t lockstep;
	uint64_t lockstep_target;
	uint32_t discard_audio;
	uint32_t filter_hz;
	uint32_t applied_filter_hz;
	uint32_t frames_seen;
	uint32_t underruns;
	uint32_t dropped_midi;
	uint32_t last_block;
	uint32_t worst_short;
	uint32_t burst;
	uint32_t longest_burst;
	double last_render_end_ms;

	float *ring[RING_CHANNELS];
	uint32_t ring_mask;
	uint64_t ring_read;
	uint64_t ring_write;

	// Journal (stage 13): the render thread bumps these, the worker takes them once a second and
	// writes one line. Off unless asked for, see journal_open.
	FILE *journal;
	double journal_start_ms;
	double journal_next_ms;
	uint32_t j_blocks;
	uint32_t j_block_min;
	uint32_t j_block_max;
	uint32_t j_burst_max;
	uint32_t j_gap_max_us;
	uint32_t j_avail_min;
	uint32_t j_waits;
	uint32_t j_wait_us;
	uint32_t j_wait_max_us;
	uint32_t j_short_blocks;
	uint32_t j_short_samples;
	uint32_t j_burst_short_max;
	uint32_t j_notes;
	uint32_t j_playing;
	// The render thread's running sum of what the current burst lacked.
	uint32_t burst_short;
	// The worker's own.
	uint32_t j_runs;
	double j_run_ms;
	double j_run_max_ms;
	double j_worker_wait_ms;
	uint64_t j_samples_base;
	ULONG64 j_cycles_base;
	uint32_t j_cpu;
	uint32_t j_cpu_moves;
	// Runs and their time on the slowest class of cores of a hybrid processor.
	uint32_t j_slow_runs;
	double j_slow_run_ms;

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

// The core hands over its voices just before the mix of the same frames.
static void voice_batch_cb(const float *voices, const uint16_t *keyon, size_t frames)
{
	mdmi_engine *e = current_engine();
	if (frames > CORE_BATCH_FRAMES) {
		frames = CORE_BATCH_FRAMES;
	}
	memcpy(e->voices, voices, frames * MDMI_VOICES * 2 * sizeof(float));
	memcpy(e->keyon, keyon, frames * sizeof(uint16_t));
	e->voice_frames = (uint32_t)frames;
}

static void reset_owners(mdmi_engine *e)
{
	for (int voice = 0; voice < MDMI_VOICES; voice++) {
		e->owner[voice] = (int8_t)voice;
		e->pending_owner[voice] = NO_OWNER;
	}
}

// Every voice plays into the output of the channel whose note it last started (D46): a release tail
// stays with its channel until another channel's note keys the voice on. Main is the outputs' sum, so
// the outputs add up to it exactly. The core's own mix only checks that the voices add up to it.
static size_t audio_batch_cb(const int16_t *data, size_t frames)
{
	mdmi_engine *e = current_engine();
	const bool have_voices = e->voice_frames == frames;
	e->voice_frames = 0;
	if (e->discard_audio) {
		return frames;
	}
	if (!have_voices) {
		// Both sides make batches of 128 frames; a batch without its voices would play silence.
		e->voice_batch_misses++;
	}
	// The core's int16 mix is its float mix * 0x7FFF; the plugin's scale is int16 / 32768.
	const float scale = 32767.0f / 32768.0f;
	const uint64_t stuck_frames = (uint64_t)(OWNER_STUCK_SECONDS * CORE_RATE);
	uint64_t write = e->ring_write;
	for (size_t i = 0; i < frames; i++, e->core_frames++) {
		float in[OUT_CHANNELS] = {0};
		if (have_voices) {
			const uint16_t keyon = e->keyon[i];
			const float *voice = e->voices + i * MDMI_VOICES * 2;
			float sum[2] = {0, 0};
			for (int v = 0; v < MDMI_VOICES; v++) {
				if (e->pending_owner[v] != NO_OWNER && keyon >> v & 1) {
					const float waited_ms = (float)((e->core_frames - e->pending_since[v]) * 1000.0 / CORE_RATE);
					e->owner_wait_ms = waited_ms > e->owner_wait_ms ? waited_ms : e->owner_wait_ms;
					e->owner_wait_round_ms = waited_ms > e->owner_wait_round_ms ? waited_ms : e->owner_wait_round_ms;
					e->owner[v] = e->pending_owner[v];
					e->pending_owner[v] = NO_OWNER;
				} else if (e->pending_owner[v] != NO_OWNER && e->core_frames - e->pending_since[v] == stuck_frames) {
					// Far longer than the ROM ever takes, even behind a whole resend after a restart:
					// the voice the dealer named isn't the one the ROM keyed on.
					e->owner_misses++;
				}
				// Each voice has its DC blocker before it goes to an output: a silent FM voice keeps
				// the YM2612's ladder offset, which would otherwise step from one output to another
				// when the voice changes owner.
				const int out = e->owner[v];
				for (int ch = 0; ch < 2; ch++) {
					const int k = v * 2 + ch;
					const float sample = voice[k] * scale;
					if (!e->dc_primed) {
						e->dc_in[k] = sample;
					}
					const float blocked = sample - e->dc_in[k] + e->dc_coef * e->dc_out[k];
					e->dc_in[k] = sample;
					e->dc_out[k] = blocked;
					in[out * 2 + ch] += blocked;
				}
				sum[0] += voice[v * 2];
				sum[1] += voice[v * 2 + 1];
			}
			// The core's lowpass rounds down in its feedback, which leaves it a slow offset the
			// voices, in float, don't have; the DC blocker takes that from both, so compare after it.
			for (int ch = 0; ch < 2; ch++) {
				const float diff = sum[ch] * 32767.0f - data[i * 2 + ch];
				const float error = diff - e->mix_diff_in[ch] + e->dc_coef * e->mix_diff_out[ch];
				e->mix_diff_in[ch] = diff;
				e->mix_diff_out[ch] = error;
				if (e->dc_primed && fabsf(error) > e->voice_mix_error) {
					e->voice_mix_error = fabsf(error);
				}
			}
		}
		e->dc_primed = e->dc_primed || have_voices;
		float out[8 * OUT_CHANNELS];
		int count = resampler_push(e->rs, in, out, 8);
		for (int n = 0; n < count; n++) {
			const float *frame = out + n * OUT_CHANNELS;
			float main_l = 0, main_r = 0;
			for (int o = 0; o < MDMI_OUTPUTS; o++) {
				main_l += frame[o * 2];
				main_r += frame[o * 2 + 1];
				e->ring[2 + o * 2][write & e->ring_mask] = frame[o * 2];
				e->ring[3 + o * 2][write & e->ring_mask] = frame[o * 2 + 1];
			}
			e->ring[0][write & e->ring_mask] = main_l;
			e->ring[1][write & e->ring_mask] = main_r;
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
	snprintf(dir, sizeof(dir), "%sMDMI-Slot\\", temp);
	CreateDirectoryA(dir, NULL);
	clean_stale_temp_files(dir);

	// This runs on the engine's own thread, whose id no other engine has, whichever plugin file
	// of this process it comes from.
	snprintf(e->dll_path, sizeof(e->dll_path), "%sengine-%lu-%lu.dll", dir, GetCurrentProcessId(), GetCurrentThreadId());
	snprintf(e->rom_path, sizeof(e->rom_path), "%srom-%lu-%lu.bin", dir, GetCurrentProcessId(), GetCurrentThreadId());
	return write_file(e->dll_path, mdmi_blob_engine, (size_t)(mdmi_blob_engine_end - mdmi_blob_engine));
}

// The image handed over last, else the built-in one on the first boot.
static bool write_rom(mdmi_engine *e)
{
	rom_image *image = __atomic_exchange_n(&e->next_rom, NULL, __ATOMIC_ACQ_REL);
	if (!image) {
		return e->boots == 0 && write_file(e->rom_path, mdmi_blob_rom, (size_t)(mdmi_blob_rom_end - mdmi_blob_rom));
	}
	bool ok = write_file(e->rom_path, image->bytes, image->size);
	free(image);
	return ok;
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
	// Decaying voices, DC blockers and filters reach denormal floats, which cost a hundred times a
	// normal one: reg_test ran 4 times as long and six real-time instances fell behind (stage 17).
	// Set at every load, in case a core's start resets the thread's float state.
	_MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
	_MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
	double t0 = now_ms();
	// A restart keeps the DLL file it already has.
	if (!(e->dll_path[0] || unpack_files(e)) || !write_rom(e)) {
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
	LOAD(test_ym_read, test_ym_read_fn, "mdmi_test_ym_read")
	LOAD(test_psg_volume, test_psg_volume_fn, "mdmi_test_psg_volume")
	LOAD(test_psg_tone, test_psg_tone_fn, "mdmi_test_psg_tone")
	LOAD(cart_differs, cart_compare_fn, "mdmi_cart_differs")
	LOAD(write_cart, cart_compare_fn, "mdmi_write_cart")
	LOAD(write_ram, write_ram_fn, "mdmi_write_ram")
	LOAD(deinit, void_fn, "retro_deinit")
	LOAD(set_lowpass, set_lowpass_fn, "mdmi_set_lowpass")
	LOAD(set_voice_batch, set_voice_batch_fn, "mdmi_set_voice_batch")
	e->retro_run = run;
	e->midi_write = midi_write;
	e->has_failed = has_failed;
	e->test_ym_read = test_ym_read;
	e->test_psg_volume = test_psg_volume;
	e->test_psg_tone = test_psg_tone;
	e->cart_differs = cart_differs;
	e->write_cart = write_cart;
	e->write_ram = write_ram;
	e->test_fatal = test_fatal;
	e->test_freeze = test_freeze;
	e->retro_deinit = deinit;
	e->set_lowpass = set_lowpass;

	set_environment(env_cb);
	set_video(video_cb);
	set_sample(audio_cb);
	set_batch(audio_batch_cb);
	set_voice_batch(voice_batch_cb);
	reset_owners(e);
	set_poll(input_poll_cb);
	set_state(input_state_cb);
	retro_init();

	struct retro_game_info game = {.path = e->rom_path};
	e->boot_stage = "load_game";
	if (!load_game(&game)) {
		return false;
	}
	// Not for its numbers: BlastEm sets up its audio output at its own rate in this call.
	struct retro_system_av_info av;
	get_av(&av);
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
	// What was queued for the ROM before a restart was meant for the one that went: the plugin
	// sends everything again once it sees the new boot.
	if (e->boots) {
		__atomic_store_n(&e->midi_tail, __atomic_load_n(&e->midi_head, __ATOMIC_ACQUIRE), __ATOMIC_RELEASE);
	}
	e->last_boot_ms = now_ms() - t0;
	__atomic_store_n(&e->booted, 1, __ATOMIC_RELEASE);
	__atomic_add_fetch(&e->boots, 1, __ATOMIC_ACQ_REL);
	return true;
}

// An image that differs from the running cartridge only where the samples are goes straight into
// it: the cartridge's bytes and the table the ROM copied to RAM. The next note plays it, nothing else
// stops (D39). False if it differs elsewhere, and then the core restarts.
static bool patch_core(mdmi_engine *e, const rom_image *image)
{
	uint32_t table, table_ram, records;
	rom_image_zone(&table, &table_ram, &records);
	if (image->size > ROM_IMAGE_MAX || image->size < records) {
		return false;
	}
	const double start = now_ms();
	if (e->cart_differs(image->bytes, image->size, 0, table) || e->cart_differs(image->bytes, image->size, table + ROM_IMAGE_TABLE_BYTES, records)) {
		return false;
	}
	e->write_cart(image->bytes, image->size, records, ROM_IMAGE_MAX);
	e->write_cart(image->bytes, image->size, table, table + ROM_IMAGE_TABLE_BYTES);
	e->write_ram(table_ram, image->bytes + table, ROM_IMAGE_TABLE_BYTES);
	e->last_patch_ms = now_ms() - start;
	__atomic_add_fetch(&e->patches, 1, __ATOMIC_ACQ_REL);
	return true;
}

// The same core boots another image: its DLL is unloaded, so the next load starts from fresh globals.
static bool restart_core(mdmi_engine *e)
{
	__atomic_store_n(&e->booted, 0, __ATOMIC_RELEASE);
	e->retro_deinit();
	FreeLibrary(e->dll);
	e->dll = NULL;
	return load_core(e);
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
		if (chunk->kind == CHUNK_OWNER) {
			// The latest wins: a voice taken by another channel before it keyed on cuts the note that
			// was meant for it anyway.
			e->pending_owner[chunk->bytes[0]] = (int8_t)chunk->bytes[1];
			e->pending_since[chunk->bytes[0]] = e->core_frames;
		} else if (chunk->kind == CHUNK_OWNER_NOW) {
			e->owner[chunk->bytes[0]] = (int8_t)chunk->bytes[1];
			e->pending_owner[chunk->bytes[0]] = NO_OWNER;
		} else {
			e->midi_write(chunk->bytes, chunk->length);
		}
		tail++;
	}
	__atomic_store_n(&e->midi_tail, tail, __ATOMIC_RELEASE);
}

// Journal counters have one writer each; a lost race between a store and the worker's take loses
// one sample of a diagnostic, nothing else.
static void j_max(uint32_t *slot, uint32_t value)
{
	if (value > __atomic_load_n(slot, __ATOMIC_RELAXED)) {
		__atomic_store_n(slot, value, __ATOMIC_RELAXED);
	}
}

static void j_min(uint32_t *slot, uint32_t value)
{
	if (value < __atomic_load_n(slot, __ATOMIC_RELAXED)) {
		__atomic_store_n(slot, value, __ATOMIC_RELAXED);
	}
}

static uint32_t j_take(uint32_t *slot, uint32_t reset)
{
	return __atomic_exchange_n(slot, reset, __ATOMIC_RELAXED);
}

// Windows 10 has it in kernel32, the image's mingw headers do not declare it.
WINBASEAPI BOOL WINAPI GetSystemCpuSetInformation(PSYSTEM_CPU_SET_INFORMATION information, ULONG length, PULONG returned, HANDLE process, ULONG flags);

// Logical processors of the lowest efficiency class, on hybrid processors only (E-cores).
static uint64_t slow_cpus;

static void find_slow_cpus(void)
{
	ULONG size = 0;
	GetSystemCpuSetInformation(NULL, 0, &size, GetCurrentProcess(), 0);
	uint8_t *buffer = size ? malloc(size) : NULL;
	if (!buffer || !GetSystemCpuSetInformation((PSYSTEM_CPU_SET_INFORMATION)buffer, size, &size, GetCurrentProcess(), 0)) {
		free(buffer);
		return;
	}
	uint8_t lowest = 255, highest = 0;
	for (ULONG at = 0; at < size; at += ((PSYSTEM_CPU_SET_INFORMATION)(buffer + at))->Size) {
		const PSYSTEM_CPU_SET_INFORMATION set = (PSYSTEM_CPU_SET_INFORMATION)(buffer + at);
		lowest = set->CpuSet.EfficiencyClass < lowest ? set->CpuSet.EfficiencyClass : lowest;
		highest = set->CpuSet.EfficiencyClass > highest ? set->CpuSet.EfficiencyClass : highest;
	}
	for (ULONG at = 0; lowest != highest && at < size; at += ((PSYSTEM_CPU_SET_INFORMATION)(buffer + at))->Size) {
		const PSYSTEM_CPU_SET_INFORMATION set = (PSYSTEM_CPU_SET_INFORMATION)(buffer + at);
		if (set->CpuSet.EfficiencyClass == lowest && set->CpuSet.LogicalProcessorIndex < 64) {
			slow_cpus |= 1ULL << set->CpuSet.LogicalProcessorIndex;
		}
	}
	free(buffer);
}

// The journal is on when %TEMP%\mdmi-journal exists, so the owner switches it in Live without a
// restart of anything but the project; tests name a folder in MDMI_JOURNAL instead.
static void journal_open(mdmi_engine *e)
{
	static LONG journal_count;
	char dir[MAX_PATH];
	const char *env = getenv("MDMI_JOURNAL");
	if (env && env[0]) {
		snprintf(dir, sizeof(dir), "%s\\", env);
	} else {
		char flag[MAX_PATH];
		if (!GetTempPathA(sizeof(dir), dir)) {
			return;
		}
		snprintf(flag, sizeof(flag), "%smdmi-journal", dir);
		if (GetFileAttributesA(flag) == INVALID_FILE_ATTRIBUTES) {
			return;
		}
	}
	char path[MAX_PATH];
	snprintf(path, sizeof(path), "%smdmi-journal-%lu-%ld.csv", dir, GetCurrentProcessId(), InterlockedIncrement(&journal_count));
	e->journal = fopen(path, "w");
	if (e->journal) {
		find_slow_cpus();
		fprintf(e->journal, "# slow_cpus=%llx\n", (unsigned long long)slow_cpus);
		fprintf(e->journal, "t,play,blocks,blk_min,blk_max,burst_max,gap_max_ms,avail_min,rwaits,rwait_ms,rwait_max_ms,short_blk,short_smp,burst_short_max,notes,"
			"runs,samples,run_ms,run_max_ms,wwait_ms,mcycles,cpu_moves,slow_runs,slow_run_ms,fill,underruns\n");
	}
}

static void journal_reset(mdmi_engine *e)
{
	e->journal_start_ms = now_ms();
	e->journal_next_ms = e->journal_start_ms + 1000.0;
	e->j_block_min = UINT32_MAX;
	e->j_avail_min = UINT32_MAX;
	e->j_samples_base = 0;
	// The worker is parked: what it counted before is not this activation's.
	e->j_runs = 0;
	e->j_run_ms = e->j_run_max_ms = e->j_worker_wait_ms = 0;
	e->j_cpu_moves = 0;
	e->j_slow_runs = 0;
	e->j_slow_run_ms = 0;
}

// The worker's line for the second that just ended.
static void journal_tick(mdmi_engine *e, uint64_t write)
{
	const uint32_t cpu = GetCurrentProcessorNumber();
	if (cpu != e->j_cpu) {
		e->j_cpu = cpu;
		e->j_cpu_moves++;
	}
	const double now = now_ms();
	if (now < e->journal_next_ms) {
		return;
	}
	e->journal_next_ms += 1000.0;
	if (e->journal_next_ms < now) {
		e->journal_next_ms = now + 1000.0;
	}
	ULONG64 cycles = 0;
	QueryThreadCycleTime(GetCurrentThread(), &cycles);
	const uint32_t block_min = j_take(&e->j_block_min, UINT32_MAX);
	const uint32_t avail_min = j_take(&e->j_avail_min, UINT32_MAX);
	const uint64_t read = __atomic_load_n(&e->ring_read, __ATOMIC_ACQUIRE);
	fprintf(e->journal, "%.1f,%u,%u,%u,%u,%u,%.2f,%d,%u,%.2f,%.2f,%u,%u,%u,%u,%u,%llu,%.1f,%.2f,%.1f,%.1f,%u,%u,%.1f,%lld,%u\n",
		(now - e->journal_start_ms) / 1000.0,
		__atomic_load_n(&e->j_playing, __ATOMIC_RELAXED),
		j_take(&e->j_blocks, 0), block_min == UINT32_MAX ? 0 : block_min, j_take(&e->j_block_max, 0), j_take(&e->j_burst_max, 0),
		j_take(&e->j_gap_max_us, 0) / 1000.0, avail_min == UINT32_MAX ? -1 : (int)avail_min,
		j_take(&e->j_waits, 0), j_take(&e->j_wait_us, 0) / 1000.0, j_take(&e->j_wait_max_us, 0) / 1000.0,
		j_take(&e->j_short_blocks, 0), j_take(&e->j_short_samples, 0), j_take(&e->j_burst_short_max, 0), j_take(&e->j_notes, 0),
		e->j_runs, (unsigned long long)(write - e->j_samples_base), e->j_run_ms, e->j_run_max_ms, e->j_worker_wait_ms,
		(cycles - e->j_cycles_base) / 1e6, e->j_cpu_moves, e->j_slow_runs, e->j_slow_run_ms, (long long)(write - read),
		__atomic_load_n(&e->underruns, __ATOMIC_RELAXED));
	fflush(e->journal);
	e->j_runs = 0;
	e->j_run_ms = e->j_run_max_ms = e->j_worker_wait_ms = 0;
	e->j_samples_base = write;
	e->j_cycles_base = cycles;
	e->j_cpu_moves = 0;
	e->j_slow_runs = 0;
	e->j_slow_run_ms = 0;
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

	bool parked = true;
	while (!__atomic_load_n(&e->quit, __ATOMIC_ACQUIRE)) {
		if (!__atomic_load_n(&e->active, __ATOMIC_SEQ_CST)) {
			// The request is taken first and active read again after it: a zero seen then is the one
			// that request switched off, not an older one the last activation has not replaced yet.
			const uint32_t request = __atomic_load_n(&e->park_request, __ATOMIC_SEQ_CST);
			if (__atomic_load_n(&e->active, __ATOMIC_SEQ_CST)) {
				continue;
			}
			__atomic_store_n(&e->park_answer, request, __ATOMIC_SEQ_CST);
			parked = true;
			WaitForSingleObject(e->wake, INFINITE);
			continue;
		}
		if (parked) {
			parked = false;
			QueryThreadCycleTime(GetCurrentThread(), &e->j_cycles_base);
		}
		rom_image *image = __atomic_exchange_n(&e->next_rom, NULL, __ATOMIC_ACQ_REL);
		if (image) {
			if (patch_core(e, image)) {
				free(image);
				continue;
			}
			// Back for write_rom, unless a newer image came meanwhile.
			rom_image *none = NULL;
			if (!__atomic_compare_exchange_n(&e->next_rom, &none, image, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
				free(image);
			}
		}
		if (__atomic_load_n(&e->next_rom, __ATOMIC_ACQUIRE)) {
			// Booting is CPU-heavy, so it runs outside the Pro Audio class, as the first boot does.
			if (mmcss) {
				AvRevertMmThreadCharacteristics(mmcss);
			}
			const bool restarted = restart_core(e);
			mmcss = AvSetMmThreadCharacteristicsA("Pro Audio", &mmcss_task);
			if (e->journal) {
				fprintf(e->journal, "# restart %.0f ms\n", e->last_boot_ms);
			}
			if (!restarted) {
				__atomic_store_n(&e->failed, 1, __ATOMIC_RELEASE);
				return 1;
			}
			// The new core has the default filter.
			e->applied_filter_hz = 0;
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
		bool full = __atomic_load_n(&e->lockstep, __ATOMIC_ACQUIRE)
			? write >= __atomic_load_n(&e->lockstep_target, __ATOMIC_ACQUIRE)
			: write - read >= e->fill_target;
		if (full) {
			double wait_start = now_ms();
			WaitForSingleObject(e->wake, 1);
			double wait_ms = now_ms() - wait_start;
			if (wait_ms > e->worst_wait_ms) {
				e->worst_wait_ms = wait_ms;
			}
			if (e->journal) {
				e->j_worker_wait_ms += wait_ms;
				journal_tick(e, write);
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
		if (e->journal) {
			e->j_runs++;
			e->j_run_ms += run_ms;
			if (run_ms > e->j_run_max_ms) {
				e->j_run_max_ms = run_ms;
			}
			const uint32_t cpu = GetCurrentProcessorNumber();
			if (cpu < 64 && (slow_cpus >> cpu & 1)) {
				e->j_slow_runs++;
				e->j_slow_run_ms += run_ms;
			}
			journal_tick(e, e->ring_write);
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
	e->wake = CreateEventA(NULL, FALSE, FALSE, NULL);
	e->rs = malloc(sizeof(resampler));
	e->midi_queue = calloc(MIDI_QUEUE_SIZE, sizeof(midi_chunk));
	e->frames = calloc(3, sizeof(mdmi_frame));
	if (!e->wake || !e->rs || !e->midi_queue || !e->frames) {
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
	free(__atomic_exchange_n(&e->next_rom, NULL, __ATOMIC_ACQ_REL));
	if (clean_exit) {
		if (e->journal) {
			fclose(e->journal);
		}
		if (e->wake) {
			CloseHandle(e->wake);
		}
		free(e->rs);
		for (int ch = 0; ch < RING_CHANNELS; ch++) {
			free(e->ring[ch]);
		}
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
	const double core_rate = CORE_RATE;
	e->sample_rate = sample_rate;
	resampler_init(e->rs, core_rate, sample_rate, OUT_CHANNELS);
	e->dc_coef = (float)(1.0 - 2.0 * 3.14159265358979 * DC_BLOCK_HZ / core_rate);
	e->dc_primed = false;
	memset(e->dc_out, 0, sizeof(e->dc_out));

	uint32_t batch = (uint32_t)(CORE_BATCH_FRAMES * sample_rate / core_rate) + 2;
	uint32_t margin = (uint32_t)(sample_rate * MARGIN_SECONDS);
	e->fill_target = max_block + margin;
	e->midi_delay = e->fill_target + batch;
	e->latency = e->midi_delay + (uint32_t)(sample_rate * ROM_REACTION_SECONDS);

	// The worker writes up to one run past the target; twice that leaves the reader its block.
	uint32_t ring_frames = RING_MIN_FRAMES;
	while (ring_frames < 2 * (e->fill_target + batch)) {
		ring_frames *= 2;
	}
	if (ring_frames != e->ring_mask + 1 || !e->ring[0]) {
		bool all = true;
		for (int ch = 0; ch < RING_CHANNELS; ch++) {
			free(e->ring[ch]);
			e->ring[ch] = calloc(ring_frames, sizeof(float));
			all = all && e->ring[ch];
		}
		if (!all) {
			// All or none: the next activation then allocates again.
			for (int ch = 0; ch < RING_CHANNELS; ch++) {
				free(e->ring[ch]);
				e->ring[ch] = NULL;
			}
			return false;
		}
		e->ring_mask = ring_frames - 1;
	}

	e->ring_read = e->ring_write = 0;
	e->lockstep_target = 0;
	e->midi_head = e->midi_tail = 0;
	if (!e->journal) {
		journal_open(e);
	}
	if (e->journal) {
		journal_reset(e);
		fprintf(e->journal, "# activate rate=%.0f max_block=%u fill_target=%u midi_delay=%u latency=%u ring=%u offline=%u\n",
			sample_rate, max_block, e->fill_target, e->midi_delay, e->latency, ring_frames, __atomic_load_n(&e->offline, __ATOMIC_ACQUIRE));
		fflush(e->journal);
	}
	__atomic_store_n(&e->active, 1, __ATOMIC_SEQ_CST);

	if (!e->thread_started) {
		e->thread = CreateThread(NULL, 0, worker_main, e, 0, NULL);
		if (!e->thread) {
			e->active = 0;
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
	const uint32_t request = __atomic_add_fetch(&e->park_request, 1, __ATOMIC_SEQ_CST);
	__atomic_store_n(&e->active, 0, __ATOMIC_SEQ_CST);
	if (!e->thread_started) {
		return;
	}
	SetEvent(e->wake);
	while (!worker_dead(e) && __atomic_load_n(&e->park_answer, __ATOMIC_ACQUIRE) != request) {
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

void mdmi_engine_set_playing(mdmi_engine *e, bool playing)
{
	__atomic_store_n(&e->j_playing, playing, __ATOMIC_RELAXED);
}

void mdmi_engine_set_filter(mdmi_engine *e, double cutoff_hz)
{
	__atomic_store_n(&e->filter_hz, (uint32_t)cutoff_hz, __ATOMIC_RELEASE);
}

void mdmi_engine_set_offline(mdmi_engine *e, bool offline)
{
	// Whether a host says so when it freezes or exports (audit 5.4) is what the journal is asked for.
	if (__atomic_exchange_n(&e->offline, offline, __ATOMIC_ACQ_REL) != offline && e->journal) {
		fprintf(e->journal, "# offline=%u\n", offline);
		fflush(e->journal);
	}
}

uint32_t mdmi_engine_boots(const mdmi_engine *e)
{
	return __atomic_load_n(&e->boots, __ATOMIC_ACQUIRE);
}

const uint8_t *mdmi_engine_builtin_rom(size_t *size)
{
	*size = (size_t)(mdmi_blob_rom_end - mdmi_blob_rom);
	return mdmi_blob_rom;
}

bool mdmi_engine_set_rom(mdmi_engine *e, const uint8_t *bytes, size_t size)
{
	rom_image *image = malloc(sizeof(*image) + size);
	if (!image) {
		return false;
	}
	image->size = size;
	memcpy(image->bytes, bytes, size);
	free(__atomic_exchange_n(&e->next_rom, image, __ATOMIC_ACQ_REL));
	SetEvent(e->wake);
	return true;
}

void mdmi_engine_test_lockstep(mdmi_engine *e, bool lockstep)
{
	__atomic_store_n(&e->lockstep, lockstep, __ATOMIC_RELEASE);
}

uint8_t mdmi_engine_test_ym(const mdmi_engine *e, uint8_t part, uint8_t reg)
{
	return e->test_ym_read(part, reg);
}

uint8_t mdmi_engine_test_psg_volume(const mdmi_engine *e, uint8_t channel)
{
	return e->test_psg_volume(channel);
}

uint16_t mdmi_engine_test_psg_tone(const mdmi_engine *e, uint8_t channel)
{
	return e->test_psg_tone(channel);
}

int mdmi_engine_test_owner(const mdmi_engine *e, int voice)
{
	return e->owner[voice];
}

float mdmi_engine_test_take_owner_wait(mdmi_engine *e)
{
	const float wait = e->owner_wait_round_ms;
	e->owner_wait_round_ms = 0;
	return wait;
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
		.boots = __atomic_load_n(&e->boots, __ATOMIC_ACQUIRE),
		.last_boot_ms = e->last_boot_ms,
		.patches = __atomic_load_n(&e->patches, __ATOMIC_ACQUIRE),
		.last_patch_ms = e->last_patch_ms,
		.boot_stage = e->boot_stage,
		.boot_error = e->boot_error,
		.worst_run_pos = e->worst_run_pos,
		.first_underrun_pos = e->first_underrun_pos,
		.last_underrun_pos = e->last_underrun_pos,
		.active = __atomic_load_n(&e->active, __ATOMIC_ACQUIRE) != 0,
		.last_block = __atomic_load_n(&e->last_block, __ATOMIC_RELAXED),
		.fill_target = e->fill_target,
		.worst_short = __atomic_load_n(&e->worst_short, __ATOMIC_RELAXED),
		.longest_burst = __atomic_load_n(&e->longest_burst, __ATOMIC_RELAXED),
		.sample_rate = (uint32_t)e->sample_rate,
		.owner_misses = __atomic_load_n(&e->owner_misses, __ATOMIC_RELAXED),
		.owner_wait_ms = e->owner_wait_ms,
		.voice_batch_misses = __atomic_load_n(&e->voice_batch_misses, __ATOMIC_RELAXED),
		.voice_mix_error = e->voice_mix_error,
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
	if (e->journal && length >= 3 && (bytes[0] & 0xF0) == 0x90 && bytes[2]) {
		__atomic_add_fetch(&e->j_notes, 1, __ATOMIC_RELAXED);
	}
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
		chunk->kind = CHUNK_MIDI;
		chunk->length = (uint8_t)count;
		memcpy(chunk->bytes, bytes, count);
		bytes += count;
		length -= count;
		head++;
	}
	__atomic_store_n(&e->midi_head, head, __ATOMIC_RELEASE);
}

void mdmi_engine_voice_owner(mdmi_engine *e, uint32_t sample_offset, int voice, int channel, bool at_once)
{
	if (voice < 0 || voice >= MDMI_VOICES || channel < 0 || channel >= MDMI_OUTPUTS) {
		return;
	}
	uint64_t target = __atomic_load_n(&e->ring_read, __ATOMIC_RELAXED) + sample_offset + e->midi_delay;
	uint32_t head = e->midi_head;
	uint32_t tail = __atomic_load_n(&e->midi_tail, __ATOMIC_ACQUIRE);
	if (MIDI_QUEUE_SIZE - (head - tail) < 1) {
		__atomic_fetch_add(&e->dropped_midi, 1, __ATOMIC_RELAXED);
		return;
	}
	midi_chunk *chunk = &e->midi_queue[head % MIDI_QUEUE_SIZE];
	chunk->target = target;
	chunk->kind = at_once ? CHUNK_OWNER_NOW : CHUNK_OWNER;
	chunk->length = 2;
	chunk->bytes[0] = (uint8_t)voice;
	chunk->bytes[1] = (uint8_t)channel;
	__atomic_store_n(&e->midi_head, head + 1, __ATOMIC_RELEASE);
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
	float *channels[MDMI_RENDER_CHANNELS] = {left, right};
	mdmi_engine_render_outputs(e, channels, frames);
}

void mdmi_engine_render_outputs(mdmi_engine *e, float *const *channels, uint32_t frames)
{
	uint64_t read = e->ring_read;
	__atomic_store_n(&e->lockstep_target, read + frames, __ATOMIC_RELEASE);
	uint64_t available = __atomic_load_n(&e->ring_write, __ATOMIC_ACQUIRE) - read;
	SetEvent(e->wake);
	__atomic_store_n(&e->last_block, frames, __ATOMIC_RELAXED);
	double render_start = now_ms();
	const double block_ms = frames * 1000.0 / e->sample_rate;
	const double gap_ms = render_start - e->last_render_end_ms;
	e->burst = gap_ms < block_ms * BURST_GAP_SHARE ? e->burst + 1 : 1;
	if (e->burst > e->longest_burst) {
		__atomic_store_n(&e->longest_burst, e->burst, __ATOMIC_RELAXED);
	}
	const bool journal = e->journal != NULL;
	if (journal) {
		__atomic_add_fetch(&e->j_blocks, 1, __ATOMIC_RELAXED);
		j_min(&e->j_block_min, frames);
		j_max(&e->j_block_max, frames);
		j_max(&e->j_burst_max, e->burst);
		if (read) {
			j_max(&e->j_gap_max_us, (uint32_t)(gap_ms * 1000.0));
		}
		j_min(&e->j_avail_min, (uint32_t)available);
		if (e->burst == 1) {
			e->burst_short = 0;
		}
	}

	bool offline = __atomic_load_n(&e->offline, __ATOMIC_ACQUIRE) != 0;
	if (available < frames && (offline || __atomic_load_n(&e->booted, __ATOMIC_ACQUIRE))) {
		// Offline rendering has no deadline. In real time a host with a large device buffer calls
		// several blocks in a row, ahead of its deadline, so a short wait beats dropping audio.
		// A block alone that has to wait means the worker is late, and six instances waiting long in a row
		// would make the whole host miss its deadline; so only a burst waits longer.
		const double wait_ms = e->burst > 1 && block_ms * 0.5 > RENDER_WAIT_MS ? block_ms * 0.5 : RENDER_WAIT_MS;
		double give_up = now_ms() + wait_ms;
		while (available < frames && !worker_dead(e) && (offline || now_ms() < give_up)) {
			SwitchToThread();
			available = __atomic_load_n(&e->ring_write, __ATOMIC_ACQUIRE) - read;
		}
		if (journal) {
			const uint32_t waited_us = (uint32_t)((now_ms() - render_start) * 1000.0);
			__atomic_add_fetch(&e->j_waits, 1, __ATOMIC_RELAXED);
			__atomic_add_fetch(&e->j_wait_us, waited_us, __ATOMIC_RELAXED);
			j_max(&e->j_wait_max_us, waited_us);
		}
	}

	uint32_t count = available < frames ? (uint32_t)available : frames;
	if (count < frames && __atomic_load_n(&e->booted, __ATOMIC_ACQUIRE) && !__atomic_load_n(&e->failed, __ATOMIC_ACQUIRE) && read) {
		if (!__atomic_fetch_add(&e->underruns, 1, __ATOMIC_RELAXED)) {
			e->first_underrun_pos = read;
		}
		e->last_underrun_pos = read;
		if (frames - count > e->worst_short) {
			__atomic_store_n(&e->worst_short, frames - count, __ATOMIC_RELAXED);
		}
		if (journal) {
			__atomic_add_fetch(&e->j_short_blocks, 1, __ATOMIC_RELAXED);
			__atomic_add_fetch(&e->j_short_samples, frames - count, __ATOMIC_RELAXED);
			e->burst_short += frames - count;
			j_max(&e->j_burst_short_max, e->burst_short);
		}
	}
	for (int ch = 0; ch < RING_CHANNELS; ch++) {
		float *dest = channels[ch];
		if (!dest) {
			continue;
		}
		const float *ring = e->ring[ch];
		for (uint32_t i = 0; i < count; i++) {
			dest[i] = ring[(read + i) & e->ring_mask];
		}
		for (uint32_t i = count; i < frames; i++) {
			dest[i] = 0;
		}
	}
	__atomic_store_n(&e->ring_read, read + count, __ATOMIC_RELEASE);
	SetEvent(e->wake);
	e->last_render_end_ms = now_ms();
}
