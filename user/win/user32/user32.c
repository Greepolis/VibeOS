/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Our user32.dll (docs/abi/windows.md W8 step 5).
 *
 * The part of user32 that needs no window system: wsprintf, the character
 * case functions, and the handles a program asks for before it draws -
 * the desktop window, the process's window station, the screen's device
 * context. There is no window manager under this, so a call that would make a
 * window, a class or a menu fails with ERROR_CALL_NOT_IMPLEMENTED rather than
 * being bound to a stub that ends the process: a program can tell "no windows
 * here" apart from a crash. Exports are named in user32.def. */

#include "vibeos_ntdll.h"

typedef int32_t BOOL32;

#define ERROR_INVALID_PARAMETER    87u
#define ERROR_CALL_NOT_IMPLEMENTED 120u
#define ERROR_INVALID_WINDOW_HANDLE 1400u

/* The handles there is one of. Windows' values are not documented and
 * programs compare them only with each other and with zero. */
#define DESKTOP_WINDOW  0x10010u
#define WINDOW_STATION  0x20u
#define DESKTOP_HANDLE  0x24u

#define WSPRINTF_MAX 1024u   /* Windows writes at most this many characters */

static void set_error(uint32_t e) {
    __asm__ __volatile__("movl %0, %%gs:%c1" : : "r"(e), "i"(VIBEOS_TEB_LAST_ERROR) : "memory");
}

/* ---- wsprintf ---------------------------------------------------------------------------- */

/* One formatter for both widths: the output is collected as UTF-16 and narrowed
 * at the end for the ANSI form, through the code page ntdll uses. */
typedef struct {
    uint16_t buf[WSPRINTF_MAX];
    uint32_t n;
} wsp_out_t;

static void put(wsp_out_t *o, uint16_t c) {
    if (o->n < WSPRINTF_MAX - 1u) {
        o->buf[o->n++] = c;
    }
}

static void pad(wsp_out_t *o, uint16_t c, int32_t count) {
    while (count-- > 0) {
        put(o, c);
    }
}

static uint16_t widen_char(char c) {
    uint16_t w = 0;
    uint32_t got = 0;

    (void)RtlMultiByteToUnicodeN(&w, 2u, &got, &c, 1u);
    return got ? w : (uint16_t)(uint8_t)c;
}

static void format(wsp_out_t *o, const void *fmt_v, int wide, __builtin_va_list ap) {
    const char *fa = (const char *)fmt_v;
    const uint16_t *fw = (const uint16_t *)fmt_v;
    uint32_t i = 0;

#define FC(k) (wide ? fw[(k)] : (uint16_t)(uint8_t)fa[(k)])
    o->n = 0;
    while (FC(i)) {
        int left = 0, zero = 0, alt = 0, prec = -1, size = 0;   /* size: 0 int, 1 short, 2 long, 3 64 */
        int32_t width = 0;
        uint16_t t;

        if (FC(i) != '%') {
            put(o, wide ? fw[i] : widen_char(fa[i]));
            i++;
            continue;
        }
        i++;
        for (;;) {
            if (FC(i) == '-') {
                left = 1;
            } else if (FC(i) == '0') {
                zero = 1;
            } else if (FC(i) == '#') {
                alt = 1;
            } else {
                break;
            }
            i++;
        }
        while (FC(i) >= '0' && FC(i) <= '9') {
            width = width * 10 + (int32_t)(FC(i) - '0');
            i++;
        }
        if (FC(i) == '.') {
            i++;
            prec = 0;
            while (FC(i) >= '0' && FC(i) <= '9') {
                prec = prec * 10 + (int)(FC(i) - '0');
                i++;
            }
        }
        if (FC(i) == 'h') {
            size = 1;
            i++;
        } else if (FC(i) == 'l') {
            size = 2;
            i++;
        } else if (FC(i) == 'w') {
            size = 2;   /* wide, for c and s */
            i++;
        } else if (FC(i) == 'I' && FC(i + 1u) == '6' && FC(i + 2u) == '4') {
            size = 3;
            i += 3u;
        } else if (FC(i) == 'I' && FC(i + 1u) == '3' && FC(i + 2u) == '2') {
            i += 3u;
        } else if (FC(i) == 'I') {
            size = 3;   /* pointer-sized, which is 64 bits here */
            i++;
        }
        t = FC(i);
        if (t == 0) {
            break;
        }
        i++;
        if (t == 'c' || t == 'C') {
            /* %c is the caller's width, %C the other one; h and l force narrow
             * and wide. */
            int as_wide = (t == 'c') ? wide : !wide;
            uint32_t v = __builtin_va_arg(ap, uint32_t);
            uint16_t c;

            if (size == 1) {
                as_wide = 0;
            } else if (size == 2) {
                as_wide = 1;
            }
            c = as_wide ? (uint16_t)v : widen_char((char)v);
            if (!left) {
                pad(o, zero ? '0' : ' ', width - 1);
            }
            put(o, c);
            if (left) {
                pad(o, ' ', width - 1);
            }
        } else if (t == 's' || t == 'S') {
            int as_wide = (t == 's') ? wide : !wide;
            const void *s = __builtin_va_arg(ap, const void *);
            int32_t len = 0;

            if (size == 1) {
                as_wide = 0;
            } else if (size == 2) {
                as_wide = 1;
            }
            if (!s) {
                s = as_wide ? (const void *)u"(null)" : (const void *)"(null)";
            }
            while ((as_wide ? ((const uint16_t *)s)[len] : (uint16_t)((const char *)s)[len]) &&
                   (prec < 0 || len < prec)) {
                len++;
            }
            if (!left) {
                pad(o, zero ? '0' : ' ', width - len);
            }
            for (int32_t k = 0; k < len; k++) {
                put(o, as_wide ? ((const uint16_t *)s)[k] : widen_char(((const char *)s)[k]));
            }
            if (left) {
                pad(o, ' ', width - len);
            }
        } else if (t == 'd' || t == 'i' || t == 'u' || t == 'x' || t == 'X' || t == 'p') {
            uint64_t v;
            int neg = 0;
            char digits[24];
            int nd = 0;
            const char *set = (t == 'X' || t == 'p') ? "0123456789ABCDEF" : "0123456789abcdef";
            uint32_t base = (t == 'x' || t == 'X' || t == 'p') ? 16u : 10u;
            int32_t body;
            int prefix;

            if (t == 'p') {
                v = (uint64_t)(uintptr_t)__builtin_va_arg(ap, void *);
                prec = 16;
            } else if (size == 3) {
                v = __builtin_va_arg(ap, uint64_t);
            } else {
                uint32_t w32 = __builtin_va_arg(ap, uint32_t);

                if (size == 1) {
                    w32 = (t == 'd' || t == 'i') ? (uint32_t)(int32_t)(int16_t)w32 : (w32 & 0xFFFFu);
                }
                v = (t == 'd' || t == 'i') ? (uint64_t)(int64_t)(int32_t)w32 : (uint64_t)w32;
            }
            if ((t == 'd' || t == 'i') && (int64_t)v < 0) {
                neg = 1;
                v = (uint64_t)(-(int64_t)v);
            }
            do {
                digits[nd++] = set[v % base];
                v /= base;
            } while (v != 0u);
            if (prec == 0 && nd == 1 && digits[0] == '0') {
                nd = 0;   /* ".0" of zero prints nothing, as C's printf */
            }
            prefix = (alt && base == 16u && t != 'p') ? 2 : 0;
            body = (nd > prec ? nd : prec) + neg + prefix;
            if (!left && !(zero && prec < 0)) {
                pad(o, ' ', width - body);
            }
            if (neg) {
                put(o, '-');
            }
            if (prefix) {
                put(o, '0');
                put(o, (uint16_t)t);
            }
            if (!left && zero && prec < 0) {
                pad(o, '0', width - body);
            }
            pad(o, '0', prec - nd);
            while (nd > 0) {
                put(o, (uint16_t)(uint8_t)digits[--nd]);
            }
            if (left) {
                pad(o, ' ', width - body);
            }
        } else {
            put(o, t);   /* %% and anything unknown: the character itself */
        }
    }
#undef FC
    o->buf[o->n] = 0;
}

int32_t wvsprintfW(uint16_t *out, const uint16_t *fmt, __builtin_va_list ap) {
    static wsp_out_t o;   /* 2 KiB: kept off a thread's first stack page */
    uint32_t k;

    if (!out || !fmt) {
        return -1;
    }
    format(&o, fmt, 1, ap);
    for (k = 0; k <= o.n; k++) {
        out[k] = o.buf[k];
    }
    return (int32_t)o.n;
}

int32_t wvsprintfA(char *out, const char *fmt, __builtin_va_list ap) {
    static wsp_out_t o;
    uint32_t k;

    if (!out || !fmt) {
        return -1;
    }
    format(&o, fmt, 0, ap);
    /* Back to Windows-1252: Latin-1 is the same bytes; anything else that
     * code page cannot spell is '?', as WideCharToMultiByte's default. */
    for (k = 0; k < o.n; k++) {
        uint16_t c = o.buf[k];

        out[k] = (char)(c < 0x100u ? c : (uint16_t)'?');
    }
    out[o.n] = 0;
    return (int32_t)o.n;
}

int32_t wsprintfW(uint16_t *out, const uint16_t *fmt, ...) {
    __builtin_va_list ap;
    int32_t r;

    __builtin_va_start(ap, fmt);
    r = wvsprintfW(out, fmt, ap);
    __builtin_va_end(ap);
    return r;
}

int32_t wsprintfA(char *out, const char *fmt, ...) {
    __builtin_va_list ap;
    int32_t r;

    __builtin_va_start(ap, fmt);
    r = wvsprintfA(out, fmt, ap);
    __builtin_va_end(ap);
    return r;
}

/* ---- character case ---------------------------------------------------------------------- */

/* Windows-1252 and the Latin-1 block of UTF-16: the two letters outside the
 * ASCII rules are the ones a code page puts above 0xC0. */
static uint16_t up_w(uint16_t c) {
    if ((c >= 'a' && c <= 'z') || (c >= 0xE0u && c <= 0xFEu && c != 0xF7u)) {
        return (uint16_t)(c - 0x20u);
    }
    return c == 0xFFu ? 0x178u : c;
}

static uint16_t low_w(uint16_t c) {
    if ((c >= 'A' && c <= 'Z') || (c >= 0xC0u && c <= 0xDEu && c != 0xD7u)) {
        return (uint16_t)(c + 0x20u);
    }
    return c == 0x178u ? 0xFFu : c;
}

static uint8_t up_a(uint8_t c) {
    return (c >= 'a' && c <= 'z') || (c >= 0xE0u && c <= 0xFEu && c != 0xF7u) ? (uint8_t)(c - 0x20u) : c;
}

static uint8_t low_a(uint8_t c) {
    return (c >= 'A' && c <= 'Z') || (c >= 0xC0u && c <= 0xDEu && c != 0xD7u) ? (uint8_t)(c + 0x20u) : c;
}

/* A pointer below 64 KiB is a character, not a string: the call converts the
 * one in the low word and gives it back the same way. */
#define IS_CHAR(p) ((uintptr_t)(p) < 0x10000u)

char *CharUpperA(char *s) {
    if (IS_CHAR(s)) {
        return (char *)(uintptr_t)up_a((uint8_t)(uintptr_t)s);
    }
    for (char *p = s; *p; p++) {
        *p = (char)up_a((uint8_t)*p);
    }
    return s;
}

char *CharLowerA(char *s) {
    if (IS_CHAR(s)) {
        return (char *)(uintptr_t)low_a((uint8_t)(uintptr_t)s);
    }
    for (char *p = s; *p; p++) {
        *p = (char)low_a((uint8_t)*p);
    }
    return s;
}

uint16_t *CharUpperW(uint16_t *s) {
    if (IS_CHAR(s)) {
        return (uint16_t *)(uintptr_t)up_w((uint16_t)(uintptr_t)s);
    }
    for (uint16_t *p = s; *p; p++) {
        *p = up_w(*p);
    }
    return s;
}

uint16_t *CharLowerW(uint16_t *s) {
    if (IS_CHAR(s)) {
        return (uint16_t *)(uintptr_t)low_w((uint16_t)(uintptr_t)s);
    }
    for (uint16_t *p = s; *p; p++) {
        *p = low_w(*p);
    }
    return s;
}

uint32_t CharUpperBuffA(char *s, uint32_t n) {
    for (uint32_t i = 0; s && i < n; i++) {
        s[i] = (char)up_a((uint8_t)s[i]);
    }
    return s ? n : 0u;
}

uint32_t CharLowerBuffA(char *s, uint32_t n) {
    for (uint32_t i = 0; s && i < n; i++) {
        s[i] = (char)low_a((uint8_t)s[i]);
    }
    return s ? n : 0u;
}

uint32_t CharUpperBuffW(uint16_t *s, uint32_t n) {
    for (uint32_t i = 0; s && i < n; i++) {
        s[i] = up_w(s[i]);
    }
    return s ? n : 0u;
}

uint32_t CharLowerBuffW(uint16_t *s, uint32_t n) {
    for (uint32_t i = 0; s && i < n; i++) {
        s[i] = low_w(s[i]);
    }
    return s ? n : 0u;
}

BOOL32 IsCharAlphaA(char c) { return low_a((uint8_t)c) != up_a((uint8_t)c); }
BOOL32 IsCharAlphaW(uint16_t c) { return low_w(c) != up_w(c); }
BOOL32 IsCharUpperA(char c) { return IsCharAlphaA(c) && up_a((uint8_t)c) == (uint8_t)c; }
BOOL32 IsCharUpperW(uint16_t c) { return IsCharAlphaW(c) && up_w(c) == c; }
BOOL32 IsCharLowerA(char c) { return IsCharAlphaA(c) && low_a((uint8_t)c) == (uint8_t)c; }
BOOL32 IsCharLowerW(uint16_t c) { return IsCharAlphaW(c) && low_w(c) == c; }

/* ---- the handles there is one of --------------------------------------------------------- */

uint64_t GetDesktopWindow(void) { return DESKTOP_WINDOW; }
uint64_t GetProcessWindowStation(void) { return WINDOW_STATION; }
uint64_t GetThreadDesktop(uint32_t tid) {
    (void)tid;
    return DESKTOP_HANDLE;
}

/* The screen is gdi32's display DC; a window's DC is the screen's, since the
 * only window is the desktop. */
extern uint64_t CreateDCA(const char *driver, const char *device, const char *port, const void *mode);
extern BOOL32 DeleteDC(uint64_t dc);

uint64_t GetDC(uint64_t wnd) {
    if (wnd != 0u && wnd != DESKTOP_WINDOW) {
        set_error(ERROR_INVALID_WINDOW_HANDLE);
        return 0;
    }
    return CreateDCA("DISPLAY", 0, 0, 0);
}

uint64_t GetWindowDC(uint64_t wnd) { return GetDC(wnd); }

int32_t ReleaseDC(uint64_t wnd, uint64_t dc) {
    (void)wnd;
    return DeleteDC(dc) ? 1 : 0;
}

/* Screen metrics of the one display gdi32 describes. */
#define SM_CXSCREEN 0
#define SM_CYSCREEN 1
#define SM_CMONITORS 80

int32_t GetSystemMetrics(int32_t index) {
    switch (index) {
    case SM_CXSCREEN:
        return 1024;
    case SM_CYSCREEN:
        return 768;
    case SM_CMONITORS:
        return 1;
    default:
        return 0;
    }
}

/* ---- what needs a window system ---------------------------------------------------------- */

/* Each answers failure with ERROR_CALL_NOT_IMPLEMENTED: there is no window
 * manager to register a class with or to put a window on. */
static uint64_t no_windows(void) {
    set_error(ERROR_CALL_NOT_IMPLEMENTED);
    return 0;
}

uint16_t RegisterClassA(const void *wc) { (void)wc; return (uint16_t)no_windows(); }
uint16_t RegisterClassW(const void *wc) { (void)wc; return (uint16_t)no_windows(); }
uint16_t RegisterClassExA(const void *wc) { (void)wc; return (uint16_t)no_windows(); }
uint16_t RegisterClassExW(const void *wc) { (void)wc; return (uint16_t)no_windows(); }

uint64_t CreateWindowExA(uint32_t ex, const char *cls, const char *name, uint32_t style, int32_t x,
                         int32_t y, int32_t w, int32_t h, uint64_t parent, uint64_t menu,
                         uint64_t inst, void *param) {
    (void)ex, (void)cls, (void)name, (void)style, (void)x, (void)y, (void)w, (void)h;
    (void)parent, (void)menu, (void)inst, (void)param;
    return no_windows();
}

uint64_t CreateWindowExW(uint32_t ex, const uint16_t *cls, const uint16_t *name, uint32_t style,
                         int32_t x, int32_t y, int32_t w, int32_t h, uint64_t parent, uint64_t menu,
                         uint64_t inst, void *param) {
    (void)ex, (void)cls, (void)name, (void)style, (void)x, (void)y, (void)w, (void)h;
    (void)parent, (void)menu, (void)inst, (void)param;
    return no_windows();
}

BOOL32 DestroyWindow(uint64_t wnd) {
    (void)wnd;
    set_error(ERROR_INVALID_WINDOW_HANDLE);
    return 0;
}

BOOL32 IsWindow(uint64_t wnd) { return wnd == DESKTOP_WINDOW; }

uint32_t RegisterClipboardFormatA(const char *name) { (void)name; return (uint32_t)no_windows(); }
uint32_t RegisterClipboardFormatW(const uint16_t *name) { (void)name; return (uint32_t)no_windows(); }

/* Resources need the image's resource directory, which nothing reads yet
 * (WO register); a missing string is what Windows answers too. */
int32_t LoadStringA(uint64_t inst, uint32_t id, char *buf, int32_t size) {
    (void)inst, (void)id;
    if (buf && size > 0) {
        buf[0] = 0;
    }
    set_error(ERROR_CALL_NOT_IMPLEMENTED);
    return 0;
}

int32_t LoadStringW(uint64_t inst, uint32_t id, uint16_t *buf, int32_t size) {
    (void)inst, (void)id;
    if (buf && size > 0) {
        buf[0] = 0;
    }
    set_error(ERROR_CALL_NOT_IMPLEMENTED);
    return 0;
}

uint64_t LoadCursorA(uint64_t inst, const char *name) { (void)inst, (void)name; return no_windows(); }
uint64_t LoadCursorW(uint64_t inst, const uint16_t *name) { (void)inst, (void)name; return no_windows(); }
uint64_t LoadIconA(uint64_t inst, const char *name) { (void)inst, (void)name; return no_windows(); }
uint64_t LoadIconW(uint64_t inst, const uint16_t *name) { (void)inst, (void)name; return no_windows(); }

BOOL32 GetCursorPos(void *pt) {
    if (!pt) {
        set_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    ((int32_t *)pt)[0] = 0;
    ((int32_t *)pt)[1] = 0;
    return 1;
}

int vibeos_user32_main(uint64_t instance, uint32_t reason, void *reserved) {
    (void)instance;
    (void)reason;
    (void)reserved;
    return 1;
}
