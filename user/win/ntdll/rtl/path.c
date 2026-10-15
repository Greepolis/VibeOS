/* DOS paths to NT paths (docs/abi/windows.md W2).
 *
 * RtlDosPathNameToNtPathName_U is what every Win32 file call does first: it makes
 * the path absolute, normalises it, and puts it under \??\, the directory of the
 * object namespace where drive letters live. The forms handled, with what each
 * becomes:
 *
 *   C:\a\b            \??\C:\a\b
 *   C:b               the current directory of C: if that is the current drive,
 *                     else C:\b                          (then as above)
 *   \a                \??\ + the current drive + \a
 *   a\b               \??\ + the current directory + \a\b
 *   \\server\share\a  \??\UNC\server\share\a
 *   \\.\NAME          \??\NAME
 *   \\?\anything      \??\anything, untouched: that prefix means "do not interpret"
 *   \??\anything      unchanged
 *
 * '/' is a separator wherever the path is interpreted; empty components collapse,
 * "." goes, ".." removes the component before it and stops at the root. What is
 * not done: trailing dots and spaces in the last component are kept (Windows
 * strips them), and reserved device names (NUL, COM1...) are not recognised. */

#include "rtl_internal.h"

#define MAX_BYTES 0xFFFCu

static const vibeos_wchar_t k_nt_prefix[4] = {'\\', '?', '?', '\\'};
static const vibeos_wchar_t k_unc_prefix[8] = {'\\', '?', '?', '\\', 'U', 'N', 'C', '\\'};

static int is_sep(vibeos_wchar_t c) { return c == '\\' || c == '/'; }

static int is_alpha(vibeos_wchar_t c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

/* Length of the root of an absolute path: "C:" (the separator after it is a
 * component boundary like any other), "\\server\share" or "\\." */
static size_t root_len(const vibeos_wchar_t *p, size_t n) {
    size_t i;

    if (n >= 2 && is_alpha(p[0]) && p[1] == ':') {
        return 2;
    }
    if (n >= 3 && p[0] == '\\' && p[1] == '\\' && p[2] == '.') {
        return 3;
    }
    if (n >= 2 && p[0] == '\\' && p[1] == '\\') {
        i = 2;
        while (i < n && p[i] != '\\') {
            i++;                             /* the server */
        }
        if (i < n) {
            i++;
            while (i < n && p[i] != '\\') {
                i++;                         /* the share */
            }
        }
        return i;
    }
    return 0;
}

/* Normalise the absolute path in[0..n) (separators already '\') into out, which
 * is at least n + 2 units. Returns the length written. Components are never
 * counted: ".." walks back to the previous separator in what has been written,
 * so there is no depth limit to exceed. */
static size_t normalise(const vibeos_wchar_t *in, size_t n, vibeos_wchar_t *out) {
    size_t root = root_len(in, n), o = root, i = root;
    int trailing = n > 0 && in[n - 1] == '\\';

    memcpy(out, in, root * sizeof(vibeos_wchar_t));
    while (i < n) {
        size_t s, len;

        while (i < n && in[i] == '\\') {
            i++;
        }
        s = i;
        while (i < n && in[i] != '\\') {
            i++;
        }
        len = i - s;
        if (len == 0 || (len == 1 && in[s] == '.')) {
            continue;
        }
        if (len == 2 && in[s] == '.' && in[s + 1] == '.') {
            while (o > root && out[o - 1] != '\\') {
                o--;
            }
            if (o > root) {
                o--;                         /* the separator in front of it */
            }
            continue;
        }
        out[o++] = '\\';
        memcpy(out + o, in + s, len * sizeof(vibeos_wchar_t));
        o += len;
    }
    if (o == root) {
        /* A drive's root always has its separator: "C:" alone would mean the
         * current directory of C:, "C:\" is the root. */
        if (root == 2 || trailing) {
            out[o++] = '\\';
        }
    } else if (trailing) {
        out[o++] = '\\';
    }
    return o;
}

/* The file part of a finished name: after the last separator, if anything is. */
static vibeos_wchar_t *file_part_of(vibeos_wchar_t *res, size_t len) {
    size_t k = len;

    while (k > 4 && res[k - 1] != '\\') {
        k--;
    }
    return (k > 4 && k < len) ? res + k : 0;
}

static vibeos_boolean_t finish(vibeos_ustring_t *nt_name, vibeos_wchar_t **file_part,
                               vibeos_wchar_t *res, size_t len) {
    if (len * 2u > MAX_BYTES) {
        vibeos_rtl_free(res);
        return 0;
    }
    res[len] = 0;
    nt_name->Length = (uint16_t)(len * 2u);
    nt_name->MaximumLength = (uint16_t)(len * 2u + 2u);
    nt_name->Buffer = res;
    if (file_part) {
        *file_part = file_part_of(res, len);
    }
    return 1;
}

vibeos_boolean_t RtlDosPathNameToNtPathName_U(const vibeos_wchar_t *dos_name, vibeos_ustring_t *nt_name,
                                              vibeos_wchar_t **file_part,
                                              vibeos_rtl_relative_name_u_t *relative) {
    static const vibeos_wchar_t root_c[3] = {'C', ':', '\\'};
    uint8_t *pp = vibeos_rtl_process_params();
    const vibeos_wchar_t *cwd = root_c;      /* a process with no directory is on C:\ */
    size_t cwd_len = 3, n, cap, abs_len = 0, out_len, i = 0, r;
    vibeos_wchar_t *abs, *norm, *res;

    if (file_part) {
        *file_part = 0;
    }
    if (relative) {
        memset(relative, 0, sizeof(*relative));
    }
    if (!dos_name || !dos_name[0] || !nt_name) {
        return 0;
    }
    n = rtl_wcslen(dos_name);
    if (n * 2u > MAX_BYTES) {
        return 0;
    }

    /* \\?\ and \??\ : the rest is the NT name as it stands. */
    if (n >= 4 && dos_name[3] == '\\' && dos_name[2] == '?' &&
        ((dos_name[0] == '\\' && dos_name[1] == '\\') || (dos_name[0] == '\\' && dos_name[1] == '?'))) {
        res = (vibeos_wchar_t *)vibeos_rtl_alloc((n + 1u) * sizeof(vibeos_wchar_t));
        if (!res) {
            return 0;
        }
        memcpy(res, k_nt_prefix, sizeof(k_nt_prefix));
        memcpy(res + 4, dos_name + 4, (n - 4u) * sizeof(vibeos_wchar_t));
        return finish(nt_name, file_part, res, n);
    }

    if (pp) {
        const vibeos_ustring_t *cd = (const vibeos_ustring_t *)(const void *)(pp + VIBEOS_RUPP_CURDIR_PATH);

        if (cd->Buffer && cd->Length >= 2u) {
            cwd = cd->Buffer;
            cwd_len = cd->Length / 2u;
        }
    }

    cap = n + cwd_len + 4u;
    abs = (vibeos_wchar_t *)vibeos_rtl_alloc(cap * sizeof(vibeos_wchar_t) * 2u);
    if (!abs) {
        return 0;
    }
    norm = abs + cap;

    /* Make it absolute. */
    if (is_sep(dos_name[0]) && n >= 2 && is_sep(dos_name[1])) {
        /* UNC, or \\.\ : already absolute. */
    } else if (n >= 2 && is_alpha(dos_name[0]) && dos_name[1] == ':') {
        if (!(n >= 3 && is_sep(dos_name[2]))) {
            /* "C:b" is relative to that drive's directory, of which we know one. */
            if (cwd_len >= 2 && cwd[1] == ':' &&
                RtlUpcaseUnicodeChar(cwd[0]) == RtlUpcaseUnicodeChar(dos_name[0])) {
                memcpy(abs, cwd, cwd_len * sizeof(vibeos_wchar_t));
                abs_len = cwd_len;
            } else {
                abs[0] = dos_name[0];
                abs[1] = ':';
                abs_len = 2;
                abs[abs_len++] = '\\';      /* another drive's directory is its root */
            }
            if (n > 2 && abs[abs_len - 1] != '\\') {
                abs[abs_len++] = '\\';      /* "C:" alone is the directory itself */
            }
            i = 2;
        }
    } else if (is_sep(dos_name[0])) {
        /* "\a" is the root of the current drive (or share). */
        r = root_len(cwd, cwd_len);
        memcpy(abs, cwd, r * sizeof(vibeos_wchar_t));
        abs_len = r;
    } else {
        memcpy(abs, cwd, cwd_len * sizeof(vibeos_wchar_t));
        abs_len = cwd_len;
        if (abs[abs_len - 1] != '\\') {
            abs[abs_len++] = '\\';
        }
    }
    memcpy(abs + abs_len, dos_name + i, (n - i) * sizeof(vibeos_wchar_t));
    abs_len += n - i;
    for (i = 0; i < abs_len; i++) {
        if (abs[i] == '/') {
            abs[i] = '\\';
        }
    }
    out_len = normalise(abs, abs_len, norm);

    res = (vibeos_wchar_t *)vibeos_rtl_alloc((out_len + 9u) * sizeof(vibeos_wchar_t));
    if (!res) {
        vibeos_rtl_free(abs);
        return 0;
    }
    if (out_len >= 3 && norm[0] == '\\' && norm[1] == '\\' && norm[2] == '.') {
        /* \\.\NAME : the three characters "\\." become "\??" and the rest stays. */
        memcpy(res, k_nt_prefix, sizeof(k_nt_prefix));
        if (out_len > 4) {
            memcpy(res + 4, norm + 4, (out_len - 4u) * sizeof(vibeos_wchar_t));
        } else {
            out_len = 4;
        }
    } else if (out_len >= 2 && norm[0] == '\\' && norm[1] == '\\') {
        memcpy(res, k_unc_prefix, sizeof(k_unc_prefix));
        memcpy(res + 8, norm + 2, (out_len - 2u) * sizeof(vibeos_wchar_t));
        out_len += 6;
    } else {
        memcpy(res, k_nt_prefix, sizeof(k_nt_prefix));
        memcpy(res + 4, norm, out_len * sizeof(vibeos_wchar_t));
        out_len += 4;
    }
    vibeos_rtl_free(abs);
    return finish(nt_name, file_part, res, out_len);
}
