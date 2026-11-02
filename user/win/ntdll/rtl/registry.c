/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The registry's Rtl helpers (docs/abi/windows.md W8 step 3): the ones Wine's
 * ntdll:reg asks for, over the Nt*Key services.
 *
 * In the DLL alone, as platform_win.c is: everything here is a system call,
 * and the host build of the Rtl runtime has none. The current user is
 * .DEFAULT for every process until there are tokens (WO-086). Expansion of
 * REG_EXPAND_SZ for RtlQueryRegistryValues is not done here: the string is
 * handed over as stored (WO-087). */

#include "rtl_internal.h"

#define RTL_REGISTRY_ABSOLUTE   0u
#define RTL_REGISTRY_SERVICES   1u
#define RTL_REGISTRY_CONTROL    2u
#define RTL_REGISTRY_WINDOWS_NT 3u
#define RTL_REGISTRY_DEVICEMAP  4u
#define RTL_REGISTRY_USER       5u
#define RTL_REGISTRY_MAXIMUM    6u
#define RTL_REGISTRY_HANDLE     0x40000000u
#define RTL_REGISTRY_OPTIONAL   0x80000000u

#define RTL_QUERY_REGISTRY_SUBKEY   0x01u
#define RTL_QUERY_REGISTRY_TOPKEY   0x02u
#define RTL_QUERY_REGISTRY_REQUIRED 0x04u
#define RTL_QUERY_REGISTRY_NOVALUE  0x08u
#define RTL_QUERY_REGISTRY_NOEXPAND 0x10u
#define RTL_QUERY_REGISTRY_DIRECT   0x20u
#define RTL_QUERY_REGISTRY_DELETE   0x40u

#define REG_NONE     0u
#define REG_SZ       1u
#define REG_EXPAND_SZ 2u
#define REG_MULTI_SZ 7u

#define KEY_READ  0x00020019u
#define KEY_WRITE 0x00020006u

static const char g_user_path[] = "\\Registry\\User\\.Default";

/* The roots RelativeTo names, each with its separator. */
static const char *const g_roots[RTL_REGISTRY_MAXIMUM] = {
    "",
    "\\Registry\\Machine\\System\\CurrentControlSet\\Services\\",
    "\\Registry\\Machine\\System\\CurrentControlSet\\Control\\",
    "\\Registry\\Machine\\Software\\Microsoft\\Windows NT\\CurrentVersion\\",
    "\\Registry\\Machine\\Hardware\\DeviceMap\\",
    "\\Registry\\User\\.Default\\",
};

static NTSTATUS_T open_key(uint64_t *h, uint32_t access, uint64_t root, const vibeos_wchar_t *path,
                           uint32_t units) {
    vibeos_ustring_t us;
    uint64_t oa[6];

    us.Length = (uint16_t)(units * 2u);
    us.MaximumLength = us.Length;
    us.Buffer = (vibeos_wchar_t *)(uintptr_t)path;
    oa[0] = 48u;
    oa[1] = root;
    oa[2] = (uint64_t)(uintptr_t)&us;
    oa[3] = 0x40u;   /* OBJ_CASE_INSENSITIVE */
    oa[4] = oa[5] = 0;
    return NtOpenKey(h, access, oa);
}

/* The key RelativeTo and Path name, and whether the caller must close it. */
static NTSTATUS_T rtl_key(uint32_t relative, const vibeos_wchar_t *path, uint32_t access, uint64_t *h,
                          int *close) {
    vibeos_wchar_t full[512];
    uint32_t n = 0, i, rel = relative & ~RTL_REGISTRY_OPTIONAL;

    *close = 0;
    if (rel & RTL_REGISTRY_HANDLE) {
        *h = (uint64_t)(uintptr_t)path;
        return VIBEOS_STATUS_SUCCESS;
    }
    if (rel >= RTL_REGISTRY_MAXIMUM) {
        return VIBEOS_STATUS_INVALID_PARAMETER;
    }
    for (i = 0; g_roots[rel][i] && n < 511u; i++) {
        full[n++] = (vibeos_wchar_t)(unsigned char)g_roots[rel][i];
    }
    for (i = 0; path && path[i] && n < 511u; i++) {
        full[n++] = path[i];
    }
    *close = 1;
    return open_key(h, access, 0, full, n);
}

vibeos_ntstatus_t RtlFormatCurrentUserKeyPath(vibeos_ustring_t *out) {
    vibeos_wchar_t *buf;
    uint32_t i, n = sizeof(g_user_path) - 1u;

    buf = (vibeos_wchar_t *)vibeos_rtl_alloc((n + 1u) * 2u);
    if (!buf) {
        return VIBEOS_STATUS_NO_MEMORY;
    }
    for (i = 0; i <= n; i++) {
        buf[i] = (vibeos_wchar_t)(unsigned char)g_user_path[i];
    }
    out->Length = (uint16_t)(n * 2u);
    out->MaximumLength = (uint16_t)((n + 1u) * 2u);
    out->Buffer = buf;
    return VIBEOS_STATUS_SUCCESS;
}

vibeos_ntstatus_t RtlOpenCurrentUser(uint32_t access, uint64_t *h) {
    vibeos_wchar_t w[sizeof(g_user_path)];
    uint32_t i;

    for (i = 0; g_user_path[i]; i++) {
        w[i] = (vibeos_wchar_t)(unsigned char)g_user_path[i];
    }
    return (vibeos_ntstatus_t)open_key(h, access, 0, w, i);
}

vibeos_ntstatus_t RtlCheckRegistryKey(uint32_t relative, const vibeos_wchar_t *path) {
    uint64_t h;
    int close;
    NTSTATUS_T st;

    if ((relative & RTL_REGISTRY_HANDLE) == 0u && !path) {
        return VIBEOS_STATUS_OBJECT_PATH_SYNTAX_BAD;
    }
    st = rtl_key(relative, path, KEY_READ, &h, &close);
    if (st == 0 && close) {
        (void)NtClose(h);
    }
    return (vibeos_ntstatus_t)st;
}

/* RTL_QUERY_REGISTRY_TABLE, as x64 lays it out. */
typedef vibeos_ntstatus_t (*rtl_query_routine_t)(vibeos_wchar_t *name, uint32_t type, void *data,
                                                 uint32_t len, void *context, void *entry);
typedef struct {
    rtl_query_routine_t routine;
    uint32_t flags;
    vibeos_wchar_t *name;
    void *entry;
    uint32_t default_type;
    void *default_data;
    uint32_t default_len;
} rtl_query_entry_t;

/* A value handed to DIRECT's EntryContext, as Windows does: a string into the
 * UNICODE_STRING it points at (made when its buffer is null), anything that
 * fits a ULONG into it, anything larger into a buffer whose first LONG is its
 * size, negated. */
static vibeos_ntstatus_t direct(void *entry, uint32_t type, const void *data, uint32_t len) {
    if (type == REG_SZ || type == REG_EXPAND_SZ || type == REG_MULTI_SZ) {
        vibeos_ustring_t *us = (vibeos_ustring_t *)entry;
        uint32_t n = len;

        if (n >= 2u && ((const vibeos_wchar_t *)data)[n / 2u - 1u] == 0) {
            n -= 2u;   /* the terminator is not part of Length */
        }
        if (!us->Buffer) {
            us->Buffer = (vibeos_wchar_t *)vibeos_rtl_alloc(n + 2u);
            if (!us->Buffer) {
                return VIBEOS_STATUS_NO_MEMORY;
            }
            us->MaximumLength = (uint16_t)(n + 2u);
        } else if (n + 2u > us->MaximumLength) {
            return VIBEOS_STATUS_BUFFER_TOO_SMALL;
        }
        memcpy(us->Buffer, data, n);
        us->Buffer[n / 2u] = 0;
        us->Length = (uint16_t)n;
        return VIBEOS_STATUS_SUCCESS;
    }
    if (len <= 4u) {
        memcpy(entry, data, len);
        return VIBEOS_STATUS_SUCCESS;
    }
    {
        int32_t cap = -*(const int32_t *)entry;

        if (cap < (int32_t)len) {
            return VIBEOS_STATUS_BUFFER_TOO_SMALL;
        }
        memcpy(entry, data, len);
    }
    return VIBEOS_STATUS_SUCCESS;
}

/* One value to an entry: its routine, or DIRECT's EntryContext. A MULTI_SZ
 * goes to a routine one string at a time unless NOEXPAND asks for it whole. */
static vibeos_ntstatus_t deliver(const rtl_query_entry_t *e, vibeos_wchar_t *name, uint32_t type,
                                 void *data, uint32_t len, void *context) {
    if (e->flags & RTL_QUERY_REGISTRY_DIRECT) {
        return direct(e->entry, type, data, len);
    }
    if (type == REG_MULTI_SZ && !(e->flags & RTL_QUERY_REGISTRY_NOEXPAND)) {
        vibeos_wchar_t *s = (vibeos_wchar_t *)data;
        uint32_t at = 0, units = len / 2u;
        vibeos_ntstatus_t st = VIBEOS_STATUS_SUCCESS;

        while (at < units && s[at]) {
            uint32_t end = at;

            while (end < units && s[end]) {
                end++;
            }
            st = e->routine(name, REG_SZ, s + at, (end - at + 1u) * 2u, context, e->entry);
            if (st != VIBEOS_STATUS_SUCCESS) {
                return st;
            }
            at = end + 1u;
        }
        return st;
    }
    return e->routine(name, type, data, len, context, e->entry);
}

/* A value's KeyValueFullInformation, in a buffer the caller frees. */
static NTSTATUS_T full_value(uint64_t h, const vibeos_ustring_t *name, uint32_t index, int by_index,
                             uint8_t **out) {
    uint32_t cap = 512u, got = 0;
    NTSTATUS_T st;

    for (;;) {
        *out = (uint8_t *)vibeos_rtl_alloc(cap);
        if (!*out) {
            return VIBEOS_STATUS_NO_MEMORY;
        }
        st = by_index ? NtEnumerateValueKey(h, index, 1u, *out, cap, &got)
                      : NtQueryValueKey(h, name, 1u, *out, cap, &got);
        if (st != (NTSTATUS_T)VIBEOS_STATUS_BUFFER_OVERFLOW && st != (NTSTATUS_T)VIBEOS_STATUS_BUFFER_TOO_SMALL) {
            break;
        }
        vibeos_rtl_free(*out);
        cap = got + 16u;
    }
    if (st != 0) {
        vibeos_rtl_free(*out);
        *out = 0;
    }
    return st;
}

vibeos_ntstatus_t RtlQueryRegistryValues(uint32_t relative, const vibeos_wchar_t *path, void *table_v,
                                         void *context, void *environment) {
    rtl_query_entry_t *e = (rtl_query_entry_t *)table_v;
    uint64_t top, cur;
    int close;
    NTSTATUS_T st;

    (void)environment;
    if (!e) {
        return VIBEOS_STATUS_INVALID_PARAMETER;
    }
    st = rtl_key(relative, path, KEY_READ | KEY_WRITE, &top, &close);
    if (st != 0) {
        st = rtl_key(relative, path, KEY_READ, &top, &close);
    }
    if (st != 0) {
        return (vibeos_ntstatus_t)st;
    }
    cur = top;
    for (; st == 0 && (e->routine || e->name); e++) {
        if (e->flags & (RTL_QUERY_REGISTRY_SUBKEY | RTL_QUERY_REGISTRY_TOPKEY)) {
            if (cur != top) {
                (void)NtClose(cur);
            }
            cur = top;
            if ((e->flags & RTL_QUERY_REGISTRY_SUBKEY) && e->name) {
                st = open_key(&cur, KEY_READ | KEY_WRITE, top, e->name, (uint32_t)rtl_wcslen(e->name));
                if (st != 0) {
                    st = open_key(&cur, KEY_READ, top, e->name, (uint32_t)rtl_wcslen(e->name));
                }
                if (st != 0) {
                    cur = top;
                    if (!(e->flags & RTL_QUERY_REGISTRY_REQUIRED)) {
                        st = 0;
                    }
                    break;
                }
            }
            if (!e->routine) {
                continue;
            }
        }
        if (!e->name && !(e->flags & RTL_QUERY_REGISTRY_SUBKEY)) {
            /* Every value of the key, in order, to the routine. */
            uint32_t i;
            uint8_t *rec;

            if (e->flags & RTL_QUERY_REGISTRY_DIRECT) {
                st = VIBEOS_STATUS_INVALID_PARAMETER;
                break;
            }
            for (i = 0;; i++) {
                NTSTATUS_T q = full_value(cur, 0, i, 1, &rec);

                if (q == (NTSTATUS_T)0x8000001Au) {
                    break;
                }
                if (q != 0) {
                    st = q;
                    break;
                }
                st = (NTSTATUS_T)deliver(e, (vibeos_wchar_t *)(void *)(rec + 20), *(const uint32_t *)(rec + 4),
                                         rec + *(const uint32_t *)(rec + 8), *(const uint32_t *)(rec + 12),
                                         context);
                vibeos_rtl_free(rec);
                if (st != 0) {
                    break;
                }
            }
            continue;
        }
        if (e->name) {
            vibeos_ustring_t us;
            uint8_t *rec = 0;
            NTSTATUS_T q;

            RtlInitUnicodeString(&us, e->name);
            q = full_value(cur, &us, 0, 0, &rec);
            if (q == (NTSTATUS_T)VIBEOS_STATUS_OBJECT_NAME_NOT_FOUND) {
                if (e->flags & RTL_QUERY_REGISTRY_REQUIRED) {
                    st = q;
                    break;
                }
                if (e->default_type != REG_NONE) {
                    uint32_t dl = e->default_len;

                    if (dl == 0u && (e->default_type == REG_SZ || e->default_type == REG_EXPAND_SZ) &&
                        e->default_data) {
                        dl = (uint32_t)(rtl_wcslen((const vibeos_wchar_t *)e->default_data) + 1u) * 2u;
                    }
                    st = (NTSTATUS_T)deliver(e, e->name, e->default_type, e->default_data, dl, context);
                }
                continue;
            }
            if (q != 0) {
                st = q;
                break;
            }
            st = (NTSTATUS_T)deliver(e, e->name, *(const uint32_t *)(rec + 4), rec + *(const uint32_t *)(rec + 8),
                                     *(const uint32_t *)(rec + 12), context);
            vibeos_rtl_free(rec);
            if (st == 0 && (e->flags & RTL_QUERY_REGISTRY_DELETE)) {
                (void)NtDeleteValueKey(cur, &us);
            }
        }
    }
    if (cur != top) {
        (void)NtClose(cur);
    }
    if (close) {
        (void)NtClose(top);
    }
    return (vibeos_ntstatus_t)st;
}

vibeos_ntstatus_t RtlWriteRegistryValue(uint32_t relative, const vibeos_wchar_t *path,
                                        const vibeos_wchar_t *name, uint32_t type, const void *data,
                                        uint32_t len) {
    vibeos_ustring_t us;
    uint64_t h;
    int close;
    NTSTATUS_T st = rtl_key(relative, path, KEY_WRITE, &h, &close);

    if (st != 0) {
        return (vibeos_ntstatus_t)st;
    }
    RtlInitUnicodeString(&us, name);
    st = NtSetValueKey(h, &us, 0, type, data, len);
    if (close) {
        (void)NtClose(h);
    }
    return (vibeos_ntstatus_t)st;
}

vibeos_ntstatus_t RtlDeleteRegistryValue(uint32_t relative, const vibeos_wchar_t *path,
                                         const vibeos_wchar_t *name) {
    vibeos_ustring_t us;
    uint64_t h;
    int close;
    NTSTATUS_T st = rtl_key(relative, path, KEY_WRITE, &h, &close);

    if (st != 0) {
        return (vibeos_ntstatus_t)st;
    }
    RtlInitUnicodeString(&us, name);
    st = NtDeleteValueKey(h, &us);
    if (close) {
        (void)NtClose(h);
    }
    return (vibeos_ntstatus_t)st;
}

/* Every key of the path made that is missing, from the root RelativeTo names. */
vibeos_ntstatus_t RtlCreateRegistryKey(uint32_t relative, const vibeos_wchar_t *path) {
    uint32_t rel = relative & ~RTL_REGISTRY_OPTIONAL, n = 0, i, at;
    vibeos_wchar_t full[512];
    uint64_t cur = 0;
    NTSTATUS_T st = 0;

    if ((rel & RTL_REGISTRY_HANDLE) || rel >= RTL_REGISTRY_MAXIMUM || (rel == RTL_REGISTRY_ABSOLUTE && !path)) {
        return VIBEOS_STATUS_INVALID_PARAMETER;
    }
    for (i = 0; g_roots[rel][i] && n < 511u; i++) {
        full[n++] = (vibeos_wchar_t)(unsigned char)g_roots[rel][i];
    }
    for (i = 0; path && path[i] && n < 511u; i++) {
        full[n++] = path[i];
    }
    /* Past "\Registry\<hive>", component by component under the last opened. */
    at = 0;
    for (i = 0; i < 3u && at < n; i++) {
        at++;
        while (at < n && full[at] != '\\') {
            at++;
        }
    }
    st = open_key(&cur, KEY_READ | KEY_WRITE, 0, full, at);
    while (st == 0 && at < n) {
        uint32_t start = at + 1u, end = start;
        uint64_t next;
        vibeos_ustring_t us;
        uint64_t oa[6];

        while (end < n && full[end] != '\\') {
            end++;
        }
        us.Length = (uint16_t)((end - start) * 2u);
        us.MaximumLength = us.Length;
        us.Buffer = full + start;
        oa[0] = 48u;
        oa[1] = cur;
        oa[2] = (uint64_t)(uintptr_t)&us;
        oa[3] = 0x40u;
        oa[4] = oa[5] = 0;
        st = end > start ? NtCreateKey(&next, KEY_READ | KEY_WRITE, oa, 0, 0, 0, 0) : 0;
        if (end > start) {
            (void)NtClose(cur);
            cur = next;
        }
        at = end;
    }
    if (cur) {
        (void)NtClose(cur);
    }
    return (vibeos_ntstatus_t)st;
}

/* The default value's type and data, as Wine's ntdll:reg asks for it: the
 * data's size in *len, written when `data` has room for it. */
vibeos_ntstatus_t RtlpNtQueryValueKey(uint64_t key, uint32_t *type, void *data, uint32_t *len,
                                      uint32_t unused) {
    vibeos_ustring_t empty = { 0, 0, 0 };
    uint8_t *rec;
    NTSTATUS_T st;

    (void)unused;
    st = full_value(key, &empty, 0, 0, &rec);
    if (st != 0) {
        return (vibeos_ntstatus_t)st;
    }
    if (type) {
        *type = *(const uint32_t *)(rec + 4);
    }
    if (len) {
        uint32_t dl = *(const uint32_t *)(rec + 12);

        if (data && *len >= dl) {
            memcpy(data, rec + *(const uint32_t *)(rec + 8), dl);
        } else if (data) {
            st = VIBEOS_STATUS_BUFFER_OVERFLOW;
        }
        *len = dl;
    }
    vibeos_rtl_free(rec);
    return (vibeos_ntstatus_t)st;
}
