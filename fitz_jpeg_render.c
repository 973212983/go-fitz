//go:build cgo && !nocgo

/*
 * Page rendering helpers for invoice JPEG previews.
 *
 * An invoice_filter device sits between the PDF interpreter and the draw device:
 *
 * 1. Decoded image tiles are shared between documents.  Invoices repeat the
 *    same seal, title and icon images, and decoding them (Flate + PNG
 *    predictors) dominated rasterization.  Each eligible image is wrapped in a
 *    proxy whose get_pixmap returns a copy of a cached tile, decoded earlier by
 *    the original image for exactly the same request.  fz_get_pixmap_from_image
 *    and the draw device then proceed as before, so pixels are identical.  The
 *    cache keeps plain malloc'd copies; no MuPDF object crosses contexts.
 *
 * 2. MuPDF wraps a page in an isolated transparency group whenever an image has
 *    a soft mask.  Over the opaque white page, an isolated normal group at full
 *    alpha composites like drawing directly, apart from 8-bit rounding, so the
 *    group (a full page buffer plus a compositing pass) is skipped.  Any other
 *    group, soft mask or tiling makes the page "unsafe" and the caller renders
 *    it again with the group kept.
 */

#include "mupdf/fitz.h"
#include "fitz_jpeg_encode.h"

#include <limits.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* The bundled headers stop at fitz; these are the MuPDF 1.28 declarations. */
typedef struct pdf_document pdf_document;
typedef struct pdf_obj pdf_obj;
pdf_document *pdf_document_from_fz_document(fz_context *ctx, fz_document *ptr);
int pdf_count_pages(fz_context *ctx, pdf_document *doc);
pdf_obj *pdf_lookup_page_obj(fz_context *ctx, pdf_document *doc, int needle);
pdf_obj *pdf_dict_getp_inheritable(fz_context *ctx, pdf_obj *dict, const char *path);
pdf_obj *pdf_dict_gets(fz_context *ctx, pdf_obj *dict, const char *key);
int pdf_dict_len(fz_context *ctx, pdf_obj *dict);
pdf_obj *pdf_dict_get_val(fz_context *ctx, pdf_obj *dict, int idx);
const char *pdf_to_name(fz_context *ctx, pdf_obj *obj);
pdf_obj *pdf_array_get(fz_context *ctx, pdf_obj *array, int i);
void pdf_dict_dels(fz_context *ctx, pdf_obj *dict, const char *key);

#define TILE_CACHE_MAX_ENTRIES 64
#define TILE_CACHE_MAX_BYTES ((size_t)16 << 20)
#define TILE_SEEN_SLOTS 64

enum { CS_ALPHA = 0, CS_GRAY = 1, CS_RGB = 3 };

/* Everything besides the compressed bytes that determines a decoded tile.
 * Hashed and compared as bytes, so it has no implicit padding and is always
 * copied with memcpy. */
typedef struct {
	size_t clen;
	int w, h, n, bpc, cs, interpolate;
	int columns, colors, predictor, flate_bpc;
	int x0, y0, x1, y1, req_w, req_h, req_l2;
	int reserved;
} tile_key;
_Static_assert(sizeof(tile_key) == sizeof(size_t) + 18 * sizeof(int), "tile_key must not have padding");

typedef struct tile_entry {
	struct tile_entry *prev, *next;
	atomic_int refs;
	uint64_t hash;
	tile_key key;
	unsigned char *cdata;
	/* the decoded tile and what get_pixmap reported */
	int x, y, w, h, n, alpha, flags, xres, yres;
	ptrdiff_t stride;
	fz_irect rect;
	int l2factor;
	size_t size;
	unsigned char *samples;
} tile_entry;

/* Guarded by tile_lock; the critical sections only touch pointers and counters. */
static struct {
	tile_entry *head, *tail;
	int count;
	size_t bytes;
	uint64_t seen[TILE_SEEN_SLOTS];
	int seen_pos;
	long hits, misses;
} tile_cache;
static atomic_flag tile_lock = ATOMIC_FLAG_INIT;

static void spin_lock(atomic_flag *lock)
{
	while (atomic_flag_test_and_set_explicit(lock, memory_order_acquire)) {
#if defined(__x86_64__) || defined(__i386__)
		__builtin_ia32_pause();
#elif defined(__aarch64__)
		__asm__ __volatile__("yield");
#endif
	}
}

static void spin_unlock(atomic_flag *lock)
{
	atomic_flag_clear_explicit(lock, memory_order_release);
}

static void cache_lock(void)
{
	spin_lock(&tile_lock);
}

static void cache_unlock(void)
{
	spin_unlock(&tile_lock);
}

static uint64_t hash_bytes(uint64_t h, const unsigned char *p, size_t n)
{
	const uint64_t m = 0x9E3779B97F4A7C15ull;
	while (n >= 8) {
		uint64_t v;
		memcpy(&v, p, 8);
		h = (h ^ v) * m;
		h ^= h >> 29;
		p += 8;
		n -= 8;
	}
	while (n--)
		h = (h ^ *p++) * m;
	h ^= h >> 32;
	return h;
}

static void entry_release(tile_entry *e)
{
	if (atomic_fetch_sub_explicit(&e->refs, 1, memory_order_acq_rel) == 1) {
		free(e->cdata);
		free(e->samples);
		free(e);
	}
}

static void lru_unlink(tile_entry *e)
{
	if (e->prev)
		e->prev->next = e->next;
	else
		tile_cache.head = e->next;
	if (e->next)
		e->next->prev = e->prev;
	else
		tile_cache.tail = e->prev;
	e->prev = e->next = NULL;
}

static void lru_push_front(tile_entry *e)
{
	e->prev = NULL;
	e->next = tile_cache.head;
	if (tile_cache.head)
		tile_cache.head->prev = e;
	tile_cache.head = e;
	if (!tile_cache.tail)
		tile_cache.tail = e;
}

/* Returns a referenced entry or NULL. */
static tile_entry *cache_find(uint64_t hash, const tile_key *key, const unsigned char *cdata)
{
	tile_entry *e;
	cache_lock();
	for (e = tile_cache.head; e; e = e->next)
		if (e->hash == hash && !memcmp(&e->key, key, sizeof *key) && !memcmp(e->cdata, cdata, key->clen))
			break;
	if (e) {
		lru_unlink(e);
		lru_push_front(e);
		atomic_fetch_add_explicit(&e->refs, 1, memory_order_relaxed);
		tile_cache.hits++;
	} else {
		tile_cache.misses++;
	}
	cache_unlock();
	return e;
}

void invoice_image_cache_stats(long *hits, long *misses, int *entries, size_t *bytes)
{
	cache_lock();
	*hits = tile_cache.hits;
	*misses = tile_cache.misses;
	*entries = tile_cache.count;
	*bytes = tile_cache.bytes;
	cache_unlock();
}

void invoice_image_cache_clear(void)
{
	tile_entry *e;
	cache_lock();
	e = tile_cache.head;
	tile_cache.head = tile_cache.tail = NULL;
	tile_cache.count = 0;
	tile_cache.bytes = 0;
	tile_cache.hits = tile_cache.misses = 0;
	memset(tile_cache.seen, 0, sizeof tile_cache.seen);
	cache_unlock();
	while (e) {
		tile_entry *next = e->next;
		entry_release(e);
		e = next;
	}
}

/* Cache a copy of a freshly decoded tile once the same request was seen before.
 * Best effort: allocation failures just skip caching. */
static void cache_offer(uint64_t hash, const tile_key *key, const unsigned char *cdata, const fz_pixmap *tile, const fz_irect *rect, int l2factor)
{
	size_t size = (size_t)tile->stride * tile->h;
	int admit = 0;
	if (tile->seps || tile->s || size + key->clen > TILE_CACHE_MAX_BYTES / 4)
		return;
	cache_lock();
	for (int i = 0; i < TILE_SEEN_SLOTS; i++)
		if (tile_cache.seen[i] == hash) {
			admit = 1;
			tile_cache.seen[i] = 0;
			break;
		}
	if (!admit) {
		tile_cache.seen[tile_cache.seen_pos] = hash;
		tile_cache.seen_pos = (tile_cache.seen_pos + 1) % TILE_SEEN_SLOTS;
	}
	cache_unlock();
	if (!admit)
		return;

	tile_entry *e = calloc(1, sizeof *e);
	if (!e)
		return;
	e->cdata = malloc(key->clen ? key->clen : 1);
	e->samples = malloc(size ? size : 1);
	if (!e->cdata || !e->samples) {
		free(e->cdata);
		free(e->samples);
		free(e);
		return;
	}
	memcpy(e->cdata, cdata, key->clen);
	memcpy(e->samples, tile->samples, size);
	atomic_init(&e->refs, 1);
	e->hash = hash;
	memcpy(&e->key, key, sizeof *key);
	e->x = tile->x;
	e->y = tile->y;
	e->w = tile->w;
	e->h = tile->h;
	e->n = tile->n;
	e->alpha = tile->alpha;
	e->flags = tile->flags & FZ_PIXMAP_FLAG_INTERPOLATE;
	e->xres = tile->xres;
	e->yres = tile->yres;
	e->stride = tile->stride;
	e->rect = rect ? *rect : fz_empty_irect;
	e->l2factor = l2factor;
	e->size = size;

	tile_entry *evicted = NULL;
	cache_lock();
	tile_entry *old;
	for (old = tile_cache.head; old; old = old->next)
		if (old->hash == hash && !memcmp(&old->key, key, sizeof *key) && !memcmp(old->cdata, cdata, key->clen))
			break;
	if (old) {
		/* Another thread cached it meanwhile. */
		cache_unlock();
		entry_release(e);
		return;
	}
	lru_push_front(e);
	tile_cache.count++;
	tile_cache.bytes += size + key->clen;
	while (tile_cache.count > TILE_CACHE_MAX_ENTRIES || tile_cache.bytes > TILE_CACHE_MAX_BYTES) {
		tile_entry *t = tile_cache.tail;
		lru_unlink(t);
		tile_cache.count--;
		tile_cache.bytes -= t->size + t->key.clen;
		t->next = evicted;
		evicted = t;
	}
	cache_unlock();
	while (evicted) {
		tile_entry *t = evicted;
		evicted = t->next;
		entry_release(t);
	}
}

typedef struct {
	fz_image super;
	fz_image *orig;
	tile_key key; /* image part; the request part is filled per call */
	uint64_t hash;
	const unsigned char *cdata;
} cached_image;

static fz_colorspace *cached_colorspace(fz_context *ctx, int cs)
{
	return cs == CS_RGB ? fz_device_rgb(ctx) : cs == CS_GRAY ? fz_device_gray(ctx) : NULL;
}

static fz_pixmap *cached_image_get_pixmap(fz_context *ctx, fz_image *image, fz_irect *subarea, int w, int h, int *l2factor)
{
	cached_image *ci = (cached_image *)image;
	tile_key key;
	memcpy(&key, &ci->key, sizeof key);
	if (subarea) {
		key.x0 = subarea->x0;
		key.y0 = subarea->y0;
		key.x1 = subarea->x1;
		key.y1 = subarea->y1;
	} else {
		key.x0 = key.y0 = key.x1 = key.y1 = -1;
	}
	key.req_w = w;
	key.req_h = h;
	key.req_l2 = l2factor ? *l2factor : -1;
	uint64_t hash = hash_bytes(ci->hash, (const unsigned char *)&key, sizeof key);

	tile_entry *e = cache_find(hash, &key, ci->cdata);
	if (e) {
		fz_pixmap *pix = NULL;
		fz_var(pix);
		fz_try(ctx) {
			pix = fz_new_pixmap(ctx, cached_colorspace(ctx, key.cs), e->w, e->h, NULL, e->alpha);
			if (pix->n != e->n || pix->stride != e->stride)
				fz_throw(ctx, FZ_ERROR_ARGUMENT, "cached image tile layout mismatch");
			pix->x = e->x;
			pix->y = e->y;
			pix->xres = e->xres;
			pix->yres = e->yres;
			pix->flags = (pix->flags & ~FZ_PIXMAP_FLAG_INTERPOLATE) | e->flags;
			memcpy(pix->samples, e->samples, e->size);
			if (subarea)
				*subarea = e->rect;
			if (l2factor)
				*l2factor = e->l2factor;
		}
		fz_always(ctx)
			entry_release(e);
		fz_catch(ctx) {
			fz_drop_pixmap(ctx, pix);
			fz_rethrow(ctx);
		}
		return pix;
	}

	fz_pixmap *tile = ci->orig->get_pixmap(ctx, ci->orig, subarea, w, h, l2factor);
	/* Only tiles that the hit path above rebuilds exactly. */
	if (tile->colorspace == cached_colorspace(ctx, key.cs) && tile->stride == (ptrdiff_t)tile->w * tile->n)
		cache_offer(hash, &key, ci->cdata, tile, subarea, l2factor ? *l2factor : 0);
	return tile;
}

static size_t cached_image_get_size(fz_context *ctx, fz_image *image)
{
	return fz_image_size(ctx, ((cached_image *)image)->orig);
}

static void cached_image_get_digest(fz_context *ctx, fz_image *image, unsigned char digest[16])
{
	fz_image_digest(ctx, ((cached_image *)image)->orig, digest);
}

static void cached_image_drop(fz_context *ctx, fz_image *image)
{
	fz_drop_image(ctx, ((cached_image *)image)->orig);
}

/* A proxy for images whose decoding is deterministic and cheap to describe:
 * 8-bit Flate data in DeviceGray, DeviceRGB or as an alpha mask, without
 * decode arrays, color keys or mattes.  Returns NULL for anything else. */
static fz_image *cached_image_proxy(fz_context *ctx, fz_image *image)
{
	fz_compressed_buffer *cb = fz_compressed_image_buffer(ctx, image);
	int cs;
	if (!cb || !cb->buffer || cb->params.type != FZ_IMAGE_FLATE)
		return NULL;
	if (image->bpc != 8 || image->imagemask || image->use_colorkey || image->use_decode || image->decoded || image->scalable)
		return NULL;
	if (image->colorspace == NULL && image->n == 1)
		cs = CS_ALPHA;
	else if (image->colorspace == fz_device_gray(ctx))
		cs = CS_GRAY;
	else if (image->colorspace == fz_device_rgb(ctx))
		cs = CS_RGB;
	else
		return NULL;

	cached_image *ci = fz_new_derived_image(ctx, image->w, image->h, image->bpc, image->colorspace,
		image->xres, image->yres, image->interpolate, 0, NULL, NULL, image->mask, cached_image,
		cached_image_get_pixmap, cached_image_get_size,
		image->get_digest ? cached_image_get_digest : NULL, cached_image_drop);
	ci->super.intent = image->intent;
	ci->super.has_intent = image->has_intent;
	ci->super.orientation = image->orientation;
	ci->orig = fz_keep_image(ctx, image);
	memset(&ci->key, 0, sizeof ci->key);
	ci->key.w = image->w;
	ci->key.h = image->h;
	ci->key.n = image->n;
	ci->key.bpc = image->bpc;
	ci->key.cs = cs;
	ci->key.interpolate = image->interpolate;
	ci->key.columns = cb->params.u.flate.columns;
	ci->key.colors = cb->params.u.flate.colors;
	ci->key.predictor = cb->params.u.flate.predictor;
	ci->key.flate_bpc = cb->params.u.flate.bpc;
	ci->key.clen = cb->buffer->len;
	ci->cdata = cb->buffer->data;
	ci->hash = hash_bytes(0x243F6A8885A308D3ull, cb->buffer->data, cb->buffer->len);
	return &ci->super;
}

typedef struct {
	fz_device super;
	fz_device *target;
	int calls;
	int depth;
	int dropped; /* skipped the page group */
	int unsafe;  /* skipping the group may have changed the result */
	int keep_group;
	int cache_images;
} invoice_filter;

#define FILTER(dev) ((invoice_filter *)(dev))

static void note_call(invoice_filter *f)
{
	f->calls++;
}

static void flt_close(fz_context *ctx, fz_device *dev)
{
	fz_close_device(ctx, FILTER(dev)->target);
}

static void flt_fill_path(fz_context *ctx, fz_device *dev, const fz_path *p, int eo, fz_matrix m, fz_colorspace *cs, const float *c, float a, fz_color_params cp)
{
	note_call(FILTER(dev));
	fz_fill_path(ctx, FILTER(dev)->target, p, eo, m, cs, c, a, cp);
}

static void flt_stroke_path(fz_context *ctx, fz_device *dev, const fz_path *p, const fz_stroke_state *st, fz_matrix m, fz_colorspace *cs, const float *c, float a, fz_color_params cp)
{
	note_call(FILTER(dev));
	fz_stroke_path(ctx, FILTER(dev)->target, p, st, m, cs, c, a, cp);
}

static void flt_clip_path(fz_context *ctx, fz_device *dev, const fz_path *p, int eo, fz_matrix m, fz_rect sc)
{
	note_call(FILTER(dev));
	fz_clip_path(ctx, FILTER(dev)->target, p, eo, m, sc);
}

static void flt_clip_stroke_path(fz_context *ctx, fz_device *dev, const fz_path *p, const fz_stroke_state *st, fz_matrix m, fz_rect sc)
{
	note_call(FILTER(dev));
	fz_clip_stroke_path(ctx, FILTER(dev)->target, p, st, m, sc);
}

static void flt_fill_text(fz_context *ctx, fz_device *dev, const fz_text *t, fz_matrix m, fz_colorspace *cs, const float *c, float a, fz_color_params cp)
{
	note_call(FILTER(dev));
	fz_fill_text(ctx, FILTER(dev)->target, t, m, cs, c, a, cp);
}

static void flt_stroke_text(fz_context *ctx, fz_device *dev, const fz_text *t, const fz_stroke_state *st, fz_matrix m, fz_colorspace *cs, const float *c, float a, fz_color_params cp)
{
	note_call(FILTER(dev));
	fz_stroke_text(ctx, FILTER(dev)->target, t, st, m, cs, c, a, cp);
}

static void flt_clip_text(fz_context *ctx, fz_device *dev, const fz_text *t, fz_matrix m, fz_rect sc)
{
	note_call(FILTER(dev));
	fz_clip_text(ctx, FILTER(dev)->target, t, m, sc);
}

static void flt_clip_stroke_text(fz_context *ctx, fz_device *dev, const fz_text *t, const fz_stroke_state *st, fz_matrix m, fz_rect sc)
{
	note_call(FILTER(dev));
	fz_clip_stroke_text(ctx, FILTER(dev)->target, t, st, m, sc);
}

static void flt_ignore_text(fz_context *ctx, fz_device *dev, const fz_text *t, fz_matrix m)
{
	note_call(FILTER(dev));
	fz_ignore_text(ctx, FILTER(dev)->target, t, m);
}

static void flt_fill_shade(fz_context *ctx, fz_device *dev, fz_shade *sh, fz_matrix m, float a, fz_color_params cp)
{
	note_call(FILTER(dev));
	fz_fill_shade(ctx, FILTER(dev)->target, sh, m, a, cp);
}

/* Run fn with a caching proxy for image when eligible. */
#define WITH_PROXY(f, image, call) \
	do { \
		fz_image *proxy_ = (f)->cache_images ? cached_image_proxy(ctx, image) : NULL; \
		fz_image *img_ = proxy_ ? proxy_ : (image); \
		fz_try(ctx) { call; } \
		fz_always(ctx) fz_drop_image(ctx, proxy_); \
		fz_catch(ctx) fz_rethrow(ctx); \
	} while (0)

static void flt_fill_image(fz_context *ctx, fz_device *dev, fz_image *image, fz_matrix m, float a, fz_color_params cp)
{
	invoice_filter *f = FILTER(dev);
	note_call(f);
	WITH_PROXY(f, image, fz_fill_image(ctx, f->target, img_, m, a, cp));
}

static void flt_fill_image_mask(fz_context *ctx, fz_device *dev, fz_image *image, fz_matrix m, fz_colorspace *cs, const float *c, float a, fz_color_params cp)
{
	invoice_filter *f = FILTER(dev);
	note_call(f);
	WITH_PROXY(f, image, fz_fill_image_mask(ctx, f->target, img_, m, cs, c, a, cp));
}

static void flt_clip_image_mask(fz_context *ctx, fz_device *dev, fz_image *image, fz_matrix m, fz_rect sc)
{
	invoice_filter *f = FILTER(dev);
	note_call(f);
	WITH_PROXY(f, image, fz_clip_image_mask(ctx, f->target, img_, m, sc));
}

static void flt_pop_clip(fz_context *ctx, fz_device *dev)
{
	note_call(FILTER(dev));
	fz_pop_clip(ctx, FILTER(dev)->target);
}

static void flt_begin_mask(fz_context *ctx, fz_device *dev, fz_rect r, int lum, fz_colorspace *cs, const float *bc, fz_color_params cp)
{
	invoice_filter *f = FILTER(dev);
	note_call(f);
	if (f->dropped)
		f->unsafe = 1;
	fz_begin_mask(ctx, f->target, r, lum, cs, bc, cp);
}

static void flt_end_mask(fz_context *ctx, fz_device *dev, fz_function *fn)
{
	note_call(FILTER(dev));
	fz_end_mask_tr(ctx, FILTER(dev)->target, fn);
}

static void flt_begin_group(fz_context *ctx, fz_device *dev, fz_rect r, fz_colorspace *cs, int isolated, int knockout, int blendmode, float alpha)
{
	invoice_filter *f = FILTER(dev);
	int first = f->calls == 0;
	note_call(f);
	if (first && !f->keep_group && isolated && !knockout && blendmode == FZ_BLEND_NORMAL && alpha == 1 && cs == NULL) {
		f->dropped = 1;
		f->depth++;
		return;
	}
	if (f->dropped)
		f->unsafe = 1;
	f->depth++;
	fz_begin_group(ctx, f->target, r, cs, isolated, knockout, blendmode, alpha);
}

static void flt_end_group(fz_context *ctx, fz_device *dev)
{
	invoice_filter *f = FILTER(dev);
	note_call(f);
	if (--f->depth == 0 && f->dropped)
		return;
	fz_end_group(ctx, f->target);
}

static int flt_begin_tile(fz_context *ctx, fz_device *dev, fz_rect a, fz_rect v, float xs, float ys, fz_matrix m, int id, int doc_id)
{
	invoice_filter *f = FILTER(dev);
	note_call(f);
	if (f->dropped)
		f->unsafe = 1;
	return fz_begin_tile_tid(ctx, f->target, a, v, xs, ys, m, id, doc_id);
}

static void flt_end_tile(fz_context *ctx, fz_device *dev)
{
	note_call(FILTER(dev));
	fz_end_tile(ctx, FILTER(dev)->target);
}

static void flt_render_flags(fz_context *ctx, fz_device *dev, int set, int clear)
{
	fz_render_flags(ctx, FILTER(dev)->target, set, clear);
}

static void flt_set_default_colorspaces(fz_context *ctx, fz_device *dev, fz_default_colorspaces *dcs)
{
	fz_set_default_colorspaces(ctx, FILTER(dev)->target, dcs);
}

static void flt_begin_layer(fz_context *ctx, fz_device *dev, const char *name)
{
	fz_begin_layer(ctx, FILTER(dev)->target, name);
}

static void flt_end_layer(fz_context *ctx, fz_device *dev)
{
	fz_end_layer(ctx, FILTER(dev)->target);
}

static fz_device *new_invoice_filter(fz_context *ctx, fz_device *target, int keep_group, int cache_images)
{
	invoice_filter *f = fz_new_derived_device(ctx, invoice_filter);
	f->target = target;
	f->keep_group = keep_group;
	f->cache_images = cache_images;
	f->super.close_device = flt_close;
	f->super.fill_path = flt_fill_path;
	f->super.stroke_path = flt_stroke_path;
	f->super.clip_path = flt_clip_path;
	f->super.clip_stroke_path = flt_clip_stroke_path;
	f->super.fill_text = flt_fill_text;
	f->super.stroke_text = flt_stroke_text;
	f->super.clip_text = flt_clip_text;
	f->super.clip_stroke_text = flt_clip_stroke_text;
	f->super.ignore_text = flt_ignore_text;
	f->super.fill_shade = flt_fill_shade;
	f->super.fill_image = flt_fill_image;
	f->super.fill_image_mask = flt_fill_image_mask;
	f->super.clip_image_mask = flt_clip_image_mask;
	f->super.pop_clip = flt_pop_clip;
	f->super.begin_mask = flt_begin_mask;
	f->super.end_mask = flt_end_mask;
	f->super.begin_group = flt_begin_group;
	f->super.end_group = flt_end_group;
	f->super.begin_tile = flt_begin_tile;
	f->super.end_tile = flt_end_tile;
	f->super.render_flags = flt_render_flags;
	f->super.set_default_colorspaces = flt_set_default_colorspaces;
	f->super.begin_layer = flt_begin_layer;
	f->super.end_layer = flt_end_layer;
	return &f->super;
}

/*
 * Page pixel buffers are reused between pages and documents.  A page at
 * 150 dpi needs several megabytes; allocating that afresh maps new memory and
 * faults in every page of it, which cost about as much as the clearing itself.
 * At most PAGE_POOL_MAX idle buffers are kept, so the retained memory is bounded
 * by that many pages.  Plain malloc'd memory, independent of any fz_context.
 */
#define PAGE_POOL_MAX 4
#define PAGE_POOL_MAX_BYTES ((size_t)64 << 20)

typedef struct {
	size_t cap;
	size_t pad; /* keeps the samples 16-byte aligned */
} page_buffer;

static struct {
	page_buffer *idle[PAGE_POOL_MAX];
	int count;
} page_pool;
static atomic_flag page_pool_lock = ATOMIC_FLAG_INIT;

static unsigned char *page_buffer_take(fz_context *ctx, size_t size)
{
	page_buffer *b = NULL;
	spin_lock(&page_pool_lock);
	for (int i = 0; i < page_pool.count; i++)
		if (page_pool.idle[i]->cap >= size) {
			b = page_pool.idle[i];
			page_pool.idle[i] = page_pool.idle[--page_pool.count];
			break;
		}
	spin_unlock(&page_pool_lock);
	if (!b) {
		if (size > SIZE_MAX - sizeof *b)
			fz_throw(ctx, FZ_ERROR_LIMIT, "page too large");
		b = malloc(sizeof *b + size);
		if (!b)
			fz_throw(ctx, FZ_ERROR_SYSTEM, "cannot allocate page buffer");
		b->cap = size;
	}
	return (unsigned char *)(b + 1);
}

static void page_buffer_give(unsigned char *samples)
{
	page_buffer *b = (page_buffer *)samples - 1;
	spin_lock(&page_pool_lock);
	if (b->cap <= PAGE_POOL_MAX_BYTES) {
		if (page_pool.count < PAGE_POOL_MAX) {
			page_pool.idle[page_pool.count++] = b;
			b = NULL;
		} else {
			/* Keep the largest buffers so that any page size can be served. */
			int smallest = 0;
			for (int i = 1; i < PAGE_POOL_MAX; i++)
				if (page_pool.idle[i]->cap < page_pool.idle[smallest]->cap)
					smallest = i;
			if (page_pool.idle[smallest]->cap < b->cap) {
				page_buffer *t = page_pool.idle[smallest];
				page_pool.idle[smallest] = b;
				b = t;
			}
		}
	}
	spin_unlock(&page_pool_lock);
	free(b);
}

void invoice_drop_page_pixmap(fz_context *ctx, fz_pixmap *pix)
{
	unsigned char *samples;
	if (!pix)
		return;
	/* Only invoice_render_page() creates these; nothing else holds a reference. */
	samples = pix->samples;
	fz_drop_pixmap(ctx, pix);
	page_buffer_give(samples);
}

void invoice_page_pool_clear(void)
{
	page_buffer *idle[PAGE_POOL_MAX];
	int count;
	spin_lock(&page_pool_lock);
	count = page_pool.count;
	memcpy(idle, page_pool.idle, sizeof idle);
	page_pool.count = 0;
	spin_unlock(&page_pool_lock);
	for (int i = 0; i < count; i++)
		free(idle[i]);
}

int invoice_page_pool_count(void)
{
	int count;
	spin_lock(&page_pool_lock);
	count = page_pool.count;
	spin_unlock(&page_pool_lock);
	return count;
}

/* Draw a page onto an opaque white RGB(A) pixmap at dpi.  The result must be
 * released with invoice_drop_page_pixmap(). */
fz_pixmap *invoice_render_page(fz_context *ctx, fz_page *page, double dpi, int alpha, int flags)
{
	fz_pixmap *pix = NULL;
	unsigned char *samples = NULL;
	fz_device *draw = NULL, *filter = NULL;
	fz_var(pix);
	fz_var(samples);
	fz_var(draw);
	fz_var(filter);
	fz_try(ctx) {
		fz_matrix scale = fz_scale((float)(dpi / 72.0), (float)(dpi / 72.0));
		fz_irect bbox = fz_round_rect(fz_transform_rect(fz_bound_page(ctx, page), scale));
		/* The same overflow checks as fz_new_pixmap_with_bbox(). */
		int w = fz_irect_width(bbox), h = fz_irect_height(bbox), n = 3 + alpha;
		if (w <= 0 || h <= 0 || w > INT_MAX / n)
			fz_throw(ctx, FZ_ERROR_LIMIT, "invalid page size %dx%d", w, h);
		samples = page_buffer_take(ctx, (size_t)w * n * h);
		pix = fz_new_pixmap_with_bbox_and_data(ctx, fz_device_rgb(ctx), bbox, NULL, alpha, samples);
		fz_set_pixmap_resolution(ctx, pix, (int)dpi, (int)dpi);
		for (int attempt = 0; attempt < 2; attempt++) {
			int keep_group = attempt > 0 || (flags & INVOICE_RENDER_KEEP_GROUP);
			fz_clear_pixmap_with_value(ctx, pix, 255);
			draw = fz_new_draw_device(ctx, scale, pix);
			fz_enable_device_hints(ctx, draw, FZ_NO_CACHE);
			if (flags & INVOICE_RENDER_PLAIN) {
				fz_run_page_contents(ctx, page, draw, fz_identity, NULL);
				fz_close_device(ctx, draw);
				fz_drop_device(ctx, draw);
				draw = NULL;
				break;
			}
			filter = new_invoice_filter(ctx, draw, keep_group, !(flags & INVOICE_RENDER_NO_IMAGE_CACHE));
			fz_enable_device_hints(ctx, filter, FZ_NO_CACHE);
			fz_run_page_contents(ctx, page, filter, fz_identity, NULL);
			fz_close_device(ctx, filter);
			int unsafe = FILTER(filter)->unsafe;
			fz_drop_device(ctx, filter);
			filter = NULL;
			fz_drop_device(ctx, draw);
			draw = NULL;
			if (!unsafe)
				break;
		}
	}
	fz_always(ctx) {
		fz_drop_device(ctx, filter);
		fz_drop_device(ctx, draw);
	}
	fz_catch(ctx) {
		if (pix)
			invoice_drop_page_pixmap(ctx, pix);
		else if (samples)
			page_buffer_give(samples);
		fz_rethrow(ctx);
	}
	return pix;
}

/*
 * Remove /ToUnicode from the Type0 fonts with embedded font programs on every
 * page.  Loading such a font remaps all 65536 codes of its Identity encoding
 * through the ToUnicode CMap, a large part of loading invoice fonts.  Glyphs of
 * embedded CID fonts come from the CIDs alone, so pixels do not change; only
 * text extraction loses its Unicode mapping.  Fonts in form XObjects keep it.
 */
void invoice_drop_to_unicode(fz_context *ctx, fz_document *doc)
{
	pdf_document *pdf = pdf_document_from_fz_document(ctx, doc);
	if (!pdf)
		return;
	int pages = pdf_count_pages(ctx, pdf);
	for (int i = 0; i < pages; i++) {
		pdf_obj *page = pdf_lookup_page_obj(ctx, pdf, i);
		pdf_obj *fonts = pdf_dict_gets(ctx, pdf_dict_getp_inheritable(ctx, page, "Resources"), "Font");
		int n = pdf_dict_len(ctx, fonts);
		for (int k = 0; k < n; k++) {
			pdf_obj *font = pdf_dict_get_val(ctx, fonts, k);
			if (strcmp(pdf_to_name(ctx, pdf_dict_gets(ctx, font, "Subtype")), "Type0"))
				continue;
			pdf_obj *cid = pdf_array_get(ctx, pdf_dict_gets(ctx, font, "DescendantFonts"), 0);
			pdf_obj *desc = pdf_dict_gets(ctx, cid, "FontDescriptor");
			if (pdf_dict_gets(ctx, desc, "FontFile2") || pdf_dict_gets(ctx, desc, "FontFile3") || pdf_dict_gets(ctx, desc, "FontFile"))
				pdf_dict_dels(ctx, font, "ToUnicode");
		}
	}
}
