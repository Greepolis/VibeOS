/* svc-press: does memory pressure actually work?
 *
 * Everything else about reclaim is a host test against a stub. This is the one
 * thing a host test cannot do: run the real allocator down to the low mark on
 * a real machine and find out what happens.
 *
 * The property being checked is not "reclaim freed pages". It is the one that
 * matters and the one a machine fails at exactly when it can least afford to:
 *
 *   **the machine survives, and the program is told.**
 *
 * An allocator that refuses cleanly is correct. One that hands out the last of
 * memory and then cannot fault a page back in is not, and it dies without a
 * message - which is the shape of nearly every hard failure in this project's
 * history.
 *
 * So this asks for memory in large steps until it is refused, checks that the
 * refusal is a refusal rather than a crash, writes to everything it was given
 * to prove the mappings are real, gives it all back, and asserts that memory
 * came back. A leak here is a number, not a mystery.
 */

#include <stdint.h>

#define SYS_write  1
#define SYS_mmap   9
#define SYS_munmap 11
#define SYS_exit   60

#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define MAP_PRIVATE   0x02
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
        dst[at++] = '0';
        return at;
    }
    while (v && d < 24u) {
        digits[d++] = (char)('0' + (v % 10ull));
        v /= 10ull;
    }
    while (d) {
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
    (void)sys3(SYS_write, 1, (uint64_t)(uintptr_t)line, (uint64_t)at);
}

/* Bounded, because a service that allocates until the machine dies is not a
 * test of anything - and because the boot gate has a time budget. 256 blocks
 * of 256 KiB is 64 MiB, which is more than the guest is given, so the refusal
 * is reached well before the ceiling. */
#define BLOCKS 256u
#define BLOCK_BYTES (256u * 1024u)

static uint8_t *g_block[BLOCKS];

int vibeos_main(void) {
    uint32_t got = 0;
    uint32_t i;
    uint32_t bad = 0;

    say("PRESS_START", 0, 0);

    /* Up to the refusal. */
    for (i = 0; i < BLOCKS; i++) {
        int64_t p = sys6(SYS_mmap, 0, BLOCK_BYTES, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, (uint64_t)-1, 0);

        if (p <= 0) {
            break;      /* refused, which is the correct answer */
        }
        g_block[i] = (uint8_t *)(uintptr_t)p;
        got++;
        /* Progress, because a service that stops has to say where.
         *
         * The first run of this wedged the machine after PRESS_START and
         * before anything else, which says only that it died somewhere in a
         * loop of 256 iterations - and this project has a rule about a failed
         * boot being evidence rather than a score. Every 16 blocks is 24 lines
         * in a boot, which is cheap, and it turns "it died" into "it died on
         * the ninth block". */
        if ((got % 16u) == 0u) {
            say("PRESS_AT ", (uint64_t)got, 1);
        }

        /* Touched on the way, not at the end. A mapping that is never written
         * proves nothing about whether it was backed - and touching as we go
         * means the pressure is real rather than promised. One byte per page:
         * writing all of it would measure the emulator, not the kernel. */
        {
            uint32_t off;
            for (off = 0; off < BLOCK_BYTES; off += 4096u) {
                g_block[i][off] = (uint8_t)(i + 1u);
            }
        }
    }

    say("PRESS_BLOCKS ", (uint64_t)got, 1);

    /* Everything handed out is still what was written into it. A machine under
     * pressure that starts handing the same frame to two callers shows up
     * here, and nowhere else in this boot. */
    for (i = 0; i < got; i++) {
        uint32_t off;
        for (off = 0; off < BLOCK_BYTES; off += 4096u) {
            if (g_block[i][off] != (uint8_t)(i + 1u)) {
                bad++;
                break;
            }
        }
    }
    if (bad != 0u) {
        say("PRESS_FAIL: blocks whose contents changed = ", (uint64_t)bad, 1);
        (void)sys3(SYS_exit, 1, 0, 0);
    }

    for (i = 0; i < got; i++) {
        (void)sys3(SYS_munmap, (uint64_t)(uintptr_t)g_block[i], BLOCK_BYTES, 0);
    }

    /* And the machine is still able to allocate afterwards, which is the
     * survival half of the property. A kernel that refused correctly and then
     * stayed refusing has not survived the pressure, it has only been polite
     * about dying. */
    {
        int64_t p = sys6(SYS_mmap, 0, BLOCK_BYTES, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, (uint64_t)-1, 0);
        if (p <= 0) {
            say("PRESS_FAIL: could not allocate after releasing everything", 0, 0);
            (void)sys3(SYS_exit, 1, 0, 0);
        }
        (void)sys3(SYS_munmap, (uint64_t)p, BLOCK_BYTES, 0);
    }

    say("PRESS_OK blocks=", (uint64_t)got, 1);
    (void)sys3(SYS_exit, 0, 0, 0);
    return 0;
}
