/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Our advapi32.dll (docs/abi/windows.md W8 step 4).
 *
 * The registry's Win32 face: Reg* over the Nt*Key services, as Windows'
 * advapi32 (through kernelbase) is. A predefined handle - HKEY_LOCAL_MACHINE
 * and its siblings - is opened on first use and kept for the life of the
 * process; RegCloseKey of one does nothing, as on Windows. Every Reg*
 * function answers a Win32 error rather than setting the last error.
 *
 * The ANSI forms convert names and string data through the code page ntdll
 * widens with (Windows-1252), as kernel32 does. Built freestanding against our
 * ntdll alone. Exports are named in advapi32.def. */

#include "vibeos_ntdll.h"

typedef int32_t BOOL32;
typedef int32_t LONG32;

#define ERROR_SUCCESS             0
#define ERROR_FILE_NOT_FOUND      2
#define ERROR_INVALID_HANDLE      6
#define ERROR_NOT_ENOUGH_MEMORY   8
#define ERROR_INVALID_PARAMETER   87
#define ERROR_MORE_DATA           234
#define ERROR_NO_MORE_ITEMS       259
#define ERROR_UNSUPPORTED_TYPE    1630
#define ERROR_CALL_NOT_IMPLEMENTED 120
#define ERROR_BAD_PATHNAME        161
#define ERROR_NOACCESS            998
#define ERROR_BAD_NETPATH         53
#define ERROR_INVALID_DATA        13

#define REG_SZ        1u
#define REG_EXPAND_SZ 2u
#define REG_MULTI_SZ  7u

#define KEY_WOW64_MASK 0x0300u
#define MAXIMUM_ALLOWED 0x02000000u
#define OBJ_CASE_INSENSITIVE 0x40u
#define OBJ_INHERIT 0x02u

#define NAME_UNITS 512u   /* a key path a call names: Windows' limit is far longer */

static LONG32 err_of(NTSTATUS_T st) {
    return st == 0 ? ERROR_SUCCESS : (LONG32)RtlNtStatusToDosError((vibeos_ntstatus_t)st);
}

static uint32_t wlen(const uint16_t *s) {
    uint32_t n = 0;

    while (s[n]) {
        n++;
    }
    return n;
}

static uint32_t alen(const char *s) {
    uint32_t n = 0;

    while (s[n]) {
        n++;
    }
    return n;
}

static void *heap(void) {
    return (void *)(uintptr_t)vibeos_nt_rd64(vibeos_nt_rd64(vibeos_nt_teb() + VIBEOS_TEB_PEB) +
                                              VIBEOS_PEB_PROCESS_HEAP);
}

/* ANSI to wide: `n` bytes (the terminator not counted) into `out`. */
static uint32_t widen_n(const char *s, uint32_t n, uint16_t *out) {
    uint32_t got = 0;

    (void)RtlMultiByteToUnicodeN(out, n * 2u, &got, s, n);
    return got / 2u;
}

/* A wide character as Windows-1252's byte, '?' for what it has none for. */
static char narrow(uint16_t w) {
    unsigned c;

    if (w < 0x80u || (w >= 0xA0u && w <= 0xFFu)) {
        return (char)w;
    }
    for (c = 0x80u; c < 0xA0u; c++) {
        unsigned char b = (unsigned char)c, *p = &b;

        if (RtlAnsiCharToUnicodeChar(&p) == w) {
            return (char)c;
        }
    }
    return '?';
}

static int is_string(uint32_t type) {
    return type == REG_SZ || type == REG_EXPAND_SZ || type == REG_MULTI_SZ;
}

/* ---- handles ------------------------------------------------------------------------------ */

/* HKEY_CLASSES_ROOT is 0x80000000, and a 64-bit program holds it sign
 * extended: both spellings are the predefined key. */
static int predefined(uint64_t h) {
    uint32_t lo = (uint32_t)h, hi = (uint32_t)(h >> 32);

    return lo >= 0x80000000u && lo <= 0x80000006u && (hi == 0u || hi == 0xFFFFFFFFu);
}

static uint64_t g_pre[7];

static NTSTATUS_T open_path(uint64_t *out, uint32_t access, uint64_t root, const uint16_t *path,
                            uint32_t n) {
    vibeos_ustring_t us;
    uint64_t oa[6];

    us.Length = (uint16_t)(n * 2u);
    us.MaximumLength = (uint16_t)(n * 2u);
    us.Buffer = (vibeos_wchar_t *)(uintptr_t)path;
    oa[0] = 48u;
    oa[1] = root;
    oa[2] = (uint64_t)(uintptr_t)&us;
    oa[3] = OBJ_CASE_INSENSITIVE;
    oa[4] = oa[5] = 0;
    return NtOpenKey(out, access, oa);
}

/* The NT handle a caller's key stands for: itself, or the predefined key's,
 * opened the first time it is used. */
static LONG32 real_key(uint64_t h, uint64_t *out) {
    static const char *const paths[7] = {
        "\\Registry\\Machine\\Software\\Classes", "\\Registry\\User\\.Default", "\\Registry\\Machine",
        "\\Registry\\User", 0, "\\Registry\\Machine\\System\\CurrentControlSet\\Hardware Profiles\\Current",
        0,
    };
    uint32_t i;

    if (!predefined(h)) {
        if (h == 0u) {
            return ERROR_INVALID_HANDLE;
        }
        *out = h;
        return ERROR_SUCCESS;
    }
    i = (uint32_t)h - 0x80000000u;
    if (!paths[i]) {
        return ERROR_CALL_NOT_IMPLEMENTED;   /* performance data: no counters here */
    }
    if (g_pre[i] == 0u) {
        uint16_t w[96];
        uint64_t k = 0, zero = 0;
        uint32_t n = 0;
        NTSTATUS_T st;

        while (paths[i][n]) {
            w[n] = (uint16_t)(unsigned char)paths[i][n];
            n++;
        }
        st = open_path(&k, MAXIMUM_ALLOWED, 0, w, n);
        if (st != 0) {
            return err_of(st);
        }
        /* Two threads opening it at once keep one handle. */
        if (!__atomic_compare_exchange_n(&g_pre[i], &zero, k, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            (void)NtClose(k);
        }
    }
    *out = g_pre[i];
    return ERROR_SUCCESS;
}

LONG32 RegCloseKey(uint64_t h) {
    if (predefined(h)) {
        return ERROR_SUCCESS;
    }
    return err_of(NtClose(h));
}

/* ---- opening and creating ------------------------------------------------------------------ */

LONG32 RegOpenKeyExW(uint64_t h, const uint16_t *sub, uint32_t options, uint32_t sam, uint64_t *out) {
    uint64_t root;
    LONG32 e;

    (void)options;
    if (!out) {
        return ERROR_INVALID_PARAMETER;
    }
    *out = 0;   /* a failed open leaves no handle behind, as Windows answers */
    e = real_key(h, &root);
    if (e != ERROR_SUCCESS) {
        return e;
    }
    if ((!sub || !sub[0]) && predefined(h)) {
        *out = h;   /* the predefined key itself, as Windows answers */
        return ERROR_SUCCESS;
    }
    e = err_of(open_path(out, sam & ~KEY_WOW64_MASK, root, sub ? sub : (const uint16_t *)u"",
                         sub ? wlen(sub) : 0u));
    if (e != ERROR_SUCCESS) {
        *out = 0;
    }
    return e;
}

LONG32 RegOpenKeyExA(uint64_t h, const char *sub, uint32_t options, uint32_t sam, uint64_t *out) {
    uint16_t w[NAME_UNITS];
    uint32_t n;

    if (!sub) {
        return RegOpenKeyExW(h, 0, options, sam, out);
    }
    n = alen(sub);
    if (n >= NAME_UNITS) {
        return ERROR_INVALID_PARAMETER;
    }
    w[widen_n(sub, n, w)] = 0;
    return RegOpenKeyExW(h, w, options, sam, out);
}

LONG32 RegOpenKeyW(uint64_t h, const uint16_t *sub, uint64_t *out) {
    return RegOpenKeyExW(h, sub, 0, MAXIMUM_ALLOWED, out);
}

LONG32 RegOpenKeyA(uint64_t h, const char *sub, uint64_t *out) {
    return RegOpenKeyExA(h, sub, 0, MAXIMUM_ALLOWED, out);
}

/* A handle of its own to the current user's key, which the caller closes -
 * not the predefined HKEY_CURRENT_USER. Every process's user is .DEFAULT
 * until there are tokens (WO-086). */
LONG32 RegOpenCurrentUser(uint32_t sam, uint64_t *out) {
    uint64_t hkcu;
    LONG32 e;

    if (!out) {
        return ERROR_INVALID_PARAMETER;
    }
    e = real_key(0x80000001u, &hkcu);
    if (e != ERROR_SUCCESS) {
        return e;
    }
    return err_of(open_path(out, sam & ~KEY_WOW64_MASK, hkcu, (const uint16_t *)u"", 0u));
}

/* Windows makes every missing key of the path, not only the last, so the
 * path is created a component at a time; the disposition is the last's. */
LONG32 RegCreateKeyExW(uint64_t h, const uint16_t *sub, uint32_t reserved, const uint16_t *cls,
                       uint32_t options, uint32_t sam, const void *sa, uint64_t *out, uint32_t *disp) {
    uint64_t root, cur, next;
    uint32_t at = 0, n, d = 0;
    LONG32 e;

    (void)reserved;
    (void)cls;
    if (!out || !sub) {
        return ERROR_INVALID_PARAMETER;
    }
    e = real_key(h, &root);
    if (e != ERROR_SUCCESS) {
        return e;
    }
    n = wlen(sub);
    while (n > 0u && sub[n - 1u] == '\\') {
        n--;
    }
    if (n > 0u && sub[0] == '\\') {
        return ERROR_BAD_PATHNAME;
    }
    cur = root;
    if (n == 0u) {
        return err_of(open_path(out, sam & ~KEY_WOW64_MASK, root, (const uint16_t *)u"", 0u));
    }
    while (at < n) {
        uint32_t end = at;
        vibeos_ustring_t us;
        uint64_t oa[6];
        NTSTATUS_T st;
        int last;

        while (end < n && sub[end] != '\\') {
            end++;
        }
        last = end >= n;
        us.Length = (uint16_t)((end - at) * 2u);
        us.MaximumLength = us.Length;
        us.Buffer = (vibeos_wchar_t *)(uintptr_t)(sub + at);
        oa[0] = 48u;
        oa[1] = cur;
        oa[2] = (uint64_t)(uintptr_t)&us;
        oa[3] = OBJ_CASE_INSENSITIVE | (sa ? OBJ_INHERIT : 0u);
        oa[4] = oa[5] = 0;
        st = NtCreateKey(&next, last ? (sam & ~KEY_WOW64_MASK) : MAXIMUM_ALLOWED, oa, 0, 0, options & 1u,
                         &d);
        if (cur != root) {
            (void)NtClose(cur);
        }
        if (st != 0) {
            return err_of(st);
        }
        cur = next;
        at = end + 1u;
    }
    *out = cur;
    if (disp) {
        *disp = d;
    }
    return ERROR_SUCCESS;
}

LONG32 RegCreateKeyExA(uint64_t h, const char *sub, uint32_t reserved, const char *cls,
                       uint32_t options, uint32_t sam, const void *sa, uint64_t *out, uint32_t *disp) {
    uint16_t w[NAME_UNITS];
    uint32_t n;

    (void)cls;
    if (!sub) {
        return ERROR_INVALID_PARAMETER;
    }
    n = alen(sub);
    if (n >= NAME_UNITS) {
        return ERROR_INVALID_PARAMETER;
    }
    w[widen_n(sub, n, w)] = 0;
    return RegCreateKeyExW(h, w, reserved, 0, options, sam, sa, out, disp);
}

LONG32 RegCreateKeyW(uint64_t h, const uint16_t *sub, uint64_t *out) {
    return RegCreateKeyExW(h, sub ? sub : (const uint16_t *)u"", 0, 0, 0, MAXIMUM_ALLOWED, 0, out, 0);
}

LONG32 RegCreateKeyA(uint64_t h, const char *sub, uint64_t *out) {
    return RegCreateKeyExA(h, sub ? sub : "", 0, 0, 0, MAXIMUM_ALLOWED, 0, out, 0);
}

/* ---- deleting -------------------------------------------------------------------------------- */

LONG32 RegDeleteKeyExW(uint64_t h, const uint16_t *sub, uint32_t sam, uint32_t reserved) {
    uint64_t root, k;
    NTSTATUS_T st;
    LONG32 e;

    (void)reserved;
    if (!sub) {
        return ERROR_INVALID_PARAMETER;
    }
    e = real_key(h, &root);
    if (e != ERROR_SUCCESS) {
        return e;
    }
    st = open_path(&k, 0x00010000u /* DELETE */ | (sam & ~KEY_WOW64_MASK), root, sub, wlen(sub));
    if (st != 0) {
        return err_of(st);
    }
    st = NtDeleteKey(k);
    (void)NtClose(k);
    return err_of(st);
}

LONG32 RegDeleteKeyW(uint64_t h, const uint16_t *sub) {
    return RegDeleteKeyExW(h, sub, 0, 0);
}

LONG32 RegDeleteKeyExA(uint64_t h, const char *sub, uint32_t sam, uint32_t reserved) {
    uint16_t w[NAME_UNITS];
    uint32_t n;

    if (!sub) {
        return ERROR_INVALID_PARAMETER;
    }
    n = alen(sub);
    if (n >= NAME_UNITS) {
        return ERROR_INVALID_PARAMETER;
    }
    w[widen_n(sub, n, w)] = 0;
    return RegDeleteKeyExW(h, w, sam, reserved);
}

LONG32 RegDeleteKeyA(uint64_t h, const char *sub) {
    return RegDeleteKeyExA(h, sub, 0, 0);
}

/* A key and everything under it; with a null subkey, everything under `h`
 * and its values, leaving `h`. Children first, by repeatedly taking the first
 * one, so the enumeration index never moves under the walk. */
static LONG32 delete_tree(uint64_t k) {
    uint8_t info[16 + 2u * 256u];   /* on the stack: delete_tree recurses, and threads call */
    uint32_t got;
    LONG32 e;

    for (;;) {
        uint64_t child;
        NTSTATUS_T st = NtEnumerateKey(k, 0, 0, info, sizeof(info), &got);

        if (st == (NTSTATUS_T)0x8000001Au) {
            break;   /* no more subkeys */
        }
        if (st != 0) {
            return err_of(st);
        }
        st = open_path(&child, 0x000F003Fu, k, (const uint16_t *)(const void *)(info + 16),
                       *(const uint32_t *)(info + 12) / 2u);
        if (st != 0) {
            return err_of(st);
        }
        e = delete_tree(child);
        if (e == ERROR_SUCCESS) {
            e = err_of(NtDeleteKey(child));
        }
        (void)NtClose(child);
        if (e != ERROR_SUCCESS) {
            return e;
        }
    }
    return ERROR_SUCCESS;
}

LONG32 RegDeleteTreeW(uint64_t h, const uint16_t *sub) {
    uint64_t root, k;
    LONG32 e = real_key(h, &root);

    if (e != ERROR_SUCCESS) {
        return e;
    }
    if (!sub) {
        return delete_tree(root);
    }
    e = err_of(open_path(&k, 0x000F003Fu, root, sub, wlen(sub)));
    if (e != ERROR_SUCCESS) {
        return e;
    }
    e = delete_tree(k);
    if (e == ERROR_SUCCESS) {
        e = err_of(NtDeleteKey(k));
    }
    (void)NtClose(k);
    return e;
}

LONG32 RegDeleteTreeA(uint64_t h, const char *sub) {
    uint16_t w[NAME_UNITS];
    uint32_t n;

    if (!sub) {
        return RegDeleteTreeW(h, 0);
    }
    n = alen(sub);
    if (n >= NAME_UNITS) {
        return ERROR_INVALID_PARAMETER;
    }
    w[widen_n(sub, n, w)] = 0;
    return RegDeleteTreeW(h, w);
}

/* ---- values ------------------------------------------------------------------------------- */

static void ustr_of(vibeos_ustring_t *us, const uint16_t *s) {
    uint32_t n = s ? wlen(s) : 0u;

    us->Length = (uint16_t)(n * 2u);
    us->MaximumLength = us->Length;
    us->Buffer = (vibeos_wchar_t *)(uintptr_t)(s ? s : (const uint16_t *)u"");
}

LONG32 RegSetValueExW(uint64_t h, const uint16_t *name, uint32_t reserved, uint32_t type,
                      const uint8_t *data, uint32_t len) {
    vibeos_ustring_t us;
    uint64_t k;
    LONG32 e = real_key(h, &k);

    (void)reserved;
    if (e != ERROR_SUCCESS) {
        return e;
    }
    if (!data && len != 0u) {
        return ERROR_NOACCESS;
    }
    ustr_of(&us, name);
    /* A string handed over without its terminator is stored with one, in the
     * wide form as in the ANSI one (advapi32:registry's off-by-one sizes). */
    if (is_string(type) && len >= 2u && (len & 1u) == 0u &&
        (data[len - 2u] != 0 || data[len - 1u] != 0)) {
        uint8_t *copy = (uint8_t *)RtlAllocateHeap(heap(), 0, (size_t)len + 2u);
        uint32_t i;

        if (!copy) {
            return ERROR_NOT_ENOUGH_MEMORY;
        }
        for (i = 0; i < len; i++) {
            copy[i] = data[i];
        }
        copy[len] = copy[len + 1u] = 0;
        e = err_of(NtSetValueKey(k, &us, 0, type, copy, len + 2u));
        RtlFreeHeap(heap(), 0, copy);
        return e;
    }
    return err_of(NtSetValueKey(k, &us, 0, type, data, len));
}

LONG32 RegSetValueExA(uint64_t h, const char *name, uint32_t reserved, uint32_t type,
                      const uint8_t *data, uint32_t len) {
    uint16_t w[NAME_UNITS];
    uint16_t *wd = 0;
    uint32_t n = name ? alen(name) : 0u;
    LONG32 e;

    if (n >= NAME_UNITS) {
        return ERROR_INVALID_PARAMETER;
    }
    w[widen_n(name ? name : "", n, w)] = 0;
    if (!is_string(type) || !data) {
        return RegSetValueExW(h, w, reserved, type, data, len);
    }
    /* String data in the ANSI form is ANSI: kept wide, as Windows keeps it -
     * and terminated: a string handed over without its NUL gets one, which
     * is what Windows stores (advapi32:registry's off-by-one size). */
    wd = (uint16_t *)RtlAllocateHeap(heap(), 0, (size_t)len * 2u + 4u);
    if (!wd) {
        return ERROR_NOT_ENOUGH_MEMORY;
    }
    {
        uint32_t units = widen_n((const char *)data, len, wd);

        if (units != 0u && wd[units - 1u] != 0) {
            wd[units++] = 0;
        }
        e = RegSetValueExW(h, w, reserved, type, (const uint8_t *)wd, units * 2u);
    }
    RtlFreeHeap(heap(), 0, wd);
    return e;
}

/* The value, as KeyValuePartialInformation: the record in a buffer of the
 * process heap, which the caller frees. */
static LONG32 query_partial(uint64_t k, const uint16_t *name, uint8_t **rec, uint32_t *type,
                            uint32_t *len) {
    vibeos_ustring_t us;
    uint32_t cap = 256u, got = 0;
    NTSTATUS_T st;

    ustr_of(&us, name);
    for (;;) {
        *rec = (uint8_t *)RtlAllocateHeap(heap(), 0, cap);
        if (!*rec) {
            return ERROR_NOT_ENOUGH_MEMORY;
        }
        st = NtQueryValueKey(k, &us, 2u, *rec, cap, &got);
        if (st != (NTSTATUS_T)0x80000005u && st != (NTSTATUS_T)0xC0000023u) {
            break;
        }
        RtlFreeHeap(heap(), 0, *rec);
        cap = got + 16u;
    }
    if (st != 0) {
        RtlFreeHeap(heap(), 0, *rec);
        *rec = 0;
        return err_of(st);
    }
    *type = *(const uint32_t *)(*rec + 4);
    *len = *(const uint32_t *)(*rec + 8);
    return ERROR_SUCCESS;
}

/* What the W and A query forms share: the data, its type and its size, with
 * ERROR_MORE_DATA and the size needed when the caller's buffer is short. */
static LONG32 give(const uint8_t *src, uint32_t n, uint8_t *data, uint32_t *len) {
    uint32_t i;

    if (data) {
        if (!len) {
            return ERROR_INVALID_PARAMETER;
        }
        if (*len < n) {
            *len = n;
            return ERROR_MORE_DATA;
        }
        for (i = 0; i < n; i++) {
            data[i] = src[i];
        }
    }
    if (len) {
        *len = n;
    }
    return ERROR_SUCCESS;
}

LONG32 RegQueryValueExW(uint64_t h, const uint16_t *name, uint32_t *reserved, uint32_t *type,
                        uint8_t *data, uint32_t *len) {
    uint8_t *rec;
    uint32_t t, n;
    uint64_t k;
    LONG32 e = real_key(h, &k);

    if (e != ERROR_SUCCESS) {
        return e;
    }
    if (reserved || (data && !len)) {
        return ERROR_INVALID_PARAMETER;
    }
    e = query_partial(k, name, &rec, &t, &n);
    if (e != ERROR_SUCCESS) {
        return e;
    }
    if (type) {
        *type = t;
    }
    e = give(rec + 12, n, data, len);
    RtlFreeHeap(heap(), 0, rec);
    return e;
}

LONG32 RegQueryValueExA(uint64_t h, const char *name, uint32_t *reserved, uint32_t *type,
                        uint8_t *data, uint32_t *len) {
    uint16_t w[NAME_UNITS];
    uint8_t *rec;
    uint32_t t, n, i, nn = name ? alen(name) : 0u;
    uint64_t k;
    LONG32 e = real_key(h, &k);

    if (e != ERROR_SUCCESS) {
        return e;
    }
    if (reserved || (data && !len) || nn >= NAME_UNITS) {
        return ERROR_INVALID_PARAMETER;
    }
    w[widen_n(name ? name : "", nn, w)] = 0;
    e = query_partial(k, w, &rec, &t, &n);
    if (e != ERROR_SUCCESS) {
        return e;
    }
    if (type) {
        *type = t;
    }
    if (is_string(t)) {
        /* Wide strings as ANSI, a character a byte: in place, half the size. */
        const uint16_t *src = (const uint16_t *)(const void *)(rec + 12);

        for (i = 0; i < n / 2u; i++) {
            rec[12u + i] = (uint8_t)narrow(src[i]);
        }
        n /= 2u;
    }
    e = give(rec + 12, n, data, len);
    RtlFreeHeap(heap(), 0, rec);
    return e;
}

LONG32 RegDeleteValueW(uint64_t h, const uint16_t *name) {
    vibeos_ustring_t us;
    uint64_t k;
    LONG32 e = real_key(h, &k);

    if (e != ERROR_SUCCESS) {
        return e;
    }
    ustr_of(&us, name);
    return err_of(NtDeleteValueKey(k, &us));
}

LONG32 RegDeleteValueA(uint64_t h, const char *name) {
    uint16_t w[NAME_UNITS];
    uint32_t n = name ? alen(name) : 0u;

    if (n >= NAME_UNITS) {
        return ERROR_INVALID_PARAMETER;
    }
    w[widen_n(name ? name : "", n, w)] = 0;
    return RegDeleteValueW(h, w);
}

/* ---- enumerating and describing ----------------------------------------------------------- */

LONG32 RegEnumKeyExW(uint64_t h, uint32_t index, uint16_t *name, uint32_t *name_len, uint32_t *reserved,
                     uint16_t *cls, uint32_t *cls_len, int64_t *last_write) {
    uint8_t info[16 + 2u * 256u];   /* on the stack: delete_tree recurses, and threads call */
    uint32_t got, n, i;
    uint64_t k;
    NTSTATUS_T st;
    LONG32 e = real_key(h, &k);

    if (e != ERROR_SUCCESS) {
        return e;
    }
    if (reserved || !name || !name_len) {
        return ERROR_INVALID_PARAMETER;
    }
    st = NtEnumerateKey(k, index, 0u, info, sizeof(info), &got);
    if (st != 0) {
        return err_of(st);
    }
    n = *(const uint32_t *)(info + 12) / 2u;
    if (*name_len <= n) {
        *name_len = n;
        return ERROR_MORE_DATA;
    }
    for (i = 0; i < n; i++) {
        name[i] = ((const uint16_t *)(const void *)(info + 16))[i];
    }
    name[n] = 0;
    *name_len = n;
    if (cls && cls_len && *cls_len) {
        cls[0] = 0;
    }
    if (cls_len) {
        *cls_len = 0;
    }
    if (last_write) {
        *last_write = *(const int64_t *)(const void *)info;
    }
    return ERROR_SUCCESS;
}

LONG32 RegEnumKeyExA(uint64_t h, uint32_t index, char *name, uint32_t *name_len, uint32_t *reserved,
                     char *cls, uint32_t *cls_len, int64_t *last_write) {
    uint16_t w[257];
    uint32_t n = 257u, i;
    LONG32 e;

    if (!name || !name_len) {
        return ERROR_INVALID_PARAMETER;
    }
    e = RegEnumKeyExW(h, index, w, &n, reserved, 0, 0, last_write);
    if (e != ERROR_SUCCESS) {
        return e;
    }
    if (*name_len <= n) {
        *name_len = n;
        return ERROR_MORE_DATA;
    }
    for (i = 0; i <= n; i++) {
        name[i] = narrow(w[i]);
    }
    *name_len = n;
    if (cls && cls_len && *cls_len) {
        cls[0] = 0;
    }
    if (cls_len) {
        *cls_len = 0;
    }
    return ERROR_SUCCESS;
}

LONG32 RegEnumKeyW(uint64_t h, uint32_t index, uint16_t *name, uint32_t len) {
    return RegEnumKeyExW(h, index, name, &len, 0, 0, 0, 0);
}

LONG32 RegEnumKeyA(uint64_t h, uint32_t index, char *name, uint32_t len) {
    return RegEnumKeyExA(h, index, name, &len, 0, 0, 0, 0);
}

/* The index-th value as KeyValueFullInformation, in a heap buffer. */
static LONG32 enum_full(uint64_t k, uint32_t index, uint8_t **rec) {
    uint32_t cap = 512u, got = 0;
    NTSTATUS_T st;

    for (;;) {
        *rec = (uint8_t *)RtlAllocateHeap(heap(), 0, cap);
        if (!*rec) {
            return ERROR_NOT_ENOUGH_MEMORY;
        }
        st = NtEnumerateValueKey(k, index, 1u, *rec, cap, &got);
        if (st != (NTSTATUS_T)0x80000005u && st != (NTSTATUS_T)0xC0000023u) {
            break;
        }
        RtlFreeHeap(heap(), 0, *rec);
        cap = got + 16u;
    }
    if (st != 0) {
        RtlFreeHeap(heap(), 0, *rec);
        *rec = 0;
        return err_of(st);
    }
    return ERROR_SUCCESS;
}

LONG32 RegEnumValueW(uint64_t h, uint32_t index, uint16_t *name, uint32_t *name_len, uint32_t *reserved,
                     uint32_t *type, uint8_t *data, uint32_t *len) {
    uint8_t *rec;
    uint32_t nl, i;
    uint64_t k;
    LONG32 e = real_key(h, &k);

    if (e != ERROR_SUCCESS) {
        return e;
    }
    if (reserved || !name || !name_len || (data && !len)) {
        return ERROR_INVALID_PARAMETER;
    }
    e = enum_full(k, index, &rec);
    if (e != ERROR_SUCCESS) {
        return e;
    }
    nl = *(const uint32_t *)(rec + 16) / 2u;
    if (*name_len <= nl) {
        /* The name does not fit: Windows leaves the caller's length as it was
         * and the buffers untouched, and still says the type and the size of
         * the data (advapi32:registry's overflow cases). */
        if (type) {
            *type = *(const uint32_t *)(rec + 4);
        }
        if (len) {
            *len = *(const uint32_t *)(rec + 12);
        }
        RtlFreeHeap(heap(), 0, rec);
        return ERROR_MORE_DATA;
    }
    for (i = 0; i < nl; i++) {
        name[i] = ((const uint16_t *)(const void *)(rec + 20))[i];
    }
    name[nl] = 0;
    *name_len = nl;
    if (type) {
        *type = *(const uint32_t *)(rec + 4);
    }
    e = give(rec + *(const uint32_t *)(rec + 8), *(const uint32_t *)(rec + 12), data, len);
    RtlFreeHeap(heap(), 0, rec);
    return e;
}

LONG32 RegEnumValueA(uint64_t h, uint32_t index, char *name, uint32_t *name_len, uint32_t *reserved,
                     uint32_t *type, uint8_t *data, uint32_t *len) {
    uint8_t *rec;
    uint32_t nl, i, t, n;
    uint64_t k;
    LONG32 e = real_key(h, &k);

    if (e != ERROR_SUCCESS) {
        return e;
    }
    if (reserved || !name || !name_len || (data && !len)) {
        return ERROR_INVALID_PARAMETER;
    }
    e = enum_full(k, index, &rec);
    if (e != ERROR_SUCCESS) {
        return e;
    }
    nl = *(const uint32_t *)(rec + 16) / 2u;
    t = *(const uint32_t *)(rec + 4);
    n = *(const uint32_t *)(rec + 12);
    if (*name_len <= nl) {
        if (type) {
            *type = t;
        }
        if (len) {
            *len = is_string(t) ? n / 2u : n;
        }
        RtlFreeHeap(heap(), 0, rec);
        return ERROR_MORE_DATA;
    }
    for (i = 0; i < nl; i++) {
        name[i] = narrow(((const uint16_t *)(const void *)(rec + 20))[i]);
    }
    name[nl] = 0;
    *name_len = nl;
    if (type) {
        *type = t;
    }
    if (is_string(t)) {
        uint8_t *d = rec + *(const uint32_t *)(rec + 8);
        const uint16_t *src = (const uint16_t *)(const void *)d;

        for (i = 0; i < n / 2u; i++) {
            d[i] = (uint8_t)narrow(src[i]);
        }
        n /= 2u;
    }
    e = give(rec + *(const uint32_t *)(rec + 8), n, data, len);
    RtlFreeHeap(heap(), 0, rec);
    return e;
}

LONG32 RegQueryInfoKeyW(uint64_t h, uint16_t *cls, uint32_t *cls_len, uint32_t *reserved, uint32_t *subkeys,
                        uint32_t *max_subkey, uint32_t *max_class, uint32_t *values, uint32_t *max_value_name,
                        uint32_t *max_value, uint32_t *security, int64_t *last_write) {
    uint8_t info[48];
    uint32_t got;
    uint64_t k;
    NTSTATUS_T st;
    LONG32 e = real_key(h, &k);

    if (e != ERROR_SUCCESS) {
        return e;
    }
    if (reserved) {
        return ERROR_INVALID_PARAMETER;
    }
    st = NtQueryKey(k, 2u, info, sizeof(info), &got);
    if (st != 0) {
        return err_of(st);
    }
    if (cls && cls_len && *cls_len) {
        cls[0] = 0;
    }
    if (cls_len) {
        *cls_len = 0;
    }
    if (subkeys) {
        *subkeys = *(const uint32_t *)(info + 20);
    }
    if (max_subkey) {
        *max_subkey = *(const uint32_t *)(info + 24) / 2u;
    }
    if (max_class) {
        *max_class = 0;
    }
    if (values) {
        *values = *(const uint32_t *)(info + 32);
    }
    if (max_value_name) {
        *max_value_name = *(const uint32_t *)(info + 36) / 2u;
    }
    if (max_value) {
        *max_value = *(const uint32_t *)(info + 40);
    }
    if (security) {
        *security = 0;
    }
    if (last_write) {
        *last_write = *(const int64_t *)(const void *)info;
    }
    return ERROR_SUCCESS;
}

LONG32 RegQueryInfoKeyA(uint64_t h, char *cls, uint32_t *cls_len, uint32_t *reserved, uint32_t *subkeys,
                        uint32_t *max_subkey, uint32_t *max_class, uint32_t *values, uint32_t *max_value_name,
                        uint32_t *max_value, uint32_t *security, int64_t *last_write) {
    LONG32 e = RegQueryInfoKeyW(h, 0, 0, reserved, subkeys, max_subkey, max_class, values, max_value_name,
                                max_value, security, last_write);

    if (e == ERROR_SUCCESS) {
        if (cls && cls_len && *cls_len) {
            cls[0] = 0;
        }
        if (cls_len) {
            *cls_len = 0;
        }
    }
    return e;
}

LONG32 RegFlushKey(uint64_t h) {
    uint64_t k;
    LONG32 e = real_key(h, &k);

    return e != ERROR_SUCCESS ? e : err_of(NtFlushKey(k));
}


/* ---- the older and the convenience forms --------------------------------------------------- */

/* RegSetValue: the default value of a subkey - made if it is missing - as a
 * string. Only REG_SZ, as Windows has it. */
LONG32 RegSetValueW(uint64_t h, const uint16_t *sub, uint32_t type, const uint16_t *data, uint32_t len) {
    uint64_t k = h;
    LONG32 e = ERROR_SUCCESS;

    (void)len;
    if (type != REG_SZ || !data) {
        return ERROR_INVALID_PARAMETER;
    }
    if (sub && sub[0]) {
        e = RegCreateKeyExW(h, sub, 0, 0, 0, 0x00020006u /* KEY_WRITE */, 0, &k, 0);
        if (e != ERROR_SUCCESS) {
            return e;
        }
    }
    e = RegSetValueExW(k, 0, 0, REG_SZ, (const uint8_t *)data, (wlen(data) + 1u) * 2u);
    if (k != h) {
        (void)RegCloseKey(k);
    }
    return e;
}

LONG32 RegSetValueA(uint64_t h, const char *sub, uint32_t type, const char *data, uint32_t len) {
    uint64_t k = h;
    LONG32 e = ERROR_SUCCESS;

    (void)len;
    if (type != REG_SZ || !data) {
        return ERROR_INVALID_PARAMETER;
    }
    if (sub && sub[0]) {
        e = RegCreateKeyExA(h, sub, 0, 0, 0, 0x00020006u, 0, &k, 0);
        if (e != ERROR_SUCCESS) {
            return e;
        }
    }
    e = RegSetValueExA(k, 0, 0, REG_SZ, (const uint8_t *)data, alen(data) + 1u);
    if (k != h) {
        (void)RegCloseKey(k);
    }
    return e;
}

/* RegQueryValue: a subkey's default value as a string; a key without one
 * answers an empty string, as Windows does. */
LONG32 RegQueryValueW(uint64_t h, const uint16_t *sub, uint16_t *data, int32_t *len) {
    uint64_t k = h;
    uint32_t n = len ? (uint32_t)*len : 0u, type = 0;
    LONG32 e;

    if (sub && sub[0]) {
        e = RegOpenKeyExW(h, sub, 0, 0x00020019u /* KEY_READ */, &k);
        if (e != ERROR_SUCCESS) {
            return e;
        }
    }
    e = RegQueryValueExW(k, 0, 0, &type, (uint8_t *)data, len ? &n : 0);
    if (e == ERROR_FILE_NOT_FOUND) {
        e = ERROR_SUCCESS;
        n = 2u;
        if (data && len && *len >= 2) {
            data[0] = 0;
        }
    }
    if (len) {
        *len = (int32_t)n;
    }
    if (k != h) {
        (void)RegCloseKey(k);
    }
    return e;
}

LONG32 RegQueryValueA(uint64_t h, const char *sub, char *data, int32_t *len) {
    uint64_t k = h;
    uint32_t n = len ? (uint32_t)*len : 0u, type = 0;
    LONG32 e;

    if (sub && sub[0]) {
        e = RegOpenKeyExA(h, sub, 0, 0x00020019u, &k);
        if (e != ERROR_SUCCESS) {
            return e;
        }
    }
    e = RegQueryValueExA(k, 0, 0, &type, (uint8_t *)data, len ? &n : 0);
    if (e == ERROR_FILE_NOT_FOUND) {
        e = ERROR_SUCCESS;
        n = 1u;
        if (data && len && *len >= 1) {
            data[0] = 0;
        }
    }
    if (len) {
        *len = (int32_t)n;
    }
    if (k != h) {
        (void)RegCloseKey(k);
    }
    return e;
}

#define RRF_RT_REG_NONE      0x00000001u
#define RRF_RT_REG_SZ        0x00000002u
#define RRF_RT_REG_EXPAND_SZ 0x00000004u
#define RRF_RT_REG_BINARY    0x00000008u
#define RRF_RT_REG_DWORD     0x00000010u
#define RRF_RT_REG_MULTI_SZ  0x00000020u
#define RRF_RT_REG_QWORD     0x00000040u
#define RRF_RT_ANY           0x0000FFFFu
#define RRF_NOEXPAND         0x10000000u
#define RRF_ZEROONFAILURE    0x20000000u

static uint32_t rrf_of(uint32_t type) {
    switch (type) {
        case 0u: return RRF_RT_REG_NONE;
        case REG_SZ: return RRF_RT_REG_SZ;
        case REG_EXPAND_SZ: return RRF_RT_REG_EXPAND_SZ;
        case 3u: return RRF_RT_REG_BINARY;
        case 4u: return RRF_RT_REG_DWORD;
        case REG_MULTI_SZ: return RRF_RT_REG_MULTI_SZ;
        case 11u: return RRF_RT_REG_QWORD;
        default: return 0u;
    }
}

/* RegGetValue: a value of a subkey, refused unless its type is one the flags
 * allow. A REG_EXPAND_SZ is not expanded here (WO-094): asked for without
 * RRF_NOEXPAND it is handed over as stored, typed REG_SZ as Windows types an
 * expanded one. */
static LONG32 get_value(uint64_t h, const void *sub, const void *value, uint32_t flags, uint32_t *type,
                        void *data, uint32_t *len, int wide) {
    uint64_t k = h;
    uint32_t t = 0, cap = len ? *len : 0u, n;
    LONG32 e;

    if ((flags & RRF_RT_ANY) == 0u || (data && !len) ||
        ((flags & RRF_RT_ANY) == RRF_RT_REG_EXPAND_SZ && !(flags & RRF_NOEXPAND))) {
        return ERROR_INVALID_PARAMETER;
    }
    if (sub && (wide ? ((const uint16_t *)sub)[0] : ((const char *)sub)[0])) {
        e = wide ? RegOpenKeyExW(h, (const uint16_t *)sub, 0, 0x00020019u, &k)
                 : RegOpenKeyExA(h, (const char *)sub, 0, 0x00020019u, &k);
        if (e != ERROR_SUCCESS) {
            return e;
        }
    }
    n = cap;
    e = wide ? RegQueryValueExW(k, (const uint16_t *)value, 0, &t, (uint8_t *)data, len ? &n : 0)
             : RegQueryValueExA(k, (const char *)value, 0, &t, (uint8_t *)data, len ? &n : 0);
    if (k != h) {
        (void)RegCloseKey(k);
    }
    if ((e == ERROR_SUCCESS || e == ERROR_MORE_DATA) && !(rrf_of(t) & flags)) {
        e = ERROR_UNSUPPORTED_TYPE;
    }
    if (e == ERROR_SUCCESS && t == REG_EXPAND_SZ && !(flags & RRF_NOEXPAND)) {
        t = REG_SZ;
    }
    if (e != ERROR_SUCCESS && (flags & RRF_ZEROONFAILURE) && data && cap) {
        uint32_t i;

        for (i = 0; i < cap; i++) {
            ((uint8_t *)data)[i] = 0;
        }
    }
    if (type) {
        *type = t;
    }
    if (len) {
        *len = n;
    }
    return e;
}

LONG32 RegGetValueW(uint64_t h, const uint16_t *sub, const uint16_t *value, uint32_t flags, uint32_t *type,
                    void *data, uint32_t *len) {
    return get_value(h, sub, value, flags, type, data, len, 1);
}

LONG32 RegGetValueA(uint64_t h, const char *sub, const char *value, uint32_t flags, uint32_t *type,
                    void *data, uint32_t *len) {
    return get_value(h, sub, value, flags, type, data, len, 0);
}

LONG32 RegRenameKey(uint64_t h, const uint16_t *sub, const uint16_t *name) {
    vibeos_ustring_t us;
    uint64_t k = h;
    LONG32 e;

    if (!name) {
        return ERROR_INVALID_PARAMETER;
    }
    if (sub && sub[0]) {
        e = RegOpenKeyExW(h, sub, 0, 0x000F003Fu, &k);
        if (e != ERROR_SUCCESS) {
            return e;
        }
    } else {
        e = real_key(h, &k);
        if (e != ERROR_SUCCESS) {
            return e;
        }
    }
    ustr_of(&us, name);
    e = err_of(NtRenameKey(k, &us));
    if (sub && sub[0]) {
        (void)RegCloseKey(k);
    }
    return e;
}

/* RegSetKeyValue: a value of a subkey, the subkey made if it is missing. */
LONG32 RegSetKeyValueW(uint64_t h, const uint16_t *sub, const uint16_t *name, uint32_t type, const void *data,
                       uint32_t len) {
    uint64_t k = h;
    LONG32 e;

    if (sub && sub[0]) {
        e = RegCreateKeyExW(h, sub, 0, 0, 0, 0x00020006u, 0, &k, 0);
        if (e != ERROR_SUCCESS) {
            return e;
        }
    }
    e = RegSetValueExW(k, name, 0, type, (const uint8_t *)data, len);
    if (k != h) {
        (void)RegCloseKey(k);
    }
    return e;
}

LONG32 RegSetKeyValueA(uint64_t h, const char *sub, const char *name, uint32_t type, const void *data,
                       uint32_t len) {
    uint64_t k = h;
    LONG32 e;

    if (sub && sub[0]) {
        e = RegCreateKeyExA(h, sub, 0, 0, 0, 0x00020006u, 0, &k, 0);
        if (e != ERROR_SUCCESS) {
            return e;
        }
    }
    e = RegSetValueExA(k, name, 0, type, (const uint8_t *)data, len);
    if (k != h) {
        (void)RegCloseKey(k);
    }
    return e;
}

LONG32 RegDeleteKeyValueW(uint64_t h, const uint16_t *sub, const uint16_t *name) {
    uint64_t k = h;
    LONG32 e;

    if (sub && sub[0]) {
        e = RegOpenKeyExW(h, sub, 0, 0x00020006u, &k);
        if (e != ERROR_SUCCESS) {
            return e;
        }
    }
    e = RegDeleteValueW(k, name);
    if (k != h) {
        (void)RegCloseKey(k);
    }
    return e;
}

LONG32 RegDeleteKeyValueA(uint64_t h, const char *sub, const char *name) {
    uint64_t k = h;
    LONG32 e;

    if (sub && sub[0]) {
        e = RegOpenKeyExA(h, sub, 0, 0x00020006u, &k);
        if (e != ERROR_SUCCESS) {
            return e;
        }
    }
    e = RegDeleteValueA(k, name);
    if (k != h) {
        (void)RegCloseKey(k);
    }
    return e;
}

/* RegCopyTree: every value of `src` (or its subkey) and every subkey under it,
 * into `dst`, recursively. */
static LONG32 copy_tree(uint64_t src, uint64_t dst) {
    uint16_t name[257];
    uint32_t i, n;
    uint8_t *rec;
    LONG32 e;

    for (i = 0;; i++) {
        e = enum_full(src, i, &rec);
        if (e == ERROR_NO_MORE_ITEMS) {
            break;
        }
        if (e != ERROR_SUCCESS) {
            return e;
        }
        {
            vibeos_ustring_t us;

            us.Length = (uint16_t)*(const uint32_t *)(rec + 16);
            us.MaximumLength = us.Length;
            us.Buffer = (vibeos_wchar_t *)(void *)(rec + 20);
            e = err_of(NtSetValueKey(dst, &us, 0, *(const uint32_t *)(rec + 4), rec + *(const uint32_t *)(rec + 8),
                                     *(const uint32_t *)(rec + 12)));
        }
        RtlFreeHeap(heap(), 0, rec);
        if (e != ERROR_SUCCESS) {
            return e;
        }
    }
    for (i = 0;; i++) {
        uint64_t s2, d2;

        n = 257u;
        e = RegEnumKeyExW(src, i, name, &n, 0, 0, 0, 0);
        if (e == ERROR_NO_MORE_ITEMS) {
            return ERROR_SUCCESS;
        }
        if (e != ERROR_SUCCESS) {
            return e;
        }
        e = RegOpenKeyExW(src, name, 0, 0x00020019u, &s2);
        if (e != ERROR_SUCCESS) {
            return e;
        }
        e = RegCreateKeyExW(dst, name, 0, 0, 0, 0x000F003Fu, 0, &d2, 0);
        if (e == ERROR_SUCCESS) {
            e = copy_tree(s2, d2);
            (void)RegCloseKey(d2);
        }
        (void)RegCloseKey(s2);
        if (e != ERROR_SUCCESS) {
            return e;
        }
    }
}

LONG32 RegCopyTreeW(uint64_t src, const uint16_t *sub, uint64_t dst) {
    uint64_t s = 0, d = 0;
    LONG32 e = real_key(dst, &d);

    if (e != ERROR_SUCCESS) {
        return e;
    }
    if (sub && sub[0]) {
        e = RegOpenKeyExW(src, sub, 0, 0x00020019u, &s);
        if (e != ERROR_SUCCESS) {
            return e;
        }
        e = copy_tree(s, d);
        (void)RegCloseKey(s);
        return e;
    }
    e = real_key(src, &s);
    return e != ERROR_SUCCESS ? e : copy_tree(s, d);
}

LONG32 RegCopyTreeA(uint64_t src, const char *sub, uint64_t dst) {
    uint16_t w[NAME_UNITS];
    uint32_t n;

    if (!sub) {
        return RegCopyTreeW(src, 0, dst);
    }
    n = alen(sub);
    if (n >= NAME_UNITS) {
        return ERROR_INVALID_PARAMETER;
    }
    w[widen_n(sub, n, w)] = 0;
    return RegCopyTreeW(src, w, dst);
}

/* The local machine's registry only: another machine's is a network path
 * nothing here can reach. */
LONG32 RegConnectRegistryW(const uint16_t *machine, uint64_t h, uint64_t *out) {
    if (!out) {
        return ERROR_INVALID_PARAMETER;
    }
    if (machine && machine[0]) {
        return ERROR_BAD_NETPATH;
    }
    return RegOpenKeyExW(h, 0, 0, MAXIMUM_ALLOWED, out);
}

LONG32 RegConnectRegistryA(const char *machine, uint64_t h, uint64_t *out) {
    if (!out) {
        return ERROR_INVALID_PARAMETER;
    }
    if (machine && machine[0]) {
        return ERROR_BAD_NETPATH;
    }
    return RegOpenKeyExW(h, 0, 0, MAXIMUM_ALLOWED, out);
}

/* RegLoadMUIString: a value's string as it is stored; one naming a resource
 * ("@file,-id") needs resource loading, which is not here (WO-095). */
LONG32 RegLoadMUIStringW(uint64_t h, const uint16_t *value, uint16_t *buf, uint32_t size, uint32_t *needed,
                         uint32_t flags, const uint16_t *dir) {
    uint32_t type = 0, n = size;
    LONG32 e;

    (void)dir;
    if (flags & ~1u /* REG_MUI_STRING_TRUNCATE */) {
        return ERROR_INVALID_PARAMETER;
    }
    e = RegQueryValueExW(h, value, 0, &type, (uint8_t *)buf, &n);
    if (needed) {
        *needed = n;
    }
    if (e != ERROR_SUCCESS) {
        return e;
    }
    if (type != REG_SZ && type != REG_EXPAND_SZ) {
        return ERROR_INVALID_DATA;
    }
    if (buf && n >= 2u && buf[0] == '@') {
        return ERROR_CALL_NOT_IMPLEMENTED;
    }
    return ERROR_SUCCESS;
}

LONG32 RegLoadMUIStringA(uint64_t h, const char *value, char *buf, uint32_t size, uint32_t *needed,
                         uint32_t flags, const char *dir) {
    (void)h;
    (void)value;
    (void)buf;
    (void)size;
    (void)needed;
    (void)flags;
    (void)dir;
    return ERROR_CALL_NOT_IMPLEMENTED;   /* Windows has never implemented the ANSI form either */
}

/* What needs the registry's service side that is not written: change
 * notification (NtNotifyChangeKey), saving and loading hives by hand, and
 * keys' security (WO-089, WO-092, WO-096). Refused by name. */
LONG32 RegNotifyChangeKeyValue(uint64_t h, BOOL32 subtree, uint32_t filter, uint64_t event, BOOL32 async) {
    (void)h;
    (void)subtree;
    (void)filter;
    (void)event;
    (void)async;
    return ERROR_CALL_NOT_IMPLEMENTED;
}

LONG32 RegSaveKeyA(uint64_t h, const char *file, const void *sa) {
    (void)h;
    (void)file;
    (void)sa;
    return ERROR_CALL_NOT_IMPLEMENTED;
}

LONG32 RegLoadKeyA(uint64_t h, const char *sub, const char *file) {
    (void)h;
    (void)sub;
    (void)file;
    return ERROR_CALL_NOT_IMPLEMENTED;
}

LONG32 RegUnLoadKeyA(uint64_t h, const char *sub) {
    (void)h;
    (void)sub;
    return ERROR_CALL_NOT_IMPLEMENTED;
}

LONG32 RegLoadAppKeyA(const char *file, uint64_t *out, uint32_t sam, uint32_t options, uint32_t reserved) {
    (void)file;
    (void)out;
    (void)sam;
    (void)options;
    (void)reserved;
    return ERROR_CALL_NOT_IMPLEMENTED;
}

LONG32 RegSetKeySecurity(uint64_t h, uint32_t info, const void *sd) {
    (void)h;
    (void)info;
    (void)sd;
    return ERROR_CALL_NOT_IMPLEMENTED;
}

/* ---- security: the data structures, and no tokens ------------------------------------------
 * A SID and a security descriptor are plain structures, built and taken apart
 * as Windows lays them out. A token is an object of the kernel's, which the
 * security plan brings (WO-086): until then a process has none to open, and
 * the calls that need one fail with ERROR_CALL_NOT_IMPLEMENTED, as Windows
 * answers what a system does not support. */

static void set_last_error(uint32_t e) {
    __asm__ __volatile__("movl %0, %%gs:%c1" : : "r"(e), "i"(VIBEOS_TEB_LAST_ERROR) : "memory");
}

BOOL32 AllocateAndInitializeSid(const uint8_t authority[6], uint8_t count, uint32_t s0, uint32_t s1, uint32_t s2,
                                uint32_t s3, uint32_t s4, uint32_t s5, uint32_t s6, uint32_t s7, void **sid) {
    const uint32_t sub[8] = { s0, s1, s2, s3, s4, s5, s6, s7 };
    uint8_t *p;
    uint32_t i;

    if (count > 8u || !sid || !authority) {
        set_last_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    p = (uint8_t *)RtlAllocateHeap(heap(), 0, 8u + 4u * count);
    if (!p) {
        set_last_error(ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    p[0] = 1;   /* SID_REVISION */
    p[1] = count;
    for (i = 0; i < 6u; i++) {
        p[2u + i] = authority[i];
    }
    for (i = 0; i < count; i++) {
        *(uint32_t *)(void *)(p + 8u + 4u * i) = sub[i];
    }
    *sid = p;
    return 1;
}

void *FreeSid(void *sid) {
    if (sid) {
        RtlFreeHeap(heap(), 0, sid);
    }
    return 0;
}

/* SECURITY_DESCRIPTOR, absolute: Revision, Sbz1, Control, then Owner,
 * Group, Sacl and Dacl, eight bytes each. */
BOOL32 InitializeSecurityDescriptor(void *sd, uint32_t revision) {
    uint8_t *p = (uint8_t *)sd;
    uint32_t i;

    if (revision != 1u) {
        set_last_error(1338u);   /* ERROR_INVALID_SECURITY_DESCR */
        return 0;
    }
    for (i = 0; i < 40u; i++) {
        p[i] = 0;
    }
    p[0] = 1;
    return 1;
}

BOOL32 SetSecurityDescriptorDacl(void *sd, BOOL32 present, void *dacl, BOOL32 defaulted) {
    uint8_t *p = (uint8_t *)sd;
    uint16_t control = *(uint16_t *)(void *)(p + 2);

    if (p[0] != 1u) {
        set_last_error(1305u);   /* ERROR_UNKNOWN_REVISION */
        return 0;
    }
    control = (uint16_t)(control & ~(0x0004u | 0x0008u));   /* SE_DACL_PRESENT, SE_DACL_DEFAULTED */
    if (present) {
        control |= 0x0004u;
        if (defaulted) {
            control |= 0x0008u;
        }
    }
    *(uint16_t *)(void *)(p + 2) = control;
    *(void **)(void *)(p + 32) = present ? dacl : 0;
    return 1;
}

/* The privileges' LUIDs, which are fixed on every Windows (winnt.h's SE_*). */
BOOL32 LookupPrivilegeValueA(const char *system, const char *name, uint64_t *luid) {
    static const char *const names[] = {
        0, 0, "SeCreateTokenPrivilege", "SeAssignPrimaryTokenPrivilege", "SeLockMemoryPrivilege",
        "SeIncreaseQuotaPrivilege", "SeMachineAccountPrivilege", "SeTcbPrivilege", "SeSecurityPrivilege",
        "SeTakeOwnershipPrivilege", "SeLoadDriverPrivilege", "SeSystemProfilePrivilege", "SeSystemtimePrivilege",
        "SeProfileSingleProcessPrivilege", "SeIncreaseBasePriorityPrivilege", "SeCreatePagefilePrivilege",
        "SeCreatePermanentPrivilege", "SeBackupPrivilege", "SeRestorePrivilege", "SeShutdownPrivilege",
        "SeDebugPrivilege", "SeAuditPrivilege", "SeSystemEnvironmentPrivilege", "SeChangeNotifyPrivilege",
        "SeRemoteShutdownPrivilege", "SeUndockPrivilege", "SeSyncAgentPrivilege", "SeEnableDelegationPrivilege",
        "SeManageVolumePrivilege", "SeImpersonatePrivilege", "SeCreateGlobalPrivilege",
    };
    uint32_t i;

    if (system && system[0]) {
        set_last_error(ERROR_BAD_NETPATH);
        return 0;
    }
    for (i = 2; name && i < sizeof(names) / sizeof(names[0]); i++) {
        const char *a = names[i], *b = name;

        while (*a && (*a | 0x20) == (*b | 0x20)) {
            a++;
            b++;
        }
        if (!*a && !*b) {
            *luid = i;
            return 1;
        }
    }
    set_last_error(1313u);   /* ERROR_NO_SUCH_PRIVILEGE */
    return 0;
}

BOOL32 OpenProcessToken(uint64_t process, uint32_t access, uint64_t *token) {
    (void)process;
    (void)access;
    if (token) {
        *token = 0;
    }
    set_last_error(ERROR_CALL_NOT_IMPLEMENTED);
    return 0;
}

BOOL32 AdjustTokenPrivileges(uint64_t token, BOOL32 disable_all, const void *state, uint32_t len, void *prev,
                             uint32_t *ret) {
    (void)token;
    (void)disable_all;
    (void)state;
    (void)len;
    (void)prev;
    (void)ret;
    set_last_error(ERROR_INVALID_HANDLE);   /* no token was ever handed out */
    return 0;
}

int vibeos_advapi32_main(uint64_t instance, uint32_t reason, void *reserved) {
    (void)instance;
    (void)reason;
    (void)reserved;
    return 1;
}
