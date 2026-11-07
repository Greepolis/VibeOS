# Timekeeping torture

**Scope:** production implementation in kernel/time/timer.c; host test executable
vibeos_timer_torture, registered with CTest.

The torture combines deterministic boundary probes with seeded state-transition
streams. Its IRQ oracle counts matching vectors modulo the divider rather than
reimplementing the production accumulator. Conversion expectations use a portable two-limb 128-bit product and
bitwise long-division oracle, independent of the production quotient/remainder
implementation and usable by MSVC without compiler extensions.

The deterministic prelude checks:
- deadline expiration at equality (now == deadline);
- wrong IRQ vectors do not advance the timer;
- rebinding the IRQ backend resets a partially consumed divider phase;
- nanosecond and millisecond conversions at values near UINT64_MAX;
- explicit UINT64_MAX -> 0 tick-counter wrap semantics.

Conversions saturate at UINT64_MAX when the duration is not representable.
Production code divides before multiplying to avoid intermediate overflow.

## Production mutation evidence

The nightly workflow applies five mutations to kernel/time/timer.c through
scripts/dev/sabotage.py. Each mutation must make the focused test fail:
1. omit resetting the divider phase during backend rebind;
2. invert the IRQ vector filter;
3. change deadline comparison from >= to >;
4. restore multiply-before-divide nanosecond conversion;
5. restore multiply-before-divide millisecond conversion.

Missing anchors and build failures are inconclusive, not successful mutations.
The harness restores the original source and reruns the ordinary kernel host
tests after all cases. The 500,000-operation runs are long state-transition
runs, not counter wraparound; wraparound is checked explicitly at the boundary.

Replay locally with:
    ./build-asan/vibeos_timer_torture 7 500000
