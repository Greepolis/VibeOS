/* Host tests for the kernel's random numbers (kernel/core/random.c, docs/abi/
 * L2 step 6).
 *
 * The block function against RFC 8439's own vector (section 2.3.2) - an
 * artefact neither side of the test wrote, and the bytes OpenSSL's chacha20
 * gives for the same key, nonce and counter. Then the pool: when it says it is
 * ready, that what is mixed in decides what comes out, and that a read never
 * repeats. getrandom and /dev/urandom on top are tested with the handlers
 * (linux_abi_tests.c). */

#include <stdio.h>
#include <string.h>

#include "vibeos/random.h"
#include "vibeos/mbz.h"

int test_random(void);

static int g_fail;

static void expect(int cond, const char *what) {
    if (!cond) {
        printf("FAIL:random %s\n", what);
        g_fail = 1;
    }
}

static void t_lock(void) {}
static void t_unlock(void) {}

static const uint8_t g_rfc_block[64] = {
    0x10, 0xf1, 0xe7, 0xe4, 0xd1, 0x3b, 0x59, 0x15, 0x50, 0x0f, 0xdd, 0x1f, 0xa3, 0x20, 0x71, 0xc4,
    0xc7, 0xd1, 0xf4, 0xc7, 0x33, 0xc0, 0x68, 0x03, 0x04, 0x22, 0xaa, 0x9a, 0xc3, 0xd4, 0x6c, 0x4e,
    0xd2, 0x82, 0x64, 0x46, 0x07, 0x9f, 0xaa, 0x09, 0x14, 0xc2, 0xd7, 0x05, 0xd9, 0x8b, 0x02, 0xa2,
    0xb5, 0x12, 0x9c, 0xd1, 0xde, 0x16, 0x4e, 0xb9, 0xcb, 0xd0, 0x83, 0xe8, 0xa2, 0x50, 0x3c, 0x4e,
};

int test_random(void) {
    uint8_t key[32], nonce[12] = {0, 0, 0, 9, 0, 0, 0, 0x4a, 0, 0, 0, 0}, out[64], a[48], b[48], c[48];
    uint8_t s1[32] = {1}, s2[32] = {2};
    uint32_t i;

    g_fail = 0;
    for (i = 0; i < 32u; i++) {
        key[i] = (uint8_t)i;
    }
    vibeos_chacha20_block(key, 1u, nonce, out);
    expect(memcmp(out, g_rfc_block, 64) == 0, "ChaCha20's block is RFC 8439's test vector");

    vibeos_random_set_lock(t_lock, t_unlock);
    vibeos_random_reset();
    expect(!vibeos_random_ready(), "an empty pool is not ready");
    vibeos_random_add(s1, sizeof(s1), 255u);
    expect(!vibeos_random_ready(), "nor with 255 bits believed");
    vibeos_random_add(s1, 1u, 1u);
    expect(vibeos_random_ready() && vibeos_random_credited() == 256u, "and is with 256");

    /* What is mixed in decides what comes out: the same history gives the
     * same bytes, a different one different bytes. */
    vibeos_random_reset();
    vibeos_random_add(s1, sizeof(s1), 256u);
    vibeos_random_read(a, sizeof(a));
    vibeos_random_reset();
    vibeos_random_add(s1, sizeof(s1), 256u);
    vibeos_random_read(b, sizeof(b));
    vibeos_random_reset();
    vibeos_random_add(s2, sizeof(s2), 256u);
    vibeos_random_read(c, sizeof(c));
    expect(memcmp(a, b, sizeof(a)) == 0 && memcmp(a, c, sizeof(a)) != 0,
           "the output is decided by what was mixed in, all of it");
    vibeos_random_read(b, sizeof(b));
    expect(memcmp(b, c, sizeof(b)) != 0 && vibeos_random_bytes_read() == 2u * sizeof(c),
           "and a read never repeats the one before it");
    {
        uint8_t big[700];
        memset(big, 0, sizeof(big));
        vibeos_random_read(big, sizeof(big));
        expect(memcmp(big, big + 256, 64) != 0 && memcmp(big + 600, "\0\0\0\0\0\0\0\0", 8) != 0,
               "a read longer than one chunk is filled to its end, each chunk its own");
    }

    {
        uint64_t before = vibeos_mbz_count(VIBEOS_MBZ_RANDOM_UNLOCKED);
        vibeos_random_set_lock(0, 0);
        (void)vibeos_random_ready();
        expect(vibeos_mbz_count(VIBEOS_MBZ_RANDOM_UNLOCKED) == before + 1u,
               "a call with no lock registered is counted as random_unlocked");
        vibeos_random_set_lock(t_lock, t_unlock);
    }
    return g_fail ? -1 : 0;
}
