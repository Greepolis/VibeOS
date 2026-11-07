#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Every mutation is checked by its own module model, never by a neighbouring
# module's failure. sabotage.py restores each source in a finally block and
# refuses to start over a stale backup.
set -euo pipefail
build_dir="${1:-build-module-asan}"

run_case() {
    local module="$1" source="$2" cases="$3"
    SABOTAGE_VERIFY="bash scripts/dev/verify-module-torture.sh $build_dir $module" \
        python3 scripts/dev/sabotage.py "$source" "$cases" "$build_dir"
}

run_case abi kernel/abi/abi_linux.c scripts/dev/cases/abi-coverage.txt
run_case ipc kernel/ipc/channel.c scripts/dev/cases/ipc-coverage.txt
run_case object kernel/object/handle_table.c scripts/dev/cases/object-coverage.txt
run_case proc kernel/proc/process.c scripts/dev/cases/proc-coverage.txt
run_case txn kernel/txn/journal.c scripts/dev/cases/txn-coverage.txt
