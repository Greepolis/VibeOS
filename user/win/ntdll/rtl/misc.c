/* Memory comparison, CRC-32 and the two random number generators
 * (docs/abi/windows.md W2). */

#include "rtl_internal.h"

/* Number of leading bytes of Source that repeat Pattern, in whole ULONGs. A
 * Length that is not a multiple of four is rounded down: the last partial word
 * is never looked at. */
size_t RtlCompareMemoryUlong(const void *src, size_t length, uint32_t pattern) {
    const uint8_t *p = (const uint8_t *)src;
    size_t i, words = length / 4u;

    for (i = 0; i < words; i++) {
        uint32_t w;

        memcpy(&w, p + i * 4u, sizeof(w));
        if (w != pattern) {
            break;
        }
    }
    return i * 4u;
}

/* CRC-32 as zlib's crc32 computes it: the reflected polynomial 0xEDB88320, the
 * register preset to all ones and the result inverted, so that the CRC of a
 * buffer continues from the CRC of its first part. Bit by bit, because a table
 * is 1 KiB of .data that this file would have to initialise or ship. */
uint32_t RtlComputeCrc32(uint32_t initial, const uint8_t *buf, int32_t length) {
    uint32_t crc = ~initial;
    int32_t i;
    int k;

    for (i = 0; i < length; i++) {
        crc ^= buf[i];
        for (k = 0; k < 8; k++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)-(int32_t)(crc & 1u));
        }
    }
    return ~crc;
}

/* The multiplicative congruential generator Microsoft documents for RtlUniform:
 * seed' = (seed * 0x7FFFFFED + 0x7FFFFFC3) mod 0x7FFFFFFF, computed in 64 bits so
 * that the product does not wrap. The seed is replaced and returned. The result
 * is below 0x7FFFFFFF. */
uint32_t RtlUniform(uint32_t *seed) {
    uint64_t v = (uint64_t)*seed * 0x7FFFFFEDull + 0x7FFFFFC3ull;

    *seed = (uint32_t)(v % 0x7FFFFFFFull);
    return *seed;
}

/* RtlRandom improves RtlUniform's low-order randomness by shuffling its output
 * through a table of 128 earlier values (Knuth's algorithm B): the previous
 * result picks the slot, the slot's value is returned and the slot is refilled
 * from RtlUniform. The table is filled from the first seed a process passes, and
 * is process-wide state, as Windows' is.
 *
 * Whether Windows' table is filled the same way is not something this was checked
 * against; the shape is the documented one. */
static int g_rand_ready;
static uint32_t g_rand_last;
static uint32_t g_rand_table[128];

void vibeos_rtl_random_reset(void) {
    g_rand_ready = 0;
}

uint32_t RtlRandom(uint32_t *seed) {
    uint32_t pos;

    if (!g_rand_ready) {
        int i;

        for (i = 0; i < 128; i++) {
            g_rand_table[i] = RtlUniform(seed);
        }
        g_rand_last = RtlUniform(seed);
        g_rand_ready = 1;
    }
    pos = (uint32_t)(((uint64_t)g_rand_last * 128u) / 0x7FFFFFFFull);
    g_rand_last = g_rand_table[pos];
    g_rand_table[pos] = RtlUniform(seed);
    return g_rand_last;
}
