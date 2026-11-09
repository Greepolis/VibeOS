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

run_case ipc kernel/ipc/channel.c scripts/dev/cases/ipc-coverage.txt
run_case ipc kernel/ipc/event.c scripts/dev/cases/event-coverage.txt
run_case ipc kernel/ipc/waitset.c scripts/dev/cases/waitset-coverage.txt
run_case object kernel/object/handle_table.c scripts/dev/cases/object-coverage.txt

# ABI has its own handler-level model and verifier; do not route it through the
# IPC/OBJECT torture executable.
SABOTAGE_VERIFY="bash scripts/dev/verify-abi-torture.sh $build_dir" \
    python3 scripts/dev/sabotage.py kernel/abi/linux/fs.c \
        scripts/dev/cases/abi-coverage.txt "$build_dir"

SABOTAGE_VERIFY="bash scripts/dev/verify-abi-torture.sh $build_dir" \
    python3 scripts/dev/sabotage.py kernel/abi/files/regular.c \
        scripts/dev/cases/abi-regular-coverage.txt "$build_dir"

SABOTAGE_VERIFY="bash scripts/dev/verify-abi-torture.sh $build_dir" \
    python3 scripts/dev/sabotage.py kernel/abi/linux/names.c \
        scripts/dev/cases/abi-names-coverage.txt "$build_dir"

SABOTAGE_VERIFY="bash scripts/dev/verify-abi-torture.sh $build_dir" \
    python3 scripts/dev/sabotage.py kernel/abi/linux/mm.c \
        scripts/dev/cases/abi-mm-coverage.txt "$build_dir"

SABOTAGE_VERIFY="bash scripts/dev/verify-abi-torture.sh $build_dir" \
    python3 scripts/dev/sabotage.py kernel/mm/vmspace.c \
        scripts/dev/cases/abi-cow-coverage.txt "$build_dir"

# NT is a distinct personality: keep its first handle-model mutation on the
# same handler-level executable, not the Linux-only dispatcher path.
SABOTAGE_VERIFY="bash scripts/dev/verify-nt-abi-torture.sh $build_dir" \
    python3 scripts/dev/sabotage.py kernel/abi/nt/object.c \
        scripts/dev/cases/abi-nt-coverage.txt "$build_dir"

SABOTAGE_VERIFY="bash scripts/dev/verify-nt-abi-torture.sh $build_dir" \
    python3 scripts/dev/sabotage.py kernel/abi/nt/object.c \
        scripts/dev/cases/abi-nt-duplicate-coverage.txt "$build_dir"

# NT file transfers are independently checked for returned bytes and the
# shared file position; keep each direction as a separate production mutation.
SABOTAGE_VERIFY="bash scripts/dev/verify-nt-abi-torture.sh $build_dir" \
    python3 scripts/dev/sabotage.py kernel/abi/nt/file.c \
        scripts/dev/cases/abi-nt-read-coverage.txt "$build_dir"

SABOTAGE_VERIFY="bash scripts/dev/verify-nt-abi-torture.sh $build_dir" \
    python3 scripts/dev/sabotage.py kernel/abi/nt/file.c \
        scripts/dev/cases/abi-nt-write-coverage.txt "$build_dir"

# NtCreateFile has its own NT dispatcher/model test in vibeos_kernel_tests;
# the Linux ABI torture executable cannot discriminate NT create dispositions.
SABOTAGE_VERIFY="bash scripts/dev/verify-nt-abi-torture.sh $build_dir" \
    python3 scripts/dev/sabotage.py kernel/abi/nt/file.c \
        scripts/dev/cases/abi-nt-create-coverage.txt "$build_dir"
