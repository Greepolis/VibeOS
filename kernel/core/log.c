#include "vibeos/log.h"

static void log_copy_message(char *dst, size_t dst_size, const char *src) {
    size_t i = 0;
    if (!dst || dst_size == 0u) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    while (i + 1u < dst_size && src[i] != '\0') {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

int vibeos_log_init(vibeos_log_t *log) {
    uint32_t i;
    if (!log) {
        return -1;
    }
    log->initialized = 1;
    log->head = 0;
    log->count = 0;
    log->dropped = 0;
    log->seq = 0;
    for (i = 0; i < VIBEOS_LOG_CAPACITY; i++) {
        log->events[i].seq = 0;
        log->events[i].level = VIBEOS_LOG_DEBUG;
        log->events[i].code = 0;
        log->events[i].arg0 = 0;
        log->events[i].arg1 = 0;
        log->events[i].message[0] = '\0';
    }
    return 0;
}

int vibeos_log_record(vibeos_log_t *log, vibeos_log_level_t level, uint32_t code, uint64_t arg0, uint64_t arg1, const char *message) {
    vibeos_log_event_t *event;
    if (!log || !log->initialized || level > VIBEOS_LOG_FATAL || !message) {
        return -1;
    }
    event = &log->events[log->head];
    log->head = (log->head + 1u) % VIBEOS_LOG_CAPACITY;
    if (log->count < VIBEOS_LOG_CAPACITY) {
        log->count++;
    } else {
        log->dropped++;
    }
    log->seq++;
    event->seq = log->seq;
    event->level = (uint32_t)level;
    event->code = code;
    event->arg0 = arg0;
    event->arg1 = arg1;
    log_copy_message(event->message, sizeof(event->message), message);
    return 0;
}

int vibeos_log_count(const vibeos_log_t *log, uint32_t *out_count) {
    if (!log || !log->initialized || !out_count) {
        return -1;
    }
    *out_count = log->count;
    return 0;
}

int vibeos_log_dropped(const vibeos_log_t *log, uint32_t *out_dropped) {
    if (!log || !log->initialized || !out_dropped) {
        return -1;
    }
    *out_dropped = log->dropped;
    return 0;
}

int vibeos_log_get(const vibeos_log_t *log, uint32_t index, vibeos_log_event_t *out_event) {
    uint32_t start;
    uint32_t slot;
    if (!log || !log->initialized || !out_event || index >= log->count) {
        return -1;
    }
    start = (log->count == VIBEOS_LOG_CAPACITY) ? log->head : 0u;
    slot = (start + index) % VIBEOS_LOG_CAPACITY;
    *out_event = log->events[slot];
    return 0;
}

int vibeos_log_latest(const vibeos_log_t *log, vibeos_log_event_t *out_event) {
    if (!log || !log->initialized || !out_event || log->count == 0u) {
        return -1;
    }
    return vibeos_log_get(log, log->count - 1u, out_event);
}

const char *vibeos_log_level_name(vibeos_log_level_t level) {
    switch (level) {
        case VIBEOS_LOG_DEBUG:
            return "DEBUG";
        case VIBEOS_LOG_INFO:
            return "INFO";
        case VIBEOS_LOG_WARN:
            return "WARN";
        case VIBEOS_LOG_ERROR:
            return "ERROR";
        case VIBEOS_LOG_FATAL:
            return "FATAL";
        default:
            return "UNKNOWN";
    }
}
