/* RtlIpv4/Ipv6 address conversions (docs/abi/windows.md W2), against the forms
 * Microsoft documents: dotted decimal; the classic inet_addr forms (one to four
 * parts, octal and hexadecimal) when not strict; RFC 4291 text with "::" and a
 * trailing dotted quad. Where a parse stopped is part of what the functions
 * return, so the terminator is checked on every failure that is a matter of the
 * text (a stray character, an empty part) and not on those that are a matter of
 * counting (too few groups), which the documentation does not place. */

#include "ntrtl_test.h"

#define ST_BAD ((vibeos_ntstatus_t)VIBEOS_STATUS_INVALID_PARAMETER)

static void t_ipv4_to_string(void) {
    static const struct { uint8_t b[4]; const char *s; } cases[] = {
        {{0, 0, 0, 0}, "0.0.0.0"},
        {{1, 2, 3, 4}, "1.2.3.4"},
        {{127, 0, 0, 1}, "127.0.0.1"},
        {{255, 255, 255, 255}, "255.255.255.255"},
        {{10, 100, 200, 9}, "10.100.200.9"},
    };
    char buf[32];
    unsigned i;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        vibeos_in_addr_t a;
        char *end;

        memcpy(a.Byte, cases[i].b, 4);
        memset(buf, 'Q', sizeof(buf));
        end = RtlIpv4AddressToStringA(&a, buf);
        CHECK(!strcmp(buf, cases[i].s), "%s -> \"%s\"", cases[i].s, buf);
        CHECK(end == buf + strlen(cases[i].s) && *end == 0, "the result points at the NUL that ends %s",
              cases[i].s);
        CHECK(buf[strlen(cases[i].s) + 1] == 'Q', "nothing is written past the NUL of %s", cases[i].s);
    }
}

typedef struct {
    const char *text;
    int strict;
    int ok;
    uint8_t addr[4];
    int term;                  /* where parsing stopped; -1: not asserted */
} v4case_t;

static void t_ipv4_from_string(void) {
    static const v4case_t cases[] = {
        /* strict: four decimal parts */
        {"1.2.3.4", 1, 1, {1, 2, 3, 4}, 7},
        {"0.0.0.0", 1, 1, {0, 0, 0, 0}, 7},
        {"255.255.255.255", 1, 1, {255, 255, 255, 255}, 15},
        {"001.002.003.004", 1, 1, {1, 2, 3, 4}, 15},
        {"010.1.1.1", 1, 1, {10, 1, 1, 1}, 9},
        {"256.1.1.1", 1, 0, {0}, -1},
        {"1.2.3.256", 1, 0, {0}, -1},
        {"1.2.3", 1, 0, {0}, -1},
        {"1", 1, 0, {0}, -1},
        {"0x1.2.3.4", 1, 0, {0}, 1},
        /* the same text, not strict */
        {"1.2.3.4", 0, 1, {1, 2, 3, 4}, 7},
        /* text that is wrong either way */
        {"", 0, 0, {0}, 0},
        {"", 1, 0, {0}, 0},
        {".", 0, 0, {0}, 0},
        {"a.b.c.d", 0, 0, {0}, 0},
        {" 1.2.3.4", 0, 0, {0}, 0},
        {"1..2", 0, 0, {0}, 2},
        {"1.2.3.", 0, 0, {0}, 6},
        {"1.2.3.", 1, 0, {0}, 6},
        {"1.2.3.4.5", 0, 0, {0}, 7},
        {"1.2.3.4.5", 1, 0, {0}, 7},
        {"1.2.3.4x", 0, 0, {0}, 7},
        {"1.2.3.4 ", 1, 0, {0}, 7},
        {"1.2.3.4/8", 0, 0, {0}, 7},
        /* not strict: fewer parts, the last filling what is left */
        {"1.2.3", 0, 1, {1, 2, 0, 3}, 5},
        {"1.2", 0, 1, {1, 0, 0, 2}, 3},
        {"16909060", 0, 1, {1, 2, 3, 4}, 8},
        {"0", 0, 1, {0, 0, 0, 0}, 1},
        {"4294967295", 0, 1, {255, 255, 255, 255}, 10},
        {"1.2.65535", 0, 1, {1, 2, 255, 255}, 9},
        {"1.16777215", 0, 1, {1, 255, 255, 255}, 10},
        /* not strict: octal and hexadecimal parts */
        {"0x7f.1", 0, 1, {127, 0, 0, 1}, 6},
        {"0X7F.0.0.1", 0, 1, {127, 0, 0, 1}, 10},
        {"0300.0250.0.1", 0, 1, {192, 168, 0, 1}, 13},
        {"0x01020304", 0, 1, {1, 2, 3, 4}, 10},
        {"1.0x2.3.4", 0, 1, {1, 2, 3, 4}, 9},
        {"010", 0, 1, {0, 0, 0, 8}, 3},
        {"00", 0, 1, {0, 0, 0, 0}, 2},
        {"0xff.0xFF.0xfF.0Xff", 0, 1, {255, 255, 255, 255}, 19},
        /* not strict: out of range */
        {"4294967296", 0, 0, {0}, -1},
        {"1.2.65536", 0, 0, {0}, -1},
        {"1.16777216", 0, 0, {0}, -1},
        {"256.1", 0, 0, {0}, -1},
        {"1.256.3", 0, 0, {0}, -1},
        {"08", 0, 0, {0}, 1},
        {"0x", 0, 0, {0}, 2},
        {"0x.1", 0, 0, {0}, 2},
    };
    unsigned i;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const v4case_t *c = &cases[i];
        vibeos_in_addr_t a;
        const char *term = (const char *)(uintptr_t)1;
        vibeos_ntstatus_t st;

        memset(a.Byte, 0xAA, 4);
        st = RtlIpv4StringToAddressA(c->text, (vibeos_boolean_t)c->strict, &term, &a);
        if (c->ok) {
            CHECK(st == 0, "\"%s\" strict=%d: STATUS_SUCCESS (got %08x)", c->text, c->strict, (unsigned)st);
            CHECK(!memcmp(a.Byte, c->addr, 4), "\"%s\" strict=%d -> %u.%u.%u.%u", c->text, c->strict, a.Byte[0],
                  a.Byte[1], a.Byte[2], a.Byte[3]);
        } else {
            CHECK(st == ST_BAD, "\"%s\" strict=%d: STATUS_INVALID_PARAMETER (got %08x)", c->text, c->strict,
                  (unsigned)st);
            CHECK(a.Byte[0] == 0xAA && a.Byte[3] == 0xAA, "\"%s\": the address is not written on failure", c->text);
        }
        if (c->term >= 0) {
            CHECK(term == c->text + c->term, "\"%s\" strict=%d: terminator at %d (got %d)", c->text, c->strict,
                  c->term, (int)(term - c->text));
        }
    }
    {
        vibeos_in_addr_t a;

        CHECK(RtlIpv4StringToAddressA("9.8.7.6", 1, 0, &a) == 0 && a.Byte[0] == 9 && a.Byte[3] == 6,
              "the terminator pointer is optional");
        CHECK(RtlIpv4StringToAddressA(0, 1, 0, &a) == ST_BAD, "a NULL string is refused");
    }
}

static void g8(vibeos_in6_addr_t *a, unsigned g0, unsigned g1, unsigned g2, unsigned g3, unsigned g4,
               unsigned g5, unsigned g6, unsigned g7) {
    const unsigned g[8] = {g0, g1, g2, g3, g4, g5, g6, g7};
    int i;

    for (i = 0; i < 8; i++) {
        a->Byte[i * 2] = (uint8_t)(g[i] >> 8);
        a->Byte[i * 2 + 1] = (uint8_t)g[i];
    }
}

static void t_ipv6_to_string(void) {
    struct { unsigned g[8]; const char *s; } cases[] = {
        {{0, 0, 0, 0, 0, 0, 0, 0}, "::"},
        {{0, 0, 0, 0, 0, 0, 0, 1}, "::1"},
        {{1, 0, 0, 0, 0, 0, 0, 0}, "1::"},
        {{0x2001, 0xdb8, 0, 0, 0, 0, 0, 1}, "2001:db8::1"},
        {{0xfe80, 0, 0, 0, 0x1234, 0, 0, 1}, "fe80::1234:0:0:1"},
        {{1, 0, 0, 2, 0, 0, 0, 3}, "1:0:0:2::3"},
        {{1, 0, 0, 2, 0, 0, 3, 4}, "1::2:0:0:3:4"},
        {{1, 0, 2, 3, 4, 5, 6, 7}, "1:0:2:3:4:5:6:7"},
        {{0, 1, 2, 3, 4, 5, 6, 0}, "0:1:2:3:4:5:6:0"},
        {{0xABCD, 0x0EF0, 0x0001, 0x00A0, 0xFFFF, 0x1000, 0x0100, 0x0010}, "abcd:ef0:1:a0:ffff:1000:100:10"},
        {{0, 0, 0, 0, 0, 0xffff, 0x0102, 0x0304}, "::ffff:1.2.3.4"},
        {{0, 0, 0, 0, 0, 0xffff, 0, 0}, "::ffff:0.0.0.0"},
        {{0, 0, 0, 0, 0, 0xffff, 0xffff, 0xffff}, "::ffff:255.255.255.255"},
        {{0, 0, 0, 0, 0, 1, 0x0102, 0x0304}, "::1:102:304"},
    };
    unsigned i;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        vibeos_in6_addr_t a;
        char buf[64], *end;

        g8(&a, cases[i].g[0], cases[i].g[1], cases[i].g[2], cases[i].g[3], cases[i].g[4], cases[i].g[5],
           cases[i].g[6], cases[i].g[7]);
        memset(buf, 'Q', sizeof(buf));
        end = RtlIpv6AddressToStringA(&a, buf);
        CHECK(!strcmp(buf, cases[i].s), "-> \"%s\" (want \"%s\")", buf, cases[i].s);
        CHECK(end == buf + strlen(cases[i].s) && *end == 0, "the result points at the NUL after \"%s\"", cases[i].s);
        CHECK(buf[strlen(cases[i].s) + 1] == 'Q', "nothing is written past the NUL of \"%s\"", cases[i].s);
    }
}

typedef struct {
    const char *text;
    int ok;
    unsigned g[8];
    int term;
} v6case_t;

static void t_ipv6_from_string(void) {
    static const v6case_t cases[] = {
        {"::", 1, {0, 0, 0, 0, 0, 0, 0, 0}, 2},
        {"::1", 1, {0, 0, 0, 0, 0, 0, 0, 1}, 3},
        {"1::", 1, {1, 0, 0, 0, 0, 0, 0, 0}, 3},
        {"1:2:3:4:5:6:7:8", 1, {1, 2, 3, 4, 5, 6, 7, 8}, 15},
        {"2001:db8::1", 1, {0x2001, 0xdb8, 0, 0, 0, 0, 0, 1}, 11},
        {"2001:DB8:0:0:0:0:0:1", 1, {0x2001, 0xdb8, 0, 0, 0, 0, 0, 1}, 20},
        {"1:2::7:8", 1, {1, 2, 0, 0, 0, 0, 7, 8}, 8},
        {"1:2:3:4:5:6:7::", 1, {1, 2, 3, 4, 5, 6, 7, 0}, 15},
        {"::2:3:4:5:6:7:8", 1, {0, 2, 3, 4, 5, 6, 7, 8}, 15},
        {"0000:0000::0001", 1, {0, 0, 0, 0, 0, 0, 0, 1}, 15},
        {"::ffff:1.2.3.4", 1, {0, 0, 0, 0, 0, 0xffff, 0x0102, 0x0304}, 14},
        {"1:2:3:4:5:6:1.2.3.4", 1, {1, 2, 3, 4, 5, 6, 0x0102, 0x0304}, 19},
        {"::1.2.3.4", 1, {0, 0, 0, 0, 0, 0, 0x0102, 0x0304}, 9},
        {"abcd:EF01:2345:6789:abcd:ef01:2345:6789", 1,
         {0xabcd, 0xef01, 0x2345, 0x6789, 0xabcd, 0xef01, 0x2345, 0x6789}, 39},
        /* wrong text */
        {"", 0, {0}, 0},
        {":1:2:3:4:5:6:7", 0, {0}, 0},
        {"g::", 0, {0}, 0},
        {"::g", 0, {0}, 2},
        {":::", 0, {0}, 2},
        {"1::2x", 0, {0}, 4},
        {"1::2 ", 0, {0}, 4},
        {"1:2:3:4:5:6:7:8x", 0, {0}, 15},
        /* wrong counts */
        {"1:2:3:4:5:6:7", 0, {0}, -1},
        {"1:2:3:4:5:6:7:8:9", 0, {0}, -1},
        {"1::2::3", 0, {0}, -1},
        {"12345::", 0, {0}, -1},
        {"1:2:3:4:5:6:7::8", 0, {0}, -1},
        {"1:2:3:4:5:6:7:8::", 0, {0}, -1},
        {"1.2.3.4", 0, {0}, -1},
        {"1:2:3:4:5:6:7:1.2.3.4", 0, {0}, -1},
        {"::1.2.3", 0, {0}, -1},
        {"::1.2.3.256", 0, {0}, -1},
        {"::a.2.3.4", 0, {0}, -1},
    };
    unsigned i;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const v6case_t *c = &cases[i];
        vibeos_in6_addr_t a, want;
        vibeos_wchar_t w[64];
        const char *term = (const char *)(uintptr_t)1;
        const vibeos_wchar_t *wterm = (const vibeos_wchar_t *)(uintptr_t)1;
        vibeos_in6_addr_t b;
        vibeos_ntstatus_t st, stw;

        memset(&a, 0xAA, sizeof(a));
        memset(&b, 0xAA, sizeof(b));
        st = RtlIpv6StringToAddressA(c->text, &term, &a);
        stw = RtlIpv6StringToAddressW(wide(w, c->text), &wterm, &b);
        if (c->ok) {
            g8(&want, c->g[0], c->g[1], c->g[2], c->g[3], c->g[4], c->g[5], c->g[6], c->g[7]);
            CHECK(st == 0, "\"%s\": STATUS_SUCCESS (got %08x)", c->text, (unsigned)st);
            CHECK(!memcmp(&a, &want, 16), "\"%s\" parsed to the wrong address", c->text);
            CHECK(stw == 0 && !memcmp(&b, &want, 16), "\"%s\": the wide form agrees", c->text);
        } else {
            CHECK(st == ST_BAD, "\"%s\": STATUS_INVALID_PARAMETER (got %08x)", c->text, (unsigned)st);
            CHECK(stw == ST_BAD, "\"%s\": the wide form refuses too", c->text);
            CHECK(a.Byte[0] == 0xAA && a.Byte[15] == 0xAA, "\"%s\": the address is not written on failure", c->text);
        }
        if (c->term >= 0) {
            CHECK(term == c->text + c->term, "\"%s\": terminator at %d (got %d)", c->text, c->term,
                  (int)(term - c->text));
            CHECK(wterm == w + c->term, "\"%s\": wide terminator at %d", c->text, c->term);
        }
    }
    {
        vibeos_in6_addr_t a;

        CHECK(RtlIpv6StringToAddressA("::1", 0, &a) == 0 && a.Byte[15] == 1, "the terminator pointer is optional");
        CHECK(RtlIpv6StringToAddressA(0, 0, &a) == ST_BAD, "a NULL string is refused");
    }
}

/* Every address printed parses back to itself. */
static void t_ipv6_round_trip(void) {
    uint32_t seed = 12345;
    int i, bad = 0;

    for (i = 0; i < 2000; i++) {
        vibeos_in6_addr_t a, b;
        char buf[64];
        int k;

        for (k = 0; k < 16; k++) {
            /* Zeros often, so that runs of zero groups exist to be compressed. */
            seed = seed * 1103515245u + 12345u;
            a.Byte[k] = (seed >> 16) % 3 ? 0 : (uint8_t)(seed >> 8);
        }
        RtlIpv6AddressToStringA(&a, buf);
        if (RtlIpv6StringToAddressA(buf, 0, &b) != 0 || memcmp(&a, &b, 16)) {
            bad++;
        }
    }
    CHECK(bad == 0, "%d of 2000 addresses did not survive printing and parsing", bad);
}

void ntrtl_run_ip(void) {
    t_ipv4_to_string();
    t_ipv4_from_string();
    t_ipv6_to_string();
    t_ipv6_from_string();
    t_ipv6_round_trip();
}
