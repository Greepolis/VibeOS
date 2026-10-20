/* msvcrt.dll's portable core (docs/abi/windows.md W2). See crt_core.h for why
 * this file knows nothing about Windows.
 *
 * Written from the C standard and Microsoft's documentation of msvcrt's
 * behaviour - the places where the two differ are marked, because those are the
 * places a program built against msvcrt will notice:
 *  - `long` is 32 bits, wchar_t 16;
 *  - printf's exponent has three digits ("1.5e+010"), %p is sixteen upper-case
 *    digits without a prefix, %n is refused, %s of a null pointer is "(null)",
 *    infinities and NaNs are "1.#INF" and "1.#QNAN";
 *  - _vsnprintf returns -1 for a truncated result, and writes no NUL then;
 *  - a tie rounds away from zero ("%.0f" of 2.5 is "3"), not to even.
 *
 * Nothing here may call a C library function: this file is compiled -nostdlib
 * into the DLL, which exports memcpy and the rest from these same bodies. */

#include "crt_core.h"

/* ---- ctype and memory ---------------------------------------------------- */

static int is_space(int c) { return c == ' ' || (c >= '\t' && c <= '\r'); }
static int is_digit(int c) { return c >= '0' && c <= '9'; }

int vc_tolower(int c) { return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c; }
int vc_toupper(int c) { return (c >= 'a' && c <= 'z') ? c - ('a' - 'A') : c; }

/* The C locale only: msvcrt's towlower consults the locale for anything above
 * 0x7f, and the locale of a process that never called setlocale is "C". */
uint16_t vc_towlower(uint16_t c) { return (c >= 'A' && c <= 'Z') ? (uint16_t)(c + ('a' - 'A')) : c; }

void *vc_memset(void *d, int c, size_t n) {
    volatile uint8_t *p = d;
    while (n--) {
        *p++ = (uint8_t)c;
    }
    return d;
}

void *vc_memcpy(void *d, const void *s, size_t n) {
    uint8_t *dp = d;
    const uint8_t *sp = s;
    while (n--) {
        *dp++ = *sp++;
    }
    return d;
}

void *vc_memmove(void *d, const void *s, size_t n) {
    uint8_t *dp = d;
    const uint8_t *sp = s;
    if (dp == sp || n == 0) {
        return d;
    }
    if (dp < sp || dp >= sp + n) {
        while (n--) {
            *dp++ = *sp++;
        }
    } else {
        dp += n;
        sp += n;
        while (n--) {
            *--dp = *--sp;
        }
    }
    return d;
}

int vc_memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *x = a, *y = b;
    for (; n; n--, x++, y++) {
        if (*x != *y) {
            return *x < *y ? -1 : 1;
        }
    }
    return 0;
}

void *vc_memchr(const void *s, int c, size_t n) {
    const uint8_t *p = s;
    for (; n; n--, p++) {
        if (*p == (uint8_t)c) {
            return (void *)(uintptr_t)p;
        }
    }
    return NULL;
}

/* ---- narrow strings ------------------------------------------------------ */

size_t vc_strlen(const char *s) {
    size_t n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

/* The sign is -1, 0 or 1: msvcrt's string comparisons return that, and a test
 * that checks for the value rather than the sign should get it. */
int vc_strcmp(const char *a, const char *b) {
    for (; *a && *a == *b; a++, b++) {
    }
    return (uint8_t)*a == (uint8_t)*b ? 0 : ((uint8_t)*a < (uint8_t)*b ? -1 : 1);
}

int vc_strncmp(const char *a, const char *b, size_t n) {
    for (; n; n--, a++, b++) {
        if (*a != *b || !*a) {
            return (uint8_t)*a == (uint8_t)*b ? 0 : ((uint8_t)*a < (uint8_t)*b ? -1 : 1);
        }
    }
    return 0;
}

int vc_stricmp(const char *a, const char *b) {
    for (;; a++, b++) {
        int x = vc_tolower((uint8_t)*a), y = vc_tolower((uint8_t)*b);
        if (x != y) {
            return x < y ? -1 : 1;
        }
        if (!x) {
            return 0;
        }
    }
}

int vc_strnicmp(const char *a, const char *b, size_t n) {
    for (; n; n--, a++, b++) {
        int x = vc_tolower((uint8_t)*a), y = vc_tolower((uint8_t)*b);
        if (x != y) {
            return x < y ? -1 : 1;
        }
        if (!x) {
            return 0;
        }
    }
    return 0;
}

char *vc_strchr(const char *s, int c) {
    for (;; s++) {
        if (*s == (char)c) {
            return (char *)(uintptr_t)s;
        }
        if (!*s) {
            return NULL;
        }
    }
}

char *vc_strrchr(const char *s, int c) {
    const char *last = NULL;
    for (;; s++) {
        if (*s == (char)c) {
            last = s;
        }
        if (!*s) {
            return (char *)(uintptr_t)last;
        }
    }
}

char *vc_strstr(const char *s, const char *needle) {
    size_t n = vc_strlen(needle);
    if (n == 0) {
        return (char *)(uintptr_t)s;
    }
    for (; *s; s++) {
        if (*s == *needle && vc_strncmp(s, needle, n) == 0) {
            return (char *)(uintptr_t)s;
        }
    }
    return NULL;
}

char *vc_strcpy(char *d, const char *s) {
    char *r = d;
    while ((*d++ = *s++) != 0) {
    }
    return r;
}

char *vc_strcat(char *d, const char *s) {
    vc_strcpy(d + vc_strlen(d), s);
    return d;
}

size_t vc_strcspn(const char *s, const char *reject) {
    size_t n = 0;
    for (; s[n]; n++) {
        if (vc_strchr(reject, s[n])) {
            break;
        }
    }
    return n;
}

/* The caller keeps the position: msvcrt keeps it per thread, and a process has
 * one thread until W5 (msvcrt.c holds the single copy). */
char *vc_strtok(char *s, const char *delim, char **state) {
    char *tok;
    if (!s) {
        s = *state;
    }
    if (!s) {
        return NULL;
    }
    while (*s && vc_strchr(delim, *s)) {
        s++;
    }
    if (!*s) {
        *state = NULL;
        return NULL;
    }
    tok = s;
    while (*s && !vc_strchr(delim, *s)) {
        s++;
    }
    if (*s) {
        *s++ = 0;
    }
    *state = s;
    return tok;
}

/* ---- wide strings -------------------------------------------------------- */

size_t vc_wcslen(const uint16_t *s) {
    size_t n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

int vc_wcscmp(const uint16_t *a, const uint16_t *b) {
    for (; *a && *a == *b; a++, b++) {
    }
    return *a == *b ? 0 : (*a < *b ? -1 : 1);
}

int vc_wcsncmp(const uint16_t *a, const uint16_t *b, size_t n) {
    for (; n; n--, a++, b++) {
        if (*a != *b || !*a) {
            return *a == *b ? 0 : (*a < *b ? -1 : 1);
        }
    }
    return 0;
}

int vc_wcsicmp(const uint16_t *a, const uint16_t *b) {
    for (;; a++, b++) {
        uint16_t x = vc_towlower(*a), y = vc_towlower(*b);
        if (x != y) {
            return x < y ? -1 : 1;
        }
        if (!x) {
            return 0;
        }
    }
}

int vc_wcsnicmp(const uint16_t *a, const uint16_t *b, size_t n) {
    for (; n; n--, a++, b++) {
        uint16_t x = vc_towlower(*a), y = vc_towlower(*b);
        if (x != y) {
            return x < y ? -1 : 1;
        }
        if (!x) {
            return 0;
        }
    }
    return 0;
}

uint16_t *vc_wcscpy(uint16_t *d, const uint16_t *s) {
    uint16_t *r = d;
    while ((*d++ = *s++) != 0) {
    }
    return r;
}

uint16_t *vc_wcscat(uint16_t *d, const uint16_t *s) {
    vc_wcscpy(d + vc_wcslen(d), s);
    return d;
}

uint16_t *vc_wcschr(const uint16_t *s, uint16_t c) {
    for (;; s++) {
        if (*s == c) {
            return (uint16_t *)(uintptr_t)s;
        }
        if (!*s) {
            return NULL;
        }
    }
}

uint16_t *vc_wcsrchr(const uint16_t *s, uint16_t c) {
    const uint16_t *last = NULL;
    for (;; s++) {
        if (*s == c) {
            last = s;
        }
        if (!*s) {
            return (uint16_t *)(uintptr_t)last;
        }
    }
}

uint16_t *vc_wcsstr(const uint16_t *s, const uint16_t *needle) {
    size_t n = vc_wcslen(needle);
    if (n == 0) {
        return (uint16_t *)(uintptr_t)s;
    }
    for (; *s; s++) {
        if (*s == *needle && vc_wcsncmp(s, needle, n) == 0) {
            return (uint16_t *)(uintptr_t)s;
        }
    }
    return NULL;
}

uint16_t *vc_wcspbrk(const uint16_t *s, const uint16_t *accept) {
    for (; *s; s++) {
        if (vc_wcschr(accept, *s)) {
            return (uint16_t *)(uintptr_t)s;
        }
    }
    return NULL;
}

uint16_t *vc_wcstok(uint16_t *s, const uint16_t *delim, uint16_t **state) {
    uint16_t *tok;
    if (!s) {
        s = *state;
    }
    if (!s) {
        return NULL;
    }
    while (*s && vc_wcschr(delim, *s)) {
        s++;
    }
    if (!*s) {
        *state = NULL;
        return NULL;
    }
    tok = s;
    while (*s && !vc_wcschr(delim, *s)) {
        s++;
    }
    if (*s) {
        *s++ = 0;
    }
    *state = s;
    return tok;
}

/* ---- integers ------------------------------------------------------------ */

static int digit_value(int c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'Z') {
        return c - 'A' + 10;
    }
    return 99;
}

/* The magnitude of the number at s, saturating: *ovf says it did not fit 32
 * bits (all that msvcrt's long and unsigned long can hold). */
static uint32_t parse_magnitude(const char *s, const char **endp, int base, int *neg, int *ovf) {
    const char *start = s;
    uint64_t v = 0;
    int any = 0;

    *neg = 0;
    *ovf = 0;
    while (is_space((uint8_t)*s)) {
        s++;
    }
    if (*s == '-' || *s == '+') {
        *neg = (*s == '-');
        s++;
    }
    if ((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X') &&
        digit_value((uint8_t)s[2]) < 16) {
        s += 2;
        base = 16;
    } else if (base == 0) {
        base = (s[0] == '0') ? 8 : 10;
    }
    if (base < 2 || base > 36) {
        *endp = start;
        return 0;
    }
    for (;; s++) {
        int d = digit_value((uint8_t)*s);
        if (d >= base) {
            break;
        }
        any = 1;
        if (!*ovf) {
            v = v * (uint64_t)base + (uint64_t)d;
            if (v > 0xFFFFFFFFu) {
                *ovf = 1;
            }
        }
    }
    *endp = any ? s : start;
    return *ovf ? 0xFFFFFFFFu : (uint32_t)v;
}

int32_t vc_atoi(const char *s) {
    uint32_t v = 0;
    int neg = 0;
    while (is_space((uint8_t)*s)) {
        s++;
    }
    if (*s == '-' || *s == '+') {
        neg = (*s == '-');
        s++;
    }
    while (is_digit((uint8_t)*s)) {
        v = v * 10u + (uint32_t)(*s - '0');
        s++;
    }
    return (int32_t)(neg ? 0u - v : v);
}

int32_t vc_strtol(const char *s, char **end, int base) {
    const char *e;
    int neg, ovf;
    uint32_t m = parse_magnitude(s, &e, base, &neg, &ovf);
    int32_t r;

    if (end) {
        *end = (char *)(uintptr_t)e;
    }
    if (neg) {
        r = (ovf || m > 0x80000000u) ? INT32_MIN : (int32_t)(0u - m);
    } else {
        r = (ovf || m > 0x7FFFFFFFu) ? INT32_MAX : (int32_t)m;
    }
    return r;
}

uint32_t vc_strtoul(const char *s, char **end, int base) {
    const char *e;
    int neg, ovf;
    uint32_t m = parse_magnitude(s, &e, base, &neg, &ovf);

    if (end) {
        *end = (char *)(uintptr_t)e;
    }
    if (ovf) {
        return 0xFFFFFFFFu;
    }
    return neg ? 0u - m : m;
}

int vc_rand(uint32_t *seed) {
    *seed = *seed * 214013u + 2531011u;
    return (int)((*seed >> 16) & 0x7FFFu);
}

/* ---- qsort --------------------------------------------------------------- */

/* Heapsort: no recursion and no allocation, so a C runtime that has neither at
 * the bottom of its stack can still sort. Not stable; neither is msvcrt's. */
static void swap_bytes(char *a, char *b, size_t n) {
    while (n--) {
        char t = *a;
        *a++ = *b;
        *b++ = t;
    }
}

static void sift_down(char *base, size_t size, size_t root, size_t end,
                      int (*cmp)(const void *, const void *)) {
    while (2 * root + 1 <= end) {
        size_t child = 2 * root + 1;
        if (child + 1 <= end && cmp(base + child * size, base + (child + 1) * size) < 0) {
            child++;
        }
        if (cmp(base + root * size, base + child * size) < 0) {
            swap_bytes(base + root * size, base + child * size, size);
            root = child;
        } else {
            return;
        }
    }
}

void vc_qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *)) {
    char *b = base;
    size_t start, end;

    if (n < 2 || size == 0) {
        return;
    }
    for (start = (n - 2) / 2 + 1; start-- > 0;) {
        sift_down(b, size, start, n - 1, cmp);
    }
    for (end = n - 1; end > 0; end--) {
        swap_bytes(b, b + end * size, size);
        sift_down(b, size, 0, end - 1, cmp);
    }
}

/* ---- acos ---------------------------------------------------------------- */

static double vc_sqrt(double x) {
#if defined(__x86_64__)
    double r;
    __asm__("sqrtsd %1, %0" : "=x"(r) : "x"(x));
    return r;
#else
    return __builtin_sqrt(x);
#endif
}

#define VC_PI   3.14159265358979323846
#define VC_PI_2 1.57079632679489661923
#define VC_PI_2_LO 6.123233995736766e-17    /* pi/2 minus the double above */

/* atan for x >= 0. Above 1 it is pi/2 - atan(1/x). Up to 1 the argument is moved
 * to the nearest multiple c of a quarter, where atan(c) is a constant (two
 * doubles for the 106 bits it needs), by atan(x) = atan(c) + atan(u) with
 * u = (x - c) / (1 + x*c); u is at most 0.125, where the Taylor series gains
 * two digits a term and eleven terms are past a double's last place. The
 * constants carry nearly all of the answer, so the error that is left is the
 * last addition's, not the reduction's: halving the angle three times instead
 * cost five ulps. */
static double atan_pos(double x) {
    static const double c_hi[5] = {0.0, 0.24497866312686414, 0.46364760900080609, 0.64350110879328437,
                                   0.78539816339744828};
    static const double c_lo[5] = {0.0, 1.0692943926138287e-17, 2.268693045925918e-17, 1.5829351718288365e-17,
                                   3.06287113727155e-17};
    double u, u2, p;
    int i;

    if (x > 1.0) {
        return (VC_PI_2 - atan_pos(1.0 / x)) + VC_PI_2_LO;
    }
    i = (int)(x * 4.0 + 0.5);
    u = i == 0 ? x : (x - 0.25 * i) / (1.0 + x * (0.25 * i));
    u2 = u * u;
    p = 1.0 / 21.0;
    p = 1.0 / 19.0 - u2 * p;
    p = 1.0 / 17.0 - u2 * p;
    p = 1.0 / 15.0 - u2 * p;
    p = 1.0 / 13.0 - u2 * p;
    p = 1.0 / 11.0 - u2 * p;
    p = 1.0 / 9.0 - u2 * p;
    p = 1.0 / 7.0 - u2 * p;
    p = 1.0 / 5.0 - u2 * p;
    p = 1.0 / 3.0 - u2 * p;
    p = 1.0 - u2 * p;
    return c_hi[i] + (c_lo[i] + u * p);
}

/* acos(x) = 2 * atan(sqrt((1 - x) / (1 + x))): 1 - x is exact for x >= 0.5 and
 * 1 + x for x <= -0.5, so the cancellation that ruins acos(x) = pi/2 - asin(x)
 * near +-1 never happens. */
double vc_acos(double x) {
    if (x != x || x > 1.0 || x < -1.0) {
        return __builtin_nan("");
    }
    if (x == 1.0) {
        return 0.0;
    }
    if (x == -1.0) {
        return VC_PI;
    }
    if (x > -0.5 && x < 0.5) {
        /* Around zero the half-angle form has t near 1, where atan has no small
         * argument to be accurate with. acos = pi/2 - asin, asin(x) =
         * atan(x / sqrt(1 - x^2)) and the pi/2 is carried in two doubles. */
        double a = atan_pos((x < 0 ? -x : x) / vc_sqrt((1.0 - x) * (1.0 + x)));
        return x < 0 ? (VC_PI_2 + a) + VC_PI_2_LO : (VC_PI_2 - a) + VC_PI_2_LO;
    }
    return 2.0 * atan_pos(vc_sqrt((1.0 - x) / (1.0 + x)));
}

/* ---- time ---------------------------------------------------------------- */

int64_t vc_filetime_to_time64(int64_t ft) {
    int64_t d = ft - 116444736000000000LL;     /* 1601 to 1970, in 100 ns */
    if (d >= 0) {
        return d / 10000000;
    }
    return -((-d + 9999999) / 10000000);        /* floor, as a clock reads before 1970 */
}

/* ---- floating point control ---------------------------------------------- */

/* Hardware keeps the six exception masks (control) and flags (status) in the
 * same order - invalid, denormal, zero-divide, overflow, underflow, inexact -
 * and Microsoft numbers them the other way round, with denormal off to one side. */
static uint32_t ms_from_hw6(uint32_t hw) {
    uint32_t r = 0;
    if (hw & 0x01u) r |= 0x00000010u;   /* invalid     */
    if (hw & 0x02u) r |= 0x00080000u;   /* denormal    */
    if (hw & 0x04u) r |= 0x00000008u;   /* zero-divide */
    if (hw & 0x08u) r |= 0x00000004u;   /* overflow    */
    if (hw & 0x10u) r |= 0x00000002u;   /* underflow   */
    if (hw & 0x20u) r |= 0x00000001u;   /* inexact     */
    return r;
}

static uint32_t hw6_from_ms(uint32_t ms) {
    uint32_t r = 0;
    if (ms & 0x00000010u) r |= 0x01u;
    if (ms & 0x00080000u) r |= 0x02u;
    if (ms & 0x00000008u) r |= 0x04u;
    if (ms & 0x00000004u) r |= 0x08u;
    if (ms & 0x00000002u) r |= 0x10u;
    if (ms & 0x00000001u) r |= 0x20u;
    return r;
}

uint32_t vc_sw_from_hw(uint32_t hw) { return ms_from_hw6(hw & 0x3Fu); }

#if defined(__x86_64__)
static uint32_t rd_mxcsr(void) {
    uint32_t v;
    __asm__ __volatile__("stmxcsr %0" : "=m"(v));
    return v;
}
static void wr_mxcsr(uint32_t v) { __asm__ __volatile__("ldmxcsr %0" : : "m"(v)); }
static uint16_t rd_x87cw(void) {
    uint16_t v;
    __asm__ __volatile__("fnstcw %0" : "=m"(v));
    return v;
}
static void wr_x87cw(uint16_t v) { __asm__ __volatile__("fldcw %0" : : "m"(v)); }
static uint16_t rd_x87sw(void) {
    uint16_t v;
    __asm__ __volatile__("fnstsw %0" : "=m"(v));
    return v;
}

/* msvcrt's word from the two hardware ones. On x64 the SSE unit does the
 * arithmetic, so its masks, rounding and flush bits are the answer; the x87's
 * only contribution is the precision control, which SSE has no use for. */
static uint32_t control_read(void) {
    uint32_t mx = rd_mxcsr(), cw = rd_x87cw(), r;
    uint32_t pc = (cw >> 8) & 3u;
    uint32_t ftz = (mx >> 15) & 1u, daz = (mx >> 6) & 1u;

    r = ms_from_hw6((mx >> 7) & 0x3Fu);
    r |= ((mx >> 13) & 3u) << 8;
    r |= (pc == 0u) ? 0x20000u : (pc == 2u ? 0x10000u : 0u);
    r |= ((ftz && daz) ? 1u : (daz ? 2u : (ftz ? 3u : 0u))) << 24;
    return r;
}

static void control_write(uint32_t ms) {
    uint32_t mx = rd_mxcsr(), cw = rd_x87cw();
    uint32_t em = hw6_from_ms(ms), rc = (ms >> 8) & 3u, dn = (ms >> 24) & 3u;
    uint32_t pc = (ms & 0x30000u) == 0x20000u ? 0u : ((ms & 0x30000u) == 0x10000u ? 2u : 3u);

    mx &= ~((0x3Fu << 7) | (3u << 13) | (1u << 15) | (1u << 6));
    mx |= (em << 7) | (rc << 13);
    mx |= ((dn == 1u || dn == 3u) ? 1u << 15 : 0u) | ((dn == 1u || dn == 2u) ? 1u << 6 : 0u);
    cw = (cw & ~(0x3Fu | (3u << 8) | (3u << 10))) | em | (pc << 8) | (rc << 10);
    wr_mxcsr(mx);
    wr_x87cw((uint16_t)cw);
}

uint32_t vc_control87(uint32_t value, uint32_t mask) {
    uint32_t cur = control_read();
    uint32_t m = mask & (VC_MCW_EM | VC_MCW_RC | VC_MCW_PC | VC_MCW_DN);

    if (m) {
        control_write((cur & ~m) | (value & m));
        cur = control_read();
    }
    return cur;
}

uint32_t vc_statusfp(void) { return ms_from_hw6((rd_mxcsr() | rd_x87sw()) & 0x3Fu); }

uint32_t vc_clearfp(void) {
    uint32_t r = vc_statusfp();
    wr_mxcsr(rd_mxcsr() & ~0x3Fu);
    __asm__ __volatile__("fnclex");
    return r;
}
#else
uint32_t vc_control87(uint32_t value, uint32_t mask) {
    (void)value;
    (void)mask;
    return 0;
}
uint32_t vc_statusfp(void) { return 0; }
uint32_t vc_clearfp(void) { return 0; }
#endif

/* ---- printf -------------------------------------------------------------- */

static void mem_put(vc_sink *b, uint16_t u) {
    vc_mem_sink *s = (vc_mem_sink *)b;
    if (b->total < s->cap) {
        if (s->wide) {
            ((uint16_t *)s->buf)[b->total] = u;
        } else {
            ((char *)s->buf)[b->total] = (char)u;
        }
    }
    b->total++;
}

void vc_mem_sink_init(vc_mem_sink *s, void *buf, size_t cap, int wide) {
    s->base.put = mem_put;
    s->base.total = 0;
    s->buf = buf;
    s->cap = cap;
    s->wide = wide;
}

static void rep(vc_sink *s, uint16_t c, long n) {
    while (n-- > 0) {
        s->put(s, c);
    }
}

typedef struct {
    int left, plus, space, alt, zero;
    long width;
    long prec;          /* -1: none */
} fspec;

/* An integer, already split into sign and magnitude. */
static void emit_integer(vc_sink *s, const fspec *f, uint64_t mag, int neg, int is_signed, unsigned base,
                         int upper) {
    char digs[72];
    char pre[2];
    int nd = 0, np = 0;
    long zeros, total, pad, k;
    const char *alpha = upper ? "0123456789ABCDEF" : "0123456789abcdef";

    if (!(mag == 0 && f->prec == 0)) {
        do {
            digs[nd++] = alpha[mag % base];
            mag /= base;
        } while (mag);
    }
    zeros = f->prec > nd ? f->prec - nd : 0;
    if (is_signed) {
        if (neg) {
            pre[np++] = '-';
        } else if (f->plus) {
            pre[np++] = '+';
        } else if (f->space) {
            pre[np++] = ' ';
        }
    } else if (f->alt && base == 16 && nd > 0 && !(nd == 1 && digs[0] == '0')) {
        pre[np++] = '0';
        pre[np++] = upper ? 'X' : 'x';
    } else if (f->alt && base == 8 && zeros == 0 && !(nd > 0 && digs[nd - 1] == '0')) {
        zeros = 1;
    }
    total = np + zeros + nd;
    if (f->zero && !f->left && f->prec < 0 && f->width > total) {
        zeros += f->width - total;
        total = f->width;
    }
    pad = f->width > total ? f->width - total : 0;
    if (!f->left) {
        rep(s, ' ', pad);
    }
    for (k = 0; k < np; k++) {
        s->put(s, (uint16_t)pre[k]);
    }
    rep(s, '0', zeros);
    for (k = nd; k-- > 0;) {
        s->put(s, (uint16_t)digs[k]);
    }
    if (f->left) {
        rep(s, ' ', pad);
    }
}

/* A string or a character, narrow or wide, into narrow or wide output. A wide
 * character that does not fit a byte becomes '?' - msvcrt would ask the code
 * page, and ours is ASCII. */
static uint16_t convert_unit(uint32_t ch, int arg_wide, int out_wide) {
    if (arg_wide) {
        return (!out_wide && ch >= 0x80u) ? (uint16_t)'?' : (uint16_t)ch;
    }
    return (uint16_t)(ch & 0xFFu);
}

static void emit_text(vc_sink *s, const fspec *f, const void *text, long n, int arg_wide, int out_wide) {
    long pad = f->width > n ? f->width - n : 0, k;

    /* msvcrt pads a string with zeros under the 0 flag; the standard calls that
     * undefined, so nobody can have depended on the other answer. */
    if (!f->left) {
        rep(s, f->zero ? '0' : ' ', pad);
    }
    for (k = 0; k < n; k++) {
        uint32_t ch = arg_wide ? ((const uint16_t *)text)[k] : ((const uint8_t *)text)[k];
        s->put(s, convert_unit(ch, arg_wide, out_wide));
    }
    if (f->left) {
        rep(s, ' ', pad);
    }
}

/* ---- exact decimal expansion of a double ---------------------------------- */

#define DEC_MAX 1500

/* All the decimal digits of v (finite, >= 0): the integer part's, then the
 * fraction's, which terminates because v is a dyadic rational. *intlen is how
 * many belong to the integer part (at least 1). Exact, which is the only way to
 * round correctly at any precision a caller asks for. */
static int dec_expand(double v, char *dig, int *intlen) {
    union {
        double d;
        uint64_t u;
    } x;
    uint64_t mant;
    int ex, n = 0;

    x.d = v;
    ex = (int)((x.u >> 52) & 0x7FFu);
    mant = x.u & 0xFFFFFFFFFFFFFull;
    /* A subnormal has no implicit leading bit and the smallest exponent; a
     * normal number has both. */
    if (ex == 0) {
        ex = 1;
    } else {
        mant |= 1ull << 52;
    }
    ex -= 1075;         /* v = mant * 2^ex */

    if (mant == 0) {
        dig[0] = '0';
        *intlen = 1;
        return 1;
    }
    /* An integer: mant shifted left by ex, as a big number in 32-bit limbs
     * (2^1024 needs 33), divided by 10^9 repeatedly - each remainder is nine
     * digits, least significant first. */
    if (ex >= 0) {
        uint32_t limb[36];
        char rev[400];
        int w = ex >> 5, sh = ex & 31, top, rn = 0, i;
        uint32_t a0 = (uint32_t)mant, a1 = (uint32_t)(mant >> 32);

        for (i = 0; i < 36; i++) {
            limb[i] = 0;
        }
        limb[w] |= a0 << sh;
        limb[w + 1] |= (sh ? a0 >> (32 - sh) : 0u) | (a1 << sh);
        limb[w + 2] |= sh ? a1 >> (32 - sh) : 0u;
        top = 35;
        for (;;) {
            uint32_t rem = 0;
            int nz = 0;
            while (top > 0 && limb[top] == 0) {
                top--;
            }
            for (i = top; i >= 0; i--) {
                uint64_t cur = ((uint64_t)rem << 32) | limb[i];
                limb[i] = (uint32_t)(cur / 1000000000u);
                rem = (uint32_t)(cur % 1000000000u);
                nz |= limb[i] != 0;
            }
            for (i = 0; i < 9; i++) {
                rev[rn++] = (char)('0' + rem % 10u);
                rem /= 10u;
            }
            if (!nz) {
                break;
            }
        }
        /* The last group was padded to nine digits: its leading zeros go. */
        while (rn > 1 && rev[rn - 1] == '0') {
            rn--;
        }
        while (rn > 0) {
            dig[n++] = rev[--rn];
        }
        *intlen = n;
        return n;
    }
    /* A fraction: the integer part from the bits above the binary point, then
     * the bits below it multiplied by ten until nothing is left - which
     * happens, because v is a multiple of 2^-1074. */
    {
        int s = -ex, i;
        uint64_t ip = s < 64 ? mant >> s : 0;
        uint64_t fr = s < 64 ? mant & ((1ull << s) - 1u) : mant;
        uint32_t fl[36];
        int nl = ((s + 4) >> 5) + 1;
        char rev[24];
        int rn = 0;

        do {
            rev[rn++] = (char)('0' + ip % 10u);
            ip /= 10u;
        } while (ip);
        while (rn > 0) {
            dig[n++] = rev[--rn];
        }
        *intlen = n;

        for (i = 0; i < 36; i++) {
            fl[i] = 0;
        }
        fl[0] = (uint32_t)fr;
        fl[1] = (uint32_t)(fr >> 32);
        for (;;) {
            uint64_t carry = 0;
            uint32_t digit = 0;
            int any = 0, b;

            for (i = 0; i < nl; i++) {
                uint64_t cur = (uint64_t)fl[i] * 10u + carry;
                fl[i] = (uint32_t)cur;
                carry = cur >> 32;
            }
            /* The digit is what the multiplication pushed above bit s. */
            for (b = 0; b < 4; b++) {
                int pos = s + b;
                digit |= ((fl[pos >> 5] >> (pos & 31)) & 1u) << b;
                fl[pos >> 5] &= ~(1u << (pos & 31));
            }
            for (i = 0; i < nl; i++) {
                any |= fl[i] != 0;
            }
            if (n < DEC_MAX) {
                dig[n++] = (char)('0' + digit);
            }
            if (!any) {
                break;
            }
        }
    }
    return n;
}

/* Round `dig` (n digits) to `keep` of them, a tie going away from zero. 1 if
 * the carry made the number a digit longer. */
static int round_digits(char *dig, int *n, int keep) {
    int k;
    int up;

    if (keep >= *n) {
        return 0;
    }
    up = dig[keep] >= '5';
    *n = keep;
    if (!up) {
        return 0;
    }
    for (k = keep - 1; k >= 0 && dig[k] == '9'; k--) {
        dig[k] = '0';
    }
    if (k >= 0) {
        dig[k]++;
        return 0;
    }
    for (k = *n; k > 0; k--) {
        dig[k] = dig[k - 1];
    }
    dig[0] = '1';
    (*n)++;
    return 1;
}

/* msvcrt's names for what is not a number. Returns the length, 0 if v is finite. */
static int nonfinite_text(double v, char *out, int *neg, int conv_e, long prec) {
    union {
        double d;
        uint64_t u;
    } x;
    const char *name;
    int n = 0;
    long k;

    x.d = v;
    if (((x.u >> 52) & 0x7FFu) != 0x7FFu) {
        return 0;
    }
    *neg = (int)(x.u >> 63);
    if ((x.u & 0xFFFFFFFFFFFFFull) == 0) {
        name = "#INF";
    } else if (x.u == 0xFFF8000000000000ull) {
        name = "#IND";
        *neg = 1;
    } else if (x.u & (1ull << 51)) {
        name = "#QNAN";
    } else {
        name = "#SNAN";
    }
    out[n++] = '1';
    if (prec != 0) {
        out[n++] = '.';
    }
    /* As many digits after the point as the precision asks for, the name first. */
    for (k = 0; k < (prec < 0 ? 6 : prec); k++) {
        out[n++] = k < (long)vc_strlen(name) ? name[k] : '0';
    }
    if (conv_e) {
        out[n++] = 'e';
        out[n++] = '+';
        out[n++] = '0';
        out[n++] = '0';
        out[n++] = '0';
    }
    return n;
}

static int put_exponent(char *out, int x, int upper) {
    int n = 0;
    char t[8];
    int k = 0;

    out[n++] = upper ? 'E' : 'e';
    out[n++] = x < 0 ? '-' : '+';
    if (x < 0) {
        x = -x;
    }
    do {
        t[k++] = (char)('0' + x % 10);
        x /= 10;
    } while (x);
    while (k < 3) {
        t[k++] = '0';
    }
    while (k > 0) {
        out[n++] = t[--k];
    }
    return n;
}

/* The body of %e, %f or %g: no sign, which the caller puts in front. */
static int float_body(char *out, double v, int conv, long prec, int alt, int *neg) {
    static char dig[DEC_MAX + 8];
    union {
        double d;
        uint64_t u;
    } x;
    int n, intlen, len = 0, upper = (conv == 'E' || conv == 'G'), lower_conv = conv | 0x20;
    int nf = nonfinite_text(v, out, neg, lower_conv == 'e', prec);

    if (nf) {
        return nf;
    }
    x.d = v;
    *neg = (int)(x.u >> 63);
    x.u &= ~(1ull << 63);
    if (prec > 1100) {
        prec = 1100;
    }
    n = dec_expand(x.d, dig, &intlen);

    if (lower_conv == 'f') {
        long p = prec < 0 ? 6 : prec, j;

        if (round_digits(dig, &n, intlen + (int)p)) {
            intlen++;
        }
        for (j = 0; j < intlen; j++) {
            out[len++] = dig[j];
        }
        if (p > 0 || alt) {
            out[len++] = '.';
        }
        for (j = 0; j < p; j++) {
            out[len++] = (intlen + j < n) ? dig[intlen + j] : '0';
        }
        return len;
    } else {
        long p = prec < 0 ? 6 : prec;      /* digits after the first, for e; significant, for g */
        long digits = (lower_conv == 'g') ? (p == 0 ? 1 : p) : p + 1, j;
        char *sd = dig;
        int first = 0, nn, x10;

        while (first < n && dig[first] == '0') {
            first++;
        }
        if (first == n) {
            /* Zero: every digit is zero and the exponent is too. */
            sd = dig;
            nn = 0;
            x10 = 0;
        } else {
            sd = dig + first;
            nn = n - first;
            x10 = intlen - 1 - first;
            if (round_digits(sd, &nn, (int)digits)) {
                x10++;
            }
        }
#define SD(j) (((j) < nn) ? sd[j] : '0')
        if (lower_conv == 'e' || x10 < -4 || x10 >= digits) {
            long frac = digits - 1;
            int start;

            out[len++] = SD(0);
            if (frac > 0 || alt) {
                out[len++] = '.';
            }
            start = len;
            for (j = 1; j <= frac; j++) {
                out[len++] = SD(j);
            }
            if (lower_conv == 'g' && !alt) {
                while (len > start && out[len - 1] == '0') {
                    len--;
                }
                if (out[len - 1] == '.') {
                    len--;      /* the point goes too */
                }
            }
            len += put_exponent(out + len, x10, upper);
        } else {
            int start;

            if (x10 >= 0) {
                for (j = 0; j <= x10; j++) {
                    out[len++] = SD(j);
                }
                out[len++] = '.';
                start = len;
                for (j = x10 + 1; j < digits; j++) {
                    out[len++] = SD(j);
                }
            } else {
                out[len++] = '0';
                out[len++] = '.';
                start = len;
                for (j = 0; j < -x10 - 1; j++) {
                    out[len++] = '0';
                }
                for (j = 0; j < digits; j++) {
                    out[len++] = SD(j);
                }
            }
            if (!alt) {
                while (len > start && out[len - 1] == '0') {
                    len--;
                }
                if (out[len - 1] == '.') {
                    len--;
                }
            }
        }
#undef SD
        return len;
    }
}

static void emit_float(vc_sink *s, const fspec *f, double v, int conv) {
    static char body[DEC_MAX * 2 + 64];
    int neg = 0, len, np = 0;
    char sign = 0;
    long total, zeros = 0, pad;
    int finite;
    union {
        double d;
        uint64_t u;
    } x;

    len = float_body(body, v, conv, f->prec, f->alt, &neg);
    x.d = v;
    finite = ((x.u >> 52) & 0x7FFu) != 0x7FFu;
    if (neg) {
        sign = '-';
    } else if (f->plus) {
        sign = '+';
    } else if (f->space) {
        sign = ' ';
    }
    np = sign ? 1 : 0;
    total = np + len;
    if (f->zero && !f->left && finite && f->width > total) {
        zeros = f->width - total;
        total = f->width;
    }
    pad = f->width > total ? f->width - total : 0;
    if (!f->left) {
        rep(s, ' ', pad);
    }
    if (sign) {
        s->put(s, (uint16_t)sign);
    }
    rep(s, '0', zeros);
    {
        int k;
        for (k = 0; k < len; k++) {
            s->put(s, (uint16_t)body[k]);
        }
    }
    if (f->left) {
        rep(s, ' ', pad);
    }
}

static uint32_t fmt_at(const void *f, int wide, size_t i) {
    return wide ? ((const uint16_t *)f)[i] : ((const uint8_t *)f)[i];
}

size_t vc_format(vc_sink *s, const void *fmt, int fw, va_list ap) {
    size_t i = 0;

    for (;;) {
        uint32_t c = fmt_at(fmt, fw, i);
        fspec f;
        int hcount = 0, lcount = 0, wmod = 0, big = 0;
        int ptr_sized = 0;

        if (!c) {
            break;
        }
        i++;
        if (c != '%') {
            s->put(s, (uint16_t)c);
            continue;
        }
        f.left = f.plus = f.space = f.alt = f.zero = 0;
        f.width = 0;
        f.prec = -1;
        for (;; i++) {
            c = fmt_at(fmt, fw, i);
            if (c == '-') f.left = 1;
            else if (c == '+') f.plus = 1;
            else if (c == ' ') f.space = 1;
            else if (c == '#') f.alt = 1;
            else if (c == '0') f.zero = 1;
            else break;
        }
        if (c == '*') {
            int w = va_arg(ap, int);
            if (w < 0) {
                f.left = 1;
                w = -w;
            }
            f.width = w;
            c = fmt_at(fmt, fw, ++i);
        } else {
            while (c >= '0' && c <= '9') {
                if (f.width < 100000) {
                    f.width = f.width * 10 + (long)(c - '0');
                }
                c = fmt_at(fmt, fw, ++i);
            }
        }
        if (c == '.') {
            c = fmt_at(fmt, fw, ++i);
            f.prec = 0;
            if (c == '*') {
                int p = va_arg(ap, int);
                f.prec = p < 0 ? -1 : p;
                c = fmt_at(fmt, fw, ++i);
            } else {
                while (c >= '0' && c <= '9') {
                    if (f.prec < 100000) {
                        f.prec = f.prec * 10 + (long)(c - '0');
                    }
                    c = fmt_at(fmt, fw, ++i);
                }
            }
        }
        for (;; c = fmt_at(fmt, fw, ++i)) {
            if (c == 'h') hcount++;
            else if (c == 'l') lcount++;
            else if (c == 'w') wmod = 1;
            else if (c == 'L' || c == 'z' || c == 't' || c == 'j' || c == 'q') big = 1;
            else if (c == 'I') {
                if (fmt_at(fmt, fw, i + 1) == '6' && fmt_at(fmt, fw, i + 2) == '4') {
                    big = 1;
                    i += 2;
                } else if (fmt_at(fmt, fw, i + 1) == '3' && fmt_at(fmt, fw, i + 2) == '2') {
                    i += 2;
                } else {
                    ptr_sized = 1;
                }
            } else break;
        }
        if (!c) {
            break;
        }
        i++;
        {
            int is64 = (lcount >= 2) || big || ptr_sized;

            switch (c) {
            case 'd':
            case 'i': {
                int64_t v;
                if (is64) v = va_arg(ap, int64_t);
                else if (hcount >= 2) v = (signed char)va_arg(ap, int);
                else if (hcount == 1) v = (short)va_arg(ap, int);
                else v = va_arg(ap, int);
                emit_integer(s, &f, v < 0 ? 0u - (uint64_t)v : (uint64_t)v, v < 0, 1, 10, 0);
                break;
            }
            case 'u':
            case 'o':
            case 'x':
            case 'X': {
                uint64_t v;
                if (is64) v = va_arg(ap, uint64_t);
                else if (hcount >= 2) v = (uint8_t)va_arg(ap, unsigned);
                else if (hcount == 1) v = (uint16_t)va_arg(ap, unsigned);
                else v = va_arg(ap, unsigned);
                emit_integer(s, &f, v, 0, 0, c == 'u' ? 10u : (c == 'o' ? 8u : 16u), c == 'X');
                break;
            }
            case 'p': {
                uint64_t v = (uint64_t)(uintptr_t)va_arg(ap, void *);
                fspec p = f;
                p.prec = 16;
                p.plus = p.space = p.alt = p.zero = 0;
                emit_integer(s, &p, v, 0, 0, 16, 1);
                break;
            }
            case 'e':
            case 'E':
            case 'f':
            case 'F':
            case 'g':
            case 'G':
                emit_float(s, &f, va_arg(ap, double), c == 'F' ? 'f' : (int)c);
                break;
            case 'c':
            case 'C': {
                /* %c is the function's own width of character, %C the other. */
                int arg_wide = (c == 'c') ? fw : !fw;
                uint16_t ch;
                if (lcount || wmod) arg_wide = 1;
                if (hcount) arg_wide = 0;
                ch = (uint16_t)va_arg(ap, int);
                if (!arg_wide) {
                    ch &= 0xFFu;
                }
                emit_text(s, &f, &ch, 1, arg_wide, fw);
                break;
            }
            case 's':
            case 'S': {
                int arg_wide = (c == 's') ? fw : !fw;
                const void *p;
                long n = 0;
                static const char nul[] = "(null)";
                if (lcount || wmod) arg_wide = 1;
                if (hcount) arg_wide = 0;
                p = va_arg(ap, const void *);
                if (!p) {
                    p = nul;
                    arg_wide = 0;
                }
                if (arg_wide) {
                    const uint16_t *w = p;
                    while ((f.prec < 0 || n < f.prec) && w[n]) n++;
                } else {
                    const uint8_t *b = p;
                    while ((f.prec < 0 || n < f.prec) && b[n]) n++;
                }
                emit_text(s, &f, p, n, arg_wide, fw);
                break;
            }
            case 'n':
                /* Writing through the pointer is how a format string turns
                 * into a write primitive; msvcrt itself disabled it later. The
                 * argument is consumed so the ones after it still line up. */
                (void)va_arg(ap, void *);
                break;
            case '%':
                s->put(s, '%');
                break;
            default:
                s->put(s, (uint16_t)c);
                break;
            }
        }
    }
    return s->total;
}

int vc_vsnprintf(char *buf, size_t count, const char *fmt, va_list ap) {
    vc_mem_sink m;
    size_t n;

    vc_mem_sink_init(&m, buf, count, 0);
    n = vc_format(&m.base, fmt, 0, ap);
    if (n > count) {
        return -1;
    }
    if (n < count) {
        buf[n] = 0;
    }
    return (int)n;
}

int vc_vsnwprintf(uint16_t *buf, size_t count, const uint16_t *fmt, va_list ap) {
    vc_mem_sink m;
    size_t n;

    vc_mem_sink_init(&m, buf, count, 1);
    n = vc_format(&m.base, fmt, 1, ap);
    if (n > count) {
        return -1;
    }
    if (n < count) {
        buf[n] = 0;
    }
    return (int)n;
}

int vc_vsprintf(char *buf, const char *fmt, va_list ap) {
    vc_mem_sink m;
    size_t n;

    vc_mem_sink_init(&m, buf, (size_t)-1 >> 1, 0);
    n = vc_format(&m.base, fmt, 0, ap);
    buf[n] = 0;
    return (int)n;
}

/* ---- scanf --------------------------------------------------------------- */

static double pow10_int(int e) {
    double r = 1.0, b = 10.0;
    int neg = e < 0;
    if (neg) {
        e = -e;
    }
    while (e) {
        if (e & 1) {
            r *= b;
        }
        b *= b;
        e >>= 1;
    }
    return neg ? 1.0 / r : r;
}

/* sscanf as msvcrt has it: the return is the number of assignments, or EOF
 * when the input ran out before the first conversion - which is why running
 * out (input_failure) and a mismatch (done) are two exits, not one. */
int vc_vsscanf(const char *s, const char *f, va_list ap) {
    const char *start = s;
    int assigned = 0, nconv = 0;

    while (*f) {
        int suppress = 0, hcount = 0, lcount = 0, big = 0, wide = 0, ptr = 0;
        long width = 0;
        int conv;

        /* White space in the format matches any amount of it, none included. */
        if (is_space((uint8_t)*f)) {
            while (is_space((uint8_t)*f)) f++;
            while (is_space((uint8_t)*s)) s++;
            continue;
        }
        if (*f != '%') {
            if (*s == 0) goto input_failure;
            if (*s != *f) goto done;
            s++;
            f++;
            continue;
        }
        /* A directive: assignment suppression, width, then msvcrt's size
         * prefixes - I64 and I32 included - and the conversion. */
        f++;
        if (*f == '*') {
            suppress = 1;
            f++;
        }
        while (is_digit((uint8_t)*f)) {
            width = width * 10 + (*f - '0');
            f++;
        }
        for (;; f++) {
            if (*f == 'h') hcount++;
            else if (*f == 'l') lcount++;
            else if (*f == 'w') wide = 1;
            else if (*f == 'L') big = 1;
            else if (*f == 'I') {
                if (f[1] == '6' && f[2] == '4') { big = 1; f += 2; }
                else if (f[1] == '3' && f[2] == '2') { f += 2; }
                else ptr = 1;
            } else break;
        }
        conv = (uint8_t)*f;
        if (!conv) goto done;
        f++;

        switch (conv) {
        case '%':
            while (is_space((uint8_t)*s)) s++;
            if (*s == 0) goto input_failure;
            if (*s != '%') goto done;
            s++;
            break;
        case 'n':
            /* Characters consumed so far; not counted as an assignment. */
            if (!suppress) {
                *va_arg(ap, int *) = (int)(s - start);
            }
            break;
        case 'd': case 'i': case 'u': case 'o': case 'x': case 'X': {
            int base = conv == 'o' ? 8 : ((conv == 'x' || conv == 'X') ? 16 : (conv == 'i' ? 0 : 10));
            int neg = 0, any = 0;
            uint64_t v = 0;
            long left = width ? width : 0x7FFFFFFF;

            while (is_space((uint8_t)*s)) s++;
            if (*s == 0) goto input_failure;
            if ((*s == '-' || *s == '+') && left > 0) {
                neg = (*s == '-');
                s++;
                left--;
            }
            /* "0x" is a prefix only if a hex digit follows it and the width
             * leaves room; otherwise the 0 is the number. */
            if ((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X') && left > 2 &&
                digit_value((uint8_t)s[2]) < 16) {
                s += 2;
                left -= 2;
                base = 16;
            } else if (base == 0) {
                base = (s[0] == '0') ? 8 : 10;
            }
            while (left > 0 && digit_value((uint8_t)*s) < base) {
                v = v * (uint64_t)base + (uint64_t)digit_value((uint8_t)*s);
                s++;
                left--;
                any = 1;
            }
            if (!any) goto done;
            if (neg) v = 0u - v;
            if (!suppress) {
                if (big || ptr || lcount >= 2) *va_arg(ap, uint64_t *) = v;
                else if (hcount >= 2) *va_arg(ap, uint8_t *) = (uint8_t)v;
                else if (hcount == 1) *va_arg(ap, uint16_t *) = (uint16_t)v;
                else *va_arg(ap, uint32_t *) = (uint32_t)v;
                assigned++;
            }
            break;
        }
        case 'e': case 'E': case 'f': case 'g': case 'G': {
            int neg = 0, any = 0, e10 = 0;
            uint64_t m = 0;
            double r;
            long left = width ? width : 0x7FFFFFFF;

            while (is_space((uint8_t)*s)) s++;
            if (*s == 0) goto input_failure;
            if ((*s == '-' || *s == '+') && left > 0) {
                neg = (*s == '-');
                s++;
                left--;
            }
            /* Digits past the nineteenth cannot change a double's nearest
             * neighbour by more than the rounding below already does; they
             * only move the point. This is not a correctly rounded strtod. */
            while (left > 0 && is_digit((uint8_t)*s)) {
                if (m < 1000000000000000000ull) m = m * 10u + (uint64_t)(*s - '0');
                else e10++;
                s++; left--; any = 1;
            }
            if (left > 0 && *s == '.') {
                s++; left--;
                while (left > 0 && is_digit((uint8_t)*s)) {
                    if (m < 1000000000000000000ull) { m = m * 10u + (uint64_t)(*s - '0'); e10--; }
                    s++; left--; any = 1;
                }
            }
            if (!any) goto done;
            if (left > 1 && (*s == 'e' || *s == 'E')) {
                const char *p = s + 1;
                long l2 = left - 1;
                int eneg = 0, ev = 0, eany = 0;
                if ((*p == '-' || *p == '+') && l2 > 1) { eneg = (*p == '-'); p++; l2--; }
                while (l2 > 0 && is_digit((uint8_t)*p)) {
                    if (ev < 10000) ev = ev * 10 + (*p - '0');
                    p++; l2--; eany = 1;
                }
                if (eany) {
                    e10 += eneg ? -ev : ev;
                    s = p;
                }
            }
            r = (double)m;
            if (e10 > 0 && e10 < 700) r *= pow10_int(e10);
            else if (e10 < 0 && e10 > -700) r /= pow10_int(-e10);
            else if (e10 >= 700) r *= pow10_int(300) * pow10_int(e10 - 300);
            if (neg) r = -r;
            if (!suppress) {
                if (lcount || big) *va_arg(ap, double *) = r;
                else *va_arg(ap, float *) = (float)r;
                assigned++;
            }
            break;
        }
        case 's': {
            long left = width ? width : 0x7FFFFFFF;
            void *dst = suppress ? NULL : va_arg(ap, void *);
            long n = 0;

            while (is_space((uint8_t)*s)) s++;
            if (*s == 0) goto input_failure;
            while (left > 0 && *s && !is_space((uint8_t)*s)) {
                if (dst) {
                    if (lcount || wide) ((uint16_t *)dst)[n] = (uint8_t)*s;
                    else ((char *)dst)[n] = *s;
                }
                n++; s++; left--;
            }
            if (dst) {
                if (lcount || wide) ((uint16_t *)dst)[n] = 0;
                else ((char *)dst)[n] = 0;
                assigned++;
            }
            break;
        }
        case 'c': {
            long want = width ? width : 1, n;
            void *dst = suppress ? NULL : va_arg(ap, void *);

            for (n = 0; n < want; n++) {
                if (s[n] == 0) {
                    if (n == 0) goto input_failure;
                    goto done;
                }
            }
            for (n = 0; n < want; n++) {
                if (dst) {
                    if (lcount || wide) ((uint16_t *)dst)[n] = (uint8_t)s[n];
                    else ((char *)dst)[n] = s[n];
                }
            }
            s += want;
            if (dst) assigned++;
            break;
        }
        case '[': {
            uint8_t set[256];
            int negate = 0, k;
            long left = width ? width : 0x7FFFFFFF, n = 0;
            void *dst;

            for (k = 0; k < 256; k++) set[k] = 0;
            if (*f == '^') { negate = 1; f++; }
            if (*f == ']') { set[(uint8_t)']'] = 1; f++; }
            while (*f && *f != ']') {
                if (f[1] == '-' && f[2] && f[2] != ']') {
                    int a = (uint8_t)f[0], b = (uint8_t)f[2];
                    for (k = a; k <= b; k++) set[k] = 1;
                    f += 3;
                } else {
                    set[(uint8_t)*f] = 1;
                    f++;
                }
            }
            if (*f == ']') f++;
            dst = suppress ? NULL : va_arg(ap, void *);
            if (*s == 0) goto input_failure;
            while (left > 0 && *s && (set[(uint8_t)*s] != 0) != negate) {
                if (dst) {
                    if (lcount || wide) ((uint16_t *)dst)[n] = (uint8_t)*s;
                    else ((char *)dst)[n] = *s;
                }
                n++; s++; left--;
            }
            if (n == 0) goto done;
            if (dst) {
                if (lcount || wide) ((uint16_t *)dst)[n] = 0;
                else ((char *)dst)[n] = 0;
                assigned++;
            }
            break;
        }
        default:
            goto done;
        }
        if (conv != 'n') nconv++;
    }
done:
    return assigned;
input_failure:
    return nconv == 0 ? -1 : assigned;
}

/* ---- the command line ---------------------------------------------------- */

typedef struct {
    char *out;
    size_t cap, pos;
    size_t *offsets;
    int max_args, argc;
} argsink;

static void arg_byte(argsink *a, uint8_t b) {
    if (a->out && a->pos < a->cap) {
        a->out[a->pos] = (char)b;
    }
    a->pos++;
}

static void arg_begin(argsink *a) {
    if (a->offsets && a->argc < a->max_args) {
        a->offsets[a->argc] = a->pos;
    }
}

static void arg_end(argsink *a) {
    arg_byte(a, 0);
    a->argc++;
}

/* One code point as UTF-8; units is how many UTF-16 units it used. A lone
 * surrogate has no code point, and becomes U+FFFD. */
static size_t arg_unit(argsink *a, const uint16_t *cmd, size_t i, size_t n) {
    uint32_t c = cmd[i];
    size_t used = 1;

    if (c >= 0xD800u && c < 0xDC00u && i + 1 < n && cmd[i + 1] >= 0xDC00u && cmd[i + 1] < 0xE000u) {
        c = 0x10000u + ((c - 0xD800u) << 10) + (cmd[i + 1] - 0xDC00u);
        used = 2;
    } else if (c >= 0xD800u && c < 0xE000u) {
        c = 0xFFFDu;
    }
    if (c < 0x80u) {
        arg_byte(a, (uint8_t)c);
    } else if (c < 0x800u) {
        arg_byte(a, (uint8_t)(0xC0u | (c >> 6)));
        arg_byte(a, (uint8_t)(0x80u | (c & 0x3Fu)));
    } else if (c < 0x10000u) {
        arg_byte(a, (uint8_t)(0xE0u | (c >> 12)));
        arg_byte(a, (uint8_t)(0x80u | ((c >> 6) & 0x3Fu)));
        arg_byte(a, (uint8_t)(0x80u | (c & 0x3Fu)));
    } else {
        arg_byte(a, (uint8_t)(0xF0u | (c >> 18)));
        arg_byte(a, (uint8_t)(0x80u | ((c >> 12) & 0x3Fu)));
        arg_byte(a, (uint8_t)(0x80u | ((c >> 6) & 0x3Fu)));
        arg_byte(a, (uint8_t)(0x80u | (c & 0x3Fu)));
    }
    return used;
}

int vc_parse_cmdline(const uint16_t *cmd, size_t n, char *out, size_t cap, size_t *offsets, int max_args,
                     size_t *bytes) {
    argsink a;
    size_t i = 0;

    a.out = out;
    a.cap = cap;
    a.pos = 0;
    a.offsets = offsets;
    a.max_args = max_args;
    a.argc = 0;

    /* The program name has rules of its own: a quote ends it and nothing is
     * escaped, because a path may end in a backslash. */
    arg_begin(&a);
    if (i < n && cmd[i] == '"') {
        i++;
        while (i < n && cmd[i] != '"') {
            i += arg_unit(&a, cmd, i, n);
        }
        if (i < n) {
            i++;
        }
    } else {
        while (i < n && cmd[i] != ' ' && cmd[i] != '\t') {
            i += arg_unit(&a, cmd, i, n);
        }
    }
    arg_end(&a);

    for (;;) {
        int inq = 0;

        while (i < n && (cmd[i] == ' ' || cmd[i] == '\t')) {
            i++;
        }
        if (i >= n) {
            break;
        }
        arg_begin(&a);
        while (i < n) {
            uint16_t c = cmd[i];

            if (!inq && (c == ' ' || c == '\t')) {
                break;
            }
            if (c == '\\') {
                size_t count = 0, k;

                while (i < n && cmd[i] == '\\') {
                    count++;
                    i++;
                }
                if (i < n && cmd[i] == '"') {
                    /* 2n backslashes and a quote: n backslashes, and the quote
                     * toggles. 2n+1: n backslashes and a literal quote. */
                    for (k = 0; k < count / 2; k++) {
                        arg_byte(&a, '\\');
                    }
                    if (count & 1u) {
                        arg_byte(&a, '"');
                    } else {
                        inq = !inq;
                    }
                    i++;
                } else {
                    for (k = 0; k < count; k++) {
                        arg_byte(&a, '\\');
                    }
                }
                continue;
            }
            if (c == '"') {
                inq = !inq;
                i++;
                continue;
            }
            i += arg_unit(&a, cmd, i, n);
        }
        arg_end(&a);
    }
    if (bytes) {
        *bytes = a.pos;
    }
    return a.argc;
}
