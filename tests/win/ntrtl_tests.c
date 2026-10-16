/* The Rtl runtime of our ntdll on the host (docs/abi/windows.md W2): the
 * allocator, strings and code pages, the checksum and the generators.
 * The same C files are what ntdll.dll is built from; the five hooks they reach the
 * machine through are answered by ntrtl_host.c. Expected values come from
 * Microsoft's documentation of each function, or from a model written here
 * independently of the code, never from running the code and pasting what it
 * said. */

#include <stdlib.h>

#include "ntrtl_test.h"

int g_fail;
int g_checks;

/* ---- the allocator --------------------------------------------------------------------- */

static void t_heap(void) {
    size_t full, largest, blocks;
    void *a, *b, *c, *big, *held[64];
    int i;
    char local;

    vibeos_rtl_heap_stats(&full, &largest, &blocks);
    CHECK(full == VIBEOS_RTL_ARENA_SIZE && blocks == 1, "a fresh arena is one block, all of it (%zu, %zu)",
          full, blocks);

    a = vibeos_rtl_alloc(1);
    b = vibeos_rtl_alloc(100);
    c = vibeos_rtl_alloc(0);
    CHECK(a && b && c, "small allocations succeed, a zero-byte one included");
    CHECK(((uintptr_t)a & 15u) == 0 && ((uintptr_t)b & 15u) == 0 && ((uintptr_t)c & 15u) == 0,
          "blocks are 16-byte aligned");
    CHECK(a != b && b != c && a != c, "blocks are distinct");
    memset(a, 0xA1, 1);
    memset(b, 0xB2, 100);
    CHECK(((unsigned char *)b)[99] == 0xB2 && ((unsigned char *)a)[0] == 0xA1,
          "a block's bytes are its own: b did not overlap a");

    /* Freeing the middle one last must merge all three back with the tail. */
    vibeos_rtl_free(a);
    vibeos_rtl_free(c);
    vibeos_rtl_heap_stats(&full, 0, &blocks);
    CHECK(blocks == 2, "two free blocks while b is held (%zu)", blocks);
    vibeos_rtl_free(b);
    vibeos_rtl_heap_stats(&full, &largest, &blocks);
    CHECK(blocks == 1 && full == VIBEOS_RTL_ARENA_SIZE && largest == VIBEOS_RTL_ARENA_SIZE,
          "everything free again is one block (%zu blocks, %zu bytes)", blocks, full);

    /* A pointer that is not ours, a double free and an interior pointer change nothing. */
    a = vibeos_rtl_alloc(64);
    vibeos_rtl_free(&local);
    vibeos_rtl_free((char *)a + 16);
    vibeos_rtl_free(0);
    vibeos_rtl_free(a);
    vibeos_rtl_free(a);
    vibeos_rtl_heap_stats(&full, 0, &blocks);
    CHECK(full == VIBEOS_RTL_ARENA_SIZE && blocks == 1, "foreign, interior and double frees are ignored (%zu)",
          full);

    /* The whole arena in one request cannot be met (headers), a smaller one can,
     * and what is freed is allocatable again. */
    CHECK(vibeos_rtl_alloc(VIBEOS_RTL_ARENA_SIZE) == 0, "a request for the whole arena fails");
    CHECK(vibeos_rtl_alloc((size_t)-1) == 0, "an absurd request fails without wrapping");
    big = vibeos_rtl_alloc(VIBEOS_RTL_ARENA_SIZE - 64);
    CHECK(big != 0, "nearly the whole arena can be had");
    CHECK(vibeos_rtl_alloc(4096) == 0, "and then nothing else can");
    vibeos_rtl_free(big);
    for (i = 0; i < 64; i++) {
        held[i] = vibeos_rtl_alloc(1000 + (size_t)i);
        memset(held[i], i, 1000 + (size_t)i);
    }
    for (i = 0; i < 64; i++) {
        size_t n = 1000 + (size_t)i;

        CHECK(((unsigned char *)held[i])[0] == i && ((unsigned char *)held[i])[n - 1] == i,
              "block %d kept its contents beside 63 others", i);
    }
    for (i = 0; i < 64; i += 2) {
        vibeos_rtl_free(held[i]);
    }
    for (i = 1; i < 64; i += 2) {
        vibeos_rtl_free(held[i]);
    }
    vibeos_rtl_heap_stats(&full, 0, &blocks);
    CHECK(full == VIBEOS_RTL_ARENA_SIZE && blocks == 1, "freed in an interleaved order, it coalesces fully");
}

/* ---- STRING, UNICODE_STRING ------------------------------------------------------------- */

static void t_init(void) {
    vibeos_astring_t as;
    vibeos_ustring_t us;
    vibeos_wchar_t w[8];
    static char longa[70000];
    static vibeos_wchar_t longw[40000];

    RtlInitString(&as, "abc");
    CHECK(as.Length == 3 && as.MaximumLength == 4 && as.Buffer && !strcmp(as.Buffer, "abc"),
          "RtlInitString: Length excludes the NUL, MaximumLength includes it");
    RtlInitString(&as, "");
    CHECK(as.Length == 0 && as.MaximumLength == 1, "an empty string has MaximumLength 1");
    RtlInitString(&as, 0);
    CHECK(as.Length == 0 && as.MaximumLength == 0 && as.Buffer == 0, "a NULL source gives an all-zero string");
    memset(longa, 'x', sizeof(longa) - 1);
    longa[sizeof(longa) - 1] = 0;
    RtlInitString(&as, longa);
    CHECK(as.Length == 0xFFFE && as.MaximumLength == 0xFFFF, "a string past 64 KiB is cut at 0xFFFE (%u, %u)",
          as.Length, as.MaximumLength);

    RtlInitUnicodeString(&us, wide(w, "abc"));
    CHECK(us.Length == 6 && us.MaximumLength == 8 && us.Buffer == w,
          "RtlInitUnicodeString counts bytes: 6 for three characters, 8 with the NUL");
    RtlInitUnicodeString(&us, wide(w, ""));
    CHECK(us.Length == 0 && us.MaximumLength == 2, "an empty wide string has MaximumLength 2");
    RtlInitUnicodeString(&us, 0);
    CHECK(us.Length == 0 && us.MaximumLength == 0 && us.Buffer == 0, "a NULL wide source gives an all-zero string");
    for (size_t i = 0; i < 39999; i++) {
        longw[i] = 'y';
    }
    longw[39999] = 0;
    RtlInitUnicodeString(&us, longw);
    CHECK(us.Length == 0xFFFC && us.MaximumLength == 0xFFFE, "a wide string past 32 K characters is cut at 0xFFFC (%u, %u)",
          us.Length, us.MaximumLength);
}

static void t_create_free(void) {
    size_t before = heap_free_bytes();
    vibeos_ustring_t u;
    vibeos_wchar_t w[16];

    memset(&u, 0xEE, sizeof(u));
    CHECK(RtlCreateUnicodeString(&u, wide(w, "Hello")) == 1, "create returns TRUE");
    CHECK(u.Length == 10 && u.MaximumLength == 12, "Length 10, MaximumLength 12 (%u, %u)", u.Length, u.MaximumLength);
    CHECK(u.Buffer != w && u.Buffer[5] == 0 && u.Buffer[0] == 'H' && u.Buffer[4] == 'o',
          "a terminated copy in a buffer of its own");
    RtlFreeUnicodeString(&u);
    CHECK(u.Length == 0 && u.MaximumLength == 0 && u.Buffer == 0, "free zeroes the string");
    CHECK(heap_free_bytes() == before, "and gives the buffer back");
    RtlFreeUnicodeString(&u);                  /* freeing the freed is harmless */

    CHECK(RtlCreateUnicodeString(&u, wide(w, "")) == 1 && u.Length == 0 && u.MaximumLength == 2 && u.Buffer[0] == 0,
          "an empty source gives an empty, terminated string");
    RtlFreeUnicodeString(&u);
    memset(&u, 0, sizeof(u));
    CHECK(RtlCreateUnicodeString(&u, 0) == 0, "a NULL source is refused");
    {
        static vibeos_wchar_t huge[40000];

        for (size_t i = 0; i < 39999; i++) huge[i] = 'z';
        CHECK(RtlCreateUnicodeString(&u, huge) == 0, "a source that cannot be a UNICODE_STRING is refused");
    }
    CHECK(heap_free_bytes() == before, "no refusal leaked");
}

static void t_duplicate(void) {
    size_t before = heap_free_bytes();
    vibeos_ustring_t src, dst;
    vibeos_wchar_t w[16];
    vibeos_ntstatus_t st;

    ustr(&src, w, "abc");
    memset(&dst, 0xEE, sizeof(dst));
    st = RtlDuplicateUnicodeString(0, &src, &dst);
    CHECK(st == 0 && dst.Length == 6 && dst.MaximumLength == 6 && dst.Buffer != w && !memcmp(dst.Buffer, w, 6),
          "flags 0: an exact copy, no terminator (%08x, %u/%u)", (unsigned)st, dst.Length, dst.MaximumLength);
    RtlFreeUnicodeString(&dst);

    st = RtlDuplicateUnicodeString(VIBEOS_RTL_DUPLICATE_UNICODE_STRING_NULL_TERMINATE, &src, &dst);
    CHECK(st == 0 && dst.Length == 6 && dst.MaximumLength == 8 && dst.Buffer[3] == 0 && dst.Buffer[2] == 'c',
          "NULL_TERMINATE: Length 6, MaximumLength 8, a NUL after the text");
    RtlFreeUnicodeString(&dst);

    src.Length = 0;
    memset(&dst, 0xEE, sizeof(dst));
    st = RtlDuplicateUnicodeString(VIBEOS_RTL_DUPLICATE_UNICODE_STRING_NULL_TERMINATE, &src, &dst);
    CHECK(st == 0 && dst.Length == 0 && dst.MaximumLength == 0 && dst.Buffer == 0,
          "an empty source needs no buffer unless asked for one");
    st = RtlDuplicateUnicodeString(VIBEOS_RTL_DUPLICATE_UNICODE_STRING_ALLOCATE_NULL_STRING, &src, &dst);
    CHECK(st == 0 && dst.Length == 0 && dst.MaximumLength == 0 && dst.Buffer != 0,
          "ALLOCATE_NULL_STRING gives an empty string a buffer");
    RtlFreeUnicodeString(&dst);
    st = RtlDuplicateUnicodeString(3, &src, &dst);
    CHECK(st == 0 && dst.Length == 0 && dst.MaximumLength == 2 && dst.Buffer && dst.Buffer[0] == 0,
          "both flags on an empty source: an empty, terminated string");
    RtlFreeUnicodeString(&dst);

    ustr(&src, w, "abc");
    CHECK(RtlDuplicateUnicodeString(4, &src, &dst) == (vibeos_ntstatus_t)VIBEOS_STATUS_INVALID_PARAMETER,
          "an unknown flag is STATUS_INVALID_PARAMETER");
    CHECK(RtlDuplicateUnicodeString(0, 0, &dst) == (vibeos_ntstatus_t)VIBEOS_STATUS_INVALID_PARAMETER,
          "a NULL source is STATUS_INVALID_PARAMETER");
    CHECK(RtlDuplicateUnicodeString(0, &src, 0) == (vibeos_ntstatus_t)VIBEOS_STATUS_INVALID_PARAMETER,
          "a NULL destination is STATUS_INVALID_PARAMETER");
    {
        static vibeos_wchar_t big[0x8000];

        src.Buffer = big;
        src.Length = 0xFFFE;
        src.MaximumLength = 0xFFFE;
        st = RtlDuplicateUnicodeString(VIBEOS_RTL_DUPLICATE_UNICODE_STRING_NULL_TERMINATE, &src, &dst);
        CHECK(st == (vibeos_ntstatus_t)VIBEOS_STATUS_NAME_TOO_LONG,
              "a terminator that would overflow MaximumLength is STATUS_NAME_TOO_LONG (%08x)", (unsigned)st);
    }
    CHECK(heap_free_bytes() == before, "nothing leaked");
}

static void t_compare(void) {
    vibeos_astring_t a, b;
    vibeos_ustring_t ua, ub;
    vibeos_wchar_t wa[16], wb[16];

    RtlInitString(&a, "abc");
    RtlInitString(&b, "abc");
    CHECK(RtlCompareString(&a, &b, 0) == 0, "equal strings compare 0");
    RtlInitString(&b, "abd");
    CHECK(RtlCompareString(&a, &b, 0) < 0 && RtlCompareString(&b, &a, 0) > 0, "c < d, and the reverse");
    RtlInitString(&b, "abcd");
    CHECK(RtlCompareString(&a, &b, 0) < 0 && RtlCompareString(&b, &a, 0) > 0,
          "a string that is a prefix of the other is the smaller");
    RtlInitString(&b, "ABC");
    CHECK(RtlCompareString(&a, &b, 0) > 0, "'a' > 'A' when case counts");
    CHECK(RtlCompareString(&a, &b, 1) == 0, "and equal when it does not");
    RtlInitString(&a, "\xE9");
    RtlInitString(&b, "\xC9");
    CHECK(RtlCompareString(&a, &b, 0) > 0 && RtlCompareString(&a, &b, 1) == 0,
          "case-insensitive comparison folds the code page's Latin-1 letters too");
    RtlInitString(&a, "\xF7");
    RtlInitString(&b, "\xD7");
    CHECK(RtlCompareString(&a, &b, 1) != 0, "the division sign is not a letter");
    a.Length = 0;
    b.Length = 0;
    CHECK(RtlCompareString(&a, &b, 1) == 0, "two empty strings are equal");
    RtlInitString(&a, "ab");
    RtlInitString(&b, "abXYZ");
    a.Length = 2;
    b.Length = 2;
    CHECK(RtlCompareString(&a, &b, 0) == 0, "only Length bytes are compared, not the terminator or beyond");

    ustr(&ua, wa, "abc");
    ustr(&ub, wb, "abc");
    CHECK(RtlCompareUnicodeString(&ua, &ub, 0) == 0, "equal wide strings compare 0");
    ustr(&ub, wb, "abd");
    CHECK(RtlCompareUnicodeString(&ua, &ub, 0) < 0 && RtlCompareUnicodeString(&ub, &ua, 0) > 0, "c < d");
    ustr(&ub, wb, "abcd");
    CHECK(RtlCompareUnicodeString(&ua, &ub, 0) < 0 && RtlCompareUnicodeString(&ub, &ua, 0) > 0, "prefix is smaller");
    ustr(&ub, wb, "ABC");
    CHECK(RtlCompareUnicodeString(&ua, &ub, 0) > 0 && RtlCompareUnicodeString(&ua, &ub, 1) == 0, "case flag");
    wa[0] = 0xE9;
    wb[0] = 0xC9;
    ua.Length = ub.Length = 2;
    CHECK(RtlCompareUnicodeString(&ua, &ub, 1) == 0 && RtlCompareUnicodeString(&ua, &ub, 0) > 0,
          "U+00E9 and U+00C9 differ by case");
    ua.Length = 3;      /* an odd byte length: the stray byte is not a character */
    ub.Length = 2;
    CHECK(RtlCompareUnicodeString(&ua, &ub, 1) > 0, "a longer length still sorts after, odd or not");
    wa[0] = 0x4E2D;
    wb[0] = 0x0041;
    ua.Length = ub.Length = 2;
    CHECK(RtlCompareUnicodeString(&ua, &ub, 0) > 0 && RtlCompareUnicodeString(&ub, &ua, 0) < 0,
          "code units compare as unsigned: U+4E2D > U+0041");
    wa[0] = 0x8000;
    wb[0] = 0x0001;
    CHECK(RtlCompareUnicodeString(&ua, &ub, 0) > 0, "and U+8000 is not negative");
}

static void t_upper(void) {
    char d[8];
    vibeos_astring_t src, dst;
    unsigned char in[] = {'a', 'z', 'A', '1', 0xE0, 0xFE, 0xF7, 0xFF};
    static const unsigned char want[] = {'A', 'Z', 'A', '1', 0xC0, 0xDE, 0xF7, 0x9F};
    unsigned i;

    src.Buffer = (char *)in;
    src.Length = sizeof(in);
    src.MaximumLength = sizeof(in);
    dst.Buffer = d;
    dst.Length = 0;
    dst.MaximumLength = sizeof(d);
    RtlUpperString(&dst, &src);
    CHECK(dst.Length == 8, "all eight bytes converted (%u)", dst.Length);
    for (i = 0; i < 8; i++) {
        CHECK((unsigned char)d[i] == want[i], "byte %u: %02x -> %02x, want %02x", i, in[i], (unsigned char)d[i], want[i]);
    }
    dst.MaximumLength = 3;
    dst.Length = 0;
    RtlUpperString(&dst, &src);
    CHECK(dst.Length == 3, "a destination smaller than the source takes what fits (%u)", dst.Length);
    in[0] = 0x9A;
    dst.MaximumLength = sizeof(d);
    RtlUpperString(&dst, &src);
    CHECK((unsigned char)d[0] == 0x8A, "Windows-1252's s caron goes up to S caron");
}

static void t_chars(void) {
    unsigned char text[] = {'A', 0x80, 0x9F, 0x81, 0xE9};
    unsigned char *p = text;
    unsigned c;

    CHECK(RtlUpcaseUnicodeChar('a') == 'A' && RtlUpcaseUnicodeChar('z') == 'Z' && RtlUpcaseUnicodeChar('A') == 'A',
          "ASCII letters");
    CHECK(RtlUpcaseUnicodeChar('@') == '@' && RtlUpcaseUnicodeChar('`') == '`' && RtlUpcaseUnicodeChar('{') == '{',
          "the characters beside the letters are untouched");
    CHECK(RtlUpcaseUnicodeChar(0xE0) == 0xC0 && RtlUpcaseUnicodeChar(0xFE) == 0xDE && RtlUpcaseUnicodeChar(0xE9) == 0xC9,
          "Latin-1 lower-case letters go up by 0x20");
    CHECK(RtlUpcaseUnicodeChar(0xF7) == 0xF7 && RtlUpcaseUnicodeChar(0xDF) == 0xDF && RtlUpcaseUnicodeChar(0xDF - 1) == 0xDE,
          "the division sign and sharp s have no upper case");
    CHECK(RtlUpcaseUnicodeChar(0xFF) == 0x178, "y diaeresis goes to U+0178");
    CHECK(RtlUpcaseUnicodeChar(0x3B1) == 0x3B1 && RtlUpcaseUnicodeChar(0) == 0 && RtlUpcaseUnicodeChar(0xFFFF) == 0xFFFF,
          "outside the subset a character is unchanged");

    c = RtlAnsiCharToUnicodeChar(&p);
    CHECK(c == 'A' && p == text + 1, "ASCII, and the pointer advances by one");
    CHECK(RtlAnsiCharToUnicodeChar(&p) == 0x20AC, "0x80 is the euro sign");
    CHECK(RtlAnsiCharToUnicodeChar(&p) == 0x0178, "0x9F is Y diaeresis");
    CHECK(RtlAnsiCharToUnicodeChar(&p) == 0x0081, "0x81, undefined in 1252, maps to the control character");
    CHECK(RtlAnsiCharToUnicodeChar(&p) == 0x00E9 && p == text + 5, "0xE9 is Latin-1's e acute");
}

static void t_multibyte(void) {
    vibeos_wchar_t out[8];
    uint32_t n;
    vibeos_ntstatus_t st;

    memset(out, 0xEE, sizeof(out));
    n = 77;
    st = RtlMultiByteToUnicodeN(out, sizeof(out), &n, "ab\x80z", 4);
    CHECK(st == 0 && n == 8 && out[0] == 'a' && out[1] == 'b' && out[2] == 0x20AC && out[3] == 'z',
          "four bytes, four characters, ResultSize 8 bytes (%u)", n);
    CHECK(out[4] == 0xEEEE, "nothing is written past the converted text, and no NUL is added");

    memset(out, 0xEE, sizeof(out));
    st = RtlMultiByteToUnicodeN(out, 4, &n, "abcd", 4);
    CHECK(st == 0 && n == 4 && out[0] == 'a' && out[1] == 'b' && out[2] == 0xEEEE,
          "a destination of 4 bytes takes two characters and still succeeds (%08x, %u)", (unsigned)st, n);
    memset(out, 0xEE, sizeof(out));
    st = RtlMultiByteToUnicodeN(out, 5, &n, "abcd", 4);
    CHECK(st == 0 && n == 4 && out[2] == 0xEEEE, "an odd destination size is rounded down");
    st = RtlMultiByteToUnicodeN(out, 0, &n, "abcd", 4);
    CHECK(st == 0 && n == 0, "an empty destination takes nothing");
    st = RtlMultiByteToUnicodeN(0, 0, &n, "abcd", 4);
    CHECK(st == 0 && n == 8, "a NULL destination asks for the size: 8 bytes");
    st = RtlMultiByteToUnicodeN(out, sizeof(out), &n, "abcd", 0);
    CHECK(st == 0 && n == 0, "an empty source gives an empty result");
    st = RtlMultiByteToUnicodeN(out, sizeof(out), 0, "ab", 2);
    CHECK(st == 0 && out[1] == 'b', "ResultSize is optional");
    st = RtlMultiByteToUnicodeN(out, sizeof(out), &n, "\xFF\xA0\x9D", 3);
    CHECK(st == 0 && out[0] == 0x00FF && out[1] == 0x00A0 && out[2] == 0x009D, "the top of the table");
}

/* ---- memory, checksum, generators --------------------------------------------------------- */

static void t_memory_ulong(void) {
    uint32_t v[5] = {7, 7, 7, 9, 7};

    CHECK(RtlCompareMemoryUlong(v, 20, 7) == 12, "three matching words are 12 bytes");
    CHECK(RtlCompareMemoryUlong(v, 8, 7) == 8, "all of a shorter range");
    CHECK(RtlCompareMemoryUlong(v, 10, 7) == 8, "a Length that is not a multiple of four is rounded down");
    CHECK(RtlCompareMemoryUlong(v, 3, 7) == 0 && RtlCompareMemoryUlong(v, 0, 7) == 0, "under one word: 0");
    CHECK(RtlCompareMemoryUlong(v, 20, 8) == 0, "no match at the first word: 0");
    CHECK(RtlCompareMemoryUlong(v + 3, 8, 9) == 4, "starts where it is told");
    {
        uint8_t raw[12] = {0};
        uint32_t pat = 0x01020304u;

        memcpy(raw + 1, &pat, 4);
        memcpy(raw + 5, &pat, 4);
        CHECK(RtlCompareMemoryUlong(raw + 1, 8, pat) == 8, "unaligned source works on this host");
    }
}

static void t_crc(void) {
    const uint8_t digits[] = "123456789";
    uint32_t part;

    CHECK(RtlComputeCrc32(0, digits, 9) == 0xCBF43926u, "CRC-32 of \"123456789\" is 0xCBF43926 (%08x)",
          RtlComputeCrc32(0, digits, 9));
    CHECK(RtlComputeCrc32(0, (const uint8_t *)"a", 1) == 0xE8B7BE43u, "CRC-32 of \"a\"");
    CHECK(RtlComputeCrc32(0, (const uint8_t *)"abc", 3) == 0x352441C2u, "CRC-32 of \"abc\"");
    CHECK(RtlComputeCrc32(0, digits, 0) == 0, "nothing hashed leaves the initial value");
    CHECK(RtlComputeCrc32(0x12345678u, digits, 0) == 0x12345678u, "and a non-zero initial value too");
    part = RtlComputeCrc32(0, digits, 4);
    CHECK(RtlComputeCrc32(part, digits + 4, 5) == 0xCBF43926u, "continuing from the CRC of the first part");
    CHECK(RtlComputeCrc32(0, (const uint8_t *)"\0\0\0\0", 4) == 0x2144DF1Cu, "four zero bytes");
    CHECK(RtlComputeCrc32(0, digits, -1) == 0, "a negative length hashes nothing");
}

/* The documented generator, again, written differently from the code: the modulus
 * is 2^31 - 1, so the reduction folds the high bits onto the low ones instead of
 * dividing. */
static uint32_t model_uniform(uint32_t *seed) {
    uint64_t v = (uint64_t)*seed * 2147483629ull + 2147483587ull;   /* 0x7FFFFFED, 0x7FFFFFC3 */

    while (v >> 31) {
        v = (v & 0x7FFFFFFFull) + (v >> 31);
    }
    if (v == 0x7FFFFFFFull) {
        v = 0;
    }
    *seed = (uint32_t)v;
    return *seed;
}

static void t_random(void) {
    uint32_t s = 0, m = 0, r_s, r_m, tab[128], last;
    int i, k, bad;

    CHECK(RtlUniform(&s) == 0x7FFFFFC3u && s == 0x7FFFFFC3u, "RtlUniform(0) is the constant, and replaces the seed");
    s = 1;
    CHECK(RtlUniform(&s) == 0x7FFFFFB1u, "RtlUniform(1) is (0x7FFFFFED + 0x7FFFFFC3) mod 0x7FFFFFFF (%08x)", s);
    s = 0x7FFFFFFEu;
    r_s = RtlUniform(&s);
    m = 0x7FFFFFFEu;
    CHECK(r_s == model_uniform(&m), "the product does not wrap at 32 bits");

    bad = 0;
    for (k = 0; k < 3; k++) {
        s = m = (uint32_t)(k * 0x1234567u + 1u);
        for (i = 0; i < 2000; i++) {
            r_s = RtlUniform(&s);
            r_m = model_uniform(&m);
            if (r_s != r_m || s != m || r_s >= 0x7FFFFFFFu) {
                bad++;
            }
        }
    }
    CHECK(bad == 0, "6000 steps of RtlUniform agree with the model (%d differ)", bad);

    /* RtlRandom: Knuth's shuffle of RtlUniform's output through 128 slots, as
     * documented; the model keeps its own table. */
    vibeos_rtl_random_reset();
    s = m = 0x2468ACE1u;
    for (i = 0; i < 128; i++) {
        tab[i] = model_uniform(&m);
    }
    last = model_uniform(&m);
    bad = 0;
    for (i = 0; i < 1000; i++) {
        uint32_t pos = (uint32_t)(((uint64_t)last * 128u) / 0x7FFFFFFFu);

        last = tab[pos];
        tab[pos] = model_uniform(&m);
        r_s = RtlRandom(&s);
        if (r_s != last || s != m || r_s >= 0x7FFFFFFFu) {
            bad++;
        }
    }
    CHECK(bad == 0, "1000 results of RtlRandom agree with the shuffle model (%d differ)", bad);

    /* Same seed, same sequence after a reset; a different seed, a different one. */
    vibeos_rtl_random_reset();
    s = 99;
    r_s = RtlRandom(&s);
    vibeos_rtl_random_reset();
    m = 99;
    r_m = RtlRandom(&m);
    CHECK(r_s == r_m, "the same seed reproduces the sequence");
    vibeos_rtl_random_reset();
    m = 100;
    CHECK(RtlRandom(&m) != r_s, "a different seed does not");
    {
        int distinct = 0;

        vibeos_rtl_random_reset();
        s = 5;
        last = RtlRandom(&s);
        for (i = 0; i < 100; i++) {
            uint32_t n = RtlRandom(&s);

            distinct += n != last;
            last = n;
        }
        CHECK(distinct > 95, "successive results differ (%d of 100)", distinct);
    }
}

int main(void) {
    setvbuf(stdout, 0, _IONBF, 0);
    t_heap();
    t_init();
    t_create_free();
    t_duplicate();
    t_compare();
    t_upper();
    t_chars();
    t_multibyte();
    t_memory_ulong();
    t_crc();
    t_random();
    ntrtl_run_ip();
    ntrtl_run_system();
    printf("ntrtl: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
