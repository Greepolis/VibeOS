/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Intensive process-table lifecycle model for issue #201.
 *
 * The reference ledger is independent of the implementation's slots. It
 * tracks public PIDs, parentage, termination and owned thread IDs, then
 * compares those expectations with the public process-table operations.
 *
 *   vibeos_proc_torture <seed> <rounds>
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include "vibeos/proc.h"

#define MODEL_CAP 3u
#define MODEL_THREADS 4u

typedef struct {
    uint32_t pid;
    uint32_t parent;
    uint32_t in_use;
    uint32_t terminated;
    uint32_t thread_count;
    uint32_t tids[MODEL_THREADS];
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

static void model_add(proc_model_t *m, uint32_t pid, uint32_t parent) {
    m->pid = pid;
    m->parent = parent;
    m->in_use = 1;
    m->terminated = 0;
    m->thread_count = 0;
}

static int model_related(const proc_model_t *a, const proc_model_t *b) {
    return a->in_use && b->in_use &&
        (a->pid == b->pid || a->parent == b->pid || b->parent == a->pid);
}

static int model_counts_match(vibeos_process_table_t *pt,
                              const proc_model_t *model,
                              unsigned round) {
    uint32_t processes = 0, live = 0, terminated = 0, threads = 0;
    uint32_t got_processes = 0, got_live = 0, got_terminated = 0, got_threads = 0;
    uint32_t i;

    for (i = 0; i < MODEL_CAP; i++) {
        if (!model[i].in_use) continue;
        processes++;
        if (model[i].terminated) terminated++;
        else live++;
        threads += model[i].thread_count;
    }

    if (vibeos_proc_process_count(pt, &got_processes) != 0 ||
        vibeos_proc_live_count(pt, &got_live) != 0 ||
        vibeos_proc_terminated_count(pt, &got_terminated) != 0 ||
        vibeos_proc_thread_count(pt, &got_threads) != 0 ||
        got_processes != processes || got_live != live ||
        got_terminated != terminated || got_threads != threads) {
        return fail(round, "public lifecycle counters differ from independent ledger");
    }
    return 0;
}

static int run_proc(unsigned rounds) {
    vibeos_process_table_t pt;
    proc_model_t model[MODEL_CAP] = {{0}};
    uint32_t init_pid = 0, last_pid = 0;
    unsigned r;

    if (vibeos_proc_init(&pt) != 0 ||
        vibeos_proc_spawn(&pt, 0, &init_pid) != 0 || init_pid != 1u) {
        return fail(0, "init process creation");
    }
    model_add(&model[0], init_pid, 0);
    if (model_counts_match(&pt, model, 0)) return 1;

    for (r = 0; r < rounds; r++) {
        uint32_t parent = 0, child = 0;
        uint32_t parent_threads = rnd32() % (MODEL_THREADS + 1u);
        uint32_t child_threads = (rnd32() & 1u) ? rnd32() % 3u : 0u;
        uint32_t i;
        int has_child = (rnd32() & 1u) != 0;
        vibeos_process_state_t state;

        model[1] = (proc_model_t){0};
        model[2] = (proc_model_t){0};
        if (!has_child) child_threads = 0;

        if (vibeos_proc_spawn(&pt, init_pid, &parent) != 0 ||
            parent <= last_pid) {
            return fail(r, "spawn did not reuse capacity with a fresh PID");
        }
        last_pid = parent;
        model_add(&model[1], parent, init_pid);
        if (model_counts_match(&pt, model, r)) return 1;

        if (has_child) {
            if (vibeos_proc_spawn(&pt, parent, &child) != 0) {
                return fail(r, "child spawn");
            }
            model_add(&model[2], child, parent);
            if (!vibeos_proc_are_related(&pt, parent, child) ||
                !model_related(&model[1], &model[2])) {
                return fail(r, "parent-child relation missing");
            }
        }

        for (i = 0; i < parent_threads; i++) {
            if (vibeos_thread_create(&pt, parent, &model[1].tids[i]) != 0) {
                return fail(r, "parent thread creation");
            }
            model[1].thread_count++;
        }
        for (i = 0; i < child_threads; i++) {
            if (!has_child ||
                vibeos_thread_create(&pt, child, &model[2].tids[i]) != 0) {
                return fail(r, "child thread creation");
            }
            model[2].thread_count++;
        }
        if (model_counts_match(&pt, model, r)) return 1;

        /* A live process is not reapable. Termination retains a queryable
         * tombstone, reparents children, and removes every owned thread. */
        if (vibeos_proc_reap(&pt, parent) == 0) {
            return fail(r, "live process was reaped");
        }
        if (vibeos_proc_terminate(&pt, parent) != 0 ||
            vibeos_proc_state(&pt, parent, &state) != 0 ||
            state != VIBEOS_PROCESS_STATE_TERMINATED) {
            return fail(r, "termination did not publish a tombstone");
        }
        model[1].terminated = 1;
        model[1].thread_count = 0;
        if (has_child) model[2].parent = init_pid;

        for (i = 0; i < parent_threads; i++) {
            uint32_t owner = 0;
            if (vibeos_thread_state(&pt, model[1].tids[i], &state) == 0 ||
                vibeos_thread_owner(&pt, model[1].tids[i], &owner) == 0) {
                return fail(r, "terminated process retained an owned thread");
            }
        }
        if (has_child &&
            (!vibeos_proc_are_related(&pt, init_pid, child) ||
             vibeos_proc_are_related(&pt, parent, child) ||
             !model_related(&model[0], &model[2]))) {
            return fail(r, "child was not reparented to init before parent reap");
        }
        if (model_counts_match(&pt, model, r)) return 1;

        if (vibeos_proc_reap(&pt, parent) != 0 ||
            vibeos_proc_reap(&pt, parent) == 0 ||
            vibeos_proc_state(&pt, parent, &state) == 0) {
            return fail(r, "reap did not release the process slot exactly once");
        }
        model[1].in_use = 0;
        if (model_counts_match(&pt, model, r)) return 1;

        if (has_child) {
            if (vibeos_proc_terminate(&pt, child) != 0 ||
                vibeos_proc_state(&pt, child, &state) != 0 ||
                state != VIBEOS_PROCESS_STATE_TERMINATED) {
                return fail(r, "child termination did not publish a tombstone");
            }
            model[2].terminated = 1;
            model[2].thread_count = 0;
            for (i = 0; i < child_threads; i++) {
                uint32_t owner = 0;
                if (vibeos_thread_state(&pt, model[2].tids[i], &state) == 0 ||
                    vibeos_thread_owner(&pt, model[2].tids[i], &owner) == 0) {
                    return fail(r, "terminated child retained an owned thread");
                }
            }
            if (model_counts_match(&pt, model, r)) return 1;
            if (vibeos_proc_reap(&pt, child) != 0) {
                return fail(r, "child reap");
            }
            model[2].in_use = 0;
            if (model_counts_match(&pt, model, r)) return 1;
        }
    }

    /* Exhaust the physical table, then prove each reclaimed slot is reusable
     * without reusing an old public PID. */
    {
        uint32_t pids[VIBEOS_MAX_PROCESSES];
        uint32_t i, count = 0;
        for (i = 0; i < VIBEOS_MAX_PROCESSES - 1u; i++) {
            if (vibeos_proc_spawn(&pt, init_pid, &pids[i]) != 0) {
                return fail(rounds, "could not fill all process slots");
            }
            count++;
        }
        if (vibeos_proc_spawn(&pt, init_pid,
                              &pids[VIBEOS_MAX_PROCESSES - 1u]) == 0) {
            return fail(rounds, "process table admitted more than its fixed capacity");
        }
        for (i = 0; i < count; i++) {
            if (vibeos_proc_terminate(&pt, pids[i]) != 0 ||
                vibeos_proc_reap(&pt, pids[i]) != 0) {
                return fail(rounds, "full-table process could not be reclaimed");
            }
        }
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
