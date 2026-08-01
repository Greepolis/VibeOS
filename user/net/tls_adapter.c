#include "vibeos/tls.h"

#include <mbedtls/version.h>

uint32_t vibeos_tls_library_version(void) {
    return (uint32_t)mbedtls_version_get_number();
}

int vibeos_tls_runtime_available(void) {
    /* The adapter is intentionally hosted-only until ring-3 entropy, trust
     * storage and socket callbacks are available in the booted system. */
    return mbedtls_version_get_number() >= 0x03060000 ? 1 : 0;
}
