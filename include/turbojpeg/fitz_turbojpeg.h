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

#endif
