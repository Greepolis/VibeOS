#!/usr/bin/env bash
# Build and verify, in one command.
#
#   scripts/dev/check.sh [build|tests|smoke|all] [build-dir]
#
# Defaults to everything against build-gcc-Release. Prints only what matters:
# the return code, the count of real warnings, the test verdict, and the boot
# reason plus every ring-3 self-check line the guest produced.
set -uo pipefail
cd "$(dirname "$0")/../.."

what="${1:-all}"
d="${2:-build-gcc-Release}"

if [ ! -d "$d" ]; then
    echo "no build directory '$d' - configure one first, e.g."
    echo "  cmake -S . -B $d -G Ninja -DCMAKE_BUILD_TYPE=Release -DVIBEOS_BUILD_TESTS=ON -DVIBEOS_BUILD_KERNEL_IMAGE=ON"
    exit 2
fi

do_build() {
    echo "=== build $d"
    cmake --build "$d" -j"$(nproc)" > /tmp/vibeos-build.log 2>&1
    echo "rc=$?"
    grep -E 'error:|error ' /tmp/vibeos-build.log | head -5
    # The build-id note warning comes from linking a freestanding image and is
    # expected; counting it would hide the ones that are not.
    echo "warnings=$(grep 'warning:' /tmp/vibeos-build.log | grep -vc build-id)"
}

do_tests() {
    echo "=== host tests"
    "./$d/vibeos_kernel_tests" | tail -2
    "./$d/vibeos_bootloader_tests" | tail -1
}

do_smoke() {
    echo "=== boot smoke $d"
    python3 scripts/qemu-cli-smoke-linux.py "$d" 900 > /dev/null 2>&1
    grep -o '^reason=.*' qemu-cli-summary.txt
    grep -aoE 'auxv ok|auxv wrong|linux abi ok[^\r]*|abi: [^\r]*|tls survived[^\r]*|tls lost[^\r]*|MUSL_OK[^\r]*|MUSL_ARGS[^\r]*|unimplemented Linux syscall nr=0x[0-9a-f]*' \
        qemu-cli-serial.log | sort -u
}

case "$what" in
    build) do_build ;;
    tests) do_tests ;;
    smoke) do_smoke ;;
    all)   do_build; do_tests; do_smoke ;;
    *)     echo "usage: $0 [build|tests|smoke|all] [build-dir]"; exit 2 ;;
esac
echo CHECK_DONE
