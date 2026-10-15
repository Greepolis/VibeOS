/* Environment blocks (docs/abi/windows.md W2).
 *
 * A block is a run of "NAME=value" strings, each ended by a NUL, and the run is
 * ended by one more NUL. Names are compared without case. A name may itself
 * begin with '=' (the per-drive directories Windows keeps as "=C:=C:\dir"), so
 * the separator is looked for from the second character on.
 *
 * The process's block is PEB->ProcessParameters->Environment, which the kernel
 * leaves NULL until W6; every function treats NULL as an empty block. A block
 * these functions build is a fresh allocation each time it changes, and the
 * previous one is freed - so a caller must not keep a pointer into a block
 * across RtlSetEnvironmentVariable. */

#include "rtl_internal.h"

/* Length in units of a block, up to and including the final NUL. */
static size_t env_units(const vibeos_wchar_t *e) {
    size_t n = 0;

    if (!e) {
        return 1;
    }
    while (e[n]) {
        n += rtl_wcslen(e + n) + 1u;
    }
    return n + 1u;
}

/* Position of the '=' that ends a name, or -1 for a string with none. */
static ptrdiff_t env_eq(const vibeos_wchar_t *s) {
    size_t i = s[0] == '=' ? 1u : 0u;

    for (; s[i]; i++) {
        if (s[i] == '=') {
            return (ptrdiff_t)i;
        }
    }
    return -1;
}

static int name_matches(const vibeos_wchar_t *entry, size_t eq, const vibeos_wchar_t *name,
                        size_t name_len) {
    size_t i;

    if (eq != name_len) {
        return 0;
    }
    for (i = 0; i < name_len; i++) {
        if (RtlUpcaseUnicodeChar(entry[i]) != RtlUpcaseUnicodeChar(name[i])) {
            return 0;
        }
    }
    return 1;
}

static vibeos_wchar_t *process_env(void) {
    uint8_t *pp = vibeos_rtl_process_params();

    return pp ? (vibeos_wchar_t *)(uintptr_t)rtl_rd64(pp, VIBEOS_RUPP_ENVIRONMENT) : 0;
}

vibeos_ntstatus_t RtlCreateEnvironment(vibeos_boolean_t inherit, vibeos_wchar_t **environment) {
    const vibeos_wchar_t *from = inherit ? process_env() : 0;
    size_t units = env_units(from);
    vibeos_wchar_t *e;

    if (!environment) {
        return VIBEOS_RTL_ST(VIBEOS_STATUS_INVALID_PARAMETER);
    }
    /* Room for two terminators even when empty, so that a reader which looks one
     * unit past the first NUL finds a second. */
    e = (vibeos_wchar_t *)vibeos_rtl_alloc((units + 1u) * sizeof(vibeos_wchar_t));
    if (!e) {
        return VIBEOS_RTL_ST(VIBEOS_STATUS_NO_MEMORY);
    }
    memset(e, 0, (units + 1u) * sizeof(vibeos_wchar_t));
    if (from) {
        memcpy(e, from, units * sizeof(vibeos_wchar_t));
    }
    *environment = e;
    return VIBEOS_RTL_ST(VIBEOS_STATUS_SUCCESS);
}

vibeos_ntstatus_t RtlDestroyEnvironment(void *environment) {
    vibeos_rtl_free(environment);
    return VIBEOS_RTL_ST(VIBEOS_STATUS_SUCCESS);
}

void RtlSetCurrentEnvironment(void *environment, void **previous) {
    uint8_t *pp = vibeos_rtl_process_params();

    if (previous) {
        *previous = pp ? (void *)(uintptr_t)rtl_rd64(pp, VIBEOS_RUPP_ENVIRONMENT) : 0;
    }
    if (pp) {
        rtl_wr64(pp, VIBEOS_RUPP_ENVIRONMENT, (uint64_t)(uintptr_t)environment);
    }
}

vibeos_ntstatus_t RtlQueryEnvironmentVariable_U(vibeos_wchar_t *environment,
                                                const vibeos_ustring_t *name,
                                                vibeos_ustring_t *value) {
    const vibeos_wchar_t *e = environment ? environment : process_env();
    size_t name_len;

    if (!name || !value || (name->Length != 0 && !name->Buffer)) {
        return VIBEOS_RTL_ST(VIBEOS_STATUS_INVALID_PARAMETER);
    }
    name_len = name->Length / 2u;
    if (!e || name_len == 0) {
        return VIBEOS_RTL_ST(VIBEOS_STATUS_VARIABLE_NOT_FOUND);
    }
    while (*e) {
        size_t entry_len = rtl_wcslen(e);
        ptrdiff_t eq = env_eq(e);

        if (eq >= 0 && name_matches(e, (size_t)eq, name->Buffer, name_len)) {
            size_t bytes = (entry_len - (size_t)eq - 1u) * sizeof(vibeos_wchar_t);

            /* Length is the size needed whether or not it fits. The terminator
             * is written when there is room for it and is not counted. */
            value->Length = (uint16_t)bytes;
            if (value->MaximumLength < bytes) {
                return VIBEOS_RTL_ST(VIBEOS_STATUS_BUFFER_TOO_SMALL);
            }
            memcpy(value->Buffer, e + eq + 1, bytes);
            if (value->MaximumLength >= bytes + 2u) {
                value->Buffer[bytes / 2u] = 0;
            }
            return VIBEOS_RTL_ST(VIBEOS_STATUS_SUCCESS);
        }
        e += entry_len + 1u;
    }
    return VIBEOS_RTL_ST(VIBEOS_STATUS_VARIABLE_NOT_FOUND);
}

vibeos_ntstatus_t RtlSetEnvironmentVariable(vibeos_wchar_t **environment, const vibeos_ustring_t *name,
                                            const vibeos_ustring_t *value) {
    vibeos_wchar_t *cur, *out, *o;
    const vibeos_wchar_t *e;
    size_t name_len, value_len, old_units, cap, i;
    int found = 0;
    uint8_t *pp = 0;

    if (!name || name->Length < 2u || !name->Buffer) {
        return VIBEOS_RTL_ST(VIBEOS_STATUS_INVALID_PARAMETER);
    }
    name_len = name->Length / 2u;
    /* A '=' after the first character would end the name early, and a name that
     * is only '=' has no name in it. */
    if (name_len == 1 && name->Buffer[0] == '=') {
        return VIBEOS_RTL_ST(VIBEOS_STATUS_INVALID_PARAMETER);
    }
    for (i = 1; i < name_len; i++) {
        if (name->Buffer[i] == '=') {
            return VIBEOS_RTL_ST(VIBEOS_STATUS_INVALID_PARAMETER);
        }
    }
    if (value && value->Length != 0 && !value->Buffer) {
        return VIBEOS_RTL_ST(VIBEOS_STATUS_INVALID_PARAMETER);
    }
    value_len = value ? value->Length / 2u : 0;

    if (environment) {
        cur = *environment;
    } else {
        pp = vibeos_rtl_process_params();
        if (!pp) {
            return VIBEOS_RTL_ST(VIBEOS_STATUS_INVALID_PARAMETER);
        }
        cur = process_env();
    }
    old_units = env_units(cur);
    /* Large enough for the old block, one new string and the terminators. */
    cap = old_units + name_len + 1u + value_len + 1u + 1u;
    out = (vibeos_wchar_t *)vibeos_rtl_alloc(cap * sizeof(vibeos_wchar_t));
    if (!out) {
        return VIBEOS_RTL_ST(VIBEOS_STATUS_NO_MEMORY);
    }
    o = out;
    for (e = cur; e && *e;) {
        size_t entry_len = rtl_wcslen(e);
        ptrdiff_t eq = env_eq(e);

        if (eq >= 0 && name_matches(e, (size_t)eq, name->Buffer, name_len)) {
            found = 1;
            if (value) {
                /* Replaced where it stood, and under the name it was first set
                 * with: the call changes the value, not the spelling. */
                memcpy(o, e, name_len * sizeof(vibeos_wchar_t));
                o += name_len;
                *o++ = '=';
                if (value_len) {
                    memcpy(o, value->Buffer, value_len * sizeof(vibeos_wchar_t));
                }
                o += value_len;
                *o++ = 0;
            }
        } else {
            memcpy(o, e, (entry_len + 1u) * sizeof(vibeos_wchar_t));
            o += entry_len + 1u;
        }
        e += entry_len + 1u;
    }
    if (!found && !value) {
        /* Deleting what is not there is reported, and changes nothing. */
        vibeos_rtl_free(out);
        return VIBEOS_RTL_ST(VIBEOS_STATUS_VARIABLE_NOT_FOUND);
    }
    if (!found) {
        memcpy(o, name->Buffer, name_len * sizeof(vibeos_wchar_t));
        o += name_len;
        *o++ = '=';
        if (value_len) {
            memcpy(o, value->Buffer, value_len * sizeof(vibeos_wchar_t));
        }
        o += value_len;
        *o++ = 0;
    }
    *o++ = 0;
    *o = 0;
    if (environment) {
        *environment = out;
    } else {
        rtl_wr64(pp, VIBEOS_RUPP_ENVIRONMENT, (uint64_t)(uintptr_t)out);
    }
    vibeos_rtl_free(cur);
    return VIBEOS_RTL_ST(VIBEOS_STATUS_SUCCESS);
}
