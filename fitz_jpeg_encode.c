//go:build cgo && !nocgo

// Copyright (C) 2004-2026 Artifex Software, Inc.
//
// This file is part of MuPDF.
//
// MuPDF is free software: you can redistribute it and/or modify it under the
// terms of the GNU Affero General Public License as published by the Free
// Software Foundation, either version 3 of the License, or (at your option)
// any later version.
//
// MuPDF is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
// FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License for more
// details.
//
// You should have received a copy of the GNU Affero General Public License
// along with MuPDF. If not, see <https://www.gnu.org/licenses/agpl-3.0.en.html>
//
// Alternative licensing terms are available from the licensor.
// For commercial licensing, see <https://www.artifex.com/> or contact
// Artifex Software, Inc., 39 Mesa Street, Suite 108A, San Francisco,
// CA 94129, USA, for further information.

#include "mupdf/fitz.h"

#include "include/jpeg/jpeglib.h"

#ifdef SHARE_JPEG

#define JZ_CTX_FROM_CINFO(c) (fz_context *)((c)->client_data)

static void fz_jpg_mem_init(j_common_ptr cinfo, fz_context *ctx)
{
	cinfo->client_data = ctx;
}

#define fz_jpg_mem_term(cinfo)

#else /* SHARE_JPEG */

typedef void * backing_store_ptr;

#include "include/jpeg/jmemcust.h"

#define JZ_CTX_FROM_CINFO(c) (fz_context *)(GET_CUST_MEM_DATA(c)->priv)

static void *
fz_jpg_mem_alloc(j_common_ptr cinfo, size_t size)
{
	fz_context *ctx = JZ_CTX_FROM_CINFO(cinfo);
	return fz_malloc_no_throw(ctx, size);
}

static void
fz_jpg_mem_free(j_common_ptr cinfo, void *object, size_t size)
{
	fz_context *ctx = JZ_CTX_FROM_CINFO(cinfo);
	fz_free(ctx, object);
}

static void
fz_jpg_mem_init(j_common_ptr cinfo, fz_context *ctx)
{
	jpeg_cust_mem_data *custmptr;
	custmptr = fz_malloc_struct(ctx, jpeg_cust_mem_data);
	if (!jpeg_cust_mem_init(custmptr, (void *) ctx, NULL, NULL, NULL,
				fz_jpg_mem_alloc, fz_jpg_mem_free,
				fz_jpg_mem_alloc, fz_jpg_mem_free, NULL))
	{
		fz_free(ctx, custmptr);
		fz_throw(ctx, FZ_ERROR_LIBRARY, "cannot initialize custom JPEG memory handler");
	}
	cinfo->client_data = custmptr;
}

static void
fz_jpg_mem_term(j_common_ptr cinfo)
{
	if (cinfo->client_data)
	{
		fz_context *ctx = JZ_CTX_FROM_CINFO(cinfo);
		fz_free(ctx, cinfo->client_data);
		cinfo->client_data = NULL;
	}
}

#endif

#define OUTPUT_BUF_SIZE (16<<10)

typedef struct {
	struct jpeg_destination_mgr pub;
	fz_output *out;
	JOCTET buffer[OUTPUT_BUF_SIZE];
} my_destination_mgr;

typedef my_destination_mgr * my_dest_ptr;

static void error_exit(j_common_ptr cinfo)
{
	char msg[JMSG_LENGTH_MAX];
	fz_context *ctx = JZ_CTX_FROM_CINFO(cinfo);
	cinfo->err->format_message(cinfo, msg);
	fz_throw(ctx, FZ_ERROR_LIBRARY, "jpeg error: %s", msg);
}

static void init_destination(j_compress_ptr cinfo)
{
	my_dest_ptr dest = (my_dest_ptr) cinfo->dest;
	dest->pub.next_output_byte = dest->buffer;
	dest->pub.free_in_buffer = OUTPUT_BUF_SIZE;
}

static boolean empty_output_buffer(j_compress_ptr cinfo)
{
	fz_context *ctx = JZ_CTX_FROM_CINFO(cinfo);
	my_dest_ptr dest = (my_dest_ptr) cinfo->dest;
	fz_output *out = dest->out;

	fz_write_data(ctx, out, dest->buffer, OUTPUT_BUF_SIZE);

	dest->pub.next_output_byte = dest->buffer;
	dest->pub.free_in_buffer = OUTPUT_BUF_SIZE;

	return TRUE;
}

static void term_destination(j_compress_ptr cinfo)
{
	fz_context *ctx = JZ_CTX_FROM_CINFO(cinfo);
	my_dest_ptr dest = (my_dest_ptr) cinfo->dest;
	fz_output *out = dest->out;
	size_t datacount = OUTPUT_BUF_SIZE - dest->pub.free_in_buffer;

	fz_write_data(ctx, out, dest->buffer, datacount);
}

void
invoice_write_pixmap_as_jpeg(fz_context *ctx, fz_output *out, fz_pixmap *pix, int quality, int invert_cmyk)
{
	struct jpeg_compress_struct cinfo;
	struct jpeg_error_mgr err;
	my_destination_mgr dest;
	JSAMPROW row_pointer[1];
	fz_colorspace *cs = pix->colorspace;
	int n = pix->n;
	int alpha = pix->alpha;
	unsigned char *rgb_row = NULL;
	fz_var(rgb_row);

	if (pix->s > 0)
		fz_throw(ctx, FZ_ERROR_ARGUMENT, "pixmap may not have separations to save as JPEG");
	if (cs && !fz_colorspace_is_gray(ctx, cs) && !fz_colorspace_is_rgb(ctx, cs) && !fz_colorspace_is_cmyk(ctx, cs))
		fz_throw(ctx, FZ_ERROR_ARGUMENT, "pixmap must be Grayscale, RGB, or CMYK to save as JPEG");

	/* Treat alpha only as greyscale */
	if (n == 1 && alpha)
		alpha = 0;
	n -= alpha;

	if (alpha > 0 && (n != 3 || !fz_colorspace_is_rgb(ctx, cs)))
		fz_throw(ctx, FZ_ERROR_ARGUMENT, "only opaque-backed RGBA pixmaps can drop alpha for JPEG");

	cinfo.mem = NULL;
	cinfo.global_state = 0;
	cinfo.err = jpeg_std_error(&err);
	err.error_exit = error_exit;

	cinfo.client_data = NULL;
	fz_jpg_mem_init((j_common_ptr)&cinfo, ctx);

	fz_try(ctx)
	{
		jpeg_create_compress(&cinfo);

		cinfo.dest = (void*) &dest;
		dest.pub.init_destination = init_destination;
		dest.pub.empty_output_buffer = empty_output_buffer;
		dest.pub.term_destination = term_destination;
		dest.out = out;

		cinfo.image_width = pix->w;
		cinfo.image_height = pix->h;
		cinfo.input_components = n;
		switch (n) {
		case 1:
			cinfo.in_color_space = JCS_GRAYSCALE;
			break;
		case 3:
			cinfo.in_color_space = JCS_RGB;
			break;
		case 4:
			cinfo.in_color_space = JCS_CMYK;
			break;
		}

		jpeg_set_defaults(&cinfo);
		jpeg_set_quality(&cinfo, quality, TRUE);

		/* Write image resolution */
		cinfo.density_unit = 1; /* dots/inch */
		cinfo.X_density = pix->xres;
		cinfo.Y_density = pix->yres;

		/* Keep baseline sequential JPEG and default 4:2:0 sampling, as in Go image/jpeg.
		 * Progressive encoding and 4:4:4 sampling cost much more CPU for previews. */

		jpeg_start_compress(&cinfo, TRUE);

		if (fz_colorspace_is_cmyk(ctx, pix->colorspace) && invert_cmyk)
			fz_invert_pixmap_raw(ctx, pix);

		if (alpha)
			rgb_row = fz_malloc(ctx, (size_t)pix->w * 3);
		while (cinfo.next_scanline < cinfo.image_height) {
			const unsigned char *source = &pix->samples[(size_t)cinfo.next_scanline * pix->stride];
			if (alpha) {
				// Rendering over an opaque white backing leaves alpha at 255.
				// Strip it one scanline at a time, without allocating an RGB page.
				for (int x = 0; x < pix->w; ++x) {
					rgb_row[3*x] = source[4*x];
					rgb_row[3*x+1] = source[4*x+1];
					rgb_row[3*x+2] = source[4*x+2];
				}
				row_pointer[0] = rgb_row;
			} else {
				row_pointer[0] = (JSAMPROW)source;
			}
			(void) jpeg_write_scanlines(&cinfo, row_pointer, 1);
		}

		if (fz_colorspace_is_cmyk(ctx, pix->colorspace) && invert_cmyk)
			fz_invert_pixmap_raw(ctx, pix);

		jpeg_finish_compress(&cinfo);
	}
	fz_always(ctx)
	{
		fz_free(ctx, rgb_row);
		jpeg_destroy_compress(&cinfo);
		fz_jpg_mem_term((j_common_ptr)&cinfo);
	}
	fz_catch(ctx)
	{
		fz_rethrow(ctx);
	}
}


/* invoicefile modification of MuPDF 1.28.0 output-jpeg.c:
 * retain the upstream allocator/error handling; use baseline 4:2:0 encoding.
 */
fz_buffer *invoice_new_buffer_as_jpeg(fz_context *ctx, fz_pixmap *pix, int quality)
{
 fz_buffer *buffer = NULL;
 fz_output *output = NULL;
 fz_var(buffer);
 fz_var(output);
 fz_try(ctx) {
  buffer = fz_new_buffer(ctx, 64 << 10);
  output = fz_new_output_with_buffer(ctx, buffer);
  invoice_write_pixmap_as_jpeg(ctx, output, pix, quality, 0);
  fz_close_output(ctx, output);
 }
 fz_always(ctx) { fz_drop_output(ctx, output); }
 fz_catch(ctx) {
  fz_drop_buffer(ctx, buffer);
  fz_rethrow(ctx);
 }
 return buffer;
}
