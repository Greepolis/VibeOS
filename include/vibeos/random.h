#ifndef VIBEOS_RANDOM_H
#define VIBEOS_RANDOM_H

#include <stdint.h>

/* The kernel's random numbers (docs/abi/ L2 step 6): what getrandom and
 * /dev/urandom read.
 *
 * A ChaCha20 key that everything gathered is mixed into, and that output is
 * generated from: each mix replaces the key with a block of ChaCha20 keyed by
 * the old key with the input folded in, and each read takes its bytes from one
 * key and then replaces it - so a key read out of memory later does not give
 * back what was read before it ("fast key erasure").
 *
 * What it is fed, and how much each is believed, is the caller's: RDRAND when
 * the processor has it, and the timestamp at every timer interrupt on every
 * core. The pool is *ready* once 256 bits have been credited, and until then
 * a caller that asked for unpredictable bytes waits. Before this file
 * getrandom answered ENOSYS, for the reason that is still true of anything
 * short of ready: predictable bytes from the call a program uses for keys are
 * worse than none.
 *
 * Portable, host-tested against RFC 8439's own vector, and locked by whoever
 * registers a lock: every core's timer interrupt mixes into it. */

/* ChaCha20's block function (RFC 8439, 2.3): 64 bytes of keystream for a
 * 32-byte key, a block counter and a 12-byte nonce. */
void vibeos_chacha20_block(const uint8_t key[32], uint32_t counter, const uint8_t nonce[12],
                           uint8_t out[64]);

/* Called with no lock registered, a call is counted (VIBEOS_MBZ_RANDOM_UNLOCKED). */
void vibeos_random_set_lock(void (*lock)(void), void (*unlock)(void));

/* Mix `len` bytes in, and believe `bits` of entropy of them. */
void vibeos_random_add(const void *buf, uint32_t len, uint32_t bits);

/* 256 bits credited: what is read now cannot be predicted from outside. */
int vibeos_random_ready(void);

/* `len` bytes, whether or not the pool is ready: waiting is the caller's
 * decision, because the caller knows whether it was asked to. */
void vibeos_random_read(void *buf, uint32_t len);

/* Bits credited so far (saturating), and bytes read. For the boot report. */
uint64_t vibeos_random_credited(void);
uint64_t vibeos_random_bytes_read(void);

/* Back to an empty pool: for tests. */
void vibeos_random_reset(void);

#endif
