# Windows personality: open items

One register for what the NT personality leaves open, so that nothing named
in a step's write-up is lost in the next one. Each item has an ID that never
changes, the step that found it, where it shows, and a status. A step that
closes an item says so here and in its own section of
[../abi/windows.md](../abi/windows.md); a step that finds one adds a row.

Status: **open**, **closed** (with the commit), **out of plan** (named
deliberately, with the reason - not forgotten).

## Defects and gaps

| ID | Found | What | Where it shows | Status |
|---|---|---|---|---|
| WO-001 | W3 | No arena below four gigabytes: ZeroBits and address requirements are refused rather than honoured | `ntdll:virtual` (ten checks), `kernel32:virtual` test_VirtualProtect | open |
| WO-002 | W3 | Write watches (MEM_WRITE_WATCH) are STATUS_NOT_SUPPORTED | `ntdll:virtual`, `kernel32:virtual` test_write_watch | open |
| WO-003 | W3 | PAGE_GUARD and PAGE_NOCACHE are recorded, not enforced | - | open |
| WO-004 | W3 | RtlCreateUserStack is missing | - | open |
| WO-005 | W4 | `ntdll:directory` fails 70 of 2,707 checks | `ntdll:directory` | open |
| WO-006 | W4 | NtLockFile neither waits nor honours its Key | `ntdll:file` | open |
| WO-007 | W5 | Another thread's context (Get/SetThreadContext on a running thread) | `ntdll:exception`, `kernel32:thread` | open |
| WO-008 | W5 | Timer APC routines are not delivered | `kernel32:sync` | open |
| WO-009 | W5 | A DLL's static TLS; FLS callbacks at a thread's end | `kernel32:thread` | open |
| WO-010 | W5 | Single-stepping: the trap flag is neither cleared nor delivered | `ntdll:exception` (2 failures) | open |
| WO-011 | W5 | Keyed events waited on by a second thread time out | `ntdll:sync` (5 failures) | open |
| WO-012 | W5 | SignalObjectAndWait (NtSignalAndWaitForSingleObject is MISSING) | `kernel32:sync` stops there | open (W7 step 4) |
| WO-013 | W5 | Named pipes and object directories | `ntdll:om` (20+ failures), `kernel32:pipe` | open |
| WO-014 | W6 | Taking a handle *out of* another process (NtDuplicateObject with another source) | - | open |
| WO-015 | W6 | NtQueryInformationProcess classes past ProcessBasicInformation | - | open |
| WO-016 | W6 | NtCreateUserProcess ignores create_info and the attribute list; HIDE_FROM_DEBUGGER is not refused as Windows refuses it | `ntdll:thread` (3 failures) | open |
| WO-017 | W7 s1 | A suspended child has not exec'd its image yet (fork then exec), so memory written or a thread made in it before resume belongs to the forked copy and goes with the exec | nothing in the oracle asks | open |
| WO-018 | W7 s1 | NtRead/WriteVirtualMemory do not reach a view's untouched page or a page in swap: a partial copy | - | open |
| WO-019 | W7 s1 | `kernel32:virtual` reaches test_VirtualAllocEx with its address space already spent (ERROR_NOT_ENOUGH_MEMORY) after test_VirtualProtect | `kernel32:virtual` | open |
| WO-020 | W7 s2 | `kernel32:thread` prints no failure but its process dies of an access violation after the TLS tests: no summary | `kernel32:thread` | open |
| WO-021 | W7 s3 | `kernel32:environ` hangs with nothing printed | `kernel32:environ` | open (W7 step 4) |
| WO-022 | W7 s3 | `kernel32:module` hangs with nothing printed | `kernel32:module` | open (W7 step 4) |

## kernel32 functions the oracle stops at (W7 step 3)

Each is the first missing function of one test; writing it lets the test run
on to the next. Step 4's list.

| ID | Function(s) | Test | Status |
|---|---|---|---|
| WO-030 | SystemTimeToFileTime, FileTimeToSystemTime | time, timer | open |
| WO-031 | GetVersionExA | version | open |
| WO-032 | GetSystemPowerStatus | power | open |
| WO-033 | Beep | process | open |
| WO-034 | SetCurrentDirectoryA | directory, path | open |
| WO-035 | GetLogicalDrives | drive | open |
| WO-036 | QueryDosDeviceA | volume | open |
| WO-037 | LocalAlloc | file | open |
| WO-038 | GetProcessHeaps | heap | open |
| WO-039 | GetShortPathNameA | loader | open |
| WO-040 | FormatMessageA | format_msg | open |
| WO-041 | WideCharToMultiByte | codepage | open |
| WO-042 | EnumTimeFormatsA | locale | open |

## Out of plan for W7

Named so that they are a decision, not an omission. Each needs another DLL
or a subsystem of its own.

| ID | What | Test | Why not W7 |
|---|---|---|---|
| WO-050 | advapi32's registry (RegOpenKeyExA, RegCreateKeyExA) | console, debugger | the registry is its own step (REG) |
| WO-051 | user32 (GetDesktopWindow) | atom | no user32 yet |
| WO-052 | Change notifications (FindFirstChangeNotificationA) | change | needs a watch mechanism in the VFS |
| WO-053 | Serial ports (GetDefaultCommConfigA) | comm | no serial device for NT |
| WO-054 | Mailslots (GetMailslotInfo) | mailslot | with named pipes (WO-013) |
| WO-055 | Profiles (GetPrivateProfileIntA) | profile | an INI layer of its own |
| WO-056 | Resource updating (BeginUpdateResourceA) | resource | writes PE images |
| WO-057 | Activation contexts | actctx (skips itself) | SxS manifests |
| WO-058 | Fibers | fiber (skips itself) | not asked by any other test yet |
| WO-059 | Toolhelp snapshots | toolhelp (skips itself) | a process/thread walk API |
