#include "vibeos/security_model.h"

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
