#!/usr/bin/env bash
# Download, verify and build the corpus's third-party programs with musl.
#
#   scripts/dev/corpus-build.sh [build-dir] [--no-ltp]
#
# Sources are pinned in tests/corpus/sources.txt and cached outside the tree
# (${CORPUS_CACHE:-~/.cache/vibeos-corpus}); a file whose SHA-256 differs from
# the pin is refused, not built. Everything is static and linked against musl,
# like the test programs the boot gate already runs:
#
#   <build>/corpus/sqlite3        the SQLite shell
#   <build>/corpus/lua            the Lua interpreter
#   <build>/corpus/ltp/<test>     LTP's syscall tests that build against musl
#   <build>/corpus/ltp-built.txt  <syscall-dir>/<test> for each of them
#
# LTP's configure wants pkg-config for optional libraries; a stand-in that finds
# none is put on PATH, so those tests are simply left out. Linux's own headers
# are made visible *after* musl's (-idirafter), because the tests include
# <linux/...> and musl ships none.
set -uo pipefail
cd "$(dirname "$0")/../.."
ROOT=$(pwd)
BUILD=${1:-build-gcc-Release}
NO_LTP=0
[ "${2:-}" = "--no-ltp" ] && NO_LTP=1
case "$BUILD" in /*) ;; *) BUILD="$ROOT/$BUILD" ;; esac
CACHE=${CORPUS_CACHE:-$HOME/.cache/vibeos-corpus}
OUT="$BUILD/corpus"
SRC="$OUT/src"
mkdir -p "$CACHE" "$OUT" "$SRC"

command -v musl-gcc > /dev/null || { echo "corpus-build: musl-gcc not installed"; exit 2; }

fetch() {   # name -> path of the verified archive
    local line url sum file
    line=$(grep -E "^$1 \|" "$ROOT/tests/corpus/sources.txt") || { echo "no source $1" >&2; return 1; }
    url=$(echo "$line" | cut -d'|' -f2 | tr -d ' ')
    sum=$(echo "$line" | cut -d'|' -f3 | tr -d ' ')
    file="$CACHE/$(basename "$url")"
    if [ ! -f "$file" ]; then
        curl -sSfL -o "$file.part" "$url" && mv "$file.part" "$file" || return 1
    fi
    if [ "$(sha256sum "$file" | cut -d' ' -f1)" != "$sum" ]; then
        echo "corpus-build: $file does not match its pinned sha256 - refusing it" >&2
        return 1
    fi
    echo "$file"
}

rc=0

# ---- SQLite: the amalgamation and its shell ----
if f=$(fetch sqlite); then
    rm -rf "$SRC/sqlite" && mkdir -p "$SRC/sqlite"
    python3 -m zipfile -e "$f" "$SRC/sqlite"
    d=$(dirname "$(find "$SRC/sqlite" -name sqlite3.c | head -1)")
    # No extension loading (no dlopen in a static musl binary) and no threads
    # (the shell does not use them): the program as a single-threaded file user.
    if musl-gcc -O2 -static -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_THREADSAFE=0 \
            "$d/shell.c" "$d/sqlite3.c" -o "$OUT/sqlite3" -lm > "$OUT/sqlite.log" 2>&1; then
        echo "corpus-build: sqlite3 ok"
    else
        echo "corpus-build: sqlite3 FAILED (see $OUT/sqlite.log)"; rc=1
    fi
else
    rc=1
fi

# ---- Lua ----
if f=$(fetch lua); then
    rm -rf "$SRC/lua" && mkdir -p "$SRC/lua"
    tar xzf "$f" -C "$SRC/lua"
    d=$(find "$SRC/lua" -maxdepth 1 -type d -name 'lua-*' | head -1)
    if make -s -C "$d" posix CC="musl-gcc -static" > "$OUT/lua.log" 2>&1; then
        cp "$d/src/lua" "$OUT/lua"
        echo "corpus-build: lua ok"
    else
        echo "corpus-build: lua FAILED (see $OUT/lua.log)"; rc=1
    fi
else
    rc=1
fi

# ---- LTP: the syscall tests that build against musl ----
if [ $NO_LTP -eq 0 ]; then
    if f=$(fetch ltp); then
        rm -rf "$SRC/ltp" && mkdir -p "$SRC/ltp"
        tar xJf "$f" -C "$SRC/ltp"
        d=$(find "$SRC/ltp" -maxdepth 1 -type d -name 'ltp-*' | head -1)
        stub="$OUT/stub-bin"
        mkdir -p "$stub"
        printf '#!/bin/sh\ncase "$1" in --version) echo 0.29.2; exit 0 ;; --atleast-pkgconfig-version*) exit 0 ;; esac\nexit 1\n' > "$stub/pkg-config"
        chmod +x "$stub/pkg-config"
        (
            cd "$d" || exit 1
            export PATH="$stub:$PATH"
            ./configure CC=musl-gcc LDFLAGS=-static \
                CFLAGS='-O2 -idirafter /usr/include -idirafter /usr/include/x86_64-linux-gnu' \
                > "$OUT/ltp-configure.log" 2>&1 || exit 1
            make -j"$(nproc)" -C lib > "$OUT/ltp-lib.log" 2>&1 || exit 1
            # One make per syscall directory. The top-level target loops over the
            # directories and stops at the first that fails, -k or not, so a
            # single test that does not link against musl (fmtmsg01 does not)
            # silently dropped every directory after it in the alphabet - the
            # first build produced 378 tests of about 1,400. A directory that
            # fails is recorded and the others go on.
            : > "$OUT/ltp-syscalls.log"
            : > "$OUT/ltp-failed-dirs.txt"
            ls -d testcases/kernel/syscalls/*/ | xargs -P "$(nproc)" -I{} sh -c \
                'make -k -C "{}" > "{}/.corpus-build.log" 2>&1 || basename "{}" >> "'"$OUT"'/ltp-failed-dirs.txt"'
            cat testcases/kernel/syscalls/*/.corpus-build.log >> "$OUT/ltp-syscalls.log"
            sort -o "$OUT/ltp-failed-dirs.txt" "$OUT/ltp-failed-dirs.txt"
            exit 0
        ) || { echo "corpus-build: ltp configure or lib FAILED (see $OUT/ltp-*.log)"; rc=1; }
        rm -rf "$OUT/ltp" && mkdir -p "$OUT/ltp"
        : > "$OUT/ltp-built.txt"
        find "$d/testcases/kernel/syscalls" -type f -perm -u+x ! -name '*.sh' ! -name '*.py' \
                -newer "$d/configure" 2>/dev/null | while read -r t; do
            if file "$t" | grep -q 'ELF 64-bit'; then
                rel=${t#"$d/testcases/kernel/syscalls/"}
                echo "$(echo "$rel" | cut -d/ -f1)/$(basename "$t")" >> "$OUT/ltp-built.txt"
                cp "$t" "$OUT/ltp/"
            fi
        done
        sort -o "$OUT/ltp-built.txt" "$OUT/ltp-built.txt"
        echo "corpus-build: ltp $(wc -l < "$OUT/ltp-built.txt") syscall tests built"
    else
        rc=1
    fi
fi
exit $rc
