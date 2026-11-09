#ifndef MDMI_ENGINE_H
#define MDMI_ENGINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// One emulated Mega Drive running the MDMI ROM on its own thread. Every engine gets
// a private copy of the BlastEm DLL because the core keeps its state in globals.
typedef struct mdmi_engine mdmi_engine;

// The chip's voices: FM 1-6 (the DAC on 6), PSG tones 1-3, noise. Each plays into the output of the
// MIDI channel whose note it last started (D46); MIDI channels 1-10 have an output each.
#define MDMI_VOICES 10
#define MDMI_OUTPUTS 10
// Main left and right, then each output's left and right.
#define MDMI_RENDER_CHANNELS (2 + 2 * MDMI_OUTPUTS)

typedef struct {
	uint32_t underruns;
	uint32_t dropped_midi;
	bool failed;
	bool booted;
	double worst_run_ms;
	double worst_wait_ms;
	// Boots done so far: a restart with another image adds one (stage 12).
	uint32_t boots;
	double last_boot_ms;
	// Images written into the running core instead of a restart (D39).
	uint32_t patches;
	double last_patch_ms;
	const char *boot_stage;
	uint32_t boot_error;
	uint64_t worst_run_pos;
	uint64_t first_underrun_pos;
	uint64_t last_underrun_pos;
	bool active;
	uint32_t last_block;
	uint32_t fill_target;
	uint32_t worst_short;
	uint32_t longest_burst;
	uint32_t sample_rate;
	// Stage 17: owners whose voice didn't key on within a second, the longest wait
	// from an owner to its key-on, batches whose voices did not come, and how far the voices' sum
	// strayed from the core's own mix after the DC blocker, in its 16-bit steps.
	uint32_t owner_misses;
	float owner_wait_ms;
	uint32_t voice_batch_misses;
	float voice_mix_error;
} mdmi_engine_stats;

// The ROM's screen. A frame stays valid until the next call to mdmi_engine_latest_frame.
#define MDMI_FRAME_MAX_WIDTH 352
#define MDMI_FRAME_MAX_HEIGHT 256
typedef struct {
	uint32_t width;
	uint32_t height;
	uint32_t pixels[MDMI_FRAME_MAX_WIDTH * MDMI_FRAME_MAX_HEIGHT];
} mdmi_frame;

bool mdmi_engine_global_init(void);
void mdmi_engine_global_deinit(void);

mdmi_engine *mdmi_engine_create(void);
void mdmi_engine_destroy(mdmi_engine *e);

// Main thread, not while processing.
bool mdmi_engine_activate(mdmi_engine *e, double sample_rate, uint32_t max_block);
void mdmi_engine_deactivate(mdmi_engine *e);
uint32_t mdmi_engine_latency(const mdmi_engine *e);
void mdmi_engine_set_offline(mdmi_engine *e, bool offline);
// Audio thread: whether the host's transport plays, for the journal (stage 13).
void mdmi_engine_set_playing(mdmi_engine *e, bool playing);
// Any thread. Applied by the emulation thread before its next step.
void mdmi_engine_set_filter(mdmi_engine *e, double cutoff_hz);
mdmi_engine_stats mdmi_engine_get_stats(const mdmi_engine *e);
// Any thread. How many times the core has booted.
uint32_t mdmi_engine_boots(const mdmi_engine *e);
// The ROM image built into the plugin.
const uint8_t *mdmi_engine_builtin_rom(size_t *size);
// Main thread. The core takes this image next, copied: before the first boot instead of the built-in
// one; after it written into the running core when only its samples differ (D39), else as a restart
// on the engine's own thread, silent while it boots. Stats show both and their time.
bool mdmi_engine_set_rom(mdmi_engine *e, const uint8_t *bytes, size_t size);
// Tests only, before activate: the emulator computes no further than the block being rendered, so
// after mdmi_engine_render it stands at the step past the audio, whatever the scheduler did. It may
// still be finishing that step by a few cycles while the test reads it.
void mdmi_engine_test_lockstep(mdmi_engine *e, bool lockstep);
// Tests only: the longest wait from an owner to its key-on since the last call, in ms.
float mdmi_engine_test_take_owner_wait(mdmi_engine *e);
// Tests only: what the running YM2612 holds in a register.
uint8_t mdmi_engine_test_ym(const mdmi_engine *e, uint8_t part, uint8_t reg);
// The attenuation of a PSG channel, 0-15, for tests.
uint8_t mdmi_engine_test_psg_volume(const mdmi_engine *e, uint8_t channel);
// A tone channel's period, or the noise channel's type and rate bits, for tests.
uint16_t mdmi_engine_test_psg_tone(const mdmi_engine *e, uint8_t channel);
// Main thread. Frames are only copied while someone is watching.
void mdmi_engine_watch_frames(mdmi_engine *e, bool watching);
const mdmi_frame *mdmi_engine_latest_frame(mdmi_engine *e);

// Audio thread. Queue a block's MIDI first, then render that block.
void mdmi_engine_midi(mdmi_engine *e, uint32_t sample_offset, const uint8_t *bytes, size_t length);
void mdmi_engine_all_notes_off(mdmi_engine *e, uint32_t sample_offset);
// The voice's next note belongs to channel 0-9 (MIDI 1-10): queued with the block's MIDI, it takes
// the voice from the voice's next key-on on, so a sounding tail stays where it was.
void mdmi_engine_voice_owner(mdmi_engine *e, uint32_t sample_offset, int voice, int channel);
// Main only.
void mdmi_engine_render(mdmi_engine *e, float *left, float *right, uint32_t frames);
// MDMI_RENDER_CHANNELS buffers, any of them NULL.
void mdmi_engine_render_outputs(mdmi_engine *e, float *const *channels, uint32_t frames);

#endif
