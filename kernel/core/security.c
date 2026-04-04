#include "vibeos/security_model.h"

static int sec_audit_slot_for_index(const vibeos_security_audit_log_t *log, uint32_t index, uint32_t *out_slot) {
    uint32_t start;
    if (!log || !out_slot || index >= log->count) {
        return -1;
    }
    start = (log->head + VIBEOS_SEC_AUDIT_CAPACITY - log->count) % VIBEOS_SEC_AUDIT_CAPACITY;
    *out_slot = (start + index) % VIBEOS_SEC_AUDIT_CAPACITY;
    return 0;
}

int vibeos_sec_token_init(vibeos_security_token_t *token, uint32_t uid, uint32_t gid, uint32_t capabilities) {
    if (!token) {
        return -1;
    }
    token->uid = uid;
    token->gid = gid;
    token->capability_mask = capabilities;
    return 0;
}

int vibeos_sec_token_can(const vibeos_security_token_t *token, uint32_t capability_bit) {
    if (!token || capability_bit >= 32u) {
        return 0;
    }
    return (token->capability_mask & (1u << capability_bit)) ? 1 : 0;
}

int vibeos_sec_audit_init(vibeos_security_audit_log_t *log) {
    uint32_t i;
    if (!log) {
        return -1;
    }
    log->initialized = 1;
    log->seq = 0;
    log->head = 0;
    log->count = 0;
    for (i = 0; i < VIBEOS_SEC_AUDIT_CAPACITY; i++) {
        log->events[i].seq = 0;
        log->events[i].action = 0;
        log->events[i].caller_pid = 0;
        log->events[i].target_pid = 0;
        log->events[i].aux = 0;
        log->events[i].success = 0;
    }
    return 0;
}

int vibeos_sec_audit_record(vibeos_security_audit_log_t *log, vibeos_sec_audit_action_t action, uint32_t caller_pid, uint32_t target_pid, uint64_t aux, uint32_t success) {
    vibeos_sec_audit_event_t *event;
    if (!log || !log->initialized || action == 0) {
        return -1;
    }
    event = &log->events[log->head];
    log->head = (log->head + 1u) % VIBEOS_SEC_AUDIT_CAPACITY;
    if (log->count < VIBEOS_SEC_AUDIT_CAPACITY) {
        log->count++;
    }
    log->seq++;
    event->seq = log->seq;
    event->action = (uint32_t)action;
    event->caller_pid = caller_pid;
    event->target_pid = target_pid;
    event->aux = aux;
    event->success = success ? 1u : 0u;
    return 0;
}

int vibeos_sec_audit_count(vibeos_security_audit_log_t *log, uint32_t *out_count) {
    if (!log || !out_count || !log->initialized) {
        return -1;
    }
    *out_count = log->count;
    return 0;
}

int vibeos_sec_audit_get(vibeos_security_audit_log_t *log, uint32_t index, vibeos_sec_audit_event_t *out_event) {
    uint32_t slot;
    if (!log || !out_event || !log->initialized) {
        return -1;
    }
    if (sec_audit_slot_for_index(log, index, &slot) != 0) {
        return -1;
    }
    *out_event = log->events[slot];
    return 0;
}

int vibeos_sec_audit_count_for_pid(vibeos_security_audit_log_t *log, uint32_t caller_pid, uint32_t *out_count) {
    uint32_t i;
    uint32_t count = 0;
    vibeos_sec_audit_event_t event;
    if (!log || !out_count || !log->initialized || caller_pid == 0) {
        return -1;
    }
    for (i = 0; i < log->count; i++) {
        if (vibeos_sec_audit_get(log, i, &event) != 0) {
            return -1;
        }
        if (event.caller_pid == caller_pid) {
            count++;
        }
    }
    *out_count = count;
    return 0;
}

int vibeos_sec_audit_get_for_pid(vibeos_security_audit_log_t *log, uint32_t caller_pid, uint32_t index, vibeos_sec_audit_event_t *out_event) {
    uint32_t i;
    uint32_t visible_index = 0;
    vibeos_sec_audit_event_t event;
    if (!log || !out_event || !log->initialized || caller_pid == 0) {
        return -1;
    }
    for (i = 0; i < log->count; i++) {
        if (vibeos_sec_audit_get(log, i, &event) != 0) {
            return -1;
        }
        if (event.caller_pid != caller_pid) {
            continue;
        }
        if (visible_index == index) {
            *out_event = event;
            return 0;
        }
        visible_index++;
    }
    return -1;
}

int vibeos_sec_audit_count_action(vibeos_security_audit_log_t *log, uint32_t action, uint32_t *out_count) {
    uint32_t i;
    uint32_t count = 0;
    vibeos_sec_audit_event_t event;
    if (!log || !out_count || !log->initialized || action == 0) {
        return -1;
    }
    for (i = 0; i < log->count; i++) {
        if (vibeos_sec_audit_get(log, i, &event) != 0) {
            return -1;
        }
        if (event.action == action) {
            count++;
        }
    }
    *out_count = count;
    return 0;
}

int vibeos_sec_audit_count_success(vibeos_security_audit_log_t *log, uint32_t success_value, uint32_t *out_count) {
    uint32_t i;
    uint32_t count = 0;
    vibeos_sec_audit_event_t event;
    if (!log || !out_count || !log->initialized || success_value > 1u) {
        return -1;
    }
    for (i = 0; i < log->count; i++) {
        if (vibeos_sec_audit_get(log, i, &event) != 0) {
            return -1;
        }
        if (event.success == success_value) {
            count++;
        }
    }
    *out_count = count;
    return 0;
}

int vibeos_sec_audit_summary(vibeos_security_audit_log_t *log, uint32_t *out_total, uint32_t *out_success, uint32_t *out_fail) {
    uint32_t i;
    uint32_t success_count = 0;
    uint32_t fail_count = 0;
    vibeos_sec_audit_event_t event;
    if (!log || !out_total || !out_success || !out_fail || !log->initialized) {
        return -1;
    }
    for (i = 0; i < log->count; i++) {
        if (vibeos_sec_audit_get(log, i, &event) != 0) {
            return -1;
        }
        if (event.success) {
            success_count++;
        } else {
            fail_count++;
        }
    }
    *out_total = log->count;
    *out_success = success_count;
    *out_fail = fail_count;
    return 0;
}

int vibeos_sec_audit_reset(vibeos_security_audit_log_t *log) {
    if (!log) {
        return -1;
    }
    return vibeos_sec_audit_init(log);
}
