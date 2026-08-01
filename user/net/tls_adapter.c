/* Hosted TLS dependency boundary.
 *
 * The audited transport-security library sits behind these few functions, so
 * nothing else in the tree includes an Mbed TLS header or names an Mbed TLS
 * type. Two properties here are deliberate:
 *
 *   - The adapter always compiles. When the submodule is not checked out the
 *     build simply has no TLS, and the adapter reports that plainly rather
 *     than failing to link. A missing optional dependency must not break the
 *     build of everything else.
 *   - It is a hosted component, compiled into its own library and never into
 *     the freestanding kernel image.
 */

#include "vibeos/tls.h"

#if defined(VIBEOS_TLS_MBEDTLS)
#include <mbedtls/version.h>
#endif

uint32_t vibeos_tls_library_version(void) {
#if defined(VIBEOS_TLS_MBEDTLS)
    return (uint32_t)mbedtls_version_get_number();
#else
    return 0u;   /* nothing compiled in: report no version rather than a guess */
#endif
}

int vibeos_tls_runtime_available(void) {
#if defined(VIBEOS_TLS_MBEDTLS)
    /* The adapter is intentionally hosted-only until ring-3 entropy, trust
     * storage and socket callbacks are available in the booted system. */
    return mbedtls_version_get_number() >= (int)VIBEOS_TLS_MIN_VERSION ? 1 : 0;
#else
    return 0;
#endif
}
