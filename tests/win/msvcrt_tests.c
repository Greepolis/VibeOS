/* msvcrt.dll's portable core, on the host (docs/abi/windows.md W2).
 *
 * crt_core.c is compiled twice: by mingw-w64 into the DLL and by the host's
 * compiler into this program. What can be compared against something this
 * project did not write is compared:
 *  - printf against glibc's snprintf for every format on which msvcrt and C99
 *    agree, a few thousand generated cases from a seed that is printed;
 *  - the exact-digits rounding of %e/%f/%g against glibc's, with the cases
 *    where msvcrt and glibc part (a tie, which msvcrt rounds away from zero)
 *    found by asking glibc for the exact expansion and left out;
 *  - the heap against a model that knows only what was handed out;
 *  - acos against libm, in ulps;
 *  - the argv rules against the examples Microsoft publishes.
 * Where msvcrt differs from C99 on purpose - %ld is 32 bits, %p has no 0x,
 * _vsnprintf returns -1 - the expected string is written out from msvcrt's
 * documentation, because glibc would say the other thing.
 *
 * Usage: vibeos_msvcrt_tests [seed]. A failure prints the seed in the summary. */

#define _GNU_SOURCE
#include <fenv.h>
#include <inttypes.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "crt_core.h"

static int g_checks, g_failed;
static uint64_t g_seed = 0x5eed1234abcdULL;

#define CHECK(cond, ...)                                                  \
    do {                                                                  \
        g_checks++;                                                       \
        if (!(cond)) {                                                    \
            g_failed++;                                                   \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                   \
            printf(__VA_ARGS__);                                          \
            printf("\n");                                                 \
        }                                                                 \
    } while (0)

static uint64_t rnd(uint64_t *s) {
    uint64_t x = *s;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *s = x;
    return x * 0x2545F4914F6CDD1DULL;
}

static uint32_t rn(uint64_t *s, uint32_t n) { return (uint32_t)(rnd(s) % n); }

/* ---- printf: msvcrt's documented behaviour ------------------------------- */

static char g_buf[8192];

static const char *fm(const char *fmt, ...) {
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vc_vsnprintf(g_buf, sizeof g_buf, fmt, ap);
    va_end(ap);
    if (n < 0) {
        strcpy(g_buf, "<-1>");
    }
    return g_buf;
}

#define EXPECT(expected, ...)                                                          \
    do {                                                                               \
        const char *got_ = fm(__VA_ARGS__);                                            \
        CHECK(strcmp(got_, (expected)) == 0, "[%s] gave [%s], wanted [%s]",            \
              #__VA_ARGS__, got_, (expected));                                         \
    } while (0)

static void widen(uint16_t *d, const char *s) {
    while ((*d++ = (uint8_t)*s++) != 0) {
    }
}

/* Format with the wide function; the answer back as narrow text, non-ASCII as '~'. */
static const char *wfm(const char *fmt, ...) {
    uint16_t wf[256], out[2048];
    va_list ap;
    int n, i;

    widen(wf, fmt);
    va_start(ap, fmt);
    n = vc_vsnwprintf(out, 2048, wf, ap);
    va_end(ap);
    if (n < 0) {
        strcpy(g_buf, "<-1>");
        return g_buf;
    }
    for (i = 0; i < n; i++) {
        g_buf[i] = out[i] < 0x80 ? (char)out[i] : '~';
    }
    g_buf[n] = 0;
    return g_buf;
}

#define WEXPECT(expected, ...)                                                         \
    do {                                                                               \
        const char *got_ = wfm(__VA_ARGS__);                                           \
        CHECK(strcmp(got_, (expected)) == 0, "wide [%s] gave [%s], wanted [%s]",       \
              #__VA_ARGS__, got_, (expected));                                         \
    } while (0)

static void test_printf_documented(void) {
    static const uint16_t abc[] = {'a', 'b', 'c', 0};
    static const uint16_t smile[] = {'a', 0x263A, 'c', 0};
    int n = 77;
    union {
        double d;
        uint64_t u;
    } nan1 = {.u = 0x7FF8000000000001ull}, ind = {.u = 0xFFF8000000000000ull};

    EXPECT("42", "%d", 42);
    EXPECT("-42|   42|42   |00042", "%d|%5d|%-5d|%05d", -42, 42, 42, 42);
    EXPECT("+42| 42", "%+d|% d", 42, 42);
    EXPECT("ff|FF|0xff|0XFF|377|010", "%x|%X|%#x|%#X|%o|%#o", 255, 255, 255, 255, 255, 8);
    EXPECT("4294967295", "%u", -1);
    EXPECT("|0007|  007|  007|+0042|", "%.0d|%.*d|%5.3d|%05.3d|%+05d|", 0, 4, 7, 7, 7, 42);
    EXPECT("   42|42   |", "%*d|%-*d|", 5, 42, 5, 42);
    EXPECT("42   |", "%*d|", -5, 42);

    /* msvcrt's long is 32 bits: an eight-byte argument is read as four. */
    EXPECT("5", "%ld", 0x100000005LL);
    EXPECT("5", "%lu", 0x100000005LL);
    EXPECT("34567890", "%lx", 0x1234567890LL);
    EXPECT("-5|7", "%ld|%d", -5, 7);
    /* ...and the 64-bit spellings read eight. */
    EXPECT("4294967301", "%I64d", 0x100000005LL);
    EXPECT("4294967301", "%I64u", 0x100000005LL);
    EXPECT("1234567890", "%I64x", 0x1234567890LL);
    EXPECT("4294967301", "%lld", 0x100000005LL);
    EXPECT("1234567890", "%llx", 0x1234567890LL);
    EXPECT("-4294967291", "%I64d", -0xFFFFFFFBLL);
    EXPECT("4294967301", "%Id", (intptr_t)0x100000005LL);
    EXPECT("1234567890", "%Ix", (uintptr_t)0x1234567890ULL);
    EXPECT("5", "%I32d", 0x100000005LL);
    EXPECT("5|1", "%hd|%hu", 0x10005, 65537);
    EXPECT("18446744073709551615", "%I64u", -1LL);
    EXPECT("-9223372036854775808", "%I64d", INT64_MIN);

    EXPECT("A|  A|A  |", "%c|%3c|%-3c|", 'A', 'A', 'A');
    EXPECT("hello|hel|   hello|hello   |", "%s|%.3s|%8s|%-8s|", "hello", "hello", "hello", "hello");
    EXPECT("(null)", "%s", (char *)NULL);
    EXPECT("%", "%%");

    /* Wide arguments, narrowed: below 0x80 as they are, the rest a '?'. */
    EXPECT("abc|a?c", "%ls|%ls", abc, smile);
    EXPECT("abc", "%S", abc);
    EXPECT("A|?", "%wc|%wc", 0x41, 0x263A);
    EXPECT("A|?", "%C|%lc", 0x41, 0x263A);

    EXPECT("0000000000001234|0000000000000000|00000000DEADBEEF", "%p|%p|%p", (void *)0x1234, (void *)0,
           (void *)0xDEADBEEFull);
    EXPECT("      0000000000001234|0000000000001234      |", "%22p|%-22p|", (void *)0x1234, (void *)0x1234);

    /* %n is refused: nothing is written through the pointer, nothing printed. */
    EXPECT("abcd", "ab%ncd", &n);
    CHECK(n == 77, "%%n wrote through its pointer: %d", n);

    EXPECT("1.500000e+000", "%e", 1.5);
    EXPECT("0.000000e+000", "%e", 0.0);
    EXPECT("1.23e+004", "%.2e", 12345.678);
    EXPECT("1.000000e+100", "%e", 1e100);
    EXPECT("1.000000e-005", "%e", 1e-5);
    EXPECT("-1.500000E+000", "%E", -1.5);
    EXPECT("1e+000", "%.0e", 1.0);
    EXPECT("1.e+000", "%#.0e", 1.0);
    EXPECT("3.141590", "%f", 3.14159);
    EXPECT("3|1|2|-3", "%.0f|%.0f|%.0f|%.0f", 2.5, 0.5, 1.5, -2.5);    /* ties go away from zero */
    EXPECT("1.00", "%.2f", 1.005);                                      /* below the tie, in binary */
    EXPECT("     3.142|3.142     |", "%10.3f|%-10.3f|", 3.14159, 3.14159);
    EXPECT("+0.5| 0.5", "%+.1f|% .1f", 0.5, 0.5);
    EXPECT("1000000000000000.000000", "%f", 1e15);
    EXPECT("-0.000000", "%f", -0.0);
    EXPECT("3.", "%#.0f", 3.0);
    /* Rounding that carries out of the leading digit makes the number longer. */
    EXPECT("10.00|10|100|1.0", "%.2f|%.0f|%.0f|%.1f", 9.999, 9.5, 99.5, 0.96);
    EXPECT("1.000000e+001|1e+001|1e+006|100000|10|1.00000e+006", "%e|%.0e|%g|%g|%.3g|%#g", 9.9999999, 9.5, 999999.5, 99999.96, 9.9996, 999999.5);
    EXPECT("0.000001", "%f", 1e-6);
    EXPECT("0.000000", "%f", 4e-7);
    EXPECT("100000|1e+006|0.0001|1e-005|1.23457e+008", "%g|%g|%g|%g|%g", 100000.0, 1000000.0, 0.0001, 0.00001,
           123456789.0);
    EXPECT("3.14|0.5|1.00000|1E-010", "%.3g|%g|%#g|%G", 3.14159, 0.5, 1.0, 1e-10);
    EXPECT("0|0.1|10", "%g|%g|%g", 0.0, 0.1, 10.0);
    EXPECT("1.#INF00|-1.#INF00|1.#INF00e+000", "%f|%f|%e", INFINITY, -INFINITY, INFINITY);
    EXPECT("1.#QNAN0|-1.#IND00", "%f|%f", nan1.d, ind.d);
    EXPECT("1.797693e+308", "%e", 1.7976931348623157e308);
    EXPECT("4.940656e-324", "%e", 4.9406564584124654e-324);

    EXPECT("00hi", "%04s", "hi");
    EXPECT("x", "x%");
    EXPECT("", "");
}

static void test_wide_printf(void) {
    static const uint16_t wide[] = {'w', 'i', 'd', 'e', 0};

    /* In a wide format %s is a wide string and %S a narrow one: the other way
     * round from printf. */
    WEXPECT("wide|narrow", "%s|%S", wide, "narrow");
    WEXPECT("wide|narrow", "%ls|%hs", wide, "narrow");
    WEXPECT("A|B", "%c|%C", 0x41, 'B');
    WEXPECT("12|-3|ff", "%d|%i|%x", 12, -3, 255);
    WEXPECT("1.500000e+000", "%e", 1.5);
    WEXPECT("5", "%ld", 0x100000005LL);
    WEXPECT("4294967301", "%I64u", 0x100000005LL);
}

/* The wide function with a wide argument kept wide, which wfm's narrowing hides. */
static int wide_keep(uint16_t *out, size_t cap, const char *fmt, ...) {
    uint16_t wf[64];
    va_list ap;
    int n;

    widen(wf, fmt);
    va_start(ap, fmt);
    n = vc_vsnwprintf(out, cap, wf, ap);
    va_end(ap);
    return n;
}

static void test_wide_keep(void) {
    static const uint16_t smile[] = {'a', 0x263A, 0};
    uint16_t out[16];
    int n = wide_keep(out, 16, "%s|%c", smile, 0x263A);

    CHECK(n == 4 && out[0] == 'a' && out[1] == 0x263A && out[2] == '|' && out[3] == 0x263A && out[4] == 0,
          "wide output lost a non-ASCII unit: n=%d %04x %04x", n, out[1], out[3]);

    /* Truncation, wide: -1, units written, no terminator. */
    {
        uint16_t b[8];
        int k;
        for (k = 0; k < 8; k++) b[k] = 0x5A5A;
        n = wide_keep(b, 5, "hello world");
        CHECK(n == -1 && b[0] == 'h' && b[4] == 'o' && b[5] == 0x5A5A, "wide truncation: n=%d b5=%04x", n, b[5]);
        for (k = 0; k < 8; k++) b[k] = 0x5A5A;
        n = wide_keep(b, 5, "hello");
        CHECK(n == 5 && b[4] == 'o' && b[5] == 0x5A5A, "wide exact fit: n=%d b5=%04x", n, b[5]);
        n = wide_keep(b, 6, "hello");
        CHECK(n == 5 && b[5] == 0, "wide fit with room: n=%d b5=%04x", n, b[5]);
    }
}

static int trunc_call(char *buf, size_t count, const char *fmt, ...) {
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vc_vsnprintf(buf, count, fmt, ap);
    va_end(ap);
    return n;
}

static void test_truncation(void) {
    char b[16];
    int n;

    /* More than fits: -1, as many characters as there is room for, no NUL. */
    memset(b, 'Z', sizeof b);
    n = trunc_call(b, 5, "%s", "hello world");
    CHECK(n == -1, "truncated _vsnprintf returned %d, wanted -1", n);
    CHECK(memcmp(b, "helloZ", 6) == 0, "truncated output: [%.6s]", b);

    /* Exactly the room there is: the count, and still no NUL. */
    memset(b, 'Z', sizeof b);
    n = trunc_call(b, 5, "%s", "hello");
    CHECK(n == 5 && memcmp(b, "helloZ", 6) == 0, "exact fit: n=%d [%.6s]", n, b);

    /* One more byte of room: the count, and the NUL. */
    memset(b, 'Z', sizeof b);
    n = trunc_call(b, 6, "%s", "hello");
    CHECK(n == 5 && memcmp(b, "hello\0Z", 7) == 0, "fit with room: n=%d", n);

    CHECK(trunc_call(b, 0, "%d", 5) == -1, "zero-size buffer, nonempty output");
    CHECK(trunc_call(b, 0, "%s", "") == 0, "zero-size buffer, empty output");
}

/* ---- printf against glibc ------------------------------------------------ */

static int g_glibc_compared, g_glibc_ties;

static const char *g_strpool[] = {"", "a", "hello", "The quick brown fox", "x y z", "0123456789abcdef"};

#ifdef __GLIBC__

/* The exponent glibc prints has two digits at least, msvcrt's three: take the
 * leading zero off ours when there is one to take. Returns 1 if it did, because
 * the field width then counts one character fewer and the reference format has
 * to be told. */
static int shrink_exponent(char *s) {
    char *p;
    for (p = s; *p; p++) {
        if ((*p == 'e' || *p == 'E') && (p[1] == '+' || p[1] == '-') && p[2] == '0' && p[3] >= '0' &&
            p[3] <= '9' && p[4] >= '0' && p[4] <= '9' && !(p[5] >= '0' && p[5] <= '9')) {
            memmove(p + 2, p + 3, strlen(p + 3) + 1);
            return 1;
        }
    }
    return 0;
}

/* Is v exactly halfway at the digit this conversion rounds to? glibc breaks the
 * tie to even and msvcrt away from zero, so those cases cannot be compared. */
static int is_tie(double v, int conv, long prec) {
    static char t[2048];
    const char *p;
    long idx;

    if (conv == 'f') {
        if (prec < 0) prec = 6;
        snprintf(t, sizeof t, "%.1100f", fabs(v));
        p = strchr(t, '.');
        idx = (p - t) + 1 + prec;
    } else {
        long digits;
        if (conv == 'e' || conv == 'E') digits = (prec < 0 ? 6 : prec) + 1;
        else digits = prec < 0 ? 6 : (prec == 0 ? 1 : prec);
        snprintf(t, sizeof t, "%.800e", fabs(v));
        idx = digits + 1;           /* d.ddd: significant digit k (k >= 1) is at t[k + 1] */
    }
    if (idx >= (long)strlen(t) || t[idx] != '5') {
        return 0;
    }
    for (p = t + idx + 1; *p && *p != 'e'; p++) {
        if (*p != '0') return 0;
    }
    return 1;
}

/* glibc's %#g forgets, when rounding carries out of the leading digit (999999.5
 * to 1e+06), that the carry made a digit it still has to show: it prints "1.e+06"
 * where the standard, and msvcrt, say "1.00000e+06". Does v's exponent change
 * when it is rounded to `digits` significant digits? */
static int rounding_carries(double v, long digits) {
    char a[64], b[1024];
    const char *ea, *eb;

    snprintf(a, sizeof a, "%.*e", (int)(digits - 1), v);
    snprintf(b, sizeof b, "%.800e", v);
    ea = strchr(a, 'e');
    eb = strchr(b, 'e');
    return ea && eb && strcmp(ea, eb) != 0;
}

typedef struct {
    char flags[8];
    int width;      /* 0: none */
    long prec;      /* -1: none */
} spec;

static void gen_spec(uint64_t *r, spec *sp, int allow_zero) {
    static const char flagset[] = "-+ #0";
    int nf = (int)rn(r, 4), k, n = 0;

    for (k = 0; k < nf; k++) {
        char c = flagset[rn(r, 5)];
        if (c == '0' && !allow_zero) continue;
        sp->flags[n++] = c;
    }
    sp->flags[n] = 0;
    sp->width = rn(r, 2) ? (int)rn(r, 25) : 0;
    sp->prec = rn(r, 2) ? (long)rn(r, 18) : -1;
}

static void build_spec(char *out, size_t cap, const spec *sp, int width, const char *len, char conv) {
    size_t n = (size_t)snprintf(out, cap, "%%%s", sp->flags);

    if (width > 0) n += (size_t)snprintf(out + n, cap - n, "%d", width);
    if (sp->prec >= 0) n += (size_t)snprintf(out + n, cap - n, ".%ld", sp->prec);
    snprintf(out + n, cap - n, "%s%c", len, conv);
}

static uint64_t gen_int64(uint64_t *r) {
    switch (rn(r, 6)) {
    case 0: return 0;
    case 1: return rn(r, 100);
    case 2: return (uint64_t)-(int64_t)rn(r, 100);
    case 3: return rnd(r) >> rn(r, 63);
    case 4: return 0x80000000ull;
    default: return rnd(r);
    }
}

static double gen_double(uint64_t *r) {
    double v;
    switch (rn(r, 8)) {
    case 0: v = (double)(int64_t)(rnd(r) >> rn(r, 60)); break;
    case 1: v = (double)rn(r, 100000) / (double)(1u << rn(r, 20)); break;
    case 2: v = (double)rn(r, 1000000) / 1000.0; break;
    case 3: return 0.0;
    case 4: {                           /* subnormal */
        uint64_t bits = rnd(r) & 0x000FFFFFFFFFFFFFull;
        memcpy(&v, &bits, 8);
        break;
    }
    case 5: v = (double)rn(r, 1000) * pow(10.0, (double)((int)rn(r, 40) - 20)); break;
    case 6:                             /* just under a power of ten: rounding carries out of the leading digit */
        v = pow(10.0, (double)rn(r, 9)) - pow(10.0, -(double)rn(r, 9)) * (double)(1 + rn(r, 9));
        if (v < 0) v = 0.5;
        break;
    default: {                          /* any finite pattern */
        uint64_t bits = rnd(r);
        if (((bits >> 52) & 0x7FF) == 0x7FF) bits &= ~(1ull << 62);
        memcpy(&v, &bits, 8);
        break;
    }
    }
    return (rnd(r) & 1) ? -v : v;
}

static void test_printf_vs_glibc(uint64_t seed) {
    uint64_t r = seed;
    char mine_fmt[96], ref_fmt[96], a[4096], b[4096];
    int i;
    int mismatches = 0;

    for (i = 0; i < 6000 && mismatches < 10; i++) {
        spec sp;
        int kind = i % 4;
        int na, nb;
        int ref_width;

        if (kind == 0) {                    /* integers */
            static const char convs[] = "diuoxX";
            static const char *mine_len[] = {"", "ll", "I64", "I"};
            char c = convs[rn(&r, 6)];
            int len = (int)rn(&r, 4);       /* 0 none, 1 ll, 2 I64, 3 I (pointer-sized) */
            uint64_t v = gen_int64(&r);

            gen_spec(&r, &sp, 1);
            build_spec(ref_fmt, sizeof ref_fmt, &sp, sp.width, len ? "ll" : "", c);
            build_spec(mine_fmt, sizeof mine_fmt, &sp, sp.width, mine_len[len], c);
            if (len == 0 && (c == 'd' || c == 'i')) {
                na = trunc_call(a, sizeof a, mine_fmt, (int)v);
                nb = snprintf(b, sizeof b, ref_fmt, (int)v);
            } else if (len == 0) {
                na = trunc_call(a, sizeof a, mine_fmt, (unsigned)v);
                nb = snprintf(b, sizeof b, ref_fmt, (unsigned)v);
            } else if (c == 'd' || c == 'i') {
                na = trunc_call(a, sizeof a, mine_fmt, (long long)v);
                nb = snprintf(b, sizeof b, ref_fmt, (long long)v);
            } else {
                na = trunc_call(a, sizeof a, mine_fmt, (unsigned long long)v);
                nb = snprintf(b, sizeof b, ref_fmt, (unsigned long long)v);
            }
        } else if (kind == 1) {             /* strings and characters */
            gen_spec(&r, &sp, 0);
            if (rn(&r, 3)) {
                const char *s = g_strpool[rn(&r, 6)];
                build_spec(ref_fmt, sizeof ref_fmt, &sp, sp.width, "", 's');
                na = trunc_call(a, sizeof a, ref_fmt, s);
                nb = snprintf(b, sizeof b, ref_fmt, s);
            } else {
                int ch = 'a' + (int)rn(&r, 26);
                sp.prec = -1;               /* glibc leaves a precision on %c undefined */
                build_spec(ref_fmt, sizeof ref_fmt, &sp, sp.width, "", 'c');
                na = trunc_call(a, sizeof a, ref_fmt, ch);
                nb = snprintf(b, sizeof b, ref_fmt, ch);
            }
            strcpy(mine_fmt, ref_fmt);
        } else {                            /* floating point */
            static const char convs[] = "eEfgG";
            char c = convs[rn(&r, 5)];
            double v = gen_double(&r);

            gen_spec(&r, &sp, 1);
            if (is_tie(v, c, sp.prec)) {
                g_glibc_ties++;
                continue;
            }
            if ((c == 'g' || c == 'G') && strchr(sp.flags, '#') &&
                rounding_carries(v, sp.prec < 0 ? 6 : (sp.prec == 0 ? 1 : sp.prec))) {
                g_glibc_ties++;
                continue;
            }
            build_spec(mine_fmt, sizeof mine_fmt, &sp, sp.width, "", c);
            na = trunc_call(a, sizeof a, mine_fmt, v);
            ref_width = sp.width;
            if (na > 0 && shrink_exponent(a) && ref_width > 0) {
                ref_width--;        /* mine counted a digit glibc does not print */
            }
            build_spec(ref_fmt, sizeof ref_fmt, &sp, ref_width, "", c);
            nb = snprintf(b, sizeof b, ref_fmt, v);
            na = (int)strlen(a);
        }
        g_glibc_compared++;
        g_checks++;
        if (na < 0 || nb < 0 || strcmp(a, b) != 0) {
            g_failed++;
            mismatches++;
            printf("FAIL glibc: mine [%s] glibc [%s] for %s (seed 0x%" PRIx64 ", case %d)\n", a, b, mine_fmt, seed, i);
        }
    }
}
#else
static void test_printf_vs_glibc(uint64_t seed) {
    (void)seed;
    printf("note: not glibc, printf comparison skipped\n");
}
#endif

/* ---- scanf --------------------------------------------------------------- */

static int scan(const char *in, const char *fmt, ...) {
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vc_vsscanf(in, fmt, ap);
    va_end(ap);
    return n;
}

static void test_scanf(uint64_t seed) {
    int a = 0, b = 0, c = 0, n = -1;
    char s[32], t[32];
    unsigned u = 0;
    short sh = 0;
    int64_t big = 0;
    float f = 0;
    double d = 0;
    uint64_t r = seed;
    int i;

    CHECK(scan("12 abc 0x1F", "%d %s %x", &a, s, &b) == 3 && a == 12 && !strcmp(s, "abc") && b == 31,
          "basic three: %d %d [%s] %d", a, b, s, b);
    CHECK(scan("0x10 010 10", "%i %i %i", &a, &b, &c) == 3 && a == 16 && b == 8 && c == 10, "%%i: %d %d %d", a, b, c);
    CHECK(scan("123456", "%3d%d", &a, &b) == 2 && a == 123 && b == 456, "width: %d %d", a, b);
    CHECK(scan("-1", "%u", &u) == 1 && u == 0xFFFFFFFFu, "negative into %%u: %u", u);
    CHECK(scan("65537", "%hd", &sh) == 1 && sh == 1, "%%hd stores sixteen bits: %d", sh);
    CHECK(scan("4294967301", "%I64d", &big) == 1 && big == 4294967301LL, "%%I64d: %" PRId64, big);
    CHECK(scan("4294967301", "%lld", &big) == 1 && big == 4294967301LL, "%%lld: %" PRId64, big);
    CHECK(scan("4294967301", "%ld", &a) == 1 && a == 5, "%%ld is 32 bits wide: %d", a);
    CHECK(scan("ab-cd", "%[a-c]%*c%[^x]", s, t) == 2 && !strcmp(s, "ab") && !strcmp(t, "cd"), "scanset: [%s] [%s]", s, t);
    CHECK(scan("abc", "%2c", s) == 1 && s[0] == 'a' && s[1] == 'b', "%%2c");
    CHECK(scan("12 34", "%*d %d", &a) == 1 && a == 34, "suppression: %d", a);
    CHECK(scan("  42xyz", "%d%n", &a, &n) == 1 && a == 42 && n == 4, "%%n: %d %d", a, n);
    CHECK(scan("3.5", "%f", &f) == 1 && f == 3.5f, "%%f: %f", f);
    {
        double d2 = 0;
        CHECK(scan("1e3 -2.25", "%lf %lf", &d, &d2) == 2 && d == 1000.0 && d2 == -2.25, "%%lf pair: %f %f", d, d2);
    }
    CHECK(scan("1e3", "%lf", &d) == 1 && d == 1000.0, "%%lf exponent: %f", d);
    CHECK(scan("", "%d", &a) == -1, "empty input is EOF");
    CHECK(scan("abc", "%d", &a) == 0, "no digits is a match failure");
    CHECK(scan("5 abc", "%d %d", &a, &b) == 1 && a == 5, "second conversion fails after the first");
    CHECK(scan("100%", "%d%%", &a) == 1 && a == 100, "literal percent");
    CHECK(scan("key=val", "key=%s", s) == 1 && !strcmp(s, "val"), "literal text");
    CHECK(scan("key=val", "kex=%s", s) == 0, "literal mismatch");
    CHECK(scan("abc", "%ls", (uint16_t[8]){0}) == 1, "%%ls");

#ifdef __GLIBC__
    {
        static const char *pool[] = {"", "  ", "42", "  -17abc", "0x1F zz", "077", "+5", "9999999999", "-0", "0",
                                     "12 34 56", "  +0x7f", "08", "1a2b", "x", "-", "000123", "7fffffff"};
        static const char convs[] = "diuoxX";
        int compared = 0;

        for (i = 0; i < 3000; i++) {
            char fmt[16];
            const char *in = pool[rn(&r, 18)];
            char conv = convs[rn(&r, 6)];
            int w = (int)rn(&r, 6);
            int ma = -1, ga = -1, mr, gr, mn = -1, gn = -1;

            /* A width that ends inside "0x" makes glibc store the zero it has not
             * seen digits for; nobody's program depends on which way that goes. */
            if (strstr(in, "0x") && w && w < 6) w = 0;
            if (w) snprintf(fmt, sizeof fmt, "%%%d%c%%n", w, conv);
            else snprintf(fmt, sizeof fmt, "%%%c%%n", conv);
            mr = scan(in, fmt, &ma, &mn);
            gr = sscanf(in, fmt, &ga, &gn);
            g_checks++;
            if (mr != gr || (mr >= 1 && (ma != ga || mn != gn))) {
                g_failed++;
                printf("FAIL scanf vs glibc: in [%s] fmt [%s]: mine %d/%d/%d glibc %d/%d/%d\n", in, fmt, mr, ma,
                       mn, gr, ga, gn);
            }
            compared++;
        }
        printf("note: scanf compared with glibc: %d cases\n", compared);
    }
#endif
    (void)i;
    (void)r;
}

/* ---- strings ------------------------------------------------------------- */

static int sgn(int x) { return (x > 0) - (x < 0); }

static void test_strings(uint64_t seed) {
    uint64_t r = seed;
    static const char *pool[] = {"", "a", "A", "abc", "ABC", "abd", "abcd", "b", "hello world", "Hello World",
                                 "\xe9", "\xc9", "a b", "  "};
    int i, j, k;
    char buf[64], ref[64];
    char *tok;
    char *st = NULL;
    char text[] = "  alpha,beta;;gamma  ,";

    for (i = 0; i < 14; i++) {
        for (j = 0; j < 14; j++) {
            const char *x = pool[i], *y = pool[j];
            CHECK(vc_strcmp(x, y) == sgn(strcmp(x, y)), "strcmp(%s,%s)", x, y);
            CHECK(vc_stricmp(x, y) == sgn(strcasecmp(x, y)) || (unsigned char)x[0] >= 0x80 || (unsigned char)y[0] >= 0x80,
                  "stricmp(%s,%s)", x, y);
            CHECK(vc_strncmp(x, y, 2) == sgn(strncmp(x, y, 2)), "strncmp(%s,%s,2)", x, y);
            CHECK(vc_strnicmp(x, y, 3) == sgn(strncasecmp(x, y, 3)) || (unsigned char)x[0] >= 0x80 || (unsigned char)y[0] >= 0x80,
                  "strnicmp(%s,%s,3)", x, y);
            CHECK(vc_strstr(x, y) == strstr(x, y), "strstr(%s,%s)", x, y);
            CHECK(vc_strcspn(x, y) == strcspn(x, y), "strcspn(%s,%s)", x, y);
        }
        for (k = 0; k < 128; k += 7) {
            CHECK(vc_strchr(pool[i], k) == strchr(pool[i], k), "strchr(%s,%d)", pool[i], k);
            CHECK(vc_strrchr(pool[i], k) == strrchr(pool[i], k), "strrchr(%s,%d)", pool[i], k);
        }
        CHECK(vc_strlen(pool[i]) == strlen(pool[i]), "strlen");
    }
    CHECK(vc_strchr("abc", 0) != NULL && *vc_strchr("abc", 0) == 0, "strchr finds the terminator");
    CHECK(vc_stricmp("ABC", "abc") == 0 && vc_stricmp("abc", "abd") < 0, "stricmp");

    strcpy(buf, "ab");
    vc_strcat(buf, "cd");
    CHECK(!strcmp(buf, "abcd") && vc_strcpy(ref, "xyz") == ref && !strcmp(ref, "xyz"), "strcpy/strcat");

    /* strtok's position, and runs of delimiters. */
    tok = vc_strtok(text, " ,;", &st);
    CHECK(tok && !strcmp(tok, "alpha"), "strtok 1");
    tok = vc_strtok(NULL, " ,;", &st);
    CHECK(tok && !strcmp(tok, "beta"), "strtok 2");
    tok = vc_strtok(NULL, " ,;", &st);
    CHECK(tok && !strcmp(tok, "gamma"), "strtok 3");
    CHECK(vc_strtok(NULL, " ,;", &st) == NULL && vc_strtok(NULL, " ,;", &st) == NULL, "strtok end");

    /* memory */
    for (i = 0; i < 2000; i++) {
        uint8_t a[300], b[300];
        size_t off = rn(&r, 100), to = rn(&r, 100), n = rn(&r, 150);

        for (k = 0; k < 300; k++) a[k] = b[k] = (uint8_t)rnd(&r);
        vc_memmove(a + to, a + off, n);
        memmove(b + to, b + off, n);
        CHECK(memcmp(a, b, 300) == 0, "memmove %zu->%zu x%zu", off, to, n);
        vc_memset(a + off, (int)rn(&r, 256), n);
        memset(b + off, a[off], n);
        CHECK(memcmp(a, b, 300) == 0, "memset");
        CHECK(vc_memcmp(a, b, 300) == sgn(memcmp(a, b, 300)), "memcmp");
        CHECK(vc_memchr(a, a[40], 300) == memchr(a, a[40], 300), "memchr");
    }
    {
        uint8_t src[40], dst[40];
        for (k = 0; k < 40; k++) src[k] = (uint8_t)k;
        vc_memcpy(dst, src, 40);
        CHECK(memcmp(dst, src, 40) == 0, "memcpy");
    }
    CHECK(vc_tolower('A') == 'a' && vc_tolower('z') == 'z' && vc_tolower('@') == '@' && vc_tolower(0xC9) == 0xC9,
          "tolower");
    CHECK(vc_toupper('a') == 'A' && vc_toupper('Z') == 'Z' && vc_toupper('`') == '`', "toupper");
}

static void test_wide_strings(void) {
    uint16_t a[32], b[32];
    uint16_t *st = NULL, *tk;
    uint16_t text[] = {' ', 'x', ',', 'y', ',', ',', 'z', 0};
    uint16_t delim[] = {' ', ',', 0};

    widen(a, "hello world");
    widen(b, "hello there");
    CHECK(vc_wcslen(a) == 11, "wcslen");
    CHECK(vc_wcscmp(a, b) > 0 && vc_wcscmp(b, a) < 0 && vc_wcscmp(a, a) == 0, "wcscmp");
    CHECK(vc_wcsncmp(a, b, 6) == 0 && vc_wcsncmp(a, b, 8) > 0, "wcsncmp");
    widen(b, "HELLO WORLD");
    CHECK(vc_wcsicmp(a, b) == 0 && vc_wcsnicmp(a, b, 5) == 0, "wcsicmp");
    widen(b, "HELLO");
    CHECK(vc_wcsicmp(a, b) > 0 && vc_wcsnicmp(a, b, 5) == 0 && vc_wcsnicmp(a, b, 6) > 0, "wcsicmp order");
    CHECK(vc_wcschr(a, 'o') == a + 4 && vc_wcsrchr(a, 'o') == a + 7 && vc_wcschr(a, 'q') == NULL, "wcschr/wcsrchr");
    CHECK(vc_wcschr(a, 0) == a + 11, "wcschr finds the terminator");
    widen(b, "wor");
    CHECK(vc_wcsstr(a, b) == a + 6 && vc_wcsstr(a, a) == a, "wcsstr");
    widen(b, "xyz");
    CHECK(vc_wcsstr(a, b) == NULL, "wcsstr absent");
    widen(b, "dlr");
    CHECK(vc_wcspbrk(a, b) == a + 2 && vc_wcspbrk(a, delim) == a + 5, "wcspbrk");
    widen(b, "ab");
    vc_wcscat(b, a);
    CHECK(vc_wcslen(b) == 13 && b[0] == 'a' && b[2] == 'h', "wcscat");
    vc_wcscpy(b, a);
    CHECK(vc_wcscmp(a, b) == 0, "wcscpy");
    tk = vc_wcstok(text, delim, &st);
    CHECK(tk && tk[0] == 'x' && tk[1] == 0, "wcstok 1");
    tk = vc_wcstok(NULL, delim, &st);
    CHECK(tk && tk[0] == 'y' && tk[1] == 0, "wcstok 2");
    tk = vc_wcstok(NULL, delim, &st);
    CHECK(tk && tk[0] == 'z', "wcstok 3");
    CHECK(vc_wcstok(NULL, delim, &st) == NULL, "wcstok end");
    /* UTF-16 above 0x7f compares as an unsigned unit. */
    a[0] = 0x263A;
    b[0] = 0x41;
    a[1] = b[1] = 0;
    CHECK(vc_wcscmp(a, b) > 0, "wcscmp is unsigned");
    CHECK(vc_towlower('A') == 'a' && vc_towlower(0x263A) == 0x263A && vc_towlower(0xC9) == 0xC9, "towlower, C locale");
}

/* ---- numbers ------------------------------------------------------------- */

static void test_numbers(uint64_t seed) {
    uint64_t r = seed;
    char *end;
    int i;

    CHECK(vc_atoi("  42x") == 42 && vc_atoi("-7") == -7 && vc_atoi("+8") == 8 && vc_atoi("x") == 0, "atoi");
    CHECK(vc_atoi("") == 0 && vc_atoi("   ") == 0, "atoi empty");

    CHECK(vc_strtol("  -123abc", &end, 10) == -123 && *end == 'a', "strtol basic");
    CHECK(vc_strtol("0x1f", &end, 0) == 31 && *end == 0, "strtol 0x");
    CHECK(vc_strtol("0x1f", &end, 16) == 31, "strtol 0x with base 16");
    CHECK(vc_strtol("077", &end, 0) == 63, "strtol octal");
    CHECK(vc_strtol("zz", &end, 36) == 1295, "strtol base 36");
    CHECK(vc_strtol("2147483647", &end, 10) == 2147483647, "strtol max");
    CHECK(vc_strtol("2147483648", &end, 10) == INT32_MAX && *end == 0, "strtol clamps high (32-bit long)");
    CHECK(vc_strtol("-2147483648", &end, 10) == INT32_MIN, "strtol min");
    CHECK(vc_strtol("-2147483649", &end, 10) == INT32_MIN, "strtol clamps low");
    CHECK(vc_strtol("99999999999999999999", &end, 10) == INT32_MAX, "strtol far past the limit");
    {
        const char *s = "xyz";
        CHECK(vc_strtol(s, &end, 10) == 0 && end == s, "strtol no digits leaves end at the start");
        s = "0x";
        CHECK(vc_strtol(s, &end, 0) == 0 && end == s + 1, "strtol 0x without digits is the zero");
        CHECK(vc_strtol("5", &end, 1) == 0 && vc_strtol("5", &end, 37) == 0, "strtol bad base");
    }
    CHECK(vc_strtoul("4294967295", &end, 10) == 0xFFFFFFFFu, "strtoul max");
    CHECK(vc_strtoul("4294967296", &end, 10) == 0xFFFFFFFFu, "strtoul clamps");
    CHECK(vc_strtoul("-1", &end, 10) == 0xFFFFFFFFu, "strtoul negates");
    CHECK(vc_strtoul("-2", &end, 10) == 0xFFFFFFFEu, "strtoul negates, 2");
    CHECK(vc_strtoul("ff", &end, 16) == 255 && vc_strtoul("0xFF", &end, 0) == 255, "strtoul hex");

#ifdef __GLIBC__
    for (i = 0; i < 3000; i++) {
        char buf[48];
        int base = (int)rn(&r, 37);
        int64_t v = (int64_t)(rnd(&r) >> rn(&r, 63)) * ((rnd(&r) & 1) ? 1 : -1);
        char *e1, *e2;
        long ref;
        int32_t mine;

        if (base == 1) base = 10;
        snprintf(buf, sizeof buf, "%s%" PRId64 "%s", rn(&r, 3) ? "" : "  ", v, rn(&r, 2) ? "" : "zz");
        mine = vc_strtol(buf, &e1, base);
        ref = strtol(buf, &e2, base);
        ref = ref > INT32_MAX ? INT32_MAX : (ref < INT32_MIN ? INT32_MIN : ref);
        CHECK(mine == ref && e1 - buf == e2 - buf, "strtol(%s,%d): %d vs %ld", buf, base, mine, ref);
    }
#endif
    (void)i;
    (void)r;
}

static void test_rand(void) {
    /* msvcrt's sequence for srand(1): the well-known one. */
    static const int expect[] = {41, 18467, 6334, 26500, 19169, 15724, 11478, 29358, 26962, 24464};
    uint32_t seed = 1;
    int i, ok = 1;

    for (i = 0; i < 10; i++) {
        int v = vc_rand(&seed);
        if (v != expect[i]) {
            ok = 0;
            printf("rand #%d = %d, wanted %d\n", i, v, expect[i]);
        }
    }
    CHECK(ok, "rand sequence from seed 1");
    seed = 0;
    CHECK(vc_rand(&seed) == ((2531011u >> 16) & 0x7FFF), "rand from seed 0");
}

typedef struct {
    int key;
    char pad[8];
} rec12;

static int cmp_int(const void *a, const void *b) {
    int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}
static int cmp_rec(const void *a, const void *b) { return cmp_int(a, b); }

static void test_qsort(uint64_t seed) {
    uint64_t r = seed;
    int round;

    for (round = 0; round < 300; round++) {
        int n = (int)rn(&r, 200), i, ok = 1;
        int a[200], b[200];
        rec12 recs[200];
        uint64_t sum = 0, sum2 = 0;

        for (i = 0; i < n; i++) {
            a[i] = b[i] = (int)rn(&r, round % 3 == 0 ? 5 : 100000) - 50;
            recs[i].key = a[i];
            memset(recs[i].pad, (char)i, 8);
            sum += (uint64_t)(uint32_t)a[i];
        }
        vc_qsort(a, (size_t)n, sizeof a[0], cmp_int);
        qsort(b, (size_t)n, sizeof b[0], cmp_int);
        CHECK(n == 0 || memcmp(a, b, (size_t)n * sizeof a[0]) == 0, "qsort ints n=%d", n);
        vc_qsort(recs, (size_t)n, sizeof recs[0], cmp_rec);
        for (i = 0; i < n; i++) {
            sum2 += (uint64_t)(uint32_t)recs[i].key;
            if (i && recs[i - 1].key > recs[i].key) ok = 0;
            /* the padding travelled with its key: it was filled from the index */
            if (recs[i].pad[0] != recs[i].pad[7]) ok = 0;
        }
        CHECK(ok && sum == sum2, "qsort records n=%d", n);
    }
    vc_qsort(NULL, 0, 4, cmp_int);
    vc_qsort(NULL, 1, 4, cmp_int);
}

static int64_t ulps(double a, double b) {
    int64_t x, y;

    memcpy(&x, &a, 8);
    memcpy(&y, &b, 8);
    return x > y ? x - y : y - x;
}

static void test_acos(uint64_t seed) {
    uint64_t r = seed;
    int64_t worst = 0;
    double worst_x = 0;
    int i;
    static const double edge[] = {0.0, 1.0, -1.0, 0.5, -0.5, 1e-300, -1e-300, 1e-17, 0.999999999999999889, -0.999999999999999889,
                                  0.70710678118654752, 0.2, 0.9, -0.9, 0.99, 0.001};

    CHECK(vc_acos(1.0) == 0.0, "acos(1)");
    CHECK(vc_acos(-1.0) == M_PI, "acos(-1) is pi");
    CHECK(vc_acos(0.0) == M_PI / 2, "acos(0)");
    CHECK(isnan(vc_acos(1.0000000000000002)) && isnan(vc_acos(-2.0)) && isnan(vc_acos(NAN)) && isnan(vc_acos(INFINITY)),
          "acos outside [-1,1] is NaN");
    for (i = 0; i < (int)(sizeof edge / sizeof edge[0]); i++) {
        int64_t u = ulps(vc_acos(edge[i]), acos(edge[i]));
        if (u > worst) { worst = u; worst_x = edge[i]; }
    }
    for (i = 0; i < 200000; i++) {
        double x;
        switch (i % 4) {
        case 0: x = (double)(int64_t)(rnd(&r) >> 11) / 4503599627370496.0 - 1.0; break;      /* uniform */
        case 1: x = 1.0 - ldexp((double)(rnd(&r) >> 11), -53 - (int)rn(&r, 40)); break;        /* near 1 */
        case 2: x = -1.0 + ldexp((double)(rnd(&r) >> 11), -53 - (int)rn(&r, 40)); break;       /* near -1 */
        default: x = ldexp((double)(int64_t)(rnd(&r) >> 11) - 4503599627370496.0, -53 - (int)rn(&r, 60)); break;
        }
        if (x > 1.0 || x < -1.0) continue;
        {
            int64_t u = ulps(vc_acos(x), acos(x));
            if (u > worst) { worst = u; worst_x = x; }
        }
    }
    printf("note: acos worst error %" PRId64 " ulp (at %.17g)\n", worst, worst_x);
    CHECK(worst <= 3,"acos is %" PRId64 " ulp off at %.17g", worst, worst_x);
}

static void test_time(void) {
    CHECK(vc_filetime_to_time64(116444736000000000LL) == 0, "the epoch");
    CHECK(vc_filetime_to_time64(116444736000000000LL + 10000000) == 1, "one second");
    CHECK(vc_filetime_to_time64(116444736000000000LL + 9999999) == 0, "just under a second");
    CHECK(vc_filetime_to_time64(132539328000000000LL) == 1609459200LL, "2021-01-01");
    CHECK(vc_filetime_to_time64(116444736000000000LL - 1) == -1, "a tick before 1970 rounds down");
    CHECK(vc_filetime_to_time64(116444736000000000LL - 10000000) == -1, "a second before 1970");
}

#if defined(__x86_64__) && defined(__GLIBC__)
static void test_fp_control(void) {
    uint32_t orig = vc_control87(0, 0), v, mx;
    volatile double one = 1.0, three = 3.0, zero = 0.0, big = 1e308, r;

    /* The defaults of a process: everything masked, round to nearest. */
    CHECK((orig & VC_MCW_EM) == VC_MCW_EM, "default exception masks: %08x", orig);
    CHECK((orig & VC_MCW_RC) == 0, "default rounding: %08x", orig);

    v = vc_control87(0x100, VC_MCW_RC);                 /* _RC_DOWN */
    CHECK((v & VC_MCW_RC) == 0x100 && fegetround() == FE_DOWNWARD, "round down: %08x %d", v, fegetround());
    v = vc_control87(0x200, VC_MCW_RC);
    CHECK((v & VC_MCW_RC) == 0x200 && fegetround() == FE_UPWARD, "round up: %08x", v);
    v = vc_control87(0x300, VC_MCW_RC);
    CHECK((v & VC_MCW_RC) == 0x300 && fegetround() == FE_TOWARDZERO, "chop: %08x", v);
    v = vc_control87(0, VC_MCW_RC);
    CHECK((v & VC_MCW_RC) == 0 && fegetround() == FE_TONEAREST, "nearest: %08x", v);

    v = vc_control87(0x20000, VC_MCW_PC);               /* _PC_24 */
    CHECK((v & VC_MCW_PC) == 0x20000, "precision 24: %08x", v);
    v = vc_control87(0x10000, VC_MCW_PC);
    CHECK((v & VC_MCW_PC) == 0x10000, "precision 53: %08x", v);
    {
        uint16_t cw;
        __asm__ __volatile__("fnstcw %0" : "=m"(cw));
        CHECK(((cw >> 8) & 3) == 2, "x87 precision field: %04x", cw);
    }
    v = vc_control87(0, VC_MCW_PC);                     /* _PC_64 */
    CHECK((v & VC_MCW_PC) == 0, "precision 64: %08x", v);

    /* Unmask invalid, read the hardware; then mask it again. */
    v = vc_control87(0, 0x10);                          /* _EM_INVALID: 0 unmasks */
    __asm__ __volatile__("stmxcsr %0" : "=m"(mx));
    CHECK(!(v & 0x10) && !(mx & (1u << 7)), "invalid unmasked: %08x mxcsr %08x", v, mx);
    v = vc_control87(0x10, 0x10);
    __asm__ __volatile__("stmxcsr %0" : "=m"(mx));
    CHECK((v & 0x10) && (mx & (1u << 7)), "invalid masked again: %08x mxcsr %08x", v, mx);
    v = vc_control87(0, 0x80000);                       /* _EM_DENORMAL */
    __asm__ __volatile__("stmxcsr %0" : "=m"(mx));
    CHECK(!(v & 0x80000) && !(mx & (1u << 8)), "denormal unmasked: %08x", v);
    vc_control87(0x80000, 0x80000);

    v = vc_control87(0x01000000, VC_MCW_DN);            /* _DN_FLUSH */
    __asm__ __volatile__("stmxcsr %0" : "=m"(mx));
    CHECK((v & VC_MCW_DN) == 0x01000000 && (mx & (1u << 15)) && (mx & (1u << 6)), "flush to zero: %08x mxcsr %08x", v, mx);
    v = vc_control87(0, VC_MCW_DN);
    CHECK((v & VC_MCW_DN) == 0, "flush off");

    /* Flags: raise each, read it back in Microsoft's numbering, clear it. */
    vc_clearfp();
    r = one / three;
    CHECK((vc_statusfp() & 0x1) != 0, "inexact: %08x", vc_statusfp());
    r = one / zero;
    CHECK((vc_statusfp() & 0x8) != 0, "zero-divide: %08x", vc_statusfp());
    r = big * big;
    CHECK((vc_statusfp() & 0x4) != 0, "overflow: %08x", vc_statusfp());
    r = zero / zero;
    CHECK((vc_statusfp() & 0x10) != 0, "invalid: %08x", vc_statusfp());
    r = 1e-300;
    r = r * 1e-300;
    CHECK((vc_statusfp() & 0x2) != 0, "underflow: %08x", vc_statusfp());
    v = vc_clearfp();
    CHECK((v & 0x1F) == 0x1F && (vc_statusfp() & 0x8001F) == 0, "clearfp returns the flags and clears them: %08x then %08x", v,
          vc_statusfp());
    CHECK(vc_sw_from_hw(0x3F) == 0x8001F && vc_sw_from_hw(0x02) == 0x80000 && vc_sw_from_hw(0x04) == 0x8 &&
              vc_sw_from_hw(0x01) == 0x10,
          "hardware flag order to Microsoft's");
    (void)r;
    vc_control87(orig, VC_MCW_EM | VC_MCW_RC | VC_MCW_PC | VC_MCW_DN);
}
#else
static void test_fp_control(void) {}
#endif

/* ---- the heap ------------------------------------------------------------ */

#define ARENA (1u << 20)

static void fill_pattern(uint8_t *p, size_t n, uint8_t tag) {
    size_t i;
    for (i = 0; i < n; i++) p[i] = (uint8_t)(tag + i * 7u);
}

static int pattern_ok(const uint8_t *p, size_t n, uint8_t tag) {
    size_t i;
    for (i = 0; i < n; i++) {
        if (p[i] != (uint8_t)(tag + i * 7u)) return 0;
    }
    return 1;
}

typedef struct {
    uint8_t *p;
    size_t n;
    uint8_t tag;
} slot;

static int cmp_slot(const void *a, const void *b) {
    const slot *x = a, *y = b;
    return (x->p > y->p) - (x->p < y->p);
}

static void heap_audit(vc_heap *h, slot *slots, int nslots, const char *when) {
    static slot live[512];
    int i, n = 0;

    CHECK(vc_heap_check(h, NULL, NULL, NULL) == 0, "heap invariants broken %s: %d", when, vc_heap_check(h, NULL, NULL, NULL));
    for (i = 0; i < nslots; i++) {
        if (slots[i].p) live[n++] = slots[i];
    }
    qsort(live, (size_t)n, sizeof live[0], cmp_slot);
    for (i = 0; i < n; i++) {
        CHECK(((uintptr_t)live[i].p & 15u) == 0, "allocation not 16-aligned %s", when);
        CHECK(live[i].p >= h->base && live[i].p + live[i].n <= h->base + h->size, "allocation outside the arena %s", when);
        CHECK(vc_heap_usable(h, live[i].p) >= live[i].n, "usable < requested %s", when);
        if (i + 1 < n) {
            CHECK(live[i].p + live[i].n <= live[i + 1].p, "allocations overlap %s", when);
        }
        CHECK(pattern_ok(live[i].p, live[i].n, live[i].tag), "contents damaged %s (slot at %p, %zu bytes)", when,
              (void *)live[i].p, live[i].n);
    }
}

static size_t pick_size(uint64_t *r) {
    switch (rn(r, 20)) {
    case 0: return 0;
    case 1: return 1000 + rn(r, 30000);
    case 2:
    case 3:
    case 4: return 100 + rn(r, 3000);
    default: return 1 + rn(r, 120);
    }
}

static void test_heap_soak(uint64_t seed, int ops) {
    uint8_t *mem = aligned_alloc(16, ARENA);
    vc_heap h;
    slot slots[300];
    uint64_t r = seed;
    size_t nf, fb, nu;
    int i, failures = 0, successes = 0, reallocs = 0, moved = 0;

    memset(slots, 0, sizeof slots);
    vc_heap_init(&h, mem, ARENA);
    CHECK(vc_heap_check(&h, &nf, &fb, &nu) == 0 && nf == 1 && nu == 0, "fresh heap");
    {
        size_t total = fb;
        for (i = 0; i < ops; i++) {
            int k = (int)rn(&r, 300);
            slot *s = &slots[k];
            unsigned what = rn(&r, 10);

            if (!s->p && what < 8) {
                size_t n = pick_size(&r);
                void *p;
                if (what == 7) {
                    p = vc_heap_calloc(&h, 1, n);
                    if (p) {
                        size_t j;
                        for (j = 0; j < n; j++) {
                            if (((uint8_t *)p)[j]) { CHECK(0, "calloc not zeroed"); break; }
                        }
                    }
                } else {
                    p = vc_heap_alloc(&h, n);
                }
                if (p) {
                    successes++;
                    s->p = p;
                    s->n = n ? n : 0;
                    s->tag = (uint8_t)rnd(&r);
                    fill_pattern(s->p, s->n, s->tag);
                } else {
                    failures++;
                }
            } else if (s->p && what < 6) {
                size_t n = pick_size(&r), keep = n < s->n ? n : s->n;
                uint8_t *old = s->p;
                void *p;

                reallocs++;
                if (n == 0) n = 1;
                keep = n < s->n ? n : s->n;
                p = vc_heap_realloc(&h, s->p, n);
                if (p) {
                    CHECK(pattern_ok(p, keep, s->tag), "realloc lost the contents (%zu -> %zu)", s->n, n);
                    if (p != old) moved++;
                    s->p = p;
                    s->n = n;
                    s->tag = (uint8_t)rnd(&r);
                    fill_pattern(s->p, s->n, s->tag);
                } else {
                    CHECK(pattern_ok(old, s->n < n ? s->n : n, s->tag), "failed realloc damaged the original");
                }
            } else if (s->p) {
                vc_heap_free(&h, s->p);
                s->p = NULL;
                s->n = 0;
            }
            if (i % 97 == 0) heap_audit(&h, slots, 300, "mid-run");
        }
        heap_audit(&h, slots, 300, "end of run");
        for (i = 0; i < 300; i++) {
            int k = (int)rn(&r, 300);
            if (slots[k].p) {
                vc_heap_free(&h, slots[k].p);
                slots[k].p = NULL;
            }
        }
        for (i = 0; i < 300; i++) {
            if (slots[i].p) vc_heap_free(&h, slots[i].p);
        }
        /* Everything freed: one block, all of it, and it can be had in one piece. */
        CHECK(vc_heap_check(&h, &nf, &fb, &nu) == 0, "invariants after freeing everything");
        CHECK(nf == 1 && nu == 0 && fb == total, "free blocks=%zu used=%zu free=%zu of %zu: it did not coalesce back", nf, nu, fb, total);
        {
            void *all = vc_heap_alloc(&h, fb - 16);
            CHECK(all != NULL, "the whole arena cannot be allocated after freeing everything");
            vc_heap_free(&h, all);
            CHECK(vc_heap_check(&h, &nf, &fb, &nu) == 0 && nf == 1 && fb == total, "whole-arena round trip");
        }
    }
    printf("note: heap soak seed 0x%" PRIx64 ": %d ops, %d allocations, %d refused, %d reallocs (%d moved)\n", seed, ops,
           successes, failures, reallocs, moved);
    CHECK(successes > ops / 10 && reallocs > ops / 20, "the soak did not exercise the heap: %d allocations, %d reallocs", successes, reallocs);
    free(mem);
}

static void test_heap_cases(void) {
    uint8_t *mem = aligned_alloc(16, 65536);
    vc_heap h;
    size_t nf, fb, nu, total;
    void *blk[3];
    static const int orders[6][3] = {{0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}};
    int o, i;

    vc_heap_init(&h, mem, 65536);
    vc_heap_check(&h, NULL, &total, NULL);

    /* Every size gets a 16-byte aligned block with at least the room asked for. */
    {
        void *held[40];
        for (i = 0; i < 40; i++) {
            held[i] = vc_heap_alloc(&h, (size_t)(1 + i * 7));
            CHECK(held[i] && ((uintptr_t)held[i] & 15u) == 0 && vc_heap_usable(&h, held[i]) >= (size_t)(1 + i * 7),
                  "allocation of %d bytes: %p, usable %zu", 1 + i * 7, held[i], vc_heap_usable(&h, held[i]));
            CHECK(vc_heap_check(&h, NULL, NULL, NULL) == 0, "invariants after allocating %d bytes", 1 + i * 7);
        }
        for (i = 0; i < 40; i++) vc_heap_free(&h, held[i]);
        CHECK(vc_heap_check(&h, &nf, &fb, &nu) == 0 && nf == 1 && fb == total, "alignment loop returned everything");
    }

    for (o = 0; o < 6; o++) {
        blk[0] = vc_heap_alloc(&h, 100);
        blk[1] = vc_heap_alloc(&h, 200);
        blk[2] = vc_heap_alloc(&h, 300);
        CHECK(blk[0] && blk[1] && blk[2] && (uint8_t *)blk[1] > (uint8_t *)blk[0] && (uint8_t *)blk[2] > (uint8_t *)blk[1],
              "first fit hands out ascending addresses from a fresh heap");
        for (i = 0; i < 3; i++) {
            vc_heap_free(&h, blk[orders[o][i]]);
            CHECK(vc_heap_check(&h, NULL, NULL, NULL) == 0, "invariants, order %d step %d", o, i);
        }
        CHECK(vc_heap_check(&h, &nf, &fb, &nu) == 0 && nf == 1 && fb == total && nu == 0,
              "order %d: free blocks=%zu bytes=%zu (of %zu) - neighbours were not merged", o, nf, fb, total);
    }

    /* A hole is reused by first fit; the block after it is not disturbed. */
    {
        void *a = vc_heap_alloc(&h, 64), *b = vc_heap_alloc(&h, 64), *c = vc_heap_alloc(&h, 64);
        void *again;
        vc_heap_free(&h, b);
        again = vc_heap_alloc(&h, 48);
        CHECK(again == b, "a freed hole is reused");
        vc_heap_free(&h, a);
        vc_heap_free(&h, again);
        vc_heap_free(&h, c);
        CHECK(vc_heap_check(&h, &nf, &fb, &nu) == 0 && nf == 1 && fb == total, "hole reuse round trip");
    }

    /* realloc keeps the contents whether it grows in place or moves. */
    {
        uint8_t *p = vc_heap_alloc(&h, 100), *q;
        void *blocker;
        fill_pattern(p, 100, 0x33);
        q = vc_heap_realloc(&h, p, 5000);
        CHECK(q == p && pattern_ok(q, 100, 0x33), "growing into the free space after it stays in place and keeps the bytes");
        /* Big enough that a grow which ignores "in use" would find room over it. */
        blocker = vc_heap_alloc(&h, 30000);
        p = q;
        q = vc_heap_realloc(&h, p, 20000);
        CHECK(q != NULL && q != p && pattern_ok(q, 100, 0x33), "a grown block that cannot extend moves and keeps the bytes");
        p = vc_heap_realloc(&h, q, 50);
        CHECK(p == q && pattern_ok(p, 50, 0x33), "shrinking keeps the bytes");
        CHECK(vc_heap_check(&h, NULL, NULL, NULL) == 0, "invariants after realloc");
        CHECK(vc_heap_realloc(&h, p, 0) == NULL, "realloc to zero frees");
        vc_heap_free(&h, blocker);
        CHECK(vc_heap_check(&h, &nf, &fb, &nu) == 0 && nf == 1 && fb == total && nu == 0, "all returned after realloc cases");
        CHECK(vc_heap_realloc(&h, NULL, 10) != NULL, "realloc(NULL) allocates");
    }

    /* Refusals. */
    CHECK(vc_heap_alloc(&h, (size_t)-1) == NULL && vc_heap_alloc(&h, 1u << 30) == NULL, "absurd sizes are refused");
    CHECK(vc_heap_calloc(&h, (size_t)1 << 40, (size_t)1 << 40) == NULL, "calloc overflow is refused");
    vc_heap_free(&h, NULL);
    vc_heap_free(&h, mem + 5);
    {
        void *x = vc_heap_alloc(&h, 32);
        vc_heap_free(&h, x);
        vc_heap_free(&h, x);        /* a double free is ignored, not trusted */
        CHECK(vc_heap_check(&h, NULL, NULL, NULL) == 0, "double free did not corrupt the heap");
    }
    free(mem);
}

/* ---- the command line ---------------------------------------------------- */

static int parse(const char *cmd, char *out, size_t cap, size_t *offsets, int max, size_t *bytes) {
    uint16_t w[512];
    size_t n = 0;

    while (cmd[n]) {
        w[n] = (uint8_t)cmd[n];
        n++;
    }
    return vc_parse_cmdline(w, n, out, cap, offsets, max, bytes);
}

/* The arguments as "<a>|<b>|<c>". */
static const char *args(const char *cmd) {
    static char out[1024], res[1024];
    size_t offs[32], bytes = 0, need = 0;
    int n, i;

    n = parse(cmd, NULL, 0, NULL, 0, &need);
    {
        int n2 = parse(cmd, out, sizeof out, offs, 32, &bytes);
        CHECK(n == n2 && need == bytes, "the counting pass and the writing pass disagree on %s", cmd);
    }
    res[0] = 0;
    for (i = 0; i < n && i < 32; i++) {
        if (i) strcat(res, "|");
        strcat(res, out + offs[i]);
    }
    return res;
}

#define ARGS(expected, cmd)                                                         \
    do {                                                                            \
        const char *got_ = args(cmd);                                               \
        CHECK(strcmp(got_, (expected)) == 0, "argv of [%s] is [%s], wanted [%s]", (cmd), got_, (expected)); \
    } while (0)

static void test_cmdline(void) {
    /* Microsoft's examples, with a program name in front. */
    ARGS("p|abc|d|e", "p \"abc\" d e");
    ARGS("p|a\\\\b|de fg|h", "p a\\\\b d\"e f\"g h");
    ARGS("p|a\\\"b|c|d", "p a\\\\\\\"b c d");
    ARGS("p|a\\\\b c|d|e", "p a\\\\\\\\\"b c\" d e");

    /* The program name has its own rule: no escapes, ended by a quote. */
    ARGS("C:\\Program Files\\x\\p.exe|a", "\"C:\\Program Files\\x\\p.exe\" a");
    ARGS("C:\\dir\\p.exe|a", "C:\\dir\\p.exe a");
    ARGS("c:\\dir\\\\|b", "\"c:\\dir\\\\\" b");

    ARGS("", "");
    ARGS("p", "p");
    ARGS("|p", "  p  ");                            /* the program name is parsed from the first character */
    ARGS("p|a|b", "p \t a\t\tb \t");
    ARGS("p|a||b", "p a \"\" b");                   /* an empty quoted argument is an argument */
    ARGS("p|a bc", "p \"a b\"c");
    ARGS("p|abc def", "p \"abc def");              /* an open quote runs to the end */
    ARGS("p|a\\b", "p a\\b");                       /* backslashes not before a quote are literal */
    ARGS("p|a\\\\b", "p a\\\\b");
    ARGS("p|\\", "p \\");
    ARGS("p|a\\\\\\", "p a\\\\\\");
    ARGS("p|a\\", "p \"a\\\\\"");                   /* 2n backslashes then the closing quote: n, and it closes */
    ARGS("p|\"", "p \\\"");
    ARGS("p|a\"b", "p a\\\"b");

    /* Non-ASCII: UTF-16 in, UTF-8 out. */
    {
        uint16_t w[] = {'p', ' ', 0x00E9, ' ', 0xD83D, 0xDE00, ' ', 0xD800, 'x'};
        char out[64];
        size_t offs[8], bytes;
        int n = vc_parse_cmdline(w, sizeof w / sizeof w[0], out, sizeof out, offs, 8, &bytes);
        CHECK(n == 4, "non-ASCII argc %d", n);
        CHECK(!strcmp(out + offs[1], "\xC3\xA9"), "U+00E9 as UTF-8");
        CHECK(!strcmp(out + offs[2], "\xF0\x9F\x98\x80"), "a surrogate pair as one four-byte sequence");
        CHECK(!strcmp(out + offs[3], "\xEF\xBF\xBD" "x"), "a lone surrogate as U+FFFD");
    }
    /* Too small a table or buffer: counts, writes nothing past them. */
    {
        char out[4] = {'#', '#', '#', '#'};
        size_t offs[1] = {99}, bytes = 0;
        int n = parse("progname a b c", out, 3, offs, 1, &bytes);
        CHECK(n == 4 && bytes == 15 && out[3] == '#', "cap respected: n=%d bytes=%zu", n, bytes);
    }
}

/* ---- main ---------------------------------------------------------------- */

/* A fault in the code under test ends the run, and a run that ends in a signal
 * says nothing about which check it was on. The handler names the test, so a
 * sabotage that corrupts the heap is reported as that and not as "core dumped". */
static const char *volatile g_current = "startup";

static void on_signal(int sig) {
    const char *p = g_current;
    size_t n = 0;

    (void)!write(1, "FAIL ", 5);
    (void)!write(1, sig == SIGALRM ? "timeout" : "crash", sig == SIGALRM ? 7 : 5);
    (void)!write(1, " in ", 4);
    while (p[n]) n++;
    (void)!write(1, p, n);
    (void)!write(1, "\n", 1);
    _exit(1);
}

#define RUN(fn)          \
    do {                 \
        g_current = #fn; \
        fn;              \
    } while (0)

int main(int argc, char **argv) {
    uint64_t seed = g_seed;

    /* A crash is when a message most needs to have left already. */
    setvbuf(stdout, NULL, _IONBF, 0);
    signal(SIGSEGV, on_signal);
    signal(SIGBUS, on_signal);
    signal(SIGABRT, on_signal);
    signal(SIGFPE, on_signal);
    signal(SIGALRM, on_signal);
    alarm(100);
    if (argc > 1) {
        seed = strtoull(argv[1], NULL, 0);
    }
    printf("vibeos_msvcrt_tests: seed 0x%" PRIx64 "\n", seed);

    RUN(test_printf_documented());
    RUN(test_wide_printf());
    RUN(test_wide_keep());
    RUN(test_truncation());
    RUN(test_printf_vs_glibc(seed));
    RUN(test_scanf(seed ^ 0x1111));
    RUN(test_strings(seed ^ 0x2222));
    RUN(test_wide_strings());
    RUN(test_numbers(seed ^ 0x3333));
    RUN(test_rand());
    RUN(test_qsort(seed ^ 0x4444));
    RUN(test_acos(seed ^ 0x5555));
    RUN(test_time());
    RUN(test_fp_control());
    RUN(test_heap_cases());
    RUN(test_heap_soak(seed ^ 0x6666, 200000));
    RUN(test_heap_soak(seed ^ 0x7777, 100000));
    RUN(test_cmdline());

    printf("vibeos_msvcrt_tests: checks=%d failed=%d glibc_printf_compared=%d ties_skipped=%d seed=0x%" PRIx64 "\n",
           g_checks, g_failed, g_glibc_compared, g_glibc_ties, seed);
    return g_failed ? 1 : 0;
}
