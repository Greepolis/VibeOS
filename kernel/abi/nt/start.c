/* NT personality: the blocks a process finds in its own memory when it starts
 * (docs/abi/windows.md W1). See vibeos_nt_build_blocks in vibeos/abi_nt.h.
 *
 * On Windows the parent builds the process parameters and the kernel the PEB
 * and the TEB. Here a Windows program's parent is, for now, always a Linux one
 * - a shell, init - so the kernel builds all three from what exec was given:
 * the arguments become one command line, as Windows passes them, and the
 * standard handles name descriptors 0, 1 and 2. NtCreateUserProcess (W6) is
 * where an NT parent supplies its own. */

#include "nt_internal.h"

typedef struct {
    uint8_t *const *pages;
    uint64_t base;
} nt_blk_t;

/* A byte at block offset `off`, whichever page it falls in. */
static void put8(const nt_blk_t *b, uint32_t off, uint8_t v) {
    b->pages[off / 4096u][off % 4096u] = v;
}

static void put16(const nt_blk_t *b, uint32_t off, uint16_t v) {
    put8(b, off, (uint8_t)v);
    put8(b, off + 1u, (uint8_t)(v >> 8));
}

static void put32(const nt_blk_t *b, uint32_t off, uint32_t v) {
    put16(b, off, (uint16_t)v);
    put16(b, off + 2u, (uint16_t)(v >> 16));
}

static void put64(const nt_blk_t *b, uint32_t off, uint64_t v) {
    put32(b, off, (uint32_t)v);
    put32(b, off + 4u, (uint32_t)(v >> 32));
}

/* Append `s`, UTF-8, as UTF-16 at *at, ending before `end`: the number of bytes
 * written, or -1 if it does not fit. A byte that does not begin a well-formed
 * sequence is taken as U+FFFD, as Windows' own conversion does. */
static long put_utf16(const nt_blk_t *b, uint32_t *at, uint32_t end, const char *s) {
    uint32_t start = *at;
    const uint8_t *p = (const uint8_t *)s;

    while (*p) {
        uint32_t c = *p++, need = 0, i;

        if (c >= 0xF0u && c < 0xF8u) {
            c &= 0x07u;
            need = 3;
        } else if (c >= 0xE0u) {
            c &= 0x0Fu;
            need = 2;
        } else if (c >= 0xC0u) {
            c &= 0x1Fu;
            need = 1;
        } else if (c >= 0x80u) {
            c = 0xFFFDu;
        }
        for (i = 0; i < need; i++) {
            if ((*p & 0xC0u) != 0x80u) {
                c = 0xFFFDu;
                break;
            }
            c = (c << 6) | (*p++ & 0x3Fu);
        }
        if (c >= 0x10000u) {
            if (*at + 4u > end) {
                return -1;
            }
            c -= 0x10000u;
            put16(b, *at, (uint16_t)(0xD800u + (c >> 10)));
            put16(b, *at + 2u, (uint16_t)(0xDC00u + (c & 0x3FFu)));
            *at += 4u;
        } else {
            if (*at + 2u > end) {
                return -1;
            }
            put16(b, *at, (uint16_t)c);
            *at += 2u;
        }
    }
    return (long)(*at - start);
}

/* A UNICODE_STRING at `field` naming `len` bytes at block offset `at`, with its
 * terminating zero counted in MaximumLength but not in Length. */
static void put_ustr(const nt_blk_t *b, uint32_t field, uint32_t at, uint32_t len) {
    put16(b, field + VIBEOS_USTR_LENGTH, (uint16_t)len);
    put16(b, field + VIBEOS_USTR_MAXIMUM, (uint16_t)(len + 2u));
    put64(b, field + VIBEOS_USTR_BUFFER, b->base + at);
}

static int needs_quotes(const char *a) {
    if (!*a) {
        return 1;
    }
    for (; *a; a++) {
        if (*a == ' ' || *a == '\t') {
            return 1;
        }
    }
    return 0;
}

int vibeos_nt_build_blocks(uint8_t *const pages[VIBEOS_NT_BLOCK_PAGES], uint64_t base,
                           const vibeos_nt_start_t *s) {
    nt_blk_t b;
    const uint32_t peb = VIBEOS_NT_BLOCK_PEB;
    const uint32_t rupp = VIBEOS_NT_BLOCK_PARAMS;
    const uint32_t teb = VIBEOS_NT_BLOCK_TEB;
    const uint32_t strings_end = VIBEOS_NT_BLOCK_TEB;   /* the params' two pages */
    uint32_t at = rupp + VIBEOS_RUPP_SIZE;
    uint32_t path_at, cmd_at, i;
    long n;

    b.pages = pages;
    b.base = base;

    /* The strings first: they are what can fail to fit. Each ends with a zero
     * the length does not count, as Windows' do. */
    path_at = at;
    n = put_utf16(&b, &at, strings_end - 2u, s->image_path ? s->image_path : "");
    if (n < 0) {
        return -1;
    }
    put16(&b, at, 0);
    at += 2u;
    put_ustr(&b, rupp + VIBEOS_RUPP_IMAGE_PATH, path_at, (uint32_t)n);

    /* The command line: the arguments joined by spaces, each with spaces in it
     * quoted - what a C runtime splits back into argv. Quotes inside an
     * argument are W6's, with the rest of Windows' quoting rules. */
    cmd_at = at;
    for (i = 0; s->argv && s->argv[i]; i++) {
        int q = needs_quotes(s->argv[i]);

        if ((i > 0u && put_utf16(&b, &at, strings_end - 2u, " ") < 0) ||
            (q && put_utf16(&b, &at, strings_end - 2u, "\"") < 0) ||
            put_utf16(&b, &at, strings_end - 2u, s->argv[i]) < 0 ||
            (q && put_utf16(&b, &at, strings_end - 2u, "\"") < 0)) {
            return -1;
        }
    }
    put_ustr(&b, rupp + VIBEOS_RUPP_COMMAND_LINE, cmd_at, at - cmd_at);
    put16(&b, at, 0);
    at += 2u;

    /* RTL_USER_PROCESS_PARAMETERS: normalised - its pointers are addresses, not
     * offsets from the block - with the standard handles. */
    put32(&b, rupp + VIBEOS_RUPP_MAXIMUM_LENGTH, at - rupp);
    put32(&b, rupp + VIBEOS_RUPP_LENGTH, at - rupp);
    put32(&b, rupp + VIBEOS_RUPP_FLAGS, VIBEOS_RUPP_FLAG_NORMALIZED);
    put64(&b, rupp + VIBEOS_RUPP_STD_INPUT, s->std_handle[0]);
    put64(&b, rupp + VIBEOS_RUPP_STD_OUTPUT, s->std_handle[1]);
    put64(&b, rupp + VIBEOS_RUPP_STD_ERROR, s->std_handle[2]);

    /* The PEB. The loader's list (Ldr) and the heap are our ntdll's to set up
     * as it grows; the version is Windows 10's, which is what Wine 9.0
     * reports by default and what its tests expect. */
    put64(&b, peb + VIBEOS_PEB_IMAGE_BASE, s->image_base);
    put64(&b, peb + VIBEOS_PEB_PROCESS_PARAMS, base + rupp);
    put32(&b, peb + VIBEOS_PEB_NUMBER_OF_PROCESSORS, s->processors);
    put32(&b, peb + VIBEOS_PEB_OS_MAJOR, 10u);
    put32(&b, peb + VIBEOS_PEB_OS_MINOR, 0u);
    put16(&b, peb + VIBEOS_PEB_OS_BUILD, 19045u);
    put32(&b, peb + VIBEOS_PEB_OS_PLATFORM, 2u);   /* VER_PLATFORM_WIN32_NT */
    /* Session 1, an interactive user's, as Wine gives every process: the
     * session is where a program's named objects live
     * (\Sessions\1\BaseNamedObjects), and Wine's tests build that path from
     * this field. Session 0 is the services'. */
    put32(&b, peb + VIBEOS_PEB_SESSION_ID, 1u);

    /* The TEB: its own address at 0x30, which is what gs:[0x30] reads, the
     * stack it runs on, who it is, and the PEB. */
    put64(&b, teb + VIBEOS_TEB_EXCEPTION_LIST, ~0ull);   /* no handler: -1 */
    put64(&b, teb + VIBEOS_TEB_STACK_BASE, s->stack_base);
    put64(&b, teb + VIBEOS_TEB_STACK_LIMIT, s->stack_limit);
    put64(&b, teb + VIBEOS_TEB_SELF, base + teb);
    put64(&b, teb + VIBEOS_TEB_CLIENT_PROCESS, s->pid);
    put64(&b, teb + VIBEOS_TEB_CLIENT_THREAD, s->tid);
    put64(&b, teb + VIBEOS_TEB_PEB, base + peb);
    return 0;
}
