/* Our kernel32.dll (docs/abi/windows.md W2).
 *
 * Windows' kernel32 is mostly a translation: a Win32 call becomes an Nt call,
 * a name becomes an object path, a timeout in milliseconds becomes an NT
 * interval, and an NTSTATUS becomes the thread's last error. This is that
 * translation, written in the order Wine's test programs ask for it: what
 * their harness calls before the first test runs (wine/test.h's main), then
 * what each test reaches first. A function not here is bound to one of
 * ntdll's named stubs and says so when called - which is how the next one is
 * chosen.
 *
 * Built freestanding against our ntdll alone, as msvcrt is. Exports are named
 * in kernel32.def. */

#include "vibeos_ntdll.h"

typedef int32_t BOOL32;

#define WAIT_FAILED          0xFFFFFFFFu
#define INFINITE_MS          0xFFFFFFFFu
#define INVALID_HANDLE       ((uint64_t)-1)

#define ERROR_INVALID_PARAMETER   87u
#define ERROR_INVALID_HANDLE      6u
#define ERROR_ALREADY_EXISTS      183u
#define ERROR_MOD_NOT_FOUND       126u
#define ERROR_PROC_NOT_FOUND      127u
#define ERROR_NO_MORE_ITEMS       259u
#define ERROR_ENVVAR_NOT_FOUND    203u
#define ERROR_FILENAME_EXCED_RANGE 206u

/* ---- the thread's last error ----------------------------------------------------------- */

static void set_error(uint32_t e) {
    __asm__ __volatile__("movl %0, %%gs:%c1" : : "r"(e), "i"(VIBEOS_TEB_LAST_ERROR) : "memory");
}

static uint32_t get_error(void) {
    uint32_t e;
    __asm__ __volatile__("movl %%gs:%c1, %0" : "=r"(e) : "i"(VIBEOS_TEB_LAST_ERROR));
    return e;
}

/* A failed status as the last error; returns zero, which is what most of the
 * callers then return themselves. */
static int fail(NTSTATUS_T st) {
    set_error(RtlNtStatusToDosError((vibeos_ntstatus_t)st));
    return 0;
}

static int failed(NTSTATUS_T st) {
    return (st & 0xC0000000u) == 0xC0000000u;
}

uint32_t GetLastError(void) { return get_error(); }
void SetLastError(uint32_t e) { set_error(e); }

static uint64_t peb(void) {
    return vibeos_nt_rd64(vibeos_nt_teb() + VIBEOS_TEB_PEB);
}

uint64_t GetCurrentProcess(void) { return VIBEOS_NT_CURRENT_PROCESS; }
uint64_t GetCurrentThread(void) { return VIBEOS_NT_CURRENT_THREAD; }
uint32_t GetCurrentProcessId(void) {
    return (uint32_t)vibeos_nt_rd64(vibeos_nt_teb() + VIBEOS_TEB_CLIENT_PROCESS);
}
uint32_t GetCurrentThreadId(void) {
    return (uint32_t)vibeos_nt_rd64(vibeos_nt_teb() + VIBEOS_TEB_CLIENT_THREAD);
}

__attribute__((noreturn)) void ExitProcess(uint32_t code) {
    (void)NtTerminateProcess(VIBEOS_NT_CURRENT_PROCESS, code);
    for (;;) {
        /* NtTerminateProcess of ourselves does not return. */
    }
}

/* ---- strings ---------------------------------------------------------------------------- */

/* An ANSI string as a wide one, through the code page ntdll converts with.
 * `cap` characters, the terminator included; -1 if it does not fit. */
static int32_t widen(const char *s, uint16_t *out, uint32_t cap) {
    uint32_t n = 0, got = 0;

    while (s[n]) {
        n++;
    }
    if (n + 1u > cap) {
        return -1;
    }
    (void)RtlMultiByteToUnicodeN(out, n * 2u, &got, s, n);
    out[got / 2u] = 0;
    return (int32_t)(got / 2u);
}

static uint32_t wlen(const uint16_t *s) {
    uint32_t n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

/* ---- named objects -------------------------------------------------------------------------
 * A Win32 name is an object in the session's BaseNamedObjects,
 * \Sessions\<n>\BaseNamedObjects with n from the PEB - the path Wine's tests
 * build to find what kernel32 made. The object manager has one flat namespace
 * of whole paths and no directory objects yet, so it is spelled out here
 * rather than opened as a root directory, as Windows' kernel32 does. */

#define NAME_CHARS 260u

typedef struct {
    uint8_t oa[VIBEOS_OA_SIZE];
    uint8_t us[VIBEOS_USTR_SIZE];
    uint16_t path[NAME_CHARS + 40u];   /* behind \Sessions\<n>\BaseNamedObjects\ */
} named_t;

/* "\Sessions\<n>\BaseNamedObjects\" into `out`; its length. */
static uint32_t base_named_objects(uint16_t *out) {
    static const char head[] = "\\Sessions\\", tail[] = "\\BaseNamedObjects\\";
    uint32_t session = (uint32_t)vibeos_nt_rd64(peb() + VIBEOS_PEB_SESSION_ID);
    char digits[10];
    uint32_t i, k = 0, d = 0;

    for (i = 0; head[i]; i++) {
        out[k++] = (uint8_t)head[i];
    }
    do {
        digits[d++] = (char)('0' + session % 10u);
        session /= 10u;
    } while (session);
    while (d) {
        out[k++] = (uint8_t)digits[--d];
    }
    for (i = 0; tail[i]; i++) {
        out[k++] = (uint8_t)tail[i];
    }
    return k;
}

/* OBJECT_ATTRIBUTES for `name` (null: an unnamed object). Returns 0 and sets
 * the last error if the name is too long. */
static void *attributes_w(named_t *n, const uint16_t *name, int inherit, int openif) {
    uint32_t i, k = 0;

    for (i = 0; i < sizeof(n->oa); i++) {
        n->oa[i] = 0;
    }
    *(uint32_t *)(n->oa + VIBEOS_OA_LENGTH) = VIBEOS_OA_SIZE;
    *(uint32_t *)(n->oa + VIBEOS_OA_ATTRIBUTES) =
        (inherit ? VIBEOS_OBJ_INHERIT : 0u) | (openif ? VIBEOS_OBJ_OPENIF : 0u);
    if (!name) {
        return n->oa;
    }
    if (wlen(name) >= NAME_CHARS) {
        set_error(ERROR_FILENAME_EXCED_RANGE);
        return 0;
    }
    k = base_named_objects(n->path);
    for (i = 0; name[i]; i++) {
        n->path[k++] = name[i];
    }
    for (i = 0; i < sizeof(n->us); i++) {
        n->us[i] = 0;
    }
    *(uint16_t *)(n->us + VIBEOS_USTR_LENGTH) = (uint16_t)(k * 2u);
    *(uint16_t *)(n->us + VIBEOS_USTR_MAXIMUM) = (uint16_t)(k * 2u);
    *(uint64_t *)(n->us + VIBEOS_USTR_BUFFER) = (uint64_t)(uintptr_t)n->path;
    *(uint64_t *)(n->oa + VIBEOS_OA_OBJECT_NAME) = (uint64_t)(uintptr_t)n->us;
    return n->oa;
}

static void *attributes_a(named_t *n, uint16_t *wide, const char *name, int inherit, int openif) {
    if (name && widen(name, wide, NAME_CHARS) < 0) {
        set_error(ERROR_FILENAME_EXCED_RANGE);
        return 0;
    }
    return attributes_w(n, name ? wide : 0, inherit, openif);
}

/* SECURITY_ATTRIBUTES: nLength, then the descriptor, then bInheritHandle. */
static int inherits(const void *sa) {
    return sa && *(const int32_t *)((const uint8_t *)sa + 16) != 0;
}

/* What every Create<object> ends with: the handle, and a last error that says
 * whether the name already existed - zero otherwise, as Windows sets it. */
static uint64_t created(NTSTATUS_T st, uint64_t h) {
    if (failed(st)) {
        return (uint64_t)fail(st);
    }
    set_error(st == VIBEOS_STATUS_OBJECT_NAME_EXISTS ? ERROR_ALREADY_EXISTS : 0u);
    return h;
}

static uint64_t opened(NTSTATUS_T st, uint64_t h) {
    return failed(st) ? (uint64_t)fail(st) : h;
}

/* ---- mutexes, events, semaphores ------------------------------------------------------- */

uint64_t CreateMutexW(const void *sa, BOOL32 owner, const uint16_t *name) {
    named_t n;
    uint64_t h = 0;
    void *oa = attributes_w(&n, name, inherits(sa), 1);

    if (!oa) {
        return 0;
    }
    return created(NtCreateMutant(&h, VIBEOS_MUTANT_ALL_ACCESS, oa, owner ? 1u : 0u), h);
}

uint64_t CreateMutexA(const void *sa, BOOL32 owner, const char *name) {
    named_t n;
    uint16_t w[NAME_CHARS];
    uint64_t h = 0;
    void *oa = attributes_a(&n, w, name, inherits(sa), 1);

    if (!oa) {
        return 0;
    }
    return created(NtCreateMutant(&h, VIBEOS_MUTANT_ALL_ACCESS, oa, owner ? 1u : 0u), h);
}

uint64_t OpenMutexW(uint32_t access, BOOL32 inherit, const uint16_t *name) {
    named_t n;
    uint64_t h = 0;
    void *oa;

    if (!name) {
        set_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    oa = attributes_w(&n, name, inherit, 0);
    return oa ? opened(NtOpenMutant(&h, access, oa), h) : 0;
}

uint64_t OpenMutexA(uint32_t access, BOOL32 inherit, const char *name) {
    uint16_t w[NAME_CHARS];

    if (!name) {
        set_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (widen(name, w, NAME_CHARS) < 0) {
        set_error(ERROR_FILENAME_EXCED_RANGE);
        return 0;
    }
    return OpenMutexW(access, inherit, w);
}

BOOL32 ReleaseMutex(uint64_t h) {
    NTSTATUS_T st = NtReleaseMutant(h, 0);
    return failed(st) ? fail(st) : 1;
}

uint64_t CreateEventW(const void *sa, BOOL32 manual, BOOL32 initial, const uint16_t *name) {
    named_t n;
    uint64_t h = 0;
    void *oa = attributes_w(&n, name, inherits(sa), 1);

    if (!oa) {
        return 0;
    }
    return created(NtCreateEvent(&h, VIBEOS_EVENT_ALL_ACCESS, oa,
                                 manual ? VIBEOS_NT_NOTIFICATION_EVENT
                                        : VIBEOS_NT_SYNCHRONIZATION_EVENT,
                                 initial ? 1u : 0u), h);
}

uint64_t CreateEventA(const void *sa, BOOL32 manual, BOOL32 initial, const char *name) {
    uint16_t w[NAME_CHARS];

    if (name && widen(name, w, NAME_CHARS) < 0) {
        set_error(ERROR_FILENAME_EXCED_RANGE);
        return 0;
    }
    return CreateEventW(sa, manual, initial, name ? w : 0);
}

uint64_t OpenEventW(uint32_t access, BOOL32 inherit, const uint16_t *name) {
    named_t n;
    uint64_t h = 0;
    void *oa;

    if (!name) {
        set_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    oa = attributes_w(&n, name, inherit, 0);
    return oa ? opened(NtOpenEvent(&h, access, oa), h) : 0;
}

uint64_t OpenEventA(uint32_t access, BOOL32 inherit, const char *name) {
    uint16_t w[NAME_CHARS];

    if (!name) {
        set_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (widen(name, w, NAME_CHARS) < 0) {
        set_error(ERROR_FILENAME_EXCED_RANGE);
        return 0;
    }
    return OpenEventW(access, inherit, w);
}

BOOL32 SetEvent(uint64_t h) {
    NTSTATUS_T st = NtSetEvent(h, 0);
    return failed(st) ? fail(st) : 1;
}

BOOL32 ResetEvent(uint64_t h) {
    NTSTATUS_T st = NtResetEvent(h, 0);
    return failed(st) ? fail(st) : 1;
}

BOOL32 PulseEvent(uint64_t h) {
    NTSTATUS_T st = NtPulseEvent(h, 0);
    return failed(st) ? fail(st) : 1;
}

uint64_t CreateSemaphoreW(const void *sa, int32_t initial, int32_t maximum,
                          const uint16_t *name) {
    named_t n;
    uint64_t h = 0;
    void *oa = attributes_w(&n, name, inherits(sa), 1);

    if (!oa) {
        return 0;
    }
    return created(NtCreateSemaphore(&h, VIBEOS_SEMAPHORE_ALL_ACCESS, oa, initial, maximum), h);
}

uint64_t CreateSemaphoreA(const void *sa, int32_t initial, int32_t maximum, const char *name) {
    uint16_t w[NAME_CHARS];

    if (name && widen(name, w, NAME_CHARS) < 0) {
        set_error(ERROR_FILENAME_EXCED_RANGE);
        return 0;
    }
    return CreateSemaphoreW(sa, initial, maximum, name ? w : 0);
}

BOOL32 ReleaseSemaphore(uint64_t h, int32_t count, int32_t *previous) {
    NTSTATUS_T st = NtReleaseSemaphore(h, count, previous);
    return failed(st) ? fail(st) : 1;
}

/* ---- handles ------------------------------------------------------------------------------ */

BOOL32 CloseHandle(uint64_t h) {
    NTSTATUS_T st = NtClose(h);
    return failed(st) ? fail(st) : 1;
}

BOOL32 DuplicateHandle(uint64_t src_process, uint64_t src, uint64_t dst_process, uint64_t *dst,
                       uint32_t access, BOOL32 inherit, uint32_t options) {
    NTSTATUS_T st = NtDuplicateObject(src_process, src, dst_process, dst, access,
                                      inherit ? VIBEOS_OBJ_INHERIT : 0u, options);
    return failed(st) ? fail(st) : 1;
}

/* STD_INPUT_HANDLE (-10), STD_OUTPUT_HANDLE (-11), STD_ERROR_HANDLE (-12):
 * the process parameters' three handles. */
uint64_t GetStdHandle(uint32_t which) {
    uint64_t params = vibeos_nt_rd64(peb() + VIBEOS_PEB_PROCESS_PARAMS);

    switch (which) {
        case (uint32_t)-10:
            return vibeos_nt_rd64(params + VIBEOS_RUPP_STD_INPUT);
        case (uint32_t)-11:
            return vibeos_nt_rd64(params + VIBEOS_RUPP_STD_OUTPUT);
        case (uint32_t)-12:
            return vibeos_nt_rd64(params + VIBEOS_RUPP_STD_ERROR);
        default:
            set_error(ERROR_INVALID_HANDLE);
            return INVALID_HANDLE;
    }
}

BOOL32 WriteFile(uint64_t h, const void *buf, uint32_t len, uint32_t *written, void *overlapped) {
    vibeos_nt_iosb_t iosb;
    NTSTATUS_T st;

    (void)overlapped;   /* asynchronous I/O is W4's */
    if (written) {
        *written = 0;
    }
    st = NtWriteFile(h, 0, 0, 0, &iosb, buf, len, 0, 0);
    if (failed(st)) {
        return fail(st);
    }
    if (written) {
        *written = (uint32_t)iosb.information;
    }
    return 1;
}

/* ---- written ahead of the kernel -----------------------------------------------------------
 * Files are W4's and threads W5's, and Wine's tests for W2 reach both early -
 * ntdll:om a named pipe in its third test, the sync tests a thread in their
 * second. So these are written now, over services the kernel answers
 * STATUS_NOT_IMPLEMENTED and counts: the program is told the call failed and
 * the test goes on, its failures named, as an unassigned Linux number answered
 * ENOSYS. Each becomes real when its service does, and nothing here changes. */

/* CreateFile's creation disposition as NtCreateFile's. */
static uint32_t nt_disposition(uint32_t d) {
    switch (d) {
        case 1u: return 2u;   /* CREATE_NEW: FILE_CREATE              */
        case 2u: return 5u;   /* CREATE_ALWAYS: FILE_OVERWRITE_IF     */
        case 3u: return 1u;   /* OPEN_EXISTING: FILE_OPEN             */
        case 4u: return 3u;   /* OPEN_ALWAYS: FILE_OPEN_IF            */
        case 5u: return 4u;   /* TRUNCATE_EXISTING: FILE_OVERWRITE    */
        default: return 0xFFFFFFFFu;
    }
}

uint64_t CreateFileW(const uint16_t *name, uint32_t access, uint32_t share, const void *sa,
                     uint32_t disposition, uint32_t flags, uint64_t template_file) {
    named_t n;
    vibeos_ustring_t nt_name;
    vibeos_nt_iosb_t iosb;
    uint64_t h = 0;
    uint32_t d = nt_disposition(disposition), options = 0x40u;   /* FILE_NON_DIRECTORY_FILE */
    NTSTATUS_T st;

    (void)template_file;
    if (!name || d == 0xFFFFFFFFu) {
        set_error(!name ? 3u /* ERROR_PATH_NOT_FOUND */ : ERROR_INVALID_PARAMETER);
        return INVALID_HANDLE;
    }
    if (!RtlDosPathNameToNtPathName_U(name, &nt_name, 0, 0)) {
        set_error(3u);
        return INVALID_HANDLE;
    }
    if (!(flags & 0x40000000u)) {
        options |= 0x20u;     /* not FILE_FLAG_OVERLAPPED: FILE_SYNCHRONOUS_IO_NONALERT */
    }
    if (flags & 0x04000000u) {
        options |= 0x1000u;   /* FILE_FLAG_DELETE_ON_CLOSE */
    }
    (void)attributes_w(&n, 0, inherits(sa), 0);
    *(uint64_t *)(n.oa + VIBEOS_OA_OBJECT_NAME) = (uint64_t)(uintptr_t)&nt_name;
    *(uint32_t *)(n.oa + VIBEOS_OA_ATTRIBUTES) |= VIBEOS_OBJ_CASE_INSENSITIVE;
    st = NtCreateFile(&h, access | VIBEOS_SYNCHRONIZE | 0x80u /* FILE_READ_ATTRIBUTES */, n.oa,
                      &iosb, 0, flags & 0xFFFFu, share, d, options, 0, 0);
    RtlFreeUnicodeString(&nt_name);
    if (failed(st)) {
        fail(st);
        return INVALID_HANDLE;
    }
    /* OPEN_ALWAYS and CREATE_ALWAYS say whether the file was there already. */
    set_error((disposition == 2u || disposition == 4u) && iosb.information != 2u /* FILE_CREATED */
                  ? ERROR_ALREADY_EXISTS : 0u);
    return h;
}

uint64_t CreateFileA(const char *name, uint32_t access, uint32_t share, const void *sa,
                     uint32_t disposition, uint32_t flags, uint64_t template_file) {
    uint16_t w[NAME_CHARS];

    if (!name || widen(name, w, NAME_CHARS) < 0) {
        set_error(!name ? 3u : ERROR_FILENAME_EXCED_RANGE);
        return INVALID_HANDLE;
    }
    return CreateFileW(w, access, share, sa, disposition, flags, template_file);
}

uint64_t CreateThread(const void *sa, size_t stack, void *start, void *param, uint32_t flags,
                      uint32_t *tid) {
    named_t n;
    uint64_t h = 0;
    NTSTATUS_T st;

    (void)attributes_w(&n, 0, inherits(sa), 0);
    st = NtCreateThreadEx(&h, 0x001FFFFFu /* THREAD_ALL_ACCESS */, n.oa, VIBEOS_NT_CURRENT_PROCESS,
                          start, param, (flags & 4u) ? 1u : 0u /* CREATE_SUSPENDED */, 0,
                          (flags & 0x10000u) ? 0 : stack, (flags & 0x10000u) ? stack : 0, 0);
    if (failed(st)) {
        return (uint64_t)fail(st);
    }
    if (tid) {
        *tid = 0;   /* the thread's id comes with W5's NtQueryInformationThread */
    }
    return h;
}

/* ---- waiting and sleeping ------------------------------------------------------------- */

/* Milliseconds as an NT interval: relative, so negative; INFINITE is none. */
static const int64_t *interval(uint32_t ms, int64_t *t) {
    if (ms == INFINITE_MS) {
        return 0;
    }
    *t = -(int64_t)ms * 10000;
    return t;
}

/* A wait's status is its answer - WAIT_OBJECT_0 + i, WAIT_ABANDONED_0 + i,
 * WAIT_TIMEOUT - and a failure is WAIT_FAILED with the last error set. */
static uint32_t waited(NTSTATUS_T st) {
    if (failed(st)) {
        fail(st);
        return WAIT_FAILED;
    }
    return st;
}

uint32_t WaitForSingleObjectEx(uint64_t h, uint32_t ms, BOOL32 alertable) {
    int64_t t;
    return waited(NtWaitForSingleObject(h, alertable ? 1u : 0u, interval(ms, &t)));
}

uint32_t WaitForSingleObject(uint64_t h, uint32_t ms) {
    return WaitForSingleObjectEx(h, ms, 0);
}

uint32_t WaitForMultipleObjectsEx(uint32_t count, const uint64_t *handles, BOOL32 all,
                                  uint32_t ms, BOOL32 alertable) {
    int64_t t;
    return waited(NtWaitForMultipleObjects(count, handles, all ? 0u : 1u, alertable ? 1u : 0u,
                                           interval(ms, &t)));
}

uint32_t WaitForMultipleObjects(uint32_t count, const uint64_t *handles, BOOL32 all, uint32_t ms) {
    return WaitForMultipleObjectsEx(count, handles, all, ms, 0);
}

uint32_t SleepEx(uint32_t ms, BOOL32 alertable) {
    int64_t t = 0;
    const int64_t *p = interval(ms, &t);
    int64_t forever = (int64_t)0x8000000000000000ull;   /* the most negative: never */
    NTSTATUS_T st = NtDelayExecution(alertable ? 1u : 0u, p ? p : &forever);

    return st == VIBEOS_STATUS_USER_APC ? 0xC0u /* WAIT_IO_COMPLETION */ : 0u;
}

void Sleep(uint32_t ms) {
    (void)SleepEx(ms, 0);
}

/* Milliseconds since the machine started, from the performance counter. */
uint64_t GetTickCount64(void) {
    int64_t c = 0, f = 1;

    (void)NtQueryPerformanceCounter(&c, &f);
    if (f <= 0) {
        return 0;
    }
    return (uint64_t)(c / f) * 1000u + (uint64_t)((c % f) * 1000 / f);
}

uint32_t GetTickCount(void) {
    return (uint32_t)GetTickCount64();
}

/* ---- modules -------------------------------------------------------------------------------- */

uint64_t GetModuleHandleW(const uint16_t *name) {
    vibeos_ustring_t us;
    void *h = 0;
    NTSTATUS_T st;

    if (!name) {
        return vibeos_nt_rd64(peb() + VIBEOS_PEB_IMAGE_BASE);
    }
    us.Length = (uint16_t)(wlen(name) * 2u);
    us.MaximumLength = us.Length;
    us.Buffer = (vibeos_wchar_t *)name;
    st = LdrGetDllHandle(0, 0, &us, &h);
    if (failed(st)) {
        set_error(ERROR_MOD_NOT_FOUND);
        return 0;
    }
    return (uint64_t)(uintptr_t)h;
}

uint64_t GetModuleHandleA(const char *name) {
    uint16_t w[NAME_CHARS];

    if (!name) {
        return GetModuleHandleW(0);
    }
    if (widen(name, w, NAME_CHARS) < 0) {
        set_error(ERROR_MOD_NOT_FOUND);
        return 0;
    }
    return GetModuleHandleW(w);
}

/* A name, or an ordinal in the low word of the pointer. */
void *GetProcAddress(uint64_t module, const char *name) {
    vibeos_astring_t as;
    void *at = 0;
    NTSTATUS_T st;

    if (!module) {
        module = GetModuleHandleW(0);
    }
    if (((uintptr_t)name >> 16) == 0u) {
        st = LdrGetProcedureAddress((void *)(uintptr_t)module, 0, (uint32_t)(uintptr_t)name, &at);
    } else {
        RtlInitString(&as, name);
        st = LdrGetProcedureAddress((void *)(uintptr_t)module, &as, 0, &at);
    }
    if (failed(st)) {
        set_error(st == VIBEOS_STATUS_DLL_NOT_FOUND ? ERROR_MOD_NOT_FOUND : ERROR_PROC_NOT_FOUND);
        return 0;
    }
    return at;
}

uint64_t LoadLibraryW(const uint16_t *name) {
    vibeos_ustring_t us;
    void *h = 0;
    NTSTATUS_T st;

    if (!name) {
        set_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    us.Length = (uint16_t)(wlen(name) * 2u);
    us.MaximumLength = us.Length;
    us.Buffer = (vibeos_wchar_t *)name;
    st = LdrLoadDll(0, 0, &us, &h);
    return failed(st) ? (uint64_t)fail(st) : (uint64_t)(uintptr_t)h;
}

uint64_t LoadLibraryA(const char *name) {
    uint16_t w[NAME_CHARS];

    if (!name) {
        set_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (widen(name, w, NAME_CHARS) < 0) {
        set_error(ERROR_FILENAME_EXCED_RANGE);
        return 0;
    }
    return LoadLibraryW(w);
}

/* The loader never unloads: a DLL stays for the life of the process, and
 * freeing it succeeds and changes nothing. */
BOOL32 FreeLibrary(uint64_t module) {
    if (!module) {
        set_error(ERROR_INVALID_HANDLE);
        return 0;
    }
    return 1;
}

/* Whether `max` characters at `s` - or up to its terminator - can be read.
 * Windows answers by catching the fault, and there is no exception dispatch
 * until W5: so what is answered here is the part that needs none - a null
 * pointer, or one in the first 64 KiB that nothing maps, is bad - and any
 * other string is read. A bad pointer elsewhere therefore ends the process
 * with its fault instead of being answered TRUE: a wrong answer that is loud,
 * never one that is quiet. */
BOOL32 IsBadStringPtrA(const char *str, uintptr_t max) {
    const volatile char *s = str;   /* read, not optimised away */
    uintptr_t i;

    if ((uintptr_t)s < 0x10000u) {
        return max != 0u;
    }
    for (i = 0; i < max && s[i]; i++) {
    }
    return 0;
}

BOOL32 IsBadStringPtrW(const uint16_t *str, uintptr_t max) {
    const volatile uint16_t *s = str;
    uintptr_t i;

    if ((uintptr_t)s < 0x10000u) {
        return max != 0u;
    }
    for (i = 0; i < max && s[i]; i++) {
    }
    return 0;
}

/* ---- the heap ------------------------------------------------------------------------------ */

uint64_t GetProcessHeap(void) {
    return vibeos_nt_rd64(peb() + VIBEOS_PEB_PROCESS_HEAP);
}

void *HeapAlloc(uint64_t heap, uint32_t flags, size_t size) {
    return RtlAllocateHeap((void *)(uintptr_t)heap, flags, size);
}

BOOL32 HeapFree(uint64_t heap, uint32_t flags, void *p) {
    return RtlFreeHeap((void *)(uintptr_t)heap, flags, p) ? 1 : 0;
}

void *HeapReAlloc(uint64_t heap, uint32_t flags, void *p, size_t size) {
    return RtlReAllocateHeap((void *)(uintptr_t)heap, flags, p, size);
}

size_t HeapSize(uint64_t heap, uint32_t flags, const void *p) {
    return RtlSizeHeap((void *)(uintptr_t)heap, flags, p);
}

/* Every block the arena handed out is well formed or refused on free; there is
 * nothing more to walk. A pointer that is not a block is invalid. */
BOOL32 HeapValidate(uint64_t heap, uint32_t flags, const void *p) {
    return !p || RtlSizeHeap((void *)(uintptr_t)heap, flags, p) != (size_t)-1;
}

/* ---- thread-local storage ------------------------------------------------------------------
 * The TEB's sixty-four slots. With one thread per process (W5 brings more) the
 * bitmap of slots in use is this DLL's, and freeing a slot clears it in the
 * only TEB there is. The expansion slots past sixty-four are not provided. */

static uint64_t g_tls_used;

static volatile uint64_t *tls_slot(uint32_t i) {
    return (volatile uint64_t *)(uintptr_t)(vibeos_nt_teb() + VIBEOS_TEB_TLS_SLOTS + 8u * i);
}

uint32_t TlsAlloc(void) {
    uint32_t i;

    for (i = 0; i < VIBEOS_TEB_TLS_SLOT_COUNT; i++) {
        if (!(g_tls_used & (1ull << i))) {
            g_tls_used |= 1ull << i;
            *tls_slot(i) = 0;
            set_error(0);
            return i;
        }
    }
    set_error(ERROR_NO_MORE_ITEMS);
    return 0xFFFFFFFFu;   /* TLS_OUT_OF_INDEXES */
}

BOOL32 TlsFree(uint32_t i) {
    if (i >= VIBEOS_TEB_TLS_SLOT_COUNT || !(g_tls_used & (1ull << i))) {
        set_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    g_tls_used &= ~(1ull << i);
    *tls_slot(i) = 0;
    return 1;
}

/* Success clears the last error: a slot holding zero and a failure both return
 * zero, and that is how a caller tells them apart. */
void *TlsGetValue(uint32_t i) {
    if (i >= VIBEOS_TEB_TLS_SLOT_COUNT) {
        set_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    set_error(0);
    return (void *)(uintptr_t)*tls_slot(i);
}

BOOL32 TlsSetValue(uint32_t i, void *v) {
    if (i >= VIBEOS_TEB_TLS_SLOT_COUNT) {
        set_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    *tls_slot(i) = (uint64_t)(uintptr_t)v;
    return 1;
}

/* ---- the environment ------------------------------------------------------------------------ */

/* The value's length in characters if it fitted, without the terminator; the
 * size it needs, with the terminator, if it did not; zero and
 * ERROR_ENVVAR_NOT_FOUND if there is no such variable. */
uint32_t GetEnvironmentVariableW(const uint16_t *name, uint16_t *buf, uint32_t size) {
    vibeos_ustring_t n, v;
    NTSTATUS_T st;
    uint32_t cap = size > 0x7FFFu ? 0x7FFFu : size;

    n.Length = (uint16_t)(wlen(name) * 2u);
    n.MaximumLength = n.Length;
    n.Buffer = (vibeos_wchar_t *)name;
    v.Length = 0;
    v.MaximumLength = (uint16_t)(cap * 2u);
    v.Buffer = buf;
    st = (NTSTATUS_T)RtlQueryEnvironmentVariable_U(0, &n, &v);
    if (st == VIBEOS_STATUS_BUFFER_TOO_SMALL || (st == 0 && v.Length / 2u >= cap)) {
        return v.Length / 2u + 1u;
    }
    if (failed(st)) {
        set_error(st == VIBEOS_STATUS_VARIABLE_NOT_FOUND ? ERROR_ENVVAR_NOT_FOUND
                                                          : RtlNtStatusToDosError((vibeos_ntstatus_t)st));
        return 0;
    }
    buf[v.Length / 2u] = 0;
    return v.Length / 2u;
}

/* A wide character in the ANSI code page ntdll widens with, Windows-1252: the
 * same value outside 0x80-0x9F, one of those 32 bytes for the characters
 * 1252 puts there, '?' for anything it has no byte for - Windows' default
 * character. */
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

/* Through the wide form, the value narrowed a character at a time. */
uint32_t GetEnvironmentVariableA(const char *name, char *buf, uint32_t size) {
    uint16_t wn[NAME_CHARS], wv[1024];
    uint32_t r, i;

    if (widen(name, wn, NAME_CHARS) < 0) {
        set_error(ERROR_ENVVAR_NOT_FOUND);
        return 0;
    }
    r = GetEnvironmentVariableW(wn, wv, sizeof(wv) / 2u);
    if (r == 0u || r >= sizeof(wv) / 2u) {
        return r;
    }
    if (r + 1u > size) {
        return r + 1u;
    }
    for (i = 0; i <= r; i++) {
        buf[i] = narrow(wv[i]);
    }
    return r;
}

/* ---- process settings that nothing acts on yet ---------------------------------------------
 * Kept and returned, as a program expects to read back what it set. The error
 * mode decides which dialogs Windows shows, and there are none; the
 * unhandled-exception filter is called by exception dispatch, which is W5's. */

static uint32_t g_error_mode;
static void *g_exception_filter;

uint32_t SetErrorMode(uint32_t mode) {
    uint32_t old = g_error_mode;
    g_error_mode = mode;
    return old;
}

uint32_t GetErrorMode(void) {
    return g_error_mode;
}

void *SetUnhandledExceptionFilter(void *filter) {
    void *old = g_exception_filter;
    g_exception_filter = filter;
    return old;
}

int vibeos_kernel32_main(uint64_t instance, uint32_t reason, void *reserved) {
    (void)instance;
    (void)reason;
    (void)reserved;
    return 1;
}
