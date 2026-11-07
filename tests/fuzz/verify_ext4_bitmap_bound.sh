#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Require write_file() to expose the removed #202 guard through its adjacent
# scratch-buffer red zone. Only the explicit marker counts as detection.
set -uo pipefail
build_dir="${1:?usage: verify-ext4-bitmap-bound.sh BUILD_DIR}"
if ! cmake --build "$build_dir" --parallel --target fuzz_ext2_image; then
  echo "BUILD_FAILED: could not rebuild instrumented ext4 fuzzer"
  exit 2
fi
tmp="$(mktemp -d)"
log="$PWD/ext4-bitmap-negative-control.log"
trap 'rm -rf "$tmp"' EXIT
if ! python3 tests/fuzz/make_seed_corpus.py "$tmp/corpus"; then
  echo "BUILD_FAILED: could not create real-tool filesystem seeds"
  exit 2
fi
seed="$tmp/corpus/ext4/ext4-bad-bitmap-geometry.img"
if [ ! -s "$seed" ]; then
  echo "BUILD_FAILED: generated ext4 geometry seed is missing"
  exit 2
fi
set +e
ASAN_OPTIONS="abort_on_error=1:halt_on_error=1:detect_leaks=0" \
  "$build_dir/fuzz_ext2_image" "$seed" -runs=1 -max_len=8388608 \
  >"$log" 2>&1
rc=$?
set -e
if grep -q "BITMAP_REDZONE_CORRUPTION:" "$log"; then
  echo "DETECTED: write_file() corrupted the bitmap scratch red zone"
  exit 1
fi
if [ "$rc" -ne 0 ]; then
  echo "BUILD_FAILED: fuzzer failed without the expected bitmap red-zone marker"
  cat "$log"
  exit 2
fi
echo "NOT_DETECTED: write_file() did not reach the removed bitmap bound"
cat "$log"
exit 0
