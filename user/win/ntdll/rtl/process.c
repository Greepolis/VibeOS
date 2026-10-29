/* Process parameters (docs/abi/windows.md W6): RtlCreateProcessParametersEx
 * and its family, which build the RTL_USER_PROCESS_PARAMETERS a parent hands
 * NtCreateUserProcess.
 *
 * One allocation: the fixed part (VIBEOS_RUPP_SIZE bytes), then every string
 * with its NUL, then the environment, which is copied too. Normalised, every
 * Buffer is an address; denormalised, an offset from the block's start - which
 * is what the flag in the block says, and what NtCreateUserProcess reads. A
 * string not given is the current process's (the directory, the DLL path, the
 * environment) or the image's path (the command line), as Windows does. */

#include "rtl_internal.h"

#define RUPP_WINDOW_TITLE 0x0B0u
#define RUPP_DESKTOP_INFO 0x0C0u
#define RUPP_SHELL_INFO 0x0D0u
#define RUPP_RUNTIME_DATA 0x0E0u
#define RUPP_ENVIRONMENT_SIZE 0x3F0u

/* The current process's string at `field`, as a UNICODE_STRING, or null. */
static const vibeos_ustring_t *proc_ustr(uint32_t field) {
    uint8_t *pp = vibeos_rtl_process_params();

    return pp ? (const vibeos_ustring_t *)(void *)(pp + field) : 0;
}

static size_t env_bytes(const vibeos_wchar_t *e) {
    size_t n = 0;

    if (!e) {
        return 0;
    }
    while (e[n]) {
        n += rtl_wcslen(e + n) + 1u;
    }
    return 2u * (n + 1u);
}

/* Copy `src` (or nothing) into the block at *at, as the string at `field`. */
static void put_ustr(uint8_t *b, uint32_t field, size_t *at, const vibeos_ustring_t *src) {
    vibeos_ustring_t *d = (vibeos_ustring_t *)(void *)(b + field);
    uint16_t len = src && src->Buffer ? src->Length : 0u;
    size_t i;

    d->Length = len;
    d->MaximumLength = (uint16_t)(len + 2u);
    d->Buffer = (vibeos_wchar_t *)(void *)(b + *at);
    for (i = 0; i < len; i++) {
        b[*at + i] = ((const uint8_t *)src->Buffer)[i];
    }
    b[*at + len] = 0;
    b[*at + len + 1u] = 0;
    *at += ((size_t)len + 2u + 7u) & ~(size_t)7u;
}

static uint32_t ustr_room(const vibeos_ustring_t *s) {
    return (uint32_t)((((s && s->Buffer) ? s->Length : 0u) + 2u + 7u) & ~7u);
}

RTL_API void *RtlNormalizeProcessParams(void *params) {
    uint8_t *b = (uint8_t *)params;
    static const uint32_t fields[] = { VIBEOS_RUPP_CURDIR_PATH, VIBEOS_RUPP_DLL_PATH,
                                       VIBEOS_RUPP_IMAGE_PATH, VIBEOS_RUPP_COMMAND_LINE,
                                       RUPP_WINDOW_TITLE, RUPP_DESKTOP_INFO, RUPP_SHELL_INFO,
                                       RUPP_RUNTIME_DATA };
    uint32_t i;

    if (b && !(*(uint32_t *)(void *)(b + VIBEOS_RUPP_FLAGS) & VIBEOS_RUPP_FLAG_NORMALIZED)) {
        for (i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
            uint64_t v = rtl_rd64(b, fields[i] + 8u);
            if (v) {
                rtl_wr64(b, fields[i] + 8u, v + (uint64_t)(uintptr_t)b);
            }
        }
        if (rtl_rd64(b, VIBEOS_RUPP_ENVIRONMENT)) {
            rtl_wr64(b, VIBEOS_RUPP_ENVIRONMENT, rtl_rd64(b, VIBEOS_RUPP_ENVIRONMENT) + (uint64_t)(uintptr_t)b);
        }
        *(uint32_t *)(void *)(b + VIBEOS_RUPP_FLAGS) |= VIBEOS_RUPP_FLAG_NORMALIZED;
    }
    return params;
}

RTL_API void *RtlDeNormalizeProcessParams(void *params) {
    uint8_t *b = (uint8_t *)params;
    static const uint32_t fields[] = { VIBEOS_RUPP_CURDIR_PATH, VIBEOS_RUPP_DLL_PATH,
                                       VIBEOS_RUPP_IMAGE_PATH, VIBEOS_RUPP_COMMAND_LINE,
                                       RUPP_WINDOW_TITLE, RUPP_DESKTOP_INFO, RUPP_SHELL_INFO,
                                       RUPP_RUNTIME_DATA };
    uint32_t i;

    if (b && (*(uint32_t *)(void *)(b + VIBEOS_RUPP_FLAGS) & VIBEOS_RUPP_FLAG_NORMALIZED)) {
        for (i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
            uint64_t v = rtl_rd64(b, fields[i] + 8u);
            if (v) {
                rtl_wr64(b, fields[i] + 8u, v - (uint64_t)(uintptr_t)b);
            }
        }
        if (rtl_rd64(b, VIBEOS_RUPP_ENVIRONMENT)) {
            rtl_wr64(b, VIBEOS_RUPP_ENVIRONMENT, rtl_rd64(b, VIBEOS_RUPP_ENVIRONMENT) - (uint64_t)(uintptr_t)b);
        }
        *(uint32_t *)(void *)(b + VIBEOS_RUPP_FLAGS) &= ~VIBEOS_RUPP_FLAG_NORMALIZED;
    }
    return params;
}

RTL_API NTSTATUS_T RtlCreateProcessParametersEx(void **result, const vibeos_ustring_t *image,
                                                const vibeos_ustring_t *dllpath,
                                                const vibeos_ustring_t *curdir,
                                                const vibeos_ustring_t *cmdline, void *env,
                                                const vibeos_ustring_t *title,
                                                const vibeos_ustring_t *desktop,
                                                const vibeos_ustring_t *shell,
                                                const vibeos_ustring_t *runtime, uint32_t flags) {
    uint8_t *pp = vibeos_rtl_process_params(), *b;
    size_t size, at, envb, i;
    static const vibeos_ustring_t empty = { 0, 0, 0 };

    if (!result || !image) {
        return VIBEOS_STATUS_INVALID_PARAMETER;
    }
    if (!dllpath) {
        dllpath = proc_ustr(VIBEOS_RUPP_DLL_PATH);
    }
    if (!curdir) {
        curdir = proc_ustr(VIBEOS_RUPP_CURDIR_PATH);
    }
    if (!cmdline) {
        cmdline = image;
    }
    if (!env && pp) {
        env = (void *)(uintptr_t)rtl_rd64(pp, VIBEOS_RUPP_ENVIRONMENT);
    }
    title = title ? title : &empty;
    desktop = desktop ? desktop : &empty;
    shell = shell ? shell : &empty;
    runtime = runtime ? runtime : &empty;
    envb = env_bytes((const vibeos_wchar_t *)env);
    size = VIBEOS_RUPP_SIZE + ustr_room(curdir) + ustr_room(dllpath) + ustr_room(image) +
           ustr_room(cmdline) + ustr_room(title) + ustr_room(desktop) + ustr_room(shell) +
           ustr_room(runtime) + envb;
    b = (uint8_t *)vibeos_rtl_alloc(size);
    if (!b) {
        return VIBEOS_STATUS_NO_MEMORY;
    }
    for (i = 0; i < size; i++) {
        b[i] = 0;
    }
    *(uint32_t *)(void *)(b + VIBEOS_RUPP_MAXIMUM_LENGTH) = (uint32_t)size;
    *(uint32_t *)(void *)(b + VIBEOS_RUPP_LENGTH) = (uint32_t)size;
    *(uint32_t *)(void *)(b + VIBEOS_RUPP_FLAGS) = VIBEOS_RUPP_FLAG_NORMALIZED;
    at = VIBEOS_RUPP_SIZE;
    put_ustr(b, VIBEOS_RUPP_CURDIR_PATH, &at, curdir);
    put_ustr(b, VIBEOS_RUPP_DLL_PATH, &at, dllpath);
    put_ustr(b, VIBEOS_RUPP_IMAGE_PATH, &at, image);
    put_ustr(b, VIBEOS_RUPP_COMMAND_LINE, &at, cmdline);
    put_ustr(b, RUPP_WINDOW_TITLE, &at, title);
    put_ustr(b, RUPP_DESKTOP_INFO, &at, desktop);
    put_ustr(b, RUPP_SHELL_INFO, &at, shell);
    put_ustr(b, RUPP_RUNTIME_DATA, &at, runtime);
    if (envb) {
        for (i = 0; i < envb; i++) {
            b[at + i] = ((const uint8_t *)env)[i];
        }
        rtl_wr64(b, VIBEOS_RUPP_ENVIRONMENT, (uint64_t)(uintptr_t)(b + at));
        rtl_wr64(b, RUPP_ENVIRONMENT_SIZE, envb);
    }
    if (pp) {   /* the standard handles: the creator's */
        rtl_wr64(b, VIBEOS_RUPP_STD_INPUT, rtl_rd64(pp, VIBEOS_RUPP_STD_INPUT));
        rtl_wr64(b, VIBEOS_RUPP_STD_OUTPUT, rtl_rd64(pp, VIBEOS_RUPP_STD_OUTPUT));
        rtl_wr64(b, VIBEOS_RUPP_STD_ERROR, rtl_rd64(pp, VIBEOS_RUPP_STD_ERROR));
    }
    if (!(flags & VIBEOS_RUPP_FLAG_NORMALIZED)) {
        (void)RtlDeNormalizeProcessParams(b);
    }
    *result = b;
    return VIBEOS_STATUS_SUCCESS;
}

RTL_API NTSTATUS_T RtlCreateProcessParameters(void **result, const vibeos_ustring_t *image,
                                              const vibeos_ustring_t *dllpath,
                                              const vibeos_ustring_t *curdir,
                                              const vibeos_ustring_t *cmdline, void *env,
                                              const vibeos_ustring_t *title,
                                              const vibeos_ustring_t *desktop,
                                              const vibeos_ustring_t *shell,
                                              const vibeos_ustring_t *runtime) {
    return RtlCreateProcessParametersEx(result, image, dllpath, curdir, cmdline, env, title, desktop,
                                        shell, runtime, 0);
}

RTL_API void RtlDestroyProcessParameters(void *params) {
    vibeos_rtl_free(params);
}
