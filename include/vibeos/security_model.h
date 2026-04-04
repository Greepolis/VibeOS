#ifndef VIBEOS_SECURITY_MODEL_H
#define VIBEOS_SECURITY_MODEL_H

#include <stdint.h>

#define VIBEOS_SEC_AUDIT_CAPACITY 256u

typedef struct vibeos_security_token {
    uint32_t uid;
    uint32_t gid;
    uint32_t capability_mask;
} vibeos_security_token_t;

typedef enum vibeos_sec_audit_action {
    VIBEOS_SEC_AUDIT_PROCESS_SPAWN = 1,
    VIBEOS_SEC_AUDIT_PROCESS_TOKEN_SET = 2,
    VIBEOS_SEC_AUDIT_POLICY_CAPABILITY_SET = 3
} vibeos_sec_audit_action_t;

typedef struct vibeos_sec_audit_event {
    uint64_t seq;
    uint32_t action;
    uint32_t caller_pid;
    uint32_t target_pid;
    uint64_t aux;
    uint32_t success;
} vibeos_sec_audit_event_t;

typedef struct vibeos_security_audit_log {
    uint32_t initialized;
    uint64_t seq;
    uint32_t head;
    uint32_t count;
    vibeos_sec_audit_event_t events[VIBEOS_SEC_AUDIT_CAPACITY];
} vibeos_security_audit_log_t;

int vibeos_sec_token_init(vibeos_security_token_t *token, uint32_t uid, uint32_t gid, uint32_t capabilities);
int vibeos_sec_token_can(const vibeos_security_token_t *token, uint32_t capability_bit);
int vibeos_sec_audit_init(vibeos_security_audit_log_t *log);
int vibeos_sec_audit_record(vibeos_security_audit_log_t *log, vibeos_sec_audit_action_t action, uint32_t caller_pid, uint32_t target_pid, uint64_t aux, uint32_t success);
int vibeos_sec_audit_count(vibeos_security_audit_log_t *log, uint32_t *out_count);
int vibeos_sec_audit_get(vibeos_security_audit_log_t *log, uint32_t index, vibeos_sec_audit_event_t *out_event);
int vibeos_sec_audit_count_for_pid(vibeos_security_audit_log_t *log, uint32_t caller_pid, uint32_t *out_count);
int vibeos_sec_audit_get_for_pid(vibeos_security_audit_log_t *log, uint32_t caller_pid, uint32_t index, vibeos_sec_audit_event_t *out_event);
int vibeos_sec_audit_count_action(vibeos_security_audit_log_t *log, uint32_t action, uint32_t *out_count);
int vibeos_sec_audit_summary(vibeos_security_audit_log_t *log, uint32_t *out_total, uint32_t *out_success, uint32_t *out_fail);
int vibeos_sec_audit_reset(vibeos_security_audit_log_t *log);

#endif
