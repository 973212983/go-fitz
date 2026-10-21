//go:build cgo && !nocgo

package fitz

import (
	"bytes"
	"encoding/binary"
	"image"
	"image/jpeg"
	"math"
	"math/rand"
	"testing"
)

// jpegLayout reports the JFIF density and the SOF marker of a JPEG stream.
type jpegLayout struct {
	units, xdensity, ydensity int
	sof                       byte  // 0xC0 for baseline
	sampling                  []int // per component, h<<4 | v
}

func parseJPEGLayout(t *testing.T, data []byte) jpegLayout {
	t.Helper()
	var l jpegLayout
	for i := 2; i+4 <= len(data) && data[i] == 0xFF; {
		marker, size := data[i+1], int(binary.BigEndian.Uint16(data[i+2:]))
		seg := data[i+4 : i+2+size]
		switch {
		case marker == 0xE0 && bytes.HasPrefix(seg, []byte("JFIF\x00")):
			l.units = int(seg[7])
			l.xdensity = int(binary.BigEndian.Uint16(seg[8:]))
			l.ydensity = int(binary.BigEndian.Uint16(seg[10:]))
		case marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC:
			l.sof = marker
			for c := range int(seg[5]) {
				l.sampling = append(l.sampling, int(seg[7+3*c]))
			}
		case marker == 0xDA:
			return l
		}
		i += 2 + size
	}
	t.Fatal("no scan in JPEG")
	return l
}

// jpegPSNR decodes data and compares it with width x height samples (n = 3 or 4).
func jpegPSNR(t *testing.T, data, samples []byte, width, height, n int) float64 {
	t.Helper()
	img, err := jpeg.Decode(bytes.NewReader(data))
	if err != nil {
		t.Fatal(err)
	}
	if img.Bounds() != image.Rect(0, 0, width, height) {
		t.Fatalf("decoded %v, want %dx%d", img.Bounds(), width, height)
	}
	var se float64
	for y := range height {
		for x := range width {
			r, g, b, _ := img.At(x, y).RGBA()
			p := samples[(y*width+x)*n:]
			for i, v := range [3]uint32{r >> 8, g >> 8, b >> 8} {
				d := float64(v) - float64(p[i])
				se += d * d
			}
		}
	}
	if se == 0 {
		return math.Inf(1)
	}
	return 10 * math.Log10(255*255*float64(width*height*3)/se)
}

// libjpeg-turbo writes the same kind of stream as libjpeg (baseline, 4:2:0,
// JFIF with the resolution) but downsamples chroma by averaging, so only the
// decoded quality is compared.  Below 16x16 a few samples decide the chroma
// and the two downsamplers differ too much to compare.
func TestTurboJPEG(t *testing.T) {
	if !hasTurboJPEG {
		t.Skip("libjpeg-turbo is not built for this platform")
	}
	rng := rand.New(rand.NewSource(2))
	type sample struct{ w, h, n, kind, quality int }
	var cases []sample
	for _, size := range [][2]int{{1, 1}, {1, 17}, {17, 1}, {9, 9}, {16, 16}, {23, 31}, {127, 129}, {255, 257}} {
		for _, n := range []int{3, 4} {
			for kind := range 6 {
				for _, quality := range []int{10, 75, 90, 100} {
					cases = append(cases, sample{size[0], size[1], n, kind, quality})
				}
			}
		}
	}
	// A full page with text-like content.
	cases = append(cases, sample{1240, 1755, 3, 3, 90})
	for _, c := range cases {
		samples := samplePixels(c.w, c.h, c.n, c.kind, rng)
		turbo, err := encodeSamples(samples, c.w, c.h, c.n, c.quality, encoderTurbo)
		if err != nil {
			t.Fatal(err)
		}
		ref, err := encodeSamples(samples, c.w, c.h, c.n, c.quality, encoderLibjpeg)
		if err != nil {
			t.Fatal(err)
		}
		l := parseJPEGLayout(t, turbo)
		if l.units != 1 || l.xdensity != 150 || l.ydensity != 150 || l.sof != 0xC0 ||
			len(l.sampling) != 3 || l.sampling[0] != 0x22 || l.sampling[1] != 0x11 || l.sampling[2] != 0x11 {
			t.Fatalf("%+v: layout %+v", c, l)
		}
		got, want := jpegPSNR(t, turbo, samples, c.w, c.h, c.n), jpegPSNR(t, ref, samples, c.w, c.h, c.n)
		if c.w >= 16 && c.h >= 16 && got < want-0.5 {
			t.Fatalf("%+v: PSNR %.2f dB, libjpeg %.2f dB", c, got, want)
		}
	}
}

func TestImageJPEGDefaultEncoder(t *testing.T) {
	want := encoderFast
	if hasTurboJPEG {
		want = encoderTurbo
	}
	doc, err := NewFromMemory(imagePDF(0, false))
	if err != nil {
		t.Fatal(err)
	}
	defer doc.Close()
	got, err := doc.ImageJPEG(0, 150, 90)
	if err != nil {
		t.Fatal(err)
	}
	ref, err := doc.imageJPEG(0, 150, 90, 0, want)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(got, ref) {
		t.Fatalf("ImageJPEG: %d bytes, encoder %d: %d bytes", len(got), want, len(ref))
	}
}
