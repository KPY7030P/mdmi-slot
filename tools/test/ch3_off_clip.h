// Stage 21, item 8: a project as 0.20 had it, Ch3 Special off, and a fixed clip on FM 1-6 and PSG.
// reg_test plays it on the built ROM; gen_reference.c drives 0.20's patch.c with the same calls to
// make tools/test/files/ch3-off-0.20.bin. Only ids 0.20 had, so both builds read it alike.
#ifndef CH3_OFF_CLIP_H
#define CH3_OFF_CLIP_H

#include <stdint.h>
#include <string.h>
#include "patch.h"

// Room for a saved state of either build.
#define CH3_OFF_STATE_BYTES 4096

#define CH3_OFF_RATE 48000
#define CH3_OFF_BLOCK 256
// Blocks of 5.3 ms rendered after the boot, before the clip starts.
#define CH3_OFF_SETTLE 24
// The held snapshot, then the end, both in blocks from the clip's start.
#define CH3_OFF_HELD_AT 140
#define CH3_OFF_END 420
// The dealer compares a voice's release with the clock, so the clock starts well past any release.
#define CH3_OFF_CLOCK_START ((uint64_t)CH3_OFF_RATE * 3600)

static void ch3_off_project(mdmi_patch *p)
{
	patch_set(p, PATCH_CHANNEL_ID(0, PARAM_PRESET), 7);
	patch_set(p, PATCH_CHANNEL_ID(1, PARAM_PRESET), 40);
	patch_set(p, PATCH_CHANNEL_ID(1, PARAM_OP_BASE), 20);
	patch_set(p, PATCH_CHANNEL_ID(1, PARAM_FMS), 5);
	patch_set(p, PATCH_CHANNEL_ID(2, PARAM_PRESET), 19);
	patch_set(p, PATCH_CHANNEL_ID(2, PARAM_PAN), 0);
	patch_set(p, PATCH_CHANNEL_ID(3, PARAM_PRESET), 33);
	patch_set(p, PATCH_CHANNEL_ID(3, PARAM_ALGORITHM), 4);
	patch_set(p, PATCH_CHANNEL_ID(4, PARAM_VOLUME), 90);
	patch_set(p, PATCH_CHANNEL_ID(5, PARAM_PRESET), 81);
	patch_set(p, PATCH_CHANNEL_ID(6, PARAM_PRESET), 80);
	patch_set(p, PATCH_CHANNEL_ID(7, PARAM_PRESET), 5);
	patch_set(p, GLOBAL_LFO_ON, 1);
	patch_set(p, GLOBAL_LFO_RATE, 3);
}

typedef struct {
	uint16_t block;
	uint8_t status, data1, data2;
} ch3_off_event;

// Notes, a chord on borrowed voices, bend, volume, pan and a program change; all off by block 260.
static const ch3_off_event CH3_OFF_CLIP[] = {
	{0, 0x90, 60, 100},
	{0, 0x90, 64, 90},
	{10, 0x91, 48, 127},
	{20, 0x92, 55, 80},
	{22, 0xE2, 0x00, 0x50},
	{30, 0xB3, 7, 100},
	{30, 0xB3, 10, 20},
	{32, 0x93, 62, 110},
	{35, 0xC4, 30, 0},
	{40, 0x94, 67, 127},
	{50, 0x95, 72, 64},
	{60, 0x96, 72, 127},
	{62, 0x97, 76, 100},
	{64, 0x98, 79, 90},
	{66, 0x99, 60, 127},
	{150, 0x80, 64, 0},
	{152, 0x80, 60, 0},
	{160, 0x90, 65, 127},
	{170, 0x96, 74, 127},
	{200, 0x81, 48, 0},
	{200, 0x82, 55, 0},
	{201, 0xE2, 0x00, 0x40},
	{202, 0x83, 62, 0},
	{204, 0x84, 67, 0},
	{206, 0x85, 72, 0},
	{210, 0x86, 72, 0},
	{211, 0x86, 74, 0},
	{212, 0x87, 76, 0},
	{214, 0x88, 79, 0},
	{216, 0x89, 60, 0},
	{260, 0x80, 65, 0},
};
#define CH3_OFF_EVENTS ((int)(sizeof(CH3_OFF_CLIP) / sizeof(CH3_OFF_CLIP[0])))

// Opens a 0.20 project in a fresh p: the values above saved by scratch, without ids 10-23, which
// 0.20 didn't have. The caller sets p->owner afterwards.
static bool ch3_off_load(mdmi_patch *p, mdmi_patch *scratch)
{
	static uint8_t state[CH3_OFF_STATE_BYTES];
	const size_t size = patch_save_size();
	if (size > sizeof(state)) {
		return false;
	}
	patch_init(scratch);
	ch3_off_project(scratch);
	patch_publish(scratch);
	patch_save(patch_view(scratch), state);
	size_t kept = 0;
	for (size_t i = 0; i < size; i += 6) {
		uint32_t id;
		memcpy(&id, state + i, 4);
		// Globals 10-23 are stage 21's; a channel's ids start at 1100.
		if (id < 10 || id >= 24) {
			memmove(state + kept, state + i, 6);
			kept += 6;
		}
	}
	patch_init(p);
	return patch_load(p, state, kept, NULL, NULL, NULL, NULL) && patch_take_incoming(p);
}

static void ch3_off_send(mdmi_patch *p, const ch3_off_event *e, patch_emit_fn emit, void *context)
{
	const uint8_t message[3] = {e->status, e->data1, e->data2};
	patch_midi(p, message, (e->status & 0xF0) == 0xC0 ? 2 : 3, emit, context);
	patch_flush(p, emit, context);
}

// From the moment the ROM has booted: what the plugin sends it, CH3_OFF_SETTLE blocks, then the
// clip block by block. block() renders one block and runs the dealer's clock (patch_due); held()
// runs before block CH3_OFF_HELD_AT, done() after the last.
static void ch3_off_play(mdmi_patch *p, patch_emit_fn emit, void *context, void (*block)(void), void (*held)(void), void (*done)(void))
{
	patch_rom_booted(p, emit, context);
	for (int i = 0; i < CH3_OFF_SETTLE; i++) {
		block();
	}
	int next = 0;
	for (int b = 0; b < CH3_OFF_END; b++) {
		while (next < CH3_OFF_EVENTS && CH3_OFF_CLIP[next].block == b) {
			ch3_off_send(p, &CH3_OFF_CLIP[next++], emit, context);
		}
		if (b == CH3_OFF_HELD_AT && held) {
			held();
		}
		block();
	}
	if (done) {
		done();
	}
}

#endif
