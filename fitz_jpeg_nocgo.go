//go:build !cgo || nocgo

package fitz

import (
	"bytes"
	"errors"
	"image/jpeg"
	"math"
)

// ImageJPEG retains API compatibility for nocgo users; CGO uses native JPEG encoding.
func (f *Document) ImageJPEG(pageNumber int, dpi float64, quality int) ([]byte, error) {
	if pageNumber < 0 || pageNumber >= f.NumPage() {
		return nil, ErrPageMissing
	}
	if dpi <= 0 || math.IsNaN(dpi) || math.IsInf(dpi, 0) || dpi > math.MaxInt32 {
		return nil, errors.New("fitz: JPEG DPI must be finite and positive")
	}
	if quality < 1 || quality > 100 {
		return nil, errors.New("fitz: JPEG quality must be between 1 and 100")
	}
	img, err := f.ImageDPI(pageNumber, dpi)
	if err != nil {
		return nil, err
	}
	var buf bytes.Buffer
	if err := jpeg.Encode(&buf, img, &jpeg.Options{Quality: quality}); err != nil {
		return nil, err
	}
	return buf.Bytes(), nil
}

// DropToUnicode only matters for the native CGO renderer.
func (f *Document) DropToUnicode() error {
	return nil
}
