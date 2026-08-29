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

#define ROUNDS 120u

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
            say("STRESS_FAIL: anonymous memory did not keep what was written at ", i, 1);
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
static int op_cow(uint64_t r) {
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
    child = sys3(SYS_fork, 0, 0, 0);
    if (child < 0) {
        say("STRESS_FAIL: fork for cow", 0, 0);
        return -1;
    }
    if (child == 0) {
        for (i = 0; i < 4096ull; i++) {
            mem[i] = after;      /* forces the copy */
        }
        for (i = 0; i < 4096ull; i += 256ull) {
            if (mem[i] != after) {
                sys3(SYS_exit, 3, 0, 0);
            }
        }
        sys3(SYS_exit, 0, 0, 0);
    }
    (void)sys3(SYS_wait4, (uint64_t)child, (uint64_t)(uintptr_t)&status, 0);
    if (((status >> 8) & 0xff) != 0) {
        say("STRESS_FAIL: the child could not keep its own copy", 0, 0);
        return -1;
    }
    /* And the parent's copy is untouched: that is the half people forget. */
    for (i = 0; i < 4096ull; i += 256ull) {
        if (mem[i] != before) {
            say("STRESS_FAIL: the child's writes reached the parent at ", i, 1);
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

    (void)envp;

    if (argc > 1 && argv[1]) {
        seed = parse_u64(argv[1]);        /* replaying a failure */
    } else {
        /* Different every boot, so the interleavings explored differ too. */
        uint64_t ts[2] = {0, 0};
        (void)sys3(SYS_clock_gettime, 0, (uint64_t)(uintptr_t)ts, 0);
        seed = ts[1] ^ (ts[0] << 20) ^ (uint64_t)sys3(SYS_getpid, 0, 0, 0);
    }
    if (seed == 0ull) {
        seed = 0x9E3779B97F4A7C15ull;      /* xorshift is stuck at zero */
    }
    g_state = seed;

    /* Printed first and on its own line: if the run wedges, this is the only
     * thing needed to reproduce it. */
    say("STRESS_SEED ", seed, 1);

    for (round = 0; round < ROUNDS; round++) {
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
            say("STRESS_FAIL: replay with EFI/BOOT/SVC_STRS.ELF ", seed, 1);
            sys3(SYS_exit, 1, 0, 0);
        }
    }

    say("STRESS_OK rounds=", (uint64_t)ROUNDS, 1);
    return 0;
}
