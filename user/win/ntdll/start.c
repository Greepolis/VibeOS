/* Where every NT process starts: our ntdll's entry point (docs/abi/windows.md W1).
 *
 * The kernel maps the program and this DLL, builds the TEB and the PEB, points
 * %gs at the TEB and starts the first thread here with the PEB in rcx. What the
 * kernel does not do is link: on Windows that is ntdll's loader's work, and so it
 * is here. The program's imports are resolved against the DLLs already in the
 * address space - in W1 that is this one - and the program's entry is called.
 * Whatever it returns is the process's exit status, as Windows' own start
 * routine makes it.
 *
 * A program that imports a DLL nobody has mapped is ended with
 * STATUS_DLL_NOT_FOUND, and one that names a function we do not export with
 * STATUS_ENTRYPOINT_NOT_FOUND - Windows' answers, and the parent sees them as
 * the exit status. Loading further DLLs from disk is the loader's next step.
 *
 * Built -nostdlib and freestanding: nothing here may need a C runtime, because
 * this is what starts one. */

#define VIBEOS_BUILDING_NTDLL 1
#include "vibeos_ntdll.h"

/* Supplied by the linker: the address this image was mapped at. */
extern const uint8_t __ImageBase[];

static uint16_t rd16(uint64_t a) { return *(const volatile uint16_t *)(uintptr_t)a; }
static uint32_t rd32(uint64_t a) { return *(const volatile uint32_t *)(uintptr_t)a; }

/* An image's data directory: its RVA, or 0 if it has none. */
static uint32_t pe_dir(uint64_t base, uint32_t which, uint32_t *size) {
    uint64_t nt = base + rd32(base + 0x3Cu);
    uint64_t opt = nt + 4u + 20u;
    uint32_t count = rd32(opt + 108u);

    if (which >= count) {
        return 0;
    }
    if (size) {
        *size = rd32(opt + 112u + which * 8u + 4u);
    }
    return rd32(opt + 112u + which * 8u);
}

static int name_eq(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/* DLL names compare without case, as Windows compares them. */
static int dll_eq(const char *a, const char *b) {
    for (;; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') {
            x = (char)(x - 'A' + 'a');
        }
        if (y >= 'A' && y <= 'Z') {
            y = (char)(y - 'A' + 'a');
        }
        if (x != y) {
            return 0;
        }
        if (!x) {
            return 1;
        }
    }
}

/* The address `base` exports under `name`, or 0. Exports are sorted by name,
 * so this could search by halves; with one DLL of a few hundred names it does
 * not need to yet. */
static uint64_t pe_export(uint64_t base, const char *name) {
    uint32_t size = 0;
    uint32_t rva = pe_dir(base, 0u /* export */, &size);
    uint64_t dir, names, ords, funcs;
    uint32_t n, i;

    if (rva == 0u) {
        return 0;
    }
    dir = base + rva;
    n = rd32(dir + 24u);                /* NumberOfNames         */
    funcs = base + rd32(dir + 28u);     /* AddressOfFunctions    */
    names = base + rd32(dir + 32u);     /* AddressOfNames        */
    ords = base + rd32(dir + 36u);      /* AddressOfNameOrdinals */
    for (i = 0; i < n; i++) {
        if (name_eq((const char *)(uintptr_t)(base + rd32(names + i * 4u)), name)) {
            return base + rd32(funcs + rd16(ords + i * 2u) * 4u);
        }
    }
    return 0;
}

/* Fill the program's import address table. 0, or the status that ends it. */
static NTSTATUS_T resolve_imports(uint64_t exe) {
    uint64_t self = (uint64_t)(uintptr_t)__ImageBase;
    uint32_t rva = pe_dir(exe, 1u /* import */, 0);
    uint64_t d;

    if (rva == 0u) {
        return VIBEOS_STATUS_SUCCESS;
    }
    /* IMAGE_IMPORT_DESCRIPTOR: OriginalFirstThunk, TimeDateStamp,
     * ForwarderChain, Name, FirstThunk - twenty bytes, ended by zeros. */
    for (d = exe + rva; rd32(d + 12u) != 0u; d += 20u) {
        const char *dll = (const char *)(uintptr_t)(exe + rd32(d + 12u));
        uint64_t lookup = exe + (rd32(d) ? rd32(d) : rd32(d + 16u));
        uint64_t iat = exe + rd32(d + 16u);

        if (!dll_eq(dll, "ntdll.dll")) {
            return VIBEOS_STATUS_DLL_NOT_FOUND;
        }
        for (;; lookup += 8u, iat += 8u) {
            uint64_t thunk = vibeos_nt_rd64(lookup);
            uint64_t at;

            if (thunk == 0u) {
                break;
            }
            if (thunk >> 63) {
                return VIBEOS_STATUS_ENTRYPOINT_NOT_FOUND;   /* by ordinal: not yet */
            }
            /* IMAGE_IMPORT_BY_NAME: a hint, then the name. */
            at = pe_export(self, (const char *)(uintptr_t)(exe + (uint32_t)thunk + 2u));
            if (at == 0u) {
                return VIBEOS_STATUS_ENTRYPOINT_NOT_FOUND;
            }
            *(volatile uint64_t *)(uintptr_t)iat = at;
        }
    }
    return VIBEOS_STATUS_SUCCESS;
}

/* The entry the kernel starts the first thread at, with the PEB in rcx. */
__attribute__((noreturn)) void vibeos_nt_process_start(uint64_t peb) {
    uint64_t exe = vibeos_nt_rd64(peb + VIBEOS_PEB_IMAGE_BASE);
    uint64_t nt = exe + rd32(exe + 0x3Cu);
    uint32_t entry_rva = rd32(nt + 4u + 20u + 16u);
    NTSTATUS_T st = resolve_imports(exe);

    if (st == VIBEOS_STATUS_SUCCESS) {
        if (entry_rva == 0u) {
            st = VIBEOS_STATUS_INVALID_IMAGE_FORMAT;
        } else {
            int (*entry)(void) = (int (*)(void))(uintptr_t)(exe + entry_rva);
            st = (NTSTATUS_T)entry();
        }
    }
    (void)NtTerminateProcess(VIBEOS_NT_CURRENT_PROCESS, st);
    for (;;) {
        /* NtTerminateProcess of ourselves does not return. */
    }
}
