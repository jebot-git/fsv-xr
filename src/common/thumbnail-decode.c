// SPDX-License-Identifier: LGPL-2.1-or-later
#include "thumbnail-decode.h"

#include <fcntl.h>
#include <gif_lib.h>
// clang-format off
#include <stdio.h>
#include <jpeglib.h>
// clang-format on
#include <png.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#define ROW_LIMIT (16u * 1024u * 1024u)
#define INPUT_LIMIT (64u * 1024u * 1024u)
struct control {
	fsv_thumbnail_cancel cancel;
	void *user;
	struct timespec start;
};
static int stopped(struct control *c) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (c->cancel && c->cancel(c->user)) ||
	       now.tv_sec - c->start.tv_sec >= 3;
}
static int output(struct fsv_thumbnail *out, unsigned width, unsigned height,
		  int mw, int mh) {
	double factor;
	if (!width || !height || width > 1048576 || height > 1048576) return 0;
	factor = (double)mw / width;
	if ((double)mh / height < factor) factor = (double)mh / height;
	if (factor > 1) factor = 1;
	out->width = (int)(width * factor);
	out->height = (int)(height * factor);
	if (out->width < 1) out->width = 1;
	if (out->height < 1) out->height = 1;
	out->source_width = (int)width;
	out->source_height = (int)height;
	out->rgba = calloc((size_t)out->width * out->height, 4);
	return out->rgba != NULL;
}
static unsigned sample(unsigned p, unsigned source, unsigned target) {
	return (unsigned)(((uint64_t)p * 2 + 1) * source / (target * 2));
}
static unsigned exif16(const unsigned char *p, int little) {
	return little ? (unsigned)p[0] | (unsigned)p[1] << 8
		      : (unsigned)p[1] | (unsigned)p[0] << 8;
}
static uint32_t exif32(const unsigned char *p, int little) {
	return little
		   ? (uint32_t)exif16(p, 1) | (uint32_t)exif16(p + 2, 1) << 16
		   : (uint32_t)exif16(p + 2, 0) | (uint32_t)exif16(p, 0) << 16;
}
static int jpeg_orientation(jpeg_saved_marker_ptr marker) {
	for (; marker; marker = marker->next) {
		const unsigned char *data = marker->data;
		size_t length = marker->data_length;
		if (length < 14 || memcmp(data, "Exif\0\0", 6)) continue;
		data += 6;
		length -= 6;
		int little = data[0] == 'I' && data[1] == 'I';
		if (!little && !(data[0] == 'M' && data[1] == 'M')) continue;
		if (exif16(data + 2, little) != 42) continue;
		uint32_t offset = exif32(data + 4, little);
		if (offset > length - 2) continue;
		unsigned entries = exif16(data + offset, little);
		offset += 2;
		for (unsigned n = 0;
		     n < entries && offset <= length && length - offset >= 12;
		     ++n, offset += 12) {
			const unsigned char *entry = data + offset;
			if (exif16(entry, little) == 0x112 &&
			    exif16(entry + 2, little) == 3 &&
			    exif32(entry + 4, little) == 1) {
				unsigned value = exif16(entry + 8, little);
				if (value >= 1 && value <= 8) return (int)value;
			}
		}
	}
	return 1;
}
static int orient(struct fsv_thumbnail *out, int orientation) {
	if (orientation == 1) return 1;
	int w = out->width, h = out->height, nw = orientation >= 5 ? h : w,
	    nh = orientation >= 5 ? w : h;
	unsigned char *rotated = malloc((size_t)w * h * 4);
	if (!rotated) return 0;
	for (int y = 0; y < h; ++y)
		for (int x = 0; x < w; ++x) {
			int dx = x, dy = y;
			switch (orientation) {
				case 2:
					dx = w - 1 - x;
					break;
				case 3:
					dx = w - 1 - x;
					dy = h - 1 - y;
					break;
				case 4:
					dy = h - 1 - y;
					break;
				case 5:
					dx = y;
					dy = x;
					break;
				case 6:
					dx = h - 1 - y;
					dy = x;
					break;
				case 7:
					dx = h - 1 - y;
					dy = w - 1 - x;
					break;
				case 8:
					dx = y;
					dy = w - 1 - x;
					break;
			}
			memcpy(rotated + ((size_t)dy * nw + dx) * 4,
			       out->rgba + ((size_t)y * w + x) * 4, 4);
		}
	free(out->rgba);
	out->rgba = rotated;
	out->width = nw;
	out->height = nh;
	return 1;
}
struct jpeg_state {
	struct jpeg_decompress_struct info;
	struct jpeg_error_mgr error;
	jmp_buf jump;
	unsigned char *row;
	int created;
	int orientation;
};
static void jpeg_error(j_common_ptr c) {
	struct jpeg_state *s = (struct jpeg_state *)c->client_data;
	longjmp(s->jump, 1);
}
static void jpeg_quiet(j_common_ptr c) { (void)c; }
static int jpeg_preview(FILE *file, int mw, int mh, struct fsv_thumbnail *out,
			struct control *control) {
	struct jpeg_state *s = calloc(1, sizeof(*s));
	int ok = 0;
	if (!s) return 0;
	s->info.err = jpeg_std_error(&s->error);
	s->error.error_exit = jpeg_error;
	s->error.output_message = jpeg_quiet;
	s->info.client_data = s;
	if (setjmp(s->jump)) goto done;
	jpeg_create_decompress(&s->info);
	s->created = 1;
	s->info.mem->max_memory_to_use = 64u * 1024u * 1024u;
	jpeg_stdio_src(&s->info, file);
	jpeg_save_markers(&s->info, JPEG_APP0 + 1, 65535);
	jpeg_read_header(&s->info, TRUE);
	s->orientation = jpeg_orientation(s->info.marker_list);
	if (s->orientation >= 5) {
		int swap = mw;
		mw = mh;
		mh = swap;
	}
	if (!output(out, s->info.image_width, s->info.image_height, mw, mh))
		goto done;
	s->info.scale_num = 1;
	s->info.scale_denom = 8;
	while (
	    s->info.scale_denom > 1 &&
	    (s->info.image_width / s->info.scale_denom < (unsigned)out->width ||
	     s->info.image_height / s->info.scale_denom <
		 (unsigned)out->height))
		s->info.scale_denom /= 2;
	s->info.out_color_space = (s->info.jpeg_color_space == JCS_CMYK ||
				   s->info.jpeg_color_space == JCS_YCCK)
				      ? JCS_CMYK
				      : JCS_RGB;
	jpeg_start_decompress(&s->info);
	out->decoder_width = (int)s->info.output_width;
	if ((uint64_t)s->info.output_width * s->info.output_components >
	    ROW_LIMIT)
		goto done;
	s->row =
	    malloc((size_t)s->info.output_width * s->info.output_components);
	if (!s->row) goto done;
	while (s->info.output_scanline < s->info.output_height) {
		unsigned y = s->info.output_scanline;
		JSAMPROW row = s->row;
		if (stopped(control)) goto done;
		if (jpeg_read_scanlines(&s->info, &row, 1) != 1) goto done;
		for (int dy = 0; dy < out->height; ++dy)
			if (sample(dy, s->info.output_height, out->height) ==
			    y) {
				for (int dx = 0; dx < out->width; ++dx) {
					unsigned sx =
					    sample(dx, s->info.output_width,
						   out->width);
					unsigned char *
					    p = s->row +
						(size_t)sx *
						    s->info.output_components,
					   *q = out->rgba +
						((size_t)dy * out->width + dx) *
						    4;
					if (s->info.output_components == 4) {
						for (int c = 0; c < 3; ++c)
							q[c] =
							    s->info.saw_Adobe_marker
								? (unsigned char)((unsigned)
										      p[c] *
										  p[3] /
										  255)
								: (unsigned char)((255u -
										   p[c]) *
										  (255u -
										   p[3]) /
										  255);
					} else {
						q[0] = p[0];
						q[1] = p[1];
						q[2] = p[2];
					}
					q[3] = 255;
				}
			}
	}
	jpeg_finish_decompress(&s->info);
	ok = orient(out, s->orientation);
done:
	if (s->created) jpeg_destroy_decompress(&s->info);
	free(s->row);
	free(s);
	return ok;
}
static void png_cache_text(png_structp png, png_infop info,
			   struct fsv_thumbnail *out) {
	png_textp text = NULL;
	int count = png_get_text(png, info, &text, NULL);
	for (int n = 0; n < count; ++n)
		if (text[n].key && text[n].text) {
			char *target = NULL;
			size_t capacity = 0;
			if (!strcmp(text[n].key, "Thumb::URI")) {
				target = out->uri;
				capacity = sizeof(out->uri);
			}
			if (!strcmp(text[n].key, "Thumb::MTime")) {
				target = out->mtime;
				capacity = sizeof(out->mtime);
			}
			if (target) {
				size_t length = strlen(text[n].text);
				if (length < capacity)
					memcpy(target, text[n].text,
					       length + 1);
				else
					target[0] = 0;
			}
		}
}
struct png_state {
	png_structp png;
	png_infop info;
	unsigned char *row;
};
static void png_error_quiet(png_structp png, const char *text) {
	(void)text;
	png_longjmp(png, 1);
}
static void png_warning_quiet(png_structp png, const char *text) {
	(void)png;
	(void)text;
}
static int png_preview(FILE *file, int mw, int mh, struct fsv_thumbnail *out,
		       struct control *control) {
	struct png_state *s = calloc(1, sizeof(*s));
	int ok = 0;
	if (!s) return 0;
	s->png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL,
					png_error_quiet, png_warning_quiet);
	if (!s->png) goto done;
	s->info = png_create_info_struct(s->png);
	if (!s->info) goto done;
	if (setjmp(png_jmpbuf(s->png))) goto done;
	png_set_user_limits(s->png, 1048576, 1048576);
	png_set_chunk_malloc_max(s->png, 1024 * 1024);
	png_set_chunk_cache_max(s->png, 32);
	png_init_io(s->png, file);
	png_read_info(s->png, s->info);
	unsigned w = png_get_image_width(s->png, s->info),
		 h = png_get_image_height(s->png, s->info);
	int color = png_get_color_type(s->png, s->info),
	    depth = png_get_bit_depth(s->png, s->info),
	    interlace = png_get_interlace_type(s->png, s->info);
	if ((uint64_t)w * 4 > ROW_LIMIT || !output(out, w, h, mw, mh))
		goto done;
	if (depth == 16) png_set_strip_16(s->png);
	if (color == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(s->png);
	if (color == PNG_COLOR_TYPE_GRAY && depth < 8)
		png_set_expand_gray_1_2_4_to_8(s->png);
	if (png_get_valid(s->png, s->info, PNG_INFO_tRNS))
		png_set_tRNS_to_alpha(s->png);
	else if (!(color & PNG_COLOR_MASK_ALPHA))
		png_set_add_alpha(s->png, 255, PNG_FILLER_AFTER);
	if (color == PNG_COLOR_TYPE_GRAY || color == PNG_COLOR_TYPE_GRAY_ALPHA)
		png_set_gray_to_rgb(s->png);
	int passes = png_set_interlace_handling(s->png);
	png_read_update_info(s->png, s->info);
	size_t rowbytes = png_get_rowbytes(s->png, s->info);
	if (rowbytes > ROW_LIMIT || rowbytes < (size_t)w * 4) goto done;
	s->row = calloc(rowbytes, 1);
	if (!s->row) goto done;
	out->decoder_width = (int)w;
	static const unsigned xstart[] = {0, 4, 0, 2, 0, 1, 0},
			      ystart[] = {0, 0, 4, 0, 2, 0, 1},
			      xstep[] = {8, 8, 4, 4, 2, 2, 1},
			      ystep[] = {8, 8, 8, 4, 4, 2, 2};
	for (int pass = 0; pass < passes; ++pass)
		for (unsigned y = 0; y < h; ++y) {
			if (stopped(control)) goto done;
			memset(s->row, 0, rowbytes);
			png_read_row(s->png, s->row, NULL);
			if (interlace && (y < ystart[pass] ||
					  (y - ystart[pass]) % ystep[pass]))
				continue;
			for (int dy = 0; dy < out->height; ++dy)
				if (sample(dy, h, out->height) == y)
					for (int dx = 0; dx < out->width;
					     ++dx) {
						unsigned sx =
						    sample(dx, w, out->width);
						if (!interlace ||
						    (sx >= xstart[pass] &&
						     (sx - xstart[pass]) %
							     xstep[pass] ==
							 0))
							memcpy(
							    out->rgba +
								((size_t)dy *
								     out->width +
								 dx) *
								    4,
							    s->row +
								(size_t)sx * 4,
							    4);
					}
		}
	png_read_end(s->png, s->info);
	png_cache_text(s->png, s->info, out);
	ok = 1;
done:
	if (s->png)
		png_destroy_read_struct(&s->png, s->info ? &s->info : NULL,
					NULL);
	free(s->row);
	free(s);
	return ok;
}
static uint32_t le32(const unsigned char *p) {
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
	       (uint32_t)p[3] << 24;
}
static int bmp_preview(FILE *file, int mw, int mh, struct fsv_thumbnail *out,
		       struct control *control, uint64_t fileSize) {
	unsigned char head[54], *row = NULL;
	int ok = 0;
	if (fread(head, 1, sizeof(head), file) != sizeof(head) ||
	    le32(head + 14) < 40 || le32(head + 30) != 0)
		return 0;
	int32_t sw = (int32_t)le32(head + 18), sh = (int32_t)le32(head + 22);
	unsigned bits = (unsigned)head[28] | (unsigned)head[29] << 8;
	if (sw <= 0 || sh == 0 || sh == INT32_MIN || (bits != 24 && bits != 32))
		return 0;
	unsigned w = (unsigned)sw, h = (unsigned)(sh < 0 ? -sh : sh);
	uint64_t stride = (((uint64_t)w * bits + 31) / 32) * 4,
		 offset = le32(head + 10);
	if (stride > ROW_LIMIT || offset > fileSize ||
	    stride * h > fileSize - offset || !output(out, w, h, mw, mh))
		return 0;
	row = malloc((size_t)stride);
	if (!row) return 0;
	out->decoder_width = out->width;
	for (int y = 0; y < out->height; ++y) {
		if (stopped(control)) goto done;
		unsigned sy = sample(y, h, out->height);
		if (sh > 0) sy = h - 1 - sy;
		if (fseeko(file, (off_t)(offset + stride * sy), SEEK_SET) ||
		    fread(row, 1, (size_t)stride, file) != (size_t)stride)
			goto done;
		for (int x = 0; x < out->width; ++x) {
			unsigned sx = sample(x, w, out->width);
			const unsigned char *p = row + (size_t)sx * (bits / 8);
			unsigned char *q =
			    out->rgba + ((size_t)y * out->width + x) * 4;
			q[0] = p[2];
			q[1] = p[1];
			q[2] = p[0];
			q[3] = 255;
		}
	}
	ok = 1;
done:
	free(row);
	return ok;
}
static int gif_input(GifFileType *gif, GifByteType *bytes, int size) {
	return (int)fread(bytes, 1, (size_t)size, (FILE *)gif->UserData);
}
static int gif_preview(FILE *file, int mw, int mh, struct fsv_thumbnail *out,
		       struct control *control) {
	int error = 0, ok = 0, transparent = -1;
	GifFileType *gif = DGifOpen(file, gif_input, &error);
	unsigned char *row = NULL;
	if (!gif) return 0;
	if (!output(out, gif->SWidth, gif->SHeight, mw, mh)) goto done;
	for (;;) {
		GifRecordType type;
		if (stopped(control) ||
		    DGifGetRecordType(gif, &type) == GIF_ERROR)
			goto done;
		if (type == TERMINATE_RECORD_TYPE) goto done;
		if (type == EXTENSION_RECORD_TYPE) {
			int code;
			GifByteType *bytes;
			if (DGifGetExtension(gif, &code, &bytes) == GIF_ERROR)
				goto done;
			if (code == GRAPHICS_EXT_FUNC_CODE && bytes &&
			    bytes[0] >= 4 && (bytes[1] & 1))
				transparent = bytes[4];
			while (bytes) {
				if (DGifGetExtensionNext(gif, &bytes) ==
				    GIF_ERROR)
					goto done;
			}
		} else if (type == IMAGE_DESC_RECORD_TYPE) {
			if (DGifGetImageDesc(gif) == GIF_ERROR) goto done;
			GifImageDesc *frame = &gif->Image;
			ColorMapObject *map =
			    frame->ColorMap ? frame->ColorMap : gif->SColorMap;
			if (!map || frame->Width <= 0 || frame->Height <= 0 ||
			    frame->Left < 0 || frame->Top < 0 ||
			    frame->Left + frame->Width > gif->SWidth ||
			    frame->Top + frame->Height > gif->SHeight)
				goto done;
			row = malloc((size_t)frame->Width);
			if (!row) goto done;
			out->decoder_width = frame->Width;
			static const int start[] = {0, 4, 2, 1},
					 step[] = {8, 8, 4, 2};
			for (int pass = 0; pass < (frame->Interlace ? 4 : 1);
			     ++pass)
				for (int fy = frame->Interlace ? start[pass]
							       : 0;
				     fy < frame->Height;
				     fy += frame->Interlace ? step[pass] : 1) {
					if (stopped(control) ||
					    DGifGetLine(gif, row,
							frame->Width) ==
						GIF_ERROR)
						goto done;
					for (int dy = 0; dy < out->height; ++dy)
						if (sample(dy, gif->SHeight,
							   out->height) ==
						    (unsigned)(frame->Top + fy))
							for (int dx = 0;
							     dx < out->width;
							     ++dx) {
								unsigned sx = sample(
								    dx,
								    gif->SWidth,
								    out->width);
								if (sx <
									(unsigned)frame
									    ->Left ||
								    sx >=
									(unsigned)(frame
										       ->Left +
										   frame
										       ->Width))
									continue;
								unsigned index = row
								    [sx -
								     frame
									 ->Left];
								if (index ==
								    (unsigned)
									transparent)
									continue;
								if (index >=
								    (unsigned)map
									->ColorCount)
									goto done;
								GifColorType color =
								    map->Colors
									[index];
								unsigned char *q =
								    out->rgba +
								    ((size_t)dy *
									 out->width +
								     dx) *
									4;
								q[0] =
								    color.Red;
								q[1] =
								    color.Green;
								q[2] =
								    color.Blue;
								q[3] = 255;
							}
				}
			ok = 1;
			break;
		}
	}
done:
	free(row);
	DGifCloseFile(gif, &error);
	return ok;
}
int fsv_thumbnail_decode(const char *path, int mw, int mh,
			 struct fsv_thumbnail *out, fsv_thumbnail_cancel cancel,
			 void *user) {
	FILE *file = NULL;
	struct stat st;
	unsigned char magic[8];
	int ok = 0;
	struct control control = {cancel, user, {0, 0}};
	memset(out, 0, sizeof(*out));
	if (mw < 1 || mh < 1 || mw > 1024 || mh > 1024) return 0;
	clock_gettime(CLOCK_MONOTONIC, &control.start);
	int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW);
	if (fd < 0) return 0;
	file = fdopen(fd, "rb");
	if (!file) {
		close(fd);
		return 0;
	}
	if (fstat(fileno(file), &st) || !S_ISREG(st.st_mode) ||
	    fread(magic, 1, 8, file) != 8)
		goto done;
	rewind(file);
	int kind = 0;
	if (magic[0] == 'B' && magic[1] == 'M')
		kind = 4;
	else if (!png_sig_cmp(magic, 0, 8))
		kind = 2;
	else if (magic[0] == 0xff && magic[1] == 0xd8)
		kind = 1;
	else if (!memcmp(magic, "GIF87a", 6) || !memcmp(magic, "GIF89a", 6))
		kind = 3;
	out->kind = kind;
	if (magic[0] == 'B' && magic[1] == 'M')
		ok = bmp_preview(file, mw, mh, out, &control,
				 (uint64_t)st.st_size);
	else if (st.st_size <= INPUT_LIMIT) {
		if (!png_sig_cmp(magic, 0, 8))
			ok = png_preview(file, mw, mh, out, &control);
		else if (magic[0] == 0xff && magic[1] == 0xd8)
			ok = jpeg_preview(file, mw, mh, out, &control);
		else if (!memcmp(magic, "GIF87a", 6) ||
			 !memcmp(magic, "GIF89a", 6))
			ok = gif_preview(file, mw, mh, out, &control);
	}
done:
	if (file) fclose(file);
	if (!ok) {
		int kind = out->kind;
		fsv_thumbnail_free(out);
		out->kind = kind;
	}
	return ok;
}
void fsv_thumbnail_free(struct fsv_thumbnail *out) {
	free(out->rgba);
	memset(out, 0, sizeof(*out));
}
