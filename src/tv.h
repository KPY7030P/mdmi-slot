// Stage 36 (D81): the ROM screen as the console looks on a TV, in the window only (docs/design/tv-look.md).
#pragma once
#include <stdint.h>

enum { TV_OFF, TV_RGB, TV_COMP, TV_LOOKS };

// Draws a 320-wide frame (0x00RRGGBB, `stride` pixels a line) at x2 into `out` (640 wide, `out_stride`
// pixels a line, 2 * height lines): RGB is the TV's gamma and its scanlines, COMP a cheap composite TV
// first. The frame is only read. `cache` keeps the last lines drawn, so only the lines that changed
// are worked out again; NULL works too, slower.
typedef struct tv_cache tv_cache;
tv_cache *tv_cache_new(void);
void tv_cache_free(tv_cache *cache);
void tv_draw(tv_cache *cache, int look, const uint32_t *frame, int stride, int height, uint32_t *out, int out_stride);
