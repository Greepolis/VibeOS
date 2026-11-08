/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Model-based intensive coverage for IPC and object handles in issue #201.
 *
 * The IPC oracle uses a FIFO array and independent event-set model; the
 * object oracle uses a seeded handle ledger.
 *
 *     vibeos_module_torture <ipc|object> <seed> <rounds>
 *
 * The seed is printed before the first operation. A failing run can be replayed
 * without relying on host scheduling or wall-clock timing.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "vibeos/ipc.h"
#include "vibeos/waitset.h"
#include "vibeos/object.h"

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

    /* A batch smaller than the ready set makes round-robin cursor movement
     * observable. Repeating the same ready set must return disjoint batches
     * before wrapping; a cursor that never advances repeats the first batch. */
    if (vibeos_waitset_set_wake_policy(&waitset,
            VIBEOS_WAITSET_WAKE_ROUND_ROBIN) != 0) {
        return fail("ipc", "round-robin setup", 0);
    }
    for (i = 0; i < 16u; i++) {
        if (i == 1u || i == 4u || i == 7u || i == 10u) {
            vibeos_event_signal(&events[i]);
            model_signaled[i] = 1;
        }
    }
    {
        const size_t expected_batches[3][2] = {{1u, 4u}, {7u, 10u}, {1u, 4u}};
        size_t batch[2], batch_count;
        unsigned pass, k;
        for (pass = 0; pass < 3u; pass++) {
            if (vibeos_waitset_wait_batch(&waitset, 0, batch, 2u, &batch_count) != 0 ||
                batch_count != 2u || batch[0] != expected_batches[pass][0] ||
                batch[1] != expected_batches[pass][1]) {
                return fail("ipc", "round-robin cursor did not rotate bounded batches", pass);
            }
            for (k = 0; k < 2u; k++) {
                if (batch[k] >= 16u || !model_signaled[batch[k]]) {
                    return fail("ipc", "round-robin returned an unready event", pass);
                }
            }
        }
        cursor = 5u;
    }
    for (i = 0; i < 16u; i++) {
        vibeos_event_clear(&events[i]);
        model_signaled[i] = 0;
    }
    if (vibeos_waitset_set_wake_policy(&waitset, VIBEOS_WAITSET_WAKE_FIFO) != 0) {
        return fail("ipc", "waitset policy reset", 0);
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
                if (want_count > 3u) {
                    cursor = (uint32_t)((want[2] + 1u) % 16u);
                } else if (want_count != 0) {
                    cursor = (uint32_t)((want[want_count - 1u] + 1u) % 16u);
                }
            } else {
                for (k = 0; k < 16u; k++) {
                    if (enabled[k] && model_signaled[k]) want[want_count++] = k;
                }
            }
            {
                size_t expected_count = want_count < 3u ? want_count : 3u;
                int wait_rc = vibeos_waitset_wait_batch(&waitset, 0, got, 3u, &got_count);
                /* wait_batch reports timeout as -1 when no event is ready;
                 * peek_signaled, used by the former full-set check, returned
                 * success with an empty result. Preserve that API distinction
                 * in the reference oracle. */
                if ((expected_count == 0u ? wait_rc == 0 : wait_rc != 0) ||
                    got_count != expected_count) {
                    return fail("ipc", "waitset batch count differs from model", r);
                }
                /* Round-robin advances only past the returned batch, not the
                 * full ready set. The reference cursor is updated above. */
                for (k = 0; k < expected_count; k++) {
                    if (got[k] != want[k]) {
                        return fail("ipc", "waitset selection order differs from model", r);
                    }
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
    uint32_t next_id = 1, active = 0;
    uint32_t quota = 8u + (uint32_t)(rnd64() % 25u);
    uint32_t types[3] = {0, 0, 0};
    uint64_t failures = 0;
    unsigned r;

    /* The quota is seed-dependent: a fixed quota can leave the model testing
     * the same saturation point in every nightly replay. */
    memset(g_objects, 0, sizeof(g_objects));
    if (vibeos_handle_table_init(&table) != 0 ||
        vibeos_handle_set_quota(&table, quota) != 0) {
        return fail("object", "table init or quota", 0);
    }

    /* Deterministically exercise both sides of the quota boundary before the
     * randomized stream. This makes the >= -> > sabotage fail on every seed,
     * rather than depending on a random stream reaching saturation. */
    for (r = 0; r < quota; r++) {
        uint32_t rights = 1u + (uint32_t)(rnd64() & 15u);
        uint32_t type = (uint32_t)(rnd64() % 3u);
        uint32_t object_id = 1u + (uint32_t)(rnd64() % 100000u);
        uint32_t handle = 0;
        int rc = type == VIBEOS_OBJECT_NONE
            ? vibeos_handle_alloc(&table, rights, &handle)
            : vibeos_handle_alloc_object(&table, rights,
                (vibeos_object_type_t)type, object_id, &handle);
        if (rc != 0 || handle != next_id || handle >= OBJ_MODEL_MAX) {
            return fail("object", "quota prefill allocation differs from model", r);
        }
        g_objects[handle].active = 1;
        g_objects[handle].rights = rights;
        g_objects[handle].type = type;
        g_objects[handle].object_id = type == VIBEOS_OBJECT_NONE ? 0 : object_id;
        active++;
        types[type]++;
        next_id++;
    }
    {
        uint32_t extra = 0;
        int rc = vibeos_handle_alloc(&table, 1u, &extra);
        failures++;
        if (rc == 0) return fail("object", "exact quota boundary admitted an extra handle", quota);
    }
    for (r = 0; r < rounds; r++) {
        uint64_t x = rnd64();
        if ((x % 100u) < 50u) {
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
            uint32_t id;
            if (vibeos_handle_stats(&table, &got_active, &got_quota, &got_failures) != 0 ||
                got_active != active || got_quota != quota || got_failures != failures) {
                return fail("object", "table statistics differ from model", r);
            }
            /* Check every historical handle, not three random samples. This
             * covers every live identity and every closed-id rejection at each
             * checkpoint, including when the physical 1024 slots are reused. */
            for (id = 1u; id < next_id; id++) {
                if (g_objects[id].active) {
                    uint32_t rights = 0, object_id = 0, missing_right;
                    vibeos_object_type_t type = VIBEOS_OBJECT_NONE;
                    if (vibeos_handle_rights(&table, id, &rights) != 0 ||
                        rights != g_objects[id].rights ||
                        vibeos_handle_object(&table, id, &type, &object_id) != 0 ||
                        (uint32_t)type != g_objects[id].type ||
                        object_id != g_objects[id].object_id ||
                        vibeos_handle_has_rights(&table, id, rights) != 1) {
                        return fail("object", "live handle differs from independent map", r);
                    }
                    missing_right = (~rights) & 15u;
                    if (missing_right == 0u) missing_right = 16u;
                    else missing_right &= (uint32_t)(0u - missing_right);
                    if (vibeos_handle_has_rights(&table, id, missing_right) != 0 ||
                        vibeos_handle_has_rights(&table, id, rights | missing_right) != 0) {
                        return fail("object", "rights oracle accepted a missing bit", r);
                    }
                } else {
                    uint32_t ignored_rights = 0;
                    if (vibeos_handle_rights(&table, id, &ignored_rights) == 0 ||
                        vibeos_handle_has_rights(&table, id, 1u) != 0) {
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
    printf("OBJECT_TORTURE_OK seed=%llu rounds=%u active=%u quota=%u\n",
           (unsigned long long)g_rng, rounds, active, quota);
    return 0;
}

int main(int argc, char **argv) {
    const char *module;
    unsigned rounds = 10000u;
    if (argc < 2) {
        printf("usage: vibeos_module_torture <ipc|object> [seed] [rounds]\n");
        return 2;
    }
    module = argv[1];
    g_rng = argc > 2 ? strtoull(argv[2], 0, 0) : 1u;
    if (g_rng == 0) g_rng = 0x9E3779B97F4A7C15ull;
    g_seed = g_rng;
    if (argc > 3) rounds = (unsigned)strtoul(argv[3], 0, 0);
    printf("MODULE_TORTURE_SEED module=%s seed=%llu rounds=%u\n",
           module, (unsigned long long)g_rng, rounds);
    if (strcmp(module, "ipc") == 0) return run_ipc(rounds);
    if (strcmp(module, "object") == 0) return run_object(rounds);
    printf("unknown module: %s\n", module);
    return 2;
}
