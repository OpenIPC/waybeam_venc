/* quirc -- QR-code recognition library
 * Copyright (C) 2010-2012 Daniel Beer <dlbeer@gmail.com>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <stdlib.h>
#include <string.h>
#include "quirc_internal.h"

const char *quirc_version(void)
{
	return "1.0";
}

struct quirc *quirc_new(void)
{
	struct quirc *q = malloc(sizeof(*q));

	if (!q)
		return NULL;

	memset(q, 0, sizeof(*q));
	return q;
}

void quirc_destroy(struct quirc *q)
{
	free(q->image);
	/* q->pixels may alias q->image when their type representation is of the
	   same size, so we need to be careful here to avoid a double free */
	if (!QUIRC_PIXEL_ALIAS_IMAGE)
		free(q->pixels);
	free(q->flood_fill_vars);
	free(q);
}

void quirc_set_marker_mode(struct quirc *q, enum quirc_marker_mode mode,
			   enum quirc_marker_profile profile)
{
	if (!q)
		return;

	if (mode < QUIRC_MARKER_OFF || mode > QUIRC_MARKER_ONLY)
		mode = QUIRC_MARKER_OFF;
	if (profile < QUIRC_MARKER_PROFILE_NONE ||
	    profile > QUIRC_MARKER_PROFILE_OUTER_FRAME_V1)
		profile = QUIRC_MARKER_PROFILE_NONE;
	if (mode != QUIRC_MARKER_OFF &&
	    profile == QUIRC_MARKER_PROFILE_NONE)
		mode = QUIRC_MARKER_OFF;

	q->marker_mode = mode;
	q->marker_profile = profile;
}

int quirc_resize(struct quirc *q, int w, int h)
{
	uint8_t		*image  = NULL;
	quirc_pixel_t	*pixels = NULL;
	size_t olddim;
	size_t newdim;
	size_t min;
	size_t num_vars;
	size_t vars_byte_size;
	struct quirc_flood_fill_vars *vars = NULL;

	/*
	 * XXX: w and h should be size_t (or at least unsigned) as negatives
	 * values would not make much sense. The downside is that it would break
	 * both the API and ABI. Thus, at the moment, let's just do a sanity
	 * check.
	 */
	if (w < 0 || h < 0)
		goto fail;

	/* Resizing to the current non-empty dimensions preserves every byte.
	 * Avoid allocating, copying and freeing an identical image/work area;
	 * quirc_begin() resets the per-scan state and callers overwrite the
	 * image before quirc_end(). */
	if (w > 0 && h > 0 && w == q->w && h == q->h)
		return 0;

	/* Compute the old/new image dimensions and required flood-fill depth
	 * before allocating so an alias-image build can reuse larger buffers. */
	olddim = (size_t)q->w * q->h;
	newdim = (size_t)w * h;
	if (w && newdim / (size_t)w != (size_t)h)
		goto fail;
	min = olddim < newdim ? olddim : newdim;
	if ((size_t)h * 2 / 2 != (size_t)h)
		goto fail;
	num_vars = (size_t)h * 2;
	if (num_vars == 0)
		num_vars = 1;

	/* The marker build aliases pixels to image. Retaining the high-water
	 * allocations across full/half/ROI passes preserves resize contents:
	 * shrinking keeps the common prefix, while growth zeroes exactly the
	 * suffix that a fresh calloc would have supplied. */
	if (QUIRC_PIXEL_ALIAS_IMAGE && q->image &&
	    newdim <= q->image_capacity &&
	    num_vars <= q->flood_fill_vars_capacity) {
		if (newdim > olddim)
			memset(q->image + olddim, 0, newdim - olddim);
		q->w = w;
		q->h = h;
		q->num_flood_fill_vars = num_vars;
		return 0;
	}

	/*
	 * alloc a new buffer for q->image. We avoid realloc(3) because we want
	 * on failure to be leave `q` in a consistant, unmodified state.
	 */
	image = calloc(w, h);
	if (!image)
		goto fail;

	/*
	 * copy the data into the new buffer, avoiding (a) to read beyond the
	 * old buffer when the new size is greater and (b) to write beyond the
	 * new buffer when the new size is smaller, hence the min computation.
	 */
	if (min)
		(void)memcpy(image, q->image, min);

	/* alloc a new buffer for q->pixels if needed */
	if (!QUIRC_PIXEL_ALIAS_IMAGE) {
		pixels = calloc(newdim, sizeof(quirc_pixel_t));
		if (!pixels)
			goto fail;
	}

	/*
	 * alloc the work area for the flood filling logic.
	 *
	 * the size was chosen with the following assumptions and observations:
	 *
	 * - rings are the regions which requires the biggest work area.
	 * - they consumes the most when they are rotated by about 45 degree.
	 *   in that case, the necessary depth is about (2 * height_of_the_ring).
	 * - normal finder rings are at most about 1/3 of the image height.
	 * - an optional continuous outer-frame marker may span the full image,
	 *   so reserve the conservative 2 * image-height depth.
	 */

	vars_byte_size = sizeof(*vars) * num_vars;
	if (vars_byte_size / sizeof(*vars) != num_vars) {
		goto fail; /* size_t overflow */
	}
	vars = malloc(vars_byte_size);
	if (!vars)
		goto fail;

	/* alloc succeeded, update `q` with the new size and buffers */
	q->w = w;
	q->h = h;
	free(q->image);
	q->image = image;
	q->image_capacity = newdim;
	if (!QUIRC_PIXEL_ALIAS_IMAGE) {
		free(q->pixels);
		q->pixels = pixels;
	}
	free(q->flood_fill_vars);
	q->flood_fill_vars = vars;
	q->num_flood_fill_vars = num_vars;
	q->flood_fill_vars_capacity = num_vars;

	return 0;
	/* NOTREACHED */
fail:
	free(image);
	free(pixels);
	free(vars);

	return -1;
}

int quirc_count(const struct quirc *q)
{
	return q->num_grids;
}

static const char *const error_table[] = {
	[QUIRC_SUCCESS] = "Success",
	[QUIRC_ERROR_INVALID_GRID_SIZE] = "Invalid grid size",
	[QUIRC_ERROR_INVALID_VERSION] = "Invalid version",
	[QUIRC_ERROR_FORMAT_ECC] = "Format data ECC failure",
	[QUIRC_ERROR_DATA_ECC] = "ECC failure",
	[QUIRC_ERROR_UNKNOWN_DATA_TYPE] = "Unknown data type",
	[QUIRC_ERROR_DATA_OVERFLOW] = "Data overflow",
	[QUIRC_ERROR_DATA_UNDERFLOW] = "Data underflow"
};

const char *quirc_strerror(quirc_decode_error_t err)
{
	if (err >= 0 && err < sizeof(error_table) / sizeof(error_table[0]))
		return error_table[err];

	return "Unknown error";
}
