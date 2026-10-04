/* The kernel's random numbers. See include/vibeos/random.h. */

#include "vibeos/random.h"
#include "vibeos/mbz.h"

#define RANDOM_READY_BITS 256u
/* Bytes generated per hold of the lock. Every core's timer interrupt mixes in
 * under the same lock, and a spinlock here masks interrupts: a read of a
 * megabyte is many short holds, not one long one. */
#define RANDOM_CHUNK 256u

static uint8_t g_key[32];
static uint64_t g_generation;   /* the nonce: never the same key and nonce twice */
static uint64_t g_credited;
static uint64_t g_read;
static void (*g_lock)(void);
static void (*g_unlock)(void);

static void lock(void) {
    if (g_lock) {
        g_lock();
    } else {
        vibeos_mbz_hit(VIBEOS_MBZ_RANDOM_UNLOCKED, 0);
    }
}

static void unlock(void) {
    if (g_unlock) {
        g_unlock();
    }
}

void vibeos_random_set_lock(void (*l)(void), void (*u)(void)) {
    g_lock = l;
    g_unlock = u;
}

/* ---- ChaCha20 (RFC 8439) ------------------------------------------------------------ */

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

#define ROTL32(v, n) (((v) << (n)) | ((v) >> (32 - (n))))
#define QR(a, b, c, d)                                   \
    do {                                                 \
        a += b; d ^= a; d = ROTL32(d, 16);               \
        c += d; b ^= c; b = ROTL32(b, 12);               \
        a += b; d ^= a; d = ROTL32(d, 8);                \
        c += d; b ^= c; b = ROTL32(b, 7);                \
    } while (0)

void vibeos_chacha20_block(const uint8_t key[32], uint32_t counter, const uint8_t nonce[12],
                           uint8_t out[64]) {
    uint32_t in[16], x[16];
    uint32_t i;

    in[0] = 0x61707865u;   /* "expand 32-byte k" */
    in[1] = 0x3320646eu;
    in[2] = 0x79622d32u;
    in[3] = 0x6b206574u;
    for (i = 0; i < 8u; i++) {
        in[4u + i] = le32(key + 4u * i);
    }
    in[12] = counter;
    for (i = 0; i < 3u; i++) {
        in[13u + i] = le32(nonce + 4u * i);
    }
    for (i = 0; i < 16u; i++) {
        x[i] = in[i];
    }
    for (i = 0; i < 10u; i++) {
        QR(x[0], x[4], x[8], x[12]);
        QR(x[1], x[5], x[9], x[13]);
        QR(x[2], x[6], x[10], x[14]);
        QR(x[3], x[7], x[11], x[15]);
        QR(x[0], x[5], x[10], x[15]);
        QR(x[1], x[6], x[11], x[12]);
        QR(x[2], x[7], x[8], x[13]);
        QR(x[3], x[4], x[9], x[14]);
    }
    for (i = 0; i < 16u; i++) {
        uint32_t v = x[i] + in[i];
        out[4u * i] = (uint8_t)v;
        out[4u * i + 1u] = (uint8_t)(v >> 8);
        out[4u * i + 2u] = (uint8_t)(v >> 16);
        out[4u * i + 3u] = (uint8_t)(v >> 24);
    }
    for (i = 0; i < 16u; i++) {
        ((volatile uint32_t *)x)[i] = 0;
    }
}

/* ---- the pool ------------------------------------------------------------------------ */

static void wipe(void *p, uint32_t n) {
    volatile uint8_t *v = (volatile uint8_t *)p;
    while (n--) {
        *v++ = 0;
    }
}

static void nonce_of(uint8_t nonce[12], uint32_t purpose) {
    uint32_t i;

    for (i = 0; i < 8u; i++) {
        nonce[i] = (uint8_t)(g_generation >> (8u * i));
    }
    for (i = 0; i < 4u; i++) {
        nonce[8u + i] = (uint8_t)(purpose >> (8u * i));
    }
    g_generation++;
}

/* Under the lock: a new key from the old one with `in` folded in. The fold is
 * an exclusive or into the key, and then the key is put through ChaCha20 - so
 * somebody who knows the input and not the key learns nothing, and somebody who
 * knew the key and not the input is as uncertain as the input was. */
static void mix_locked(const uint8_t *in, uint32_t n) {
    uint8_t nonce[12], block[64];
    uint32_t i;

    for (i = 0; i < n; i++) {
        g_key[i % 32u] ^= in[i];
    }
    nonce_of(nonce, 0x78696du);   /* "mix" */
    vibeos_chacha20_block(g_key, 0, nonce, block);
    for (i = 0; i < 32u; i++) {
        g_key[i] = block[i];
    }
    wipe(block, sizeof(block));
}

void vibeos_random_add(const void *buf, uint32_t len, uint32_t bits) {
    const uint8_t *p = (const uint8_t *)buf;

    lock();
    while (len > 0u) {
        uint32_t n = len > 32u ? 32u : len;
        mix_locked(p, n);
        p += n;
        len -= n;
    }
    g_credited = (g_credited + bits < g_credited) ? ~0ull : g_credited + bits;
    unlock();
}

int vibeos_random_ready(void) {
    int r;

    lock();
    r = g_credited >= RANDOM_READY_BITS;
    unlock();
    return r;
}

/* Blocks 1.. of the current key are the output, then block 0 becomes the next
 * key: once this returns, nothing in memory reproduces what it handed out. */
void vibeos_random_read(void *buf, uint32_t len) {
    uint8_t *out = (uint8_t *)buf;

    while (len > 0u) {
        uint8_t nonce[12], block[64];
        uint32_t n = len > RANDOM_CHUNK ? RANDOM_CHUNK : len;
        uint32_t done = 0, ctr = 1, i;

        lock();
        nonce_of(nonce, 0x64616572u);   /* "read" */
        while (done < n) {
            uint32_t take = n - done > 64u ? 64u : n - done;
            vibeos_chacha20_block(g_key, ctr++, nonce, block);
            for (i = 0; i < take; i++) {
                out[done + i] = block[i];
            }
            done += take;
        }
        vibeos_chacha20_block(g_key, 0, nonce, block);
        for (i = 0; i < 32u; i++) {
            g_key[i] = block[i];
        }
        g_read += n;
        unlock();
        wipe(block, sizeof(block));
        out += n;
        len -= n;
    }
}

uint64_t vibeos_random_credited(void) {
    uint64_t v;

    lock();
    v = g_credited;
    unlock();
    return v;
}

uint64_t vibeos_random_bytes_read(void) {
    uint64_t v;

    lock();
    v = g_read;
    unlock();
    return v;
}

void vibeos_random_reset(void) {
    lock();
    wipe(g_key, sizeof(g_key));
    g_generation = 0;
    g_credited = 0;
    g_read = 0;
    unlock();
}
