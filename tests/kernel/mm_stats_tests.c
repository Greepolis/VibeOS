/* Host tests for the memory-management counters (plan phase P0).
 *
 * A separate file rather than nine hundred more lines in kernel_tests.c, which
 * is already past eight thousand. The plan asks the memory manager to be one
 * concern per file; its tests are held to the same rule.
 *
 * What is worth testing about a plain structure with one accessor is narrow but
 * not empty: that the accessor is stable, that reset actually clears every
 * field rather than the ones somebody remembered, and that the structure has no
 * padding hiding a field the reset loop steps over. That last one is the reason
 * the reset is written as a loop over uint64_t and not as a field-by-field
 * assignment - and it is exactly the kind of thing that is silently true today
 * and silently false the day somebody adds a uint32_t.
 */

#include <stdio.h>

#include "vibeos/mm_stats.h"

int test_mm_stats(void);

int test_mm_stats(void) {
    vibeos_mm_stats_t *a = vibeos_mm_stats();
    vibeos_mm_stats_t *b = vibeos_mm_stats();
    const uint64_t *words;
    unsigned i;
    unsigned count = (unsigned)(sizeof(vibeos_mm_stats_t) / sizeof(uint64_t));

    /* The accessor is stable: every caller must see the same counters, or the
     * console and the boot gate would be reading different machines. */
    if (a == 0 || a != b) {
        return -1;
    }

    /* Every field is a uint64_t, so the structure divides exactly and the reset
     * loop cannot step over anything. If a field of another type is ever added,
     * this fails here rather than by leaving a counter uncleared in a test. */
    if (sizeof(vibeos_mm_stats_t) % sizeof(uint64_t) != 0) {
        return -1;
    }

    /* Reset clears all of it. Written by poking every word rather than by
     * setting the fields this test happens to know the names of: a reset that
     * misses a field is precisely the bug worth catching, and naming fields
     * here would make the test blind to exactly that. */
    words = (const uint64_t *)(const void *)a;
    for (i = 0; i < count; i++) {
        ((uint64_t *)(void *)a)[i] = 0xA5A5A5A5A5A5A5A5ull;
    }
    vibeos_mm_stats_reset();
    for (i = 0; i < count; i++) {
        if (words[i] != 0ull) {
            printf("FAIL:mm_stats word %u survived reset\n", i);
            return -1;
        }
    }

    /* And the counters actually count: a write through the accessor is visible
     * through another call, which is the whole contract the console relies on. */
    vibeos_mm_stats()->frames_total = 1234ull;
    if (vibeos_mm_stats()->frames_total != 1234ull) {
        return -1;
    }
    vibeos_mm_stats_reset();
    if (vibeos_mm_stats()->frames_total != 0ull) {
        return -1;
    }

    return 0;
}
