// End-to-end test of the plugin's knobs: drives them through CLAP and measures the audio.
//
//   param_test <plugin.clap>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <clap/clap.h>

#define RATE 48000
#define BLOCK 256
#define MAX_EVENTS 512
#define FFT_SIZE 16384
#define NUM_PARAMS 360

// Ids by D3: a global's id is its index, a channel's knob 1000 + channel number * 100 + param.
// Id 0 held Polyphony until D20 and stays unused.
enum { P_RETIRED, P_FILTER, P_LFO_ON, P_LFO_RATE, P_CH3, P_DAC };
enum { Q_PRESET, Q_VOLUME, Q_PAN, Q_FINE, Q_BEND, Q_PORTA_ON, Q_PORTA_TIME, Q_ALG, Q_FB, Q_AMS, Q_FMS, Q_OP };
#define ID(channel, param) (1000 + ((channel) + 1) * 100 + (param))
#define OP_SR 6
#define OP_SSG 10
#define OP_FIELDS 11
// Channel 1's knobs.
#define P_PRESET ID(0, Q_PRESET)
#define P_VOLUME ID(0, Q_VOLUME)
#define P_PAN ID(0, Q_PAN)
#define P_FINE ID(0, Q_FINE)
#define P_BEND ID(0, Q_BEND)
#define P_PORTA_ON ID(0, Q_PORTA_ON)
#define P_PORTA_TIME ID(0, Q_PORTA_TIME)
#define P_ALG ID(0, Q_ALG)
#define P_FB ID(0, Q_FB)
#define P_OP ID(0, Q_OP)
#define P_OP_END (P_OP + 4 * OP_FIELDS)
#define MAX_ID 2100

static int failures;
static int callbacks_requested, rescans;

#define CHECK(condition, ...) \
	do { \
		bool check_passed = (condition); \
		printf("  %s: ", check_passed ? "ok  " : "FAIL"); \
		printf(__VA_ARGS__); \
		printf("\n"); \
		if (!check_passed) failures++; \
	} while (0)

static void params_rescan(const clap_host_t *host, clap_param_rescan_flags flags)
{
	(void)host;
	(void)flags;
	rescans++;
}
static void params_clear(const clap_host_t *host, clap_id id, clap_param_clear_flags flags)
{
	(void)host;
	(void)id;
	(void)flags;
}
static void params_request_flush(const clap_host_t *host) { (void)host; }
static const clap_host_params_t host_params = {params_rescan, params_clear, params_request_flush};

static const void *host_get_extension(const clap_host_t *host, const char *id)
{
	(void)host;
	return strcmp(id, CLAP_EXT_PARAMS) ? NULL : &host_params;
}
static void host_noop(const clap_host_t *host) { (void)host; }
static void host_request_callback(const clap_host_t *host)
{
	(void)host;
	callbacks_requested++;
}

static const clap_host_t host = {
	.clap_version = CLAP_VERSION_INIT,
	.name = "mdmi-param-test",
	.vendor = "",
	.url = "",
	.version = "1",
	.get_extension = host_get_extension,
	.request_restart = host_noop,
	.request_process = host_noop,
	.request_callback = host_request_callback,
};

typedef union {
	clap_event_header_t header;
	clap_event_midi_t midi;
	clap_event_param_value_t param;
} any_event;

typedef struct {
	any_event events[MAX_EVENTS];
	uint32_t count;
} event_list;

static uint32_t events_size(const clap_input_events_t *list)
{
	return ((const event_list *)list->ctx)->count;
}
static const clap_event_header_t *events_get(const clap_input_events_t *list, uint32_t index)
{
	return &((const event_list *)list->ctx)->events[index].header;
}
static bool out_try_push(const clap_output_events_t *list, const clap_event_header_t *event)
{
	(void)list;
	(void)event;
	return true;
}

static const clap_plugin_factory_t *factory;
static const char *plugin_id;

typedef struct {
	const clap_plugin_t *plugin;
	const clap_plugin_params_t *params;
	const clap_plugin_state_t *state;
	event_list pending;
} instance;

static instance *create_instance(void)
{
	instance *in = calloc(1, sizeof(*in));
	in->plugin = factory->create_plugin(factory, &host, plugin_id);
	in->plugin->init(in->plugin);
	const clap_plugin_render_t *render = in->plugin->get_extension(in->plugin, CLAP_EXT_RENDER);
	render->set(in->plugin, CLAP_RENDER_OFFLINE);
	in->plugin->activate(in->plugin, RATE, BLOCK, BLOCK);
	in->plugin->start_processing(in->plugin);
	in->params = in->plugin->get_extension(in->plugin, CLAP_EXT_PARAMS);
	in->state = in->plugin->get_extension(in->plugin, CLAP_EXT_STATE);
	return in;
}

static void destroy_instance(instance *in)
{
	in->plugin->stop_processing(in->plugin);
	in->plugin->deactivate(in->plugin);
	in->plugin->destroy(in->plugin);
	free(in);
}

static void queue_param(instance *in, uint32_t id, double value)
{
	clap_event_param_value_t *e = &in->pending.events[in->pending.count++].param;
	memset(e, 0, sizeof(*e));
	e->header.size = sizeof(*e);
	e->header.space_id = CLAP_CORE_EVENT_SPACE_ID;
	e->header.type = CLAP_EVENT_PARAM_VALUE;
	e->param_id = id;
	e->value = value;
}

static void queue_midi(instance *in, uint8_t a, uint8_t b, uint8_t c)
{
	clap_event_midi_t *e = &in->pending.events[in->pending.count++].midi;
	memset(e, 0, sizeof(*e));
	e->header.size = sizeof(*e);
	e->header.space_id = CLAP_CORE_EVENT_SPACE_ID;
	e->header.type = CLAP_EVENT_MIDI;
	e->data[0] = a;
	e->data[1] = b;
	e->data[2] = c;
}

// Renders whole blocks, handing queued events to the first one.
static float *render(instance *in, uint32_t frames)
{
	uint32_t blocks = frames / BLOCK;
	float *out = calloc(blocks * BLOCK * 2, sizeof(float));
	float *dump = calloc(BLOCK, sizeof(float));
	for (uint32_t b = 0; b < blocks; b++) {
		float *channels[2] = {out + b * BLOCK, dump};
		clap_audio_buffer_t bus = {.data32 = channels, .channel_count = 2};
		clap_input_events_t events = {.ctx = &in->pending, .size = events_size, .get = events_get};
		clap_output_events_t out_events = {.ctx = NULL, .try_push = out_try_push};
		clap_process_t process = {
			.frames_count = BLOCK,
			.audio_outputs = &bus,
			.audio_outputs_count = 1,
			.in_events = &events,
			.out_events = &out_events,
		};
		in->plugin->process(in->plugin, &process);
		in->pending.count = 0;
	}
	free(dump);
	return out;
}

static void settle(instance *in, double seconds)
{
	free(render(in, (uint32_t)(seconds * RATE)));
}

static int get_value(instance *in, uint32_t id)
{
	double v = -1;
	in->params->get_value(in->plugin, id, &v);
	return (int)v;
}

// A note's sustain plus its release, past the latency.
static float *play_note(instance *in, uint8_t channel, uint8_t note, double seconds)
{
	queue_midi(in, 0x90 | channel, note, 127);
	uint32_t frames = (uint32_t)(seconds * RATE);
	float *held = render(in, frames);
	queue_midi(in, 0x80 | channel, note, 0);
	settle(in, 0.25);
	return held;
}

static void fft(double *re, double *im, int n)
{
	for (int i = 1, j = 0; i < n; i++) {
		int bit = n >> 1;
		for (; j & bit; bit >>= 1) {
			j ^= bit;
		}
		j ^= bit;
		if (i < j) {
			double t = re[i]; re[i] = re[j]; re[j] = t;
			t = im[i]; im[i] = im[j]; im[j] = t;
		}
	}
	for (int len = 2; len <= n; len <<= 1) {
		double ang = -2 * M_PI / len;
		for (int i = 0; i < n; i += len) {
			for (int k = 0; k < len / 2; k++) {
				double wr = cos(ang * k), wi = sin(ang * k);
				double ur = re[i + k], ui = im[i + k];
				double vr = re[i + k + len / 2] * wr - im[i + k + len / 2] * wi;
				double vi = re[i + k + len / 2] * wi + im[i + k + len / 2] * wr;
				re[i + k] = ur + vr;
				im[i + k] = ui + vi;
				re[i + k + len / 2] = ur - vr;
				im[i + k + len / 2] = ui - vi;
			}
		}
	}
}

// Magnitude spectrum of FFT_SIZE samples starting at the given offset.
static double *spectrum(const float *samples, uint32_t start)
{
	static double re[FFT_SIZE], im[FFT_SIZE];
	for (int i = 0; i < FFT_SIZE; i++) {
		re[i] = samples[start + i] * (0.5 - 0.5 * cos(2 * M_PI * i / (FFT_SIZE - 1)));
		im[i] = 0;
	}
	fft(re, im, FFT_SIZE);
	double *mag = malloc(sizeof(double) * (FFT_SIZE / 2));
	for (int i = 0; i < FFT_SIZE / 2; i++) {
		mag[i] = sqrt(re[i] * re[i] + im[i] * im[i]);
	}
	return mag;
}

static double similarity(const double *a, const double *b)
{
	double dot = 0, na = 0, nb = 0;
	for (int i = 0; i < FFT_SIZE / 2; i++) {
		dot += a[i] * b[i];
		na += a[i] * a[i];
		nb += b[i] * b[i];
	}
	return na > 0 && nb > 0 ? dot / sqrt(na * nb) : 0;
}

static double rms(const float *samples, uint32_t start, uint32_t count)
{
	double sum = 0;
	for (uint32_t i = 0; i < count; i++) {
		sum += samples[start + i] * samples[start + i];
	}
	return sqrt(sum / count);
}

static double band_energy(const double *mag, double low_hz, double high_hz)
{
	double sum = 0;
	for (int i = (int)(low_hz * FFT_SIZE / RATE); i < (int)(high_hz * FFT_SIZE / RATE); i++) {
		sum += mag[i] * mag[i];
	}
	return sum;
}

static double note_hz(int note)
{
	return 440.0 * pow(2.0, (note - 69) / 12.0);
}

static void test_listing(void)
{
	printf("listing\n");
	instance *in = create_instance();
	uint32_t count = in->params->count(in->plugin);
	CHECK(count == NUM_PARAMS, "%u parameters: 6 global, 55 on each FM channel, 6 on each PSG channel", count);
	static bool seen[MAX_ID];
	bool ids_valid = true;
	for (uint32_t i = 0; i < count; i++) {
		clap_param_info_t info;
		in->params->get_info(in->plugin, i, &info);
		char text[64] = "";
		in->params->value_to_text(in->plugin, info.id, info.default_value, text, sizeof(text));
		ids_valid = ids_valid && info.id < MAX_ID && !seen[info.id];
		if (info.id < MAX_ID) {
			seen[info.id] = true;
		}
		if (i < 5 + 18 || i >= count - 6) {
			printf("    %-18s id %4u  0..%-5.0f default %s\n", info.name, info.id, info.max_value, text);
		}
	}
	CHECK(ids_valid, "every knob has a unique id");
	CHECK(seen[ID(3, Q_OP)] && seen[ID(9, Q_VOLUME)] && !seen[ID(9, Q_PAN)] && !seen[ID(6, Q_ALG)] && !seen[P_RETIRED], "channel 4 has Op1 TL at 1411, PSG channels have no pan or FM, id 0 is gone");
	clap_param_info_t info;
	in->params->get_info(in->plugin, 5 + 55 * 3 + Q_OP, &info);
	CHECK(info.id == ID(3, Q_OP) && !strcmp(info.name, "Ch4 Op1 TL"), "knobs are named by channel: %s", info.name);
	char ssg[64];
	in->params->value_to_text(in->plugin, P_OP + OP_SSG, 7, ssg, sizeof(ssg));
	CHECK(!strcmp(ssg, "Off"), "SSG 7 reads %s", ssg);
	in->params->value_to_text(in->plugin, P_OP + OP_SSG, 8, ssg, sizeof(ssg));
	CHECK(!strcmp(ssg, "Env 1"), "SSG 8 reads %s", ssg);
	in->params->value_to_text(in->plugin, P_OP + OP_SSG, 15, ssg, sizeof(ssg));
	CHECK(!strcmp(ssg, "Env 8"), "SSG 15 reads %s", ssg);
	destroy_instance(in);
}

// Typing in Live (audit 1.15): every label a knob shows reads back as itself, a number as the label
// with that number.
static void test_text(void)
{
	printf("text\n");
	instance *in = create_instance();
	uint32_t count = in->params->count(in->plugin);
	int wrong = 0;
	for (uint32_t i = 0; i < count; i++) {
		clap_param_info_t info;
		in->params->get_info(in->plugin, i, &info);
		for (int v = 0; v <= (int)info.max_value; v++) {
			char text[64], again[64] = "";
			double back = -1;
			in->params->value_to_text(in->plugin, info.id, v, text, sizeof(text));
			if (in->params->text_to_value(in->plugin, info.id, text, &back)) {
				in->params->value_to_text(in->plugin, info.id, back, again, sizeof(again));
			}
			if (strcmp(text, again)) {
				if (wrong++ < 3) {
					printf("    %s: \"%s\" reads back as \"%s\"\n", info.name, text, again);
				}
			}
		}
	}
	CHECK(!wrong, "every label of every knob reads back as itself, %d do not", wrong);
	double value = -1;
	CHECK(in->params->text_to_value(in->plugin, ID(0, Q_PRESET), "80", &value) && value == 79, "\"80\" on Preset sets preset 80 (value %.0f)", value);
	CHECK(in->params->text_to_value(in->plugin, P_FINE, "+3 ct", &value) && value == 67, "\"+3 ct\" on Fine Tune sets +3 (value %.0f)", value);
	CHECK(in->params->text_to_value(in->plugin, P_FINE, "-3", &value) && value == 61, "\"-3\" on Fine Tune sets -3 (value %.0f)", value);
	CHECK(in->params->text_to_value(in->plugin, P_OP, "80", &value) && value == 80, "\"80\" on Op1 TL sets 80 (value %.0f)", value);
	CHECK(in->params->text_to_value(in->plugin, ID(0, Q_PAN), "left", &value) && value == 0, "\"left\" on Pan sets Left (value %.0f)", value);
	CHECK(!in->params->text_to_value(in->plugin, P_OP, "loud", &value), "a word that is no label is refused");
	destroy_instance(in);
}

// What matters most: knobs set to a preset's values must equal the ROM loading that preset,
// which proves both the table generated from the ROM's source and every CC mapping.
static void test_preset_table(void)
{
	printf("knobs reproduce ROM presets\n");
	static const int programs[] = {0, 1, 4, 16, 33, 38, 56, 62, 80, 81, 88};
	for (size_t i = 0; i < sizeof(programs) / sizeof(*programs); i++) {
		int program = programs[i];
		instance *rom = create_instance();
		settle(rom, 0.4);
		queue_param(rom, P_PRESET, program);
		settle(rom, 0.1);
		int values[P_OP_END];
		bool reachable = true;
		for (int id = P_ALG; id < P_OP_END; id++) {
			values[id] = get_value(rom, id);
			if ((id - P_OP) % OP_FIELDS == OP_SR && id >= P_OP && values[id] > 15) {
				reachable = false;
			}
		}
		if (!reachable) {
			printf("  skip: program %d uses sustain rates above what the ROM's CC reaches\n", program);
			destroy_instance(rom);
			continue;
		}
		float *rom_audio = play_note(rom, 0, 60, 0.7);

		instance *knobs = create_instance();
		settle(knobs, 0.4);
		for (int id = P_ALG; id < P_OP_END; id++) {
			queue_param(knobs, id, values[id]);
		}
		settle(knobs, 0.1);
		float *knob_audio = play_note(knobs, 0, 60, 0.7);

		double *a = spectrum(rom_audio, 4000);
		double *b = spectrum(knob_audio, 4000);
		double sim = similarity(a, b);
		double level_a = rms(rom_audio, 4000, FFT_SIZE), level_b = rms(knob_audio, 4000, FFT_SIZE);
		double ratio = level_a > 0 ? level_b / level_a : 0;
		CHECK(sim > 0.97 && ratio > 0.9 && ratio < 1.1, "program %3d: spectrum %.4f, level ratio %.3f", program, sim, ratio);
		free(a);
		free(b);
		free(rom_audio);
		free(knob_audio);
		destroy_instance(rom);
		destroy_instance(knobs);
	}
}

static void test_knob_effects(void)
{
	printf("knobs change the sound\n");
	instance *in = create_instance();
	settle(in, 0.4);
	float *base = play_note(in, 0, 60, 0.7);
	double *base_spec = spectrum(base, 4000);
	double base_level = rms(base, 4000, FFT_SIZE);
	double base_high = band_energy(base_spec, 3000, 20000);

	queue_param(in, P_ALG, 7);
	settle(in, 0.05);
	float *alg = play_note(in, 0, 60, 0.7);
	double *alg_spec = spectrum(alg, 4000);
	double alg_sim = similarity(base_spec, alg_spec);
	CHECK(alg_sim < 0.95, "algorithm 7 vs default: spectrum %.3f", alg_sim);
	queue_param(in, P_ALG, 2);

	queue_param(in, P_VOLUME, 60);
	settle(in, 0.05);
	float *quiet = play_note(in, 0, 60, 0.7);
	double quiet_level = rms(quiet, 4000, FFT_SIZE);
	CHECK(quiet_level < base_level * 0.7, "volume 60: level %.4f vs %.4f", quiet_level, base_level);
	queue_param(in, P_VOLUME, 127);

	queue_param(in, P_PRESET, 80);
	settle(in, 0.05);
	float *square = play_note(in, 0, 60, 0.7);
	double *square_dull = spectrum(square, 4000);
	double dull = band_energy(square_dull, 3000, 20000);
	queue_param(in, P_FILTER, 1000);
	settle(in, 0.05);
	float *bright = play_note(in, 0, 60, 0.7);
	double *bright_spec = spectrum(bright, 4000);
	double open = band_energy(bright_spec, 3000, 20000);
	CHECK(open > dull * 1.5, "filter off vs 3390 Hz: energy above 3 kHz x%.2f", open / dull);
	(void)base_high;

	queue_param(in, P_FILTER, 0);
	settle(in, 0.05);
	float *dark = play_note(in, 0, 60, 0.7);
	double *dark_spec = spectrum(dark, 4000);
	double closed = band_energy(dark_spec, 3000, 20000);
	CHECK(closed < dull * 0.7, "filter 500 Hz vs 3390 Hz: energy above 3 kHz x%.2f", closed / dull);

	queue_param(in, P_PAN, 0);
	settle(in, 0.05);
	float *left_only = play_note(in, 0, 60, 0.5);
	CHECK(rms(left_only, 4000, 8000) > 0.001, "pan left still sounds on the left output: rms %.4f", rms(left_only, 4000, 8000));
	queue_param(in, P_PAN, 127);
	settle(in, 0.05);
	float *right_only = play_note(in, 0, 60, 0.5);
	// Not fully silent: BlastEm emulates the YM2612 leaking 1/60 of a channel into its muted output.
	double leak = rms(right_only, 4000, 8000) / rms(left_only, 4000, 8000);
	CHECK(leak < 0.08, "pan right leaves only the chip's leak on the left output: x%.3f", leak);
	destroy_instance(in);
}

static void test_channels(void)
{
	printf("each channel has its own knobs\n");
	instance *in = create_instance();
	settle(in, 0.4);
	int default_alg = get_value(in, P_ALG);
	queue_param(in, ID(1, Q_PRESET), 40);
	settle(in, 0.05);
	char text[64];
	in->params->value_to_text(in->plugin, ID(1, Q_PRESET), 40, text, sizeof(text));
	CHECK(get_value(in, ID(1, Q_PRESET)) == 40, "channel 2 shows preset %s", text);
	CHECK(get_value(in, P_PRESET) == 0 && get_value(in, P_ALG) == default_alg, "channel 1 still has preset 0, algorithm %d", get_value(in, P_ALG));

	float *ch1 = play_note(in, 0, 60, 0.7);
	float *ch2 = play_note(in, 1, 60, 0.7);
	double *a = spectrum(ch1, 4000), *b = spectrum(ch2, 4000);
	double sim = similarity(a, b);
	CHECK(sim < 0.95, "channels 1 and 2 sound different: spectrum %.3f", sim);

	// Op1 TL of channel 4 changes channel 4 and nothing else. Long pauses, so no release is left.
	settle(in, 1.5);
	float *ch3_before = play_note(in, 2, 60, 0.7);
	settle(in, 1.5);
	float *ch4_before = play_note(in, 3, 60, 0.7);
	queue_param(in, ID(3, Q_OP), 0);
	queue_param(in, ID(3, Q_ALG), 7);
	settle(in, 1.5);
	float *ch3_after = play_note(in, 2, 60, 0.7);
	settle(in, 1.5);
	float *ch4_after = play_note(in, 3, 60, 0.7);
	double *s3a = spectrum(ch3_before, 4000), *s3b = spectrum(ch3_after, 4000);
	double *s4a = spectrum(ch4_before, 4000), *s4b = spectrum(ch4_after, 4000);
	CHECK(similarity(s4a, s4b) < 0.95 && similarity(s3a, s3b) > 0.97, "channel 4's knobs change channel 4 (%.3f), not channel 3 (%.3f)", similarity(s4a, s4b), similarity(s3a, s3b));

	in->params->value_to_text(in->plugin, ID(6, Q_PRESET), 80, text, sizeof(text));
	CHECK(strstr(text, "Square") != NULL, "PSG channel names its presets: %s", text);
	queue_param(in, ID(6, Q_VOLUME), 127);
	settle(in, 0.05);
	float *psg = play_note(in, 6, 72, 0.5);
	CHECK(rms(psg, 4000, 8000) > 0.005, "PSG plays: rms %.4f", rms(psg, 4000, 8000));
	destroy_instance(in);
}

static double chord_note_energy(const double *mag, int note)
{
	double hz = note_hz(note);
	return band_energy(mag, hz * 0.985, hz * 1.015);
}

// How many of a chord's notes sound in a second of audio after the given start.
static int sounding_notes(const float *audio, const uint8_t *notes, int count)
{
	double *mag = spectrum(audio, 6000);
	double e[8], top = 0;
	for (int n = 0; n < count; n++) {
		e[n] = chord_note_energy(mag, notes[n]);
		top = e[n] > top ? e[n] : top;
	}
	int sounding = 0;
	for (int n = 0; n < count; n++) {
		sounding += e[n] > top * 0.1;
	}
	free(mag);
	return sounding;
}

// Voices of silent channels go to a chord, and a note that ends where it starts holds none (D20).
static void test_voices(void)
{
	printf("voices go where they are needed\n");
	static const uint8_t chord[6] = {48, 52, 55, 59, 62, 65};
	static const uint8_t with_tune[4] = {48, 52, 55, 73};
	instance *in = create_instance();
	settle(in, 0.4);
	for (int n = 0; n < 3; n++) {
		queue_midi(in, 0x93, chord[n], 127);
	}
	queue_midi(in, 0x91, with_tune[3], 127);
	float *audio = render(in, RATE);
	CHECK(sounding_notes(audio, with_tune, 4) == 4, "a chord of 3 on channel 4 sounds whole, and channel 2 plays its own note: %d of 4", sounding_notes(audio, with_tune, 4));
	free(audio);
	queue_midi(in, 0x81, with_tune[3], 0);
	for (int n = 0; n < 3; n++) {
		queue_midi(in, 0x83, chord[n], 0);
	}
	settle(in, 0.4);
	// A note on and its note off at the same moment.
	queue_midi(in, 0x90, 70, 127);
	queue_midi(in, 0x80, 70, 0);
	settle(in, 0.1);
	for (int n = 0; n < 6; n++) {
		queue_midi(in, 0x93, chord[n], 127);
	}
	audio = render(in, RATE);
	CHECK(sounding_notes(audio, chord, 6) == 6, "then a chord of 6 has every voice: %d of 6", sounding_notes(audio, chord, 6));
	free(audio);
	destroy_instance(in);
}

typedef struct {
	// A project holds the bank, about 17 KB.
	uint8_t data[65536];
	size_t size, pos;
} memory_stream;

static int64_t stream_write(const clap_ostream_t *stream, const void *buffer, uint64_t size)
{
	memory_stream *m = stream->ctx;
	memcpy(m->data + m->size, buffer, size);
	m->size += size;
	return (int64_t)size;
}

static int64_t stream_read(const clap_istream_t *stream, void *buffer, uint64_t size)
{
	memory_stream *m = stream->ctx;
	uint64_t left = m->size - m->pos;
	uint64_t count = size < left ? size : left;
	memcpy(buffer, m->data + m->pos, count);
	m->pos += count;
	return (int64_t)count;
}

static void test_state(void)
{
	printf("project save and restore\n");
	instance *a = create_instance();
	settle(a, 0.4);
	queue_param(a, P_PRESET, 38);
	settle(a, 0.05);
	queue_param(a, P_FB, 6);
	queue_param(a, P_OP, 20);
	queue_param(a, P_FILTER, 800);
	queue_param(a, ID(1, Q_PRESET), 81);
	queue_param(a, ID(8, Q_VOLUME), 33);
	settle(a, 0.05);
	float *original = play_note(a, 0, 48, 0.7);

	static memory_stream m;
	clap_ostream_t out = {.ctx = &m, .write = stream_write};
	bool saved = a->state->save(a->plugin, &out);
	CHECK(saved && m.size > 0, "saved %zu bytes", m.size);

	// A fresh instance gets the state before any audio, as when a project opens.
	instance *b = calloc(1, sizeof(*b));
	b->plugin = factory->create_plugin(factory, &host, plugin_id);
	b->plugin->init(b->plugin);
	b->params = b->plugin->get_extension(b->plugin, CLAP_EXT_PARAMS);
	b->state = b->plugin->get_extension(b->plugin, CLAP_EXT_STATE);
	clap_istream_t in = {.ctx = &m, .read = stream_read};
	bool loaded = b->state->load(b->plugin, &in);
	CHECK(loaded, "loaded");
	const clap_plugin_render_t *render_ext = b->plugin->get_extension(b->plugin, CLAP_EXT_RENDER);
	render_ext->set(b->plugin, CLAP_RENDER_OFFLINE);
	b->plugin->activate(b->plugin, RATE, BLOCK, BLOCK);
	b->plugin->start_processing(b->plugin);

	int mismatched = 0;
	for (uint32_t i = 0; i < NUM_PARAMS; i++) {
		clap_param_info_t info;
		a->params->get_info(a->plugin, i, &info);
		mismatched += get_value(a, info.id) != get_value(b, info.id);
	}
	CHECK(mismatched == 0, "%d of %d knobs differ after restore", mismatched, NUM_PARAMS);
	settle(b, 0.4);
	float *restored = play_note(b, 0, 48, 0.7);
	double *sa = spectrum(original, 4000), *sb = spectrum(restored, 4000);
	double sim = similarity(sa, sb);
	CHECK(sim > 0.97, "restored sound matches: spectrum %.4f", sim);
	CHECK(get_value(b, ID(1, Q_PRESET)) == 81 && get_value(b, ID(8, Q_VOLUME)) == 33, "other channels are restored too: preset %d", get_value(b, ID(1, Q_PRESET)));

	// Loading into a running instance, with nothing else telling it to send anything.
	instance *c = create_instance();
	settle(c, 0.4);
	m.pos = 0;
	CHECK(c->state->load(c->plugin, &in), "loaded while running");
	settle(c, 0.1);
	float *running = play_note(c, 0, 48, 0.7);
	double *sc = spectrum(running, 4000);
	CHECK(similarity(sa, sc) > 0.97, "a state loaded while running reaches the sound: spectrum %.4f", similarity(sa, sc));

	// A host may process with its output off. The engine keeps time and nothing breaks.
	clap_input_events_t no_events = {.ctx = &c->pending, .size = events_size, .get = events_get};
	clap_output_events_t no_out = {.ctx = NULL, .try_push = out_try_push};
	clap_process_t silent = {.frames_count = BLOCK, .audio_outputs_count = 0, .in_events = &no_events, .out_events = &no_out};
	for (int i = 0; i < 16; i++) {
		c->plugin->process(c->plugin, &silent);
	}
	float *after_silent = play_note(c, 0, 48, 0.7);
	double *sd = spectrum(after_silent, 4000);
	CHECK(similarity(sa, sd) > 0.97, "processing without an output changes nothing: spectrum %.4f", similarity(sa, sd));
	destroy_instance(c);

	static memory_stream junk;
	junk.size = 10;
	junk.pos = 0;
	clap_istream_t bad = {.ctx = &junk, .read = stream_read};
	CHECK(!b->state->load(b->plugin, &bad), "a damaged state is refused");
	destroy_instance(a);
	destroy_instance(b);
}

// Strongest partial in a band, from a spectrum of FFT_SIZE samples.
static double peak_hz(const double *mag, double low_hz, double high_hz)
{
	int low = (int)(low_hz * FFT_SIZE / RATE), high = (int)(high_hz * FFT_SIZE / RATE);
	int best = low;
	for (int i = low; i < high; i++) {
		if (mag[i] > mag[best]) {
			best = i;
		}
	}
	double a = mag[best - 1], b = mag[best], c = mag[best + 1];
	double shift = (a - c) / (2 * (a - 2 * b + c));
	return (best + shift) * RATE / FFT_SIZE;
}

// Power at one frequency over a short window.
static double power_at(const float *samples, uint32_t start, uint32_t count, double hz)
{
	double re = 0, im = 0;
	for (uint32_t i = 0; i < count; i++) {
		double window = 0.5 - 0.5 * cos(2 * M_PI * i / (count - 1));
		re += samples[start + i] * window * cos(2 * M_PI * hz * i / RATE);
		im += samples[start + i] * window * sin(2 * M_PI * hz * i / RATE);
	}
	return (re * re + im * im) / ((double)count * count);
}

static void test_tuning_knobs(void)
{
	printf("tuning knobs\n");
	instance *in = create_instance();
	settle(in, 0.4);
	char text[64];
	in->params->value_to_text(in->plugin, P_FINE, 127, text, sizeof(text));
	float *base_audio = play_note(in, 0, 60, 0.7);
	double *base = spectrum(base_audio, 4000);
	double base_hz = peak_hz(base, 200, 330);

	queue_param(in, P_FINE, 127);
	settle(in, 0.05);
	float *sharp_audio = play_note(in, 0, 60, 0.7);
	double *sharp = spectrum(sharp_audio, 4000);
	double fine_cents = 1200 * log2(peak_hz(sharp, 200, 330) / base_hz);
	CHECK(fine_cents > 50 && fine_cents < 76, "fine tune at %s: pitch moved %+.1f cents", text, fine_cents);
	queue_param(in, P_FINE, 64);

	queue_midi(in, 0xE0, 0x7F, 0x7F);
	settle(in, 0.05);
	float *bent2_audio = play_note(in, 0, 60, 0.7);
	double *bent2 = spectrum(bent2_audio, 4000);
	double semis2 = 12 * log2(peak_hz(bent2, 240, 360) / base_hz);
	CHECK(semis2 > 1.7 && semis2 < 2.3, "default bend range: full bend moves %+.2f semitones", semis2);
	queue_param(in, P_BEND, 12);
	settle(in, 0.05);
	float *bent12_audio = play_note(in, 0, 60, 0.7);
	double *bent12 = spectrum(bent12_audio, 4000);
	double semis12 = 12 * log2(peak_hz(bent12, 470, 600) / base_hz);
	CHECK(semis12 > 11.5 && semis12 < 12.5, "bend range 12: full bend moves %+.2f semitones", semis12);
	queue_midi(in, 0xE0, 0x00, 0x40);
	settle(in, 0.05);

	// Legato jump of an octave: with glide the target pitch must not be there yet.
	double target = note_hz(72);
	double powers[2];
	for (int on = 0; on < 2; on++) {
		queue_param(in, P_PORTA_ON, on);
		queue_param(in, P_PORTA_TIME, 110);
		queue_midi(in, 0x90, 60, 127);
		settle(in, 0.3);
		queue_midi(in, 0x90, 72, 127);
		float *jump = render(in, RATE / 4);
		powers[on] = power_at(jump, 3000, 4096, target);
		queue_midi(in, 0x80, 72, 0);
		queue_midi(in, 0x80, 60, 0);
		settle(in, 0.3);
		free(jump);
	}
	CHECK(powers[1] < powers[0] * 0.25, "portamento: power at the target 60 ms in, on x%.3f of off", powers[1] / powers[0]);
	destroy_instance(in);
}

static void test_mode_knobs(void)
{
	printf("mode knobs\n");
	instance *in = create_instance();
	settle(in, 0.4);
	// Toggling the DAC clears the YM2612, so every FM channel has to come back as it was: a knob
	// tweak on channel 1, a preset with values CCs can't reach on channel 2, plain defaults on channel 6.
	queue_param(in, P_FB, 6);
	queue_param(in, P_OP + 1, 3);
	settle(in, 0.05);
	queue_param(in, ID(1, Q_PRESET), 16);
	settle(in, 0.05);
	float *ch1_before = play_note(in, 0, 60, 0.7);
	double *ch1_spec = spectrum(ch1_before, 4000);
	float *ch2_before = play_note(in, 1, 60, 0.7);
	double *ch2_spec = spectrum(ch2_before, 4000);
	float *fm_audio = play_note(in, 5, 36, 0.7);
	double *fm = spectrum(fm_audio, 2000);

	queue_param(in, P_DAC, 1);
	settle(in, 0.1);
	float *dac_audio = play_note(in, 5, 36, 0.7);
	double dac_level = rms(dac_audio, 700, RATE / 4);
	double *dac = spectrum(dac_audio, 2000);
	double dac_sim = similarity(fm, dac);
	CHECK(dac_level > 0.002 && dac_sim < 0.9, "DAC on: channel 6 plays a sample, rms %.4f, spectrum vs FM %.3f", dac_level, dac_sim);
	float *ch1_dac = play_note(in, 0, 60, 0.7);
	double *ch1_dac_spec = spectrum(ch1_dac, 4000);
	CHECK(similarity(ch1_spec, ch1_dac_spec) > 0.97, "DAC on: channel 1 keeps its tweaked sound, spectrum %.3f", similarity(ch1_spec, ch1_dac_spec));

	queue_param(in, P_DAC, 0);
	settle(in, 0.1);
	float *ch1_after = play_note(in, 0, 60, 0.7);
	double *ch1_after_spec = spectrum(ch1_after, 4000);
	float *ch2_after = play_note(in, 1, 60, 0.7);
	double *ch2_after_spec = spectrum(ch2_after, 4000);
	float *back_audio = play_note(in, 5, 36, 0.7);
	double *back = spectrum(back_audio, 2000);
	CHECK(similarity(ch1_spec, ch1_after_spec) > 0.97, "DAC off: channel 1 keeps its tweaked sound, spectrum %.3f", similarity(ch1_spec, ch1_after_spec));
	CHECK(similarity(ch2_spec, ch2_after_spec) > 0.97, "DAC off: channel 2 keeps a preset with SR above 15, spectrum %.3f", similarity(ch2_spec, ch2_after_spec));
	CHECK(similarity(fm, back) > 0.97, "DAC off: channel 6 is FM again, spectrum %.3f", similarity(fm, back));

	queue_midi(in, 0xB0, 78, 127);
	settle(in, 0.1);
	queue_midi(in, 0xB0, 78, 0);
	settle(in, 0.1);
	float *ch1_clip = play_note(in, 0, 60, 0.7);
	double *ch1_clip_spec = spectrum(ch1_clip, 4000);
	CHECK(similarity(ch1_spec, ch1_clip_spec) > 0.97, "CC78 from a clip recovers too, spectrum %.3f", similarity(ch1_spec, ch1_clip_spec));
	destroy_instance(in);

	// Channels 11-13 set the frequencies of Ch3's operators 1-3 in special mode, and do nothing otherwise.
	double sims[2];
	for (int mode = 0; mode < 2; mode++) {
		in = create_instance();
		settle(in, 0.4);
		queue_param(in, P_CH3, mode);
		settle(in, 0.05);
		float *plain = play_note(in, 2, 60, 0.7);
		double *plain_spec = spectrum(plain, 4000);
		settle(in, 0.2);
		queue_midi(in, 0x9A, 79, 127);
		queue_midi(in, 0x9B, 84, 127);
		queue_midi(in, 0x9C, 91, 127);
		settle(in, 0.05);
		float *split = play_note(in, 2, 60, 0.7);
		double *split_spec = spectrum(split, 4000);
		sims[mode] = similarity(plain_spec, split_spec);
		destroy_instance(in);
	}
	CHECK(sims[0] > 0.97, "special mode off: channels 11-13 leave channel 3 alone, spectrum %.3f", sims[0]);
	CHECK(sims[1] < 0.9, "special mode on: channels 11-13 retune channel 3's operators, spectrum %.3f", sims[1]);

	in = create_instance();
	settle(in, 0.4);
	queue_midi(in, 0xB0, 85, 100);
	queue_midi(in, 0xB0, 5, 30);
	queue_midi(in, 0xB0, 65, 127);
	queue_midi(in, 0xB0, 78, 127);
	queue_midi(in, 0xB0, 80, 127);
	queue_midi(in, 0xB0, 101, 0);
	queue_midi(in, 0xB0, 100, 0);
	queue_midi(in, 0xB0, 6, 7);
	settle(in, 0.05);
	CHECK(get_value(in, P_FINE) == 100 && get_value(in, P_PORTA_TIME) == 30 && get_value(in, P_PORTA_ON) == 1, "knobs follow CC85, CC5, CC65");
	CHECK(get_value(in, P_DAC) == 1 && get_value(in, P_CH3) == 1, "knobs follow CC78, CC80");
	CHECK(get_value(in, P_BEND) == 7, "bend range knob follows RPN 0,0 to %d", get_value(in, P_BEND));
	queue_midi(in, 0xB0, 101, 127);
	queue_midi(in, 0xB0, 100, 127);
	queue_midi(in, 0xB0, 6, 20);
	settle(in, 0.05);
	CHECK(get_value(in, P_BEND) == 7, "data entry without the RPN leaves it alone");
	destroy_instance(in);
}

// The VST3 wrapper hands notes over before knobs of the same moment. A preset point exactly on a
// note's start must still be meant for that note.
static void test_same_moment(void)
{
	printf("a preset at the start of a note\n");
	instance *first = create_instance();
	settle(first, 0.4);
	queue_param(first, P_PRESET, 80);
	settle(first, 0.1);
	float *expected = play_note(first, 0, 60, 0.7);
	destroy_instance(first);
	instance *plain = create_instance();
	settle(plain, 0.4);
	float *other = play_note(plain, 0, 60, 0.7);
	destroy_instance(plain);
	instance *same = create_instance();
	settle(same, 0.4);
	queue_midi(same, 0x90, 60, 127);
	queue_param(same, P_PRESET, 80);
	float *together = render(same, (uint32_t)(0.7 * RATE));
	destroy_instance(same);
	double *a = spectrum(expected, 4000), *b = spectrum(together, 4000), *c = spectrum(other, 4000);
	CHECK(similarity(a, b) > 0.97 && similarity(c, b) < 0.95, "the note sounds with the new preset: %.3f, with the old one %.3f", similarity(a, b), similarity(c, b));
}

static void test_midi_tracking(void)
{
	printf("knobs follow MIDI from clips\n");
	instance *in = create_instance();
	settle(in, 0.4);
	queue_midi(in, 0xB0, 14, 48);
	settle(in, 0.05);
	CHECK(get_value(in, P_ALG) == 3, "CC14 moves Algorithm to %d", get_value(in, P_ALG));
	queue_midi(in, 0xB0, 16, 99);
	settle(in, 0.05);
	CHECK(get_value(in, P_OP) == 99, "CC16 moves Op1 TL to %d", get_value(in, P_OP));
	queue_midi(in, 0xC0, 33, 0);
	settle(in, 0.05);
	CHECK(get_value(in, P_PRESET) == 33, "program change moves Preset to %d", get_value(in, P_PRESET));
	const int ch1_tl = get_value(in, P_OP), ch1_volume = get_value(in, P_VOLUME);
	queue_midi(in, 0xB2, 16, 77);
	queue_midi(in, 0xB2, 7, 90);
	queue_midi(in, 0xB8, 7, 50);
	settle(in, 0.05);
	CHECK(get_value(in, ID(2, Q_OP)) == 77 && get_value(in, ID(2, Q_VOLUME)) == 90 && get_value(in, ID(8, Q_VOLUME)) == 50
		&& get_value(in, P_OP) == ch1_tl && get_value(in, P_VOLUME) == ch1_volume,
		"a CC moves the knob of its own channel only");
	queue_midi(in, 0xB2, 3, 50);
	settle(in, 0.05);
	CHECK(get_value(in, ID(2, Q_PRESET)) == 50 && get_value(in, ID(2, Q_OP)) != 77, "the preset CC loads a new preset on channel 3");
	queue_param(in, ID(2, Q_OP), 77);
	queue_midi(in, 0xB2, 3, 50);
	settle(in, 0.05);
	CHECK(get_value(in, ID(2, Q_OP)) == 77, "and the same preset again keeps the tweak");
	queue_midi(in, 0xC2, 50, 0);
	settle(in, 0.05);
	CHECK(get_value(in, ID(2, Q_OP)) != 77, "a program change wipes it");

	int before = rescans;
	callbacks_requested = 0;
	queue_param(in, P_PRESET, 12);
	settle(in, 0.05);
	CHECK(callbacks_requested > 0, "preset change asks for the main thread, to mark the project changed");
	in->plugin->on_main_thread(in->plugin);
	CHECK(rescans == before, "but the host isn't asked to re-read the knobs, which would stop Live's automation");
	destroy_instance(in);
}

int main(int argc, char **argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: %s <plugin.clap>\n", argv[0]);
		return 2;
	}
	setvbuf(stdout, NULL, _IONBF, 0);
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
	factory = entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
	plugin_id = factory->get_plugin_descriptor(factory, 0)->id;

	test_listing();
	test_text();
	test_preset_table();
	test_knob_effects();
	test_channels();
	test_voices();
	test_state();
	test_midi_tracking();
	test_same_moment();
	test_tuning_knobs();
	test_mode_knobs();

	entry->deinit();
	printf("%s: %d failed\n", failures ? "FAILED" : "PASSED", failures);
	return failures ? 1 : 0;
}
