// Feasibility host: boots the MDMI ROM in libblastem.dll, plays MIDI through the
// EverDrive bridge and records what comes out. No plugin code yet.
//
//   proto_host <libblastem.dll> <rom> <out.wav> <out.bmp>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "libretro.h"

#define BOOT_FRAMES 120

typedef struct {
	int frame;
	uint8_t bytes[3];
	const char *label;
} midi_event;

// MDMI default mapping: MIDI channels 1-6 are FM, 7-9 PSG tones.
static const midi_event script[] = {
	{BOOT_FRAMES + 0,   {0x90, 60, 127}, "FM ch1 C4 on"},
	{BOOT_FRAMES + 45,  {0x80, 60, 0},   "FM ch1 C4 off"},
	{BOOT_FRAMES + 90,  {0x90, 64, 127}, "FM ch1 E4 on"},
	{BOOT_FRAMES + 135, {0x80, 64, 0},   "FM ch1 E4 off"},
	{BOOT_FRAMES + 180, {0x96, 72, 127}, "PSG ch7 C5 on"},
	{BOOT_FRAMES + 225, {0x86, 72, 0},   "PSG ch7 C5 off"},
};
#define NUM_EVENTS (sizeof(script) / sizeof(*script))
#define TOTAL_FRAMES (BOOT_FRAMES + 300)

static int16_t *samples;
static size_t num_samples, cap_samples;
static const uint32_t *last_frame;
static unsigned last_width, last_height;
static size_t last_pitch;
static uint32_t *frame_copy;

static bool env_cb(unsigned cmd, void *data)
{
	switch (cmd) {
	case RETRO_ENVIRONMENT_GET_OVERSCAN:
		*(bool *)data = true;
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
	if (!data) {
		return;
	}
	last_frame = data;
	last_width = width;
	last_height = height;
	last_pitch = pitch;
}

static void audio_cb(int16_t left, int16_t right)
{
	(void)left;
	(void)right;
}

static size_t audio_batch_cb(const int16_t *data, size_t frames)
{
	if (num_samples + frames > cap_samples) {
		cap_samples = (cap_samples + frames) * 2;
		samples = realloc(samples, cap_samples * 2 * sizeof(int16_t));
	}
	memcpy(samples + num_samples * 2, data, frames * 2 * sizeof(int16_t));
	num_samples += frames;
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

static void write_le(FILE *f, uint32_t value, int bytes)
{
	for (int i = 0; i < bytes; i++) {
		fputc(value >> (8 * i) & 0xFF, f);
	}
}

static void write_wav(const char *path, uint32_t rate)
{
	FILE *f = fopen(path, "wb");
	uint32_t data_size = (uint32_t)(num_samples * 4);
	fwrite("RIFF", 1, 4, f);
	write_le(f, 36 + data_size, 4);
	fwrite("WAVEfmt ", 1, 8, f);
	write_le(f, 16, 4);
	write_le(f, 1, 2);
	write_le(f, 2, 2);
	write_le(f, rate, 4);
	write_le(f, rate * 4, 4);
	write_le(f, 4, 2);
	write_le(f, 16, 2);
	fwrite("data", 1, 4, f);
	write_le(f, data_size, 4);
	fwrite(samples, 4, num_samples, f);
	fclose(f);
}

static void write_bmp(const char *path)
{
	FILE *f = fopen(path, "wb");
	uint32_t size = last_width * last_height * 4;
	fwrite("BM", 1, 2, f);
	write_le(f, 54 + size, 4);
	write_le(f, 0, 4);
	write_le(f, 54, 4);
	write_le(f, 40, 4);
	write_le(f, last_width, 4);
	write_le(f, last_height, 4);
	write_le(f, 1, 2);
	write_le(f, 32, 2);
	write_le(f, 0, 4);
	write_le(f, size, 4);
	write_le(f, 2835, 4);
	write_le(f, 2835, 4);
	write_le(f, 0, 4);
	write_le(f, 0, 4);
	for (int y = (int)last_height - 1; y >= 0; y--) {
		fwrite(frame_copy + y * last_width, 4, last_width, f);
	}
	fclose(f);
}

#define LOAD(name) \
	name##_t name = (name##_t)GetProcAddress(lib, #name); \
	if (!name) { fprintf(stderr, "missing export " #name "\n"); return 1; }

typedef void (*retro_set_environment_t)(retro_environment_t);
typedef void (*retro_set_video_refresh_t)(retro_video_refresh_t);
typedef void (*retro_set_audio_sample_t)(retro_audio_sample_t);
typedef void (*retro_set_audio_sample_batch_t)(retro_audio_sample_batch_t);
typedef void (*retro_set_input_poll_t)(retro_input_poll_t);
typedef void (*retro_set_input_state_t)(retro_input_state_t);
typedef void (*retro_init_t)(void);
typedef bool (*retro_load_game_t)(const struct retro_game_info *);
typedef void (*retro_get_system_av_info_t)(struct retro_system_av_info *);
typedef void (*retro_run_t)(void);
typedef size_t (*mdmi_midi_write_t)(const uint8_t *, size_t);

int main(int argc, char **argv)
{
	if (argc != 5) {
		fprintf(stderr, "usage: %s <libblastem.dll> <rom> <out.wav> <out.bmp>\n", argv[0]);
		return 2;
	}
	HMODULE lib = LoadLibraryA(argv[1]);
	if (!lib) {
		fprintf(stderr, "LoadLibrary failed: %lu\n", GetLastError());
		return 1;
	}
	LOAD(retro_set_environment)
	LOAD(retro_set_video_refresh)
	LOAD(retro_set_audio_sample)
	LOAD(retro_set_audio_sample_batch)
	LOAD(retro_set_input_poll)
	LOAD(retro_set_input_state)
	LOAD(retro_init)
	LOAD(retro_load_game)
	LOAD(retro_get_system_av_info)
	LOAD(retro_run)
	LOAD(mdmi_midi_write)

	retro_set_environment(env_cb);
	retro_set_video_refresh(video_cb);
	retro_set_audio_sample(audio_cb);
	retro_set_audio_sample_batch(audio_batch_cb);
	retro_set_input_poll(input_poll_cb);
	retro_set_input_state(input_state_cb);
	retro_init();

	struct retro_game_info game = {.path = argv[2]};
	if (!retro_load_game(&game)) {
		fprintf(stderr, "retro_load_game failed\n");
		return 1;
	}
	struct retro_system_av_info av;
	retro_get_system_av_info(&av);
	printf("rate=%.1f fps=%.3f\n", av.timing.sample_rate, av.timing.fps);

	LARGE_INTEGER freq, start, end, frame_start, frame_end;
	QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&start);
	double worst_frame = 0;
	size_t next_event = 0;
	for (int frame = 0; frame < TOTAL_FRAMES; frame++) {
		while (next_event < NUM_EVENTS && script[next_event].frame == frame) {
			const midi_event *ev = &script[next_event++];
			mdmi_midi_write(ev->bytes, 3);
			printf("EVENT %s sample=%zu\n", ev->label, num_samples);
		}
		QueryPerformanceCounter(&frame_start);
		retro_run();
		QueryPerformanceCounter(&frame_end);
		double ms = (double)(frame_end.QuadPart - frame_start.QuadPart) * 1000.0 / (double)freq.QuadPart;
		// Boot compiles most of the JIT cache, so track spikes separately after it.
		if (frame >= BOOT_FRAMES && ms > worst_frame) {
			worst_frame = ms;
		}
	}
	QueryPerformanceCounter(&end);
	double secs = (double)(end.QuadPart - start.QuadPart) / (double)freq.QuadPart;
	printf("emulated=%.2fs wall=%.3fs speed=%.1fx worst_frame_after_boot=%.2fms samples=%zu\n",
		TOTAL_FRAMES / av.timing.fps, secs, (TOTAL_FRAMES / av.timing.fps) / secs, worst_frame, num_samples);

	write_wav(argv[3], (uint32_t)av.timing.sample_rate);
	if (last_frame) {
		frame_copy = malloc(last_width * last_height * 4);
		for (unsigned y = 0; y < last_height; y++) {
			memcpy(frame_copy + y * last_width, (const uint8_t *)last_frame + y * last_pitch, last_width * 4);
		}
		write_bmp(argv[4]);
		printf("frame=%ux%u\n", last_width, last_height);
	}
	fflush(stdout);
	// BlastEm has no clean shutdown path worth exercising in this probe.
	ExitProcess(0);
}
