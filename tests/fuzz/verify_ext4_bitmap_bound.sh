#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Require the real ext4 writer fuzz target to catch the removed #202 bound.
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
if grep -Eq "AddressSanitizer|libFuzzer: deadly signal|DEADLYSIGNAL|Illegal instruction" "$log"; then
  echo "DETECTED: ext4 writer crossed the bitmap scratch boundary"
  exit 1
fi
if [ "$rc" -ne 0 ]; then
  echo "BUILD_FAILED: fuzzer failed without the expected ASan report"
  cat "$log"
  exit 2
fi
echo "NOT_DETECTED: malformed ext4 image did not reach the removed guard"
cat "$log"
exit 0
