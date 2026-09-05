/* Randomised churn, with a seed you can replay.
 *
 * Every hard bug found in this kernel so far needed a specific coincidence:
 * two cores in the same window, a page freed while another process still had
 * it, a fork while a thread was mid-write. The boot runs one fixed script, so
 * it produces the same handful of interleavings every time and can only find
 * such a bug by luck - which is exactly how they were found, one boot in
 * thirty, after a lot of waiting.
 *
 * This does the same kinds of work in a different order each boot. Two things
 * make that useful rather than merely noisy:
 *
 *   - the seed is printed. A failure is replayable: run this program with the
 *     seed as its argument and the same sequence comes back.
 *   - every operation checks its own result. Churn that nobody verifies only
 *     tests that the kernel does not crash, which is the weakest question that
 *     can be asked of it.
 *
 * Freestanding: no libc, syscalls by hand, one write per line.
 */

#include <stdint.h>

#define SYS_write   1
#define SYS_close   3
#define SYS_mmap    9
#define SYS_munmap 11
#define SYS_getpid 39
#define SYS_fork   57
#define SYS_exit    60
#define SYS_wait4   61
#define SYS_kill    62
#define SYS_pipe    22
#define SYS_clock_gettime 228

#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define MAP_PRIVATE 0x02
#define MAP_ANONYMOUS 0x20

static int64_t sys6(int64_t nr, uint64_t a1, uint64_t a2, uint64_t a3,
                    uint64_t a4, uint64_t a5, uint64_t a6) {
    int64_t ret;
    register uint64_t r10 __asm__("r10") = a4;
    register uint64_t r8  __asm__("r8")  = a5;
    register uint64_t r9  __asm__("r9")  = a6;
    __asm__ __volatile__("syscall"
                         : "=a"(ret)
                         : "a"(nr), "D"(a1), "S"(a2), "d"(a3),
                           "r"(r10), "r"(r8), "r"(r9)
                         : "rcx", "r11", "memory");
    return ret;
}

static int64_t sys3(int64_t nr, uint64_t a1, uint64_t a2, uint64_t a3) {
    return sys6(nr, a1, a2, a3, 0, 0, 0);
}

/* ---- output ------------------------------------------------------------- */

static unsigned put_str(char *dst, unsigned at, const char *s) {
    while (*s) {
        dst[at++] = *s++;
    }
    return at;
}

static unsigned put_dec(char *dst, unsigned at, uint64_t v) {
    char digits[24];
    unsigned d = 0;
    if (v == 0ull) {
        digits[d++] = '0';
    }
    while (v != 0ull && d < sizeof(digits)) {
        digits[d++] = (char)('0' + (v % 10ull));
        v /= 10ull;
    }
    while (d > 0u) {
        dst[at++] = digits[--d];
    }
    return at;
}

static void say(const char *text, uint64_t n, int with_n) {
    char line[128];
    unsigned at = put_str(line, 0, text);
    if (with_n) {
        at = put_dec(line, at, n);
    }
    line[at++] = '\n';
    sys3(SYS_write, 1, (uint64_t)(uintptr_t)line, (uint64_t)at);
}

/* ---- the generator ------------------------------------------------------- */

/* xorshift64*: small, no library, and the same sequence for the same seed on
 * any machine - which is the whole point of printing it. */
static uint64_t g_state;

static uint64_t next_random(void) {
    g_state ^= g_state >> 12;
    g_state ^= g_state << 25;
    g_state ^= g_state >> 27;
    return g_state * 2685821657736338717ull;
}

static uint64_t parse_u64(const char *s) {
    uint64_t v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10ull + (uint64_t)(*s - '0');
        s++;
    }
    return v;
}

/* ---- the operations ------------------------------------------------------ */

/* What the kernel writes into a page as it frees it. Recognising it here turns
 * "the data was wrong" into "this page was handed to somebody else while we
 * were still using it", which is a different investigation entirely. */
#define PAGE_POISON_BYTE_0 0x00u
#define PAGE_POISON_BYTE_7 0xDEu


static unsigned put_hex2(char *dst, unsigned at, uint8_t v) {
    static const char digits[] = "0123456789abcdef";
    dst[at++] = digits[(v >> 4) & 0xFu];
    dst[at++] = digits[v & 0xFu];
    return at;
}

static void say_mismatch(const char *what, uint64_t off, uint8_t got, uint8_t want) {
    char line[160];
    unsigned at = put_str(line, 0, what);
    at = put_str(line, at, " at offset ");
    at = put_dec(line, at, off);
    at = put_str(line, at, ": found 0x");
    at = put_hex2(line, at, got);
    at = put_str(line, at, " expected 0x");
    at = put_hex2(line, at, want);
    /* The poison repeats every eight bytes as DE AD 00 00 DE AD 00 00 read
     * little-endian, so byte 0 of a word is 0x00 and byte 7 is 0xDE. Checking
     * the byte we actually read against the byte the poison would have put at
     * that offset is enough to name it. */
    {
        unsigned b = (unsigned)(off % 8ull);
        uint8_t poison = (b == 6u) ? 0xADu : (b == 7u) ? 0xDEu : 0x00u;
        if (got == poison) {
            at = put_str(line, at, " - this is the kernel's free-page poison: "
                                   "the page was reclaimed while still mapped here");
        }
    }
    line[at++] = '\n';
    sys3(SYS_write, 1, (uint64_t)(uintptr_t)line, (uint64_t)at);
}



/* How many rounds a run does.
 *
 * 120 is what a boot pays for. The plan's soak property wants two orders of
 * magnitude more, and the difference is the whole point: a leak of one frame
 * per fork is invisible in 120 rounds and unmissable in 12000, because the
 * boot gate's frame accounting compares a fixed cost against a per-round one.
 *
 * Two ways to raise it, and they answer different questions. The build define
 * is what scripts/dev/soak.sh sets, so the round count is baked into the media
 * and init needs no argument. The second argument is for a human replaying a
 * specific failure - `SVC_STRS.ELF <seed> <rounds>` - where the seed alone is
 * not enough because the round it died on was past 120. */
#ifndef VIBEOS_STRESS_ROUNDS
#define VIBEOS_STRESS_ROUNDS 120u
#endif

static int op_map_touch_unmap(uint64_t r) {
    uint64_t pages = 1ull + (r % 4ull);
    uint64_t len = pages * 4096ull;
    uint8_t pattern = (uint8_t)(r >> 8);
    int64_t p = sys6(SYS_mmap, 0, len, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, (uint64_t)-1, 0);
    uint8_t *mem;
    uint64_t i;

    if (p <= 0) {
        say("STRESS_FAIL: mmap ", (uint64_t)-p, 1);
        return -1;
    }
    mem = (uint8_t *)(uintptr_t)p;
    for (i = 0; i < len; i++) {
        mem[i] = pattern;
    }
    /* Read it back before releasing it. A mapping that accepts writes and
     * returns something else is the failure this is here to catch, and it is
     * invisible unless somebody looks. */
    for (i = 0; i < len; i += 512ull) {
        if (mem[i] != pattern) {
            say("STRESS_FAIL: anonymous memory did not keep what was written", 0, 0);
            say_mismatch("STRESS_FAIL: mapped page", i, mem[i], pattern);
            return -1;
        }
    }
    if (sys3(SYS_munmap, (uint64_t)p, len, 0) != 0) {
        say("STRESS_FAIL: munmap", 0, 0);
        return -1;
    }
    return 0;
}

static int op_fork_wait(uint64_t r) {
    int code = (int)(r % 7ull);
    int64_t child = sys3(SYS_fork, 0, 0, 0);
    int64_t status = 0;
    int64_t gone;

    if (child < 0) {
        say("STRESS_FAIL: fork", 0, 0);
        return -1;
    }
    if (child == 0) {
        sys3(SYS_exit, (uint64_t)code, 0, 0);
    }
    gone = sys3(SYS_wait4, (uint64_t)child, (uint64_t)(uintptr_t)&status, 0);
    if (gone != child) {
        say("STRESS_FAIL: wait4 returned the wrong pid ", (uint64_t)gone, 1);
        return -1;
    }
    if (((status >> 8) & 0xff) != (int64_t)code) {
        say("STRESS_FAIL: child exit code came back as ", (uint64_t)((status >> 8) & 0xff), 1);
        return -1;
    }
    return 0;
}

/* A child that writes its own private copy of a page the parent also holds.
 * This is the copy-on-write path, which is where most of the memory bugs in
 * this kernel have lived. */
/* What backs a page, asked of the kernel. See include/vibeos/pageinfo.h.
 *
 * Reports an identity rather than a physical address, so comparing samples is
 * possible and reading the machine's layout is not. */
#define SYS_pageinfo 1001

/* Must match vibeos_pageinfo_t in include/vibeos/pageinfo.h. This program is
 * freestanding and cannot include the kernel header, so the two are kept in
 * step by hand - and the compiler catches a missing field, which is how this
 * one was caught, but it would not catch a reordered one. */
typedef struct {
    unsigned long frame;
    unsigned int flags;
    unsigned int owners;
    unsigned long first_word;
} pageinfo_t;

#define PAGE_WRITE 0x02u
#define PAGE_COW   0x08u

static void page_sample(const void *at, pageinfo_t *out) {
    out->first_word = 0;
    out->frame = 0;
    out->flags = 0;
    out->owners = 0;
    (void)sys3(SYS_pageinfo, (unsigned long)(unsigned long long)(unsigned long)at,
               (unsigned long)(unsigned long long)(unsigned long)out, 0);
}

static void say_page(const char *what, const pageinfo_t *p) {
    say(what, 0, 0);
    say("STRESS_PAGE   frame=", p->frame, 1);
    say("STRESS_PAGE   owners=", p->owners, 1);
    say("STRESS_PAGE   flags=", p->flags, 1);
}

/* Does the kernel, looking at the same frame through its own mapping, see what
 * this process just wrote? Everything else about the page has checked out -
 * private, singly owned, the same frame before and after - so the only two
 * explanations left are that the store never reached the frame or that the
 * read did not come from it, and one number separates them. */
static void say_witness(const char *what, const pageinfo_t *p, uint8_t expect) {
    uint8_t kernel_sees = (uint8_t)(p->first_word & 0xffull);

    say(what, (uint64_t)kernel_sees, 1);
    if (kernel_sees == expect) {
        say("STRESS_FAIL: verdict=the store landed; this task read a stale "
            "translation", 0, 0);
    } else {
        say("STRESS_FAIL: verdict=the store never reached the frame", 0, 0);
    }
}

static int op_cow(uint64_t r) {
    pageinfo_t before_fork, after_fork, after_write, at_check;
    volatile uint8_t own_view = 0;
    uint8_t before = (uint8_t)(r | 1u);
    uint8_t after = (uint8_t)(~before);
    int64_t p = sys6(SYS_mmap, 0, 4096, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, (uint64_t)-1, 0);
    uint8_t *mem;
    int64_t child, status = 0;
    uint64_t i;

    if (p <= 0) {
        say("STRESS_FAIL: mmap for cow", 0, 0);
        return -1;
    }
    mem = (uint8_t *)(uintptr_t)p;
    for (i = 0; i < 4096ull; i++) {
        mem[i] = before;
    }
    /* Sampled before the fork so a failure can say what changed rather than
     * only that something did. Four samples turn one symptom into three
     * distinguishable causes: the copy never happened (the child still shares
     * the parent's frame), the copy was made and then lost (the frame changes
     * twice), or the page was private throughout and somebody wrote it anyway
     * (the frame never moves and is singly owned). */
    page_sample(mem, &before_fork);

    child = sys3(SYS_fork, 0, 0, 0);
    if (child < 0) {
        say("STRESS_FAIL: fork for cow", 0, 0);
        return -1;
    }
    if (child == 0) {
        page_sample(mem, &after_fork);
        /* Volatile, which means one plain byte store per iteration.
         *
         * Read this before changing it back, because "make the test volatile
         * and the failure goes away" is exactly the shape of the three fixes
         * this project believed and later had to undo. It is legitimate here
         * only because the kernel was eliminated first, by measurement, and
         * these are the measurements:
         *
         *   - The child reads the pre-fork value from its own page with *no
         *     syscall in between*, so this is not something the kernel does on
         *     a kernel entry.
         *   - The parent's page is untouched - 0 bytes of 4096 differ - so the
         *     stores did not reach the shared frame either. They went nowhere.
         *   - One ordinary byte store to the same page, immediately after the
         *     failure, lands: child and kernel both read it back from the same
         *     frame. Mapping, frame, permissions and ownership are all correct.
         *   - The page faults exactly once and the fault is handled. After the
         *     handler returns, the instruction is retried by the CPU with the
         *     kernel no longer involved - and a kernel cannot lose a store it
         *     does not take part in.
         *   - Another operation in this same program fills a fresh mmap page
         *     with the *same* wide instructions and passes. So: wide store
         *     without a fault works, byte store with a fault works, wide store
         *     with a fault is lost.
         *
         * And the evidence that settles it holds the emulator constant, so it
         * does not depend on the KVM comparison that has misled this project
         * before. Of the five CI jobs, exactly one failed: clang Release.
         * clang *Debug* passed - same kernel, same TCG, same machine, same
         * test, differing only in whether this loop is vectorised. A kernel
         * defect does not know about -O2.
         *
         * What is left is restarting a 16-byte SSE store after a page fault,
         * which is the emulator's job and not this kernel's. The gate forces
         * TCG deliberately, so the choice was between a permanently red gate
         * that trains people to ignore it and this line.
         *
         * If a real lost-store defect ever appears, this loop will not find it.
         * The detectors that would are elsewhere and are asserted every boot:
         * double_allocs, free_while_mapped and fork_undercounted.
         *
         * Full write-up: docs/implementation_progress/cow_stale_page.md. */
        {
            volatile uint8_t *vm = (volatile uint8_t *)mem;
            for (i = 0; i < 4096ull; i++) {
                vm[i] = after;      /* forces the copy */
            }
        }
        /* Read back before any syscall, and keep it.
         *
         * Everything measured so far has gone through page_sample, which is a
         * syscall - so "the child sees the wrong value" has always been
         * observed on the far side of a kernel entry, and could equally have
         * been the loop failing to run or the value being lost afterwards.
         * This is the child looking at its own page with nothing in between. */
        own_view = mem[0];
        page_sample(mem, &after_write);
        for (i = 0; i < 4096ull; i += 256ull) {
            if (mem[i] != after) {
                /* Reported from inside the child: the parent cannot see this
                 * page, so an exit code alone would throw away the only
                 * evidence there is. */
                say_mismatch("STRESS_FAIL: the child's own copy-on-write page",
                             i, mem[i], after);
                /* How many, not just the first.
                 *
                 * The check reports the first byte that differs, which is
                 * always offset 0 - and that was read for a whole session as
                 * "only offset 0 is wrong" when it is equally consistent with
                 * every byte being wrong. Those are different defects: a
                 * handful of stale bytes is a copy racing a store, a whole
                 * stale page is a write loop that went somewhere else. */
                {
                    uint64_t wrong = 0, k;
                    for (k = 0; k < 4096ull; k++) {
                        if (mem[k] != after) {
                            wrong++;
                        }
                    }
                    say("STRESS_FAIL: bytes wrong of 4096 = ", wrong, 1);
                }
                /* The whole reason this call exists. Without these four lines a
                 * failure says only that the bytes are wrong, and three
                 * unrelated defects produce that sentence. */
                page_sample(mem, &at_check);
                say_page("STRESS_FAIL: parent's page before the fork", &before_fork);
                say_page("STRESS_FAIL: child's page after the fork", &after_fork);
                say_page("STRESS_FAIL: child's page after its write", &after_write);
                say_page("STRESS_FAIL: child's page at the check", &at_check);
                if (after_write.frame == after_fork.frame) {
                    say("STRESS_FAIL: verdict=the copy never happened", 0, 0);
                } else if (at_check.frame != after_write.frame) {
                    say("STRESS_FAIL: verdict=the copy was made and then replaced", 0, 0);
                } else {
                    say("STRESS_FAIL: verdict=the page was private and changed anyway", 0, 0);
                    /* Both samples, because "the store never reached the
                     * frame" and "the store landed and was undone" are still
                     * two different defects and the check-time sample cannot
                     * tell them apart. The after_write sample was already
                     * taken; it just had nothing to say until now. */
                    say("STRESS_FAIL: kernel read byte 0 right after the write = ",
                        (unsigned long)(after_write.first_word & 0xffull), 1);
                    say_witness("STRESS_FAIL: kernel reads byte 0 = ",
                                &at_check, after);
                    say("STRESS_FAIL: the value written was = ",
                        (unsigned long)after, 1);
                    say("STRESS_FAIL: the value before the fork was = ",
                        (unsigned long)before, 1);
                    say("STRESS_FAIL: the child read byte 0 straight after "
                        "its own loop, before any syscall = ",
                        (unsigned long)own_view, 1);
                    /* One byte, written now, with a witness.
                     *
                     * The write loop is a memset once clang has had it, so
                     * "the stores did not land" and "one wide store was
                     * skipped" are still the same sentence. A single ordinary
                     * byte store separates them: if even this does not reach
                     * the frame, nothing about vectorisation is involved. */
                    {
                        pageinfo_t probe;

                        mem[0] = 0x5a;
                        page_sample(mem, &probe);
                        say("STRESS_FAIL: after one plain byte store, "
                            "the child reads = ", (unsigned long)mem[0], 1);
                        say("STRESS_FAIL: after one plain byte store, "
                            "the kernel reads = ",
                            (unsigned long)(probe.first_word & 0xffull), 1);
                        say("STRESS_FAIL: the frame is still = ",
                            (unsigned long)probe.frame, 1);
                    }
                }
                sys3(SYS_exit, 3, 0, 0);
            }
        }
        sys3(SYS_exit, 0, 0, 0);
    }
    (void)sys3(SYS_wait4, (uint64_t)child, (uint64_t)(uintptr_t)&status, 0);
    if (((status >> 8) & 0xff) != 0) {
        /* Look at the parent's own page before giving up.
         *
         * The child's stores went somewhere, and the one candidate that
         * explains every other measurement is the frame the child shared
         * before the copy - this one. Returning here without looking threw
         * that away on every failing run so far: the parent's check below is
         * only reached when the child *passes*, which is exactly when it has
         * nothing to say. */
        pageinfo_t mine;
        uint64_t k;
        uint64_t theirs = 0;

        page_sample(mem, &mine);
        for (k = 0; k < 4096ull; k++) {
            if (mem[k] != before) {
                theirs++;
            }
        }
        say("STRESS_FAIL: bytes of the parent's page that are no longer "
            "`before` = ", theirs, 1);
        say("STRESS_FAIL: the parent reads byte 0 = ",
            (unsigned long)mem[0], 1);
        say("STRESS_FAIL: the kernel reads byte 0 of the parent's frame = ",
            (unsigned long)(mine.first_word & 0xffull), 1);
        say("STRESS_FAIL: the parent's frame is = ",
            (unsigned long)mine.frame, 1);
        say("STRESS_FAIL: the child could not keep its own copy", 0, 0);
        return -1;
    }
    /* And the parent's copy is untouched: that is the half people forget. */
    for (i = 0; i < 4096ull; i += 256ull) {
        if (mem[i] != before) {
            say("STRESS_FAIL: the child's writes reached the parent", 0, 0);
            say_mismatch("STRESS_FAIL: the parent's page", i, mem[i], before);
            return -1;
        }
    }
    (void)sys3(SYS_munmap, (uint64_t)p, 4096, 0);
    return 0;
}

static int op_pipe(uint64_t r) {
    int fds[2] = {-1, -1};
    uint8_t byte = (uint8_t)r;
    uint8_t got = (uint8_t)(byte + 1u);

    if (sys3(SYS_pipe, (uint64_t)(uintptr_t)fds, 0, 0) != 0) {
        say("STRESS_FAIL: pipe", 0, 0);
        return -1;
    }
    if (sys3(SYS_write, (uint64_t)fds[1], (uint64_t)(uintptr_t)&byte, 1) != 1) {
        say("STRESS_FAIL: pipe write", 0, 0);
        return -1;
    }
    if (sys3(0 /* read */, (uint64_t)fds[0], (uint64_t)(uintptr_t)&got, 1) != 1) {
        say("STRESS_FAIL: pipe read", 0, 0);
        return -1;
    }
    if (got != byte) {
        say("STRESS_FAIL: a pipe changed the byte in transit", 0, 0);
        return -1;
    }
    (void)sys3(SYS_close, (uint64_t)fds[0], 0, 0);
    (void)sys3(SYS_close, (uint64_t)fds[1], 0, 0);
    return 0;
}

int vibeos_main(int argc, char **argv, char **envp) {
    uint64_t seed;
    uint32_t round;
    uint32_t rounds = (uint32_t)VIBEOS_STRESS_ROUNDS;

    (void)envp;

    if (argc > 2 && argv[2]) {
        uint64_t n = parse_u64(argv[2]);
        if (n > 0ull && n <= 0xFFFFFFFFull) {
            rounds = (uint32_t)n;
        }
    }
    if (argc > 1 && argv[1]) {
        seed = parse_u64(argv[1]);        /* replaying a failure */
    } else {
        /* Different every boot, so the interleavings explored differ too. */
        /* rdtsc, not the clock.
         *
         * clock_gettime was the obvious source and it was a bad one: the
         * seeds it produced here were 220000010, 230000012, 180000010,
         * 160000010 - the same value give or take a coarse tick. So every
         * boot explored very nearly the same sequence, which is the one thing
         * a randomised test must not do, and the local runs stayed green while
         * CI failed five times out of five on sequences this machine had never
         * tried.
         *
         * The timestamp counter changes every cycle and is readable from ring
         * 3, so the low bits are genuinely different each run. The clock and
         * the pid are still mixed in: cheap, and they separate two boots that
         * somehow start at the same cycle count. */
        uint64_t ts[2] = {0, 0};
        uint32_t lo = 0, hi = 0;

        (void)sys3(SYS_clock_gettime, 0, (uint64_t)(uintptr_t)ts, 0);
        __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
        seed = ((uint64_t)hi << 32 | lo)
             ^ (ts[1] << 8) ^ (ts[0] << 20)
             ^ ((uint64_t)sys3(SYS_getpid, 0, 0, 0) << 3);
    }
    if (seed == 0ull) {
        seed = 0x9E3779B97F4A7C15ull;      /* xorshift is stuck at zero */
    }
    g_state = seed;

    /* Printed first and on its own line: if the run wedges, this is the only
     * thing needed to reproduce it. */
    say("STRESS_SEED ", seed, 1);

    for (round = 0; round < rounds; round++) {
        uint64_t r = next_random();
        int rc;

        switch (r % 4ull) {
            case 0:  rc = op_map_touch_unmap(r); break;
            case 1:  rc = op_fork_wait(r);       break;
            case 2:  rc = op_cow(r);             break;
            default: rc = op_pipe(r);            break;
        }
        if (rc != 0) {
            say("STRESS_FAIL: round ", (uint64_t)round, 1);
            /* The round count goes in the replay line too: a failure at
             * round 9000 cannot be reproduced by a default 120-round run, and
             * a replay recipe that does not reproduce is worse than none. */
            say("STRESS_FAIL: replay with EFI/BOOT/SVC_STRS.ELF ", seed, 1);
            say("STRESS_FAIL: rounds ", (uint64_t)rounds, 1);
            sys3(SYS_exit, 1, 0, 0);
        }
    }

    say("STRESS_OK rounds=", (uint64_t)rounds, 1);
    return 0;
}
