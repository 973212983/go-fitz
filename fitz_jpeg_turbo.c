//go:build cgo && !nocgo

#include "fitz_jpeg_encode.h"

#ifdef FITZ_TURBOJPEG
#include "turbojpeg/fitz_turbojpeg.h"

/* libjpeg-turbo (SIMD) baseline encoder: YCbCr 4:2:0, accurate integer DCT,
 * standard Huffman tables and a JFIF marker with the pixmap resolution, as the
 * other encoders write.  Chroma is downsampled by averaging, so the bytes differ
 * from libjpeg 9; the decoded pixels stay within a few levels. */
fz_buffer *invoice_encode_jpeg_turbo(fz_context *ctx, const fz_pixmap *pix, int quality)
{
	char message[200] = "";
	tjhandle tj = NULL;
	unsigned char *out = NULL;
	size_t size = 0;
	fz_buffer *buf = NULL;

	if (pix->n - pix->alpha != 3 || (pix->colorspace && !fz_colorspace_is_rgb(ctx, pix->colorspace)))
		fz_throw(ctx, FZ_ERROR_ARGUMENT, "JPEG encoder needs an RGB pixmap");
	if (pix->s > 0)
		fz_throw(ctx, FZ_ERROR_ARGUMENT, "JPEG encoder needs a pixmap without separations");
	if (pix->w <= 0 || pix->h <= 0 || pix->w > 65500 || pix->h > 65500)
		fz_throw(ctx, FZ_ERROR_ARGUMENT, "invalid JPEG image size %dx%d", pix->w, pix->h);
	if (quality < 1 || quality > 100)
		fz_throw(ctx, FZ_ERROR_ARGUMENT, "invalid JPEG quality %d", quality);

	tj = tj3Init(TJINIT_COMPRESS);
	if (!tj)
		fz_throw(ctx, FZ_ERROR_SYSTEM, "libjpeg-turbo: %s", tj3GetErrorStr(NULL));
	/* An opaque alpha channel is skipped as padding, like the other encoders do. */
	if (fitz_tj_compress(tj, pix->samples, pix->w, (int)pix->stride, pix->h,
			pix->n == 4 ? TJPF_RGBX : TJPF_RGB, quality, pix->xres, pix->yres, &out, &size))
		fz_strlcpy(message, tj3GetErrorStr(tj), sizeof message);
	tj3Destroy(tj);
	if (message[0]) {
		tj3Free(out);
		fz_throw(ctx, FZ_ERROR_LIBRARY, "libjpeg-turbo: %s", message);
	}

	fz_try(ctx)
		buf = fz_new_buffer_from_copied_data(ctx, out, size);
	fz_always(ctx)
		tj3Free(out);
	fz_catch(ctx)
		fz_rethrow(ctx);
	return buf;
}

int invoice_has_turbojpeg(void)
{
	return 1;
}
#else
fz_buffer *invoice_encode_jpeg_turbo(fz_context *ctx, const fz_pixmap *pix, int quality)
{
	fz_throw(ctx, FZ_ERROR_UNSUPPORTED, "libjpeg-turbo is not built for this platform");
}

int invoice_has_turbojpeg(void)
{
	return 0;
}
#endif
