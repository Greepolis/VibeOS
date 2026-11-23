/* SPDX-License-Identifier: GPL-3.0-or-later */
/* A small shell32.dll: the three calls Wine's cmd.exe makes (docs/shell/s6.md
 * step 5). What runs a file, whether a program is a console one, and the
 * recursive delete of `rmdir /s`. There is no association table, so only .exe
 * and .com have a program that runs them. */

#pragma GCC optimize("no-tree-loop-distribute-patterns")

#include <stdint.h>

typedef int32_t BOOL32;

#define INVALID_H ((uint64_t)-1)
#define NO_ATTRIBUTES 0xFFFFFFFFu
#define ATTR_READONLY 0x01u
#define ATTR_DIRECTORY 0x10u
#define ATTR_REPARSE 0x400u
#define SE_ERR_FNF 2
#define SE_ERR_NOASSOC 31
#define FO_DELETE 3u
#define PATH_UNITS 1024u
#define MAX_DEPTH 64

__declspec(dllimport) uint32_t GetFileAttributesW(const uint16_t *name);
__declspec(dllimport) BOOL32 SetFileAttributesW(const uint16_t *name, uint32_t attributes);
__declspec(dllimport) uint64_t CreateFileW(const uint16_t *name, uint32_t access, uint32_t share, const void *sa,
                                           uint32_t disposition, uint32_t flags, uint64_t template_file);
__declspec(dllimport) BOOL32 ReadFile(uint64_t h, void *buf, uint32_t len, uint32_t *got, void *overlapped);
__declspec(dllimport) BOOL32 SetFilePointerEx(uint64_t h, int64_t to, int64_t *now, uint32_t how);
__declspec(dllimport) BOOL32 CloseHandle(uint64_t h);
__declspec(dllimport) BOOL32 DeleteFileW(const uint16_t *name);
__declspec(dllimport) BOOL32 RemoveDirectoryW(const uint16_t *name);
__declspec(dllimport) uint64_t FindFirstFileW(const uint16_t *pattern, void *data);
__declspec(dllimport) BOOL32 FindNextFileW(uint64_t h, void *data);
__declspec(dllimport) BOOL32 FindClose(uint64_t h);
__declspec(dllimport) uint32_t GetLastError(void);

static uint32_t wl(const uint16_t *s) {
    uint32_t n = 0;

    while (s[n]) {
        n++;
    }
    return n;
}

static uint16_t lower(uint16_t c) {
    return (c >= 'A' && c <= 'Z') ? (uint16_t)(c + 32) : c;
}

static int is_program(const uint16_t *name) {
    uint32_t n = wl(name);

    return n > 4u && name[n - 4u] == '.' &&
           ((lower(name[n - 3u]) == 'e' && lower(name[n - 2u]) == 'x' && lower(name[n - 1u]) == 'e') ||
            (lower(name[n - 3u]) == 'c' && lower(name[n - 2u]) == 'o' && lower(name[n - 1u]) == 'm'));
}

/* Above 32 is success, in this call's old convention. */
uint64_t FindExecutableW(const uint16_t *file, const uint16_t *dir, uint16_t *result) {
    uint32_t n, i;

    (void)dir;
    result[0] = 0;
    if (GetFileAttributesW(file) == NO_ATTRIBUTES) {
        return SE_ERR_FNF;
    }
    if (!is_program(file)) {
        return SE_ERR_NOASSOC;
    }
    n = wl(file);
    if (n >= 260u) {
        return SE_ERR_FNF;
    }
    for (i = 0; i <= n; i++) {
        result[i] = file[i];
    }
    return 33;
}

/* With SHGFI_EXETYPE: 'PE' for a console program, 'PE' and a version in the high
 * word for a windowed one, 0 for anything else. */
uint64_t SHGetFileInfoW(const uint16_t *path, uint32_t attributes, void *info, uint32_t size, uint32_t flags) {
    uint8_t head[0x200];
    uint32_t got = 0, pe, sub;
    uint64_t h;

    (void)attributes;
    (void)info;
    (void)size;
    if (!(flags & 0x2000u)) {
        return 0;
    }
    h = CreateFileW(path, 0x80000000u, 1u, 0, 3u, 0x80u, 0);
    if (h == INVALID_H) {
        return 0;
    }
    if (!ReadFile(h, head, sizeof head, &got, 0)) {
        got = 0;
    }
    (void)CloseHandle(h);
    if (got < 0x40u || head[0] != 'M' || head[1] != 'Z') {
        return 0;
    }
    pe = *(const uint32_t *)(const void *)(head + 0x3C);
    if (pe > got - 24u - 70u || head[pe] != 'P' || head[pe + 1u] != 'E') {
        return 0;
    }
    sub = *(const uint16_t *)(const void *)(head + pe + 24u + 68u);
    return sub == 3u ? 0x4550u : (0x06000000u | 0x4550u);
}

/* Links are removed, never followed: a junction out of the tree being deleted
 * must not take what it points at along. */
static uint32_t delete_tree(uint16_t *path, uint32_t len, int depth) {
    uint8_t data[592];
    uint32_t at = GetFileAttributesW(path), rc = 0;
    uint64_t h;

    if (at == NO_ATTRIBUTES) {
        return GetLastError();
    }
    if (at & ATTR_READONLY) {
        (void)SetFileAttributesW(path, at & ~ATTR_READONLY);
    }
    if (!(at & ATTR_DIRECTORY)) {
        return DeleteFileW(path) ? 0u : GetLastError();
    }
    if (!(at & ATTR_REPARSE)) {
        if (depth >= MAX_DEPTH || len + 3u >= PATH_UNITS) {
            return 206u;    /* ERROR_FILENAME_EXCED_RANGE */
        }
        /* The listing is asked again after each removal: it is kept by position
         * here, so going on with it after deleting an entry skips the next one
         * (WO-107), and the directory is then not empty. */
        for (;;) {
            const uint16_t *name = (const uint16_t *)(const void *)(data + 44);
            uint32_t n = 0, i;
            int found = 0;

            path[len] = '\\';
            path[len + 1u] = '*';
            path[len + 2u] = 0;
            h = FindFirstFileW(path, data);
            path[len] = 0;
            if (h == INVALID_H) {
                break;
            }
            do {
                n = wl(name);
                if (!((n == 1u && name[0] == '.') || (n == 2u && name[0] == '.' && name[1] == '.'))) {
                    found = 1;
                    break;
                }
            } while (FindNextFileW(h, data));
            (void)FindClose(h);
            if (!found) {
                break;
            }
            if (len + 1u + n >= PATH_UNITS) {
                return 206u;
            }
            path[len] = '\\';
            for (i = 0; i <= n; i++) {
                path[len + 1u + i] = name[i];
            }
            rc = delete_tree(path, len + 1u + n, depth + 1);
            path[len] = 0;
            if (rc) {
                return rc;
            }
        }
    }
    return RemoveDirectoryW(path) ? 0u : GetLastError();
}

/* SHFILEOPSTRUCTW on x64: wFunc at 8, pFrom at 16. Only delete, which is all cmd
 * asks; pFrom is a list ended by an empty string. */
int SHFileOperationW(void *op) {
    static uint16_t path[PATH_UNITS];
    const uint8_t *o = op;
    const uint16_t *from;

    if (*(const uint32_t *)(const void *)(o + 8) != FO_DELETE) {
        return 120;     /* ERROR_CALL_NOT_IMPLEMENTED */
    }
    from = *(const uint16_t *const *)(const void *)(o + 16);
    while (from && *from) {
        uint32_t n = wl(from), i;
        uint32_t rc;

        if (n >= PATH_UNITS) {
            return 206;
        }
        for (i = 0; i <= n; i++) {
            path[i] = from[i];
        }
        while (n > 3u && (path[n - 1u] == '\\' || path[n - 1u] == '/')) {
            path[--n] = 0;
        }
        rc = delete_tree(path, n, 0);
        if (rc) {
            return (int)rc;
        }
        from += wl(from) + 1u;
    }
    return 0;
}

int vibeos_shell32_main(uint64_t instance, uint32_t reason, void *reserved) {
    (void)instance;
    (void)reason;
    (void)reserved;
    return 1;
}
