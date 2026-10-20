/* Our msvcrt.dll (docs/abi/windows.md W2).
 *
 * The C runtime Wine's conformance executables import, written from the C
 * standard and Microsoft's documentation, freestanding, importing only our
 * ntdll. This file is the part that is Windows: the exported names, the
 * calling convention (mingw's default is the Microsoft x64 one), the FILE
 * streams, and the handful of ntdll services a runtime needs. Everything with
 * logic in it is in crt_core.c, which the host tests also compile.
 *
 * What the kernel does not provide yet, and this runtime therefore answers
 * the way a runtime with nothing behind it would:
 *  - no files (W4): fopen fails, fgets finds nothing, fclose of anything but
 *    a standard stream fails;
 *  - no environment block: getenv finds nothing, envp is empty;
 *  - no second thread (W5): the heap, strtok's position and the generator's
 *    seed are plain statics. Each of them is a decision to revisit with the
 *    thread, not a property: the heap says so in crt_core.h. */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include "crt_core.h"
#include "crt_file.h"
#include "vibeos_ntdll.h"

_Static_assert(sizeof(long) == 4, "msvcrt's long is 32 bits: this must be built for LLP64");
_Static_assert(sizeof(size_t) == sizeof(char *), "__getmainargs reuses argv's storage for offsets");

typedef vc_iobuf FILE;
typedef uint16_t wchar16;

#define NT_STATUS_SUCCESS 0u

/* ---- the process's standard handles -------------------------------------- */

static uint64_t std_handle(int fd) {
    uint64_t peb = vibeos_nt_rd64(vibeos_nt_teb() + VIBEOS_TEB_PEB);
    uint64_t params = vibeos_nt_rd64(peb + VIBEOS_PEB_PROCESS_PARAMS);
    uint32_t at = fd == 0 ? VIBEOS_RUPP_STD_INPUT : (fd == 1 ? VIBEOS_RUPP_STD_OUTPUT : VIBEOS_RUPP_STD_ERROR);

    return vibeos_nt_rd64(params + at);
}

/* All of it or an error. NtWriteFile is synchronous in W2 and reports how much
 * it took in the status block; a call that takes nothing and says success would
 * loop here for ever, so it is an error. */
static int write_fd(int fd, const void *buf, size_t n) {
    const char *p = buf;
    uint64_t h = std_handle(fd);

    while (n) {
        vibeos_nt_iosb_t iosb;
        uint32_t chunk = n > 0x10000u ? 0x10000u : (uint32_t)n;
        NTSTATUS_T st = NtWriteFile(h, 0, 0, 0, &iosb, p, chunk, 0, 0);

        if (st != NT_STATUS_SUCCESS || iosb.information == 0 || iosb.information > chunk) {
            return -1;
        }
        p += iosb.information;
        n -= iosb.information;
    }
    return 0;
}

/* ---- the three standard streams ------------------------------------------ */

/* _IOREAD and _IOWRT, msvcrt's numbers. The streams are unbuffered: every
 * write goes straight to NtWriteFile, so fflush has nothing to do and a program
 * that exits without flushing loses nothing. _cnt, _ptr and _base are
 * therefore always zero. */
static FILE g_iob[3] = {
    {0, 0, 0, 0x1, 0, 0, 0, 0},
    {0, 0, 0, 0x2, 1, 0, 0, 0},
    {0, 0, 0, 0x2, 2, 0, 0, 0},
};

FILE *__iob_func(void) { return g_iob; }

int _fileno(FILE *f) { return f ? f->_file : -1; }

intptr_t _get_osfhandle(int fd) {
    if (fd < 0 || fd > 2) {
        return -1;
    }
    return (intptr_t)std_handle(fd);
}

/* The console is the only thing a descriptor can be until W4, so the three
 * standard ones are terminals and nothing else exists to ask about. */
int _isatty(int fd) { return fd >= 0 && fd <= 2; }

void __set_app_type(int type) { (void)type; }

int setvbuf(FILE *f, char *buf, int mode, size_t size) {
    (void)buf;
    (void)size;
    /* _IOFBF 0, _IOLBF 0x40, _IONBF 4. The streams are unbuffered whatever the
     * program asks, which is a valid way to honour any of them. */
    if (!f || (mode != 0 && mode != 0x40 && mode != 4)) {
        return -1;
    }
    return 0;
}

int fflush(FILE *f) {
    (void)f;
    return 0;
}

int fclose(FILE *f) {
    /* A standard stream is the process's, not the program's; closing it succeeds
     * and changes nothing. Any other FILE cannot exist yet. */
    return (f >= g_iob && f < g_iob + 3) ? 0 : -1;
}

FILE *fopen(const char *name, const char *mode) {
    (void)name;
    (void)mode;
    return NULL;
}

char *fgets(char *s, int n, FILE *f) {
    (void)s;
    (void)n;
    (void)f;
    return NULL;
}

size_t fwrite(const void *buf, size_t size, size_t count, FILE *f) {
    size_t n;

    if (!f || !size || !count || (f->_file != 1 && f->_file != 2)) {
        return 0;
    }
    if (count > (size_t)-1 / size) {
        return 0;
    }
    n = size * count;
    return write_fd(f->_file, buf, n) == 0 ? count : 0;
}

/* ---- printf through a small buffer --------------------------------------- */

typedef struct {
    vc_sink base;
    int fd;
    int failed;
    size_t len;
    char buf[256];
} out_sink;

static void out_flush(out_sink *o) {
    if (o->len && !o->failed && write_fd(o->fd, o->buf, o->len) != 0) {
        o->failed = 1;
    }
    o->len = 0;
}

static void out_put(vc_sink *b, uint16_t u) {
    out_sink *o = (out_sink *)b;

    o->base.total++;
    o->buf[o->len++] = (char)u;
    if (o->len == sizeof o->buf) {
        out_flush(o);
    }
}

static int stream_vprintf(int fd, const char *fmt, va_list ap) {
    out_sink o;
    size_t n;

    o.base.put = out_put;
    o.base.total = 0;
    o.fd = fd;
    o.failed = 0;
    o.len = 0;
    n = vc_format(&o.base, fmt, 0, ap);
    out_flush(&o);
    return o.failed ? -1 : (int)n;
}

int vprintf(const char *fmt, va_list ap) { return stream_vprintf(1, fmt, ap); }

int printf(const char *fmt, ...) {
    va_list ap;
    int r;

    va_start(ap, fmt);
    r = stream_vprintf(1, fmt, ap);
    va_end(ap);
    return r;
}

int fprintf(FILE *f, const char *fmt, ...) {
    va_list ap;
    int r;

    if (!f || (f->_file != 1 && f->_file != 2)) {
        return -1;
    }
    va_start(ap, fmt);
    r = stream_vprintf(f->_file, fmt, ap);
    va_end(ap);
    return r;
}

int puts(const char *s) {
    size_t n = vc_strlen(s);

    if (write_fd(1, s, n) != 0 || write_fd(1, "\n", 1) != 0) {
        return -1;
    }
    return 1;
}

int putchar(int c) {
    char ch = (char)c;

    return write_fd(1, &ch, 1) == 0 ? (int)(uint8_t)ch : -1;
}

int _vsnprintf(char *buf, size_t count, const char *fmt, va_list ap) { return vc_vsnprintf(buf, count, fmt, ap); }
int _vsnwprintf(wchar16 *buf, size_t count, const wchar16 *fmt, va_list ap) {
    return vc_vsnwprintf(buf, count, fmt, ap);
}
int vsprintf(char *buf, const char *fmt, va_list ap) { return vc_vsprintf(buf, fmt, ap); }

int sscanf(const char *s, const char *fmt, ...) {
    va_list ap;
    int r;

    va_start(ap, fmt);
    r = vc_vsscanf(s, fmt, ap);
    va_end(ap);
    return r;
}

/* ---- the heap ------------------------------------------------------------ */

/* The process heap, as Microsoft's msvcrt has it: malloc is HeapAlloc on
 * GetProcessHeap, so a program that frees with HeapFree what malloc gave it is
 * right to. It used to be a 2 MiB arena of msvcrt's own in its .bss - paid for
 * at load by every Windows program, and a ceiling none could pass; the process
 * heap grows on NtAllocateVirtualMemory (W3 step 3). */
#define HEAP_ZERO_MEMORY 0x00000008u

static void *heap(void) {
    uint64_t peb = vibeos_nt_rd64(vibeos_nt_teb() + VIBEOS_TEB_PEB);

    return (void *)(uintptr_t)vibeos_nt_rd64(peb + VIBEOS_PEB_PROCESS_HEAP);
}

void *malloc(size_t n) { return RtlAllocateHeap(heap(), 0, n); }

void *calloc(size_t count, size_t size) {
    if (count && size > (size_t)-1 / count) {
        return NULL;   /* the product would wrap */
    }
    return RtlAllocateHeap(heap(), HEAP_ZERO_MEMORY, count * size);
}

/* realloc(NULL, n) is malloc, realloc(p, 0) frees and answers NULL - msvcrt's
 * rules - and a failed one leaves the block as it was. */
void *realloc(void *p, size_t n) {
    if (!p) {
        return malloc(n);
    }
    if (n == 0) {
        (void)RtlFreeHeap(heap(), 0, p);
        return NULL;
    }
    return RtlReAllocateHeap(heap(), 0, p, n);
}

void free(void *p) {
    if (p) {
        (void)RtlFreeHeap(heap(), 0, p);
    }
}

char *_strdup(const char *s) {
    size_t n;
    char *d;

    if (!s) {
        return NULL;
    }
    n = vc_strlen(s) + 1;
    d = malloc(n);
    if (d) {
        vc_memcpy(d, s, n);
    }
    return d;
}

/* ---- the process --------------------------------------------------------- */

__attribute__((noreturn)) void exit(int code) {
    /* Nothing is buffered, so there is nothing to flush; atexit is not exported. */
    (void)NtTerminateProcess(VIBEOS_NT_CURRENT_PROCESS, (NTSTATUS_T)code);
    for (;;) {
        /* NtTerminateProcess of ourselves does not return. */
    }
}

/* No environment block yet: the kernel passes none, so there is nothing to find. */
char *getenv(const char *name) {
    (void)name;
    return NULL;
}

static char *g_envp[1] = {NULL};

int __getmainargs(int *argc, char ***argv, char ***envp, int expand_wildcards, void *startinfo) {
    uint64_t peb = vibeos_nt_rd64(vibeos_nt_teb() + VIBEOS_TEB_PEB);
    uint64_t params = vibeos_nt_rd64(peb + VIBEOS_PEB_PROCESS_PARAMS);
    uint64_t cl = params + VIBEOS_RUPP_COMMAND_LINE;
    const wchar16 *text = (const wchar16 *)(uintptr_t)vibeos_nt_rd64(cl + VIBEOS_USTR_BUFFER);
    size_t units = *(const volatile uint16_t *)(uintptr_t)(cl + VIBEOS_USTR_LENGTH) / 2u;
    size_t bytes = 0;
    int n, i;
    char **v;
    char *data;

    (void)expand_wildcards;     /* no globbing: there is no filesystem to expand against */
    (void)startinfo;
    n = vc_parse_cmdline(text, units, NULL, 0, NULL, 0, &bytes);
    v = malloc((size_t)(n + 1) * sizeof(char *) + bytes);
    if (!v) {
        return -1;
    }
    data = (char *)(v + n + 1);
    /* The offsets go into the array that will hold the pointers, then become
     * them: the two are the same size (asserted above). */
    (void)vc_parse_cmdline(text, units, data, bytes, (size_t *)v, n, &bytes);
    for (i = 0; i < n; i++) {
        v[i] = data + (size_t)v[i];
    }
    v[n] = NULL;
    *argc = n;
    *argv = v;
    *envp = g_envp;
    return 0;
}

static void err_printf(const char *fmt, ...) {
    va_list ap;

    va_start(ap, fmt);
    (void)stream_vprintf(2, fmt, ap);
    va_end(ap);
}

/* What the C runtime says on stderr before abort(), whose exit code is 3. */
__attribute__((noreturn)) void _assert(const char *msg, const char *file, unsigned line) {
    err_printf("Assertion failed: %s, file %s, line %u\n", msg, file, line);
    exit(3);
}

/* ---- time and floating point --------------------------------------------- */

int64_t _time64(int64_t *t) {
    int64_t now = 0, r;

    (void)NtQuerySystemTime(&now);
    r = vc_filetime_to_time64(now);
    if (t) {
        *t = r;
    }
    return r;
}

unsigned int _control87(unsigned int value, unsigned int mask) { return vc_control87(value, mask); }
unsigned int _clearfp(void) { return vc_clearfp(); }
unsigned int _statusfp(void) { return vc_statusfp(); }
int _isnan(double x) { return x != x; }
double acos(double x) { return vc_acos(x); }

/* ---- strings, memory, numbers ------------------------------------------- */

void *memchr(const void *s, int c, size_t n) { return vc_memchr(s, c, n); }
int memcmp(const void *a, const void *b, size_t n) { return vc_memcmp(a, b, n); }
void *memcpy(void *d, const void *s, size_t n) { return vc_memcpy(d, s, n); }
void *memmove(void *d, const void *s, size_t n) { return vc_memmove(d, s, n); }
void *memset(void *d, int c, size_t n) { return vc_memset(d, c, n); }

size_t strlen(const char *s) { return vc_strlen(s); }
int strcmp(const char *a, const char *b) { return vc_strcmp(a, b); }
int strncmp(const char *a, const char *b, size_t n) { return vc_strncmp(a, b, n); }
int _stricmp(const char *a, const char *b) { return vc_stricmp(a, b); }
int _strnicmp(const char *a, const char *b, size_t n) { return vc_strnicmp(a, b, n); }
char *strchr(const char *s, int c) { return vc_strchr(s, c); }
char *strrchr(const char *s, int c) { return vc_strrchr(s, c); }
char *strstr(const char *s, const char *n) { return vc_strstr(s, n); }
char *strcpy(char *d, const char *s) { return vc_strcpy(d, s); }
char *strcat(char *d, const char *s) { return vc_strcat(d, s); }
size_t strcspn(const char *s, const char *r) { return vc_strcspn(s, r); }

static char *g_strtok_state;
char *strtok(char *s, const char *delim) { return vc_strtok(s, delim, &g_strtok_state); }

int tolower(int c) { return vc_tolower(c); }
int toupper(int c) { return vc_toupper(c); }
wchar16 towlower(wchar16 c) { return vc_towlower(c); }

int atoi(const char *s) { return vc_atoi(s); }
int32_t atol(const char *s) { return vc_atoi(s); }
int32_t strtol(const char *s, char **end, int base) { return vc_strtol(s, end, base); }
uint32_t strtoul(const char *s, char **end, int base) { return vc_strtoul(s, end, base); }

static uint32_t g_rand_seed = 1;
void srand(unsigned int seed) { g_rand_seed = seed; }
int rand(void) { return vc_rand(&g_rand_seed); }

void qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *)) {
    vc_qsort(base, n, size, cmp);
}

size_t wcslen(const wchar16 *s) { return vc_wcslen(s); }
int wcscmp(const wchar16 *a, const wchar16 *b) { return vc_wcscmp(a, b); }
int wcsncmp(const wchar16 *a, const wchar16 *b, size_t n) { return vc_wcsncmp(a, b, n); }
int _wcsicmp(const wchar16 *a, const wchar16 *b) { return vc_wcsicmp(a, b); }
int _wcsnicmp(const wchar16 *a, const wchar16 *b, size_t n) { return vc_wcsnicmp(a, b, n); }
wchar16 *wcscpy(wchar16 *d, const wchar16 *s) { return vc_wcscpy(d, s); }
wchar16 *wcscat(wchar16 *d, const wchar16 *s) { return vc_wcscat(d, s); }
wchar16 *wcschr(const wchar16 *s, wchar16 c) { return vc_wcschr(s, c); }
wchar16 *wcsrchr(const wchar16 *s, wchar16 c) { return vc_wcsrchr(s, c); }
wchar16 *wcsstr(const wchar16 *s, const wchar16 *n) { return vc_wcsstr(s, n); }
wchar16 *wcspbrk(const wchar16 *s, const wchar16 *a) { return vc_wcspbrk(s, a); }

/* The two-argument form msvcrt.dll exports: the position is the runtime's. */
static wchar16 *g_wcstok_state;
wchar16 *wcstok(wchar16 *s, const wchar16 *delim) { return vc_wcstok(s, delim, &g_wcstok_state); }

/* ---- entry --------------------------------------------------------------- */

/* The DLL has no per-process setup: the heap starts on first use. A loader that
 * calls the entry point (the next step's) gets "attached". */
int vibeos_msvcrt_entry(void *module, uint32_t reason, void *reserved) {
    (void)module;
    (void)reason;
    (void)reserved;
    return 1;
}
