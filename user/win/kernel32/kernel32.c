/* SPDX-License-Identifier: GPL-3.0-or-later */
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
 * whether the name already existed - zero otherwise, as Windows sets it.
 *
 * The handle by its address, read here: these are called as
 * created(NtCreateX(&h, ...), &h), and C leaves the order its arguments are
 * evaluated in to the compiler. Passed by value, `h` could be read before the
 * call that writes it - and was: CreateFileMapping made its section and
 * returned 0 with no error, found by Wine's kernel32:virtual (W3 step 9),
 * while the same shape happened to work for the events. */
static uint64_t created(NTSTATUS_T st, const uint64_t *h) {
    if (failed(st)) {
        return (uint64_t)fail(st);
    }
    set_error(st == VIBEOS_STATUS_OBJECT_NAME_EXISTS ? ERROR_ALREADY_EXISTS : 0u);
    return *h;
}

static uint64_t opened(NTSTATUS_T st, const uint64_t *h) {
    return failed(st) ? (uint64_t)fail(st) : *h;
}

/* ---- mutexes, events, semaphores ------------------------------------------------------- */

uint64_t CreateMutexW(const void *sa, BOOL32 owner, const uint16_t *name) {
    named_t n;
    uint64_t h = 0;
    void *oa = attributes_w(&n, name, inherits(sa), 1);

    if (!oa) {
        return 0;
    }
    return created(NtCreateMutant(&h, VIBEOS_MUTANT_ALL_ACCESS, oa, owner ? 1u : 0u), &h);
}

uint64_t CreateMutexA(const void *sa, BOOL32 owner, const char *name) {
    named_t n;
    uint16_t w[NAME_CHARS];
    uint64_t h = 0;
    void *oa = attributes_a(&n, w, name, inherits(sa), 1);

    if (!oa) {
        return 0;
    }
    return created(NtCreateMutant(&h, VIBEOS_MUTANT_ALL_ACCESS, oa, owner ? 1u : 0u), &h);
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
    return oa ? opened(NtOpenMutant(&h, access, oa), &h) : 0;
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
                                 initial ? 1u : 0u), &h);
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
    return oa ? opened(NtOpenEvent(&h, access, oa), &h) : 0;
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
    return created(NtCreateSemaphore(&h, VIBEOS_SEMAPHORE_ALL_ACCESS, oa, initial, maximum), &h);
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

uint32_t GetThreadId(uint64_t h);   /* below, with the thread calls; CreateThread reports it */

/* A thread in `process` - this one, or another (W7 step 2). */
uint64_t CreateRemoteThread(uint64_t process, const void *sa, size_t stack, void *start, void *param,
                            uint32_t flags, uint32_t *tid) {
    named_t n;
    uint64_t h = 0;
    NTSTATUS_T st;

    (void)attributes_w(&n, 0, inherits(sa), 0);
    st = NtCreateThreadEx(&h, 0x001FFFFFu /* THREAD_ALL_ACCESS */, n.oa, process,
                          start, param, (flags & 4u) ? 1u : 0u /* CREATE_SUSPENDED */, 0,
                          (flags & 0x10000u) ? 0 : stack, (flags & 0x10000u) ? stack : 0, 0);
    if (failed(st)) {
        return (uint64_t)fail(st);
    }
    if (tid) {
        *tid = GetThreadId(h);
    }
    return h;
}

uint64_t CreateThread(const void *sa, size_t stack, void *start, void *param, uint32_t flags,
                      uint32_t *tid) {
    return CreateRemoteThread(VIBEOS_NT_CURRENT_PROCESS, sa, stack, start, param, flags, tid);
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

/* ---- fiber-local storage (W5 step 6) --------------------------------------------------------
 * With no fibers, a fiber's slots are its thread's: FlsAlloc's index names a
 * TLS slot of this DLL's, and the callback runs for the calling thread's value
 * when the index is freed. A thread's own end does not run the callbacks yet,
 * which leaks what they would free and nothing else. */

static void (*g_fls_callback[VIBEOS_TEB_TLS_SLOT_COUNT])(void *);

uint32_t FlsAlloc(void (*callback)(void *)) {
    uint32_t i = TlsAlloc();

    if (i != 0xFFFFFFFFu) {
        g_fls_callback[i] = callback;
    }
    return i;
}

BOOL32 FlsFree(uint32_t i) {
    void *v;

    if (i >= VIBEOS_TEB_TLS_SLOT_COUNT || !(g_tls_used & (1ull << i))) {
        set_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    v = (void *)(uintptr_t)*tls_slot(i);
    if (v && g_fls_callback[i]) {
        g_fls_callback[i](v);
    }
    g_fls_callback[i] = 0;
    return TlsFree(i);
}

void *FlsGetValue(uint32_t i) {
    return TlsGetValue(i);
}

BOOL32 FlsSetValue(uint32_t i, void *v) {
    if (i >= VIBEOS_TEB_TLS_SLOT_COUNT || !(g_tls_used & (1ull << i))) {
        set_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    return TlsSetValue(i, v);
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
    /* An empty value is a zero too, with no error: told apart by the error
     * the wide form leaves untouched when it succeeds (kernel32:environ). */
    {
        uint32_t saved = get_error();

        set_error(0xFFFFFFFEu);
        r = GetEnvironmentVariableW(wn, wv, sizeof(wv) / 2u);
        if (r == 0u && get_error() != 0xFFFFFFFEu) {
            return 0;
        }
        set_error(saved);
    }
    if (r >= sizeof(wv) / 2u) {
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

/* ---- virtual memory (W3) ------------------------------------------------------------------
 * The Nt calls with Win32's shape: an address back instead of a status, the
 * status as the last error. The one rule kernel32 adds is VirtualFree's: a
 * release names the whole allocation, so its size must be zero. */

void *VirtualAllocEx(uint64_t process, void *addr, uint64_t size, uint32_t type,
                     uint32_t protect) {
    uint64_t base = (uint64_t)(uintptr_t)addr, len = size;
    NTSTATUS_T st = NtAllocateVirtualMemory(process, &base, 0, &len, type, protect);

    if (failed(st)) {
        return (void *)(uintptr_t)fail(st);
    }
    return (void *)(uintptr_t)base;
}

void *VirtualAlloc(void *addr, uint64_t size, uint32_t type, uint32_t protect) {
    return VirtualAllocEx(VIBEOS_NT_CURRENT_PROCESS, addr, size, type, protect);
}

BOOL32 VirtualFreeEx(uint64_t process, void *addr, uint64_t size, uint32_t type) {
    uint64_t base = (uint64_t)(uintptr_t)addr, len = size;
    NTSTATUS_T st;

    if (type == VIBEOS_NT_MEM_RELEASE && size != 0u) {
        set_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    st = NtFreeVirtualMemory(process, &base, &len, type);
    return failed(st) ? fail(st) : 1;
}

BOOL32 VirtualFree(void *addr, uint64_t size, uint32_t type) {
    return VirtualFreeEx(VIBEOS_NT_CURRENT_PROCESS, addr, size, type);
}

BOOL32 VirtualProtectEx(uint64_t process, void *addr, uint64_t size, uint32_t protect,
                        uint32_t *old) {
    uint64_t base = (uint64_t)(uintptr_t)addr, len = size;
    NTSTATUS_T st = NtProtectVirtualMemory(process, &base, &len, protect, old);

    return failed(st) ? fail(st) : 1;
}

BOOL32 VirtualProtect(void *addr, uint64_t size, uint32_t protect, uint32_t *old) {
    return VirtualProtectEx(VIBEOS_NT_CURRENT_PROCESS, addr, size, protect, old);
}

/* The bytes written - the structure's size - or 0 with the last error set. */
uint64_t VirtualQueryEx(uint64_t process, const void *addr, void *info, uint64_t len) {
    uint64_t got = 0;
    NTSTATUS_T st = NtQueryVirtualMemory(process, (uint64_t)(uintptr_t)addr,
                                         VIBEOS_NT_MEMORY_BASIC_INFORMATION, info, len, &got);

    if (failed(st)) {
        return (uint64_t)fail(st);
    }
    return got;
}

uint64_t VirtualQuery(const void *addr, void *info, uint64_t len) {
    return VirtualQueryEx(VIBEOS_NT_CURRENT_PROCESS, addr, info, len);
}

/* ---- another process's memory (W7 step 1) ------------------------------------------------
 * NtRead/WriteVirtualMemory with Win32's shape. A partial copy is a failure
 * here, though its status is only a warning: the count says how far it got. */

BOOL32 ReadProcessMemory(uint64_t process, const void *addr, void *buf, uint64_t size,
                         uint64_t *done) {
    NTSTATUS_T st = NtReadVirtualMemory(process, (uint64_t)(uintptr_t)addr, buf, size, done);

    return st != 0 ? fail(st) : 1;
}

/* Windows writes into code a debugger patches: a page that is not writable is
 * made so for the write and given its protection back, as Wine's kernelbase
 * does - only the range's first region is asked about, as there. */
BOOL32 WriteProcessMemory(uint64_t process, void *addr, const void *buf, uint64_t size,
                          uint64_t *done) {
    vibeos_nt_memory_basic_t mbi;
    uint32_t old = 0, prot;
    NTSTATUS_T st;
    int changed = 0;

    if (!VirtualQueryEx(process, addr, &mbi, sizeof(mbi))) {
        return 0;
    }
    prot = mbi.protect & 0xFFu;
    if (prot == VIBEOS_NT_PAGE_READONLY || prot == VIBEOS_NT_PAGE_EXECUTE ||
        prot == VIBEOS_NT_PAGE_EXECUTE_READ) {
        uint32_t want = prot == VIBEOS_NT_PAGE_READONLY ? VIBEOS_NT_PAGE_READWRITE
                                                        : VIBEOS_NT_PAGE_EXECUTE_READWRITE;

        if (!VirtualProtectEx(process, addr, size, want, &old)) {
            return 0;
        }
        changed = 1;
    }
    st = NtWriteVirtualMemory(process, (uint64_t)(uintptr_t)addr, buf, size, done);
    if (changed) {
        uint32_t ignored;

        (void)VirtualProtectEx(process, addr, size, old, &ignored);
    }
    return st != 0 ? fail(st) : 1;
}

/* ---- file mappings (W3 step 5) ---------------------------------------------------------
 * A file mapping is a section; a view of one is NtMapViewOfSection's. The
 * protection a mapping is created with carries its SEC_* flags in the high
 * bits, SEC_COMMIT when it names none; the access a view asks for picks the
 * view's protection. */

#define SEC_MASK        0xFFFF0000u
#define SEC_COMMIT_K32  0x08000000u
#define FILE_MAP_COPY   0x0001u
#define FILE_MAP_WRITE  0x0002u
#define FILE_MAP_READ   0x0004u
#define FILE_MAP_EXECUTE 0x0020u
#define SECTION_QUERY_K32 0x0001u
#define SECTION_MAP_WRITE_K32 0x0002u
#define SECTION_MAP_READ_K32  0x0004u
#define SECTION_MAP_EXECUTE_K32 0x0008u
#define STANDARD_RIGHTS_REQUIRED_K32 0x000F0000u

/* The rights a mapping's handle is made with: what its protection allows. */
static uint32_t mapping_access(uint32_t page) {
    uint32_t a = STANDARD_RIGHTS_REQUIRED_K32 | SECTION_QUERY_K32 | SECTION_MAP_READ_K32;

    if (page & (VIBEOS_NT_PAGE_READWRITE | VIBEOS_NT_PAGE_EXECUTE_READWRITE)) {
        a |= SECTION_MAP_WRITE_K32;
    }
    if (page & (VIBEOS_NT_PAGE_EXECUTE | VIBEOS_NT_PAGE_EXECUTE_READ |
                VIBEOS_NT_PAGE_EXECUTE_READWRITE | VIBEOS_NT_PAGE_EXECUTE_WRITECOPY)) {
        a |= SECTION_MAP_EXECUTE_K32;
    }
    return a;
}

uint64_t CreateFileMappingW(uint64_t file, const void *sa, uint32_t protect, uint32_t size_hi,
                            uint32_t size_lo, const uint16_t *name) {
    named_t n;
    uint64_t h = 0;
    int64_t size = (int64_t)(((uint64_t)size_hi << 32) | size_lo);
    uint32_t sec = protect & SEC_MASK, page = protect & ~SEC_MASK;
    void *oa = attributes_w(&n, name, inherits(sa), 1);

    if (!oa) {
        return 0;
    }
    if (file == INVALID_HANDLE) {
        file = 0;   /* memory alone: the paging file's, on Windows */
    }
    return created(NtCreateSection(&h, mapping_access(page), oa, size ? &size : 0, page,
                                   sec ? sec : SEC_COMMIT_K32, file), &h);
}

uint64_t CreateFileMappingA(uint64_t file, const void *sa, uint32_t protect, uint32_t size_hi,
                            uint32_t size_lo, const char *name) {
    uint16_t w[NAME_CHARS];

    if (name && widen(name, w, NAME_CHARS) < 0) {
        set_error(ERROR_FILENAME_EXCED_RANGE);
        return 0;
    }
    return CreateFileMappingW(file, sa, protect, size_hi, size_lo, name ? w : 0);
}

uint64_t OpenFileMappingW(uint32_t access, BOOL32 inherit, const uint16_t *name) {
    named_t n;
    uint64_t h = 0;
    void *oa;

    if (!name) {
        set_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    /* FILE_MAP_* are the section's rights by number; FILE_MAP_COPY reads. */
    if (access & FILE_MAP_COPY) {
        access = (access & ~FILE_MAP_COPY) | FILE_MAP_READ;
    }
    oa = attributes_w(&n, name, inherit, 0);
    return oa ? opened(NtOpenSection(&h, access, oa), &h) : 0;
}

uint64_t OpenFileMappingA(uint32_t access, BOOL32 inherit, const char *name) {
    uint16_t w[NAME_CHARS];

    if (!name) {
        set_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (widen(name, w, NAME_CHARS) < 0) {
        set_error(ERROR_FILENAME_EXCED_RANGE);
        return 0;
    }
    return OpenFileMappingW(access, inherit, w);
}

void *MapViewOfFileEx(uint64_t mapping, uint32_t access, uint32_t off_hi, uint32_t off_lo,
                      uint64_t bytes, void *at) {
    uint64_t base = (uint64_t)(uintptr_t)at, size = bytes;
    int64_t off = (int64_t)(((uint64_t)off_hi << 32) | off_lo);
    uint32_t page;
    NTSTATUS_T st;

    if (access & FILE_MAP_COPY) {
        page = VIBEOS_NT_PAGE_WRITECOPY;
    } else if (access & FILE_MAP_WRITE) {
        page = VIBEOS_NT_PAGE_READWRITE;
    } else {
        page = VIBEOS_NT_PAGE_READONLY;
    }
    if (access & FILE_MAP_EXECUTE) {
        page <<= 4;   /* READONLY to EXECUTE_READ, and so on: Windows' own encoding */
    }
    st = NtMapViewOfSection(mapping, VIBEOS_NT_CURRENT_PROCESS, &base, 0, 0, &off, &size, 1, 0,
                            page);
    if (failed(st)) {
        return (void *)(uintptr_t)fail(st);
    }
    return (void *)(uintptr_t)base;
}

void *MapViewOfFile(uint64_t mapping, uint32_t access, uint32_t off_hi, uint32_t off_lo,
                    uint64_t bytes) {
    return MapViewOfFileEx(mapping, access, off_hi, off_lo, bytes, 0);
}

BOOL32 UnmapViewOfFile(const void *base) {
    NTSTATUS_T st = NtUnmapViewOfSection(VIBEOS_NT_CURRENT_PROCESS, (uint64_t)(uintptr_t)base);

    return failed(st) ? fail(st) : 1;
}

/* ---- the system (W3 step 9) -------------------------------------------------------------
 * GetSystemInfo: SYSTEM_INFO from SystemBasicInformation, the first thing
 * Wine's kernel32:virtual asks. x64's processor architecture and type, as a
 * 64-bit Windows reports them to a 64-bit program. */

typedef struct {
    uint16_t processor_architecture;
    uint16_t reserved;
    uint32_t page_size;
    uint64_t minimum_application_address;
    uint64_t maximum_application_address;
    uint64_t active_processor_mask;
    uint32_t number_of_processors;
    uint32_t processor_type;
    uint32_t allocation_granularity;
    uint16_t processor_level;
    uint16_t processor_revision;
} k32_system_info_t;

_Static_assert(sizeof(k32_system_info_t) == 48, "SYSTEM_INFO is 48 bytes on x64");

void GetSystemInfo(void *out) {
    k32_system_info_t *si = (k32_system_info_t *)out;
    vibeos_nt_system_basic_t sbi;
    uint32_t got = 0, i;

    for (i = 0; i < sizeof(*si); i++) {
        ((uint8_t *)si)[i] = 0;
    }
    if (failed(NtQuerySystemInformation(VIBEOS_NT_SYSTEM_BASIC_INFORMATION, &sbi, sizeof(sbi),
                                        &got))) {
        return;
    }
    si->processor_architecture = 9u;   /* PROCESSOR_ARCHITECTURE_AMD64 */
    si->page_size = sbi.page_size;
    si->minimum_application_address = sbi.minimum_user_address;
    si->maximum_application_address = sbi.maximum_user_address;
    si->active_processor_mask = sbi.active_processors_mask;
    si->number_of_processors = sbi.processors;
    si->processor_type = 8664u;        /* PROCESSOR_AMD_X8664 */
    si->allocation_granularity = sbi.allocation_granularity;
    si->processor_level = 6u;
}

void GetNativeSystemInfo(void *out) {
    GetSystemInfo(out);   /* a 64-bit program is native */
}


/* ---- files (docs/abi/windows.md W4 step 8) ---------------------------------------------
 *
 * Each of these is a few NT services, as Windows' own kernel32 is: a DOS path
 * becomes an NT one through RtlDosPathNameToNtPathName_U, every name is asked
 * for without case, and a failed status becomes the last error through the
 * table ntdll keeps. Nothing here knows the VFS. */

#define INVALID_FILE_ATTRIBUTES  0xFFFFFFFFu
#define INVALID_SET_FILE_POINTER 0xFFFFFFFFu
#define ERROR_NO_MORE_FILES      18u
#define ERROR_FILE_NOT_FOUND     2u
#define ERROR_PATH_NOT_FOUND     3u

/* An NT name for the DOS path `name`, in `n`; 0 and the last error if there
 * is none. The caller frees `nt_name`. */
static void *file_attributes(named_t *n, const uint16_t *name, vibeos_ustring_t *nt_name) {
    if (!name || !RtlDosPathNameToNtPathName_U(name, nt_name, 0, 0)) {
        set_error(ERROR_PATH_NOT_FOUND);
        return 0;
    }
    (void)attributes_w(n, 0, 0, 0);
    *(uint64_t *)(n->oa + VIBEOS_OA_OBJECT_NAME) = (uint64_t)(uintptr_t)nt_name;
    *(uint32_t *)(n->oa + VIBEOS_OA_ATTRIBUTES) |= VIBEOS_OBJ_CASE_INSENSITIVE;
    return n->oa;
}

/* NtCreateFile on a DOS path, synchronous. A status. */
static NTSTATUS_T file_open(const uint16_t *name, uint32_t access, uint32_t share, uint32_t disposition,
                            uint32_t options, uint64_t *h) {
    named_t n;
    vibeos_ustring_t nt_name;
    vibeos_nt_iosb_t iosb;
    NTSTATUS_T st;

    if (!file_attributes(&n, name, &nt_name)) {
        return 0xC000003Au;   /* OBJECT_PATH_NOT_FOUND */
    }
    st = NtCreateFile(h, access | VIBEOS_SYNCHRONIZE, n.oa, &iosb, 0, 0, share, disposition,
                      options | 0x20u /* FILE_SYNCHRONOUS_IO_NONALERT */, 0, 0);
    RtlFreeUnicodeString(&nt_name);
    return st;
}

/* The A form's name, widened, or 0 with the last error set. */
static const uint16_t *wide_name(const char *name, uint16_t *w) {
    if (!name) {
        set_error(ERROR_PATH_NOT_FOUND);
        return 0;
    }
    if (widen(name, w, NAME_CHARS) < 0) {
        set_error(ERROR_FILENAME_EXCED_RANGE);
        return 0;
    }
    return w;
}

BOOL32 ReadFile(uint64_t h, void *buf, uint32_t len, uint32_t *read, void *overlapped) {
    vibeos_nt_iosb_t iosb;
    NTSTATUS_T st;

    (void)overlapped;   /* asynchronous I/O is W5's */
    if (read) {
        *read = 0;
    }
    st = NtReadFile(h, 0, 0, 0, &iosb, buf, len, 0, 0);
    if (st == 0xC0000011u) {
        return 1;   /* END_OF_FILE: a read at the end succeeds and reads nothing */
    }
    if (failed(st)) {
        return fail(st);
    }
    if (read) {
        *read = (uint32_t)iosb.information;
    }
    return 1;
}

BOOL32 FlushFileBuffers(uint64_t h) {
    vibeos_nt_iosb_t iosb;
    NTSTATUS_T st = NtFlushBuffersFile(h, &iosb);

    return failed(st) ? fail(st) : 1;
}

BOOL32 GetFileSizeEx(uint64_t h, int64_t *size) {
    vibeos_nt_iosb_t iosb;
    uint8_t std[24];
    NTSTATUS_T st = NtQueryInformationFile(h, &iosb, std, sizeof(std), 5u /* Standard */);

    if (failed(st)) {
        return fail(st);
    }
    *size = *(int64_t *)(std + 8);
    return 1;
}

uint32_t GetFileSize(uint64_t h, uint32_t *high) {
    int64_t size;

    if (!GetFileSizeEx(h, &size)) {
        return 0xFFFFFFFFu;
    }
    if (high) {
        *high = (uint32_t)((uint64_t)size >> 32);
    }
    set_error(0);   /* a low word of all ones is a size too: the caller asks */
    return (uint32_t)size;
}

BOOL32 SetFilePointerEx(uint64_t h, int64_t distance, int64_t *new_pos, uint32_t method) {
    vibeos_nt_iosb_t iosb;
    int64_t base = 0;
    NTSTATUS_T st;

    if (method == 1u) {        /* FILE_CURRENT */
        st = NtQueryInformationFile(h, &iosb, &base, 8u, 14u /* Position */);
        if (failed(st)) {
            return fail(st);
        }
    } else if (method == 2u) { /* FILE_END */
        if (!GetFileSizeEx(h, &base)) {
            return 0;
        }
    } else if (method != 0u) {
        set_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    base += distance;
    if (base < 0) {
        set_error(131u);   /* ERROR_NEGATIVE_SEEK */
        return 0;
    }
    st = NtSetInformationFile(h, &iosb, &base, 8u, 14u);
    if (failed(st)) {
        return fail(st);
    }
    if (new_pos) {
        *new_pos = base;
    }
    return 1;
}

uint32_t SetFilePointer(uint64_t h, int32_t low, int32_t *high, uint32_t method) {
    int64_t distance = high ? (int64_t)(((uint64_t)(uint32_t)*high << 32) | (uint32_t)low) : (int64_t)low;
    int64_t pos;

    if (!SetFilePointerEx(h, distance, &pos, method)) {
        return INVALID_SET_FILE_POINTER;
    }
    if (high) {
        *high = (int32_t)((uint64_t)pos >> 32);
    }
    set_error(0);
    return (uint32_t)pos;
}

BOOL32 SetEndOfFile(uint64_t h) {
    vibeos_nt_iosb_t iosb;
    int64_t pos;
    NTSTATUS_T st = NtQueryInformationFile(h, &iosb, &pos, 8u, 14u);

    if (!failed(st)) {
        st = NtSetInformationFile(h, &iosb, &pos, 8u, 20u /* EndOfFile */);
    }
    return failed(st) ? fail(st) : 1;
}

BOOL32 CreateDirectoryW(const uint16_t *name, const void *sa) {
    uint64_t h = 0;
    NTSTATUS_T st;

    (void)sa;
    st = file_open(name, 0x100000u /* SYNCHRONIZE */ | 1u /* FILE_LIST_DIRECTORY */, 3u, 2u /* FILE_CREATE */,
                   1u /* FILE_DIRECTORY_FILE */, &h);
    if (failed(st)) {
        return fail(st);
    }
    (void)NtClose(h);
    return 1;
}

BOOL32 CreateDirectoryA(const char *name, const void *sa) {
    uint16_t w[NAME_CHARS];

    return wide_name(name, w) ? CreateDirectoryW(w, sa) : 0;
}

/* DeleteFile: an open for DELETE that goes with its handle - a file, never a
 * directory - so a file somebody holds without sharing deletion stays. */
BOOL32 DeleteFileW(const uint16_t *name) {
    uint64_t h = 0;
    NTSTATUS_T st = file_open(name, VIBEOS_DELETE, 7u, 1u /* FILE_OPEN */,
                              0x40u /* NON_DIRECTORY */ | 0x1000u /* DELETE_ON_CLOSE */, &h);

    if (failed(st)) {
        return fail(st);
    }
    (void)NtClose(h);
    return 1;
}

BOOL32 DeleteFileA(const char *name) {
    uint16_t w[NAME_CHARS];

    return wide_name(name, w) ? DeleteFileW(w) : 0;
}

/* RemoveDirectory: it has to be a directory, and empty. */
BOOL32 RemoveDirectoryW(const uint16_t *name) {
    uint64_t h = 0;
    uint8_t info[8];
    vibeos_nt_iosb_t iosb;
    NTSTATUS_T st = file_open(name, VIBEOS_DELETE, 7u, 1u, 1u /* DIRECTORY_FILE */, &h);

    if (!failed(st)) {
        info[0] = 1;   /* FileDispositionInformation: DeleteFile */
        st = NtSetInformationFile(h, &iosb, info, 1u, 13u);
        (void)NtClose(h);
    }
    return failed(st) ? fail(st) : 1;
}

BOOL32 RemoveDirectoryA(const char *name) {
    uint16_t w[NAME_CHARS];

    return wide_name(name, w) ? RemoveDirectoryW(w) : 0;
}

/* MoveFile: FileRenameInformation on an open of the source, without replacing. */
BOOL32 MoveFileW(const uint16_t *from, const uint16_t *to) {
    vibeos_ustring_t nt_to;
    vibeos_nt_iosb_t iosb;
    uint64_t h = 0;
    uint8_t info[20u + 2u * (NAME_CHARS + 8u)];
    uint32_t i, len;
    NTSTATUS_T st;

    if (!to || !RtlDosPathNameToNtPathName_U(to, &nt_to, 0, 0)) {
        set_error(ERROR_PATH_NOT_FOUND);
        return 0;
    }
    len = nt_to.Length;
    if (len > 2u * (NAME_CHARS + 8u)) {
        RtlFreeUnicodeString(&nt_to);
        set_error(ERROR_FILENAME_EXCED_RANGE);
        return 0;
    }
    for (i = 0; i < 20u; i++) {
        info[i] = 0;
    }
    *(uint32_t *)(info + 16) = len;
    for (i = 0; i < len; i++) {
        info[20u + i] = ((const uint8_t *)nt_to.Buffer)[i];
    }
    RtlFreeUnicodeString(&nt_to);
    st = file_open(from, VIBEOS_DELETE, 7u, 1u, 0u, &h);
    if (!failed(st)) {
        st = NtSetInformationFile(h, &iosb, info, 20u + len, 10u /* Rename */);
        (void)NtClose(h);
    }
    return failed(st) ? fail(st) : 1;
}

BOOL32 MoveFileA(const char *from, const char *to) {
    uint16_t wf[NAME_CHARS], wt[NAME_CHARS];

    return wide_name(from, wf) && wide_name(to, wt) ? MoveFileW(wf, wt) : 0;
}

uint32_t GetFileAttributesW(const uint16_t *name) {
    named_t n;
    vibeos_ustring_t nt_name;
    uint8_t basic[40];
    NTSTATUS_T st;

    if (!file_attributes(&n, name, &nt_name)) {
        return INVALID_FILE_ATTRIBUTES;
    }
    st = NtQueryAttributesFile(n.oa, basic);
    RtlFreeUnicodeString(&nt_name);
    if (failed(st)) {
        fail(st);
        return INVALID_FILE_ATTRIBUTES;
    }
    return *(uint32_t *)(basic + 32);
}

uint32_t GetFileAttributesA(const char *name) {
    uint16_t w[NAME_CHARS];

    return wide_name(name, w) ? GetFileAttributesW(w) : INVALID_FILE_ATTRIBUTES;
}

/* WIN32_FILE_ATTRIBUTE_DATA: attributes, three times, the size high then low. */
BOOL32 GetFileAttributesExW(const uint16_t *name, uint32_t level, void *out) {
    named_t n;
    vibeos_ustring_t nt_name;
    uint8_t full[56], *o = (uint8_t *)out;
    uint32_t i;
    NTSTATUS_T st;

    if (level != 0u) {   /* GetFileExInfoStandard is the only level */
        set_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (!file_attributes(&n, name, &nt_name)) {
        return 0;
    }
    st = NtQueryFullAttributesFile(n.oa, full);
    RtlFreeUnicodeString(&nt_name);
    if (failed(st)) {
        return fail(st);
    }
    *(uint32_t *)o = *(uint32_t *)(full + 48);
    for (i = 0; i < 24u; i++) {
        o[4u + i] = full[i];   /* creation, last access, last write */
    }
    *(uint32_t *)(o + 28) = *(uint32_t *)(full + 44);
    *(uint32_t *)(o + 32) = *(uint32_t *)(full + 40);
    return 1;
}

BOOL32 GetFileAttributesExA(const char *name, uint32_t level, void *out) {
    uint16_t w[NAME_CHARS];

    return wide_name(name, w) ? GetFileAttributesExW(w, level, out) : 0;
}

/* ---- FindFirstFile: a directory handle and its mask ---------------------------------
 *
 * The find handle is the directory's own handle; NtQueryDirectoryFile keeps the
 * mask and the place with it, which is all a search is. One entry a call, in
 * FILE_DIRECTORY_INFORMATION, turned into WIN32_FIND_DATAW. */

static uint32_t find_entry(uint64_t h, const vibeos_ustring_t *mask, int restart, uint8_t *data) {
    uint8_t e[64u + 2u * NAME_CHARS];
    vibeos_nt_iosb_t iosb;
    uint32_t i, n;
    NTSTATUS_T st;

    st = NtQueryDirectoryFile(h, 0, 0, 0, &iosb, e, sizeof(e), 1u /* Directory */, 1, mask, restart);
    if (failed(st) || st == 0x80000006u /* NO_MORE_FILES */) {
        return st;
    }
    for (i = 0; i < 592u; i++) {
        data[i] = 0;
    }
    *(uint32_t *)data = *(uint32_t *)(e + 56);
    for (i = 0; i < 24u; i++) {
        data[4u + i] = e[8u + i];   /* creation, last access, last write */
    }
    *(uint32_t *)(data + 28) = *(uint32_t *)(e + 44);
    *(uint32_t *)(data + 32) = *(uint32_t *)(e + 40);
    n = *(uint32_t *)(e + 60) / 2u;
    n = n < NAME_CHARS - 1u ? n : NAME_CHARS - 1u;
    for (i = 0; i < 2u * n; i++) {
        data[44u + i] = e[64u + i];
    }
    return 0;
}

uint64_t FindFirstFileW(const uint16_t *pattern, void *data) {
    uint16_t dir[NAME_CHARS];
    vibeos_ustring_t mask;
    uint32_t i, cut = 0, len;
    uint64_t h = 0;
    NTSTATUS_T st;

    if (!pattern || (len = wlen(pattern)) >= NAME_CHARS) {
        set_error(ERROR_PATH_NOT_FOUND);
        return INVALID_HANDLE;
    }
    for (i = 0; i < len; i++) {
        dir[i] = pattern[i];
        if (pattern[i] == '\\' || pattern[i] == '/') {
            cut = i + 1u;
        }
    }
    if (cut == len) {
        set_error(ERROR_FILE_NOT_FOUND);   /* "dir\": nothing to look for */
        return INVALID_HANDLE;
    }
    dir[cut] = 0;
    if (cut == 0u) {
        dir[0] = '.';
        dir[1] = 0;
    }
    mask.Length = (uint16_t)(2u * (len - cut));
    mask.MaximumLength = mask.Length;
    mask.Buffer = (vibeos_wchar_t *)(uintptr_t)(pattern + cut);
    st = file_open(dir, 1u /* FILE_LIST_DIRECTORY */, 7u, 1u, 1u /* DIRECTORY_FILE */, &h);
    if (!failed(st)) {
        st = find_entry(h, &mask, 1, (uint8_t *)data);
        if (st != 0u) {
            (void)NtClose(h);
        }
    }
    if (st != 0u) {
        set_error(st == 0x80000006u || st == 0xC000000Fu ? ERROR_FILE_NOT_FOUND
                                                         : RtlNtStatusToDosError((vibeos_ntstatus_t)st));
        return INVALID_HANDLE;
    }
    return h;
}

BOOL32 FindNextFileW(uint64_t h, void *data) {
    uint32_t st = find_entry(h, 0, 0, (uint8_t *)data);

    if (st != 0u) {
        set_error(st == 0x80000006u ? ERROR_NO_MORE_FILES : RtlNtStatusToDosError((vibeos_ntstatus_t)st));
        return 0;
    }
    return 1;
}

BOOL32 FindClose(uint64_t h) {
    NTSTATUS_T st = NtClose(h);

    return failed(st) ? fail(st) : 1;
}

/* WIN32_FIND_DATAA from the wide one: the same fields, the name narrowed. */
static void find_data_a(const uint8_t *w, uint8_t *a) {
    uint32_t i;

    for (i = 0; i < 44u; i++) {
        a[i] = w[i];
    }
    for (i = 0; i < NAME_CHARS; i++) {
        a[44u + i] = (uint8_t)narrow(*(const uint16_t *)(w + 44u + 2u * i));
    }
    for (i = 0; i < 14u; i++) {
        a[304u + i] = 0;
    }
}

uint64_t FindFirstFileA(const char *pattern, void *data) {
    uint16_t w[NAME_CHARS];
    uint8_t wide[592];
    uint64_t h;

    if (!wide_name(pattern, w)) {
        return INVALID_HANDLE;
    }
    h = FindFirstFileW(w, wide);
    if (h != INVALID_HANDLE) {
        find_data_a(wide, (uint8_t *)data);
    }
    return h;
}

BOOL32 FindNextFileA(uint64_t h, void *data) {
    uint8_t wide[592];

    if (!FindNextFileW(h, wide)) {
        return 0;
    }
    find_data_a(wide, (uint8_t *)data);
    return 1;
}

/* ---- where things are --------------------------------------------------------------
 * C: is the boot volume, /tmp its temporary files and /win our DLLs; the
 * filesystem's hierarchy is the roadmap's next item and may move each. The
 * answer is copied when it fits, with its length; otherwise the size needed,
 * terminator included, as Windows answers. */

static uint32_t put_path_w(const char *s, uint16_t *buf, uint32_t size) {
    uint32_t n = 0, i;

    while (s[n]) {
        n++;
    }
    if (!buf || size <= n) {
        return n + 1u;
    }
    for (i = 0; i <= n; i++) {
        buf[i] = (uint8_t)s[i];
    }
    return n;
}

static uint32_t put_path_a(const char *s, char *buf, uint32_t size) {
    uint32_t n = 0, i;

    while (s[n]) {
        n++;
    }
    if (!buf || size <= n) {
        return n + 1u;
    }
    for (i = 0; i <= n; i++) {
        buf[i] = s[i];
    }
    return n;
}

uint32_t GetTempPathW(uint32_t size, uint16_t *buf) { return put_path_w("C:\\tmp\\", buf, size); }
uint32_t GetTempPathA(uint32_t size, char *buf) { return put_path_a("C:\\tmp\\", buf, size); }
/* Windows' own answers (E4-023): the kernel maps C:\Windows onto
 * /usr/lib/windows, and our DLLs are in its System32. */
uint32_t GetSystemDirectoryW(uint16_t *buf, uint32_t size) { return put_path_w("C:\\Windows\\System32", buf, size); }
uint32_t GetSystemDirectoryA(char *buf, uint32_t size) { return put_path_a("C:\\Windows\\System32", buf, size); }
uint32_t GetWindowsDirectoryW(uint16_t *buf, uint32_t size) { return put_path_w("C:\\Windows", buf, size); }
uint32_t GetWindowsDirectoryA(char *buf, uint32_t size) { return put_path_a("C:\\Windows", buf, size); }

/* ---- strings, times and the rest the file tests reach first (W4 step 9) ---------- */

int32_t lstrlenA(const char *s) {
    int32_t n = 0;

    while (s && s[n]) {
        n++;
    }
    return n;
}

int32_t lstrlenW(const uint16_t *s) {
    return s ? (int32_t)wlen(s) : 0;
}

uint16_t *lstrcpyW(uint16_t *dst, const uint16_t *src) {
    uint32_t i = 0;

    do {
        dst[i] = src[i];
    } while (src[i++]);
    return dst;
}

static uint16_t fold_w(uint16_t c) {
    return (c >= 'a' && c <= 'z') ? (uint16_t)(c - 32u) : c;
}

static int32_t cmp_w(const uint16_t *a, const uint16_t *b, int nocase) {
    for (;; a++, b++) {
        uint16_t x = nocase ? fold_w(*a) : *a, y = nocase ? fold_w(*b) : *b;

        if (x != y || x == 0u) {
            return x < y ? -1 : x > y ? 1 : 0;
        }
    }
}

int32_t lstrcmpW(const uint16_t *a, const uint16_t *b) { return cmp_w(a, b, 0); }
int32_t lstrcmpiW(const uint16_t *a, const uint16_t *b) { return cmp_w(a, b, 1); }

static int32_t cmp_a(const char *a, const char *b, int nocase) {
    for (;; a++, b++) {
        uint16_t x = (uint8_t)*a, y = (uint8_t)*b;

        if (nocase) {
            x = fold_w(x);
            y = fold_w(y);
        }
        if (x != y || x == 0u) {
            return x < y ? -1 : x > y ? 1 : 0;
        }
    }
}

int32_t lstrcmpA(const char *a, const char *b) { return cmp_a(a, b, 0); }
int32_t lstrcmpiA(const char *a, const char *b) { return cmp_a(a, b, 1); }

void GetSystemTimeAsFileTime(int64_t *ft) {
    (void)NtQuerySystemTime(ft);
}

int32_t CompareFileTime(const int64_t *a, const int64_t *b) {
    uint64_t x = (uint64_t)*a, y = (uint64_t)*b;

    return x < y ? -1 : x > y ? 1 : 0;
}

/* GetFileType: a disk file, or a character device - the console. */
uint32_t GetFileType(uint64_t h) {
    vibeos_nt_iosb_t iosb;
    uint32_t dev[2];
    NTSTATUS_T st = NtQueryVolumeInformationFile(h, &iosb, dev, sizeof(dev), 4u /* Device */);

    if (failed(st)) {
        return 2u;   /* FILE_TYPE_CHAR: what is not on a volume is the console here */
    }
    return dev[0] == 7u ? 1u /* FILE_TYPE_DISK */ : 2u;
}

/* A critical section, as Windows lays it out - LockCount at 8 (-1 free, else
 * the number of threads that want it, less one), RecursionCount at 12, the
 * owner's id at 16 - and as Windows waits on it: a thread that finds it held
 * sleeps on the process's keyed event under the section's address, and the
 * leaving owner releases one sleeper there (W5 step 8). Until then it only
 * counted, which was right while a process had one thread. */
void InitializeCriticalSection(void *cs) {
    uint8_t *p = (uint8_t *)cs;
    uint32_t i;

    for (i = 0; i < 40u; i++) {
        p[i] = 0;
    }
    *(volatile int32_t *)(p + 8) = -1;
}

BOOL32 InitializeCriticalSectionAndSpinCount(void *cs, uint32_t spin) {
    InitializeCriticalSection(cs);
    *(uint64_t *)((uint8_t *)cs + 32) = spin;
    return 1;
}

BOOL32 InitializeCriticalSectionEx(void *cs, uint32_t spin, uint32_t flags) {
    (void)flags;
    return InitializeCriticalSectionAndSpinCount(cs, spin);
}

void EnterCriticalSection(void *cs) {
    uint8_t *p = (uint8_t *)cs;
    volatile int32_t *lock = (volatile int32_t *)(p + 8);
    volatile uint64_t *owner = (volatile uint64_t *)(p + 16);
    uint64_t me = GetCurrentThreadId();

    if (*owner == me) {
        __atomic_add_fetch(lock, 1, __ATOMIC_ACQ_REL);
        (*(int32_t *)(p + 12))++;
        return;
    }
    if (__atomic_add_fetch(lock, 1, __ATOMIC_ACQ_REL) != 0) {
        /* Held: the owner's leave releases one of us under this address. */
        (void)NtWaitForKeyedEvent(0, cs, 0, 0);
    }
    *owner = me;
    *(int32_t *)(p + 12) = 1;
}

BOOL32 TryEnterCriticalSection(void *cs) {
    uint8_t *p = (uint8_t *)cs;
    volatile int32_t *lock = (volatile int32_t *)(p + 8);
    uint64_t me = GetCurrentThreadId();
    int32_t free_ = -1;

    if (*(volatile uint64_t *)(p + 16) == me) {
        __atomic_add_fetch(lock, 1, __ATOMIC_ACQ_REL);
        (*(int32_t *)(p + 12))++;
        return 1;
    }
    if (!__atomic_compare_exchange_n(lock, &free_, 0, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        return 0;
    }
    *(volatile uint64_t *)(p + 16) = me;
    *(int32_t *)(p + 12) = 1;
    return 1;
}

void LeaveCriticalSection(void *cs) {
    uint8_t *p = (uint8_t *)cs;
    volatile int32_t *lock = (volatile int32_t *)(p + 8);

    if (--(*(int32_t *)(p + 12)) != 0) {
        __atomic_sub_fetch(lock, 1, __ATOMIC_ACQ_REL);   /* still ours, one level less */
        return;
    }
    *(volatile uint64_t *)(p + 16) = 0u;
    if (__atomic_sub_fetch(lock, 1, __ATOMIC_ACQ_REL) >= 0) {
        (void)NtReleaseKeyedEvent(0, cs, 0, 0);   /* somebody wanted it: hand it on */
    }
}

void DeleteCriticalSection(void *cs) {
    (void)cs;
}

/* ---- threads (W5 step 8) ---------------------------------------------------------------- */

/* THREAD_BASIC_INFORMATION: ExitStatus at 0, TebBaseAddress at 8, ClientId
 * (process, thread) at 16, AffinityMask at 32, Priority at 40. */
static int thread_basic(uint64_t h, uint8_t *b) {
    NTSTATUS_T st = NtQueryInformationThread(h, 0u, b, 48u, 0);

    return failed(st) ? fail(st) : 1;
}

uint32_t GetThreadId(uint64_t h) {
    uint8_t b[48];

    return thread_basic(h, b) ? (uint32_t)*(uint64_t *)(b + 24) : 0u;
}

uint32_t GetProcessIdOfThread(uint64_t h) {
    uint8_t b[48];

    return thread_basic(h, b) ? (uint32_t)*(uint64_t *)(b + 16) : 0u;
}

BOOL32 GetExitCodeThread(uint64_t h, uint32_t *code) {
    uint8_t b[48];

    if (!thread_basic(h, b)) {
        return 0;
    }
    *code = *(uint32_t *)b;
    return 1;
}

__attribute__((noreturn)) void ExitThread(uint32_t code) {
    (void)NtTerminateThread(VIBEOS_NT_CURRENT_THREAD, code);
    for (;;) {
        /* NtTerminateThread of ourselves does not return. */
    }
}

BOOL32 TerminateThread(uint64_t h, uint32_t code) {
    NTSTATUS_T st = NtTerminateThread(h, code);

    return failed(st) ? fail(st) : 1;
}

uint32_t SuspendThread(uint64_t h) {
    uint32_t prev = 0;
    NTSTATUS_T st = NtSuspendThread(h, &prev);

    if (failed(st)) {
        fail(st);
        return 0xFFFFFFFFu;
    }
    return prev;
}

uint32_t ResumeThread(uint64_t h) {
    uint32_t prev = 0;
    NTSTATUS_T st = NtResumeThread(h, &prev);

    if (failed(st)) {
        fail(st);
        return 0xFFFFFFFFu;
    }
    return prev;
}

uint64_t OpenThread(uint32_t access, BOOL32 inherit, uint32_t tid) {
    named_t n;
    uint64_t h = 0, cid[2] = { 0, tid };
    NTSTATUS_T st;

    (void)attributes_w(&n, 0, inherit, 0);
    st = NtOpenThread(&h, access, n.oa, cid);
    return failed(st) ? (uint64_t)fail(st) : h;
}

/* QueueUserAPC(Routine, Thread, Data): the routine is called with Data in rcx,
 * which is the first of the three arguments NtQueueApcThread passes. Nonzero
 * on success, as Windows answers. */
uint32_t QueueUserAPC(void *routine, uint64_t thread, uint64_t data) {
    NTSTATUS_T st = NtQueueApcThread(thread, routine, data, 0, 0);

    return failed(st) ? (uint32_t)fail(st) : 1u;
}

BOOL32 SwitchToThread(void) {
    return NtYieldExecution() == VIBEOS_STATUS_SUCCESS;
}

/* Priorities are kept by the handle's thread as Windows reports them back; the
 * scheduler's classes are Linux's, and W5 does not map one onto the other. */
BOOL32 SetThreadPriority(uint64_t h, int32_t prio) {
    NTSTATUS_T st = NtSetInformationThread(h, 3u /* ThreadBasePriority */, &prio, 4u);

    return failed(st) ? fail(st) : 1;
}

int32_t GetThreadPriority(uint64_t h) {
    uint8_t b[48];

    return thread_basic(h, b) ? 0 : 0x7FFFFFFF;   /* THREAD_PRIORITY_NORMAL, or the error value */
}

uint64_t SetThreadAffinityMask(uint64_t h, uint64_t mask) {
    uint8_t b[48];
    NTSTATUS_T st;

    if (!thread_basic(h, b)) {
        return 0;
    }
    st = NtSetInformationThread(h, 4u /* ThreadAffinityMask */, &mask, 8u);
    return failed(st) ? (uint64_t)fail(st) : *(uint64_t *)(b + 32);
}

BOOL32 GetThreadTimes(uint64_t h, int64_t *created, int64_t *exited, int64_t *kernel, int64_t *user) {
    int64_t t[4];
    NTSTATUS_T st = NtQueryInformationThread(h, 1u, t, sizeof(t), 0);

    if (failed(st)) {
        return fail(st);
    }
    *created = t[0];
    *exited = t[1];
    *kernel = t[2];
    *user = t[3];
    return 1;
}

void GetCurrentThreadStackLimits(uint64_t *low, uint64_t *high) {
    *low = vibeos_nt_rd64(vibeos_nt_teb() + VIBEOS_TEB_DEALLOCATION_STACK);
    *high = vibeos_nt_rd64(vibeos_nt_teb() + VIBEOS_TEB_STACK_BASE);
}

BOOL32 GetThreadContext(uint64_t h, void *ctx) {
    NTSTATUS_T st = NtGetContextThread(h, ctx);

    return failed(st) ? fail(st) : 1;
}

BOOL32 SetThreadContext(uint64_t h, const void *ctx) {
    NTSTATUS_T st = NtSetContextThread(h, ctx);

    return failed(st) ? fail(st) : 1;
}

/* ---- waitable timers (W5 step 7) -------------------------------------------------------- */

uint64_t CreateWaitableTimerW(const void *sa, BOOL32 manual, const uint16_t *name) {
    named_t n;
    uint64_t h = 0;
    NTSTATUS_T st;
    void *oa = attributes_w(&n, name, inherits(sa), 1);

    if (!oa) {
        return 0;
    }
    st = NtCreateTimer(&h, 0x001F0003u, oa, manual ? 0u : 1u);
    return created(st, &h);
}

uint64_t CreateWaitableTimerA(const void *sa, BOOL32 manual, const char *name) {
    named_t n;
    uint16_t w[NAME_CHARS];

    if (name && !attributes_a(&n, w, name, 0, 0)) {
        return 0;
    }
    return CreateWaitableTimerW(sa, manual, name ? w : 0);
}

uint64_t OpenWaitableTimerW(uint32_t access, BOOL32 inherit, const uint16_t *name) {
    named_t n;
    uint64_t h = 0;
    void *oa = attributes_w(&n, name, inherit, 0);

    if (!oa) {
        return 0;
    }
    return opened(NtOpenTimer(&h, access, oa), &h);
}

uint64_t OpenWaitableTimerA(uint32_t access, BOOL32 inherit, const char *name) {
    uint16_t w[NAME_CHARS];

    if (!name || widen(name, w, NAME_CHARS) < 0) {
        set_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    return OpenWaitableTimerW(access, inherit, w);
}

BOOL32 SetWaitableTimer(uint64_t h, const int64_t *due, int32_t period, void *apc, void *arg,
                        BOOL32 resume) {
    NTSTATUS_T st;

    (void)arg;
    st = NtSetTimer(h, due, apc, arg, (uint8_t)(resume != 0), period, 0);
    return failed(st) ? fail(st) : 1;
}

BOOL32 CancelWaitableTimer(uint64_t h) {
    NTSTATUS_T st = NtCancelTimer(h, 0);

    return failed(st) ? fail(st) : 1;
}

/* GetTempFileName: <path>\<prefix, three characters><hex>.tmp. With `unique`
 * zero, a number is tried until a file of that name can be made, and the file
 * is left behind - which is the point of asking. */
uint32_t GetTempFileNameW(const uint16_t *path, const uint16_t *prefix, uint32_t unique, uint16_t *out) {
    static const char hex[] = "0123456789ABCDEF";
    uint32_t n, i, k, tries, num;
    int64_t now = 0;

    if (!path || !out) {
        set_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    n = wlen(path);
    if (n + 3u + 4u + 6u >= NAME_CHARS) {
        set_error(ERROR_FILENAME_EXCED_RANGE);
        return 0;
    }
    (void)NtQuerySystemTime(&now);
    num = unique ? (unique & 0xFFFFu) : (uint32_t)(now / 10000) & 0xFFFFu;
    for (tries = 0; tries < 65536u; tries++, num = (num + 1u) & 0xFFFFu) {
        uint64_t h = 0;
        NTSTATUS_T st;

        if (num == 0u) {
            continue;
        }
        k = 0;
        for (i = 0; i < n; i++) {
            out[k++] = path[i];
        }
        if (k && out[k - 1u] != '\\') {
            out[k++] = '\\';
        }
        for (i = 0; prefix && i < 3u && prefix[i]; i++) {
            out[k++] = prefix[i];
        }
        for (i = 0; i < 4u; i++) {
            out[k++] = (uint16_t)hex[(num >> (12u - 4u * i)) & 0xFu];
        }
        out[k++] = '.';
        out[k++] = 't';
        out[k++] = 'm';
        out[k++] = 'p';
        out[k] = 0;
        if (unique) {
            return unique;
        }
        st = file_open(out, 0x80000000u /* GENERIC_READ */ | 0x40000000u, 0u, 2u /* FILE_CREATE */,
                       0x40u, &h);
        if (!failed(st)) {
            (void)NtClose(h);
            return num;
        }
        if (st != 0xC0000035u /* OBJECT_NAME_COLLISION */) {
            fail(st);
            return 0;
        }
    }
    set_error(80u);   /* ERROR_FILE_EXISTS */
    return 0;
}

uint32_t GetTempFileNameA(const char *path, const char *prefix, uint32_t unique, char *out) {
    uint16_t wp[NAME_CHARS], wx[4] = { 0, 0, 0, 0 }, wo[NAME_CHARS];
    uint32_t r, i;

    if (!wide_name(path, wp)) {
        return 0;
    }
    for (i = 0; prefix && i < 3u && prefix[i]; i++) {
        wx[i] = (uint8_t)prefix[i];
    }
    r = GetTempFileNameW(wp, wx, unique, wo);
    if (r != 0u) {
        for (i = 0; wo[i]; i++) {
            out[i] = narrow(wo[i]);
        }
        out[i] = 0;
    }
    return r;
}

/* ---- a file's attributes and times, the module's name, the directory -------------- */

/* FILE_BASIC_INFORMATION through a handle opened for attributes alone: zero
 * times are "leave as they are", as Windows reads them. */
static BOOL32 set_basic(uint64_t h, const int64_t *created, const int64_t *accessed, const int64_t *written,
                        uint32_t attributes) {
    uint8_t basic[40];
    vibeos_nt_iosb_t iosb;
    uint32_t i;
    NTSTATUS_T st;

    for (i = 0; i < sizeof(basic); i++) {
        basic[i] = 0;
    }
    if (created) {
        *(int64_t *)basic = *created;
    }
    if (accessed) {
        *(int64_t *)(basic + 8) = *accessed;
    }
    if (written) {
        *(int64_t *)(basic + 16) = *written;
    }
    *(uint32_t *)(basic + 32) = attributes;
    st = NtSetInformationFile(h, &iosb, basic, sizeof(basic), 4u /* Basic */);
    return failed(st) ? fail(st) : 1;
}

BOOL32 SetFileAttributesW(const uint16_t *name, uint32_t attributes) {
    uint64_t h = 0;
    BOOL32 r;
    NTSTATUS_T st = file_open(name, 0x100u /* FILE_WRITE_ATTRIBUTES */, 7u, 1u, 0u, &h);

    if (failed(st)) {
        return fail(st);
    }
    /* FILE_ATTRIBUTE_NORMAL alone is "none", which Basic spells as itself. */
    r = set_basic(h, 0, 0, 0, attributes ? attributes : 0x80u);
    (void)NtClose(h);
    return r;
}

BOOL32 SetFileAttributesA(const char *name, uint32_t attributes) {
    uint16_t w[NAME_CHARS];

    return wide_name(name, w) ? SetFileAttributesW(w, attributes) : 0;
}

BOOL32 SetFileTime(uint64_t h, const int64_t *created, const int64_t *accessed, const int64_t *written) {
    return set_basic(h, created, accessed, written, 0u);
}

BOOL32 GetFileTime(uint64_t h, int64_t *created, int64_t *accessed, int64_t *written) {
    uint8_t basic[40];
    vibeos_nt_iosb_t iosb;
    NTSTATUS_T st = NtQueryInformationFile(h, &iosb, basic, sizeof(basic), 4u);

    if (failed(st)) {
        return fail(st);
    }
    if (created) {
        *created = *(int64_t *)basic;
    }
    if (accessed) {
        *accessed = *(int64_t *)(basic + 8);
    }
    if (written) {
        *written = *(int64_t *)(basic + 16);
    }
    return 1;
}

/* BY_HANDLE_FILE_INFORMATION: attributes, three times, the volume's serial,
 * the size high and low, the links, the index high and low. */
BOOL32 GetFileInformationByHandle(uint64_t h, void *out) {
    uint8_t all[100], *o = (uint8_t *)out;
    vibeos_nt_iosb_t iosb;
    uint32_t i;
    NTSTATUS_T st = NtQueryInformationFile(h, &iosb, all, sizeof(all), 18u /* All */);

    if (failed(st) && st != 0x80000005u /* the name did not fit, which is not asked for */) {
        return fail(st);
    }
    *(uint32_t *)o = *(uint32_t *)(all + 32);
    for (i = 0; i < 24u; i++) {
        o[4u + i] = all[i];
    }
    *(uint32_t *)(o + 28) = 0x56494245u;   /* the volume's serial: one volume */
    *(uint32_t *)(o + 32) = *(uint32_t *)(all + 52);
    *(uint32_t *)(o + 36) = *(uint32_t *)(all + 48);
    *(uint32_t *)(o + 40) = *(uint32_t *)(all + 56);
    *(uint32_t *)(o + 44) = *(uint32_t *)(all + 64 + 4);
    *(uint32_t *)(o + 48) = *(uint32_t *)(all + 64);
    return 1;
}

/* A UNICODE_STRING of the process parameters, into a wide buffer. */
static uint32_t params_string_w(uint32_t field, uint16_t *buf, uint32_t size) {
    uint64_t params = vibeos_nt_rd64(peb() + VIBEOS_PEB_PROCESS_PARAMS);
    const vibeos_ustring_t *u = (const vibeos_ustring_t *)(uintptr_t)(params + field);
    uint32_t n = u->Length / 2u, i;

    if (!buf || size == 0u) {
        return 0;
    }
    for (i = 0; i < n && i + 1u < size; i++) {
        buf[i] = u->Buffer[i];
    }
    buf[i] = 0;
    if (i < n) {
        set_error(122u);   /* ERROR_INSUFFICIENT_BUFFER: truncated, as Windows does */
    }
    return i;
}

uint32_t GetModuleFileNameW(uint64_t module, uint16_t *buf, uint32_t size) {
    if (module != 0u && module != vibeos_nt_rd64(peb() + VIBEOS_PEB_IMAGE_BASE)) {
        set_error(ERROR_MOD_NOT_FOUND);   /* a DLL's path: the loader's list is not asked yet */
        return 0;
    }
    return params_string_w(VIBEOS_RUPP_IMAGE_PATH, buf, size);
}

uint32_t GetModuleFileNameA(uint64_t module, char *buf, uint32_t size) {
    uint16_t w[NAME_CHARS];
    uint32_t n, i;

    n = GetModuleFileNameW(module, w, size < NAME_CHARS ? size : NAME_CHARS);
    for (i = 0; i < n; i++) {
        buf[i] = narrow(w[i]);
    }
    if (size) {
        buf[n < size ? n : size - 1u] = 0;
    }
    return n;
}

/* GetFullPathName: what RtlDosPathNameToNtPathName_U makes of it, without
 * the "\??\" in front. The part after the last separator is the file part. */
uint32_t GetFullPathNameW(const uint16_t *name, uint32_t size, uint16_t *buf, uint16_t **file_part) {
    vibeos_ustring_t nt;
    uint32_t n, i, skip = 0, last = 0;

    if (!name || !RtlDosPathNameToNtPathName_U(name, &nt, 0, 0)) {
        set_error(ERROR_PATH_NOT_FOUND);
        return 0;
    }
    n = nt.Length / 2u;
    if (n >= 4u && nt.Buffer[0] == '\\' && nt.Buffer[1] == '?' && nt.Buffer[2] == '?' && nt.Buffer[3] == '\\') {
        skip = 4u;
    }
    n -= skip;
    if (!buf || size <= n) {
        RtlFreeUnicodeString(&nt);
        return n + 1u;
    }
    for (i = 0; i < n; i++) {
        buf[i] = nt.Buffer[skip + i];
        if (buf[i] == '\\') {
            last = i + 1u;
        }
    }
    buf[n] = 0;
    RtlFreeUnicodeString(&nt);
    if (file_part) {
        *file_part = last < n ? buf + last : 0;
    }
    return n;
}

uint32_t GetFullPathNameA(const char *name, uint32_t size, char *buf, char **file_part) {
    uint16_t w[NAME_CHARS], out[NAME_CHARS], *fp = 0;
    uint32_t n, i;

    if (!wide_name(name, w)) {
        return 0;
    }
    n = GetFullPathNameW(w, NAME_CHARS, out, &fp);
    if (n == 0u || n >= NAME_CHARS) {
        return n;
    }
    if (!buf || size <= n) {
        return n + 1u;
    }
    for (i = 0; i <= n; i++) {
        buf[i] = narrow(out[i]);
    }
    if (file_part) {
        *file_part = fp ? buf + (fp - out) : 0;
    }
    return n;
}

uint32_t GetCurrentDirectoryW(uint32_t size, uint16_t *buf) {
    uint64_t params = vibeos_nt_rd64(peb() + VIBEOS_PEB_PROCESS_PARAMS);
    const vibeos_ustring_t *u = (const vibeos_ustring_t *)(uintptr_t)(params + VIBEOS_RUPP_CURDIR_PATH);
    uint32_t n = u->Length / 2u, i;

    if (n > 3u && u->Buffer[n - 1u] == '\\') {
        n--;   /* "C:\dir\" is kept with its separator and answered without */
    }
    if (n == 0u) {
        return put_path_w("C:\\", buf, size);
    }
    if (!buf || size <= n) {
        return n + 1u;
    }
    for (i = 0; i < n; i++) {
        buf[i] = u->Buffer[i];
    }
    buf[n] = 0;
    return n;
}

uint32_t GetCurrentDirectoryA(uint32_t size, char *buf) {
    uint16_t w[NAME_CHARS];
    uint32_t n = GetCurrentDirectoryW(NAME_CHARS, w), i;

    if (n == 0u || n >= NAME_CHARS) {
        return n;
    }
    if (!buf || size <= n) {
        return n + 1u;
    }
    for (i = 0; i <= n; i++) {
        buf[i] = narrow(w[i]);
    }
    return n;
}

/* ---- the 16-bit file calls, as Windows still exports them: CreateFile and
 * friends with a handle as an int, and HFILE_ERROR (-1) for failure. */

int32_t _lcreat(const char *name, int32_t attributes) {
    uint64_t h = CreateFileA(name, 0xC0000000u, 3u, 0, 2u /* CREATE_ALWAYS */,
                             (uint32_t)attributes & 7u, 0);

    return h == INVALID_HANDLE ? -1 : (int32_t)h;
}

int32_t _lopen(const char *name, int32_t mode) {
    static const uint32_t access[3] = { 0x80000000u, 0x40000000u, 0xC0000000u };
    static const uint32_t share[8] = { 3u, 0u, 1u, 2u, 3u, 3u, 3u, 3u };
    uint64_t h;

    if ((mode & 3) == 3) {
        set_error(ERROR_INVALID_PARAMETER);
        return -1;
    }
    h = CreateFileA(name, access[mode & 3], share[(mode >> 4) & 7], 0, 3u /* OPEN_EXISTING */, 0, 0);
    return h == INVALID_HANDLE ? -1 : (int32_t)h;
}

int32_t _lclose(int32_t h) {
    return CloseHandle((uint64_t)(uint32_t)h) ? 0 : -1;
}

uint32_t _lread(int32_t h, void *buf, uint32_t len) {
    uint32_t got = 0;

    return ReadFile((uint64_t)(uint32_t)h, buf, len, &got, 0) ? got : 0xFFFFFFFFu;
}

int32_t _hread(int32_t h, void *buf, int32_t len) {
    return (int32_t)_lread(h, buf, (uint32_t)len);
}

/* A write of nothing is truncation to the file's position, as it was. */
uint32_t _lwrite(int32_t h, const char *buf, uint32_t len) {
    uint32_t put = 0;

    if (len == 0u) {
        return SetEndOfFile((uint64_t)(uint32_t)h) ? 0u : 0xFFFFFFFFu;
    }
    return WriteFile((uint64_t)(uint32_t)h, buf, len, &put, 0) ? put : 0xFFFFFFFFu;
}

int32_t _hwrite(int32_t h, const char *buf, int32_t len) {
    return (int32_t)_lwrite(h, buf, (uint32_t)len);
}

int32_t _llseek(int32_t h, int32_t offset, int32_t origin) {
    return (int32_t)SetFilePointer((uint64_t)(uint32_t)h, offset, 0, (uint32_t)origin);
}

/* ---- exceptions (W5 step 5): kernel32's names for ntdll's runtime ------------------ */

void *AddVectoredExceptionHandler(uint32_t first, void *handler) {
    return RtlAddVectoredExceptionHandler(first, handler);
}

uint32_t RemoveVectoredExceptionHandler(void *handle) {
    return RtlRemoveVectoredExceptionHandler(handle);
}

/* RaiseException(Code, Flags, NumberOfArguments, Arguments): a record built
 * here, at most fifteen arguments, as Windows truncates. */
void RaiseException(uint32_t code, uint32_t flags, uint32_t nargs, const uint64_t *args) {
    uint64_t rec[19];
    uint32_t i;

    for (i = 0; i < 19u; i++) {
        rec[i] = 0;
    }
    rec[0] = (uint64_t)code | (uint64_t)(flags & 1u) << 32;   /* ExceptionCode, ExceptionFlags */
    if (nargs > 15u) {
        nargs = 15u;
    }
    rec[3] = nargs;                                          /* NumberParameters */
    for (i = 0; args && i < nargs; i++) {
        rec[4u + i] = args[i];
    }
    RtlRaiseException(rec);
}

/* ---- processes (W6) ---------------------------------------------------------------------
 * CreateProcess over NtCreateUserProcess. The image is the application name or
 * the command line's first token, ".exe" added when it has no extension;
 * bInheritHandles, STARTF_USESTDHANDLES's three handles and CREATE_SUSPENDED
 * go to the kernel as Windows' own kernel32 passes them. */

typedef struct {
    uint64_t process, thread;
    uint32_t pid, tid;
} process_information_t;

static uint32_t process_basic(uint64_t h, uint8_t *b) {
    return NtQueryInformationProcess(h, 0, b, 48, 0);
}

BOOL32 CreateProcessW(const uint16_t *app, uint16_t *cmd, void *pattr, void *tattr, BOOL32 inherit,
                      uint32_t flags, void *env, const uint16_t *curdir, void *startup,
                      process_information_t *pi) {
    uint16_t image[260];
    vibeos_ustring_t img, line, nt, *dir = 0, cd;
    void *params = 0;
    uint64_t ph = 0, th = 0;
    uint8_t b[48];
    uint32_t n = 0, i = 0, dot = 0;
    NTSTATUS_T st;

    (void)pattr;
    (void)tattr;
    if (app) {
        while (app[n] && n + 5u < 260u) {
            image[n] = app[n];
            n++;
        }
    } else if (cmd) {
        int quoted = cmd[0] == '"';

        for (i = quoted ? 1u : 0u; cmd[i] && n + 5u < 260u; i++) {
            if (quoted ? cmd[i] == '"' : (cmd[i] == ' ' || cmd[i] == '\t')) {
                break;
            }
            image[n++] = cmd[i];
        }
    }
    if (n == 0u) {
        set_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    for (i = 0; i < n; i++) {
        if (image[i] == '.') {
            dot = 1;
        } else if (image[i] == '\\' || image[i] == '/') {
            dot = 0;
        }
    }
    if (!dot) {
        image[n++] = '.';
        image[n++] = 'e';
        image[n++] = 'x';
        image[n++] = 'e';
    }
    image[n] = 0;
    if (!RtlDosPathNameToNtPathName_U(image, &nt, 0, 0)) {
        set_error(ERROR_PATH_NOT_FOUND);
        return 0;
    }
    RtlInitUnicodeString(&img, nt.Buffer);
    RtlInitUnicodeString(&line, cmd ? cmd : image);
    if (curdir) {
        RtlInitUnicodeString(&cd, curdir);
        dir = &cd;
    }
    st = RtlCreateProcessParametersEx(&params, &img, 0, dir, &line, env, 0, 0, 0, 0, 1);
    if (!failed(st) && startup && (*(uint32_t *)((uint8_t *)startup + 0x3C) & 0x100u)) {
        /* STARTF_USESTDHANDLES: hStdInput, hStdOutput, hStdError at 0x50. */
        uint32_t k;

        for (k = 0; k < 3u; k++) {
            *(uint64_t *)((uint8_t *)params + VIBEOS_RUPP_STD_INPUT + 8u * k) =
                *(uint64_t *)((uint8_t *)startup + 0x50 + 8u * k);
        }
    }
    if (!failed(st)) {
        st = NtCreateUserProcess(&ph, &th, 0x001FFFFFu, 0x001FFFFFu, 0, 0,
                                 inherit ? 0x4u : 0u /* PROCESS_CREATE_FLAGS_INHERIT_HANDLES */,
                                 (flags & 4u) ? 1u : 0u /* CREATE_SUSPENDED */, params, 0, 0);
        RtlDestroyProcessParameters(params);
    }
    RtlFreeUnicodeString(&nt);
    if (failed(st)) {
        return fail(st);
    }
    pi->process = ph;
    pi->thread = th;
    pi->pid = !failed(process_basic(ph, b)) ? (uint32_t)*(uint64_t *)(b + 32) : 0u;
    pi->tid = GetThreadId(th);
    return 1;
}

BOOL32 CreateProcessA(const char *app, char *cmd, void *pattr, void *tattr, BOOL32 inherit,
                      uint32_t flags, void *env, const char *curdir, void *startup,
                      process_information_t *pi) {
    static uint16_t wapp[260], wcmd[2048], wdir[260];

    if ((app && widen(app, wapp, 260) < 0) || (cmd && widen(cmd, wcmd, 2048) < 0) ||
        (curdir && widen(curdir, wdir, 260) < 0)) {
        set_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    return CreateProcessW(app ? wapp : 0, cmd ? wcmd : 0, pattr, tattr, inherit, flags, env,
                          curdir ? wdir : 0, startup, pi);
}

BOOL32 GetExitCodeProcess(uint64_t h, uint32_t *code) {
    uint8_t b[48];
    NTSTATUS_T st = process_basic(h, b);

    if (failed(st)) {
        return fail(st);
    }
    *code = *(uint32_t *)b;
    return 1;
}

uint32_t GetProcessId(uint64_t h) {
    uint8_t b[48];

    return failed(process_basic(h, b)) ? 0u : (uint32_t)*(uint64_t *)(b + 32);
}

uint64_t OpenProcess(uint32_t access, BOOL32 inherit, uint32_t pid) {
    uint64_t h = 0, cid[2] = { pid, 0 };
    NTSTATUS_T st = NtOpenProcess(&h, access, 0, cid);

    (void)inherit;
    if (failed(st)) {
        (void)fail(st);
        return 0;
    }
    return h;
}

BOOL32 TerminateProcess(uint64_t h, uint32_t code) {
    NTSTATUS_T st = NtTerminateProcess(h, code);

    return failed(st) ? fail(st) : 1;
}

/* ---- W7 step 4: the functions kernel32's tests stop at ------------------------------------
 * Each is the first missing call of one of Wine's tests (the table in
 * docs/abi/windows.md, W7 step 3; the rows WO-030 onward of
 * docs/implementation_progress/windows_open_items.md). Written to Windows'
 * behaviour as those tests check it, and no further. */

#define ERROR_INSUFFICIENT_BUFFER 122u
#define ERROR_DIRECTORY_INVALID   267u   /* ERROR_DIRECTORY */
#define ERROR_INVALID_FLAGS       1004u
#define ERROR_MR_MID_NOT_FOUND    317u

/* -- SYSTEMTIME and FILETIME (WO-030): 100 ns ticks since 1601-01-01, a Monday. */

typedef struct {
    uint16_t year, month, day_of_week, day, hour, minute, second, ms;
} k32_systime_t;

static int k32_leap(uint32_t y) {
    return (y % 4u == 0u && y % 100u != 0u) || y % 400u == 0u;
}

static uint32_t k32_month_days(uint32_t y, uint32_t m) {
    static const uint8_t d[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

    return d[m - 1u] + (m == 2u && k32_leap(y) ? 1u : 0u);
}

#define K32_TICKS_PER_DAY 864000000000ull

BOOL32 SystemTimeToFileTime(const k32_systime_t *st, int64_t *ft) {
    uint64_t days, years;
    uint32_t m;

    if (st->year < 1601u || st->year > 30827u || st->month < 1u || st->month > 12u || st->day < 1u ||
        st->day > k32_month_days(st->year, st->month) || st->hour > 23u || st->minute > 59u ||
        st->second > 59u || st->ms > 999u) {
        set_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    years = st->year - 1601u;
    days = years * 365u + years / 4u - years / 100u + years / 400u;
    for (m = 1; m < st->month; m++) {
        days += k32_month_days(st->year, m);
    }
    days += st->day - 1u;
    *ft = (int64_t)(days * K32_TICKS_PER_DAY +
                    ((st->hour * 3600ull + st->minute * 60ull + st->second) * 1000ull + st->ms) * 10000ull);
    return 1;
}

BOOL32 FileTimeToSystemTime(const int64_t *ft, k32_systime_t *st) {
    uint64_t t = (uint64_t)*ft, days, rem;
    uint32_t y = 1601u, m = 1u;

    if (*ft < 0) {
        set_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    days = t / K32_TICKS_PER_DAY;
    rem = t % K32_TICKS_PER_DAY;
    st->day_of_week = (uint16_t)((days + 1u) % 7u);
    y += (uint32_t)(days / 146097u) * 400u;   /* whole four-hundred-year cycles */
    days %= 146097u;
    while (days >= (k32_leap(y) ? 366u : 365u)) {
        days -= k32_leap(y) ? 366u : 365u;
        y++;
    }
    while (days >= k32_month_days(y, m)) {
        days -= k32_month_days(y, m);
        m++;
    }
    st->year = (uint16_t)y;
    st->month = (uint16_t)m;
    st->day = (uint16_t)(days + 1u);
    rem /= 10000u;   /* milliseconds */
    st->ms = (uint16_t)(rem % 1000u);
    rem /= 1000u;
    st->second = (uint16_t)(rem % 60u);
    st->minute = (uint16_t)((rem / 60u) % 60u);
    st->hour = (uint16_t)(rem / 3600u);
    return 1;
}

/* -- the version (WO-031): Windows 10 22H2, as Wine 9 reports by default. */

#define K32_VER_MAJOR 10u
#define K32_VER_MINOR 0u
#define K32_VER_BUILD 19045u

static BOOL32 k32_version(void *info, uint32_t base, uint32_t ex, uint32_t csd_bytes) {
    uint8_t *b = (uint8_t *)info;
    uint32_t size = *(const uint32_t *)info, i;

    if (size != base && size != ex) {
        set_error(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    for (i = 4; i < size; i++) {
        b[i] = 0;
    }
    ((uint32_t *)info)[1] = K32_VER_MAJOR;
    ((uint32_t *)info)[2] = K32_VER_MINOR;
    ((uint32_t *)info)[3] = K32_VER_BUILD;
    ((uint32_t *)info)[4] = 2u;   /* VER_PLATFORM_WIN32_NT */
    if (size == ex) {
        b[20u + csd_bytes + 8u] = 1u;   /* wProductType: VER_NT_WORKSTATION */
    }
    return 1;
}

BOOL32 GetVersionExA(void *info) { return k32_version(info, 148u, 156u, 128u); }
BOOL32 GetVersionExW(void *info) { return k32_version(info, 276u, 284u, 256u); }

uint32_t GetVersion(void) {
    return K32_VER_MAJOR | (K32_VER_MINOR << 8) | (K32_VER_BUILD << 16);
}

/* -- power (WO-032): on the mains, no battery. */

BOOL32 GetSystemPowerStatus(void *status) {
    uint8_t *s = (uint8_t *)status;

    s[0] = 1u;     /* ACLineStatus: online */
    s[1] = 128u;   /* BatteryFlag: no system battery */
    s[2] = 255u;   /* BatteryLifePercent: unknown */
    s[3] = 0u;
    ((uint32_t *)status)[1] = 0xFFFFFFFFu;   /* BatteryLifeTime: unknown */
    ((uint32_t *)status)[2] = 0xFFFFFFFFu;   /* BatteryFullLifeTime */
    return 1;
}

/* -- Beep (WO-033): there is no speaker; Windows succeeds without one too. */

BOOL32 Beep(uint32_t freq, uint32_t duration) {
    (void)freq;
    (void)duration;
    return 1;
}

/* -- the current directory (WO-034): the process parameters' CurrentDirectory,
 * which GetCurrentDirectory and every relative path read, kept with its
 * trailing separator as Windows keeps it. */

BOOL32 SetCurrentDirectoryW(const uint16_t *dir) {
    uint64_t params = vibeos_nt_rd64(peb() + VIBEOS_PEB_PROCESS_PARAMS);
    vibeos_ustring_t *u = (vibeos_ustring_t *)(uintptr_t)(params + VIBEOS_RUPP_CURDIR_PATH);
    uint16_t full[NAME_CHARS + 2u];
    uint32_t n, a, i;

    if (!dir) {
        set_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    n = GetFullPathNameW(dir, NAME_CHARS, full, 0);
    if (n == 0u || n >= NAME_CHARS) {
        set_error(n == 0u ? ERROR_PATH_NOT_FOUND : ERROR_FILENAME_EXCED_RANGE);
        return 0;
    }
    a = GetFileAttributesW(full);
    if (a == INVALID_FILE_ATTRIBUTES) {
        return 0;   /* GetFileAttributesW said why */
    }
    if (!(a & 0x10u)) {
        set_error(ERROR_DIRECTORY_INVALID);
        return 0;
    }
    if (full[n - 1u] != '\\') {
        full[n++] = '\\';
    }
    full[n] = 0;
    if ((n + 1u) * 2u > u->MaximumLength) {
        uint16_t *nb = (uint16_t *)HeapAlloc(GetProcessHeap(), 0, (NAME_CHARS + 2u) * 2u);

        if (!nb) {
            set_error(8u);   /* ERROR_NOT_ENOUGH_MEMORY */
            return 0;
        }
        u->Buffer = nb;
        u->MaximumLength = (uint16_t)((NAME_CHARS + 2u) * 2u);
    }
    for (i = 0; i <= n; i++) {
        u->Buffer[i] = full[i];
    }
    u->Length = (uint16_t)(n * 2u);
    return 1;
}

BOOL32 SetCurrentDirectoryA(const char *dir) {
    uint16_t w[NAME_CHARS];

    return wide_name(dir, w) ? SetCurrentDirectoryW(w) : 0;
}

/* -- drives (WO-035, WO-036): one, C:, the boot volume. */

uint32_t GetLogicalDrives(void) {
    return 1u << 2;
}

static const char g_k32_volume[] = "\\Device\\HarddiskVolume1";

/* The device a DOS name names, or with a null name every DOS name; each
 * string terminated, the list by one more. The count of characters written. */
uint32_t QueryDosDeviceW(const uint16_t *name, uint16_t *buf, uint32_t max) {
    const char *s;
    uint32_t n = 0, i;

    if (!name) {
        s = "C:";
    } else if ((name[0] == 'C' || name[0] == 'c') && name[1] == ':' && name[2] == 0) {
        s = g_k32_volume;
    } else {
        set_error(ERROR_FILE_NOT_FOUND);
        return 0;
    }
    while (s[n]) {
        n++;
    }
    if (!buf || max < n + 2u) {
        set_error(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    for (i = 0; i < n; i++) {
        buf[i] = (uint16_t)(unsigned char)s[i];
    }
    buf[n] = 0;
    buf[n + 1u] = 0;
    return n + 2u;
}

uint32_t QueryDosDeviceA(const char *name, char *buf, uint32_t max) {
    uint16_t wn[NAME_CHARS], wb[NAME_CHARS];
    uint32_t n, i;

    if (name && !wide_name(name, wn)) {
        return 0;
    }
    n = QueryDosDeviceW(name ? wn : 0, wb, NAME_CHARS);
    if (n == 0u) {
        return 0;
    }
    if (!buf || max < n) {
        set_error(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    for (i = 0; i < n; i++) {
        buf[i] = narrow(wb[i]);
    }
    return n;
}

/* -- Local and Global memory (WO-037): blocks of the process heap. A moveable
 * block is a fixed one here - its handle is its address, which LocalLock
 * hands back as it is - so nothing ever moves and a lock count is not kept. */

#define K32_LMEM_ZEROINIT 0x40u

void *LocalAlloc(uint32_t flags, size_t size) {
    void *p = HeapAlloc(GetProcessHeap(), (flags & K32_LMEM_ZEROINIT) ? 8u /* HEAP_ZERO_MEMORY */ : 0u,
                        size);

    if (!p) {
        set_error(8u);   /* ERROR_NOT_ENOUGH_MEMORY */
    }
    return p;
}

void *LocalFree(void *h) {
    if (h && !HeapFree(GetProcessHeap(), 0, h)) {
        set_error(6u);   /* ERROR_INVALID_HANDLE */
        return h;
    }
    return 0;
}

void *LocalReAlloc(void *h, size_t size, uint32_t flags) {
    void *p = HeapReAlloc(GetProcessHeap(), (flags & K32_LMEM_ZEROINIT) ? 8u : 0u, h, size);

    if (!p) {
        set_error(8u);
    }
    return p;
}

size_t LocalSize(void *h) {
    return h ? HeapSize(GetProcessHeap(), 0, h) : 0u;
}

void *LocalLock(void *h) { return h; }
BOOL32 LocalUnlock(void *h) {
    (void)h;
    set_error(158u);   /* ERROR_NOT_LOCKED: Windows' answer for a fixed block */
    return 0;
}

void *GlobalAlloc(uint32_t flags, size_t size) { return LocalAlloc(flags, size); }
void *GlobalFree(void *h) { return LocalFree(h); }
void *GlobalReAlloc(void *h, size_t size, uint32_t flags) { return LocalReAlloc(h, size, flags); }
size_t GlobalSize(void *h) { return LocalSize(h); }
void *GlobalLock(void *h) { return h; }
BOOL32 GlobalUnlock(void *h) { return LocalUnlock(h); }

/* -- heaps (WO-038): the process heap is the only one. */

uint32_t GetProcessHeaps(uint32_t count, uint64_t *heaps) {
    if (count >= 1u && heaps) {
        heaps[0] = GetProcessHeap();
    }
    return 1u;
}

/* -- short names (WO-039): a volume here has none, so the long name is the
 * short one, as Windows answers on a volume with 8.3 names turned off. */

uint32_t GetShortPathNameW(const uint16_t *path, uint16_t *buf, uint32_t size) {
    uint32_t n, i;

    if (!path) {
        set_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
        return 0;
    }
    n = wlen(path);
    if (!buf || size <= n) {
        return n + 1u;
    }
    for (i = 0; i <= n; i++) {
        buf[i] = path[i];
    }
    return n;
}

uint32_t GetShortPathNameA(const char *path, char *buf, uint32_t size) {
    uint16_t w[NAME_CHARS];
    uint32_t n, i;

    if (!path) {
        set_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (!wide_name(path, w) || GetFileAttributesW(w) == INVALID_FILE_ATTRIBUTES) {
        return 0;
    }
    n = (uint32_t)lstrlenA(path);
    if (!buf || size <= n) {
        return n + 1u;
    }
    for (i = 0; i <= n; i++) {
        buf[i] = path[i];
    }
    return n;
}

/* -- code pages (WO-041): UTF-8, and everything else as the ANSI code page
 * ntdll widens with, Windows-1252. */

#define K32_CP_UTF8 65001u

int32_t WideCharToMultiByte(uint32_t cp, uint32_t flags, const uint16_t *w, int32_t wn, char *out,
                            int32_t cap, const char *def, BOOL32 *used_def) {
    int32_t i, n = 0, used = 0;

    (void)flags;
    if (!w || wn == 0 || cap < 0 || (cap > 0 && !out) ||
        (cp == K32_CP_UTF8 && (def || used_def))) {
        set_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (wn < 0) {
        wn = (int32_t)wlen(w) + 1;
    }
    for (i = 0; i < wn; i++) {
        uint8_t b[4];
        uint32_t c = w[i], k, len;

        if (cp == K32_CP_UTF8) {
            if (c >= 0xD800u && c < 0xDC00u && i + 1 < wn && w[i + 1] >= 0xDC00u && w[i + 1] < 0xE000u) {
                c = 0x10000u + ((c - 0xD800u) << 10) + (w[i + 1] - 0xDC00u);
                i++;
            } else if (c >= 0xD800u && c < 0xE000u) {
                c = 0xFFFDu;   /* a lone surrogate */
            }
            if (c < 0x80u) {
                b[0] = (uint8_t)c;
                len = 1;
            } else if (c < 0x800u) {
                b[0] = (uint8_t)(0xC0u | (c >> 6));
                b[1] = (uint8_t)(0x80u | (c & 0x3Fu));
                len = 2;
            } else if (c < 0x10000u) {
                b[0] = (uint8_t)(0xE0u | (c >> 12));
                b[1] = (uint8_t)(0x80u | ((c >> 6) & 0x3Fu));
                b[2] = (uint8_t)(0x80u | (c & 0x3Fu));
                len = 3;
            } else {
                b[0] = (uint8_t)(0xF0u | (c >> 18));
                b[1] = (uint8_t)(0x80u | ((c >> 12) & 0x3Fu));
                b[2] = (uint8_t)(0x80u | ((c >> 6) & 0x3Fu));
                b[3] = (uint8_t)(0x80u | (c & 0x3Fu));
                len = 4;
            }
        } else {
            char ch = narrow((uint16_t)c);

            if (ch == '?' && c != '?') {
                used = 1;
                if (def) {
                    ch = *def;
                }
            }
            b[0] = (uint8_t)ch;
            len = 1;
        }
        if (cap > 0) {
            if (n + (int32_t)len > cap) {
                set_error(ERROR_INSUFFICIENT_BUFFER);
                return 0;
            }
            for (k = 0; k < len; k++) {
                out[n + (int32_t)k] = (char)b[k];
            }
        }
        n += (int32_t)len;
    }
    if (used_def) {
        *used_def = used;
    }
    return n;
}

int32_t MultiByteToWideChar(uint32_t cp, uint32_t flags, const char *s, int32_t sn, uint16_t *out,
                            int32_t cap) {
    const uint8_t *b = (const uint8_t *)s;
    int32_t i = 0, n = 0;

    (void)flags;
    if (!s || sn == 0 || cap < 0 || (cap > 0 && !out)) {
        set_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (sn < 0) {
        sn = lstrlenA(s) + 1;
    }
    while (i < sn) {
        uint32_t c = b[i++], need = 0, k;
        uint16_t u[2];
        int32_t len = 1;

        if (cp == K32_CP_UTF8) {
            if (c >= 0xF0u && c < 0xF8u) {
                need = 3;
                c &= 0x07u;
            } else if (c >= 0xE0u) {
                need = 2;
                c &= 0x0Fu;
            } else if (c >= 0xC2u) {
                need = 1;
                c &= 0x1Fu;
            } else if (c >= 0x80u) {
                c = 0xFFFDu;
            }
            for (k = 0; k < need; k++) {
                if (i >= sn || (b[i] & 0xC0u) != 0x80u) {
                    c = 0xFFFDu;
                    need = 0;
                    break;
                }
                c = (c << 6) | (b[i++] & 0x3Fu);
            }
            if (c >= 0x10000u && c != 0xFFFDu) {
                c -= 0x10000u;
                u[0] = (uint16_t)(0xD800u + (c >> 10));
                u[1] = (uint16_t)(0xDC00u + (c & 0x3FFu));
                len = 2;
            } else {
                u[0] = (uint16_t)c;
            }
        } else {
            unsigned char ch = (unsigned char)c, *p = &ch;

            u[0] = RtlAnsiCharToUnicodeChar(&p);
        }
        if (cap > 0) {
            if (n + len > cap) {
                set_error(ERROR_INSUFFICIENT_BUFFER);
                return 0;
            }
            out[n] = u[0];
            if (len == 2) {
                out[n + 1] = u[1];
            }
        }
        n += len;
    }
    return n;
}

/* -- time formats (WO-042): the user default's, which is en-US. */

typedef BOOL32 (*k32_enum_a_t)(char *);
typedef BOOL32 (*k32_enum_w_t)(uint16_t *);

static const char *const g_k32_times[2][4] = {
    { "h:mm:ss tt", "hh:mm:ss tt", "H:mm:ss", "HH:mm:ss" },
    { "h:mm tt", "hh:mm tt", "H:mm", "HH:mm" },   /* TIME_NOSECONDS */
};

static BOOL32 k32_enum_times(void *proc, uint32_t flags, int wide) {
    uint32_t i, k;

    if (!proc) {
        set_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (flags & ~2u) {
        set_error(ERROR_INVALID_FLAGS);
        return 0;
    }
    for (i = 0; i < 4u; i++) {
        const char *f = g_k32_times[flags ? 1 : 0][i];
        char a[16];
        uint16_t w[16];
        BOOL32 more;

        for (k = 0; f[k]; k++) {
            a[k] = f[k];
            w[k] = (uint16_t)f[k];
        }
        a[k] = 0;
        w[k] = 0;
        more = wide ? ((k32_enum_w_t)proc)(w) : ((k32_enum_a_t)proc)(a);
        if (!more) {
            break;
        }
    }
    return 1;
}

BOOL32 EnumTimeFormatsA(void *proc, uint32_t lcid, uint32_t flags) {
    (void)lcid;
    return k32_enum_times(proc, flags, 0);
}

BOOL32 EnumTimeFormatsW(void *proc, uint32_t lcid, uint32_t flags) {
    (void)lcid;
    return k32_enum_times(proc, flags, 1);
}

/* -- SignalObjectAndWait (WO-012): the signal, then the wait - two calls, so
 * not atomic as NtSignalAndWaitForSingleObject would make it, which the
 * kernel does not have yet. An event is set, a mutant released, a semaphore
 * released by one: whichever the handle accepts. */

uint32_t SignalObjectAndWait(uint64_t signal, uint64_t wait, uint32_t ms, BOOL32 alertable) {
    NTSTATUS_T st = NtSetEvent(signal, 0);

    if (st == (NTSTATUS_T)0xC0000024u /* OBJECT_TYPE_MISMATCH */) {
        st = NtReleaseMutant(signal, 0);
    }
    if (st == (NTSTATUS_T)0xC0000024u) {
        st = NtReleaseSemaphore(signal, 1, 0);
    }
    if (failed(st)) {
        fail(st);
        return 0xFFFFFFFFu;   /* WAIT_FAILED */
    }
    return WaitForSingleObjectEx(wait, ms, alertable);
}

/* -- the environment (kernel32:environ, after W7 step 3) */

BOOL32 SetEnvironmentVariableW(const uint16_t *name, const uint16_t *value) {
    vibeos_ustring_t n, v;
    NTSTATUS_T st;

    if (!name || !name[0]) {
        set_error(ERROR_ENVVAR_NOT_FOUND);
        return 0;
    }
    n.Length = (uint16_t)(wlen(name) * 2u);
    n.MaximumLength = n.Length;
    n.Buffer = (vibeos_wchar_t *)name;
    if (value) {
        v.Length = (uint16_t)(wlen(value) * 2u);
        v.MaximumLength = v.Length;
        v.Buffer = (vibeos_wchar_t *)value;
    }
    st = (NTSTATUS_T)RtlSetEnvironmentVariable(0, &n, value ? &v : 0);
    return failed(st) ? fail(st) : 1;
}

BOOL32 SetEnvironmentVariableA(const char *name, const char *value) {
    uint16_t wn[NAME_CHARS], *wv = 0;
    BOOL32 r;

    if (!name || widen(name, wn, NAME_CHARS) < 0) {
        set_error(ERROR_ENVVAR_NOT_FOUND);
        return 0;
    }
    if (value) {
        uint32_t n = (uint32_t)lstrlenA(value) + 1u;

        wv = (uint16_t *)HeapAlloc(GetProcessHeap(), 0, n * 2u);
        if (!wv || widen(value, wv, n) < 0) {
            HeapFree(GetProcessHeap(), 0, wv);
            set_error(8u);
            return 0;
        }
    }
    r = SetEnvironmentVariableW(wn, wv);
    if (wv) {
        HeapFree(GetProcessHeap(), 0, wv);
    }
    return r;
}

/* -- FormatMessage (WO-040): a message from a string, with its inserts.
 * There are no message tables here, so FROM_SYSTEM and FROM_HMODULE find
 * nothing and say ERROR_MR_MID_NOT_FOUND. An insert is %1 to %99 with an
 * optional !printf-format!, taken from the argument array or, without
 * ARGUMENT_ARRAY, from the va_list - every slot eight bytes in this ABI, so
 * the n-th insert is the n-th slot. %0 ends the message, %n is a line break,
 * %% and %. %! and %space are the character. Width wrapping is not done:
 * FORMAT_MESSAGE_MAX_WIDTH_MASK only turns the source's line breaks into
 * spaces. The wide form's %s is a wide string, the ANSI form's an ANSI one. */

#define K32_FM_ALLOCATE   0x0100u
#define K32_FM_IGNORE     0x0200u
#define K32_FM_STRING     0x0400u
#define K32_FM_HMODULE    0x0800u
#define K32_FM_SYSTEM     0x1000u
#define K32_FM_ARRAY      0x2000u
#define K32_FM_WIDTH      0x00FFu

typedef struct {
    uint16_t *b;
    uint32_t n, cap;
    int oom;
} k32_out_t;

static void k32_put(k32_out_t *o, uint16_t c) {
    if (o->n + 1u >= o->cap) {
        uint32_t cap = o->cap ? o->cap * 2u : 256u;
        uint16_t *nb = (uint16_t *)(o->b ? HeapReAlloc(GetProcessHeap(), 0, o->b, cap * 2u)
                                         : HeapAlloc(GetProcessHeap(), 0, cap * 2u));

        if (!nb) {
            o->oom = 1;
            return;
        }
        o->b = nb;
        o->cap = cap;
    }
    o->b[o->n++] = c;
}

/* One printf conversion - `spec` is what stood between the bangs, without
 * them - of one argument, and any '*' widths from the slots after it. */
static void k32_format_one(k32_out_t *o, const uint16_t *spec, uint32_t len, const uint64_t *slot,
                           uint32_t *used, int wide) {
    char flags[8];
    uint32_t nf = 0, i = 0, width = 0, prec = 0, k;
    int has_prec = 0, longlong = 0, half = 0;
    char conv;
    char num[72];
    uint32_t nn = 0;
    uint64_t v;

    *used = 1;
    while (i < len && (spec[i] == '-' || spec[i] == '+' || spec[i] == ' ' || spec[i] == '#' ||
                       spec[i] == '0') && nf < sizeof(flags)) {
        flags[nf++] = (char)spec[i++];
    }
    if (i < len && spec[i] == '*') {
        width = (uint32_t)slot[0];
        slot++;
        (*used)++;
        i++;
    }
    while (i < len && spec[i] >= '0' && spec[i] <= '9') {
        width = width * 10u + (spec[i++] - '0');
    }
    if (i < len && spec[i] == '.') {
        has_prec = 1;
        i++;
        if (i < len && spec[i] == '*') {
            prec = (uint32_t)slot[0];
            slot++;
            (*used)++;
            i++;
        }
        while (i < len && spec[i] >= '0' && spec[i] <= '9') {
            prec = prec * 10u + (spec[i++] - '0');
        }
    }
    while (i < len && (spec[i] == 'l' || spec[i] == 'h' || spec[i] == 'I' || spec[i] == '6' ||
                       spec[i] == '4' || spec[i] == '3' || spec[i] == '2' || spec[i] == 'w')) {
        if (spec[i] == '6' || (spec[i] == 'l' && i + 1 < len && spec[i + 1] == 'l')) {
            longlong = 1;
        }
        if (spec[i] == 'h') {
            half = 1;
        }
        if (spec[i] == 'w') {
            half = -1;   /* a wide string or character whatever the form */
        }
        i++;
    }
    conv = i < len ? (char)spec[i] : 's';
    v = slot[0];
    if (conv == 's' || conv == 'S') {
        int w = (conv == 's') == (wide != 0);   /* %s is the form's own width, %S the other */
        uint32_t n = 0, pad, left = 0;

        if (half == -1) {
            w = 1;
        } else if (half == 1) {
            w = 0;
        }
        if (v == 0u) {
            static const char null_s[] = "(null)";

            for (k = 0; null_s[k]; k++) {
                k32_put(o, (uint16_t)null_s[k]);
            }
            return;
        }
        if (w) {
            const uint16_t *s = (const uint16_t *)(uintptr_t)v;

            while (s[n] && (!has_prec || n < prec)) {
                n++;
            }
        } else {
            const char *s = (const char *)(uintptr_t)v;

            while (s[n] && (!has_prec || n < prec)) {
                n++;
            }
        }
        for (k = 0; k < nf; k++) {
            left |= flags[k] == '-';
        }
        pad = width > n ? width - n : 0u;
        for (k = 0; !left && k < pad; k++) {
            k32_put(o, ' ');
        }
        for (k = 0; k < n; k++) {
            k32_put(o, w ? ((const uint16_t *)(uintptr_t)v)[k]
                         : (uint16_t)(unsigned char)((const char *)(uintptr_t)v)[k]);
        }
        for (k = 0; left && k < pad; k++) {
            k32_put(o, ' ');
        }
        return;
    }
    if (conv == 'c' || conv == 'C') {
        k32_put(o, (uint16_t)v);
        return;
    }
    {
        int neg = 0, left = 0, zero = 0, plus = 0, space = 0, alt = 0;
        uint32_t base = (conv == 'x' || conv == 'X' || conv == 'p') ? 16u : conv == 'o' ? 8u : 10u;
        const char *digits = conv == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
        char prefix[3] = { 0, 0, 0 };
        uint32_t np = 0, total, pad;

        for (k = 0; k < nf; k++) {
            left |= flags[k] == '-';
            zero |= flags[k] == '0';
            plus |= flags[k] == '+';
            space |= flags[k] == ' ';
            alt |= flags[k] == '#';
        }
        if (conv == 'p') {
            longlong = 1;
            if (!has_prec) {
                has_prec = 1;
                prec = 16;
            }
        }
        if (!longlong) {
            v = half == 1 ? (uint64_t)(uint16_t)v : (uint64_t)(uint32_t)v;
        }
        if (conv == 'd' || conv == 'i') {
            int64_t s = longlong ? (int64_t)v : half == 1 ? (int64_t)(int16_t)v : (int64_t)(int32_t)v;

            if (s < 0) {
                neg = 1;
                v = (uint64_t)(-(s + 1)) + 1u;
            } else {
                v = (uint64_t)s;
            }
        }
        do {
            num[nn++] = digits[v % base];
            v /= base;
        } while (v != 0u && nn < sizeof(num));
        if (has_prec && prec == 0u && nn == 1u && num[0] == '0') {
            nn = 0;
        }
        while (has_prec && nn < prec && nn < sizeof(num)) {
            num[nn++] = '0';
        }
        if (neg) {
            prefix[np++] = '-';
        } else if (plus && (conv == 'd' || conv == 'i')) {
            prefix[np++] = '+';
        } else if (space && (conv == 'd' || conv == 'i')) {
            prefix[np++] = ' ';
        }
        if (alt && base == 16u && slot[0] != 0u) {
            prefix[np++] = '0';
            prefix[np++] = conv == 'X' ? 'X' : 'x';
        }
        total = nn + np;
        pad = width > total ? width - total : 0u;
        for (k = 0; !left && !(zero && !has_prec) && k < pad; k++) {
            k32_put(o, ' ');
        }
        for (k = 0; k < np; k++) {
            k32_put(o, (uint16_t)prefix[k]);
        }
        for (k = 0; !left && zero && !has_prec && k < pad; k++) {
            k32_put(o, '0');
        }
        while (nn > 0u) {
            k32_put(o, (uint16_t)num[--nn]);
        }
        for (k = 0; left && k < pad; k++) {
            k32_put(o, ' ');
        }
    }
}

/* The message, in wide characters, into `o`. A Windows error, or 0. */
static uint32_t k32_format(uint32_t flags, const uint16_t *src, void *args, k32_out_t *o, int wide) {
    const uint64_t *slots = 0;
    uint32_t i = 0;

    if (args) {
        slots = (flags & K32_FM_ARRAY) ? (const uint64_t *)args
                                       : *(const uint64_t *const *)args;   /* a va_list's slots */
    }
    while (src[i]) {
        uint16_t c = src[i++];

        if (c == '\r' || c == '\n') {
            if ((flags & K32_FM_WIDTH) == K32_FM_WIDTH) {
                if (c == '\r' && src[i] == '\n') {
                    i++;
                }
                k32_put(o, ' ');
            } else {
                k32_put(o, c);
            }
            continue;
        }
        if (c != '%') {
            k32_put(o, c);
            continue;
        }
        c = src[i];
        if (c == 0) {
            break;
        }
        if (c == '0') {
            break;   /* %0: the message ends here, with no line break */
        }
        if (c >= '1' && c <= '9') {
            uint32_t n = (uint32_t)(c - '0'), start = i - 1u, spec = 0, spec_len = 0, used = 0;

            i++;
            if (src[i] >= '0' && src[i] <= '9') {
                n = n * 10u + (uint32_t)(src[i++] - '0');
            }
            if (src[i] == '!') {
                spec = i + 1u;
                while (src[spec + spec_len] && src[spec + spec_len] != '!') {
                    spec_len++;
                }
                if (!src[spec + spec_len]) {
                    return ERROR_INVALID_PARAMETER;   /* an unterminated format */
                }
                i = spec + spec_len + 1u;
            }
            if (flags & K32_FM_IGNORE) {
                uint32_t k;

                for (k = start; k < i; k++) {
                    k32_put(o, src[k]);
                }
                continue;
            }
            if (!slots) {
                return ERROR_INVALID_PARAMETER;
            }
            if (spec) {
                k32_format_one(o, src + spec, spec_len, slots + n - 1u, &used, wide);
            } else {
                static const uint16_t s_spec[1] = { 's' };

                k32_format_one(o, s_spec, 1u, slots + n - 1u, &used, wide);
            }
            continue;
        }
        i++;
        if (c == 'n') {
            k32_put(o, '\r');
            k32_put(o, '\n');
        } else if (c == 'r') {
            k32_put(o, '\r');
        } else if (c == 't') {
            k32_put(o, '\t');
        } else {
            k32_put(o, c);   /* %% %. %! %space and the rest: the character */
        }
    }
    return o->oom ? 8u /* ERROR_NOT_ENOUGH_MEMORY */ : 0u;
}

uint32_t FormatMessageW(uint32_t flags, const void *source, uint32_t id, uint32_t lang, uint16_t *buf,
                        uint32_t size, void *args) {
    k32_out_t o = { 0, 0, 0, 0 };
    uint32_t err, i;

    (void)id;
    (void)lang;
    if (!(flags & K32_FM_STRING)) {
        set_error(ERROR_MR_MID_NOT_FOUND);   /* no message tables (FROM_SYSTEM, FROM_HMODULE) */
        return 0;
    }
    if (!source || (!buf && !(flags & K32_FM_ALLOCATE))) {
        set_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    err = k32_format(flags, (const uint16_t *)source, args, &o, 1);
    if (err == 0u) {
        k32_put(&o, 0);
        err = o.oom ? 8u : 0u;
    }
    if (err != 0u) {
        HeapFree(GetProcessHeap(), 0, o.b);
        set_error(err);
        return 0;
    }
    o.n--;   /* the terminator is not counted */
    if (flags & K32_FM_ALLOCATE) {
        uint16_t *a = (uint16_t *)LocalAlloc(0, ((o.n + 1u) > size ? o.n + 1u : size) * 2u);

        if (!a) {
            HeapFree(GetProcessHeap(), 0, o.b);
            return 0;
        }
        for (i = 0; i <= o.n; i++) {
            a[i] = o.b[i];
        }
        *(uint16_t **)buf = a;
    } else if (o.n + 1u > size) {
        HeapFree(GetProcessHeap(), 0, o.b);
        set_error(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    } else {
        for (i = 0; i <= o.n; i++) {
            buf[i] = o.b[i];
        }
    }
    HeapFree(GetProcessHeap(), 0, o.b);
    return o.n;
}

uint32_t FormatMessageA(uint32_t flags, const void *source, uint32_t id, uint32_t lang, char *buf,
                        uint32_t size, void *args) {
    k32_out_t o = { 0, 0, 0, 0 };
    uint16_t *w = 0;
    uint32_t err, i, n;

    (void)id;
    (void)lang;
    if (!(flags & K32_FM_STRING)) {
        set_error(ERROR_MR_MID_NOT_FOUND);
        return 0;
    }
    if (!source || (!buf && !(flags & K32_FM_ALLOCATE))) {
        set_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    n = (uint32_t)lstrlenA((const char *)source) + 1u;
    w = (uint16_t *)HeapAlloc(GetProcessHeap(), 0, n * 2u);
    if (!w) {
        set_error(8u);
        return 0;
    }
    for (i = 0; i < n; i++) {
        unsigned char ch = ((const unsigned char *)source)[i], *p = &ch;

        w[i] = ch < 0x80u ? ch : RtlAnsiCharToUnicodeChar(&p);
    }
    err = k32_format(flags, w, args, &o, 0);
    HeapFree(GetProcessHeap(), 0, w);
    if (err == 0u) {
        k32_put(&o, 0);
        err = o.oom ? 8u : 0u;
    }
    if (err != 0u) {
        HeapFree(GetProcessHeap(), 0, o.b);
        set_error(err);
        return 0;
    }
    o.n--;
    if (flags & K32_FM_ALLOCATE) {
        char *a = (char *)LocalAlloc(0, (o.n + 1u) > size ? o.n + 1u : size);

        if (!a) {
            HeapFree(GetProcessHeap(), 0, o.b);
            return 0;
        }
        for (i = 0; i <= o.n; i++) {
            a[i] = narrow(o.b[i]);
        }
        *(char **)buf = a;
    } else if (o.n + 1u > size) {
        HeapFree(GetProcessHeap(), 0, o.b);
        set_error(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    } else {
        for (i = 0; i <= o.n; i++) {
            buf[i] = narrow(o.b[i]);
        }
    }
    HeapFree(GetProcessHeap(), 0, o.b);
    return o.n;
}

/* ---- locale and machine, as user32 and gdi32's tests ask (W8 steps 5, 6) ----------------- */

/* English (United States): the only locale there is, and the one Windows-1252,
 * the code page ntdll widens with, belongs to. */
uint16_t GetSystemDefaultLangID(void) { return 0x0409u; }
uint16_t GetUserDefaultLangID(void) { return 0x0409u; }
uint32_t GetSystemDefaultLCID(void) { return 0x0409u; }
uint32_t GetUserDefaultLCID(void) { return 0x0409u; }
uint32_t GetACP(void) { return 1252u; }
uint32_t GetOEMCP(void) { return 437u; }

/* A 64-bit process on a 64-bit machine: not under WOW64. */
int32_t IsWow64Process(uint64_t process, int32_t *wow64) {
    (void)process;
    if (!wow64) {
        set_error(87u);
        return 0;
    }
    *wow64 = 0;
    return 1;
}

/* CPINFO: a single-byte code page has no lead bytes. */
int32_t GetCPInfo(uint32_t cp, uint8_t *info) {
    if (!info || (cp != 0u && cp != 1u && cp != 3u && cp != 1252u && cp != 437u && cp != 20127u)) {
        set_error(87u);
        return 0;
    }
    *(uint32_t *)info = 1u;   /* MaxCharSize */
    info[4] = '?';
    info[5] = 0;
    for (int i = 6; i < 18; i++) {
        info[i] = 0;
    }
    return 1;
}

/* CPINFOEXW: CPINFO, then the default character in UTF-16, the code page's
 * number and its name. */
int32_t GetCPInfoExW(uint32_t cp, uint32_t flags, uint8_t *info) {
    static const char *const name = "1252  (ANSI - Latin I)";
    uint16_t *w;
    int i;

    (void)flags;
    if (!GetCPInfo(cp, info)) {
        return 0;
    }
    if (cp == 0u || cp == 1u || cp == 3u) {
        cp = 1252u;
    }
    *(uint16_t *)(info + 18) = '?';
    *(uint32_t *)(info + 20) = cp;
    w = (uint16_t *)(info + 24);
    if (cp == 1252u) {
        for (i = 0; name[i]; i++) {
            w[i] = (uint16_t)name[i];
        }
        w[i] = 0;
    } else {
        w[0] = 0;
    }
    return 1;
}

int32_t IsDBCSLeadByte(uint8_t c) {
    (void)c;
    return 0;
}
