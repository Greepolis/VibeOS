/* Host tests for the pipe module (C5). Each check names the defect it stands for. */

#include <stdio.h>
#include <string.h>

#include "vibeos/mbz.h"
#include "vibeos/pipe.h"

int test_pipe(void);

static int g_locks, g_unlocks, g_held, g_bad_nesting;

static void t_lock(void) {
    g_locks++;
    g_held++;
    if (g_held != 1) {
        g_bad_nesting = 1;
    }
}

static void t_unlock(void) {
    g_unlocks++;
    g_held--;
}

/* A copy that can be told to fail after n calls, standing in for a user address that
 * went away between the check and the copy. */
static int g_fail_after;

static int t_copy(void *dst, const void *src, uint64_t n) {
    if (g_fail_after == 0) {
        return -1;
    }
    if (g_fail_after > 0) {
        g_fail_after--;
    }
    memcpy(dst, src, (size_t)n);
    return 0;
}

static int expect(int cond, const char *what) {
    if (!cond) {
        printf("FAIL:pipe %s\n", what);
    }
    return cond;
}

int test_pipe(void) {
    vibeos_pipe_status_t st;
    static uint8_t out[VIBEOS_PIPE_BYTES + 64];
    static uint8_t in[VIBEOS_PIPE_BYTES + 64];
    uint8_t small[16];
    vibeos_fd_t rd, wr;
    uint64_t base_under, base_bad;
    int a, b, i, slots[VIBEOS_PIPE_MAX];
    long n;

    g_locks = g_unlocks = g_held = g_bad_nesting = 0;
    g_fail_after = -1;
    vibeos_pipe_set_lock(t_lock, t_unlock);
    vibeos_pipe_reset();
    base_under = vibeos_mbz_count(VIBEOS_MBZ_PIPE_END_UNDERFLOW);
    base_bad = vibeos_mbz_count(VIBEOS_MBZ_PIPE_BAD_SLOT);

    /* ---- the table ------------------------------------------------------------- */
    for (i = 0; i < (int)VIBEOS_PIPE_MAX; i++) {
        slots[i] = vibeos_pipe_create();
        if (slots[i] < 0) { printf("FAIL:pipe create failed at %d of %u\n", i, VIBEOS_PIPE_MAX); return -1; }
    }
    if (!expect(vibeos_pipe_create() == -1, "a full table reports -1")) { return -1; }
    if (!expect(vibeos_pipe_in_use() == VIBEOS_PIPE_MAX, "in_use counts every pipe")) { return -1; }
    vibeos_pipe_abandon(slots[3]);
    if (!expect(vibeos_pipe_create() == slots[3], "an abandoned pipe is reusable, and only that one")) { return -1; }
    vibeos_pipe_reset();
    if (!expect(vibeos_pipe_in_use() == 0u, "reset frees everything")) { return -1; }

    a = vibeos_pipe_create();
    if (!expect(vibeos_pipe_readers(a) == 1u && vibeos_pipe_writers(a) == 1u, "a new pipe has one reader and one writer")) { return -1; }

    /* ---- bytes, and the ring's wrap --------------------------------------------- */
    for (i = 0; i < (int)sizeof(out); i++) { out[i] = (uint8_t)(i * 7 + 3); }
    n = vibeos_pipe_write(a, out, 10, t_copy, &st);
    if (!expect(n == 10 && st == VIBEOS_PIPE_OK, "a write of 10 moves 10")) { return -1; }
    n = vibeos_pipe_read(a, in, 4, t_copy, &st);
    if (!expect(n == 4 && memcmp(in, out, 4) == 0, "a short read gets the first bytes in order")) { return -1; }
    n = vibeos_pipe_read(a, in, 100, t_copy, &st);
    if (!expect(n == 6 && memcmp(in, out + 4, 6) == 0, "the rest comes next, and only what is there")) { return -1; }
    n = vibeos_pipe_read(a, in, 100, t_copy, &st);
    if (!expect(n == 0 && st == VIBEOS_PIPE_EMPTY, "empty with a live writer is EMPTY, not end of file")) { return -1; }

    /* fill it to the brim, so head/tail wrap on the next round */
    n = vibeos_pipe_write(a, out, VIBEOS_PIPE_BYTES + 64, t_copy, &st);
    if (!expect(n == (long)VIBEOS_PIPE_BYTES && st == VIBEOS_PIPE_OK, "a write larger than the pipe moves what fits")) { return -1; }
    n = vibeos_pipe_write(a, out, 8, t_copy, &st);
    if (!expect(n == 0 && st == VIBEOS_PIPE_FULL, "a full pipe with a live reader is FULL")) { return -1; }
    n = vibeos_pipe_read(a, in, 100, t_copy, &st);
    if (!expect(n == 100, "draining makes room")) { return -1; }
    n = vibeos_pipe_write(a, out + 200, 100, t_copy, &st);   /* tail wraps past the end */
    if (!expect(n == 100, "a write across the wrap moves all of it")) { return -1; }
    n = vibeos_pipe_read(a, in, VIBEOS_PIPE_BYTES + 64, t_copy, &st);
    if (!expect(n == (long)VIBEOS_PIPE_BYTES, "a full drain returns everything held")) { return -1; }
    if (!expect(memcmp(in, out + 100, VIBEOS_PIPE_BYTES - 100) == 0 &&
                memcmp(in + VIBEOS_PIPE_BYTES - 100, out + 200, 100) == 0,
                "bytes come out in the order they went in across the wrap")) { return -1; }

    /* ---- a fault in the copy ---------------------------------------------------- */
    g_fail_after = 0;
    n = vibeos_pipe_write(a, out, 8, t_copy, &st);
    if (!expect(n == 0 && st == VIBEOS_PIPE_FAULT, "a write whose copy faults reports FAULT and moves nothing")) { return -1; }
    g_fail_after = -1;
    (void)vibeos_pipe_write(a, out, 8, t_copy, &st);
    g_fail_after = 0;
    n = vibeos_pipe_read(a, in, 8, t_copy, &st);
    if (!expect(n == 0 && st == VIBEOS_PIPE_FAULT, "a read whose copy faults reports FAULT")) { return -1; }
    g_fail_after = -1;
    n = vibeos_pipe_read(a, in, 8, t_copy, &st);
    if (!expect(n == 8 && memcmp(in, out, 8) == 0, "the bytes a faulted read did not take are still there (consumed only once copied)")) { return -1; }

    /* ---- ends: end of file and broken pipe ---------------------------------------- */
    (void)vibeos_pipe_write(a, out, 5, t_copy, &st);
    vibeos_pipe_release_end(a, 1);
    n = vibeos_pipe_read(a, in, 100, t_copy, &st);
    if (!expect(n == 5, "a reader drains what was written after every writer has closed")) { return -1; }
    n = vibeos_pipe_read(a, in, 100, t_copy, &st);
    if (!expect(n == 0 && st == VIBEOS_PIPE_EOF, "then empty with no writer is end of file")) { return -1; }

    b = vibeos_pipe_create();
    vibeos_pipe_release_end(b, 0);
    n = vibeos_pipe_write(b, out, 5, t_copy, &st);
    if (!expect(n == 0 && st == VIBEOS_PIPE_NO_READER, "writing with no reader is NO_READER (the caller raises SIGPIPE)")) { return -1; }

    /* ---- the counts: a duplicated end needs a second release --------------------- */
    vibeos_pipe_reset();
    a = vibeos_pipe_create();
    vibeos_pipe_add_end(a, 1);   /* fork gave a child the write end */
    if (!expect(vibeos_pipe_writers(a) == 2u, "an inherited end is counted")) { return -1; }
    vibeos_pipe_release_end(a, 1);
    (void)vibeos_pipe_write(a, out, 3, t_copy, &st);
    n = vibeos_pipe_read(a, in, 100, t_copy, &st);
    if (!expect(n == 3, "one of two writers closing does not end the stream")) { return -1; }
    n = vibeos_pipe_read(a, in, 100, t_copy, &st);
    if (!expect(st == VIBEOS_PIPE_EMPTY, "with a writer left the reader waits instead of seeing end of file")) { return -1; }
    vibeos_pipe_release_end(a, 1);
    n = vibeos_pipe_read(a, in, 100, t_copy, &st);
    if (!expect(st == VIBEOS_PIPE_EOF, "the last writer closing is end of file")) { return -1; }
    vibeos_pipe_release_end(a, 0);
    if (!expect(vibeos_pipe_in_use() == 0u, "a pipe with no ends left is freed")) { return -1; }
    if (!expect(vibeos_mbz_count(VIBEOS_MBZ_PIPE_END_UNDERFLOW) == base_under,
                "balanced acquires and releases record no underflow")) { return -1; }

    /* ---- the descriptor helpers ---------------------------------------------------- */
    a = vibeos_pipe_create();
    memset(&rd, 0, sizeof(rd));
    memset(&wr, 0, sizeof(wr));
    rd.used = 1; rd.pipe = a; rd.writable = 0;
    wr.used = 1; wr.pipe = a; wr.writable = 1;
    vibeos_pipe_end_acquire(&wr);
    if (!expect(vibeos_pipe_writers(a) == 2u, "acquire on a used pipe descriptor adds its end")) { return -1; }
    wr.used = 0;
    vibeos_pipe_end_acquire(&wr);
    if (!expect(vibeos_pipe_writers(a) == 2u, "acquire on an unused descriptor adds nothing")) { return -1; }
    wr.used = 1;
    vibeos_pipe_end_release(&wr);
    if (!expect(wr.pipe == -1 && vibeos_pipe_writers(a) == 1u, "release detaches the descriptor so it cannot release twice")) { return -1; }
    vibeos_pipe_end_release(&wr);
    if (!expect(vibeos_pipe_writers(a) == 1u, "a second release of a detached descriptor changes nothing")) { return -1; }
    vibeos_pipe_end_release(&rd);

    /* ---- a count that goes wrong is counted, not clamped ---------------------------- */
    b = vibeos_pipe_create();
    vibeos_pipe_release_end(b, 0);
    vibeos_pipe_release_end(b, 0);   /* the reader end again: nothing left to release */
    if (!expect(vibeos_mbz_count(VIBEOS_MBZ_PIPE_END_UNDERFLOW) > base_under,
                "releasing an end that is not held is recorded (it used to be clamped in silence)")) { return -1; }
    vibeos_pipe_release_end(b, 1);   /* frees it */
    vibeos_pipe_add_end(b, 1);
    if (!expect(vibeos_mbz_count(VIBEOS_MBZ_PIPE_BAD_SLOT) > base_bad,
                "acquiring an end of a pipe that is free is recorded")) { return -1; }
    (void)vibeos_pipe_read(99, in, 4, t_copy, &st);
    if (!expect(st == VIBEOS_PIPE_FAULT, "reading a slot that does not exist is refused")) { return -1; }
    (void)vibeos_pipe_write(-1, small, 4, t_copy, &st);
    if (!expect(st == VIBEOS_PIPE_FAULT, "writing slot -1 is refused")) { return -1; }

    /* ---- the layer serialises itself, and never nests its lock ---------------------- */
    if (!expect(g_locks > 20 && g_locks == g_unlocks && g_held == 0,
                "every operation took and released the module's own lock")) { return -1; }
    if (!expect(!g_bad_nesting, "the lock was never taken twice (the copy callback runs under it)")) { return -1; }
    vibeos_pipe_set_lock(0, 0);
    return 0;
}
