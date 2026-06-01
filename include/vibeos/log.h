#ifndef VIBEOS_LOG_H
#define VIBEOS_LOG_H

#include <stddef.h>
#include <stdint.h>

#define VIBEOS_LOG_CAPACITY 64u
#define VIBEOS_LOG_MESSAGE_SIZE 96u

typedef enum vibeos_log_level {
    VIBEOS_LOG_DEBUG = 0,
    VIBEOS_LOG_INFO = 1,
    VIBEOS_LOG_WARN = 2,
    VIBEOS_LOG_ERROR = 3,
    VIBEOS_LOG_FATAL = 4
} vibeos_log_level_t;

typedef struct vibeos_log_event {
    uint64_t seq;
    uint32_t level;
    uint32_t code;
    uint64_t arg0;
    uint64_t arg1;
    char message[VIBEOS_LOG_MESSAGE_SIZE];
} vibeos_log_event_t;

typedef struct vibeos_log {
    uint32_t initialized;
    uint32_t head;
    uint32_t count;
    uint32_t dropped;
    uint64_t seq;
    vibeos_log_event_t events[VIBEOS_LOG_CAPACITY];
} vibeos_log_t;

int vibeos_log_init(vibeos_log_t *log);
int vibeos_log_record(vibeos_log_t *log, vibeos_log_level_t level, uint32_t code, uint64_t arg0, uint64_t arg1, const char *message);
int vibeos_log_count(const vibeos_log_t *log, uint32_t *out_count);
int vibeos_log_dropped(const vibeos_log_t *log, uint32_t *out_dropped);
int vibeos_log_get(const vibeos_log_t *log, uint32_t index, vibeos_log_event_t *out_event);
int vibeos_log_latest(const vibeos_log_t *log, vibeos_log_event_t *out_event);
const char *vibeos_log_level_name(vibeos_log_level_t level);

#endif
