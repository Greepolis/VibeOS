/* Host tests for the file-lock table (kernel/fs/filelock.c, docs/abi/ L1 step 6).
 *
 * The table is asked directly, with owners that are just numbers: what a
 * process or a description is belongs to the caller. The Linux handlers on top
 * of it - fcntl, flock - are tested where the other handlers are
 * (linux_abi_tests.c); the torture (filelock_torture.c) checks it against a
 * model over many random sequences. */

#include <stdio.h>
#include <string.h>

#include "vibeos/filelock.h"
#include "vibeos/abi_linux.h"
#include "vibeos/mbz.h"

int test_filelock(void);

static int g_fail;
static int g_locked;

static void expect(int cond, const char *what) {
    if (!cond) {
        printf("FAIL:filelock %s\n", what);
        g_fail = 1;
    }
}

static void t_lock(void) {
    g_locked++;
}

static void t_unlock(void) {
    g_locked--;
}

#define REC VIBEOS_FLK_RECORD
#define SH VIBEOS_FLK_SHARED
#define EX VIBEOS_FLK_EXCL
#define UN VIBEOS_FLK_UNLOCK
#define A 0x11ull
#define B 0x22ull
#define C 0x33ull

static int fsa, fsb;   /* two filesystems: their addresses are their names */

static int set(uint64_t owner, uint32_t type, uint64_t s, uint64_t e) {
    return vibeos_flk_set(REC, &fsa, 7, owner, (uint32_t)owner, type, s, e, 0);
}

static int held(uint64_t asker, uint32_t type, uint64_t s, uint64_t e) {
    return vibeos_flk_test(REC, &fsa, 7, asker, type, s, e, 0);
}

int test_filelock(void) {
    vibeos_flk_info_t info;
    uint64_t blocker = 0, before;
    uint32_t i;

    g_fail = 0;
    vibeos_flk_set_lock(t_lock, t_unlock);
    vibeos_flk_reset();

    /* Shared with shared, exclusive with nothing. */
    expect(set(A, SH, 0, 99) == 0 && set(B, SH, 50, 149) == 0, "two owners share a range");
    expect(vibeos_flk_set(REC, &fsa, 7, C, 3, EX, 99, 99, &blocker) == -VIBEOS_EAGAIN &&
           (blocker == A || blocker == B), "an exclusive lock on a shared byte is refused, naming a holder");
    expect(set(C, EX, 150, 199) == 0, "and granted beside them");
    expect(set(A, SH, 150, 150) == -VIBEOS_EAGAIN, "a shared lock on an exclusive byte is refused");
    expect(vibeos_flk_test(REC, &fsa, 7, A, SH, 0, VIBEOS_FLK_END, &info) == 1 && info.type == EX &&
           info.start == 150u && info.end == 199u && info.owner == C && info.pid == (uint32_t)C,
           "the lock in the way is reported whole");
    expect(held(C, EX, 150, 199) == 0, "an owner's own lock is never in its way");
    /* Taken at 150 first and at 0 after: the one in the way of "everything"
     * is still the one that starts first. */
    expect(vibeos_flk_test(REC, &fsa, 7, C, EX, 0, VIBEOS_FLK_END, &info) == 1 && info.start == 0u &&
           info.type == SH && info.owner == A,
           "of several locks in the way, the one reported is the first by position");
    expect(set(B, SH, 10, 5) == -VIBEOS_EINVAL, "a range that ends before it starts is EINVAL");
    vibeos_flk_reset();

    /* An owner asking again replaces, splits and joins. */
    expect(set(A, EX, 0, 99) == 0 && vibeos_flk_count() == 1u, "one lock");
    expect(set(A, UN, 40, 59) == 0 && vibeos_flk_count() == 2u, "unlocking the middle leaves two");
    expect(held(B, EX, 40, 59) == 0 && held(B, SH, 39, 39) == 1 && held(B, SH, 60, 60) == 1,
           "exactly the middle is free");
    expect(set(A, EX, 40, 59) == 0 && vibeos_flk_count() == 1u, "locking it again joins all three");
    expect(set(A, SH, 10, 19) == 0 && vibeos_flk_count() == 3u, "a change of type in the middle leaves three");
    expect(held(B, SH, 10, 19) == 0 && held(B, SH, 9, 9) == 1 && held(B, SH, 20, 20) == 1,
           "and exactly that part is shared");
    expect(set(A, UN, 0, VIBEOS_FLK_END) == 0 && vibeos_flk_count() == 0u, "unlocking everything leaves none");
    expect(set(A, SH, 0, 9) == 0 && set(A, SH, 10, 19) == 0 && set(A, SH, 30, 39) == 0 &&
           vibeos_flk_count() == 2u, "touching locks of one type are one; a gap keeps them two");
    expect(set(A, SH, 20, 29) == 0 && vibeos_flk_count() == 1u, "filling the gap joins both sides at once");
    expect(set(A, EX, 40, 49) == 0 && vibeos_flk_count() == 2u, "a different type beside it is not joined");
    expect(set(A, UN, 500, 600) == 0 && vibeos_flk_count() == 2u, "unlocking what was never locked is nothing");
    vibeos_flk_reset();

    /* To the end of the file, wherever that goes. */
    expect(set(A, EX, 100, VIBEOS_FLK_END) == 0 && held(B, SH, ~0ull - 1u, ~0ull - 1u) == 1 &&
           held(B, SH, 0, 99) == 0, "a lock to the end covers every byte after its start");
    expect(set(A, EX, 0, 99) == 0 && vibeos_flk_count() == 1u, "and joins what comes before it");
    expect(set(A, UN, 10, VIBEOS_FLK_END) == 0 && held(B, EX, 10, VIBEOS_FLK_END) == 0 &&
           held(B, SH, 9, 9) == 1, "unlocking to the end cuts it");
    vibeos_flk_reset();

    /* Files, filesystems and the two kinds do not see each other. */
    expect(set(A, EX, 0, 9) == 0 &&
           vibeos_flk_set(REC, &fsa, 8, B, 2, EX, 0, 9, 0) == 0 &&
           vibeos_flk_set(REC, &fsb, 7, B, 2, EX, 0, 9, 0) == 0 &&
           vibeos_flk_set(VIBEOS_FLK_WHOLE, &fsa, 7, B, 2, EX, 0, VIBEOS_FLK_END, 0) == 0,
           "another file, another filesystem and the other kind of lock are all free");
    expect(vibeos_flk_set(VIBEOS_FLK_WHOLE, &fsa, 7, C, 3, SH, 0, VIBEOS_FLK_END, 0) == -VIBEOS_EAGAIN,
           "and a whole-file lock conflicts with a whole-file lock");
    vibeos_flk_drop_file(B, &fsa, 7);
    expect(vibeos_flk_count() == 3u &&
           vibeos_flk_set(VIBEOS_FLK_WHOLE, &fsa, 7, C, 3, SH, 0, VIBEOS_FLK_END, 0) == 0,
           "dropping an owner's locks on one file leaves its others");
    vibeos_flk_drop_owner(B);
    expect(vibeos_flk_count() == 2u && held(B, EX, 0, 9) == 1, "dropping an owner leaves everyone else's");
    vibeos_flk_reset();

    /* A full table refuses before it changes anything. */
    for (i = 0; i < VIBEOS_FLK_MAX - 1u; i++) {
        (void)set(A, (i & 1u) ? SH : EX, (uint64_t)i * 10u, (uint64_t)i * 10u + 4u);
    }
    expect(vibeos_flk_count() == VIBEOS_FLK_MAX - 1u, "the table one short of full");
    expect(set(A, UN, 2, 2) == -VIBEOS_ENOLCK && vibeos_flk_count() == VIBEOS_FLK_MAX - 1u &&
           held(B, SH, 2, 2) == 1, "a split with no room for both pieces is ENOLCK, and nothing was cut");
    expect(set(B, SH, 7, 7) == -VIBEOS_ENOLCK, "and so is a new lock");
    vibeos_flk_reset();

    /* A wait that cannot end. */
    expect(vibeos_flk_wait(A, B) == 0 && vibeos_flk_wait(B, C) == 0, "A waits for B, B for C");
    expect(vibeos_flk_wait(C, A) == -VIBEOS_EDEADLK, "C waiting for A would close the circle");
    expect(vibeos_flk_wait(C, 0x44ull) == 0, "C waiting for somebody else is fine");
    vibeos_flk_wait_done(B);
    expect(vibeos_flk_wait(0x44ull, A) == 0, "and with B no longer waiting there is no circle through it");
    vibeos_flk_reset();
    expect(vibeos_flk_wait(A, B) == 0, "A waits for B again");
    vibeos_flk_drop_owner(A);
    expect(vibeos_flk_wait(B, A) == 0, "an owner that is gone is waiting for nobody");
    expect(vibeos_flk_wait(A, A) == -VIBEOS_EDEADLK, "an owner cannot wait for itself");
    vibeos_flk_reset();

    expect(g_locked == 0, "every call released the table's lock");

    /* With no lock registered the table still answers, and says so: the
     * must-be-zero, hit on purpose. */
    before = vibeos_mbz_count(VIBEOS_MBZ_FILELOCK_UNLOCKED);
    vibeos_flk_set_lock(0, 0);
    (void)set(A, SH, 0, 0);
    expect(vibeos_mbz_count(VIBEOS_MBZ_FILELOCK_UNLOCKED) == before + 1u,
           "a call with no lock registered is counted as filelock_unlocked");
    vibeos_flk_set_lock(t_lock, t_unlock);
    vibeos_flk_reset();
    return g_fail ? -1 : 0;
}
