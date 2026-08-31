/* Host tests for the backing layer and the page cache (plan phase P4).
 *
 * The cache is where a wrong answer is worst: a hit that returns the wrong
 * frame hands a program another file's contents and nothing anywhere reports
 * an error. So most of what follows checks identity - that what came back is
 * what was asked for - rather than merely that something came back.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vibeos/backing.h"
#include "vibeos/frame.h"
#include "vibeos/mm_stats.h"

int test_backing(void);

#define BK_FRAMES 64u
#define BK_BASE   0x300000ull
#define ENTRIES   8u

static unsigned char *g_ram;

static void *bk_map(uint64_t phys) {
    if (phys < BK_BASE || phys >= BK_BASE + (uint64_t)BK_FRAMES * 4096ull) {
        return 0;
    }
    return g_ram + (phys - BK_BASE);
}

static vibeos_frame_t g_ftable[BK_FRAMES];
static vibeos_cache_entry_t g_cache[ENTRIES];

static uint32_t g_reads;        /* how many times the "disk" was touched */
static int g_read_fails;

/* The stand-in disk writes a byte pattern derived from the key, so a page that
 * came from the wrong key is recognisable rather than merely different. */
static int bk_read(void *ctx, uint32_t file_id, uint64_t offset, uint64_t phys) {
    unsigned char *p = (unsigned char *)bk_map(phys);

    (void)ctx;
    g_reads++;
    if (g_read_fails || !p) {
        return -1;
    }
    p[0] = (unsigned char)file_id;
    p[1] = (unsigned char)(offset >> 12);
    return 0;
}

static int holds(uint64_t phys, uint32_t file_id, uint64_t offset) {
    const unsigned char *p = (const unsigned char *)bk_map(phys);
    return p && p[0] == (unsigned char)file_id &&
           p[1] == (unsigned char)(offset >> 12);
}

static void setup(void) {
    memset(g_ftable, 0, sizeof(g_ftable));
    vibeos_mm_stats_reset();
    (void)vibeos_frame_init(BK_BASE, (uint64_t)BK_FRAMES * 4096ull,
                            g_ftable, BK_FRAMES, bk_map);
    vibeos_cache_init(g_cache, ENTRIES, bk_read, 0);
    g_reads = 0;
    g_read_fails = 0;
}

int test_backing(void) {
    uint64_t a, b, c;
    uint32_t i;

    g_ram = (unsigned char *)malloc((size_t)BK_FRAMES * 4096u);
    if (!g_ram) {
        return -1;
    }

    /* ---- anonymous backing hands out a zeroed frame ---------------------- */
    setup();
    {
        const vibeos_backing_ops_t *anon = vibeos_backing_anon();
        uint64_t phys = 0;
        if (anon->fault_in(0, 0, &phys) != 0 || phys == 0ull) { goto fail; }
        for (i = 0; i < 4096u; i++) {
            if (((unsigned char *)bk_map(phys))[i] != 0u) {
                printf("FAIL:backing anon page was not zeroed\n");
                goto fail;
            }
        }
        /* Nowhere to write it back to, and that is said rather than pretended:
         * an interface member that silently succeeds reads as a feature. */
        if (anon->write_back(0, 0, phys) == 0) { goto fail; }
    }

    /* ---- miss, then hit -------------------------------------------------- */
    setup();
    if (vibeos_cache_get(1u, 0ull, &a) != 0) { goto fail; }
    if (!holds(a, 1u, 0ull)) { goto fail; }
    if (g_reads != 1u) { goto fail; }
    if (vibeos_mm_stats()->cache_misses != 1ull) { goto fail; }
    if (vibeos_cache_get(1u, 0ull, &b) != 0) { goto fail; }
    if (b != a) {
        printf("FAIL:backing hit returned a different frame\n");
        goto fail;
    }
    if (g_reads != 1u) {
        printf("FAIL:backing hit went to the disk anyway\n");
        goto fail;
    }
    if (vibeos_mm_stats()->cache_hits != 1ull) { goto fail; }

    /* ---- different keys are different pages ------------------------------ *
     *
     * The failure this guards against is silent: a hit that returns the wrong
     * frame hands a program another file's contents and nothing reports it. */
    if (vibeos_cache_get(2u, 0ull, &b) != 0) { goto fail; }
    if (b == a) { goto fail; }
    if (!holds(b, 2u, 0ull)) { goto fail; }
    if (vibeos_cache_get(1u, 4096ull, &c) != 0) { goto fail; }
    if (c == a || c == b) { goto fail; }
    if (!holds(c, 1u, 4096ull)) { goto fail; }

    /* ---- file id zero is refused ----------------------------------------- *
     *
     * Zero marks an empty slot, so letting it through would make "not cached"
     * and "cached under file zero" the same state. */
    if (vibeos_cache_get(0u, 0ull, &a) == 0) {
        printf("FAIL:backing accepted file id zero\n");
        goto fail;
    }

    /* ---- eviction: more keys than slots ---------------------------------- */
    setup();
    {
        /* Frames, not just counters. An eviction that forgets its entry and
         * keeps the frame leaks one page every time the table turns over, and
         * no counter would notice: the frame layer still has an owner for it,
         * so nothing is leaked as far as it can tell. Only the free count
         * falling faster than the table fills shows it. */
        uint64_t free_before = vibeos_frame_free_count();
        uint64_t phys;
        for (i = 0; i < ENTRIES * 3u; i++) {
            if (vibeos_cache_get(1u, (uint64_t)i * 4096ull, &phys) != 0) { goto fail; }
        }
        if (vibeos_frame_free_count() != free_before - (uint64_t)ENTRIES) {
            printf("FAIL:backing held %llu frames for a table of %u\n",
                   (unsigned long long)(free_before - vibeos_frame_free_count()),
                   ENTRIES);
            goto fail;
        }
    }
    setup();
    for (i = 0; i < ENTRIES * 3u; i++) {
        uint64_t phys;
        if (vibeos_cache_get(1u, (uint64_t)i * 4096ull, &phys) != 0) {
            printf("FAIL:backing could not serve key %u\n", i);
            goto fail;
        }
        if (!holds(phys, 1u, (uint64_t)i * 4096ull)) {
            printf("FAIL:backing served the wrong page for key %u\n", i);
            goto fail;
        }
        if (vibeos_cache_resident() > ENTRIES) {
            printf("FAIL:backing kept %u pages in a table of %u\n",
                   vibeos_cache_resident(), ENTRIES);
            goto fail;
        }
    }
    /* Evicted frames go back, or the cache leaks one frame per eviction and
     * the frame layer has nobody to blame. */
    if (vibeos_mm_stats()->frames_leaked != 0ull ||
        vibeos_mm_stats()->frames_double_put != 0ull) { goto fail; }

    /* ---- a failed read caches nothing ------------------------------------ *
     *
     * Placing the key before the read would leave it pointing at a frame
     * holding whatever was there, and the next hit would hand that out as the
     * file's contents. */
    setup();
    g_read_fails = 1;
    if (vibeos_cache_get(5u, 0ull, &a) == 0) { goto fail; }
    if (vibeos_cache_resident() != 0u) {
        printf("FAIL:backing cached a page it could not read\n");
        goto fail;
    }
    g_read_fails = 0;
    if (vibeos_cache_get(5u, 0ull, &a) != 0) { goto fail; }
    if (!holds(a, 5u, 0ull)) { goto fail; }

    /* ---- forget drops a file, and only that file ------------------------- */
    setup();
    if (vibeos_cache_get(1u, 0ull, &a) != 0) { goto fail; }
    if (vibeos_cache_get(2u, 0ull, &b) != 0) { goto fail; }
    vibeos_cache_forget(1u);
    if (vibeos_cache_resident() != 1u) { goto fail; }
    {
        uint32_t before = g_reads;
        if (vibeos_cache_get(2u, 0ull, &c) != 0) { goto fail; }
        if (c != b || g_reads != before) {
            printf("FAIL:backing forgot a file it was not asked about\n");
            goto fail;
        }
        if (vibeos_cache_get(1u, 0ull, &c) != 0) { goto fail; }
        if (g_reads != before + 1u) {
            printf("FAIL:backing kept a file it was told to forget\n");
            goto fail;
        }
    }

    free(g_ram);
    g_ram = 0;
    return 0;

fail:
    free(g_ram);
    g_ram = 0;
    return -1;
}
