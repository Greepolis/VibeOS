#ifndef VIBEOS_TLS_H
#define VIBEOS_TLS_H

#include <stdint.h>

/* Hosted TLS dependency boundary. The runtime service will later provide
 * entropy, trust-store and socket callbacks without exposing Mbed TLS types. */
uint32_t vibeos_tls_library_version(void);
int vibeos_tls_runtime_available(void);

#endif
