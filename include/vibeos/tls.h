#ifndef VIBEOS_TLS_H
#define VIBEOS_TLS_H

#include <stdint.h>

/* Hosted TLS dependency boundary. The runtime service will later provide
 * entropy, trust-store and socket callbacks without exposing Mbed TLS types.
 *
 * The audited dependency is optional at build time. When it is not compiled in,
 * vibeos_tls_runtime_available() reports 0 and vibeos_tls_library_version()
 * reports 0, so callers must check availability rather than assume the build
 * has TLS. */

/* Minimum acceptable version of the audited dependency (Mbed TLS 3.6.0),
 * encoded the way Mbed TLS encodes it: MMNNPP00. */
#define VIBEOS_TLS_MIN_VERSION 0x03060000u

/* Version of the linked library, or 0 when TLS is not compiled in. */
uint32_t vibeos_tls_library_version(void);

/* Non-zero when an acceptable TLS dependency is present and usable. */
int vibeos_tls_runtime_available(void);

#endif
