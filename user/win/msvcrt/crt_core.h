#ifndef VIBEOS_WIN_CRT_CORE_H
#define VIBEOS_WIN_CRT_CORE_H

/* The part of our msvcrt.dll that is plain C (docs/abi/windows.md W2).
 *
 * Nothing here knows it is a Windows DLL: no windows.h, no ntdll, and the types
 * are the ones msvcrt's semantics need spelled out - a wide character is a
 * uint16_t, msvcrt's `long` is an int32_t. That is what lets the same file be
 * compiled by mingw-w64 into the DLL and by the host's compiler into
 * vibeos_msvcrt_tests, where printf can be held against glibc's, the heap
 * against a model and the argv rules against Microsoft's own examples.
 *
 * Every name is vc_*: the host's C library is linked into the test, and the DLL
 * exports its msvcrt names from msvcrt.c, as thin wrappers over these. */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

/* ---- strings, memory, ctype (C locale) ---------------------------------- */

size_t vc_strlen(const char *s);
int vc_strcmp(const char *a, const char *b);
int vc_strncmp(const char *a, const char *b, size_t n);
int vc_stricmp(const char *a, const char *b);
int vc_strnicmp(const char *a, const char *b, size_t n);
char *vc_strchr(const char *s, int c);
char *vc_strrchr(const char *s, int c);
char *vc_strstr(const char *s, const char *needle);
char *vc_strcpy(char *d, const char *s);
char *vc_strcat(char *d, const char *s);
size_t vc_strcspn(const char *s, const char *reject);
char *vc_strtok(char *s, const char *delim, char **state);
void *vc_memchr(const void *s, int c, size_t n);
int vc_memcmp(const void *a, const void *b, size_t n);
void *vc_memcpy(void *d, const void *s, size_t n);
void *vc_memmove(void *d, const void *s, size_t n);
void *vc_memset(void *d, int c, size_t n);
int vc_tolower(int c);
int vc_toupper(int c);

size_t vc_wcslen(const uint16_t *s);
int vc_wcscmp(const uint16_t *a, const uint16_t *b);
int vc_wcsncmp(const uint16_t *a, const uint16_t *b, size_t n);
int vc_wcsicmp(const uint16_t *a, const uint16_t *b);
int vc_wcsnicmp(const uint16_t *a, const uint16_t *b, size_t n);
uint16_t *vc_wcscpy(uint16_t *d, const uint16_t *s);
uint16_t *vc_wcscat(uint16_t *d, const uint16_t *s);
uint16_t *vc_wcschr(const uint16_t *s, uint16_t c);
uint16_t *vc_wcsrchr(const uint16_t *s, uint16_t c);
uint16_t *vc_wcsstr(const uint16_t *s, const uint16_t *needle);
uint16_t *vc_wcspbrk(const uint16_t *s, const uint16_t *accept);
uint16_t *vc_wcstok(uint16_t *s, const uint16_t *delim, uint16_t **state);
uint16_t vc_towlower(uint16_t c);

/* ---- numbers ------------------------------------------------------------- */

/* msvcrt's `long` is 32 bits: these clamp at the 32-bit limits. */
int32_t vc_atoi(const char *s);
int32_t vc_strtol(const char *s, char **end, int base);
uint32_t vc_strtoul(const char *s, char **end, int base);

/* msvcrt's generator: seed = seed * 214013 + 2531011; (seed >> 16) & 0x7fff. */
int vc_rand(uint32_t *seed);

void vc_qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *));

double vc_acos(double x);

/* 100 ns ticks since 1601, as NtQuerySystemTime counts, to seconds since 1970. */
int64_t vc_filetime_to_time64(int64_t filetime);

/* msvcrt's floating point control and status words, over the x87 control word
 * and MXCSR. The _MCW_/_EM_/_SW_ numbers are Microsoft's (float.h). On a host
 * that is not x86-64 these do nothing and return 0. */
#define VC_MCW_EM 0x0008001Fu
#define VC_MCW_RC 0x00000300u
#define VC_MCW_PC 0x00030000u
#define VC_MCW_DN 0x03000000u
uint32_t vc_control87(uint32_t value, uint32_t mask);
uint32_t vc_statusfp(void);
uint32_t vc_clearfp(void);
/* The pure mapping, for the test: hardware flag bits (low six of MXCSR or the
 * x87 status word) to Microsoft's _SW_ bits. */
uint32_t vc_sw_from_hw(uint32_t hw);

/* ---- printf and scanf ---------------------------------------------------- */

/* Where formatted output goes, one unit at a time. A unit is a byte or a UTF-16
 * code unit, whichever the sink stores. `total` counts every unit the format
 * produced, written or not - the caller decides what a short buffer means. */
typedef struct vc_sink vc_sink;
struct vc_sink {
    void (*put)(vc_sink *s, uint16_t unit);
    size_t total;
};

/* A sink over a caller's buffer. Units past `cap` are counted and dropped. */
typedef struct {
    vc_sink base;
    void *buf;
    size_t cap;
    int wide;
} vc_mem_sink;

void vc_mem_sink_init(vc_mem_sink *s, void *buf, size_t cap, int wide);

/* Format into `s`. `fmt` is a char string or, if fmt_wide, a UTF-16 one; `wide_out`
 * says which flavour of function this is for - printf's %s is a narrow string
 * and wprintf's is a wide one. Returns the units produced. */
size_t vc_format(vc_sink *s, const void *fmt, int fmt_wide, va_list ap);

/* The msvcrt _vsnprintf contract: the count if it fit (a NUL after it only if
 * there was room), -1 if it did not, and then no NUL. */
int vc_vsnprintf(char *buf, size_t count, const char *fmt, va_list ap);
int vc_vsnwprintf(uint16_t *buf, size_t count, const uint16_t *fmt, va_list ap);
int vc_vsprintf(char *buf, const char *fmt, va_list ap);

/* sscanf; returns the conversions stored, or -1 if the input ended first. */
int vc_vsscanf(const char *s, const char *fmt, va_list ap);

/* ---- the command line ---------------------------------------------------- */

/* Split a UTF-16 command line into arguments by Microsoft's rules and write them
 * as UTF-8, NUL-separated, into `out` (if non-null; at most `cap` bytes), with
 * each argument's offset in `offsets` (if non-null; at most `max_args`). Returns
 * argc, and the bytes needed through `bytes`. Always at least one argument. */
int vc_parse_cmdline(const uint16_t *cmd, size_t units, char *out, size_t cap, size_t *offsets,
                     int max_args, size_t *bytes);

#endif
