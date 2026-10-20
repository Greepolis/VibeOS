#ifndef VIBEOS_RTL_INTERNAL_H
#define VIBEOS_RTL_INTERNAL_H

/* What the Rtl files share (docs/abi/windows.md W2).
 *
 * The Rtl core is written to compile twice: into ntdll.dll with mingw-w64, and
 * into tests/win/ntrtl_tests.c on the Linux host. So it includes no Windows
 * header, and everything that touches the machine - the allocator's memory
 * excepted - goes through the five hooks below. platform_win.c answers them
 * from the TEB and NtWriteFile; tests/win/ntrtl_host.c answers them from
 * variables a test sets. A hook is the one place the two builds differ, which
 * is what makes the host test a test of the code that ships. */

#define VIBEOS_BUILDING_NTDLL 1
#include <stdarg.h>
#include <string.h>

#include "vibeos_ntdll.h"

/* ---- the allocator (heap.c) ------------------------------------------------------
 * The process heap, which RtlAllocateHeap and the rest of Rtl share. A pointer
 * that is not ours is ignored by free: the process parameters and an
 * environment block the kernel built are not in the heap, and a program that
 * frees one must leak it, not corrupt the free list. */
void *vibeos_rtl_alloc(size_t size);
void vibeos_rtl_free(void *p);
/* Bytes free, the largest free block and the number of free blocks - for the
 * tests, which assert that everything allocated can be given back. */
void vibeos_rtl_heap_stats(size_t *free_bytes, size_t *largest, size_t *blocks);
/* The size a block was asked for, or (size_t)-1 if the pointer is not one of
 * ours; and the process heap's handle. */
size_t vibeos_rtl_size(const void *p);
void *vibeos_rtl_process_heap(void);
/* Walk every block the heap holds: 0 if its invariants hold, else which broke.
 * `total` is the bytes the segments hold, free and allocated. For the tests. */
int vibeos_rtl_heap_check(size_t *total);
/* The heap grows a segment at a time, each at least this - a megabyte that
 * costs only the pages that are touched - and at most this many of them. */
#define VIBEOS_RTL_SEGMENT      (1u << 20)
#define VIBEOS_RTL_SEGMENTS_MAX 256u
/* The largest request: past it the rounding could wrap, and no segment would
 * be had for it anyway. */
#define VIBEOS_RTL_ALLOC_MAX    ((size_t)1 << 40)

/* ---- the hooks ------------------------------------------------------------------ */

/* `bytes` of fresh read-write memory, 16-byte aligned at least, for the heap -
 * NtAllocateVirtualMemory on Windows' side; null if there is none. */
void *vibeos_rtl_pages(size_t bytes);

/* PEB->ProcessParameters, as a byte block read through VIBEOS_RUPP_*; NULL when
 * the process has none. */
uint8_t *vibeos_rtl_process_params(void);
/* TEB->ClientId.UniqueThread: who is calling. */
void *vibeos_rtl_current_thread(void);
/* A critical section is held by another thread. With one thread per process this
 * cannot happen, so it ends the process with STATUS_NOT_IMPLEMENTED rather than
 * spinning on a lock nobody can release; it does not return on Windows. */
void vibeos_rtl_contention(void);
/* Bytes for the debugger's channel: the standard error handle. */
void vibeos_rtl_dbg_write(const char *buf, size_t len);

/* ---- small helpers ---------------------------------------------------------------- */

static inline uint64_t rtl_rd64(const uint8_t *p, uint32_t off) {
    uint64_t v;
    memcpy(&v, p + off, sizeof(v));
    return v;
}

static inline void rtl_wr64(uint8_t *p, uint32_t off, uint64_t v) {
    memcpy(p + off, &v, sizeof(v));
}

static inline size_t rtl_wcslen(const vibeos_wchar_t *s) {
    size_t n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

/* ASCII lower-case, for digits of hex numbers. */
static inline int rtl_hexval(unsigned c) {
    if (c >= '0' && c <= '9') {
        return (int)(c - '0');
    }
    if (c >= 'a' && c <= 'f') {
        return (int)(c - 'a' + 10);
    }
    if (c >= 'A' && c <= 'F') {
        return (int)(c - 'A' + 10);
    }
    return -1;
}

/* The upper-casing of a byte in the ANSI code page (Windows-1252), shared by
 * RtlUpperString and RtlCompareString. */
unsigned char vibeos_rtl_upcase_ansi(unsigned char c);

/* Windows-1252 to UTF-16. */
vibeos_wchar_t vibeos_rtl_ansi_to_wchar(unsigned char c);

/* Forget RtlRandom's table, so the next call fills it from its seed again.
 * Windows has no such call; the tests need one to be repeatable. */
void vibeos_rtl_random_reset(void);

/* Formats like the C library's vsnprintf with Windows' LLP64 meanings (printf.c);
 * returns the characters written, never more than cap - 1, and always NUL-ends a
 * buffer of at least one byte. */
size_t vibeos_rtl_vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap);

#endif
