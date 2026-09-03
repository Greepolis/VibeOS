#!/usr/bin/env bash
# Build and verify, in one command.
#
#   scripts/dev/check.sh [build|tests|smoke|all] [build-dir]
#
# Every subcommand builds first. "tests" and "smoke" used to run whatever binary
# was lying around, and a sabotage run scored green against a stale one within an
# hour of that being possible - the same trap CLAUDE.md records for boots.sh,
# wearing a third set of clothes. Building twice costs a no-op ninja run.
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
    # The memory manager's layering, checked by the build rather than by
    # review. The property it guards - one place decides what an address space
    # owns - is the whole of phase P2, and it is the kind that erodes one
    # reasonable-looking line at a time.
    bash scripts/dev/check-mm-layering.sh | tail -1
    # The interpreter substitution, checked the same way and for the same
    # reason: a rule that lives only in a comment erodes one reasonable
    # looking line at a time.
    bash scripts/dev/check-exec-layering.sh | tail -1
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
    local k=0 b=0 t=0
    "./$d/vibeos_kernel_tests" | tail -2 || k=1
    "./$d/vibeos_kernel_tests" >/dev/null 2>&1 || k=1
    "./$d/vibeos_bootloader_tests" | tail -1 || b=1
    "./$d/vibeos_bootloader_tests" >/dev/null 2>&1 || b=1
    # A short torture run here too, not only in the nightly. The nightly is
    # where the seeds get deep enough to matter, but a change that breaks the
    # memory manager outright should fail on the machine that made it.
    "./$d/vibeos_mm_torture" 1 2000 >/dev/null 2>&1 || t=1
    if [ "$k" -eq 0 ] && [ "$b" -eq 0 ] && [ "$t" -eq 0 ]; then
        echo "host-tests=pass"
    else
        echo "host-tests=FAIL kernel=$k bootloader=$b mm-torture=$t"
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
# The kernel, compiled by the other compiler CI uses.
#
# This is not thoroughness for its own sake. gcc accepts an implicit function
# declaration with a warning; clang rejects it. A file lifted out of arch_hw.c
# that forgot an include therefore built green here and failed in CI - and the
# local warning count had moved from 0 to 4, which is the build saying so and
# was read as noise.
#
# Only the compile: the boot and the tests already run against the gcc build,
# and what differs between the two compilers is what they refuse, not what they
# emit.
do_clang() {
    echo "=== clang build"
    if ! command -v clang > /dev/null 2>&1; then
        # Said out loud rather than skipped quietly, for the same reason the
        # fuzz step says it: a step that vanishes reads exactly like one that
        # passed.
        echo "clang=skipped-no-clang"
        return
    fi
    cmake -S . -B build-clang-Release -G Ninja -DCMAKE_C_COMPILER=clang \
          -DCMAKE_BUILD_TYPE=Release > /tmp/vibeos-clang-cfg.log 2>&1
    cmake --build build-clang-Release -j"$(nproc)" > /tmp/vibeos-clang.log 2>&1
    echo "clang-rc=$?"
    # Source warnings only, and the filter is the whole point.
    #
    # The first version counted every line containing "warning:", which is
    # mostly the linker saying it discarded a build-id note - nineteen of
    # them on a clean build and none on an incremental one. The number moved
    # between 0, 1, 2, 3 and 19 for reasons that had nothing to do with the
    # code, which makes it a check that reports healthy behaviour and so a
    # check people learn to ignore. CLAUDE.md says to treat a moving warning
    # count as the build telling you something; that only works if it is
    # telling the truth.
    echo "clang-warnings=$(grep 'warning:' /tmp/vibeos-clang.log | grep -vc 'build-id\|unused-command-line-argument')"
    grep -E 'error:' /tmp/vibeos-clang.log | head -5
}

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
    tests) do_build; do_tests ;;
    smoke) do_build; do_smoke ;;
    fuzz)  do_fuzz ;;
    clang) do_clang ;;
    all)   do_build; do_tests; do_clang; do_fuzz; do_smoke ;;
    *)     echo "usage: $0 [build|tests|smoke|fuzz|all] [build-dir]"; exit 2 ;;
esac
echo CHECK_DONE
