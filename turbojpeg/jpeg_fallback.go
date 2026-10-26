//go:build !cgo || nocgo || !amd64 || (!windows && !linux) || android

package turbojpeg

import (
	"bytes"
	"image"
	"image/jpeg"
)

// Available reports whether this build uses the bundled libjpeg-turbo encoder.
func Available() bool { return false }

func encodeRGB(pix []byte, width, height, stride, quality, density int) ([]byte, error) {
	// image/jpeg converts *image.RGBA in bulk; any other image type goes through
	// At, which allocates per pixel.
	img := image.NewRGBA(image.Rect(0, 0, width, height))
	for y := 0; y < height; y++ {
		src := pix[y*stride : y*stride+width*3]
		dst := img.Pix[y*img.Stride : y*img.Stride+width*4]
		for s, d := 0, 0; s < len(src); s, d = s+3, d+4 {
			dst[d], dst[d+1], dst[d+2], dst[d+3] = src[s], src[s+1], src[s+2], 255
		}
	}
	var out bytes.Buffer
	if err := jpeg.Encode(&out, img, &jpeg.Options{Quality: quality}); err != nil {
		return nil, err
	}
	// image/jpeg omits APP0. Insert the same density metadata as the native path.
	data := out.Bytes()
	result := make([]byte, 0, len(data)+18)
	result = append(result, data[:2]...)
	result = append(result, 0xff, 0xe0, 0, 16, 'J', 'F', 'I', 'F', 0, 1, 1, 1,
		byte(density>>8), byte(density), byte(density>>8), byte(density), 0, 0)
	return append(result, data[2:]...), nil
}
