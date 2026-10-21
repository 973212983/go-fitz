#ifndef INVOICE_FITZ_JPEG_ENCODE_H
#define INVOICE_FITZ_JPEG_ENCODE_H
#include <mupdf/fitz.h>

/* libjpeg baseline encoder; kept as the reference for invoice_encode_jpeg(). */
fz_buffer *invoice_new_buffer_as_jpeg(fz_context *ctx, fz_pixmap *pix, int quality);

/* Byte-identical to invoice_new_buffer_as_jpeg() for RGB and opaque RGBA pixmaps. */
fz_buffer *invoice_encode_jpeg(fz_context *ctx, const fz_pixmap *pix, int quality);

/* libjpeg-turbo encoder, on platforms that ship libs/libturbojpeg_*.a
 * (invoice_has_turbojpeg() returns 1); throws elsewhere. */
fz_buffer *invoice_encode_jpeg_turbo(fz_context *ctx, const fz_pixmap *pix, int quality);
int invoice_has_turbojpeg(void);

/* Encoder choice for invoice_encode(); the default is turbo when built, else fast. */
enum {
	INVOICE_ENCODER_DEFAULT = 0,
	INVOICE_ENCODER_FAST = 1,
	INVOICE_ENCODER_LIBJPEG = 2,
	INVOICE_ENCODER_TURBO = 3,
};

enum {
	INVOICE_RENDER_KEEP_GROUP = 1,     /* keep MuPDF's page transparency group */
	INVOICE_RENDER_NO_IMAGE_CACHE = 2, /* decode every image again */
	INVOICE_RENDER_PLAIN = 4,          /* draw device only, as fz_run_page_contents */
};

/* Draw a page onto an opaque white RGB(A) pixmap at dpi; flags are for tests.
 * The pixel buffer comes from a shared pool: release the result only with
 * invoice_drop_page_pixmap(). */
fz_pixmap *invoice_render_page(fz_context *ctx, fz_page *page, double dpi, int alpha, int flags);
void invoice_drop_page_pixmap(fz_context *ctx, fz_pixmap *pix);
void invoice_page_pool_clear(void);
int invoice_page_pool_count(void);

/* Remove /ToUnicode from embedded Type0 fonts; rendering is unchanged. */
void invoice_drop_to_unicode(fz_context *ctx, fz_document *doc);

void invoice_image_cache_stats(long *hits, long *misses, int *entries, size_t *bytes);
void invoice_image_cache_clear(void);
#endif
