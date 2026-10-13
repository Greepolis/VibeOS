#!/usr/bin/env bash
# Build Wine's conformance tests for the NT personality's oracle (docs/abi/windows.md).
#
#   scripts/dev/winetest-build.sh [build-dir] [dll...]      default: ntdll kernel32
#
# Produces, under <build>/winetest/:
#   <dll>_test.exe        one PE executable a DLL, as Wine builds them: every test
#                         file of dlls/<dll>/tests and a generated list, run as
#                         `<dll>_test.exe <test>`; `--list` names the tests
#
# The source is Wine's own release tarball, pinned in tests/corpus/sources.txt and
# kept in the corpus cache; nothing of it is committed, and nothing of it goes into
# VibeOS - the tests are an oracle.
#
# They are built by Wine's own build system, configured without X and without
# any of the host libraries a test executable does not need, because compiling
# the test files alone against mingw-w64 (the first version of this script) does
# not work: they include headers Wine generates from IDL at build time, Wine's
# headers and mingw's disagree on basic types, and they link against helpers
# (debugstr_w, __wine_dbg_strdup) that live in Wine's own import libraries. Make
# builds the tools, the generated headers and the import libraries the two
# executables need, and nothing else. The build tree lives on the WSL side
# (WINETEST_DIR), since Wine's build over /mnt/c takes several times as long.
#
# The answers come from running the executables under Wine on the host
# (winetest-run-host.sh); VibeOS is measured against those answers.
set -u
cd "$(dirname "$0")/../.." || exit 1
BUILD="${1:-build-gcc-Release}"
shift || true
DLLS="${*:-ntdll kernel32}"
CACHE=${CORPUS_CACHE:-$HOME/.cache/vibeos-corpus}
WORK=${WINETEST_DIR:-$HOME/.cache/vibeos-winetest}
OUT="$BUILD/winetest"

for tool in x86_64-w64-mingw32-gcc flex bison; do
    command -v "$tool" > /dev/null || { echo "winetest-build: $tool not installed"; exit 2; }
done
mkdir -p "$CACHE" "$WORK" "$OUT"

line=$(grep -E "^wine \|" tests/corpus/sources.txt) || { echo "winetest-build: no wine source pinned"; exit 2; }
url=$(echo "$line" | cut -d'|' -f2 | tr -d ' ')
sum=$(echo "$line" | cut -d'|' -f3 | tr -d ' ')
tarball="$CACHE/$(basename "$url")"
if [ ! -f "$tarball" ]; then
    curl -sSfL -o "$tarball.part" "$url" && mv "$tarball.part" "$tarball" || { echo "winetest-build: download failed"; exit 1; }
fi
if [ "$(sha256sum "$tarball" | cut -d' ' -f1)" != "$sum" ]; then
    echo "winetest-build: $tarball does not match its pinned sha256 - refusing it"; exit 1
fi
SRC="$WORK/$(basename "$url" .tar.xz)"
if [ ! -d "$SRC" ]; then
    tar xJf "$tarball" -C "$WORK" || exit 1
fi

OBJ="$WORK/build"
mkdir -p "$OBJ"
if [ ! -f "$OBJ/Makefile" ]; then
    # Everything a test executable does not link is left out, so that configure
    # asks the host for nothing beyond a compiler, mingw-w64, flex and bison.
    (cd "$OBJ" && "$SRC/configure" --enable-win64 --without-x --without-freetype \
        --without-gstreamer --without-pulse --without-alsa --without-oss \
        --without-cups --without-sane --without-gphoto --without-v4l2 \
        --without-dbus --without-krb5 --without-gssapi --without-netapi \
        --without-pcap --without-usb --without-udev --without-unwind \
        --without-vulkan --without-opencl --without-osmesa --without-wayland \
        --without-capi --without-sdl --without-xinerama --without-gnutls \
        > configure.log 2>&1) || { echo "winetest-build: configure failed (see $OBJ/configure.log)"; exit 1; }
fi

rc=0
for dll in $DLLS; do
    target="dlls/$dll/tests/x86_64-windows/${dll}_test.exe"
    if make -C "$OBJ" -j"$(nproc)" "$target" > "$OUT/$dll.log" 2>&1; then
        cp "$OBJ/$target" "$OUT/" && echo "winetest-build: $dll ok"
    else
        echo "winetest-build: $dll did not build (see $OUT/$dll.log)"; rc=1
    fi
done
exit $rc
