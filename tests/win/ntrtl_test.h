#ifndef VIBEOS_NTRTL_TEST_H
#define VIBEOS_NTRTL_TEST_H

/* Shared by the files of vibeos_ntrtl_tests (docs/abi/windows.md W2). */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "rtl_internal.h"

extern int g_fail;
extern int g_checks;

/* One check. The text is the property, so that a red line says what stopped
 * being true. stdout is not buffered by the runner: a crash after a failure must
 * not lose the line (the kernel test runner learned that the hard way). */
#define CHECK(cond, ...)                                                   \
    do {                                                                   \
        g_checks++;                                                        \
        if (!(cond)) {                                                     \
            g_fail++;                                                      \
            printf("  %s: FAIL (line %d) ", __func__, __LINE__);           \
            printf(__VA_ARGS__);                                           \
            printf("\n");                                                  \
        }                                                                  \
    } while (0)

/* The test hooks (ntrtl_host.c). */
extern uint8_t g_host_params[VIBEOS_RUPP_SIZE];
extern int g_host_params_present;
extern void *g_host_thread;
extern int g_host_contention;
extern char g_host_dbg[4096];
extern size_t g_host_dbg_len;
extern int g_host_dbg_writes;
extern size_t g_host_pages_gap;
extern int g_host_pages_below;
extern size_t g_host_pages_budget;
extern int g_host_pages_calls;
extern uint8_t *g_host_pages_last;

/* What the heap has handed out now, to assert that a test gave back everything
 * it took. Bytes in use, not bytes free: the heap may grow during the test, and
 * then more is free afterwards than before with nothing leaked. */
static inline size_t heap_used_bytes(void) {
    size_t f, total;

    vibeos_rtl_heap_stats(&f, 0, 0);
    (void)vibeos_rtl_heap_check(&total);
    return total - f;
}

/* ASCII to UTF-16 into buf, which must be large enough. */
static inline vibeos_wchar_t *wide(vibeos_wchar_t *buf, const char *s) {
    size_t i;

    for (i = 0; s[i]; i++) {
        buf[i] = (unsigned char)s[i];
    }
    buf[i] = 0;
    return buf;
}

/* A UNICODE_STRING over buf holding s, MaximumLength as given or the length. */
static inline void ustr(vibeos_ustring_t *u, vibeos_wchar_t *buf, const char *s) {
    wide(buf, s);
    u->Length = (uint16_t)(strlen(s) * 2u);
    u->MaximumLength = u->Length;
    u->Buffer = buf;
}

void ntrtl_run_ip(void);
void ntrtl_run_system(void);

#endif
