// Headless CLAP host for testing the plugin without a DAW.
//
//   clap_host <plugin.clap> <out_prefix> <rate> <block> <offline|realtime> <instances>
//
// Each instance plays its own note script. Writes <out_prefix>-<n>.wav and prints
// the sample each note was scheduled at, so the analyzer can check timing.
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <avrt.h>
#include <clap/clap.h>

#define MAX_INSTANCES 8
#define SECONDS 6

typedef struct {
	double seconds;
	uint8_t bytes[3];
} script_event;

static const script_event script[] = {
	{3.000, {0x90, 60, 127}},
	{3.500, {0x80, 60, 0}},
	{4.000, {0x90, 64, 127}},
	{4.500, {0x80, 64, 0}},
	{5.000, {0x96, 72, 127}},
	{5.500, {0x86, 72, 0}},
};
#define NUM_SCRIPT (sizeof(script) / sizeof(*script))

static const void *host_get_extension(const clap_host_t *host, const char *id)
{
	(void)host;
	(void)id;
	return NULL;
}
static void host_noop(const clap_host_t *host) { (void)host; }

static const clap_host_t host = {
	.clap_version = CLAP_VERSION_INIT,
	.name = "mdmi-test-host",
	.vendor = "",
	.url = "",
	.version = "1",
	.get_extension = host_get_extension,
	.request_restart = host_noop,
	.request_process = host_noop,
	.request_callback = host_noop,
};

typedef struct {
	clap_event_midi_t events[NUM_SCRIPT];
	uint32_t count;
} block_events;

static uint32_t events_size(const clap_input_events_t *list)
{
	return ((const block_events *)list->ctx)->count;
}

static const clap_event_header_t *events_get(const clap_input_events_t *list, uint32_t index)
{
	return &((const block_events *)list->ctx)->events[index].header;
}

static bool out_try_push(const clap_output_events_t *list, const clap_event_header_t *event)
{
	(void)list;
	(void)event;
	return true;
}

static void write_le(FILE *f, uint32_t value, int bytes)
{
	for (int i = 0; i < bytes; i++) {
		fputc(value >> (8 * i) & 0xFF, f);
	}
}

static void write_wav(const char *path, const float *left, const float *right, uint32_t frames, uint32_t rate)
{
	FILE *f = fopen(path, "wb");
	uint32_t size = frames * 8;
	fwrite("RIFF", 1, 4, f);
	write_le(f, 36 + size, 4);
	fwrite("WAVEfmt ", 1, 8, f);
	write_le(f, 16, 4);
	write_le(f, 3, 2);
	write_le(f, 2, 2);
	write_le(f, rate, 4);
	write_le(f, rate * 8, 4);
	write_le(f, 8, 2);
	write_le(f, 32, 2);
	fwrite("data", 1, 4, f);
	write_le(f, size, 4);
	for (uint32_t i = 0; i < frames; i++) {
		fwrite(&left[i], 4, 1, f);
		fwrite(&right[i], 4, 1, f);
	}
	fclose(f);
}

int main(int argc, char **argv)
{
	if (argc != 7) {
		fprintf(stderr, "usage: %s <plugin> <out_prefix> <rate> <block> <offline|realtime> <instances>\n", argv[0]);
		return 2;
	}
	uint32_t rate = (uint32_t)atoi(argv[3]);
	uint32_t block = (uint32_t)atoi(argv[4]);
	bool offline = !strcmp(argv[5], "offline");
	int instances = atoi(argv[6]);

	HMODULE lib = LoadLibraryA(argv[1]);
	if (!lib) {
		fprintf(stderr, "LoadLibrary failed: %lu\n", GetLastError());
		return 1;
	}
	const clap_plugin_entry_t *entry = (const clap_plugin_entry_t *)GetProcAddress(lib, "clap_entry");
	if (!entry || !entry->init(argv[1])) {
		fprintf(stderr, "bad clap_entry\n");
		return 1;
	}
	const clap_plugin_factory_t *factory = entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
	const clap_plugin_descriptor_t *desc = factory->get_plugin_descriptor(factory, 0);
	printf("plugin=%s\n", desc->id);

	const clap_plugin_t *plugins[MAX_INSTANCES];
	uint32_t latency = 0;
	for (int i = 0; i < instances; i++) {
		plugins[i] = factory->create_plugin(factory, &host, desc->id);
		if (!plugins[i] || !plugins[i]->init(plugins[i])) {
			fprintf(stderr, "instance %d failed to init\n", i);
			return 1;
		}
		const clap_plugin_render_t *render = plugins[i]->get_extension(plugins[i], CLAP_EXT_RENDER);
		render->set(plugins[i], offline ? CLAP_RENDER_OFFLINE : CLAP_RENDER_REALTIME);
		if (!plugins[i]->activate(plugins[i], rate, block, block)) {
			fprintf(stderr, "instance %d failed to activate\n", i);
			return 1;
		}
		plugins[i]->start_processing(plugins[i]);
		const clap_plugin_latency_t *lat = plugins[i]->get_extension(plugins[i], CLAP_EXT_LATENCY);
		latency = lat->get(plugins[i]);
	}
	printf("latency=%u\n", latency);

	// Notes start after the engines have booted, so a slow boot does not skew timing checks.
	uint32_t total = rate * SECONDS;
	float *left[MAX_INSTANCES], *right[MAX_INSTANCES];
	for (int i = 0; i < instances; i++) {
		left[i] = calloc(total + block, sizeof(float));
		right[i] = calloc(total + block, sizeof(float));
	}

	// Audio drivers call back on an MMCSS thread; without this the OS preempts the test loop
	// and its catch-up bursts look like plugin underruns.
	DWORD mmcss_task = 0;
	if (!offline) {
		AvSetMmThreadCharacteristicsA("Pro Audio", &mmcss_task);
	}
	LARGE_INTEGER freq, start, now;
	QueryPerformanceFrequency(&freq);
	double worst_process_ms = 0;
	double worst_late_ms = 0;
	int passes = getenv("MDMI_TEST_REACTIVATE") ? 2 : 1;
	for (int pass = 0; pass < passes; pass++) {
	if (pass) {
		for (int i = 0; i < instances; i++) {
			plugins[i]->stop_processing(plugins[i]);
			plugins[i]->deactivate(plugins[i]);
			memset(left[i], 0, (total + block) * sizeof(float));
			memset(right[i], 0, (total + block) * sizeof(float));
			if (!plugins[i]->activate(plugins[i], rate, block, block)) {
				fprintf(stderr, "instance %d failed to reactivate\n", i);
				return 1;
			}
			plugins[i]->start_processing(plugins[i]);
		}
	}
	QueryPerformanceCounter(&start);
	size_t next = 0;
	for (uint32_t pos = 0; pos < total; pos += block) {
		block_events pending = {.count = 0};
		while (next < NUM_SCRIPT && (uint32_t)(script[next].seconds * rate) < pos + block) {
			uint32_t at = (uint32_t)(script[next].seconds * rate);
			clap_event_midi_t *ev = &pending.events[pending.count++];
			memset(ev, 0, sizeof(*ev));
			ev->header.size = sizeof(*ev);
			ev->header.time = at - pos;
			ev->header.space_id = CLAP_CORE_EVENT_SPACE_ID;
			ev->header.type = CLAP_EVENT_MIDI;
			memcpy(ev->data, script[next].bytes, 3);
			printf("EVENT %02X %d sample=%u\n", ev->data[0], ev->data[1], at);
			next++;
		}
		clap_input_events_t in_events = {.ctx = &pending, .size = events_size, .get = events_get};
		clap_output_events_t out_events = {.ctx = NULL, .try_push = out_try_push};
		for (int i = 0; i < instances; i++) {
			float *channels[2] = {left[i] + pos, right[i] + pos};
			clap_audio_buffer_t out = {.data32 = channels, .channel_count = 2};
			clap_process_t process = {
				.steady_time = pos,
				.frames_count = block,
				.audio_outputs = &out,
				.audio_outputs_count = 1,
				.in_events = &in_events,
				.out_events = &out_events,
			};
			LARGE_INTEGER t0, t1;
			QueryPerformanceCounter(&t0);
			plugins[i]->process(plugins[i], &process);
			QueryPerformanceCounter(&t1);
			double ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)freq.QuadPart;
			if (ms > worst_process_ms) {
				worst_process_ms = ms;
			}
		}
		if (!offline) {
			// Pace blocks against the clock like an audio callback.
			double due = (double)(pos + block) / rate;
			QueryPerformanceCounter(&now);
			double late_ms = ((double)(now.QuadPart - start.QuadPart) / (double)freq.QuadPart - due) * 1000.0;
			if (pos > rate && late_ms > worst_late_ms) {
				worst_late_ms = late_ms;
			}
			for (;;) {
				QueryPerformanceCounter(&now);
				double elapsed = (double)(now.QuadPart - start.QuadPart) / (double)freq.QuadPart;
				if (elapsed >= due) {
					break;
				}
				// Spin rather than Sleep: Sleep can oversleep by 15ms, and catching up afterwards
				// would call process() faster than real time, which no audio driver does.
				SwitchToThread();
			}
		}
	}
	}
	QueryPerformanceCounter(&now);
	printf("wall=%.3fs audio=%ds worst_process=%.3fms worst_host_late=%.2fms\n", (double)(now.QuadPart - start.QuadPart) / (double)freq.QuadPart, SECONDS, worst_process_ms, worst_late_ms);

	for (int i = 0; i < instances; i++) {
		plugins[i]->stop_processing(plugins[i]);
		plugins[i]->deactivate(plugins[i]);
		char path[MAX_PATH];
		snprintf(path, sizeof(path), "%s-%d.wav", argv[2], i);
		write_wav(path, left[i], right[i], total, rate);
	}
	for (int i = 0; i < instances; i++) {
		plugins[i]->destroy(plugins[i]);
	}
	entry->deinit();
	printf("clean_shutdown=1\n");
	fflush(stdout);
	return 0;
}
