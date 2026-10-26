/* libs/libturbojpeg_*.a carry every libjpeg-turbo symbol with the fitz_ljt_
 * prefix, so they never clash with the libjpeg inside libmupdfthird.  Include
 * this header instead of turbojpeg.h to declare the prefixed functions. */
#ifndef FITZ_TURBOJPEG_H
#define FITZ_TURBOJPEG_H

#define tj3InitVersion fitz_ljt_tj3InitVersion
#define tj3Destroy fitz_ljt_tj3Destroy
#define tj3GetErrorStr fitz_ljt_tj3GetErrorStr
#define tj3Set fitz_ljt_tj3Set
#define tj3Free fitz_ljt_tj3Free
#define tj3Compress8 fitz_ljt_tj3Compress8

#include "turbojpeg.h"

/* Shared encoder settings: baseline YCbCr 4:2:0, accurate integer DCT,
 * standard Huffman tables and a JFIF density in dots per inch.  Every
 * libjpeg-turbo encoding path in this module goes through here, so they all
 * write the same stream for the same pixels. */
static inline int fitz_tj_compress(tjhandle tj, const unsigned char *pix, int width,
	int stride, int height, int pixel_format, int quality, int xdensity, int ydensity,
	unsigned char **out, size_t *size)
{
	return tj3Set(tj, TJPARAM_QUALITY, quality) ||
		tj3Set(tj, TJPARAM_SUBSAMP, TJSAMP_420) ||
		tj3Set(tj, TJPARAM_FASTDCT, 0) ||
		tj3Set(tj, TJPARAM_OPTIMIZE, 0) ||
		tj3Set(tj, TJPARAM_DENSITYUNITS, 1) ||
		tj3Set(tj, TJPARAM_XDENSITY, xdensity) ||
		tj3Set(tj, TJPARAM_YDENSITY, ydensity) ||
		tj3Compress8(tj, pix, width, stride, height, pixel_format, out, size);
}

#endif
