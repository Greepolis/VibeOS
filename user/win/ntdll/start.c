/* Where every NT process starts, and its loader: our ntdll's entry point
 * (docs/abi/windows.md W1, W2).
 *
 * The kernel maps the program and this DLL, builds the TEB and the PEB, points
 * %gs at the TEB and starts the first thread here with the PEB in rcx. What the
 * kernel does not do is link: on Windows that is ntdll's loader's work, and so it
 * is here.
 *
 * Linking, since W2: every DLL the program imports is brought in the way
 * Windows' loader does it - NtOpenFile of \??\C:\win\<name>, NtCreateSection
 * with SEC_IMAGE, NtMapViewOfSection, which places and relocates it - and its
 * own imports after it, depth first. A DLL is in the module list before its
 * imports are resolved, so two DLLs that import each other end. Its DllMain runs
 * once its imports are bound; the program's entry runs last, and what it returns
 * is the process's exit status, as Windows' own start routine makes it.
 *
 * A function a DLL does not export is not a failure to load. It is bound to a
 * stub that knows which import it stands for (stubs.S), and calling it prints
 * "unimplemented: kernel32.CreateFileA" and ends the process with
 * STATUS_ENTRYPOINT_NOT_FOUND. Wine's test programs import some three hundred
 * functions and call a few of them each: this way they run until the first one
 * that is missing, and say which - the next function to write, as LTP said the
 * next system call. A DLL that is not on the disk at all ends the process with
 * STATUS_DLL_NOT_FOUND, named the same way.
 *
 * Built -nostdlib and freestanding: nothing here may need a C runtime, because
 * this is what starts one. */

#define VIBEOS_BUILDING_NTDLL 1
#include "vibeos_ntdll.h"

/* Supplied by the linker: the address this image was mapped at. */
extern const uint8_t __ImageBase[];

static uint16_t rd16(uint64_t a) { return *(const volatile uint16_t *)(uintptr_t)a; }
static uint32_t rd32(uint64_t a) { return *(const volatile uint32_t *)(uintptr_t)a; }

/* ---- saying what went wrong --------------------------------------------------------- */

static uint32_t slen(const char *s) {
    uint32_t n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

static void say(const char *s) {
    uint64_t peb = vibeos_nt_rd64(vibeos_nt_teb() + VIBEOS_TEB_PEB);
    uint64_t params = vibeos_nt_rd64(peb + VIBEOS_PEB_PROCESS_PARAMS);
    vibeos_nt_iosb_t iosb;

    (void)NtWriteFile(vibeos_nt_rd64(params + VIBEOS_RUPP_STD_ERROR), 0, 0, 0, &iosb, s, slen(s),
                      0, 0);
}

/* "unimplemented: <dll>.<what>", then the end of the process. One write: a
 * line written in pieces is pieces in the console's log too, each with its own
 * prefix, and the line is the thing read. */
static __attribute__((noreturn)) void give_up(const char *dll, const char *what,
                                              NTSTATUS_T status) {
    char line[256];
    const char *parts[5];
    uint32_t n = 0, i, k;

    parts[0] = "unimplemented: ";
    parts[1] = dll;
    parts[2] = ".";
    parts[3] = what;
    parts[4] = "\n";
    for (i = 0; i < 5u; i++) {
        for (k = 0; parts[i][k] && n < sizeof(line) - 2u; k++) {
            line[n++] = parts[i][k];
        }
    }
    if (line[n - 1u] != '\n') {
        line[n++] = '\n';   /* cut short: still a line */
    }
    line[n] = 0;
    say(line);
    (void)NtTerminateProcess(VIBEOS_NT_CURRENT_PROCESS, status);
    for (;;) {
    }
}

/* ---- images ---------------------------------------------------------------------------- */

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

static uint32_t pe_entry_rva(uint64_t base) {
    return rd32(base + rd32(base + 0x3Cu) + 4u + 20u + 16u);
}

static char lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

/* DLL names compare without case, as Windows compares them. */
static int dll_eq(const char *a, const char *b) {
    for (;; a++, b++) {
        if (lower(*a) != lower(*b)) {
            return 0;
        }
        if (!*a) {
            return 1;
        }
    }
}

/* The address `base` exports under `name`, or 0. Exports are sorted by name, so
 * this searches by halves - kernel32's tests import six hundred names. */
static uint64_t pe_export(uint64_t base, const char *name) {
    uint32_t size = 0;
    uint32_t rva = pe_dir(base, 0u /* export */, &size);
    uint64_t dir, names, ords, funcs;
    uint32_t lo, hi;

    if (rva == 0u) {
        return 0;
    }
    dir = base + rva;
    funcs = base + rd32(dir + 28u);     /* AddressOfFunctions    */
    names = base + rd32(dir + 32u);     /* AddressOfNames        */
    ords = base + rd32(dir + 36u);      /* AddressOfNameOrdinals */
    lo = 0;
    hi = rd32(dir + 24u);               /* NumberOfNames         */
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        const char *have = (const char *)(uintptr_t)(base + rd32(names + mid * 4u));
        const char *a = have, *b = name;

        while (*a && *a == *b) {
            a++;
            b++;
        }
        if (*a == *b) {
            return base + rd32(funcs + rd16(ords + mid * 2u) * 4u);
        }
        if ((uint8_t)*a < (uint8_t)*b) {
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }
    return 0;
}

/* ---- the stubs a missing function is bound to ---------------------------------------- */

#define STUBS 2048u
extern const uint8_t vibeos_nt_stub_table[];   /* stubs.S: 16 bytes each */
static const char *g_stub_dll[STUBS];
static const char *g_stub_name[STUBS];
static uint32_t g_stubs;

/* Called by stub `k` (stubs.S) with its index: never returns. */
void vibeos_nt_unimplemented(uint32_t k) {
    if (k < g_stubs) {
        give_up(g_stub_dll[k], g_stub_name[k], VIBEOS_STATUS_ENTRYPOINT_NOT_FOUND);
    }
    give_up("?", "?", VIBEOS_STATUS_ENTRYPOINT_NOT_FOUND);
}

/* A stub standing for dll.name. The strings are the importing image's own,
 * which stays mapped as long as the process does. */
static uint64_t stub_for(const char *dll, const char *name) {
    if (g_stubs >= STUBS) {
        give_up(dll, "(more missing functions than stubs)", VIBEOS_STATUS_NO_MEMORY);
    }
    g_stub_dll[g_stubs] = dll;
    g_stub_name[g_stubs] = name;
    return (uint64_t)(uintptr_t)(vibeos_nt_stub_table + 16u * g_stubs++);
}

/* ---- modules --------------------------------------------------------------------------- */

#define MODULES 32u
typedef struct {
    char name[64];       /* as the importer spelled it, without a path */
    uint64_t base;
} module_t;

static module_t g_mod[MODULES];
static uint32_t g_mods;

static void add_module(const char *name, uint64_t base) {
    uint32_t i;

    if (g_mods >= MODULES) {
        give_up(name, "(more DLLs than the loader's list holds)", VIBEOS_STATUS_NO_MEMORY);
    }
    for (i = 0; name[i] && i < sizeof(g_mod[0].name) - 1u; i++) {
        g_mod[g_mods].name[i] = name[i];
    }
    g_mod[g_mods].name[i] = 0;
    g_mod[g_mods].base = base;
    g_mods++;
}

static uint64_t find_module(const char *name) {
    uint32_t i;
    for (i = 0; i < g_mods; i++) {
        if (dll_eq(g_mod[i].name, name)) {
            return g_mod[i].base;
        }
    }
    return 0;
}

static void resolve_imports(uint64_t base);

/* Bring `name` in from \??\C:\win\, or end the process saying it is not there. */
static uint64_t load_dll(const char *name) {
    static const char dir[] = "\\??\\C:\\win\\";
    uint16_t path[128];
    uint8_t us[VIBEOS_USTR_SIZE], oa[VIBEOS_OA_SIZE];
    uint64_t file = 0, section = 0, base = 0, size = 0;
    vibeos_nt_iosb_t iosb;
    NTSTATUS_T st;
    uint32_t n = 0, i;
    int done_entry;

    base = find_module(name);
    if (base) {
        return base;
    }
    for (i = 0; dir[i]; i++) {
        path[n++] = (uint8_t)dir[i];
    }
    for (i = 0; name[i] && n < 127u; i++) {
        path[n++] = (uint8_t)lower(name[i]);
    }
    for (i = 0; i < sizeof(us); i++) {
        us[i] = 0;
    }
    for (i = 0; i < sizeof(oa); i++) {
        oa[i] = 0;
    }
    *(uint16_t *)(us + VIBEOS_USTR_LENGTH) = (uint16_t)(n * 2u);
    *(uint16_t *)(us + VIBEOS_USTR_MAXIMUM) = (uint16_t)(n * 2u);
    *(uint64_t *)(us + VIBEOS_USTR_BUFFER) = (uint64_t)(uintptr_t)path;
    *(uint32_t *)(oa + VIBEOS_OA_LENGTH) = VIBEOS_OA_SIZE;
    *(uint64_t *)(oa + VIBEOS_OA_OBJECT_NAME) = (uint64_t)(uintptr_t)us;
    *(uint32_t *)(oa + VIBEOS_OA_ATTRIBUTES) = VIBEOS_OBJ_CASE_INSENSITIVE;

    /* FILE_READ_DATA | FILE_EXECUTE | SYNCHRONIZE, shared for reading, as a
     * loader opens an image. */
    st = NtOpenFile(&file, 0x00100021u, oa, &iosb, 1u, 0x20u);
    if (st != VIBEOS_STATUS_SUCCESS) {
        give_up(name, "(no such DLL)", VIBEOS_STATUS_DLL_NOT_FOUND);
    }
    st = NtCreateSection(&section, 0x000F001Fu, 0, 0, 0x02u /* PAGE_READONLY */,
                         0x01000000u /* SEC_IMAGE */, file);
    (void)NtClose(file);
    if (st != VIBEOS_STATUS_SUCCESS) {
        give_up(name, "(not an image)", VIBEOS_STATUS_INVALID_IMAGE_FORMAT);
    }
    st = NtMapViewOfSection(section, VIBEOS_NT_CURRENT_PROCESS, &base, 0, 0, 0, &size,
                            1u /* ViewShare */, 0, 0x02u);
    (void)NtClose(section);
    if (st != VIBEOS_STATUS_SUCCESS && st != 0x40000003u /* IMAGE_NOT_AT_BASE */) {
        give_up(name, "(could not be mapped)", st);
    }
    /* In the list before its imports: a DLL importing one that imports it back
     * finds it here instead of loading it again. */
    add_module(name, base);
    resolve_imports(base);
    done_entry = pe_entry_rva(base) != 0u;
    if (done_entry) {
        /* DllMain(instance, DLL_PROCESS_ATTACH, 0): false refuses the load. */
        int (*dll_main)(uint64_t, uint32_t, void *) =
            (int (*)(uint64_t, uint32_t, void *))(uintptr_t)(base + pe_entry_rva(base));
        if (!dll_main(base, 1u, 0)) {
            give_up(name, "(DllMain refused to load)", VIBEOS_STATUS_DLL_NOT_FOUND);
        }
    }
    return base;
}

/* Fill an image's import address table: every function by name from its DLL,
 * and a stub for every one the DLL does not have. */
static void resolve_imports(uint64_t base) {
    uint32_t rva = pe_dir(base, 1u /* import */, 0);
    uint64_t d;

    if (rva == 0u) {
        return;
    }
    /* IMAGE_IMPORT_DESCRIPTOR: OriginalFirstThunk, TimeDateStamp,
     * ForwarderChain, Name, FirstThunk - twenty bytes, ended by zeros. */
    for (d = base + rva; rd32(d + 12u) != 0u; d += 20u) {
        const char *dll = (const char *)(uintptr_t)(base + rd32(d + 12u));
        uint64_t lookup = base + (rd32(d) ? rd32(d) : rd32(d + 16u));
        uint64_t iat = base + rd32(d + 16u);
        uint64_t from = load_dll(dll);

        for (;; lookup += 8u, iat += 8u) {
            uint64_t thunk = vibeos_nt_rd64(lookup);
            uint64_t at = 0;
            const char *fn = "(by ordinal)";

            if (thunk == 0u) {
                break;
            }
            if (!(thunk >> 63)) {
                /* IMAGE_IMPORT_BY_NAME: a hint, then the name. */
                fn = (const char *)(uintptr_t)(base + (uint32_t)thunk + 2u);
                at = pe_export(from, fn);
            }
            *(volatile uint64_t *)(uintptr_t)iat = at ? at : stub_for(dll, fn);
        }
    }
}

/* The entry the kernel starts the first thread at, with the PEB in rcx. */
__attribute__((noreturn)) void vibeos_nt_process_start(uint64_t peb) {
    uint64_t exe = vibeos_nt_rd64(peb + VIBEOS_PEB_IMAGE_BASE);
    uint32_t entry_rva = pe_entry_rva(exe);
    NTSTATUS_T st;

    add_module("ntdll.dll", (uint64_t)(uintptr_t)__ImageBase);
    resolve_imports(exe);
    if (entry_rva == 0u) {
        st = VIBEOS_STATUS_INVALID_IMAGE_FORMAT;
    } else {
        int (*entry)(void) = (int (*)(void))(uintptr_t)(exe + entry_rva);
        st = (NTSTATUS_T)entry();
    }
    (void)NtTerminateProcess(VIBEOS_NT_CURRENT_PROCESS, st);
    for (;;) {
        /* NtTerminateProcess of ourselves does not return. */
    }
}
