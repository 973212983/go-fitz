package turbojpeg

import (
	"bytes"
	"encoding/binary"
	"image"
	"image/jpeg"
	"math"
	"runtime"
	"testing"
)

// TestAvailable checks that cgo builds on the platforms with bundled libraries
// really link libjpeg-turbo instead of silently using the fallback.
func TestAvailable(t *testing.T) {
	want := cgoBuild && runtime.GOARCH == "amd64" && (runtime.GOOS == "windows" || runtime.GOOS == "linux")
	if Available() != want {
		t.Fatalf("Available() = %v, want %v on %s/%s (cgo %v)", Available(), want, runtime.GOOS, runtime.GOARCH, cgoBuild)
	}
}

func TestEncodeRGBDensityRounding(t *testing.T) {
	for _, tc := range []struct {
		dpi  float64
		want uint16
	}{{0.5, 1}, {149.6, 150}, {65535.3, 65535}} {
		data, err := EncodeRGB(rgbFixture(2, 2, 6), 2, 2, 6, Options{90, tc.dpi})
		if err != nil {
			t.Fatalf("DPI %v: %v", tc.dpi, err)
		}
		if got := binary.BigEndian.Uint16(data[14:16]); got != tc.want {
			t.Fatalf("DPI %v: density %d, want %d", tc.dpi, got, tc.want)
		}
	}
}

func rgbFixture(w, h, stride int) []byte {
	pix := make([]byte, (h-1)*stride+w*3)
	for y := range h {
		for x := range w {
			i := y*stride + x*3
			pix[i], pix[i+1], pix[i+2] = byte(x*13), byte(y*17), byte((x+y)*11)
		}
	}
	return pix
}

func TestEncodeRGB(t *testing.T) {
	for _, size := range [][2]int{{1, 1}, {7, 9}, {16, 16}, {31, 23}} {
		w, h := size[0], size[1]
		pix := rgbFixture(w, h, w*3)
		for _, quality := range []int{1, 90, 100} {
			data, err := EncodeRGB(pix, w, h, w*3, Options{Quality: quality, DPI: 150})
			if err != nil {
				t.Fatal(err)
			}
			img, err := jpeg.Decode(bytes.NewReader(data))
			if err != nil {
				t.Fatal(err)
			}
			if img.Bounds() != image.Rect(0, 0, w, h) {
				t.Fatalf("bounds = %v", img.Bounds())
			}
			ycbcr, ok := img.(*image.YCbCr)
			if !ok || ycbcr.SubsampleRatio != image.YCbCrSubsampleRatio420 {
				t.Fatalf("expected 4:2:0 JPEG, got %T", img)
			}
			if len(data) < 20 || !bytes.Equal(data[2:11], []byte{0xff, 0xe0, 0, 16, 'J', 'F', 'I', 'F', 0}) || data[13] != 1 || binary.BigEndian.Uint16(data[14:16]) != 150 || binary.BigEndian.Uint16(data[16:18]) != 150 {
				t.Fatal("missing JFIF DPI metadata")
			}
		}
		padded := rgbFixture(w, h, w*3+7)
		a, err := EncodeRGB(pix, w, h, w*3, Options{90, 72})
		if err != nil {
			t.Fatal(err)
		}
		b, err := EncodeRGB(padded, w, h, w*3+7, Options{90, 72})
		if err != nil {
			t.Fatal(err)
		}
		if !bytes.Equal(a, b) {
			t.Fatal("row padding changed output")
		}
	}
}

func TestEncodeRGBRejectsInvalidInput(t *testing.T) {
	for _, tc := range []struct {
		w, h, stride int
		pix          []byte
		opt          Options
	}{
		{0, 1, 3, make([]byte, 3), Options{90, 150}},
		{65501, 1, 65501 * 3, nil, Options{90, 150}},
		{1, 1, 2, make([]byte, 3), Options{90, 150}},
		{2, 2, 6, make([]byte, 11), Options{90, 150}},
		{1, 1, 3, nil, Options{90, 150}},
		{1, 1, 3, make([]byte, 3), Options{0, 150}},
		{1, 1, 3, make([]byte, 3), Options{101, 150}},
		{1, 1, 3, make([]byte, 3), Options{90, 0}},
		{1, 1, 3, make([]byte, 3), Options{90, math.NaN()}},
		{1, 1, 3, make([]byte, 3), Options{90, math.Inf(1)}},
		{1, 1, 3, make([]byte, 3), Options{90, 65536}},
		{1, 1, 3, make([]byte, 3), Options{90, 0.4}},
		{1, 1, 3, make([]byte, 3), Options{90, -1}},
	} {
		if _, err := EncodeRGB(tc.pix, tc.w, tc.h, tc.stride, tc.opt); err == nil {
			t.Fatalf("accepted invalid input: %+v", tc)
		}
	}
}

func TestEncodeRGBConcurrent(t *testing.T) {
	pix := rgbFixture(31, 23, 31*3)
	want, err := EncodeRGB(pix, 31, 23, 31*3, Options{90, 150})
	if err != nil {
		t.Fatal(err)
	}
	done := make(chan bool, 8)
	for range 8 {
		go func() {
			got, err := EncodeRGB(pix, 31, 23, 31*3, Options{90, 150})
			done <- err == nil && bytes.Equal(got, want)
		}()
	}
	for range 8 {
		if !<-done {
			t.Fatal("concurrent encoder result differs")
		}
	}
}
