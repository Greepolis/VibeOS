/* svc-reclaim: does the machine reclaim, and does it give back what it took?
 *
 * Nothing on a machine had ever reclaimed an anonymous page. The anonymous
 * tier, swap-out and swap-in were host-proved and never once run here: every
 * boot, the nightly soak included, ended with anon_scanned=0, because nothing
 * ever took the machine below its low watermark. svc-press takes 64 MiB of a
 * machine with nearly 400 free and is nowhere near it.
 *
 * So this one goes there on purpose, and only there. It asks the kernel how
 * much is free (sysinfo), fills memory down to the low watermark, and then
 * keeps going for a fixed amount more - enough to use up the clean page cache,
 * which reclaim takes first because it costs nothing, and push into the
 * anonymous tier, which writes pages to swap. It does not go to refusal: past
 * the minimum every other program on the machine is refused too, and a load
 * that starves its neighbours tests them rather than reclaim.
 *
 * Then it reads back every page it wrote. Its own pages are among those the
 * anonymous tier picked, so reading them is swap-in; a page that comes back
 * with somebody else's contents, or the wrong page of its own, is counted.
 *
 * The watermark is computed here as total/64, which is the kernel's policy
 * (hw_pmm_bringup). If that policy moves, this load stops reaching the tier it
 * exists for - and the gate's anon_evicted assertion is what says so.
 */

#include <stdint.h>

#define SYS_write   1
#define SYS_mmap    9
#define SYS_munmap  11
#define SYS_exit    60
#define SYS_sysinfo 99

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

/* One line, one write: a line built from several would interleave with other
 * programs' output, and the gate reads these. */
static void say4(const char *a, uint64_t av, const char *b, uint64_t bv,
                 const char *c, uint64_t cv, const char *d, uint64_t dv) {
    char line[192];
    unsigned at = put_str(line, 0, a);

    at = put_dec(line, at, av);
    if (b) { at = put_str(line, at, b); at = put_dec(line, at, bv); }
    if (c) { at = put_str(line, at, c); at = put_dec(line, at, cv); }
    if (d) { at = put_str(line, at, d); at = put_dec(line, at, dv); }
    line[at++] = '\n';
    (void)sys3(SYS_write, 1, (uint64_t)(uintptr_t)line, (uint64_t)at);
}

static void say(const char *text) {
    unsigned n = 0;
    char line[96];

    n = put_str(line, 0, text);
    line[n++] = '\n';
    (void)sys3(SYS_write, 1, (uint64_t)(uintptr_t)line, (uint64_t)n);
}

/* Linux's struct sysinfo on x86_64, as words: 112 bytes. */
typedef struct {
    uint64_t w[14];
} linux_sysinfo_t;

#define SI_TOTALRAM 4
#define SI_FREERAM  5
#define SI_TOTALSWAP 8
#define SI_FREESWAP 9
#define SI_MEM_UNIT 13   /* low 32 bits */

static int free_and_total(uint64_t *free_pages, uint64_t *total_pages,
                          uint64_t *swap_pages, uint64_t *swap_free) {
    linux_sysinfo_t si;
    uint64_t unit;

    if (sys3(SYS_sysinfo, (uint64_t)(uintptr_t)&si, 0, 0) != 0) {
        return -1;
    }
    unit = si.w[SI_MEM_UNIT] & 0xFFFFFFFFull;
    if (unit == 0u) {
        unit = 1u;
    }
    *free_pages = si.w[SI_FREERAM] * unit / 4096u;
    *total_pages = si.w[SI_TOTALRAM] * unit / 4096u;
    *swap_pages = si.w[SI_TOTALSWAP] * unit / 4096u;
    *swap_free = si.w[SI_FREESWAP] * unit / 4096u;
    return 0;
}

#define BLOCK_PAGES 64u                      /* 256 KiB */
#define BLOCK_BYTES (BLOCK_PAGES * 4096u)
#define MAX_BLOCKS  2048u                    /* 512 MiB, more than the guest */
/* How far past the low mark: at most 12 MiB, and no further once three
 * quarters of swap is in use.
 *
 * The first version went the whole 12 MiB and said, in this comment, that it
 * would never get near the minimum. It did: past the low mark every page it
 * takes has to be found by reclaim, and reclaim can find only what the clean
 * cache and swap hold - a few MiB and 8 MiB. With svc-press beside it the boot
 * happened to have the slack; without it the load reached the minimum, every
 * other program was refused, and the machine starved with swap full. A load
 * that forces reclaim has to stay inside what reclaim can give back, and swap
 * is the part it can see. */
#define PAST_LOW_BLOCKS 48u

static uint8_t *g_block[MAX_BLOCKS];

static uint64_t stamp(uint32_t block, uint32_t page) {
    return 0x5EC1A1A000000000ull | ((uint64_t)block << 8) | (uint64_t)page;
}

int vibeos_main(void) {
    uint64_t free_pages = 0, total_pages = 0, swap_pages = 0, swap_free = 0, low;
    uint32_t got = 0, past = 0, refused = 0, bad = 0, i, p;

    say("RECLAIM_START");
    if (free_and_total(&free_pages, &total_pages, &swap_pages, &swap_free) != 0 ||
        total_pages == 0u) {
        say("RECLAIM_FAIL: sysinfo");
        (void)sys3(SYS_exit, 1, 0, 0);
    }
    low = total_pages / 64u;
    say4("RECLAIM_PLAN total=", total_pages, " free=", free_pages,
         " low=", low, " swap=", swap_pages);
    if (swap_pages == 0u) {
        /* Nowhere to put an anonymous page: the tier this exists for cannot
         * run, and saying so is better than a green run that proved nothing. */
        say("RECLAIM_FAIL: no swap");
        (void)sys3(SYS_exit, 1, 0, 0);
    }

    while (got < MAX_BLOCKS && past < PAST_LOW_BLOCKS) {
        int64_t a = sys6(SYS_mmap, 0, BLOCK_BYTES, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, (uint64_t)-1, 0);

        if (a <= 0) {
            refused++;
            break;      /* refused: the minimum, which this should not reach */
        }
        g_block[got] = (uint8_t *)(uintptr_t)a;
        /* Every page written, twice: the start and the middle, so a page that
         * came back half from somewhere else is caught as well. */
        for (p = 0; p < BLOCK_PAGES; p++) {
            uint64_t *w = (uint64_t *)(g_block[got] + p * 4096u);
            w[0] = stamp(got, p);
            w[256] = ~stamp(got, p);
        }
        got++;
        if (past == 0u && (got % 64u) == 0u) {
            say4("RECLAIM_AT ", got, " free=", free_pages, 0, 0, 0, 0);
        }
        if (free_and_total(&free_pages, &total_pages, &swap_pages, &swap_free) != 0) {
            break;
        }
        if (past > 0u || free_pages <= low) {
            past++;
            if (swap_free < swap_pages / 4u) {
                break;   /* swap nearly full: going on would reach the minimum */
            }
        }
    }
    say4("RECLAIM_FILLED blocks=", got, " past_low=", past,
         " free=", free_pages, " refused=", refused);
    say4("RECLAIM_SWAP total=", swap_pages, " free=", swap_free, 0, 0, 0, 0);

    /* Every page back, in the order it was written: the oldest are the ones
     * the clock most likely took, so this is where swap-in happens.
     *
     * Each block is given back as soon as it has been checked. The first
     * version read everything and then unmapped everything, which asks for a
     * frame per swapped page while holding all the rest - on a machine below
     * its low watermark with swap full, where reclaim can free nothing. It
     * drained slowly and said nothing while it did, and the gate, hearing
     * nothing for 45 seconds, called the machine wedged. Checking and
     * releasing together means every page-in finds the frames the previous
     * blocks gave back; and it says how far it has got. */
    for (i = 0; i < got; i++) {
        for (p = 0; p < BLOCK_PAGES; p++) {
            const uint64_t *w = (const uint64_t *)(g_block[i] + p * 4096u);
            if (w[0] != stamp(i, p) || w[256] != ~stamp(i, p)) {
                bad++;
            }
        }
        (void)sys3(SYS_munmap, (uint64_t)(uintptr_t)g_block[i], BLOCK_BYTES, 0);
        if ((i % 256u) == 255u) {
            say4("RECLAIM_CHECKED ", (uint64_t)i + 1u, " bad=", bad, 0, 0, 0, 0);
        }
    }
    if (bad != 0u) {
        say4("RECLAIM_FAIL: pages whose contents changed = ", bad, 0, 0, 0, 0, 0, 0);
        (void)sys3(SYS_exit, 1, 0, 0);
    }
    say4("RECLAIM_OK blocks=", got, " past_low=", past, 0, 0, 0, 0);
    (void)sys3(SYS_exit, 0, 0, 0);
    return 0;
}
