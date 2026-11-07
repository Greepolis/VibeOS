/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Model-based intensive coverage for the five modules left by issue #201.
 *
 * Each mode has a deliberately different oracle from the production data
 * structure: a linear syscall scan, a FIFO array plus event-set model, a
 * handle map, a process/thread ledger, or durable/volatile disk images.
 *
 *     vibeos_module_torture <abi|ipc|object|proc|txn> <seed> <rounds>
 *
 * The seed is printed before the first operation. A failing run can be replayed
 * without relying on host scheduling or wall-clock timing.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "vibeos/abi_linux.h"
#include "vibeos/ipc.h"
#include "vibeos/waitset.h"
#include "vibeos/object.h"
#include "vibeos/proc.h"
#include "vibeos/journal.h"
#include "vibeos/blockdev.h"

static uint64_t g_rng;
static uint64_t g_seed;

static uint64_t rnd64(void) {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return g_rng;
}

static int fail(const char *module, const char *what, unsigned round) {
    printf("FAIL:%s_torture seed=%llu round=%u: %s\n",
           module, (unsigned long long)g_seed, round, what);
    return 1;
}

static int run_abi(unsigned rounds) {
    uint32_t count = vibeos_linux_syscall_count();
    uint32_t i;
    uint32_t previous = 0;
    unsigned r;

    if (count == 0 || vibeos_linux_syscall_at(count) != 0) {
        return fail("abi", "registry count or end sentinel", 0);
    }
    /* The reference is a linear scan over the immutable published rows. It
     * does not share the production binary-search algorithm. */
    for (i = 0; i < count; i++) {
        const vibeos_sys_entry_t *entry = vibeos_linux_syscall_at(i);
        if (!entry || (i != 0 && entry->nr <= previous)) {
            return fail("abi", "registry is not strictly ordered", i);
        }
        previous = entry->nr;
        if (vibeos_linux_syscall(entry->nr) != entry) {
            return fail("abi", "exact registry lookup differs from linear row", i);
        }
    }
    for (r = 0; r < rounds; r++) {
        uint64_t nr;
        const vibeos_sys_entry_t *want = 0;
        const vibeos_sys_entry_t *got;
        if ((rnd64() & 7u) == 0u) {
            nr = vibeos_linux_syscall_at((uint32_t)(rnd64() % count))->nr;
        } else {
            nr = rnd64() % 8192u;
        }
        for (i = 0; i < count; i++) {
            const vibeos_sys_entry_t *entry = vibeos_linux_syscall_at(i);
            if ((uint64_t)entry->nr == nr) {
                want = entry;
                break;
            }
        }
        got = vibeos_linux_syscall(nr);
        if (got != want) {
            return fail("abi", "binary lookup differs from independent linear oracle", r);
        }
    }
    if (vibeos_linux_syscall(UINT64_MAX) != 0) {
        return fail("abi", "out-of-domain syscall number was accepted", rounds);
    }
    printf("ABI_TORTURE_OK seed=%llu rows=%u rounds=%u\n",
           (unsigned long long)g_rng, count, rounds);
    return 0;
}

static int same_message(const vibeos_message_t *a, const vibeos_message_t *b) {
    return a->code == b->code && a->payload == b->payload &&
           a->transferred_handle == b->transferred_handle &&
           a->transferred_rights == b->transferred_rights;
}

/* Exercise the channel and waitset through their public operations only.
 * The channel oracle is a separately indexed FIFO array; the waitset oracle
 * derives each policy's order from event state and its own round-robin cursor.
 * Keeping both models separate is important: copying the implementation's
 * ring indices or selection cursor would make the same defect pass twice.
 */
static int run_ipc(unsigned rounds) {
    vibeos_channel_t channel;
    vibeos_message_t model[VIBEOS_CHANNEL_CAPACITY];
    size_t head = 0, count = 0;
    vibeos_waitset_t waitset;
    vibeos_event_t events[16];
    uint8_t enabled[16], priority[16], model_signaled[16];
    uint32_t cursor = 0;
    unsigned i, r;

    /* Start from a known empty channel and a fixed registration order. The
     * model owns its head/count and never inspects the production ring slots.
     */
    vibeos_channel_init(&channel);
    for (i = 0; i < 16u; i++) {
        vibeos_event_init(&events[i]);
        model_signaled[i] = 0;
        enabled[i] = 1;
        priority[i] = 0;
    }
    if (vibeos_waitset_init(&waitset) != 0) {
        return fail("ipc", "waitset init", 0);
    }
    for (i = 0; i < 16u; i++) {
        if (vibeos_waitset_add_with_priority(&waitset, &events[i], 0) != 0) {
            return fail("ipc", "waitset registration", i);
        }
    }

    for (r = 0; r < rounds; r++) {
        uint64_t x = rnd64();
        /* The reference FIFO owns its own head and count. It never reads the
         * production channel's ring indices, so a shared indexing defect is
         * observable instead of being reproduced by the oracle. */
        if ((x & 3u) != 0u) {
            vibeos_message_t msg;
            int rc;
            msg.code = (uint32_t)(x >> 8);
            msg.payload = rnd64();
            msg.transferred_handle = (uint32_t)(rnd64() | 1u);
            msg.transferred_rights = (uint32_t)(rnd64() & 15u);
            rc = vibeos_channel_send(&channel, msg);
            if (count == VIBEOS_CHANNEL_CAPACITY) {
                if (rc == 0) return fail("ipc", "full channel accepted a message", r);
            } else {
                if (rc != 0) return fail("ipc", "channel refused available capacity", r);
                model[(head + count) % VIBEOS_CHANNEL_CAPACITY] = msg;
                count++;
            }
        } else {
            vibeos_message_t got;
            int rc = vibeos_channel_recv(&channel, &got);
            if (count == 0) {
                if (rc == 0) return fail("ipc", "empty channel produced a message", r);
            } else {
                if (rc != 0 || !same_message(&got, &model[head])) {
                    return fail("ipc", "channel FIFO differs from array model", r);
                }
                head = (head + 1u) % VIBEOS_CHANNEL_CAPACITY;
                count--;
            }
        }
        if (channel.count != count) {
            return fail("ipc", "channel count differs from model", r);
        }

        /* Rebuild the expected event ordering from the public event state.
         * The oracle does not call the waitset's collection helper or reuse
         * its round-robin cursor. Registration order is intentionally stable.
         * Priority ties are resolved by the first registered event. The
         * independent cursor advances only after a non-empty round-robin peek.
         */
        {
            size_t got[16], got_count = 0, want[16], want_count = 0;
            uint32_t policy = (uint32_t)(rnd64() % 4u);
            uint32_t k;
            if (vibeos_waitset_set_wake_policy(&waitset,
                    (vibeos_waitset_wake_policy_t)policy) != 0) {
                return fail("ipc", "wake policy rejected", r);
            }
            for (k = 0; k < 16u; k++) {
                uint64_t v = rnd64();
                if ((v & 1u) != 0u) {
                    vibeos_event_signal(&events[k]);
                    model_signaled[k] = 1;
                } else {
                    vibeos_event_clear(&events[k]);
                    model_signaled[k] = 0;
                }
                if (vibeos_event_is_signaled(&events[k]) != model_signaled[k]) {
                    return fail("ipc", "event API differs from independent signal model", r);
                }
                enabled[k] = (uint8_t)((v >> 1) & 1u);
                priority[k] = (uint8_t)(v >> 8);
                if (vibeos_waitset_set_event_enabled(&waitset, k, enabled[k]) != 0 ||
                    vibeos_waitset_set_event_priority(&waitset, k, priority[k]) != 0) {
                    return fail("ipc", "event metadata update", r);
                }
            }
            if (policy == VIBEOS_WAITSET_WAKE_PRIORITY) {
                uint8_t used[16] = {0};
                while (want_count < 16u) {
                    int best = -1;
                    for (k = 0; k < 16u; k++) {
                        if (!used[k] && enabled[k] && model_signaled[k] &&
                            (best < 0 || priority[k] > priority[best])) {
                            best = (int)k;
                        }
                    }
                    if (best < 0) break;
                    want[want_count++] = (size_t)best;
                    used[best] = 1;
                }
            } else if (policy == VIBEOS_WAITSET_WAKE_REVERSE) {
                for (k = 16u; k > 0; k--) {
                    uint32_t idx = k - 1u;
                    if (enabled[idx] && model_signaled[idx]) want[want_count++] = idx;
                }
            } else if (policy == VIBEOS_WAITSET_WAKE_ROUND_ROBIN) {
                for (k = 0; k < 16u; k++) {
                    uint32_t idx = (cursor + k) % 16u;
                    if (enabled[idx] && model_signaled[idx]) want[want_count++] = idx;
                }
                if (want_count != 0) cursor = (uint32_t)((want[want_count - 1u] + 1u) % 16u);
            } else {
                for (k = 0; k < 16u; k++) {
                    if (enabled[k] && model_signaled[k]) want[want_count++] = k;
                }
            }
            if (vibeos_waitset_peek_signaled(&waitset, got, 16u, &got_count) != 0 ||
                got_count != want_count) {
                return fail("ipc", "waitset result count differs from model", r);
            }
            for (k = 0; k < want_count; k++) {
                if (got[k] != want[k]) {
                    return fail("ipc", "waitset selection order differs from model", r);
                }
            }
        }
    }
    printf("IPC_TORTURE_OK seed=%llu rounds=%u\n",
           (unsigned long long)g_rng, rounds);
    return 0;
}

#define OBJ_MODEL_MAX 65536u
typedef struct {
    uint32_t active, rights, type, object_id;
} obj_model_t;
static obj_model_t g_objects[OBJ_MODEL_MAX];

static int run_object(unsigned rounds) {
    vibeos_handle_table_t table;
    uint32_t next_id = 1, active = 0, quota = 48;
    uint32_t types[3] = {0, 0, 0};
    uint64_t failures = 0;
    unsigned r;

    memset(g_objects, 0, sizeof(g_objects));
    if (vibeos_handle_table_init(&table) != 0 ||
        vibeos_handle_set_quota(&table, quota) != 0) {
        return fail("object", "table init or quota", 0);
    }
    for (r = 0; r < rounds; r++) {
        uint64_t x = rnd64();
        if ((x % 100u) < 58u) {
            uint32_t rights = (uint32_t)((x >> 12) & 15u) + 1u;
            uint32_t type = (uint32_t)((x >> 20) % 3u);
            uint32_t object_id = (uint32_t)((x >> 32) % 100000u) + 1u;
            uint32_t handle = 0;
            int rc;
            if (type == VIBEOS_OBJECT_NONE) {
                rc = vibeos_handle_alloc(&table, rights, &handle);
            } else {
                rc = vibeos_handle_alloc_object(&table, rights,
                    (vibeos_object_type_t)type, object_id, &handle);
            }
            if (active >= quota) {
                failures++;
                if (rc == 0) return fail("object", "quota admitted an extra handle", r);
            } else {
                if (rc != 0 || handle != next_id || handle >= OBJ_MODEL_MAX) {
                    return fail("object", "allocation differs from monotonic-id model", r);
                }
                g_objects[handle].active = 1;
                g_objects[handle].rights = rights;
                g_objects[handle].type = type;
                g_objects[handle].object_id = type == VIBEOS_OBJECT_NONE ? 0 : object_id;
                active++;
                types[type]++;
                next_id++;
            }
        } else {
            uint32_t handle = (uint32_t)(rnd64() % (uint64_t)(next_id + 2u));
            int rc = vibeos_handle_close(&table, handle);
            if (handle != 0 && handle < next_id && g_objects[handle].active) {
                if (rc != 0) return fail("object", "live handle could not be closed", r);
                g_objects[handle].active = 0;
                types[g_objects[handle].type]--;
                active--;
            } else if (rc == 0) {
                return fail("object", "stale or unknown handle was closed", r);
            }
        }
        if ((r % 17u) == 0u) {
            uint32_t got_active = 0, got_quota = 0;
            uint64_t got_failures = 0;
            uint32_t id, k;
            if (vibeos_handle_stats(&table, &got_active, &got_quota, &got_failures) != 0 ||
                got_active != active || got_quota != quota || got_failures != failures) {
                return fail("object", "table statistics differ from model", r);
            }
            /* Sample the map periodically: the table deliberately scans a
             * bounded 1024-entry array, so checking every id every round would
             * make the test quadratic in its own history rather than in the
             * operations under test. */
            for (k = 0; k < 3u && next_id > 1u; k++) {
                uint32_t probe = 1u + (uint32_t)(rnd64() % (uint64_t)(next_id - 1u));
                if (g_objects[probe].active) {
                    uint32_t rights = 0, object_id = 0;
                    uint32_t missing_right;
                    vibeos_object_type_t type = VIBEOS_OBJECT_NONE;
                    if (vibeos_handle_rights(&table, probe, &rights) != 0 ||
                        rights != g_objects[probe].rights ||
                        vibeos_handle_object(&table, probe, &type, &object_id) != 0 ||
                        (uint32_t)type != g_objects[probe].type ||
                        object_id != g_objects[probe].object_id) {
                        return fail("object", "handle contents differ from independent map", r);
                    }
                    if (vibeos_handle_has_rights(&table, probe, rights) != 1) {
                        return fail("object", "handle rejected rights it owns", r);
                    }
                    missing_right = (~rights) & 15u;
                    if (missing_right == 0u) missing_right = 16u;
                    else missing_right &= (uint32_t)(0u - missing_right);
                    if (vibeos_handle_has_rights(&table, probe, missing_right) != 0) {
                        return fail("object", "handle accepted a right absent from model", r);
                    }
                } else {
                    uint32_t ignored_rights = 0;
                    if (vibeos_handle_rights(&table, probe, &ignored_rights) == 0 ||
                        vibeos_handle_has_rights(&table, probe, 1u) != 0) {
                        return fail("object", "closed handle still grants rights", r);
                    }
                }
            }
            for (id = VIBEOS_OBJECT_PROCESS; id <= VIBEOS_OBJECT_THREAD; id++) {
                uint32_t got = 0;
                if (vibeos_handle_count_by_type(&table, (vibeos_object_type_t)id, &got) != 0 ||
                    got != types[id]) {
                    return fail("object", "per-type count differs from model", r);
                }
            }
        }
    }
    printf("OBJECT_TORTURE_OK seed=%llu rounds=%u active=%u\n",
           (unsigned long long)g_rng, rounds, active);
    return 0;
}

static int run_proc(unsigned rounds) {
    vibeos_process_table_t pt;
    uint32_t root = 0, child[8], grandchild[4], tids[64], owners[64];
    uint32_t nt = 0, i;
    uint8_t thread_live[64] = {0};
    uint32_t pid[13], parent[13];
    vibeos_process_state_t process_model[13];
    vibeos_thread_state_t thread_model[64];
    uint64_t expected_proc_transitions = 0, expected_thread_transitions = 0;
    uint32_t expected_thread_exits = 0, expected_terminations = 0;
    unsigned r;

    memset(&pt, 0, sizeof(pt));
    if (vibeos_proc_init(&pt) != 0 || vibeos_proc_spawn(&pt, 0, &root) != 0 || root != 1u) {
        return fail("proc", "root creation", 0);
    }
    pid[0] = root;
    parent[0] = 0;
    process_model[0] = VIBEOS_PROCESS_STATE_NEW;
    for (i = 0; i < 8u; i++) {
        if (vibeos_proc_spawn(&pt, root, &child[i]) != 0) {
            return fail("proc", "child creation", i);
        }
        pid[i + 1u] = child[i];
        parent[i + 1u] = root;
        process_model[i + 1u] = VIBEOS_PROCESS_STATE_NEW;
    }
    for (i = 0; i < 4u; i++) {
        uint32_t parent_index = (i & 1u) + 1u;
        if (vibeos_proc_spawn(&pt, child[i & 1u], &grandchild[i]) != 0) {
            return fail("proc", "grandchild creation", i);
        }
        pid[i + 9u] = grandchild[i];
        parent[i + 9u] = pid[parent_index];
        process_model[i + 9u] = VIBEOS_PROCESS_STATE_NEW;
    }
    if (pt.process_count != 13u) return fail("proc", "process count after spawn", 0);

    for (i = 0; i < 13u; i++) {
        unsigned j;
        for (j = 0; j < 3u; j++) {
            uint32_t tid = 0, owner = 0;
            if (vibeos_thread_create(&pt, pid[i], &tid) != 0 ||
                vibeos_thread_owner(&pt, tid, &owner) != 0 || owner != pid[i]) {
                return fail("proc", "thread ownership after creation", i);
            }
            tids[nt] = tid;
            owners[nt] = pid[i];
            thread_live[nt] = 1;
            thread_model[nt] = VIBEOS_THREAD_STATE_RUNNABLE;
            expected_thread_transitions++;
            nt++;
            if (process_model[i] != VIBEOS_PROCESS_STATE_RUNNING) {
                process_model[i] = VIBEOS_PROCESS_STATE_RUNNING;
                expected_proc_transitions++;
            }
        }
    }

    for (r = 0; r < rounds; r++) {
        uint64_t x = rnd64();
        if ((x & 1u) == 0u) {
            uint32_t p = (uint32_t)((x >> 8) % 13u);
            vibeos_process_state_t next = (vibeos_process_state_t)((x >> 16) % 4u);
            vibeos_process_state_t got;
            if (vibeos_proc_set_state(&pt, pid[p], next) != 0) {
                return fail("proc", "process state update refused", r);
            }
            if (process_model[p] != next) expected_proc_transitions++;
            process_model[p] = next;
            if (vibeos_proc_state(&pt, pid[p], &got) != 0 || got != process_model[p]) {
                return fail("proc", "process state differs from independent ledger", r);
            }
        } else {
            uint32_t t = (uint32_t)((x >> 8) % nt);
            if (thread_live[t]) {
                if ((x & 0x10000u) != 0u) {
                    vibeos_thread_state_t next = (vibeos_thread_state_t)((x >> 24) % 3u);
                    vibeos_thread_state_t got;
                    if (vibeos_thread_set_state(&pt, tids[t], next) != 0) {
                        return fail("proc", "thread state update refused", r);
                    }
                    if (thread_model[t] != next) expected_thread_transitions++;
                    thread_model[t] = next;
                    if (vibeos_thread_state(&pt, tids[t], &got) != 0 || got != thread_model[t]) {
                        return fail("proc", "thread state differs from independent ledger", r);
                    }
                } else {
                    if (vibeos_thread_exit(&pt, tids[t]) != 0) {
                        return fail("proc", "live thread exit refused", r);
                    }
                    if (thread_model[t] != VIBEOS_THREAD_STATE_EXITED) expected_thread_transitions++;
                    thread_model[t] = VIBEOS_THREAD_STATE_EXITED;
                    thread_live[t] = 0;
                    expected_thread_exits++;
                    if (vibeos_thread_owner(&pt, tids[t], &i) == 0) {
                        return fail("proc", "exited thread remained resolvable", r);
                    }
                }
            }
        }
        if ((r % 97u) == 0u) {
            uint32_t live = 0, p;
            uint64_t got_proc_transitions = 0, got_thread_transitions = 0;
            uint64_t got_terminations = 0, got_exits = 0;
            for (i = 0; i < nt; i++) {
                live += thread_live[i] ? 1u : 0u;
                if (thread_live[i]) {
                    vibeos_thread_state_t got;
                    uint32_t owner = 0;
                    if (vibeos_thread_state(&pt, tids[i], &got) != 0 ||
                        got != thread_model[i] ||
                        vibeos_thread_owner(&pt, tids[i], &owner) != 0 ||
                        owner != owners[i]) {
                        return fail("proc", "thread state or owner differs from ledger", r);
                    }
                }
            }
            for (p = 0; p < 13u; p++) {
                vibeos_process_state_t got;
                if (vibeos_proc_state(&pt, pid[p], &got) != 0 ||
                    got != process_model[p]) {
                    return fail("proc", "process state differs from ledger", r);
                }
            }
            if (pt.thread_count != live ||
                vibeos_proc_transition_counters(&pt, &got_proc_transitions,
                    &got_thread_transitions, &got_terminations, &got_exits) != 0 ||
                got_proc_transitions != expected_proc_transitions ||
                got_thread_transitions != expected_thread_transitions ||
                got_terminations != expected_terminations || got_exits != expected_thread_exits) {
                return fail("proc", "lifecycle counters differ from independent ledger", r);
            }
        }
    }

    /* Reparent every child of child[0] to init in the reference parent map,
     * then compare every live entry, not just one selected grandchild. */
    if (vibeos_proc_terminate(&pt, child[0]) != 0) {
        return fail("proc", "child termination", rounds);
    }
    if (process_model[1] != VIBEOS_PROCESS_STATE_TERMINATED) {
        expected_proc_transitions++;
        expected_terminations++;
    }
    process_model[1] = VIBEOS_PROCESS_STATE_TERMINATED;
    for (i = 0; i < 13u; i++) {
        if (parent[i] == child[0]) parent[i] = root;
        {
            vibeos_process_state_t got_state;
            if (!pt.entries[i].in_use || pt.entries[i].pid != pid[i] ||
                pt.entries[i].parent_pid != parent[i] ||
                pt.entries[i].state != process_model[i] ||
                vibeos_proc_state(&pt, pid[i], &got_state) != 0 ||
                got_state != process_model[i]) {
                return fail("proc", "reparented process missing or differs from parent/state ledger", rounds);
            }
        }
    }
    for (i = 0; i < nt; i++) {
        if (owners[i] == child[0] && thread_live[i]) {
            if (thread_model[i] != VIBEOS_THREAD_STATE_EXITED) expected_thread_transitions++;
            thread_model[i] = VIBEOS_THREAD_STATE_EXITED;
            thread_live[i] = 0;
            expected_thread_exits++;
        }
    }
    if (vibeos_proc_terminate(&pt, root) != 0) {
        return fail("proc", "root termination", rounds + 1u);
    }
    if (process_model[0] != VIBEOS_PROCESS_STATE_TERMINATED) {
        expected_proc_transitions++;
        expected_terminations++;
    }
    process_model[0] = VIBEOS_PROCESS_STATE_TERMINATED;
    for (i = 0; i < 13u; i++) {
        if (parent[i] == root) parent[i] = 0;
        {
            vibeos_process_state_t got_state;
            if (!pt.entries[i].in_use || pt.entries[i].pid != pid[i] ||
                pt.entries[i].parent_pid != parent[i] ||
                pt.entries[i].state != process_model[i] ||
                vibeos_proc_state(&pt, pid[i], &got_state) != 0 ||
                got_state != process_model[i]) {
                return fail("proc", "init teardown process missing or differs from parent/state ledger", rounds + 2u);
            }
        }
    }
    for (i = 0; i < nt; i++) {
        if (thread_live[i]) {
            if (thread_model[i] != VIBEOS_THREAD_STATE_EXITED) expected_thread_transitions++;
            thread_model[i] = VIBEOS_THREAD_STATE_EXITED;
            thread_live[i] = 0;
            expected_thread_exits++;
        }
    }
    {
        uint64_t got_proc_transitions = 0, got_thread_transitions = 0;
        uint64_t got_terminations = 0, got_exits = 0;
        if (pt.thread_count != 0u ||
            vibeos_proc_transition_counters(&pt, &got_proc_transitions,
                &got_thread_transitions, &got_terminations, &got_exits) != 0 ||
            got_proc_transitions != expected_proc_transitions ||
            got_thread_transitions != expected_thread_transitions ||
            got_terminations != expected_terminations || got_exits != expected_thread_exits) {
            return fail("proc", "final teardown counters differ from ledger", rounds + 3u);
        }
    }
    printf("PROC_TORTURE_OK seed=%llu rounds=%u processes=%u threads=%u\n",
           (unsigned long long)g_seed, rounds, pt.process_count, pt.thread_count);
    return 0;
}

#define TX_SECTORS 64u
#define TX_SLOTS 8u
static uint8_t g_durable[TX_SECTORS][VIBEOS_BLOCK_SIZE];
static uint8_t g_volatile[TX_SECTORS][VIBEOS_BLOCK_SIZE];
static uint8_t g_slot_data[TX_SLOTS][VIBEOS_BLOCK_SIZE];
static vibeos_block_slot_t g_slots[TX_SLOTS];
static vibeos_blockdev_t g_dev;
static vibeos_blockcache_t g_cache;
static uint64_t g_targets[VIBEOS_JOURNAL_MAX_TARGETS];
static uint8_t g_staging[VIBEOS_JOURNAL_MAX_TARGETS * VIBEOS_BLOCK_SIZE];
static uint64_t g_fail_lba;
static int g_fail_once;

static int tx_read(void *ctx, uint64_t lba, void *buf) {
    (void)ctx;
    if (lba >= TX_SECTORS || !buf) return -1;
    memcpy(buf, g_volatile[lba], VIBEOS_BLOCK_SIZE);
    return 0;
}
static int tx_write(void *ctx, uint64_t lba, const void *buf) {
    (void)ctx;
    if (lba >= TX_SECTORS || !buf) return -1;
    if (g_fail_once && lba == g_fail_lba) {
        g_fail_once = 0;
        return -1;
    }
    memcpy(g_volatile[lba], buf, VIBEOS_BLOCK_SIZE);
    return 0;
}
static int tx_flush(void *ctx) {
    (void)ctx;
    memcpy(g_durable, g_volatile, sizeof(g_durable));
    return 0;
}
static int tx_cache_init(void) {
    uint32_t i;
    for (i = 0; i < TX_SLOTS; i++) g_slots[i].data = g_slot_data[i];
    memset(g_slot_data, 0, sizeof(g_slot_data));
    return vibeos_blockcache_init(&g_cache, &g_dev, g_slots, TX_SLOTS);
}

static int run_txn(unsigned rounds) {
    unsigned r;
    g_dev.read = tx_read;
    g_dev.write = tx_write;
    g_dev.flush = tx_flush;
    g_dev.ctx = 0;
    g_dev.sectors = TX_SECTORS;

    for (r = 0; r < rounds; r++) {
        vibeos_journal_t journal;
        uint8_t expected[3][VIBEOS_BLOCK_SIZE];
        uint8_t got[VIBEOS_BLOCK_SIZE];
        uint32_t i, b;
        int rc;

        memset(g_durable, 0, sizeof(g_durable));
        for (i = 0; i < 3u; i++) {
            memset(g_durable[3u + i], (int)(0x20u + i), VIBEOS_BLOCK_SIZE);
        }
        memcpy(g_volatile, g_durable, sizeof(g_durable));
        if (tx_cache_init() != 0 ||
            vibeos_journal_init(&journal, &g_cache, 16u, 8u, g_targets, g_staging) != 0 ||
            vibeos_journal_begin(&journal) != 0) {
            return fail("txn", "journal setup", r);
        }
        for (i = 0; i < 3u; i++) {
            for (b = 0; b < VIBEOS_BLOCK_SIZE; b++) {
                expected[i][b] = (uint8_t)(rnd64() >> 24);
            }
            if (vibeos_journal_stage(&journal, 3u + i, expected[i]) != 0) {
                return fail("txn", "stage target", r);
            }
        }
        /* The failure is injected after the commit record is durable, while
         * checkpointing a target. Recovery must replay the whole set. */
        g_fail_lba = 3u + (rnd64() % 3u);
        g_fail_once = 1;
        rc = vibeos_journal_commit(&journal);
        if (rc == 0 || g_fail_once != 0) {
            return fail("txn", "checkpoint failure was not reached and reported", r);
        }

        /* A power cut loses volatile device state and every cache slot. */
        memcpy(g_volatile, g_durable, sizeof(g_durable));
        if (tx_cache_init() != 0 ||
            vibeos_journal_init(&journal, &g_cache, 16u, 8u, g_targets, g_staging) != 0 ||
            journal.replays != 1u) {
            return fail("txn", "committed transaction was not replayed", r);
        }
        for (i = 0; i < 3u; i++) {
            if (vibeos_blockcache_read(&g_cache, 3u + i, got) != 0 ||
                memcmp(got, expected[i], VIBEOS_BLOCK_SIZE) != 0) {
                return fail("txn", "recovery left a mixed or stale target set", r);
            }
        }
        /* A second reboot must find the journal retired, not replay it again. */
        memcpy(g_volatile, g_durable, sizeof(g_durable));
        if (tx_cache_init() != 0 ||
            vibeos_journal_init(&journal, &g_cache, 16u, 8u, g_targets, g_staging) != 0 ||
            journal.replays != 0u) {
            return fail("txn", "recovery did not retire the committed record", r);
        }
    }
    printf("TXN_TORTURE_OK seed=%llu rounds=%u\n",
           (unsigned long long)g_rng, rounds);
    return 0;
}

int main(int argc, char **argv) {
    const char *module;
    unsigned rounds = 10000u;
    if (argc < 2) {
        printf("usage: vibeos_module_torture <abi|ipc|object|proc|txn> [seed] [rounds]\n");
        return 2;
    }
    module = argv[1];
    g_rng = argc > 2 ? strtoull(argv[2], 0, 0) : 1u;
    if (g_rng == 0) g_rng = 0x9E3779B97F4A7C15ull;
    g_seed = g_rng;
    if (argc > 3) rounds = (unsigned)strtoul(argv[3], 0, 0);
    printf("MODULE_TORTURE_SEED module=%s seed=%llu rounds=%u\n",
           module, (unsigned long long)g_rng, rounds);
    if (strcmp(module, "abi") == 0) return run_abi(rounds);
    if (strcmp(module, "ipc") == 0) return run_ipc(rounds);
    if (strcmp(module, "object") == 0) return run_object(rounds);
    if (strcmp(module, "proc") == 0) return run_proc(rounds);
    if (strcmp(module, "txn") == 0) return run_txn(rounds);
    printf("unknown module: %s\n", module);
    return 2;
}
