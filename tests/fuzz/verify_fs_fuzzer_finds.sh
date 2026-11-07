#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Rebuild fuzz_fs_images over a sabotaged parser and require the fuzzer to
# find the defect by itself, from the real-tool seeds, within a fixed budget.
# Exit 1 = found (the case is red, as it should be), 0 = not found, 2 = the
# run proved nothing (build or seed failure, or a crash that is not ASan's).
set -uo pipefail
build_dir="${1:?usage: verify_fs_fuzzer_finds.sh BUILD_DIR [SECONDS]}"
budget="${2:-120}"
if ! cmake --build "$build_dir" --parallel --target fuzz_fs_images; then
  echo "BUILD_FAILED: could not rebuild fuzz_fs_images"
  exit 2
fi
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
if ! python3 tests/fuzz/make_seed_corpus.py "$tmp/corpus" >/dev/null; then
  echo "BUILD_FAILED: could not create real-tool seeds"
  exit 2
fi
log="$tmp/fuzz.log"
ASAN_OPTIONS="abort_on_error=1:halt_on_error=1:detect_leaks=0" \
  "$build_dir/fuzz_fs_images" "$tmp/corpus/fs" -max_total_time="$budget" \
  -max_len=8388608 -artifact_prefix="$tmp/" >"$log" 2>&1
rc=$?
if grep -q "ERROR: AddressSanitizer" "$log"; then
  grep -m1 "ERROR: AddressSanitizer" "$log"
  echo "DETECTED: the fuzzer found the sabotaged parser"
  exit 1
fi
if [ "$rc" -ne 0 ]; then
  echo "BUILD_FAILED: the fuzzer stopped without an AddressSanitizer report"
  tail -20 "$log"
  exit 2
fi
echo "NOT_DETECTED: no report in ${budget}s"
exit 0
