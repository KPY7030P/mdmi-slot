// Makes tools/test/files/ch3-off-0.20.bin, the reference of stage 21, item 8 (a): every MIDI byte
// 0.20's patch.c (f94414f) sends from a booted ROM for the project and clip of ch3_off_clip.h, with
// the dealer's clock run block by block as reg_test runs it. No engine: the bytes don't depend on it.
// reg_test sends the same calls through today's patch.c and compares, stage 21's CCs taken out.
//
// Run from the repository root in Git Bash, with T a scratch folder (nothing is checked out):
//   mkdir -p $T/old && for f in patch.c patch.h presets_gm.h; do git show f94414f:src/$f > $T/old/$f; done
//   MSYS_NO_PATHCONV=1 docker run --rm --network none -v "$(cygpath -m .):/src" -v "$(cygpath -m $T):/t" -w /src mdmi-plugin-build x86_64-w64-mingw32-gcc -O2 -Wall -I/t/old -o /t/gen_reference.exe tools/test/gen_reference.c /t/old/patch.c -lm
//   $T/gen_reference.exe tools/test/files/ch3-off-0.20.bin
//
// The second reference, ch3-off-0.20-regs.bin (item 8 (b)), is the registers 0.20's ROM holds after
// that clip; reg_test writes it itself when given 0.20's ROM (see test_ch3_off in reg_test.c).
#include <stdio.h>
#include <stdlib.h>
#include "ch3_off_clip.h"

static uint8_t sent[16384];
static size_t sent_bytes;
static mdmi_patch model, scratch;
static uint64_t clock_samples = CH3_OFF_CLOCK_START;

static void record(void *context, const uint8_t *bytes, size_t length)
{
	(void)context;
	if (sent_bytes + length <= sizeof(sent)) {
		memcpy(sent + sent_bytes, bytes, length);
	}
	sent_bytes += length;
}

static void owner(void *context, int voice, int channel, bool at_once)
{
	(void)context;
	(void)voice;
	(void)channel;
	(void)at_once;
}

// reg_test's tick without the render.
static void block(void)
{
	patch_set_now(&model, clock_samples, CH3_OFF_RATE);
	patch_due(&model, record, NULL);
	clock_samples += CH3_OFF_BLOCK;
	patch_set_now(&model, clock_samples, CH3_OFF_RATE);
}

int main(int argc, char **argv)
{
	if (argc != 2) {
		fprintf(stderr, "gen_reference <output file>\n");
		return 1;
	}
	patch_global_init();
	if (!ch3_off_load(&model, &scratch)) {
		fprintf(stderr, "the project doesn't load\n");
		return 1;
	}
	model.owner = owner;
	patch_set_now(&model, clock_samples, CH3_OFF_RATE);
	ch3_off_play(&model, record, NULL, block, NULL, NULL);
	if (sent_bytes > sizeof(sent)) {
		fprintf(stderr, "%d bytes, more than %d\n", (int)sent_bytes, (int)sizeof(sent));
		return 1;
	}
	FILE *file = fopen(argv[1], "wb");
	if (!file || fwrite(sent, 1, sent_bytes, file) != sent_bytes || fclose(file)) {
		fprintf(stderr, "can't write %s\n", argv[1]);
		return 1;
	}
	printf("%d bytes\n", (int)sent_bytes);
	return 0;
}
