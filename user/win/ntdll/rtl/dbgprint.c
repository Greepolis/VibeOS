/* DbgPrint and its relatives (docs/abi/windows.md W2).
 *
 * On Windows these go to an attached debugger, and to nowhere when there is none.
 * There is no debugger here, so the text goes where a developer will see it: the
 * standard error handle of the process (vibeos_rtl_dbg_write). The component and
 * level of DbgPrintEx select what a debugger shows, and with no debugger there is
 * nothing to select, so they are accepted and every message is written.
 *
 * A message is cut at 511 characters, as Windows cuts it at 512 bytes with its
 * terminator. All four return STATUS_SUCCESS. */

#include "rtl_internal.h"

#define DBG_MAX 512u

uint32_t vDbgPrintExWithPrefix(const char *prefix, uint32_t component, uint32_t level, const char *format,
                               va_list args) {
    char buf[DBG_MAX];
    size_t n = 0;

    (void)component;
    (void)level;
    if (prefix) {
        while (prefix[n] && n < DBG_MAX - 1u) {
            buf[n] = prefix[n];
            n++;
        }
    }
    if (format) {
        n += vibeos_rtl_vsnprintf(buf + n, DBG_MAX - n, format, args);
    } else {
        buf[n] = 0;
    }
    vibeos_rtl_dbg_write(buf, n);
    return VIBEOS_STATUS_SUCCESS;
}

uint32_t vDbgPrintEx(uint32_t component, uint32_t level, const char *format, va_list args) {
    return vDbgPrintExWithPrefix(0, component, level, format, args);
}

uint32_t DbgPrintEx(uint32_t component, uint32_t level, const char *format, ...) {
    va_list ap;
    uint32_t st;

    va_start(ap, format);
    st = vDbgPrintExWithPrefix(0, component, level, format, ap);
    va_end(ap);
    return st;
}

uint32_t DbgPrint(const char *format, ...) {
    va_list ap;
    uint32_t st;

    va_start(ap, format);
    st = vDbgPrintExWithPrefix(0, 0, 0, format, ap);
    va_end(ap);
    return st;
}
