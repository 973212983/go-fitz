package fitz_test

import (
	"bytes"
	"errors"
	"fmt"
	"image/jpeg"
	"math"
	"sync"
	"testing"

	"github.com/gen2brain/go-fitz"
)

// A small PDF with a red square and a shifted page origin exercises RGB and bbox handling.
func jpegTestPDF(origin int) []byte {
	content := "1 0 0 rg 25 25 30 30 re f\n"
	objects := []string{
		"<< /Type /Catalog /Pages 2 0 R >>",
		"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
		fmt.Sprintf("<< /Type /Page /Parent 2 0 R /MediaBox [%d %d %d %d] /Resources << >> /Contents 4 0 R >>", origin, origin, origin+100, origin+100),
		fmt.Sprintf("<< /Length %d >>\nstream\n%sendstream", len(content), content),
	}
	var b bytes.Buffer
	b.WriteString("%PDF-1.4\n")
	offsets := []int{0}
	for i, obj := range objects {
		offsets = append(offsets, b.Len())
		fmt.Fprintf(&b, "%d 0 obj\n%s\nendobj\n", i+1, obj)
	}
	xref := b.Len()
	fmt.Fprintf(&b, "xref\n0 %d\n0000000000 65535 f \n", len(offsets))
	for _, offset := range offsets[1:] {
		fmt.Fprintf(&b, "%010d 00000 n \n", offset)
	}
	fmt.Fprintf(&b, "trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n", len(offsets), xref)
	return b.Bytes()
}

func TestImageJPEGSizeAndColors(t *testing.T) {
	for _, origin := range []int{0, 10, -10} {
		doc, err := fitz.NewFromMemory(jpegTestPDF(origin))
		if err != nil {
			t.Fatal(err)
		}
		for _, dpi := range []float64{72, 150} {
			data, err := doc.ImageJPEG(0, dpi, 90)
			if err != nil {
				t.Fatal(err)
			}
			if len(data) < 4 || bytes.Contains(data, []byte{0xff, 0xc2}) {
				t.Fatal("JPEG unexpectedly uses progressive encoding")
			}
			img, err := jpeg.Decode(bytes.NewReader(data))
			if err != nil {
				t.Fatal(err)
			}
			want := int(math.Ceil(100 * dpi / 72))
			if img.Bounds().Dx() != want || img.Bounds().Dy() != want {
				t.Fatalf("origin=%d dpi=%v size=%v", origin, dpi, img.Bounds())
			}
			red, green, blue, _ := img.At(0, 0).RGBA()
			if red < 64000 || green < 64000 || blue < 64000 {
				t.Fatalf("background is not white: %d,%d,%d", red, green, blue)
			}
		}
		data, err := doc.ImageJPEG(0, 72, 90)
		if err != nil {
			t.Fatal(err)
		}
		img, err := jpeg.Decode(bytes.NewReader(data))
		if err != nil {
			t.Fatal(err)
		}
		red, green, blue, _ := img.At(40-origin, 60+origin).RGBA()
		if red < 60000 || green > 5000 || blue > 5000 {
			t.Fatalf("square is not red: %d,%d,%d", red, green, blue)
		}
		doc.Close()
	}
}

func TestImageJPEGConcurrentDocuments(t *testing.T) {
	var wg sync.WaitGroup
	errors := make(chan error, 8)
	for worker := 0; worker < 8; worker++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			doc, err := fitz.NewFromMemory(jpegTestPDF(0))
			if err != nil {
				errors <- err
				return
			}
			defer doc.Close()
			for i := 0; i < 20; i++ {
				if _, err := doc.ImageJPEG(0, 150, 90); err != nil {
					errors <- err
					return
				}
			}
		}()
	}
	wg.Wait()
	close(errors)
	for err := range errors {
		t.Error(err)
	}
}

func TestImageJPEGInvalidParameters(t *testing.T) {
	doc, err := fitz.NewFromMemory(jpegTestPDF(0))
	if err != nil {
		t.Fatal(err)
	}
	defer doc.Close()
	for _, page := range []int{-1, 1} {
		if _, err := doc.ImageJPEG(page, 72, 90); !errors.Is(err, fitz.ErrPageMissing) {
			t.Fatalf("page %d: %v", page, err)
		}
	}
	for _, dpi := range []float64{0, -1, math.NaN(), math.Inf(1)} {
		if _, err := doc.ImageJPEG(0, dpi, 90); err == nil {
			t.Fatalf("accepted DPI %v", dpi)
		}
	}
	for _, quality := range []int{0, 101} {
		if _, err := doc.ImageJPEG(0, 72, quality); err == nil {
			t.Fatalf("accepted quality %d", quality)
		}
	}
	// Parameter errors leave the document usable.
	if _, err := doc.ImageJPEG(0, 72, 90); err != nil {
		t.Fatal(err)
	}
}
