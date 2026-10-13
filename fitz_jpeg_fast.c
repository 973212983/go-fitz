//go:build cgo && !nocgo

/*
 * Fast baseline JPEG encoder for rendered invoice pages.
 *
 * The output is byte-for-byte identical to MuPDF 1.28.0's bundled libjpeg 10
 * with jpeg_set_defaults() and jpeg_set_quality(quality, TRUE) for RGB input:
 * JFIF 1.01, YCbCr 4:2:0 with chroma downsampled by 16x16 scaled DCTs
 * (do_fancy_downsampling), JDCT_ISLOW, sequential mode and standard Huffman
 * tables.  The color conversion, forward DCTs, quantizer rounding, edge
 * padding, dummy blocks, Huffman coding and markers follow jccolor.c,
 * jfdctint.c, jcdctmgr.c, jcprepct.c, jcsample.c, jccoefct.c, jchuff.c and
 * jcmarker.c.  The DCT routines are adapted from jfdctint.c:
 *
 *   Copyright (C) 1991-1996, Thomas G. Lane.
 *   Modification developed 2003-2025 by Guido Vollbeding.
 *   This file is part of the Independent JPEG Group's software.
 *
 * Rendered documents are mostly blank paper and black text.  Blocks whose
 * samples are all equal have only a DC coefficient; for those the DCT is
 * skipped and the coefficient is computed in closed form.  Neutral pixels
 * have exactly Cb = Cr = CENTERJSAMPLE in libjpeg's fixed point conversion.
 * Everything else takes the full libjpeg path.
 */

#include "mupdf/fitz.h"
#include "fitz_jpeg_encode.h"

#include <stdint.h>
#include <string.h>

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "invoice JPEG encoder assumes little-endian pixel loads"
#endif

#define CONST_BITS 13
#define PASS1_BITS 2 /* 10 - BITS_IN_JSAMPLE */
#define PASS2_BITS 2 /* 10 - JPEG_DATA_PRECISION */
#define ONE ((int32_t)1)
#define FIX(x) ((int32_t)((x) * (ONE << CONST_BITS) + 0.5))
#define DESCALE(x, n) (((x) + (ONE << ((n)-1))) >> (n))

#define FIX_0_298631336 ((int32_t)2446)
#define FIX_0_390180644 ((int32_t)3196)
#define FIX_0_541196100 ((int32_t)4433)
#define FIX_0_765366865 ((int32_t)6270)
#define FIX_0_899976223 ((int32_t)7373)
#define FIX_1_175875602 ((int32_t)9633)
#define FIX_1_501321110 ((int32_t)12299)
#define FIX_1_847759065 ((int32_t)15137)
#define FIX_1_961570560 ((int32_t)16069)
#define FIX_2_053119869 ((int32_t)16819)
#define FIX_2_562915447 ((int32_t)20995)
#define FIX_3_072711026 ((int32_t)25172)

/* jccolor.c */
#define SCALEBITS 16
#define CBCR_OFFSET ((int32_t)128 << SCALEBITS)
#define ONE_HALF ((int32_t)1 << (SCALEBITS - 1))
#define CFIX(x) ((int32_t)((x) * (1L << SCALEBITS) + 0.5))

static const int natural_order[64] = {
	0, 1, 8, 16, 9, 2, 3, 10,
	17, 24, 32, 25, 18, 11, 4, 5,
	12, 19, 26, 33, 40, 48, 41, 34,
	27, 20, 13, 6, 7, 14, 21, 28,
	35, 42, 49, 56, 57, 50, 43, 36,
	29, 22, 15, 23, 30, 37, 44, 51,
	58, 59, 52, 45, 38, 31, 39, 46,
	53, 60, 61, 54, 47, 55, 62, 63
};

/* jcparam.c; the chrominance DC entry is 16 since libjpeg 9. */
static const unsigned int std_luminance_quant_tbl[64] = {
	16, 11, 10, 16, 24, 40, 51, 61,
	12, 12, 14, 19, 26, 58, 60, 55,
	14, 13, 16, 24, 40, 57, 69, 56,
	14, 17, 22, 29, 51, 87, 80, 62,
	18, 22, 37, 56, 68, 109, 103, 77,
	24, 35, 55, 64, 81, 104, 113, 92,
	49, 64, 78, 87, 103, 121, 120, 101,
	72, 92, 95, 98, 112, 100, 103, 99
};
static const unsigned int std_chrominance_quant_tbl[64] = {
	16, 18, 24, 47, 99, 99, 99, 99,
	18, 21, 26, 66, 99, 99, 99, 99,
	24, 26, 56, 99, 99, 99, 99, 99,
	47, 66, 99, 99, 99, 99, 99, 99,
	99, 99, 99, 99, 99, 99, 99, 99,
	99, 99, 99, 99, 99, 99, 99, 99,
	99, 99, 99, 99, 99, 99, 99, 99,
	99, 99, 99, 99, 99, 99, 99, 99
};

/* jcomapi.c, JPEG standard section K.3 */
static const unsigned char bits_dc_luminance[17] = { 0, 0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0 };
static const unsigned char val_dc_luminance[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
static const unsigned char bits_dc_chrominance[17] = { 0, 0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0 };
static const unsigned char val_dc_chrominance[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
static const unsigned char bits_ac_luminance[17] = { 0, 0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d };
static const unsigned char val_ac_luminance[] = {
	0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12,
	0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07,
	0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08,
	0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0,
	0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16,
	0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28,
	0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39,
	0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
	0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59,
	0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
	0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79,
	0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
	0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98,
	0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
	0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6,
	0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5,
	0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4,
	0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,
	0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea,
	0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
	0xf9, 0xfa
};
static const unsigned char bits_ac_chrominance[17] = { 0, 0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77 };
static const unsigned char val_ac_chrominance[] = {
	0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21,
	0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71,
	0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91,
	0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0,
	0x15, 0x62, 0x72, 0xd1, 0x0a, 0x16, 0x24, 0x34,
	0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26,
	0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38,
	0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
	0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58,
	0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68,
	0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78,
	0x79, 0x7a, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
	0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96,
	0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5,
	0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4,
	0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3,
	0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2,
	0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda,
	0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9,
	0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
	0xf9, 0xfa
};

typedef struct {
	uint16_t code[256];
	uint8_t size[256];
} huff_table;

typedef struct {
	uint32_t recip[64]; /* ceil(2^32 / divisor), zigzag order */
	uint16_t half[64];  /* divisor >> 1, zigzag order */
	int16_t dc[256];    /* quantized DC of a uniform block of each sample value */
} quant_table;

typedef struct {
	fz_context *ctx;
	fz_buffer *buf;
	unsigned char *out; /* write position inside buf->data */
	unsigned char *end; /* buf->data + buf->cap */
	uint64_t acc;       /* pending bits in the low 64 - free bits */
	int free;
} writer;

typedef struct {
	huff_table dc[2], ac[2];
	quant_table q[2];
	uint32_t blank_code; /* a uniform MCU whose DC values equal the previous MCU's */
	int blank_size;
	int last_dc[3];
	writer w;
} encoder;

/* jchuff.c jpeg_make_c_derived_tbl() for the standard tables. */
static void make_huff_table(huff_table *t, const unsigned char *bits, const unsigned char *val)
{
	unsigned int code = 0;
	int p = 0;
	memset(t, 0, sizeof *t);
	for (int l = 1; l <= 16; l++) {
		for (int i = 0; i < bits[l]; i++) {
			t->code[val[p]] = (uint16_t)code++;
			t->size[val[p]] = (uint8_t)l;
			p++;
		}
		code <<= 1;
	}
}

/* jcparam.c jpeg_add_quant_table() with force_baseline; jcdctmgr.c ISLOW divisors. */
static void make_quant_table(quant_table *t, unsigned char *zigzag, const unsigned int *basic, int scale)
{
	unsigned int qval[64];
	for (int i = 0; i < 64; i++) {
		long temp = ((long)basic[i] * scale + 50L) / 100L;
		if (temp <= 0L)
			temp = 1L;
		if (temp > 32767L)
			temp = 32767L;
		if (temp > 255L)
			temp = 255L;
		qval[i] = (unsigned int)temp;
	}
	for (int k = 0; k < 64; k++) {
		unsigned int q = qval[natural_order[k]];
		uint32_t divisor = q << 3;
		zigzag[k] = (unsigned char)q;
		/* (n * ceil(2^32/d)) >> 32 == n / d exactly for n * d < 2^32. */
		t->recip[k] = (uint32_t)((((uint64_t)1 << 32) + divisor - 1) / divisor);
		t->half[k] = (uint16_t)(divisor >> 1);
	}
	/* A uniform block of sample v transforms to DC = 64 * (v - 128), AC = 0,
	 * in both jpeg_fdct_islow() and jpeg_fdct_16x16(). */
	uint32_t divisor = qval[0] << 3;
	for (int v = 0; v < 256; v++) {
		int temp = 64 * (v - 128);
		uint32_t a = (uint32_t)(temp < 0 ? -temp : temp) + (divisor >> 1);
		int q = (int)(a / divisor);
		t->dc[v] = (int16_t)(temp < 0 ? -q : q);
	}
}

static void grow(writer *w, size_t need)
{
	size_t len = (size_t)(w->out - w->buf->data);
	if ((size_t)(w->end - w->out) >= need)
		return;
	size_t cap = w->buf->cap;
	while (cap - len < need)
		cap += cap / 2 + need;
	fz_resize_buffer(w->ctx, w->buf, cap);
	w->out = w->buf->data + len;
	w->end = w->buf->data + w->buf->cap;
}

static inline void put_byte(writer *w, int c)
{
	*w->out++ = (unsigned char)c;
}

static inline void put_2bytes(writer *w, int v)
{
	put_byte(w, (v >> 8) & 0xFF);
	put_byte(w, v & 0xFF);
}

/* Output eight bytes of entropy data, stuffing a zero after each 0xFF. */
static inline void flush_word(writer *w, uint64_t v)
{
	/* No false negatives: a 0xFF byte always sets its high bit here. */
	if (v & 0x8080808080808080ull & ~(v + 0x0101010101010101ull)) {
		for (int s = 56; s >= 0; s -= 8) {
			unsigned char c = (unsigned char)(v >> s);
			*w->out++ = c;
			if (c == 0xFF)
				*w->out++ = 0;
		}
	} else {
		v = __builtin_bswap64(v);
		memcpy(w->out, &v, 8);
		w->out += 8;
	}
}

/* code < 2^size, size <= 32 */
static inline void put_bits(writer *w, uint64_t code, int size)
{
	w->free -= size;
	if (w->free >= 0) {
		w->acc = (w->acc << size) | code;
	} else {
		flush_word(w, (w->acc << (size + w->free)) | (code >> -w->free));
		w->free += 64;
		w->acc = code;
	}
}

/* jchuff.c flush_bits_s(): pad the last byte with one bits. */
static void flush_bits(writer *w)
{
	int bits = 64 - w->free;
	if (bits % 8)
		put_bits(w, (1u << (8 - bits % 8)) - 1, 8 - bits % 8);
	bits = 64 - w->free;
	for (int s = bits - 8; s >= 0; s -= 8) {
		unsigned char c = (unsigned char)(w->acc >> s);
		*w->out++ = c;
		if (c == 0xFF)
			*w->out++ = 0;
	}
	w->acc = 0;
	w->free = 64;
}

static inline int bit_length(unsigned int v)
{
	return v ? 32 - __builtin_clz(v) : 0;
}

/* Huffman code for a DC difference followed by its magnitude bits. */
static inline uint64_t dc_bits(writer *w, int diff, const huff_table *dct, int *size)
{
	int sign = diff >> 31;
	int nbits = bit_length((unsigned int)((diff ^ sign) - sign));
	if (nbits > 11)
		fz_throw(w->ctx, FZ_ERROR_LIBRARY, "JPEG DC coefficient out of range");
	*size = dct->size[nbits] + nbits;
	return ((uint64_t)dct->code[nbits] << nbits) | ((unsigned int)(diff + sign) & ((1u << nbits) - 1));
}

/* jchuff.c encode_one_block(); zz is in zigzag order, bit k of nz set when zz[k] != 0. */
static void encode_block(writer *wp, const int16_t *zz, uint64_t nz, int last_dc, const huff_table *dct, const huff_table *act)
{
	/* A local copy keeps the bit buffer in registers despite the byte stores. */
	writer local = *wp, *w = &local;
	int size;
	uint64_t bits = dc_bits(w, zz[0] - last_dc, dct, &size);
	put_bits(w, bits, size);
	int last = 0;
	nz &= ~(uint64_t)1;
	while (nz) {
		int k = __builtin_ctzll(nz);
		nz &= nz - 1;
		int r = k - last - 1;
		while (r > 15) {
			put_bits(w, act->code[0xF0], act->size[0xF0]);
			r -= 16;
		}
		int v = zz[k];
		int sign = v >> 31;
		int nbits = bit_length((unsigned int)((v ^ sign) - sign));
		if (nbits > 10)
			fz_throw(w->ctx, FZ_ERROR_LIBRARY, "JPEG AC coefficient out of range");
		int sym = (r << 4) + nbits;
		put_bits(w, ((uint64_t)act->code[sym] << nbits) | ((unsigned int)(v + sign) & ((1u << nbits) - 1)), act->size[sym] + nbits);
		last = k;
	}
	if (last != 63)
		put_bits(w, act->code[0], act->size[0]);
	*wp = local;
}

/* A block whose AC coefficients are all zero: DC difference and end of block. */
static inline void encode_dc_block(writer *w, int dc, int last_dc, const huff_table *dct, const huff_table *act)
{
	int size;
	uint64_t bits = dc_bits(w, dc - last_dc, dct, &size);
	put_bits(w, (bits << act->size[0]) | act->code[0], size + act->size[0]);
}

/* jcdctmgr.c forward_DCT() quantization, rounding half away from zero,
 * written in zigzag order; returns the bitmap of nonzero coefficients. */
static inline uint64_t quantize(int16_t *zz, const int *ws, const quant_table *q)
{
	uint64_t nz = 0;
	for (int k = 0; k < 64; k++) {
		int x = ws[natural_order[k]];
		int sign = x >> 31;
		uint32_t a = (uint32_t)((x ^ sign) - sign) + q->half[k];
		int v = (int)(((uint64_t)a * q->recip[k]) >> 32);
		v = (v ^ sign) - sign;
		zz[k] = (int16_t)v;
		nz |= (uint64_t)(v != 0) << k;
	}
	return nz;
}

/* jfdctint.c jpeg_fdct_islow() */
static void fdct_islow(int *data, const unsigned char *s, int stride)
{
	int32_t tmp0, tmp1, tmp2, tmp3, tmp10, tmp11, tmp12, tmp13, z1;
	int *p = data;
	for (int ctr = 0; ctr < 8; ctr++, s += stride, p += 8) {
		tmp0 = s[0] + s[7];
		tmp1 = s[1] + s[6];
		tmp2 = s[2] + s[5];
		tmp3 = s[3] + s[4];
		tmp10 = tmp0 + tmp3;
		tmp12 = tmp0 - tmp3;
		tmp11 = tmp1 + tmp2;
		tmp13 = tmp1 - tmp2;
		tmp0 = s[0] - s[7];
		tmp1 = s[1] - s[6];
		tmp2 = s[2] - s[5];
		tmp3 = s[3] - s[4];
		p[0] = (tmp10 + tmp11 - 8 * 128) * (1 << PASS1_BITS);
		p[4] = (tmp10 - tmp11) * (1 << PASS1_BITS);
		z1 = (tmp12 + tmp13) * FIX_0_541196100;
		z1 += ONE << (CONST_BITS - PASS1_BITS - 1);
		p[2] = (z1 + tmp12 * FIX_0_765366865) >> (CONST_BITS - PASS1_BITS);
		p[6] = (z1 - tmp13 * FIX_1_847759065) >> (CONST_BITS - PASS1_BITS);
		tmp12 = tmp0 + tmp2;
		tmp13 = tmp1 + tmp3;
		z1 = (tmp12 + tmp13) * FIX_1_175875602;
		z1 += ONE << (CONST_BITS - PASS1_BITS - 1);
		tmp12 = tmp12 * -FIX_0_390180644;
		tmp13 = tmp13 * -FIX_1_961570560;
		tmp12 += z1;
		tmp13 += z1;
		z1 = (tmp0 + tmp3) * -FIX_0_899976223;
		tmp0 = tmp0 * FIX_1_501321110;
		tmp3 = tmp3 * FIX_0_298631336;
		tmp0 += z1 + tmp12;
		tmp3 += z1 + tmp13;
		z1 = (tmp1 + tmp2) * -FIX_2_562915447;
		tmp1 = tmp1 * FIX_3_072711026;
		tmp2 = tmp2 * FIX_2_053119869;
		tmp1 += z1 + tmp13;
		tmp2 += z1 + tmp12;
		p[1] = tmp0 >> (CONST_BITS - PASS1_BITS);
		p[3] = tmp1 >> (CONST_BITS - PASS1_BITS);
		p[5] = tmp2 >> (CONST_BITS - PASS1_BITS);
		p[7] = tmp3 >> (CONST_BITS - PASS1_BITS);
	}
	p = data;
	for (int ctr = 0; ctr < 8; ctr++, p++) {
		tmp0 = p[8 * 0] + p[8 * 7];
		tmp1 = p[8 * 1] + p[8 * 6];
		tmp2 = p[8 * 2] + p[8 * 5];
		tmp3 = p[8 * 3] + p[8 * 4];
		tmp10 = tmp0 + tmp3 + (ONE << (PASS2_BITS - 1));
		tmp12 = tmp0 - tmp3;
		tmp11 = tmp1 + tmp2;
		tmp13 = tmp1 - tmp2;
		tmp0 = p[8 * 0] - p[8 * 7];
		tmp1 = p[8 * 1] - p[8 * 6];
		tmp2 = p[8 * 2] - p[8 * 5];
		tmp3 = p[8 * 3] - p[8 * 4];
		p[8 * 0] = (tmp10 + tmp11) >> PASS2_BITS;
		p[8 * 4] = (tmp10 - tmp11) >> PASS2_BITS;
		z1 = (tmp12 + tmp13) * FIX_0_541196100;
		z1 += ONE << (CONST_BITS + PASS2_BITS - 1);
		p[8 * 2] = (z1 + tmp12 * FIX_0_765366865) >> (CONST_BITS + PASS2_BITS);
		p[8 * 6] = (z1 - tmp13 * FIX_1_847759065) >> (CONST_BITS + PASS2_BITS);
		tmp12 = tmp0 + tmp2;
		tmp13 = tmp1 + tmp3;
		z1 = (tmp12 + tmp13) * FIX_1_175875602;
		z1 += ONE << (CONST_BITS + PASS2_BITS - 1);
		tmp12 = tmp12 * -FIX_0_390180644;
		tmp13 = tmp13 * -FIX_1_961570560;
		tmp12 += z1;
		tmp13 += z1;
		z1 = (tmp0 + tmp3) * -FIX_0_899976223;
		tmp0 = tmp0 * FIX_1_501321110;
		tmp3 = tmp3 * FIX_0_298631336;
		tmp0 += z1 + tmp12;
		tmp3 += z1 + tmp13;
		z1 = (tmp1 + tmp2) * -FIX_2_562915447;
		tmp1 = tmp1 * FIX_3_072711026;
		tmp2 = tmp2 * FIX_2_053119869;
		tmp1 += z1 + tmp13;
		tmp2 += z1 + tmp12;
		p[8 * 1] = tmp0 >> (CONST_BITS + PASS2_BITS);
		p[8 * 3] = tmp1 >> (CONST_BITS + PASS2_BITS);
		p[8 * 5] = tmp2 >> (CONST_BITS + PASS2_BITS);
		p[8 * 7] = tmp3 >> (CONST_BITS + PASS2_BITS);
	}
}

/* jfdctint.c jpeg_fdct_16x16(): 16x16 samples to 8x8 coefficients. */
static void fdct_16x16(int *data, const unsigned char *s, int stride)
{
	int32_t tmp0, tmp1, tmp2, tmp3, tmp4, tmp5, tmp6, tmp7;
	int32_t tmp10, tmp11, tmp12, tmp13, tmp14, tmp15, tmp16, tmp17;
	int workspace[64];
	int *p = data;
	for (int ctr = 0; ctr < 16; ctr++, s += stride) {
		if (ctr == 8)
			p = workspace;
		tmp0 = s[0] + s[15];
		tmp1 = s[1] + s[14];
		tmp2 = s[2] + s[13];
		tmp3 = s[3] + s[12];
		tmp4 = s[4] + s[11];
		tmp5 = s[5] + s[10];
		tmp6 = s[6] + s[9];
		tmp7 = s[7] + s[8];
		tmp10 = tmp0 + tmp7;
		tmp14 = tmp0 - tmp7;
		tmp11 = tmp1 + tmp6;
		tmp15 = tmp1 - tmp6;
		tmp12 = tmp2 + tmp5;
		tmp16 = tmp2 - tmp5;
		tmp13 = tmp3 + tmp4;
		tmp17 = tmp3 - tmp4;
		tmp0 = s[0] - s[15];
		tmp1 = s[1] - s[14];
		tmp2 = s[2] - s[13];
		tmp3 = s[3] - s[12];
		tmp4 = s[4] - s[11];
		tmp5 = s[5] - s[10];
		tmp6 = s[6] - s[9];
		tmp7 = s[7] - s[8];
		p[0] = (tmp10 + tmp11 + tmp12 + tmp13 - 16 * 128) * (1 << PASS1_BITS);
		p[4] = DESCALE((tmp10 - tmp13) * FIX(1.306562965) + (tmp11 - tmp12) * FIX_0_541196100, CONST_BITS - PASS1_BITS);
		tmp10 = (tmp17 - tmp15) * FIX(0.275899379) + (tmp14 - tmp16) * FIX(1.387039845);
		p[2] = DESCALE(tmp10 + tmp15 * FIX(1.451774982) + tmp16 * FIX(2.172734804), CONST_BITS - PASS1_BITS);
		p[6] = DESCALE(tmp10 - tmp14 * FIX(0.211164243) - tmp17 * FIX(1.061594338), CONST_BITS - PASS1_BITS);
		tmp11 = (tmp0 + tmp1) * FIX(1.353318001) + (tmp6 - tmp7) * FIX(0.410524528);
		tmp12 = (tmp0 + tmp2) * FIX(1.247225013) + (tmp5 + tmp7) * FIX(0.666655658);
		tmp13 = (tmp0 + tmp3) * FIX(1.093201867) + (tmp4 - tmp7) * FIX(0.897167586);
		tmp14 = (tmp1 + tmp2) * FIX(0.138617169) + (tmp6 - tmp5) * FIX(1.407403738);
		tmp15 = (tmp1 + tmp3) * -FIX(0.666655658) + (tmp4 + tmp6) * -FIX(1.247225013);
		tmp16 = (tmp2 + tmp3) * -FIX(1.353318001) + (tmp5 - tmp4) * FIX(0.410524528);
		tmp10 = tmp11 + tmp12 + tmp13 - tmp0 * FIX(2.286341144) + tmp7 * FIX(0.779653625);
		tmp11 += tmp14 + tmp15 + tmp1 * FIX(0.071888074) - tmp6 * FIX(1.663905119);
		tmp12 += tmp14 + tmp16 - tmp2 * FIX(1.125726048) + tmp5 * FIX(1.227391138);
		tmp13 += tmp15 + tmp16 + tmp3 * FIX(1.065388962) + tmp4 * FIX(2.167985692);
		p[1] = DESCALE(tmp10, CONST_BITS - PASS1_BITS);
		p[3] = DESCALE(tmp11, CONST_BITS - PASS1_BITS);
		p[5] = DESCALE(tmp12, CONST_BITS - PASS1_BITS);
		p[7] = DESCALE(tmp13, CONST_BITS - PASS1_BITS);
		p += 8;
	}
	p = data;
	int *ws = workspace;
	for (int ctr = 0; ctr < 8; ctr++, p++, ws++) {
		tmp0 = p[8 * 0] + ws[8 * 7];
		tmp1 = p[8 * 1] + ws[8 * 6];
		tmp2 = p[8 * 2] + ws[8 * 5];
		tmp3 = p[8 * 3] + ws[8 * 4];
		tmp4 = p[8 * 4] + ws[8 * 3];
		tmp5 = p[8 * 5] + ws[8 * 2];
		tmp6 = p[8 * 6] + ws[8 * 1];
		tmp7 = p[8 * 7] + ws[8 * 0];
		tmp10 = tmp0 + tmp7;
		tmp14 = tmp0 - tmp7;
		tmp11 = tmp1 + tmp6;
		tmp15 = tmp1 - tmp6;
		tmp12 = tmp2 + tmp5;
		tmp16 = tmp2 - tmp5;
		tmp13 = tmp3 + tmp4;
		tmp17 = tmp3 - tmp4;
		tmp0 = p[8 * 0] - ws[8 * 7];
		tmp1 = p[8 * 1] - ws[8 * 6];
		tmp2 = p[8 * 2] - ws[8 * 5];
		tmp3 = p[8 * 3] - ws[8 * 4];
		tmp4 = p[8 * 4] - ws[8 * 3];
		tmp5 = p[8 * 5] - ws[8 * 2];
		tmp6 = p[8 * 6] - ws[8 * 1];
		tmp7 = p[8 * 7] - ws[8 * 0];
		p[8 * 0] = DESCALE(tmp10 + tmp11 + tmp12 + tmp13, PASS2_BITS + 2);
		p[8 * 4] = DESCALE((tmp10 - tmp13) * FIX(1.306562965) + (tmp11 - tmp12) * FIX_0_541196100, CONST_BITS + PASS2_BITS + 2);
		tmp10 = (tmp17 - tmp15) * FIX(0.275899379) + (tmp14 - tmp16) * FIX(1.387039845);
		p[8 * 2] = DESCALE(tmp10 + tmp15 * FIX(1.451774982) + tmp16 * FIX(2.172734804), CONST_BITS + PASS2_BITS + 2);
		p[8 * 6] = DESCALE(tmp10 - tmp14 * FIX(0.211164243) - tmp17 * FIX(1.061594338), CONST_BITS + PASS2_BITS + 2);
		tmp11 = (tmp0 + tmp1) * FIX(1.353318001) + (tmp6 - tmp7) * FIX(0.410524528);
		tmp12 = (tmp0 + tmp2) * FIX(1.247225013) + (tmp5 + tmp7) * FIX(0.666655658);
		tmp13 = (tmp0 + tmp3) * FIX(1.093201867) + (tmp4 - tmp7) * FIX(0.897167586);
		tmp14 = (tmp1 + tmp2) * FIX(0.138617169) + (tmp6 - tmp5) * FIX(1.407403738);
		tmp15 = (tmp1 + tmp3) * -FIX(0.666655658) + (tmp4 + tmp6) * -FIX(1.247225013);
		tmp16 = (tmp2 + tmp3) * -FIX(1.353318001) + (tmp5 - tmp4) * FIX(0.410524528);
		tmp10 = tmp11 + tmp12 + tmp13 - tmp0 * FIX(2.286341144) + tmp7 * FIX(0.779653625);
		tmp11 += tmp14 + tmp15 + tmp1 * FIX(0.071888074) - tmp6 * FIX(1.663905119);
		tmp12 += tmp14 + tmp16 - tmp2 * FIX(1.125726048) + tmp5 * FIX(1.227391138);
		tmp13 += tmp15 + tmp16 + tmp3 * FIX(1.065388962) + tmp4 * FIX(2.167985692);
		p[8 * 1] = DESCALE(tmp10, CONST_BITS + PASS2_BITS + 2);
		p[8 * 3] = DESCALE(tmp11, CONST_BITS + PASS2_BITS + 2);
		p[8 * 5] = DESCALE(tmp12, CONST_BITS + PASS2_BITS + 2);
		p[8 * 7] = DESCALE(tmp13, CONST_BITS + PASS2_BITS + 2);
	}
}

/* jccolor.c rgb_ycc_convert() for one pixel packed as 0x00BBGGRR. */
static inline int rgb_y(uint32_t px)
{
	int r = px & 0xFF, g = (px >> 8) & 0xFF, b = px >> 16;
	return (CFIX(0.299) * r + CFIX(0.587) * g + CFIX(0.114) * b + ONE_HALF) >> SCALEBITS;
}

static inline int rgb_cb(uint32_t px)
{
	int r = px & 0xFF, g = (px >> 8) & 0xFF, b = px >> 16;
	return (-CFIX(0.168735892) * r - CFIX(0.331264108) * g + (b << (SCALEBITS - 1)) + CBCR_OFFSET + ONE_HALF - 1) >> SCALEBITS;
}

static inline int rgb_cr(uint32_t px)
{
	int r = px & 0xFF, g = (px >> 8) & 0xFF, b = px >> 16;
	return ((r << (SCALEBITS - 1)) + CBCR_OFFSET + ONE_HALF - 1 - CFIX(0.418687589) * g - CFIX(0.081312411) * b) >> SCALEBITS;
}

static inline uint32_t load_px(const unsigned char *p, int n)
{
	if (n == 4) {
		uint32_t v;
		memcpy(&v, p, 4);
		return v & 0x00FFFFFFu; /* R, G, B; alpha is ignored */
	}
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
}

/* Copy one MCU of packed RGB; columns and rows past the image repeat the edge,
 * like expand_right_edge() and expand_bottom_edge(). */
static void gather_mcu(uint32_t *px, const fz_pixmap *pix, int x0, int y0)
{
	int n = pix->n;
	for (int y = 0; y < 16; y++, px += 16) {
		int sy = y0 + y < pix->h ? y0 + y : pix->h - 1;
		const unsigned char *row = pix->samples + (size_t)sy * pix->stride;
		if (x0 + 16 <= pix->w) {
			const unsigned char *p = row + (size_t)x0 * n;
			for (int x = 0; x < 16; x++, p += n)
				px[x] = load_px(p, n);
		} else {
			for (int x = 0; x < 16; x++) {
				int sx = x0 + x < pix->w ? x0 + x : pix->w - 1;
				px[x] = load_px(row + (size_t)sx * n, n);
			}
		}
	}
}

/* Whether every pixel of an RGBA MCU inside the image has the same color. */
static int uniform_rgba_mcu(const fz_pixmap *pix, int x0, int y0, uint32_t *value)
{
	const unsigned char *row = pix->samples + (size_t)y0 * pix->stride + (size_t)x0 * 4;
	uint32_t first;
	memcpy(&first, row, 4);
	first &= 0x00FFFFFFu;
	uint64_t want = first | ((uint64_t)first << 32);
	for (int y = 0; y < 16; y++, row += pix->stride) {
		uint64_t diff = 0;
		for (int x = 0; x < 64; x += 8) {
			uint64_t v;
			memcpy(&v, row + x, 8);
			diff |= (v & 0x00FFFFFF00FFFFFFull) ^ want;
		}
		if (diff)
			return 0;
	}
	*value = first;
	return 1;
}

/* The same for RGB pixmaps: a row of 16 pixels is six words repeating every 24 bytes. */
static int uniform_rgb_mcu(const fz_pixmap *pix, int x0, int y0, uint32_t *value)
{
	const unsigned char *row = pix->samples + (size_t)y0 * pix->stride + (size_t)x0 * 3;
	unsigned char pattern[24];
	for (int i = 0; i < 24; i += 3)
		memcpy(pattern + i, row, 3);
	uint64_t p[3];
	memcpy(p, pattern, 24);
	for (int y = 0; y < 16; y++, row += pix->stride) {
		uint64_t v[6];
		memcpy(v, row, 48);
		if ((v[0] ^ p[0]) | (v[1] ^ p[1]) | (v[2] ^ p[2]) | (v[3] ^ p[0]) | (v[4] ^ p[1]) | (v[5] ^ p[2]))
			return 0;
	}
	*value = load_px(pattern, 3);
	return 1;
}

static void encode_uniform_mcu(encoder *e, uint32_t value)
{
	int dy = e->q[0].dc[rgb_y(value)];
	int dcb = e->q[1].dc[rgb_cb(value)];
	int dcr = e->q[1].dc[rgb_cr(value)];
	writer *w = &e->w;
	if (dy == e->last_dc[0] && dcb == e->last_dc[1] && dcr == e->last_dc[2]) {
		put_bits(w, e->blank_code, e->blank_size);
		return;
	}
	/* Dummy blocks copy the DC of the previous block, which is the same value. */
	encode_dc_block(w, dy, e->last_dc[0], &e->dc[0], &e->ac[0]);
	for (int i = 1; i < 4; i++)
		encode_dc_block(w, dy, dy, &e->dc[0], &e->ac[0]);
	encode_dc_block(w, dcb, e->last_dc[1], &e->dc[1], &e->ac[1]);
	encode_dc_block(w, dcr, e->last_dc[2], &e->dc[1], &e->ac[1]);
	e->last_dc[0] = dy;
	e->last_dc[1] = dcb;
	e->last_dc[2] = dcr;
}

/* One chroma component of an MCU: 16x16 full resolution samples, one block. */
static void encode_chroma(encoder *e, int ci, const unsigned char *s)
{
	writer *w = &e->w;
	unsigned int diff = 0;
	for (int i = 0; i < 256; i++)
		diff |= s[i] ^ s[0];
	if (!diff) {
		int dc = e->q[1].dc[s[0]];
		encode_dc_block(w, dc, e->last_dc[ci], &e->dc[1], &e->ac[1]);
		e->last_dc[ci] = dc;
		return;
	}
	int ws[64];
	int16_t zz[64];
	fdct_16x16(ws, s, 16);
	uint64_t nz = quantize(zz, ws, &e->q[1]);
	encode_block(w, zz, nz, e->last_dc[ci], &e->dc[1], &e->ac[1]);
	e->last_dc[ci] = zz[0];
}

/* Encode an MCU that is not a single color.  y_wide and y_high give the
 * luminance blocks inside the image; the rest are dummy blocks (jccoefct.c). */
static void encode_mcu(encoder *e, const uint32_t *px, int y_wide, int y_high)
{
	writer *w = &e->w;
	unsigned char ys[256];
	int have_y = 0;
	int dc[4];

	for (int b = 0; b < 4; b++) {
		int by = b >> 1, bx = b & 1;
		int last = b > 0 ? dc[b - 1] : e->last_dc[0];
		if (by >= y_high || bx >= y_wide) {
			/* Dummy block: DC of the previous block, no AC. */
			dc[b] = last;
			encode_dc_block(w, last, last, &e->dc[0], &e->ac[0]);
			continue;
		}
		const uint32_t *p = px + by * 128 + bx * 8;
		uint32_t diff = 0;
		for (int y = 0; y < 8; y++)
			for (int x = 0; x < 8; x++)
				diff |= p[y * 16 + x] ^ p[0];
		if (!diff) {
			dc[b] = e->q[0].dc[rgb_y(p[0])];
			encode_dc_block(w, dc[b], last, &e->dc[0], &e->ac[0]);
			continue;
		}
		if (!have_y) {
			for (int i = 0; i < 256; i++)
				ys[i] = (unsigned char)rgb_y(px[i]);
			have_y = 1;
		}
		const unsigned char *s = ys + by * 128 + bx * 8;
		unsigned int ydiff = 0;
		for (int y = 0; y < 8; y++)
			for (int x = 0; x < 8; x++)
				ydiff |= s[y * 16 + x] ^ s[0];
		if (!ydiff) {
			dc[b] = e->q[0].dc[s[0]];
			encode_dc_block(w, dc[b], last, &e->dc[0], &e->ac[0]);
			continue;
		}
		int ws[64];
		int16_t zz[64];
		fdct_islow(ws, s, 16);
		uint64_t nz = quantize(zz, ws, &e->q[0]);
		encode_block(w, zz, nz, last, &e->dc[0], &e->ac[0]);
		dc[b] = zz[0];
	}
	e->last_dc[0] = dc[3];

	uint32_t color = 0;
	for (int i = 0; i < 256; i++)
		color |= (px[i] ^ (px[i] >> 8)) & 0xFFFF;
	if (!color) {
		/* R = G = B gives Cb = Cr = CENTERJSAMPLE exactly, a zero DC. */
		int z = e->q[1].dc[128];
		encode_dc_block(w, z, e->last_dc[1], &e->dc[1], &e->ac[1]);
		encode_dc_block(w, z, e->last_dc[2], &e->dc[1], &e->ac[1]);
		e->last_dc[1] = e->last_dc[2] = z;
		return;
	}
	unsigned char cb[256], cr[256];
	for (int i = 0; i < 256; i++) {
		cb[i] = (unsigned char)rgb_cb(px[i]);
		cr[i] = (unsigned char)rgb_cr(px[i]);
	}
	encode_chroma(e, 1, cb);
	encode_chroma(e, 2, cr);
}

/* jcmarker.c: SOI, JFIF APP0, DQT, SOF0, DHT and SOS as libjpeg writes them. */
static void write_headers(encoder *e, const fz_pixmap *pix, const unsigned char zigzag[2][64])
{
	writer *w = &e->w;
	put_2bytes(w, 0xFFD8);
	put_2bytes(w, 0xFFE0);
	put_2bytes(w, 16);
	put_byte(w, 'J');
	put_byte(w, 'F');
	put_byte(w, 'I');
	put_byte(w, 'F');
	put_byte(w, 0);
	put_byte(w, 1); /* version 1.01 */
	put_byte(w, 1);
	put_byte(w, 1); /* dots per inch */
	put_2bytes(w, pix->xres);
	put_2bytes(w, pix->yres);
	put_byte(w, 0);
	put_byte(w, 0);
	for (int t = 0; t < 2; t++) {
		put_2bytes(w, 0xFFDB);
		put_2bytes(w, 67);
		put_byte(w, t);
		for (int i = 0; i < 64; i++)
			put_byte(w, zigzag[t][i]);
	}
	put_2bytes(w, 0xFFC0);
	put_2bytes(w, 17);
	put_byte(w, 8);
	put_2bytes(w, pix->h);
	put_2bytes(w, pix->w);
	put_byte(w, 3);
	static const unsigned char comps[9] = { 1, 0x22, 0, 2, 0x11, 1, 3, 0x11, 1 };
	for (int i = 0; i < 9; i++)
		put_byte(w, comps[i]);
	const unsigned char *bits[4] = { bits_dc_luminance, bits_ac_luminance, bits_dc_chrominance, bits_ac_chrominance };
	const unsigned char *vals[4] = { val_dc_luminance, val_ac_luminance, val_dc_chrominance, val_ac_chrominance };
	static const int index[4] = { 0x00, 0x10, 0x01, 0x11 };
	for (int t = 0; t < 4; t++) {
		int length = 0;
		for (int i = 1; i <= 16; i++)
			length += bits[t][i];
		put_2bytes(w, 0xFFC4);
		put_2bytes(w, length + 2 + 1 + 16);
		put_byte(w, index[t]);
		for (int i = 1; i <= 16; i++)
			put_byte(w, bits[t][i]);
		for (int i = 0; i < length; i++)
			put_byte(w, vals[t][i]);
	}
	put_2bytes(w, 0xFFDA);
	put_2bytes(w, 12);
	static const unsigned char scan[10] = { 3, 1, 0x00, 2, 0x11, 3, 0x11, 0, 63, 0 };
	for (int i = 0; i < 10; i++)
		put_byte(w, scan[i]);
}

static void encode_pixmap(encoder *e, const fz_pixmap *pix, int quality)
{
	/* jcparam.c jpeg_quality_scaling() */
	int scale = quality < 50 ? 5000 / quality : 200 - quality * 2;
	unsigned char zigzag[2][64];
	make_quant_table(&e->q[0], zigzag[0], std_luminance_quant_tbl, scale);
	make_quant_table(&e->q[1], zigzag[1], std_chrominance_quant_tbl, scale);
	make_huff_table(&e->dc[0], bits_dc_luminance, val_dc_luminance);
	make_huff_table(&e->ac[0], bits_ac_luminance, val_ac_luminance);
	make_huff_table(&e->dc[1], bits_dc_chrominance, val_dc_chrominance);
	make_huff_table(&e->ac[1], bits_ac_chrominance, val_ac_chrominance);
	for (int b = 0; b < 6; b++) {
		const huff_table *dct = &e->dc[b < 4 ? 0 : 1], *act = &e->ac[b < 4 ? 0 : 1];
		e->blank_code = (e->blank_code << (dct->size[0] + act->size[0])) | ((uint32_t)dct->code[0] << act->size[0]) | act->code[0];
		e->blank_size += dct->size[0] + act->size[0];
	}
	e->w.free = 64;

	grow(&e->w, 1024);
	write_headers(e, pix, (const unsigned char (*)[64])zigzag);

	int w = pix->w, h = pix->h;
	int mcus_wide = (w + 15) / 16, mcus_high = (h + 15) / 16;
	/* Luminance blocks inside the image in the last MCU column and row. */
	int y_last_wide = (w + 7) / 8 % 2 ? 1 : 2;
	int y_last_high = (h + 7) / 8 % 2 ? 1 : 2;
	uint32_t px[256];
	for (int my = 0; my < mcus_high; my++) {
		int y0 = my * 16;
		int y_high = my == mcus_high - 1 ? y_last_high : 2;
		/* A block needs at most 213 bytes, twice that with 0xFF stuffing. */
		grow(&e->w, (size_t)mcus_wide * 6 * 512 + 64);
		for (int mx = 0; mx < mcus_wide; mx++) {
			int x0 = mx * 16;
			int y_wide = mx == mcus_wide - 1 ? y_last_wide : 2;
			uint32_t value;
			if (x0 + 16 <= w && y0 + 16 <= h) {
				if (pix->n == 4 ? uniform_rgba_mcu(pix, x0, y0, &value) : uniform_rgb_mcu(pix, x0, y0, &value)) {
					encode_uniform_mcu(e, value);
					continue;
				}
				gather_mcu(px, pix, x0, y0);
			} else {
				gather_mcu(px, pix, x0, y0);
				uint32_t diff = 0;
				for (int i = 0; i < 256; i++)
					diff |= px[i] ^ px[0];
				if (!diff) {
					encode_uniform_mcu(e, px[0]);
					continue;
				}
			}
			encode_mcu(e, px, y_wide, y_high);
		}
	}
	grow(&e->w, 32);
	flush_bits(&e->w);
	put_2bytes(&e->w, 0xFFD9);
}

/* Encode an RGB or opaque RGBA pixmap; same bytes as invoice_new_buffer_as_jpeg(). */
fz_buffer *invoice_encode_jpeg(fz_context *ctx, const fz_pixmap *pix, int quality)
{
	fz_buffer *buf = NULL;
	encoder *e = NULL;
	fz_var(buf);
	fz_var(e);

	if (pix->n - pix->alpha != 3 || (pix->colorspace && !fz_colorspace_is_rgb(ctx, pix->colorspace)))
		fz_throw(ctx, FZ_ERROR_ARGUMENT, "JPEG encoder needs an RGB pixmap");
	if (pix->s > 0)
		fz_throw(ctx, FZ_ERROR_ARGUMENT, "JPEG encoder needs a pixmap without separations");
	if (pix->w <= 0 || pix->h <= 0 || pix->w > 65500 || pix->h > 65500)
		fz_throw(ctx, FZ_ERROR_ARGUMENT, "invalid JPEG image size %dx%d", pix->w, pix->h);
	if (quality < 1 || quality > 100)
		fz_throw(ctx, FZ_ERROR_ARGUMENT, "invalid JPEG quality %d", quality);

	fz_try(ctx) {
		e = fz_malloc_struct(ctx, encoder);
		buf = fz_new_buffer(ctx, 64 << 10);
		e->w.ctx = ctx;
		e->w.buf = buf;
		e->w.out = buf->data;
		e->w.end = buf->data + buf->cap;
		encode_pixmap(e, pix, quality);
		buf->len = (size_t)(e->w.out - buf->data);
	}
	fz_always(ctx)
		fz_free(ctx, e);
	fz_catch(ctx) {
		fz_drop_buffer(ctx, buf);
		fz_rethrow(ctx);
	}
	return buf;
}
