#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Every mutation must make its own model torture go red. sabotage.py restores
# each source in a finally block and refuses to start over a stale backup.
set -euo pipefail
build_dir="${1:-build-module-asan}"
export SABOTAGE_VERIFY="bash scripts/dev/verify-module-torture.sh $build_dir"
python3 scripts/dev/sabotage.py kernel/abi/abi_linux.c scripts/dev/cases/abi-coverage.txt "$build_dir"
python3 scripts/dev/sabotage.py kernel/ipc/channel.c scripts/dev/cases/ipc-coverage.txt "$build_dir"
python3 scripts/dev/sabotage.py kernel/object/handle_table.c scripts/dev/cases/object-coverage.txt "$build_dir"
python3 scripts/dev/sabotage.py kernel/proc/process.c scripts/dev/cases/proc-coverage.txt "$build_dir"
python3 scripts/dev/sabotage.py kernel/txn/journal.c scripts/dev/cases/txn-coverage.txt "$build_dir"
