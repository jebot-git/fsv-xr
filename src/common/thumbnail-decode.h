// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Bounded preview decoder. No full-resolution image/canvas is allocated.
 * JPEG uses decoder downscaling, PNG/GIF use scanlines, BMP samples rows.
 * All outputs are RGBA8, capped at 1024 per edge. Invalid/over-budget images
 * return zero so callers can display their normal file-type placeholder. */
struct fsv_thumbnail {
	int width, height, source_width, source_height, decoder_width;
	int kind; /* 1 JPEG, 2 PNG, 3 GIF, 4 BMP; retained on rejected known
		     input. */
	char uri[4096], mtime[32];
	unsigned char *rgba;
};
typedef int (*fsv_thumbnail_cancel)(void *);
int fsv_thumbnail_decode(const char *path, int max_width, int max_height,
			 struct fsv_thumbnail *, fsv_thumbnail_cancel, void *);
void fsv_thumbnail_free(struct fsv_thumbnail *);
#ifdef __cplusplus
}
#endif
