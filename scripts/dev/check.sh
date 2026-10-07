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
    # Two kinds of expected noise, filtered so the number means something.
    #
    # build-id: comes from linking a freestanding image.
    # unused-command-line-argument: clang is handed the C flags when it
    #   assembles entry.s and says so, eight times, on every clean build.
    #
    # Filtering the second one is not cosmetic. It was unfiltered, so a clean
    # build always printed warnings=8 - and a counter that is never zero is a
    # counter nobody reads. Four -Wcomment warnings from a real mistake were
    # dismissed three times in one session as "transient", precisely because
    # the number was already noise. The clang counter below has always filtered
    # this; the two disagreed, and the noisier one is the one people saw first.
    echo "warnings=$(grep 'warning:' /tmp/vibeos-build.log | grep -vc 'build-id\|unused-command-line-argument')"
    # The checks that read the tree rather than run it. They live in their own
    # file so that the CI runs the same list (external review, 2026-10-07).
    bash scripts/dev/static-checks.sh "$d"
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
    # The GUI's too (C7): the thread phase is the only thing that can see its
    # lock go missing, short of the nightly's ThreadSanitizer.
    "./$d/vibeos_gui_torture" 1 300 4 >/dev/null 2>&1 || t=1
    # And the file locks' (docs/abi/ L1 step 6): ranges split and joined,
    # against a model that has no ranges.
    "./$d/vibeos_filelock_torture" 1 3000 >/dev/null 2>&1 || t=1
    # The FAT writer against somebody else's reader (docs/abi/ L1 step 4): the
    # host tests format their own volume, so they can only say the writer and
    # the reader agree with each other. Skips, and says so, without mtools.
    local fm
    fm="$(bash scripts/dev/verify-fat-mtools.sh "$d" | tail -1)"
    echo "$fm"
    case "$fm" in fat-mtools=ok*|fat-mtools=skip*) ;; *) t=1 ;; esac
    if [ "$k" -eq 0 ] && [ "$b" -eq 0 ] && [ "$t" -eq 0 ]; then
        echo "host-tests=pass"
    else
        echo "host-tests=FAIL kernel=$k bootloader=$b torture=$t"
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
