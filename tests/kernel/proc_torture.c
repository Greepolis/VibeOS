/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Intensive process-table lifecycle model for issue #201.
 *
 * The reference ledger is independent of the table's slots: it tracks public
 * PIDs, parentage, lifecycle state and thread ownership. The test deliberately
 * reaps and reuses the same bounded process table thousands of times.
 *
 *   vibeos_proc_torture <seed> <rounds>
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "vibeos/proc.h"

#define MODEL_CAP 64u

typedef struct {
    uint32_t pid;
    uint32_t parent;
    uint32_t live;
    uint32_t terminated;
    uint32_t threads;
} proc_model_t;

static uint64_t rng_state;
static uint64_t initial_seed;

static uint32_t rnd32(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return (uint32_t)(rng_state >> 16);
}

static int fail(unsigned round, const char *why) {
    printf("FAIL:proc_torture seed=%llu round=%u: %s\n",
           (unsigned long long)initial_seed, round, why);
    return 1;
}

static int counts_match(vibeos_process_table_t *pt, uint32_t processes,
                        uint32_t live, uint32_t terminated,
                        uint32_t threads, unsigned round) {
    uint32_t got_processes = 0, got_live = 0, got_terminated = 0, got_threads = 0;
    if (vibeos_proc_process_count(pt, &got_processes) != 0 ||
        vibeos_proc_live_count(pt, &got_live) != 0 ||
        vibeos_proc_terminated_count(pt, &got_terminated) != 0 ||
        vibeos_proc_thread_count(pt, &got_threads) != 0 ||
        got_processes != processes || got_live != live ||
        got_terminated != terminated || got_threads != threads) {
        return fail(round, "public lifecycle counters differ from reference ledger");
    }
    return 0;
}

static int run_proc(unsigned rounds) {
    vibeos_process_table_t pt;
    uint32_t init_pid = 0, last_pid = 0;
    unsigned r;

    if (vibeos_proc_init(&pt) != 0 ||
        vibeos_proc_spawn(&pt, 0, &init_pid) != 0 || init_pid != 1u) {
        return fail(0, "init process creation");
    }
    if (counts_match(&pt, 1, 1, 0, 0, 0)) return 1;

    for (r = 0; r < rounds; r++) {
        uint32_t parent = 0, child = 0, tid[4] = {0, 0, 0, 0};
        uint32_t parent_threads = rnd32() % 5u;
        uint32_t child_threads;
        uint32_t i, has_child = (rnd32() & 1u);
        child_threads = has_child ? (rnd32() % 3u) : 0u;
        vibeos_process_state_t state;

        if (vibeos_proc_spawn(&pt, init_pid, &parent) != 0 ||
            parent <= last_pid) {
            return fail(r, "spawn did not reuse capacity with a fresh PID");
        }
        last_pid = parent;
        if (counts_match(&pt, 2, 2, 0, 0, r)) return 1;

        if (has_child && vibeos_proc_spawn(&pt, parent, &child) != 0) {
            return fail(r, "child spawn");
        }
        if (has_child && !vibeos_proc_are_related(&pt, parent, child)) {
            return fail(r, "parent-child relation missing");
        }

        for (i = 0; i < parent_threads; i++) {
            if (vibeos_thread_create(&pt, parent, &tid[i]) != 0) {
                return fail(r, "parent thread creation");
            }
        }
        for (i = 0; i < child_threads; i++) {
            uint32_t child_tid = 0;
            if (!has_child || vibeos_thread_create(&pt, child, &child_tid) != 0) {
                return fail(r, "child thread creation");
            }
        }
        if (counts_match(&pt, has_child ? 3u : 2u,
                         has_child ? 3u : 2u, 0,
                         parent_threads + child_threads, r)) return 1;

        /* A live process is not reapable. Termination must retain a queryable
         * tombstone until the explicit reap, while closing every owned thread. */
        if (vibeos_proc_reap(&pt, parent) == 0) {
            return fail(r, "live process was reaped");
        }
        if (vibeos_proc_terminate(&pt, parent) != 0 ||
            vibeos_proc_state(&pt, parent, &state) != 0 ||
            state != VIBEOS_PROCESS_STATE_TERMINATED) {
            return fail(r, "termination did not publish a tombstone");
        }
        if (has_child &&
            (!vibeos_proc_are_related(&pt, init_pid, child) ||
             vibeos_proc_are_related(&pt, parent, child))) {
            return fail(r, "child was not reparented to init before parent reap");
        }
        if (counts_match(&pt, has_child ? 3u : 2u,
                         has_child ? 2u : 1u, 1,
                         child_threads, r)) return 1;
        if (vibeos_proc_reap(&pt, parent) != 0 ||
            vibeos_proc_reap(&pt, parent) == 0 ||
            vibeos_proc_state(&pt, parent, &state) == 0) {
            return fail(r, "reap did not release the process slot exactly once");
        }
        if (counts_match(&pt, has_child ? 2u : 1u,
                         has_child ? 2u : 1u, 0,
                         child_threads, r)) return 1;

        if (has_child) {
            if (vibeos_proc_terminate(&pt, child) != 0 ||
                vibeos_proc_state(&pt, child, &state) != 0 ||
                state != VIBEOS_PROCESS_STATE_TERMINATED ||
                vibeos_proc_reap(&pt, child) != 0) {
                return fail(r, "child terminate/reap");
            }
            if (counts_match(&pt, 1, 1, 0, 0, r)) return 1;
        }
    }

    /* Exhaust the physical process table, then prove each reclaimed slot can
     * be allocated again without reusing an old public PID. */
    {
        uint32_t pids[VIBEOS_MAX_PROCESSES];
        uint32_t i, count = 0;
        for (i = 0; i < VIBEOS_MAX_PROCESSES - 1u; i++) {
            if (vibeos_proc_spawn(&pt, init_pid, &pids[i]) != 0) {
                return fail(rounds, "could not fill all process slots");
            }
            count++;
        }
        if (vibeos_proc_spawn(&pt, init_pid, &pids[VIBEOS_MAX_PROCESSES - 1u]) == 0) {
            return fail(rounds, "process table admitted more than its fixed capacity");
        }
        for (i = 0; i < count; i++) {
            if (vibeos_proc_terminate(&pt, pids[i]) != 0 ||
                vibeos_proc_reap(&pt, pids[i]) != 0) {
                return fail(rounds, "full-table process could not be reclaimed");
            }
        }
        if (counts_match(&pt, 1, 1, 0, 0, rounds)) return 1;
        if (vibeos_proc_spawn(&pt, init_pid, &pids[0]) != 0 ||
            pids[0] <= last_pid) {
            return fail(rounds, "reclaimed slot was not reusable with a fresh PID");
        }
        if (vibeos_proc_terminate(&pt, pids[0]) != 0 ||
            vibeos_proc_reap(&pt, pids[0]) != 0) {
            return fail(rounds, "final slot cleanup");
        }
    }

    printf("PROC_TORTURE_OK seed=%llu rounds=%u last_pid=%u\n",
           (unsigned long long)initial_seed, rounds, last_pid);
    return 0;
}

int main(int argc, char **argv) {
    unsigned rounds = 5000u;
    if (argc > 1) rng_state = strtoull(argv[1], 0, 0);
    else rng_state = 1u;
    if (rng_state == 0) rng_state = 0x9E3779B97F4A7C15ull;
    initial_seed = rng_state;
    if (argc > 2) rounds = (unsigned)strtoul(argv[2], 0, 0);
    printf("PROC_TORTURE_SEED seed=%llu rounds=%u\n",
           (unsigned long long)initial_seed, rounds);
    return run_proc(rounds);
}
