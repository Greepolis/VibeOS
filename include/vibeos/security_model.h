#ifndef VIBEOS_SECURITY_MODEL_H
#define VIBEOS_SECURITY_MODEL_H

#include <stdint.h>

typedef struct vibeos_security_token {
    uint32_t uid;
    uint32_t gid;
    uint32_t capability_mask;
} vibeos_security_token_t;

int vibeos_sec_token_init(vibeos_security_token_t *token, uint32_t uid, uint32_t gid, uint32_t capabilities);
int vibeos_sec_token_can(const vibeos_security_token_t *token, uint32_t capability_bit);

#endif
