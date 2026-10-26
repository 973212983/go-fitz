// Package turbojpeg encodes packed RGB pixels without linking MuPDF.
// Bundled SIMD libraries are used on Windows/amd64 and Linux/amd64 with cgo;
// other builds use image/jpeg.
package turbojpeg

import (
	"errors"
	"fmt"
	"math"
)

// Options controls baseline, 4:2:0 JPEG output.
type Options struct {
	Quality int     // 1 through 100.
	DPI     float64 // Resolution written as the JFIF density; must round to 1 through 65535.
}

// EncodeRGB encodes opaque, top-down, interleaved RGB pixels. Stride is in
// bytes and may include row padding. The caller retains ownership of pix.
func EncodeRGB(pix []byte, width, height, stride int, opt Options) ([]byte, error) {
	if width <= 0 || height <= 0 || width > 65500 || height > 65500 {
		return nil, fmt.Errorf("turbojpeg: invalid image dimensions %dx%d", width, height)
	}
	if stride < width*3 || int64(stride) > math.MaxInt32 {
		return nil, errors.New("turbojpeg: invalid RGB stride")
	}
	needed := int64(height-1)*int64(stride) + int64(width)*3
	if needed > int64(len(pix)) {
		return nil, errors.New("turbojpeg: RGB buffer is too short")
	}
	if opt.Quality < 1 || opt.Quality > 100 {
		return nil, errors.New("turbojpeg: quality must be between 1 and 100")
	}
	density := math.Round(opt.DPI) // NaN fails both comparisons below
	if !(density >= 1 && density <= 65535) {
		return nil, errors.New("turbojpeg: DPI must round to an integer between 1 and 65535")
	}
	return encodeRGB(pix, width, height, stride, opt.Quality, int(density))
}
