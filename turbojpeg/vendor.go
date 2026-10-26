//go:build required

package turbojpeg

// Keeps the cgo headers and static libraries in `go mod vendor` output.
import (
	_ "github.com/gen2brain/go-fitz/include/turbojpeg"
	_ "github.com/gen2brain/go-fitz/libs"
)
