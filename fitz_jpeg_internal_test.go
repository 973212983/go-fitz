//go:build cgo && !nocgo

package fitz

import (
	"bytes"
	"compress/zlib"
	"fmt"
	"math/rand"
	"strings"
	"sync"
	"testing"
)

// samplePixels fills width x height pixels (n = 3 or 4) with blank paper,
// noise, gradients, gray strokes or colored blocks.
func samplePixels(width, height, n, kind int, rng *rand.Rand) []byte {
	out := make([]byte, width*height*n)
	palette := [][3]byte{{255, 255, 255}, {0, 0, 0}, {200, 30, 30}, {128, 128, 128}, {240, 248, 255}}
	for y := range height {
		for x := range width {
			var c [3]byte
			switch kind {
			case 0:
				c = palette[0]
			case 1:
				c = [3]byte{byte(rng.Intn(256)), byte(rng.Intn(256)), byte(rng.Intn(256))}
			case 2:
				v := byte(x*7 + y*3)
				c = [3]byte{v, v, v}
			case 3:
				if rng.Intn(23) == 0 {
					v := byte(rng.Intn(128))
					c = [3]byte{v, v, v}
				} else {
					c = palette[0]
				}
			case 4:
				c = palette[(x/16*31+y/16*17)%len(palette)]
				if rng.Intn(50) == 0 {
					c[0] = byte(rng.Intn(256))
				}
			default:
				c = [3]byte{byte(x * 255 / max(width-1, 1)), byte(y * 255 / max(height-1, 1)), byte(255 - x*255/max(width-1, 1))}
			}
			p := out[(y*width+x)*n:]
			copy(p, c[:])
			if n == 4 {
				p[3] = 255
			}
		}
	}
	return out
}

func TestEncodeJPEGMatchesLibjpeg(t *testing.T) {
	rng := rand.New(rand.NewSource(1))
	sizes := [][2]int{{1, 1}, {1, 17}, {17, 1}, {7, 7}, {8, 8}, {9, 9}, {15, 15}, {16, 16}, {17, 17}, {23, 31}, {33, 33}, {47, 49}, {127, 129}, {200, 3}, {255, 257}}
	for _, size := range sizes {
		for _, n := range []int{3, 4} {
			for kind := range 6 {
				samples := samplePixels(size[0], size[1], n, kind, rng)
				for _, quality := range []int{1, 10, 50, 75, 90, 100} {
					fast, err := encodeSamples(samples, size[0], size[1], n, quality, false)
					if err != nil {
						t.Fatal(err)
					}
					ref, err := encodeSamples(samples, size[0], size[1], n, quality, true)
					if err != nil {
						t.Fatal(err)
					}
					if !bytes.Equal(fast, ref) {
						t.Fatalf("%dx%d n=%d kind=%d quality=%d: %d bytes, libjpeg %d bytes", size[0], size[1], n, kind, quality, len(fast), len(ref))
					}
				}
			}
		}
	}
	// A full page with text-like content.
	samples := samplePixels(1240, 1755, 3, 3, rng)
	fast, _ := encodeSamples(samples, 1240, 1755, 3, 90, false)
	ref, _ := encodeSamples(samples, 1240, 1755, 3, 90, true)
	if len(fast) == 0 || !bytes.Equal(fast, ref) {
		t.Fatalf("page: %d bytes, libjpeg %d bytes", len(fast), len(ref))
	}
}

// pdfStream returns a PDF stream object, Flate compressed.
func pdfStream(dict string, data []byte) string {
	var b bytes.Buffer
	w := zlib.NewWriter(&b)
	w.Write(data)
	w.Close()
	return fmt.Sprintf("<< %s /Filter /FlateDecode /Length %d >>\nstream\n%s\nendstream", dict, b.Len(), b.String())
}

// imageRows prefixes every row with PNG filter type 0, as /Predictor 15 expects.
func imageRows(width, height, n int, pixel func(x, y int) []byte) []byte {
	var b bytes.Buffer
	for y := range height {
		b.WriteByte(0)
		for x := range width {
			b.Write(pixel(x, y)[:n])
		}
	}
	return b.Bytes()
}

// imagePDF builds a one page PDF with a soft masked Flate image like the invoice
// seal, an opaque image, text-free strokes and, with blend, a Multiply fill.
func imagePDF(variant int, blend bool) []byte {
	const w, h = 160, 100
	rgb := imageRows(w, h, 3, func(x, y int) []byte {
		return []byte{byte(200 + variant), byte(x), byte(y * 2)}
	})
	alpha := imageRows(w, h, 1, func(x, y int) []byte {
		dx, dy := x-w/2, y-h/2
		if dx*dx*4+dy*dy*9 < 70*70 {
			return []byte{255}
		}
		return []byte{byte((x * y) % 256)}
	})
	parms := func(colors int) string {
		return fmt.Sprintf("/DecodeParms << /Predictor 15 /Colors %d /BitsPerComponent 8 /Columns %d >>", colors, w)
	}
	content := "0.2 0.2 0.2 RG 1 w 20 20 m 380 260 l S\n" +
		"q 120 0 0 75 30 150 cm /Seal Do Q\n" +
		"q 100 0 0 60 200 40 cm /Title Do Q\n"
	extg := ""
	if blend {
		content += "q /Mul gs 1 0 0 rg 60 60 200 80 re f Q\n"
		extg = "/ExtGState << /Mul << /Type /ExtGState /BM /Multiply >> >>"
	}
	objects := []string{
		"<< /Type /Catalog /Pages 2 0 R >>",
		"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
		fmt.Sprintf("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 400 300] /Resources << /XObject << /Seal 5 0 R /Title 7 0 R >> %s >> /Contents 4 0 R >>", extg),
		fmt.Sprintf("<< /Length %d >>\nstream\n%sendstream", len(content), content),
		pdfStream(fmt.Sprintf("/Type /XObject /Subtype /Image /Width %d /Height %d /ColorSpace /DeviceRGB /BitsPerComponent 8 %s /SMask 6 0 R", w, h, parms(3)), rgb),
		pdfStream(fmt.Sprintf("/Type /XObject /Subtype /Image /Width %d /Height %d /ColorSpace /DeviceGray /BitsPerComponent 8 %s", w, h, parms(1)), alpha),
		pdfStream(fmt.Sprintf("/Type /XObject /Subtype /Image /Width %d /Height %d /ColorSpace /DeviceGray /BitsPerComponent 8 %s", w, h, parms(1)), imageRows(w, h, 1, func(x, y int) []byte { return []byte{byte(x ^ y)} })),
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

func renderRGBOnce(t *testing.T, pdf []byte, flags int) ([]byte, int, int) {
	t.Helper()
	doc, err := NewFromMemory(pdf)
	if err != nil {
		t.Fatal(err)
	}
	defer doc.Close()
	pix, w, h, err := doc.renderRGB(0, 150, flags)
	if err != nil {
		t.Fatal(err)
	}
	return pix, w, h
}

func maxDiff(a, b []byte) (int, int) {
	most, count := 0, 0
	for i := range a {
		d := int(a[i]) - int(b[i])
		if d < 0 {
			d = -d
		}
		if d > 0 {
			count++
		}
		most = max(most, d)
	}
	return most, count
}

func TestImageCacheKeepsPixels(t *testing.T) {
	clearImageCache()
	defer clearImageCache()
	pdf := imagePDF(0, false)
	plain, _, _ := renderRGBOnce(t, pdf, renderPlain)
	// Fresh documents each time: the first decode is only remembered as seen,
	// the second is stored and later ones copy the cached tiles.
	for i := range 4 {
		cached, _, _ := renderRGBOnce(t, pdf, renderKeepGroup)
		if most, count := maxDiff(plain, cached); count != 0 {
			t.Fatalf("render %d: %d pixels differ, max %d", i, count, most)
		}
	}
	hits, _, entries, size := imageCacheStats()
	if hits == 0 || entries != 3 || size == 0 {
		t.Fatalf("hits=%d entries=%d bytes=%d", hits, entries, size)
	}
	// Different image data must not reuse those tiles.
	other := imagePDF(9, false)
	want, _, _ := renderRGBOnce(t, other, renderPlain)
	for range 3 {
		got, _, _ := renderRGBOnce(t, other, renderKeepGroup)
		if most, count := maxDiff(want, got); count != 0 {
			t.Fatalf("other image: %d pixels differ, max %d", count, most)
		}
	}
}

func TestPageGroupSkip(t *testing.T) {
	pdf := imagePDF(0, false)
	plain, _, _ := renderRGBOnce(t, pdf, renderPlain)
	fast, _, _ := renderRGBOnce(t, pdf, renderNoImageCache)
	// Compositing the isolated page group over white only changes rounding.
	if most, count := maxDiff(plain, fast); most > 2 || count == 0 {
		t.Fatalf("max difference %d over %d samples", most, count)
	}
	// Other transparency (here a Multiply fill) keeps MuPDF's page group.
	blend := imagePDF(0, true)
	plain, _, _ = renderRGBOnce(t, blend, renderPlain)
	for _, flags := range []int{0, renderNoImageCache} {
		got, _, _ := renderRGBOnce(t, blend, flags)
		if most, count := maxDiff(plain, got); count != 0 {
			t.Fatalf("flags=%d: %d samples differ, max %d", flags, count, most)
		}
	}
}

func TestRenderedJPEGMatchesLibjpeg(t *testing.T) {
	for _, pdf := range [][]byte{imagePDF(0, false), imagePDF(3, true), jpegTestPDFInternal()} {
		doc, err := NewFromMemory(pdf)
		if err != nil {
			t.Fatal(err)
		}
		for _, flags := range []int{0, renderPlain} {
			fast, err := doc.imageJPEG(0, 150, 90, flags, false)
			if err != nil {
				t.Fatal(err)
			}
			ref, err := doc.imageJPEG(0, 150, 90, flags, true)
			if err != nil {
				t.Fatal(err)
			}
			if !bytes.Equal(fast, ref) {
				t.Fatalf("flags=%d: %d bytes, libjpeg %d bytes", flags, len(fast), len(ref))
			}
		}
		doc.Close()
	}
}

func jpegTestPDFInternal() []byte {
	content := "1 0 0 rg 25 25 30 30 re f\n"
	objects := []string{
		"<< /Type /Catalog /Pages 2 0 R >>",
		"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
		"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 100 100] /Resources << >> /Contents 4 0 R >>",
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

func TestImageCacheConcurrentDocuments(t *testing.T) {
	clearImageCache()
	defer clearImageCache()
	pdfs := [][]byte{imagePDF(0, false), imagePDF(1, false), imagePDF(2, true)}
	want := make([][]byte, len(pdfs))
	for i, pdf := range pdfs {
		doc, err := NewFromMemory(pdf)
		if err != nil {
			t.Fatal(err)
		}
		want[i], err = doc.imageJPEG(0, 150, 90, renderNoImageCache, false)
		doc.Close()
		if err != nil {
			t.Fatal(err)
		}
	}
	var wg sync.WaitGroup
	errs := make(chan error, 8)
	for worker := range 8 {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for i := range 30 {
				k := (worker + i) % len(pdfs)
				doc, err := NewFromMemory(pdfs[k])
				if err != nil {
					errs <- err
					return
				}
				got, err := doc.ImageJPEG(0, 150, 90)
				doc.Close()
				if err != nil {
					errs <- err
					return
				}
				if !bytes.Equal(got, want[k]) {
					errs <- fmt.Errorf("worker %d document %d: output differs", worker, k)
					return
				}
			}
		}()
	}
	wg.Wait()
	close(errs)
	for err := range errs {
		t.Error(err)
	}
	if hits, _, _, _ := imageCacheStats(); hits == 0 {
		t.Fatal("no cache hits")
	}
}

// glyphFont returns a TrueType font whose glyph 1 is a square and glyph 2 a
// triangle, with only the tables FreeType needs for CID-keyed use.
func glyphFont() []byte {
	be := func(vs ...int) []byte {
		var out []byte
		for _, v := range vs {
			out = append(out, byte(v>>8), byte(v))
		}
		return out
	}
	glyph := func(xs, ys []int) []byte {
		n := len(xs)
		g := be(1, 0, 0, 1000, 1000, n-1, 0) // one contour, bbox, end point, no instructions
		for range n {
			g = append(g, 0x01) // on curve, 16-bit coordinates
		}
		px, py := 0, 0
		for i := range n {
			g = append(g, be(xs[i]-px)...)
			px = xs[i]
		}
		for i := range n {
			g = append(g, be(ys[i]-py)...)
			py = ys[i]
		}
		return g
	}
	glyf := append(glyph([]int{100, 100, 900, 900}, []int{100, 900, 900, 100}), glyph([]int{100, 500, 900}, []int{100, 900, 100})...)
	if len(glyf)%2 != 0 {
		glyf = append(glyf, 0)
	}
	first := len(glyph([]int{100, 100, 900, 900}, []int{100, 900, 900, 100}))
	tables := []struct {
		tag  string
		data []byte
	}{
		{"glyf", glyf},
		// unitsPerEm 1000, short loca
		{"head", append(be(1, 0, 0, 0, 0, 0, 0x5F0F, 0x3CF5, 0, 1000, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1000, 1000, 0, 8, 2), be(0, 0)...)},
		{"hhea", be(1, 0, 1000, 0, 0, 1000, 0, 0, 1000, 1, 0, 0, 0, 0, 0, 0, 0, 3)},
		{"hmtx", be(1000, 0, 1000, 0, 1000, 0)},
		{"loca", be(0, 0, first/2, len(glyf)/2)},
		{"maxp", be(1, 0, 3, 4, 1, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0)},
	}
	var font bytes.Buffer
	font.Write(be(1, 0, len(tables), 64, 2, len(tables)*16-64))
	offset := 12 + 16*len(tables)
	for _, table := range tables {
		font.WriteString(table.tag)
		font.Write(be(0, 0, offset>>16, offset, len(table.data)>>16, len(table.data)))
		offset += (len(table.data) + 3) &^ 3
	}
	for _, table := range tables {
		font.Write(table.data)
		font.Write(make([]byte, (4-len(table.data)%4)%4))
	}
	return font.Bytes()
}

// cidFontPDF draws glyphs 1 and 2 with an embedded Identity-H CID font whose
// ToUnicode maps them to "AB".
func cidFontPDF() []byte {
	font := glyphFont()
	cmap := "/CIDInit /ProcSet findresource begin 12 dict begin begincmap\n" +
		"/CIDSystemInfo << /Registry (Adobe) /Ordering (UCS) /Supplement 0 >> def\n" +
		"/CMapName /Adobe-Identity-UCS def /CMapType 2 def\n" +
		"1 begincodespacerange <0000> <FFFF> endcodespacerange\n" +
		"2 beginbfchar <0001> <0041> <0002> <0042> endbfchar\n" +
		"endcmap CMapName currentdict /CMap defineresource pop end end\n"
	content := "0 0 1 rg BT /F1 40 Tf 10 20 Td <00010002> Tj ET\n"
	objects := []string{
		"<< /Type /Catalog /Pages 2 0 R >>",
		"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
		"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 100 80] /Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R >>",
		pdfStream("", []byte(content)),
		"<< /Type /Font /Subtype /Type0 /BaseFont /Glyphs /Encoding /Identity-H /DescendantFonts [6 0 R] /ToUnicode 9 0 R >>",
		"<< /Type /Font /Subtype /CIDFontType2 /BaseFont /Glyphs /CIDSystemInfo << /Registry (Adobe) /Ordering (Identity) /Supplement 0 >> /FontDescriptor 7 0 R /W [1 [1000 1000]] >>",
		"<< /Type /FontDescriptor /FontName /Glyphs /Flags 32 /FontBBox [0 0 1000 1000] /ItalicAngle 0 /Ascent 1000 /Descent 0 /CapHeight 1000 /StemV 80 /FontFile2 8 0 R >>",
		pdfStream(fmt.Sprintf("/Length1 %d", len(font)), font),
		pdfStream("", []byte(cmap)),
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

func TestDropToUnicodeKeepsPixels(t *testing.T) {
	pdf := cidFontPDF()
	render := func(drop bool) ([]byte, string) {
		doc, err := NewFromMemory(pdf)
		if err != nil {
			t.Fatal(err)
		}
		defer doc.Close()
		if drop {
			if err := doc.DropToUnicode(); err != nil {
				t.Fatal(err)
			}
		}
		pix, _, _, err := doc.renderRGB(0, 150, 0)
		if err != nil {
			t.Fatal(err)
		}
		text, err := doc.Text(0)
		if err != nil {
			t.Fatal(err)
		}
		return pix, text
	}
	want, text := render(false)
	got, dropped := render(true)
	if most, count := maxDiff(want, got); count != 0 {
		t.Fatalf("%d samples differ, max %d", count, most)
	}
	ink := 0
	for i := 0; i+2 < len(want); i += 3 {
		if want[i] < 128 && want[i+2] > 128 {
			ink++
		}
	}
	if ink < 1000 {
		t.Fatalf("glyphs not drawn: %d blue pixels", ink)
	}
	if !strings.Contains(text, "AB") || strings.Contains(dropped, "AB") {
		t.Fatalf("text %q, after DropToUnicode %q", text, dropped)
	}
}

func TestPagePoolReuse(t *testing.T) {
	clearPagePool()
	defer clearPagePool()
	pdfs := [][]byte{imagePDF(0, false), jpegTestPDFInternal(), cidFontPDF()}
	want := make([][]byte, len(pdfs))
	for i, pdf := range pdfs {
		want[i], _, _ = renderRGBOnce(t, pdf, 0)
	}
	if n := pagePoolCount(); n != 1 {
		t.Fatalf("idle buffers after sequential renders: %d", n)
	}
	// Buffers left by larger pages serve smaller ones and the other way round.
	for round := range 3 {
		for i := len(pdfs) - 1; i >= 0; i-- {
			got, _, _ := renderRGBOnce(t, pdfs[i], 0)
			if !bytes.Equal(got, want[i]) {
				t.Fatalf("round %d document %d: pixels differ", round, i)
			}
		}
	}
	var wg sync.WaitGroup
	for worker := range 12 {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for i := range 20 {
				k := (worker + i) % len(pdfs)
				doc, err := NewFromMemory(pdfs[k])
				if err != nil {
					t.Error(err)
					return
				}
				got, _, _, err := doc.renderRGB(0, 150, 0)
				doc.Close()
				if err != nil || !bytes.Equal(got, want[k]) {
					t.Errorf("worker %d document %d: err=%v", worker, k, err)
					return
				}
			}
		}()
	}
	wg.Wait()
	if n := pagePoolCount(); n < 1 || n > 4 {
		t.Fatalf("idle buffers after concurrent renders: %d", n)
	}
}
