#!/usr/bin/env bash
# The verify step for sabotage cases that only a sanitizer can see: the CI job
# "Linux clang / Debug" (cmake-multi-platform.yml), as it runs - clang, ASan
# and UBSan, host tests only, ctest - and its first failure.
#
# In the repo for the reason verify-boot.sh gives. Its own build directory, so
# it never touches the one check.sh and the boot use.
set -u
cd "$(dirname "$0")/../.." || exit 1

B=build-asan
if [ ! -f "$B/build.ninja" ]; then
    if ! cmake -S . -B "$B" -G Ninja \
            -DCMAKE_C_COMPILER=clang \
            -DCMAKE_BUILD_TYPE=Debug \
            -DVIBEOS_BUILD_TESTS=ON \
            -DVIBEOS_BUILD_KERNEL_IMAGE=OFF \
            -DVIBEOS_ENABLE_TLS=OFF \
            -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
            -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined" \
            > /tmp/verify-san-cfg.log 2>&1; then
        echo "BUILD_FAILED (configure; see /tmp/verify-san-cfg.log)"
        exit 1
    fi
fi
if ! cmake --build "$B" --parallel > /tmp/verify-san-build.log 2>&1; then
    echo "BUILD_FAILED (see /tmp/verify-san-build.log)"
    exit 1
fi
if ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
        ctest --test-dir "$B" --output-on-failure > /tmp/verify-san.log 2>&1; then
    exit 0
fi
grep -m2 -E "runtime error|ERROR: AddressSanitizer|FAIL" /tmp/verify-san.log | cut -c1-200
exit 1
