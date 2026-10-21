//go:build cgo && !nocgo

package fitz

/*
#cgo windows,amd64 CFLAGS: -DFITZ_TURBOJPEG
#cgo windows,amd64 LDFLAGS: -L${SRCDIR}/libs -lturbojpeg_windows_amd64
#cgo linux,!android,amd64 CFLAGS: -DFITZ_TURBOJPEG
#cgo linux,!android,amd64,!musl LDFLAGS: -L${SRCDIR}/libs -lturbojpeg_linux_amd64
#cgo linux,!android,amd64,musl LDFLAGS: -L${SRCDIR}/libs -lturbojpeg_linux_amd64_musl

#include <mupdf/fitz.h>
#include "fitz_jpeg_encode.h"
#include <stdio.h>

static fz_buffer *invoice_encode(fz_context *ctx, fz_pixmap *pixmap, int quality, int encoder)
{
    if (encoder == INVOICE_ENCODER_DEFAULT)
        encoder = invoice_has_turbojpeg() ? INVOICE_ENCODER_TURBO : INVOICE_ENCODER_FAST;
    switch (encoder) {
    case INVOICE_ENCODER_LIBJPEG:
        return invoice_new_buffer_as_jpeg(ctx, pixmap, quality);
    case INVOICE_ENCODER_TURBO:
        return invoice_encode_jpeg_turbo(ctx, pixmap, quality);
    default:
        return invoice_encode_jpeg(ctx, pixmap, quality);
    }
}

// All MuPDF calls that may throw stay inside fz_try; longjmp must never cross Go frames.
// flags and encoder select the reference paths compared by tests.
static fz_buffer *invoice_render_jpeg(fz_context *ctx, fz_document *doc, int number,
    double dpi, int quality, int flags, int encoder, char *message, size_t message_size)
{
    fz_page *page = NULL;
    fz_pixmap *pixmap = NULL;
    fz_buffer *buffer = NULL;
    fz_var(page);
    fz_var(pixmap);
    fz_var(buffer);

    fz_try(ctx) {
        page = fz_load_page(ctx, doc, number);
        // Without alpha the draw device produces the same pixels, faster.
        pixmap = invoice_render_page(ctx, page, dpi, 0, flags);
        buffer = invoice_encode(ctx, pixmap, quality, encoder);
    }
    fz_always(ctx) {
        invoice_drop_page_pixmap(ctx, pixmap);
        fz_drop_page(ctx, page);
    }
    fz_catch(ctx) {
        snprintf(message, message_size, "%s", fz_caught_message(ctx));
        fz_drop_buffer(ctx, buffer);
        return NULL;
    }
    return buffer;
}

// Returns the packed RGB samples of a rendered page.
static fz_buffer *invoice_render_rgb(fz_context *ctx, fz_document *doc, int number, double dpi,
    int flags, int *w, int *h, char *message, size_t message_size)
{
    fz_page *page = NULL;
    fz_pixmap *pixmap = NULL;
    fz_buffer *buffer = NULL;
    fz_var(page);
    fz_var(pixmap);
    fz_var(buffer);

    fz_try(ctx) {
        page = fz_load_page(ctx, doc, number);
        pixmap = invoice_render_page(ctx, page, dpi, 0, flags);
        buffer = fz_new_buffer(ctx, (size_t)pixmap->w * pixmap->h * 3);
        for (int y = 0; y < pixmap->h; y++)
            fz_append_data(ctx, buffer, pixmap->samples + (size_t)y * pixmap->stride, (size_t)pixmap->w * 3);
        *w = pixmap->w;
        *h = pixmap->h;
    }
    fz_always(ctx) {
        invoice_drop_page_pixmap(ctx, pixmap);
        fz_drop_page(ctx, page);
    }
    fz_catch(ctx) {
        snprintf(message, message_size, "%s", fz_caught_message(ctx));
        fz_drop_buffer(ctx, buffer);
        return NULL;
    }
    return buffer;
}

static int invoice_drop_to_unicode_safe(fz_context *ctx, fz_document *doc, char *message, size_t message_size)
{
    fz_try(ctx)
        invoice_drop_to_unicode(ctx, doc);
    fz_catch(ctx) {
        snprintf(message, message_size, "%s", fz_caught_message(ctx));
        return 0;
    }
    return 1;
}

static fz_context *invoice_new_context(void)
{
    return fz_new_context(NULL, NULL, FZ_STORE_DEFAULT);
}

// Encodes caller-owned RGB or RGBA samples with one of the encoders.
static fz_buffer *invoice_encode_samples(fz_context *ctx, unsigned char *samples, int w, int h,
    int n, int quality, int encoder, char *message, size_t message_size)
{
    fz_pixmap *pixmap = NULL;
    fz_buffer *buffer = NULL;
    fz_var(pixmap);
    fz_var(buffer);

    fz_try(ctx) {
        pixmap = fz_new_pixmap_with_data(ctx, fz_device_rgb(ctx), w, h, NULL, n == 4, w * n, samples);
        fz_set_pixmap_resolution(ctx, pixmap, 150, 150);
        buffer = invoice_encode(ctx, pixmap, quality, encoder);
    }
    fz_always(ctx)
        fz_drop_pixmap(ctx, pixmap);
    fz_catch(ctx) {
        snprintf(message, message_size, "%s", fz_caught_message(ctx));
        fz_drop_buffer(ctx, buffer);
        return NULL;
    }
    return buffer;
}
*/
import "C"

import (
	"errors"
	"fmt"
	"math"
	"unsafe"
)

// Render variants for tests comparing the fast paths with the reference ones.
const (
	renderKeepGroup    = C.INVOICE_RENDER_KEEP_GROUP
	renderNoImageCache = C.INVOICE_RENDER_NO_IMAGE_CACHE
	renderPlain        = C.INVOICE_RENDER_PLAIN
)

// JPEG encoders.  ImageJPEG uses encoderDefault: libjpeg-turbo where libs/
// ships it (hasTurboJPEG), otherwise the fast encoder.
const (
	encoderDefault = C.INVOICE_ENCODER_DEFAULT
	encoderFast    = C.INVOICE_ENCODER_FAST
	encoderLibjpeg = C.INVOICE_ENCODER_LIBJPEG
	encoderTurbo   = C.INVOICE_ENCODER_TURBO
)

var hasTurboJPEG = C.invoice_has_turbojpeg() != 0

// ImageJPEG renders a zero-based page at dpi and encodes baseline JPEG in C.
// Only the compressed JPEG is copied into Go; no Go pixel buffer is allocated.
func (f *Document) ImageJPEG(pageNumber int, dpi float64, quality int) ([]byte, error) {
	return f.imageJPEG(pageNumber, dpi, quality, 0, encoderDefault)
}

func (f *Document) imageJPEG(pageNumber int, dpi float64, quality int, flags int, encoder int) ([]byte, error) {
	f.mtx.Lock()
	defer f.mtx.Unlock()
	if pageNumber < 0 || pageNumber >= f.NumPage() {
		return nil, ErrPageMissing
	}
	if err := checkJPEGParams(dpi, quality); err != nil {
		return nil, err
	}
	var message [256]C.char
	buffer := C.invoice_render_jpeg(f.ctx, f.doc, C.int(pageNumber), C.double(dpi), C.int(quality),
		C.int(flags), C.int(encoder), &message[0], C.size_t(len(message)))
	if buffer == nil {
		return nil, fmt.Errorf("fitz: JPEG rendering failed: %s", C.GoString(&message[0]))
	}
	return takeBuffer(f.ctx, buffer)
}

// DropToUnicode removes the ToUnicode maps of embedded CID (Type0) fonts from
// this document in memory, before any page is rendered.  Loading such a font
// otherwise remaps every code of its encoding through the map, which is a large
// part of rendering a short document.  Rendered pixels stay the same, but text
// extraction from this Document afterwards loses the Unicode mapping, so only
// call it on documents opened for rendering.
func (f *Document) DropToUnicode() error {
	f.mtx.Lock()
	defer f.mtx.Unlock()
	var message [256]C.char
	if C.invoice_drop_to_unicode_safe(f.ctx, f.doc, &message[0], C.size_t(len(message))) == 0 {
		return fmt.Errorf("fitz: dropping ToUnicode failed: %s", C.GoString(&message[0]))
	}
	return nil
}

func checkJPEGParams(dpi float64, quality int) error {
	if dpi <= 0 || math.IsNaN(dpi) || math.IsInf(dpi, 0) || dpi > math.MaxInt32 {
		return errors.New("fitz: JPEG DPI must be finite and positive")
	}
	if quality < 1 || quality > 100 {
		return errors.New("fitz: JPEG quality must be between 1 and 100")
	}
	return nil
}

// takeBuffer copies a MuPDF buffer into Go memory and drops it.
func takeBuffer(ctx *C.struct_fz_context, buffer *C.fz_buffer) ([]byte, error) {
	defer C.fz_drop_buffer(ctx, buffer)
	var data *C.uchar
	size := C.fz_buffer_storage(ctx, buffer, &data)
	if size == 0 || uint64(size) > math.MaxInt32 {
		return nil, errors.New("fitz: invalid buffer size")
	}
	return C.GoBytes(unsafe.Pointer(data), C.int(size)), nil
}

// renderRGB returns the RGB samples of a page rendered with flags; for tests.
func (f *Document) renderRGB(pageNumber int, dpi float64, flags int) ([]byte, int, int, error) {
	f.mtx.Lock()
	defer f.mtx.Unlock()
	if pageNumber < 0 || pageNumber >= f.NumPage() {
		return nil, 0, 0, ErrPageMissing
	}
	var message [256]C.char
	var w, h C.int
	buffer := C.invoice_render_rgb(f.ctx, f.doc, C.int(pageNumber), C.double(dpi), C.int(flags), &w, &h, &message[0], C.size_t(len(message)))
	if buffer == nil {
		return nil, 0, 0, fmt.Errorf("fitz: rendering failed: %s", C.GoString(&message[0]))
	}
	out, err := takeBuffer(f.ctx, buffer)
	return out, int(w), int(h), err
}

// encodeSamples encodes width x height RGB (n = 3) or RGBA (n = 4) samples with
// one of the encoders; for tests.
func encodeSamples(samples []byte, width, height, n, quality, encoder int) ([]byte, error) {
	if width <= 0 || height <= 0 || (n != 3 && n != 4) || len(samples) != width*height*n {
		return nil, errors.New("fitz: invalid samples")
	}
	ctx := C.invoice_new_context()
	if ctx == nil {
		return nil, ErrCreateContext
	}
	defer C.fz_drop_context(ctx)
	var message [256]C.char
	buffer := C.invoice_encode_samples(ctx, (*C.uchar)(unsafe.Pointer(&samples[0])), C.int(width), C.int(height),
		C.int(n), C.int(quality), C.int(encoder), &message[0], C.size_t(len(message)))
	if buffer == nil {
		return nil, fmt.Errorf("fitz: JPEG encoding failed: %s", C.GoString(&message[0]))
	}
	return takeBuffer(ctx, buffer)
}

// imageCacheStats reports the shared decoded image cache; for tests.
func imageCacheStats() (hits, misses int64, entries, bytes int) {
	var h, m C.long
	var e C.int
	var b C.size_t
	C.invoice_image_cache_stats(&h, &m, &e, &b)
	return int64(h), int64(m), int(e), int(b)
}

// pagePoolCount reports the idle page buffers; for tests.
func pagePoolCount() int {
	return int(C.invoice_page_pool_count())
}

// clearPagePool frees the idle page buffers; for tests.
func clearPagePool() {
	C.invoice_page_pool_clear()
}

// clearImageCache empties the shared decoded image cache; for tests.
func clearImageCache() {
	C.invoice_image_cache_clear()
}
