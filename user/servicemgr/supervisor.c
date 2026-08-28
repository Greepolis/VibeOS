#include "vibeos/services.h"

static int find_service(const vibeos_service_supervisor_t *s, uint32_t id) {
    uint32_t i;
    for (i = 0; i < s->manifest_count; i++) {
        if (s->manifests[i].service_id == id) {
            return (int)i;
        }
    }
    return -1;
}

int vibeos_service_supervisor_init(vibeos_service_supervisor_t *supervisor) {
    if (!supervisor) {
        return -1;
    }
    *supervisor = (vibeos_service_supervisor_t){0};
    return 0;
}

int vibeos_service_supervisor_load(vibeos_service_supervisor_t *supervisor,
                                   const vibeos_service_manifest_t *manifests,
                                   uint32_t manifest_count) {
    uint32_t i;
    if (!supervisor || !manifests || manifest_count == 0 ||
        manifest_count > VIBEOS_NATIVE_MAX_SERVICES) {
        return -1;
    }
    for (i = 0; i < manifest_count; i++) {
        if (vibeos_service_manifest_validate(manifests, manifest_count, i) != 0) {
            return -1;
        }
        {
            uint32_t previous;
            for (previous = 0; previous < i; previous++) {
                if (manifests[previous].service_id == manifests[i].service_id) {
                    return -1;
                }
            }
        }
    }
    supervisor->manifest_count = manifest_count;
    supervisor->started_mask = 0;
    supervisor->failed_mask = 0;
    for (i = 0; i < manifest_count; i++) {
        supervisor->manifests[i] = manifests[i];
        supervisor->runtime[i] = (vibeos_service_runtime_snapshot_t){
            VIBEOS_NATIVE_ABI_MAJOR, VIBEOS_NATIVE_ABI_MINOR,
            sizeof(vibeos_service_runtime_snapshot_t), manifests[i].service_id, 0, 0,
            0, VIBEOS_PROCESS_EXIT_NORMAL, VIBEOS_NATIVE_SERVICE_STOPPED, 0, 0, 0
        };
    }
    return 0;
}

int vibeos_service_supervisor_start_ready(vibeos_service_supervisor_t *supervisor) {
    uint32_t i;
    uint32_t progress = 1;
    uint32_t started_before;
    if (!supervisor || supervisor->manifest_count == 0) {
        return -1;
    }
    while (progress) {
        started_before = supervisor->started_mask;
        progress = 0;
        for (i = 0; i < supervisor->manifest_count; i++) {
            uint32_t deps = supervisor->manifests[i].dependency_mask;
            if ((supervisor->started_mask & (1u << i)) != 0 ||
                (supervisor->failed_mask & (1u << i)) != 0 ||
                (deps & supervisor->started_mask) != deps) {
                continue;
            }
            supervisor->runtime[i].state = VIBEOS_NATIVE_SERVICE_RUNNING;
            supervisor->runtime[i].started_at_ticks = supervisor->now_ticks;
            supervisor->runtime[i].last_transition_ticks = supervisor->now_ticks;
            supervisor->started_mask |= 1u << i;
            progress = 1;
        }
        if (progress == 0 && started_before != supervisor->started_mask) {
            progress = 1;
        }
    }
    return supervisor->started_mask == ((1u << supervisor->manifest_count) - 1u) ? 0 : -1;
}

int vibeos_service_supervisor_report_exit(vibeos_service_supervisor_t *supervisor,
                                          uint32_t service_id, uint32_t exit_code,
                                          vibeos_process_exit_reason_t reason) {
    int index;
    vibeos_service_runtime_snapshot_t *r;
    const vibeos_service_manifest_t *m;
    if (!supervisor || reason > VIBEOS_PROCESS_EXIT_TIMEOUT) {
        return -1;
    }
    index = find_service(supervisor, service_id);
    if (index < 0) {
        return -1;
    }
    r = &supervisor->runtime[index];
    m = &supervisor->manifests[index];
    r->last_exit_code = exit_code;
    r->last_exit_reason = reason;
    r->last_transition_ticks = supervisor->now_ticks;
    supervisor->started_mask &= ~(1u << index);
    if (m->restart_policy == VIBEOS_NATIVE_RESTART_NEVER ||
        (reason == VIBEOS_PROCESS_EXIT_NORMAL && m->restart_policy != VIBEOS_NATIVE_RESTART_ALWAYS)) {
        r->state = reason == VIBEOS_PROCESS_EXIT_NORMAL ? VIBEOS_NATIVE_SERVICE_STOPPED
                                                        : VIBEOS_NATIVE_SERVICE_FAILED;
        if (r->state == VIBEOS_NATIVE_SERVICE_FAILED) {
            supervisor->failed_mask |= 1u << index;
        }
        return 0;
    }
    if (r->restart_count >= m->restart_limit) {
        r->state = VIBEOS_NATIVE_SERVICE_FAILED;
        supervisor->failed_mask |= 1u << index;
        return 0;
    }
    r->state = VIBEOS_NATIVE_SERVICE_STARTING;
    r->restart_count++;
    r->last_transition_ticks = supervisor->now_ticks + (1ull << (r->restart_count > 6 ? 6 : r->restart_count));
    return 0;
}

int vibeos_service_supervisor_bind_pid(vibeos_service_supervisor_t *supervisor,
                                       uint32_t service_id, uint32_t pid) {
    int index;
    uint32_t i;
    if (!supervisor || service_id == 0 || pid == 0) {
        return -1;
    }
    index = find_service(supervisor, service_id);
    if (index < 0) {
        return -1;
    }
    for (i = 0; i < supervisor->manifest_count; i++) {
        if (i != (uint32_t)index && supervisor->runtime[i].pid == pid) {
            return -1;
        }
    }
    if (supervisor->runtime[index].pid != 0 && supervisor->runtime[index].pid != pid) {
        return -1;
    }
    supervisor->runtime[index].pid = pid;
    return 0;
}

int vibeos_service_supervisor_unbind_pid(vibeos_service_supervisor_t *supervisor,
                                         uint32_t pid) {
    uint32_t i;
    if (!supervisor || pid == 0) {
        return -1;
    }
    for (i = 0; i < supervisor->manifest_count; i++) {
        if (supervisor->runtime[i].pid == pid) {
            supervisor->runtime[i].pid = 0;
            return 0;
        }
    }
    return -1;
}

int vibeos_service_supervisor_service_for_pid(const vibeos_service_supervisor_t *supervisor,
                                              uint32_t pid, uint32_t *out_service_id) {
    uint32_t i;
    if (!supervisor || !out_service_id || pid == 0) {
        return -1;
    }
    for (i = 0; i < supervisor->manifest_count; i++) {
        if (supervisor->runtime[i].pid == pid) {
            *out_service_id = supervisor->runtime[i].service_id;
            return 0;
        }
    }
    return -1;
}

int vibeos_service_supervisor_report_exit_pid(vibeos_service_supervisor_t *supervisor,
                                              uint32_t pid, uint32_t exit_code,
                                              vibeos_process_exit_reason_t reason) {
    uint32_t service_id;
    if (vibeos_service_supervisor_service_for_pid(supervisor, pid, &service_id) != 0) {
        return -1;
    }
    if (vibeos_service_supervisor_unbind_pid(supervisor, pid) != 0) {
        return -1;
    }
    return vibeos_service_supervisor_report_exit(supervisor, service_id, exit_code, reason);
}

int vibeos_service_supervisor_tick(vibeos_service_supervisor_t *supervisor, uint64_t ticks) {
    uint32_t i;
    if (!supervisor) {
        return -1;
    }
    supervisor->now_ticks += ticks;
    for (i = 0; i < supervisor->manifest_count; i++) {
        if (supervisor->runtime[i].state == VIBEOS_NATIVE_SERVICE_STARTING &&
            supervisor->now_ticks >= supervisor->runtime[i].last_transition_ticks) {
            supervisor->runtime[i].state = VIBEOS_NATIVE_SERVICE_RUNNING;
            supervisor->started_mask |= 1u << i;
            supervisor->runtime[i].last_transition_ticks = supervisor->now_ticks;
        }
    }
    /* A failed service is a valid terminal observation for a tick. The
     * supervisor reports it through the runtime snapshot; only malformed
     * input or an invalid transition should make the clock operation fail. */
    (void)vibeos_service_supervisor_start_ready(supervisor);
    return 0;
}

int vibeos_service_supervisor_health(const vibeos_service_supervisor_t *supervisor,
                                     uint32_t *out_running, uint32_t *out_failed) {
    uint32_t i, running = 0, failed = 0;
    if (!supervisor || !out_running || !out_failed) {
        return -1;
    }
    for (i = 0; i < supervisor->manifest_count; i++) {
        running += supervisor->runtime[i].state == VIBEOS_NATIVE_SERVICE_RUNNING;
        failed += supervisor->runtime[i].state == VIBEOS_NATIVE_SERVICE_FAILED;
    }
    *out_running = running;
    *out_failed = failed;
    return 0;
}
