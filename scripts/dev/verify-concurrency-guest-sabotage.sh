#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Rebuild the guest after a production-source sabotage and require one of the
# named QEMU invariants to fail. Infrastructure/build failures are not scored red.
set -uo pipefail
build_dir="${1:?usage: verify-concurrency-guest-sabotage.sh BUILD_DIR EXPECTED_REASON[|ALTERNATE]}"
expected="${2:?expected QEMU summary reason required}"
# The guest test program is a separate ALL target, not a dependency of
# vibeos_image. Rebuild it explicitly so a sabotage in musl_threads.c reaches
# the EFI image rather than silently testing the previous binary.
if ! cmake --build "$build_dir" --parallel --target vibeos_musl_threads; then
  echo "BUILD_FAILED: could not rebuild the guest thread workload"
  exit 2
fi
if ! cmake --build "$build_dir" --parallel --target vibeos_image; then
  echo "BUILD_FAILED: could not rebuild sabotaged guest"
  exit 2
fi
set +e
python3 scripts/qemu-cli-smoke-linux.py "$build_dir" 900
rc=$?
set -e
IFS='|' read -r -a expected_reasons <<< "$expected"
for reason in "${expected_reasons[@]}"; do
  # A production invariant may be detected in the serial evidence even when
  # the boot harness also reports a later-stage wedge (for example, a file
  # reference underflow can retire the self-test before its final marker).
  if [ "$reason" = "file_put_underflow" ]; then
    if [ -f qemu-cli-serial.log ] &&
       grep -Eq '\[MBZ\] MUSTBEZERO total=0x0*[1-9a-f][0-9a-f]* first=file_put_underflow' qemu-cli-serial.log; then
      echo "DETECTED: production sabotage triggered nonzero file_put_underflow"
      exit 1
    fi
  elif { [ -f qemu-cli-summary.txt ] && grep -Fq "$reason" qemu-cli-summary.txt; } ||
       { [ -f qemu-cli-serial.log ] && grep -Fq "$reason" qemu-cli-serial.log; }; then
    echo "DETECTED: production sabotage triggered $reason"
    exit 1
  fi
done
if [ "$rc" -ne 0 ]; then
  echo "BUILD_FAILED: QEMU failed for a reason other than the expected sabotage"
  if [ -f qemu-cli-summary.txt ]; then cat qemu-cli-summary.txt; fi
  exit 2
fi
echo "NOT_DETECTED: production sabotage did not trigger any expected reason"
exit 0
