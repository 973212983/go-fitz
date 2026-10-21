#!/usr/bin/env bash
# Builds libs/libturbojpeg_{windows_amd64,linux_amd64,linux_amd64_musl}.a from the
# official libjpeg-turbo release tarball, with SIMD, static only.
#
# Every global symbol is renamed with the fitz_ljt_ prefix (include/turbojpeg/
# fitz_turbojpeg.h maps the API names), so the archives never clash with the
# libjpeg built into libmupdfthird_*.a.
#
# Host: Windows with Git Bash, MinGW-w64 gcc and binutils (its objcopy/nm/ar
# handle both PE and ELF), cmake, nasm, zig and curl on PATH.
# Set RUN_TESTS=1 to run the libjpeg-turbo regression tests on the Windows build.
set -euo pipefail

VERSION=3.2.0
SHA256=6f30092cef9fb839779646608f4ee14ae3cbac989c47fa05e841b0841f09878e
PREFIX=fitz_ljt_

root=$(cd "$(dirname "$0")/.." && pwd)
work=${WORK:-$(mktemp -d)}
mkdir -p "$work"
cd "$work"

tarball=libjpeg-turbo-$VERSION.tar.gz
if [ ! -f "$tarball" ]; then
	curl -fsSL -o "$tarball" "https://github.com/libjpeg-turbo/libjpeg-turbo/releases/download/$VERSION/$tarball"
fi
echo "$SHA256  $tarball" | sha256sum -c -
rm -rf "libjpeg-turbo-$VERSION"
tar xzf "$tarball"
src=$work/libjpeg-turbo-$VERSION

binutils=$(cygpath -m "$(dirname "$(command -v objcopy)")")
common=(-G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release -DENABLE_SHARED=OFF -DREQUIRE_SIMD=ON
	-DWITH_ARITH_DEC=OFF -DWITH_ARITH_ENC=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON)

build() { # name, extra cmake args...
	local name=$1
	shift
	rm -rf "build-$name"
	cmake -S "$src" -B "build-$name" "${common[@]}" "$@"
	cmake --build "build-$name" -j8 --target turbojpeg-static
}

cross() { # name, zig target
	cat > "tc-$1.cmake" <<EOF
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER zig)
set(CMAKE_C_COMPILER_ARG1 "cc -target $2")
set(CMAKE_C_FLAGS_INIT "-fno-sanitize=undefined")
set(CMAKE_AR "$binutils/ar.exe" CACHE FILEPATH "")
set(CMAKE_RANLIB "$binutils/ranlib.exe" CACHE FILEPATH "")
EOF
	build "$1" -DCMAKE_TOOLCHAIN_FILE="$work/tc-$1.cmake" -DWITH_TOOLS=OFF -DWITH_TESTS=OFF
}

# Prefixes every defined global symbol; .refptr.X stubs (MinGW) follow X.
prefix() { # build name, output archive
	nm -g --defined-only "build-$1/libturbojpeg.a" 2>/dev/null | awk 'NF == 3 { print $3 }' | sort -u |
		awk -v p=$PREFIX '{ if (index($0, ".refptr.") == 1) print $0, ".refptr." p substr($0, 9); else print $0, p $0 }' > "$1.map"
	objcopy --redefine-syms="$1.map" --strip-debug "build-$1/libturbojpeg.a" "$root/libs/$2"
	if nm -g --defined-only "$root/libs/$2" 2>/dev/null | awk 'NF == 3 { print $3 }' | grep -v "^$PREFIX\|^\.refptr\.$PREFIX"; then
		echo "unprefixed symbols left in $2" >&2
		exit 1
	fi
}

if [ "${RUN_TESTS:-}" = 1 ]; then
	build windows
	cmake --build build-windows -j8
	(cd build-windows && ctest -j8)
else
	build windows -DWITH_TOOLS=OFF -DWITH_TESTS=OFF
fi
cross musl x86_64-linux-musl
cross gnu x86_64-linux-gnu.2.17

prefix windows libturbojpeg_windows_amd64.a
prefix musl libturbojpeg_linux_amd64_musl.a
prefix gnu libturbojpeg_linux_amd64.a
cp "$src/src/turbojpeg.h" "$src/LICENSE.md" "$src/README.ijg" "$root/include/turbojpeg/"
sha256sum "$root"/libs/libturbojpeg_*.a "$root"/include/turbojpeg/turbojpeg.h
