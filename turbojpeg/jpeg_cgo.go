//go:build cgo && !nocgo && amd64 && (windows || (linux && !android))

package turbojpeg

/*
#cgo CFLAGS: -I${SRCDIR}/../include
#cgo windows LDFLAGS: -L${SRCDIR}/../libs -lturbojpeg_windows_amd64
#cgo linux,!musl LDFLAGS: -L${SRCDIR}/../libs -lturbojpeg_linux_amd64
#cgo linux,musl LDFLAGS: -L${SRCDIR}/../libs -lturbojpeg_linux_amd64_musl

#include <stdio.h>
#include "turbojpeg/fitz_turbojpeg.h"

static tjhandle new_encoder(char *message, size_t capacity)
{
    tjhandle tj = tj3Init(TJINIT_COMPRESS);
    if (!tj)
        snprintf(message, capacity, "%s", tj3GetErrorStr(NULL));
    return tj;
}

static int encode_rgb(tjhandle tj, const unsigned char *pix, int width, int height, int stride,
    int quality, int density, unsigned char **out, size_t *size, char *message, size_t capacity)
{
    if (fitz_tj_compress(tj, pix, width, stride, height, TJPF_RGB, quality, density, density, out, size)) {
        snprintf(message, capacity, "%s", tj3GetErrorStr(tj));
        return 0;
    }
    return 1;
}
*/
import "C"

import (
	"errors"
	"fmt"
	"math"
	"unsafe"
)

// Available reports whether this build uses the bundled libjpeg-turbo encoder.
func Available() bool { return true }

// encoder is a TurboJPEG compressor and the output buffer from its last call.
// Passing the buffer back lets libjpeg-turbo reuse it, growing it only when a
// larger image needs more room.
type encoder struct {
	tj  C.tjhandle
	out *C.uchar
}

// idle holds encoders between calls so pages do not each pay for compressor
// setup; encoders beyond its capacity are released.
var idle = make(chan *encoder, 16)

func getEncoder() (*encoder, error) {
	select {
	case e := <-idle:
		return e, nil
	default:
	}
	var message [256]C.char
	tj := C.new_encoder(&message[0], C.size_t(len(message)))
	if tj == nil {
		return nil, fmt.Errorf("turbojpeg: %s", C.GoString(&message[0]))
	}
	return &encoder{tj: tj}, nil
}

func (e *encoder) release() {
	select {
	case idle <- e:
	default:
		e.destroy()
	}
}

func (e *encoder) destroy() {
	C.tj3Free(unsafe.Pointer(e.out))
	C.tj3Destroy(e.tj)
}

func encodeRGB(pix []byte, width, height, stride, quality, density int) ([]byte, error) {
	e, err := getEncoder()
	if err != nil {
		return nil, err
	}
	out := e.out
	var size C.size_t
	var message [256]C.char
	ok := C.encode_rgb(e.tj, (*C.uchar)(unsafe.Pointer(&pix[0])), C.int(width), C.int(height),
		C.int(stride), C.int(quality), C.int(density), &out, &size, &message[0], C.size_t(len(message)))
	e.out = out
	if ok == 0 {
		// The compressor state after a failure is not worth trusting.
		e.destroy()
		return nil, fmt.Errorf("turbojpeg: %s", C.GoString(&message[0]))
	}
	defer e.release()
	if size == 0 || uint64(size) > math.MaxInt32 {
		return nil, errors.New("turbojpeg: invalid encoded output size")
	}
	return C.GoBytes(unsafe.Pointer(out), C.int(size)), nil
}
