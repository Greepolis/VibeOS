/* SPDX-License-Identifier: GPL-3.0-or-later */
/* What Wine's cmd.exe asks of kernel32 beyond what the conformance tests did
 * (docs/shell/s6.md step 4): the console, the environment block, dates and a few
 * file operations.
 *
 * The console here is a terminal that speaks UTF-8 and ANSI sequences, not a
 * Windows screen buffer, so these calls translate to it. There is no time zone
 * yet (local time is UTC) and one locale, en-US. What cannot be done yet fails
 * with a Windows error instead of pretending: links, disk space, volume names. */

#pragma GCC optimize("no-tree-loop-distribute-patterns")

#include "vibeos_ntdll.h"

typedef int32_t BOOL32;

typedef struct {
    uint16_t year, month, weekday, day, hour, minute, second, ms;
} systime_t;

#define ERR_FILE_NOT_FOUND 2u
#define ERR_INVALID_HANDLE 6u
#define ERR_NOT_SUPPORTED 50u
#define ERR_INVALID_PARAMETER 87u
#define ERR_CALL_NOT_IMPLEMENTED 120u
#define ERR_INSUFFICIENT_BUFFER 122u
#define ERR_HANDLE_EOF 38u
#define INVALID_H ((uint64_t)-1)

void SetLastError(uint32_t e);
uint32_t GetFileType(uint64_t h);
uint64_t GetStdHandle(uint32_t which);
BOOL32 WriteFile(uint64_t h, const void *buf, uint32_t len, uint32_t *written, void *overlapped);
BOOL32 ReadFile(uint64_t h, void *buf, uint32_t len, uint32_t *got, void *overlapped);
BOOL32 CloseHandle(uint64_t h);
uint64_t CreateFileW(const uint16_t *name, uint32_t access, uint32_t share, const void *sa,
                     uint32_t disposition, uint32_t flags, uint64_t template_file);
BOOL32 MoveFileW(const uint16_t *from, const uint16_t *to);
BOOL32 DeleteFileW(const uint16_t *name);
uint32_t GetFileAttributesW(const uint16_t *name);
uint32_t GetFullPathNameW(const uint16_t *name, uint32_t size, uint16_t *buf, uint16_t **file_part);
uint32_t GetEnvironmentVariableW(const uint16_t *name, uint16_t *buf, uint32_t size);
uint32_t GetSystemDirectoryW(uint16_t *buf, uint32_t size);
BOOL32 FileTimeToSystemTime(const int64_t *ft, systime_t *st);
uint32_t GetACP(void);

static uint32_t wl(const uint16_t *s) {
    uint32_t n = 0;

    while (s[n]) {
        n++;
    }
    return n;
}

static uint64_t params(void) {
    uint64_t peb = vibeos_nt_rd64(vibeos_nt_teb() + VIBEOS_TEB_PEB);

    return vibeos_nt_rd64(peb + VIBEOS_PEB_PROCESS_PARAMS);
}

static void *heap(void) {
    uint64_t peb = vibeos_nt_rd64(vibeos_nt_teb() + VIBEOS_TEB_PEB);

    return (void *)(uintptr_t)vibeos_nt_rd64(peb + VIBEOS_PEB_PROCESS_HEAP);
}

/* ---- strings ------------------------------------------------------------------ */

uint16_t *lstrcatW(uint16_t *d, const uint16_t *s) {
    uint32_t n = wl(d), i = 0;

    do {
        d[n + i] = s[i];
    } while (s[i++]);
    return d;
}

/* At most n units, the terminator included. */
uint16_t *lstrcpynW(uint16_t *d, const uint16_t *s, int n) {
    int i;

    if (n <= 0) {
        return d;
    }
    for (i = 0; i + 1 < n && s[i]; i++) {
        d[i] = s[i];
    }
    d[i] = 0;
    return d;
}

static uint16_t fold(uint16_t c) {
    return (c >= 'a' && c <= 'z') ? (uint16_t)(c - 32) : c;
}

/* CSTR_LESS_THAN 1, CSTR_EQUAL 2, CSTR_GREATER_THAN 3. By code unit: cmd compares
 * keywords and names for equality, which one locale cannot get wrong. */
int CompareStringW(uint32_t lcid, uint32_t flags, const uint16_t *a, int la, const uint16_t *b, int lb) {
    uint32_t na, nb, i;

    (void)lcid;
    if (!a || !b) {
        SetLastError(ERR_INVALID_PARAMETER);
        return 0;
    }
    na = la < 0 ? wl(a) : (uint32_t)la;
    nb = lb < 0 ? wl(b) : (uint32_t)lb;
    for (i = 0; i < na && i < nb; i++) {
        uint16_t x = (flags & 1u) ? fold(a[i]) : a[i], y = (flags & 1u) ? fold(b[i]) : b[i];

        if (x != y) {
            return x < y ? 1 : 3;
        }
    }
    return na == nb ? 2 : (na < nb ? 1 : 3);
}

/* ---- the environment and the process's start ------------------------------------ */

uint16_t *GetEnvironmentStringsW(void) {
    const uint16_t *e = (const uint16_t *)(uintptr_t)vibeos_nt_rd64(params() + VIBEOS_RUPP_ENVIRONMENT);
    size_t n = 0, i;
    uint16_t *copy;

    if (e) {
        while (e[n]) {
            n += wl(e + n) + 1u;
        }
    }
    copy = RtlAllocateHeap(heap(), 0, (n + 2u) * 2u);
    if (!copy) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        copy[i] = e[i];
    }
    copy[n] = 0;
    copy[n + 1u] = 0;
    return copy;
}

BOOL32 FreeEnvironmentStringsW(uint16_t *block) {
    return RtlFreeHeap(heap(), 0, block) ? 1 : 0;
}

/* The count includes the terminator, and is what is needed when it did not fit.
 * %NAME% with no such variable stays as written. */
uint32_t ExpandEnvironmentStringsW(const uint16_t *src, uint16_t *dst, uint32_t size) {
    uint32_t at = 0, i = 0;

    while (src[i]) {
        uint32_t k = i + 1u, got = 0;

        if (src[i] == '%') {
            while (src[k] && src[k] != '%') {
                k++;
            }
            if (src[k] == '%' && k > i + 1u && k - i < 256u) {
                uint16_t name[256];
                /* On the heap: this DLL has no stack probe for a frame past a page. */
                uint16_t *value = RtlAllocateHeap(heap(), 0, 4096u * 2u);
                uint32_t j;

                for (j = 0; j < k - i - 1u; j++) {
                    name[j] = src[i + 1u + j];
                }
                name[j] = 0;
                got = value ? GetEnvironmentVariableW(name, value, 4096u) : 0u;
                if (got > 0u && got < 4096u) {
                    for (j = 0; j < got; j++) {
                        if (at + j < size) {
                            dst[at + j] = value[j];
                        }
                    }
                    (void)RtlFreeHeap(heap(), 0, value);
                    at += got;
                    i = k + 1u;
                    continue;
                }
                if (value) {
                    (void)RtlFreeHeap(heap(), 0, value);
                }
            }
        }
        if (at < size) {
            dst[at] = src[i];
        }
        at++;
        i++;
    }
    if (at < size) {
        dst[at] = 0;
    } else if (size > 0u) {
        dst[size - 1u] = 0;
    }
    return at + 1u;
}

/* STARTUPINFOW is 104 bytes, its three handles the last 24. */
void GetStartupInfoW(void *info) {
    uint8_t *p = info;
    uint32_t i;

    for (i = 0; i < 104u; i++) {
        p[i] = 0;
    }
    *(uint32_t *)p = 104u;
    *(uint64_t *)(p + 80) = GetStdHandle((uint32_t)-10);
    *(uint64_t *)(p + 88) = GetStdHandle((uint32_t)-11);
    *(uint64_t *)(p + 96) = GetStdHandle((uint32_t)-12);
}

BOOL32 SetStdHandle(uint32_t which, uint64_t h) {
    uint32_t at = which == (uint32_t)-10 ? VIBEOS_RUPP_STD_INPUT
                : which == (uint32_t)-11 ? VIBEOS_RUPP_STD_OUTPUT
                : which == (uint32_t)-12 ? VIBEOS_RUPP_STD_ERROR : 0u;

    if (!at) {
        SetLastError(ERR_INVALID_HANDLE);
        return 0;
    }
    *(volatile uint64_t *)(uintptr_t)(params() + at) = h;
    return 1;
}

/* ---- the console ------------------------------------------------------------------ */

static int is_console(uint64_t h) {
    if (h != INVALID_H && h != 0u && GetFileType(h) == 2u) {
        return 1;
    }
    SetLastError(ERR_INVALID_HANDLE);
    return 0;
}

uint32_t GetConsoleCP(void) { return GetACP(); }
uint32_t GetConsoleOutputCP(void) { return GetACP(); }

/* ENABLE_PROCESSED_INPUT | LINE_INPUT | ECHO_INPUT: what a terminal in its
 * canonical mode does. A change of mode is accepted and not applied yet. */
BOOL32 GetConsoleMode(uint64_t h, uint32_t *mode) {
    if (!is_console(h)) {
        return 0;
    }
    *mode = 7u;
    return 1;
}

BOOL32 SetConsoleMode(uint64_t h, uint32_t mode) {
    (void)mode;
    return is_console(h);
}

BOOL32 SetConsoleCtrlHandler(void *handler, BOOL32 add) {
    (void)handler;
    (void)add;
    return 1;
}

BOOL32 SetConsoleTitleW(const uint16_t *title) {
    (void)title;
    return 1;
}

BOOL32 SetConsoleTextAttribute(uint64_t h, uint16_t attr) {
    (void)attr;
    return is_console(h);
}

/* CONSOLE_SCREEN_BUFFER_INFO, 22 bytes of 16-bit fields. The size is the one a
 * terminal is assumed to have until the call can ask it. */
BOOL32 GetConsoleScreenBufferInfo(uint64_t h, void *info) {
    static const uint16_t v[11] = {80, 25, 0, 0, 7, 0, 0, 79, 24, 80, 25};
    uint16_t *o = info;
    uint32_t i;

    if (!is_console(h)) {
        return 0;
    }
    for (i = 0; i < 11u; i++) {
        o[i] = v[i];
    }
    return 1;
}

static void put_num(char *b, uint32_t *n, uint32_t v) {
    char t[12];
    uint32_t k = 0;

    do {
        t[k++] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v);
    while (k) {
        b[(*n)++] = t[--k];
    }
}

/* COORD arrives by value: X in the low 16 bits. */
BOOL32 SetConsoleCursorPosition(uint64_t h, uint32_t pos) {
    char b[32];
    uint32_t n = 0, put = 0;

    if (!is_console(h)) {
        return 0;
    }
    b[n++] = 27;
    b[n++] = '[';
    put_num(b, &n, (pos >> 16) + 1u);
    b[n++] = ';';
    put_num(b, &n, (pos & 0xFFFFu) + 1u);
    b[n++] = 'H';
    return WriteFile(h, b, n, &put, 0);
}

/* cmd's CLS fills the whole buffer with spaces from the origin. */
BOOL32 FillConsoleOutputCharacterW(uint64_t h, uint16_t c, uint32_t count, uint32_t pos, uint32_t *done) {
    uint32_t put = 0;

    if (!is_console(h)) {
        return 0;
    }
    if (c == ' ' && pos == 0u && !WriteFile(h, "\x1b[2J", 4u, &put, 0)) {
        return 0;
    }
    *done = count;
    return 1;
}

BOOL32 FillConsoleOutputAttribute(uint64_t h, uint16_t attr, uint32_t count, uint32_t pos, uint32_t *done) {
    (void)attr;
    (void)pos;
    if (!is_console(h)) {
        return 0;
    }
    *done = count;
    return 1;
}

BOOL32 WriteConsoleW(uint64_t h, const uint16_t *text, uint32_t len, uint32_t *written, void *reserved) {
    char b[512];
    uint32_t i = 0, n = 0, put;

    (void)reserved;
    if (!is_console(h)) {
        return 0;
    }
    while (i < len) {
        uint32_t c = text[i++];

        if (c >= 0xD800u && c < 0xDC00u && i < len && text[i] >= 0xDC00u && text[i] < 0xE000u) {
            c = 0x10000u + ((c - 0xD800u) << 10) + (text[i++] - 0xDC00u);
        } else if (c >= 0xD800u && c < 0xE000u) {
            c = 0xFFFDu;
        }
        if (c < 0x80u) {
            b[n++] = (char)c;
        } else if (c < 0x800u) {
            b[n++] = (char)(0xC0u | (c >> 6));
            b[n++] = (char)(0x80u | (c & 0x3Fu));
        } else if (c < 0x10000u) {
            b[n++] = (char)(0xE0u | (c >> 12));
            b[n++] = (char)(0x80u | ((c >> 6) & 0x3Fu));
            b[n++] = (char)(0x80u | (c & 0x3Fu));
        } else {
            b[n++] = (char)(0xF0u | (c >> 18));
            b[n++] = (char)(0x80u | ((c >> 12) & 0x3Fu));
            b[n++] = (char)(0x80u | ((c >> 6) & 0x3Fu));
            b[n++] = (char)(0x80u | (c & 0x3Fu));
        }
        if (n + 4u > sizeof b) {
            if (!WriteFile(h, b, n, &put, 0)) {
                return 0;
            }
            n = 0;
        }
    }
    if (n && !WriteFile(h, b, n, &put, 0)) {
        return 0;
    }
    if (written) {
        *written = len;
    }
    return 1;
}

/* A line from the terminal, as UTF-16 ending in CR LF as Windows' line input
 * does. Room is kept for the CR that a bare LF gains. */
BOOL32 ReadConsoleW(uint64_t h, uint16_t *buf, uint32_t max, uint32_t *got, void *control) {
    uint8_t b[512];
    uint32_t n = 0, i = 0, k = 0, want;

    (void)control;
    if (!is_console(h)) {
        return 0;
    }
    if (max < 2u) {
        SetLastError(ERR_INVALID_PARAMETER);
        return 0;
    }
    want = (max - 1u) / 2u + 1u;
    want = want > sizeof b ? (uint32_t)sizeof b : want;
    if (!ReadFile(h, b, want, &n, 0)) {
        return 0;
    }
    if (n == 0u) {
        SetLastError(ERR_HANDLE_EOF);
        return 0;
    }
    while (i < n && k + 2u < max) {
        uint32_t c = b[i++], more = c >= 0xF0u ? 3u : c >= 0xE0u ? 2u : c >= 0xC0u ? 1u : 0u, j;

        if (c >= 0x80u && more == 0u) {
            c = 0xFFFDu;
        } else if (more) {
            c &= 0x3Fu >> more;
            for (j = 0; j < more; j++) {
                if (i >= n || (b[i] & 0xC0u) != 0x80u) {
                    c = 0xFFFDu;
                    break;
                }
                c = (c << 6) | (b[i++] & 0x3Fu);
            }
        }
        if (c == '\n' && (k == 0u || buf[k - 1u] != '\r')) {
            buf[k++] = '\r';
        }
        if (c >= 0x10000u && c <= 0x10FFFFu) {
            c -= 0x10000u;
            buf[k++] = (uint16_t)(0xD800u + (c >> 10));
            buf[k++] = (uint16_t)(0xDC00u + (c & 0x3FFu));
        } else {
            buf[k++] = (uint16_t)c;
        }
    }
    *got = k;
    return 1;
}

/* ---- dates and times -------------------------------------------------------------- */

void GetLocalTime(systime_t *st) {
    int64_t now = 0;

    (void)NtQuerySystemTime(&now);
    (void)FileTimeToSystemTime(&now, st);
}

BOOL32 FileTimeToLocalFileTime(const int64_t *ft, int64_t *local) {
    *local = *ft;
    return 1;
}

static int put_text(uint16_t *o, int cap, int n, const char *s) {
    for (; *s; s++, n++) {
        if (cap && n < cap) {
            o[n] = (uint8_t)*s;
        }
    }
    return n;
}

static int put_field(uint16_t *o, int cap, int n, uint32_t v, int width) {
    char t[12];
    int k = 0;

    do {
        t[k++] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v);
    while (k < width) {
        t[k++] = '0';
    }
    while (k) {
        if (cap && n < cap) {
            o[n] = (uint8_t)t[k - 1];
        }
        n++;
        k--;
    }
    return n;
}

/* The picture strings of GetDateFormat and GetTimeFormat. Returns the count with
 * the terminator, or 0 with the error when a buffer was given and it did not fit. */
static int format_time(const systime_t *st, const uint16_t *pic, uint16_t *out, int cap) {
    static const char *const days[7] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
    static const char *const months[12] = {"January", "February", "March", "April", "May", "June", "July",
                                           "August", "September", "October", "November", "December"};
    int n = 0;

    while (*pic) {
        uint16_t c = *pic;
        int run = 0;

        if (c == '\'') {
            pic++;
            while (*pic && *pic != '\'') {
                if (cap && n < cap) {
                    out[n] = *pic;
                }
                n++;
                pic++;
            }
            if (*pic) {
                pic++;
            }
            continue;
        }
        while (pic[run] == c) {
            run++;
        }
        pic += run;
        switch (c) {
            case 'd':
                if (run >= 3) {
                    char s[4];
                    const char *f = days[st->weekday % 7u];

                    s[0] = f[0];
                    s[1] = f[1];
                    s[2] = f[2];
                    s[3] = 0;
                    n = put_text(out, cap, n, run == 3 ? s : f);
                } else {
                    n = put_field(out, cap, n, st->day, run);
                }
                break;
            case 'M':
                if (run >= 3 && st->month >= 1u && st->month <= 12u) {
                    char s[4];
                    const char *f = months[st->month - 1u];

                    s[0] = f[0];
                    s[1] = f[1];
                    s[2] = f[2];
                    s[3] = 0;
                    n = put_text(out, cap, n, run == 3 ? s : f);
                } else {
                    n = put_field(out, cap, n, st->month, run);
                }
                break;
            case 'y':
                n = run <= 2 ? put_field(out, cap, n, st->year % 100u, run) : put_field(out, cap, n, st->year, 4);
                break;
            case 'h':
                n = put_field(out, cap, n, st->hour % 12u ? st->hour % 12u : 12u, run);
                break;
            case 'H':
                n = put_field(out, cap, n, st->hour, run);
                break;
            case 'm':
                n = put_field(out, cap, n, st->minute, run);
                break;
            case 's':
                n = put_field(out, cap, n, st->second, run);
                break;
            case 't':
                n = put_text(out, cap, n, run == 1 ? (st->hour < 12u ? "A" : "P") : (st->hour < 12u ? "AM" : "PM"));
                break;
            default: {
                int k;

                for (k = 0; k < run; k++, n++) {
                    if (cap && n < cap) {
                        out[n] = c;
                    }
                }
                break;
            }
        }
    }
    if (cap && n >= cap) {
        SetLastError(ERR_INSUFFICIENT_BUFFER);
        return 0;
    }
    if (cap) {
        out[n] = 0;
    }
    return n + 1;
}

static const uint16_t g_short_date[] = {'M', '/', 'd', '/', 'y', 'y', 'y', 'y', 0};
static const uint16_t g_long_time[] = {'h', ':', 'm', 'm', ':', 's', 's', ' ', 't', 't', 0};
static const uint16_t g_short_time[] = {'h', ':', 'm', 'm', ' ', 't', 't', 0};

int GetDateFormatW(uint32_t lcid, uint32_t flags, const systime_t *st, const uint16_t *fmt, uint16_t *out, int cap) {
    systime_t now;

    (void)lcid;
    (void)flags;
    if (!st) {
        GetLocalTime(&now);
        st = &now;
    }
    return format_time(st, fmt ? fmt : g_short_date, out, cap);
}

int GetTimeFormatW(uint32_t lcid, uint32_t flags, const systime_t *st, const uint16_t *fmt, uint16_t *out, int cap) {
    systime_t now;

    (void)lcid;
    if (!st) {
        GetLocalTime(&now);
        st = &now;
    }
    return format_time(st, fmt ? fmt : ((flags & 2u) ? g_short_time : g_long_time), out, cap);
}

/* LOCALE_SSHORTDATE and LOCALE_STIMEFORMAT, the two cmd's DIR asks for. */
int GetLocaleInfoW(uint32_t lcid, uint32_t type, uint16_t *out, int cap) {
    const uint16_t *v = (type & 0xFFFFu) == 0x1Fu ? g_short_date : (type & 0xFFFFu) == 0x1003u ? g_long_time : 0;
    int n, i;

    (void)lcid;
    if (!v) {
        SetLastError(ERR_INVALID_PARAMETER);
        return 0;
    }
    n = (int)wl(v) + 1;
    if (cap == 0) {
        return n;
    }
    if (cap < n) {
        SetLastError(ERR_INSUFFICIENT_BUFFER);
        return 0;
    }
    for (i = 0; i < n; i++) {
        out[i] = v[i];
    }
    return n;
}

/* ---- files -------------------------------------------------------------------------- */

BOOL32 CopyFileW(const uint16_t *from, const uint16_t *to, BOOL32 fail_if_exists) {
    enum { CHUNK = 65536 };
    uint64_t in, out;
    uint8_t *b;
    BOOL32 ok = 0;

    in = CreateFileW(from, 0x80000000u, 1u, 0, 3u, 0x80u, 0);
    if (in == INVALID_H) {
        return 0;
    }
    out = CreateFileW(to, 0x40000000u, 0u, 0, fail_if_exists ? 1u : 2u, 0x80u, 0);
    if (out == INVALID_H) {
        (void)CloseHandle(in);
        return 0;
    }
    b = RtlAllocateHeap(heap(), 0, CHUNK);
    if (b) {
        for (;;) {
            uint32_t got = 0, put = 0;

            if (!ReadFile(in, b, CHUNK, &got, 0)) {
                break;
            }
            if (got == 0u) {
                ok = 1;
                break;
            }
            if (!WriteFile(out, b, got, &put, 0) || put != got) {
                break;
            }
        }
        (void)RtlFreeHeap(heap(), 0, b);
    }
    (void)CloseHandle(in);
    (void)CloseHandle(out);
    if (!ok) {
        (void)DeleteFileW(to);      /* half a copy is not a copy */
    }
    return ok;
}

/* MOVEFILE_REPLACE_EXISTING removes the target first: not atomic, as a rename
 * over it would be, which MoveFileW's service does not offer yet. */
BOOL32 MoveFileExW(const uint16_t *from, const uint16_t *to, uint32_t flags) {
    if (!to) {
        return DeleteFileW(from);
    }
    if ((flags & 1u) && GetFileAttributesW(from) != 0xFFFFFFFFu) {
        (void)DeleteFileW(to);
    }
    return MoveFileW(from, to);
}

BOOL32 CreateHardLinkW(const uint16_t *link, const uint16_t *target, const void *sa) {
    (void)link;
    (void)target;
    (void)sa;
    SetLastError(ERR_CALL_NOT_IMPLEMENTED);
    return 0;
}

uint8_t CreateSymbolicLinkW(const uint16_t *link, const uint16_t *target, uint32_t flags) {
    (void)link;
    (void)target;
    (void)flags;
    SetLastError(ERR_CALL_NOT_IMPLEMENTED);
    return 0;
}

static uint32_t search_one(const uint16_t *dir, uint32_t dlen, const uint16_t *file, const uint16_t *ext,
                           uint32_t size, uint16_t *buf, uint16_t **part) {
    uint16_t path[1024];
    uint32_t n = 0, i, flen = wl(file), elen = ext ? wl(ext) : 0u;

    if (dlen + flen + elen + 2u > 1024u) {
        return 0;
    }
    for (i = 0; i < dlen; i++) {
        path[n++] = dir[i];
    }
    if (n && path[n - 1u] != '\\' && path[n - 1u] != '/') {
        path[n++] = '\\';
    }
    for (i = 0; i < flen; i++) {
        path[n++] = file[i];
    }
    path[n] = 0;
    if (GetFileAttributesW(path) == 0xFFFFFFFFu) {
        int dotted = 0;

        for (i = 0; i < flen; i++) {
            dotted = file[i] == '.' ? 1 : (file[i] == '\\' || file[i] == '/') ? 0 : dotted;
        }
        if (!elen || dotted) {
            return 0;
        }
        for (i = 0; i < elen; i++) {
            path[n++] = ext[i];
        }
        path[n] = 0;
        if (GetFileAttributesW(path) == 0xFFFFFFFFu) {
            return 0;
        }
    }
    return GetFullPathNameW(path, size, buf, part);
}

/* `path` is a semicolon-separated list; none means the current directory, then
 * the system's. A name with a directory in it is looked at where it says. */
uint32_t SearchPathW(const uint16_t *path, const uint16_t *file, const uint16_t *ext, uint32_t size,
                     uint16_t *buf, uint16_t **part) {
    uint16_t sys[260];
    uint32_t i, r;

    if (!file || !file[0]) {
        SetLastError(ERR_INVALID_PARAMETER);
        return 0;
    }
    for (i = 0; file[i]; i++) {
        if (file[i] == '\\' || file[i] == '/' || file[i] == ':') {
            r = search_one(file, 0, file, ext, size, buf, part);
            if (!r) {
                SetLastError(ERR_FILE_NOT_FOUND);
            }
            return r;
        }
    }
    if (path) {
        const uint16_t *p = path;

        while (*p) {
            uint32_t n = 0;

            while (p[n] && p[n] != ';') {
                n++;
            }
            if (n && (r = search_one(p, n, file, ext, size, buf, part)) != 0u) {
                return r;
            }
            p += n + (p[n] ? 1u : 0u);
        }
    } else {
        if ((r = search_one(file, 0, file, ext, size, buf, part)) != 0u) {
            return r;
        }
        i = GetSystemDirectoryW(sys, 260u);
        if (i && i < 260u && (r = search_one(sys, i, file, ext, size, buf, part)) != 0u) {
            return r;
        }
    }
    SetLastError(ERR_FILE_NOT_FOUND);
    return 0;
}

BOOL32 GetDiskFreeSpaceExW(const uint16_t *dir, uint64_t *avail, uint64_t *total, uint64_t *free_bytes) {
    (void)dir;
    (void)avail;
    (void)total;
    (void)free_bytes;
    SetLastError(ERR_NOT_SUPPORTED);
    return 0;
}

BOOL32 GetVolumeInformationW(const uint16_t *root, uint16_t *name, uint32_t nsize, uint32_t *serial,
                             uint32_t *maxlen, uint32_t *flags, uint16_t *fs, uint32_t fsize) {
    (void)root;
    (void)name;
    (void)nsize;
    (void)serial;
    (void)maxlen;
    (void)flags;
    (void)fs;
    (void)fsize;
    SetLastError(ERR_NOT_SUPPORTED);
    return 0;
}

BOOL32 SetVolumeLabelW(const uint16_t *root, const uint16_t *label) {
    (void)root;
    (void)label;
    SetLastError(ERR_NOT_SUPPORTED);
    return 0;
}
