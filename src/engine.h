#ifndef MDMI_ENGINE_H
#define MDMI_ENGINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// One emulated Mega Drive running the MDMI ROM on its own thread. Every engine gets
// a private copy of the BlastEm DLL because the core keeps its state in globals.
typedef struct mdmi_engine mdmi_engine;

typedef struct {
	uint32_t underruns;
	uint32_t dropped_midi;
	bool failed;
	bool booted;
	double worst_run_ms;
	double worst_wait_ms;
	uint64_t worst_run_pos;
	uint64_t first_underrun_pos;
	uint64_t last_underrun_pos;
} mdmi_engine_stats;

bool mdmi_engine_global_init(void);
void mdmi_engine_global_deinit(void);

mdmi_engine *mdmi_engine_create(void);
void mdmi_engine_destroy(mdmi_engine *e);

// Main thread, not while processing.
bool mdmi_engine_activate(mdmi_engine *e, double sample_rate, uint32_t max_block);
void mdmi_engine_deactivate(mdmi_engine *e);
uint32_t mdmi_engine_latency(const mdmi_engine *e);
void mdmi_engine_set_offline(mdmi_engine *e, bool offline);
// Any thread. Applied by the emulation thread before its next step.
void mdmi_engine_set_filter(mdmi_engine *e, double cutoff_hz);
mdmi_engine_stats mdmi_engine_get_stats(const mdmi_engine *e);

// Audio thread. Queue a block's MIDI first, then render that block.
void mdmi_engine_midi(mdmi_engine *e, uint32_t sample_offset, const uint8_t *bytes, size_t length);
void mdmi_engine_all_notes_off(mdmi_engine *e, uint32_t sample_offset);
void mdmi_engine_render(mdmi_engine *e, float *left, float *right, uint32_t frames);

#endif
