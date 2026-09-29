/* Linux ABI: what several of the handlers share.
 *
 * Lifted out of arch_hw.c (C4 stage 3). The per-process address-space lock that
 * used to sit here went back to the architecture in A2 as ks_mm_lock: it opens
 * an interrupt window while it spins, which is a statement about the machine. */

#include "linux_internal.h"

/* Which image the staging buffer currently holds.
 *
 * A shell runs the same binary over and over - every external command in a
 * BusyBox system is the same two megabytes - and re-reading it from the
 * filesystem each time is the single most expensive thing an exec does. The
 * buffer is already there and already holds exactly those bytes, so the read
 * can be skipped when the path has not changed.
 *
 * Correctness rests on the whole thing living under g_exec_lock, and on any
 * write to the volume dropping the cache: a program that has been rewritten
 * must not keep running as its old self. */
char g_exec_cached[128];

long g_exec_cached_len;

/* Which cache identity the bytes in the staging buffer came from, or 0 if they
 * did not come from the cache. Kept beside the path and the length because it
 * describes the same thing they do: what is currently staged. On a staging hit
 * the bytes are the previous read of this same path, so this stays valid. */
uint32_t g_exec_cached_id;

void linux_exec_cache_drop(void) {
    g_exec_cached[0] = 0;
    g_exec_cached_len = 0;
    g_exec_cached_id = 0;
}
