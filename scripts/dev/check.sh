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
    # A deliberate fault planted to test the panic path once got committed,
    # because removing it was a separate step that a change of plan skipped.
    # Verifying a crash handler means planting crashes, so this will be done
    # again; the check costs nothing and the mistake costs a boot that dies on
    # purpose in everybody's build.
    if grep -rn 'TEMPORARY' kernel/ --include=*.c > /dev/null 2>&1; then
        echo "LEFTOVER-DEBUG-CODE:"
        grep -rn 'TEMPORARY' kernel/ --include=*.c | head -3
    fi
}

do_tests() {
    echo "=== host tests"
    # A single verdict line, in the same shape as rc= and reason=, because the
    # detail above it is easy to filter away by accident - and was. Several
    # sessions reported "host tests green" while test_kmain had been failing,
    # for want of one greppable word in a fixed place.
    local k=0 b=0
    "./$d/vibeos_kernel_tests" | tail -2 || k=1
    "./$d/vibeos_kernel_tests" >/dev/null 2>&1 || k=1
    "./$d/vibeos_bootloader_tests" | tail -1 || b=1
    "./$d/vibeos_bootloader_tests" >/dev/null 2>&1 || b=1
    if [ "$k" -eq 0 ] && [ "$b" -eq 0 ]; then
        echo "host-tests=pass"
    else
        echo "host-tests=FAIL kernel=$k bootloader=$b"
    fi
}

do_smoke() {
    echo "=== boot smoke $d"
    # 300s is ample for a healthy boot (about 90s) and the harness now
    # gives up early on a wedged guest, so a failure costs about two minutes
    # rather than the whole budget.
    python3 scripts/qemu-cli-smoke-linux.py "$d" 300 > /dev/null 2>&1
    grep -o '^reason=.*' qemu-cli-summary.txt
    grep -aoE 'auxv ok|auxv wrong|linux abi ok[^\r]*|abi: [^\r]*|tls survived[^\r]*|tls lost[^\r]*|MUSL_OK[^\r]*|MUSL_ARGS[^\r]*|unimplemented Linux syscall nr=0x[0-9a-f]*' \
        qemu-cli-serial.log | sort -u
}

# The fuzz target is the one thing this script did not build, because it needs
# clang and its own configuration - and that is exactly how a receive path that
# had grown a new dependency stayed green here while CI could not link it. It
# gets its own build directory rather than disturbing "$d".
do_fuzz() {
    echo "=== fuzz build"
    if ! command -v clang > /dev/null 2>&1; then
        # Said out loud rather than skipped quietly: a step that vanishes when
        # a tool is missing reads exactly like a step that passed.
        echo "fuzz=skipped-no-clang"
        return
    fi
    cmake -S . -B build-fuzz -G Ninja -DCMAKE_C_COMPILER=clang           -DCMAKE_BUILD_TYPE=Release -DVIBEOS_BUILD_FUZZERS=ON           > /tmp/vibeos-fuzz.log 2>&1 &&
        cmake --build build-fuzz --target fuzz_inet_input -j"$(nproc)"               >> /tmp/vibeos-fuzz.log 2>&1
    echo "fuzz-rc=$?"
    grep -E 'error:|undefined reference' /tmp/vibeos-fuzz.log | head -5
}

case "$what" in
    build) do_build ;;
    tests) do_tests ;;
    smoke) do_smoke ;;
    fuzz)  do_fuzz ;;
    all)   do_build; do_tests; do_fuzz; do_smoke ;;
    *)     echo "usage: $0 [build|tests|smoke|fuzz|all] [build-dir]"; exit 2 ;;
esac
echo CHECK_DONE
