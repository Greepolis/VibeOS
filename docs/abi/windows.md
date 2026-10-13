# The NT personality: Windows programs on VibeOS

The second ABI after Linux's. Decided on 2026-10-13, after the Linux plan's L7,
and first of the six things [../roadmap.md](../roadmap.md) puts before L8.

## What was decided, and why

**Our own `ntdll.dll`, and an NT personality in the kernel under it.** A Windows
program never makes a system call itself: it calls `ntdll.dll`, and the numbers
`ntdll` uses change with every Windows build. So "Windows system calls" is not a
table of someone else's numbers to honour, as Linux's was - it is a choice of who
writes `ntdll`. We do, from scratch, as ReactOS and Wine did: its `Nt*`
functions call a second table of rows in the kernel, numbered by us because our
`ntdll` is the only caller, and those rows reach the kernel through
`include/vibeos/ksvc.h` exactly as the Linux handlers do. That is what the
services interface was kept personality-neutral for since docs/abi/ A2. No
Microsoft binary is shipped or needed.

The alternatives, and why not: running Wine as a Linux program would bring
Win32 almost for free on L1-L7 and give the kernel no Windows personality at all;
honouring one real Windows build's syscall numbers under Microsoft's own `ntdll`
ties the system to a build and to binaries that cannot be distributed.

**Handles and objects live in the kernel.** A portable object manager, with a
handle table per process - the counterpart of A3's open file descriptions.
`WaitForMultipleObjects` over events, mutexes, semaphores, threads and processes
needs the kernel anyway, and a user-space subsystem service would be one more
moving part on every wait. [../compatibility.md](../compatibility.md) proposed a
service outside the kernel; what stays outside is what does not need it - the
registry, and later the window system.

**The oracle is Wine's conformance tests.** Wine's own tests of `ntdll` and
`kernel32` (`dlls/*/tests` in the Wine source), built with mingw-w64 into PE
executables. Each is run first under Wine on the build host, which is what the
answers are compared with - the corpus's rule, "compare with what the reference
printed, not with what you expect". They are LGPL and used only as an oracle:
nothing of Wine goes into VibeOS, which is MIT, and our `ntdll` and `kernel32`
are written without its code in front of us.

Unlike LTP, the tests cannot be the measure from the first day: they need a PE
loader, an `ntdll`, a `kernel32` and a C runtime before the first line runs. W1
gets a program of our own to print "hello"; from W2 on the tests measure.

## The pieces

| Piece | Where | Built with |
|---|---|---|
| PE32+ loader: map, relocate, resolve imports against our DLLs, TEB and PEB (GS base), first thread | the kernel's exec layer, beside ELF | the kernel's compiler |
| NT rows: `Nt*` services, our numbering, a registry like `linux_syscalls.def` | `kernel/abi/nt/` | the kernel's compiler |
| Object manager: object types, per-process handle tables, waits | `kernel/ob/`, portable, host-tested | the kernel's compiler |
| `ntdll.dll`, then `kernel32.dll` and a C runtime | `user/win/` | mingw-w64 |
| The oracle: Wine 9.0's `ntdll` and `kernel32` tests | `scripts/dev/winetest-build.sh`, cached outside the tree | mingw-w64 |

The NT handlers follow the Linux handlers' rules: nothing under `kernel/abi/nt/`
names the architecture, every user pointer is judged before it is used, and
`check-abi-layering.py` covers the directory from its first file.

## Steps

| Step | What | Measured by |
|---|---|---|
| W0 | This plan; the oracle built and run under Wine on the host, its results recorded; the registry's format | the list of tests per area, and Wine's own answers |
| W1 | PE32+ loader and process start; `ntdll` minimal (`NtWriteFile` to the console, `NtTerminateProcess`) | a program of ours prints "hello" |
| W2 | Object manager and handles: `NtClose`, `NtDuplicateObject`, events, mutants, semaphores, `NtWaitForSingleObject`/`Multiple` | `ntdll:om`, `ntdll:sync`, `kernel32:sync` |
| W3 | Memory: `NtAllocateVirtualMemory`, `Free`, `Protect`, `Query`; sections and views | `ntdll:virtual`, `kernel32:virtual` |
| W4 | Files: `NtCreateFile`, `Read`, `Write`, `QueryInformationFile`, directories; `\??\C:\` onto the VFS, names without case | `ntdll:file`, `ntdll:directory`, `kernel32:file` |
| W5 | Threads and exceptions: `NtCreateThreadEx`, TLS, SEH through `KiUserExceptionDispatcher`, APCs, alertable waits | `ntdll:exception`, `ntdll:thread`, `kernel32:thread` |
| W6 | Processes: `NtCreateUserProcess`, environment, command line, exit codes | `kernel32:process` |
| W7 | `kernel32` and the console, as far as the tests ask | the rest of `kernel32`'s tests |

Each step is closed as an L-step is: host tests, sabotage cases, check, six
boots, and the step's own tests from the oracle.

## What this plan does not do

No graphics: `user32`, `gdi32` and the window system wait for the user interface
(the roadmap's third item). No registry in the kernel: when `kernel32`'s tests
need `advapi32`, the registry is a service of its own. No 32-bit programs (WoW64).
No driver model. Each of those is a later decision, made when a test or a program
asks for it.

## W0 (2026-10-13): done

- The oracle's source is Wine 9.0 (`tests/corpus/sources.txt`), the version Ubuntu
  24.04 packages as `wine64`, so the tests and the reference answering them are
  one release. Its sha256 is pinned on first use: WineHQ signs its tarballs with
  PGP and publishes no digest.
- `ntdll` has 25 test files and `kernel32` 34; `kernelbase` 4, `msvcrt` 13 and
  `ucrtbase` 8 come later, if a step needs them.
- Built by Wine's own build system (`scripts/dev/winetest-build.sh`), configured
  without X and without any host library, making only the two test executables.
  Compiling the test files alone against mingw-w64 was tried first and does not
  work: they include headers Wine generates from IDL, Wine's headers and mingw's
  disagree on basic types, and they link against helpers that live in Wine's own
  import libraries. Host needs: mingw-w64, flex, bison, and `wine64` for the
  answers.
- The executables import `user32` and `advapi32` besides `kernel32`, `ntdll` and
  `msvcrt`, though nothing W1-W6 measures calls into the first two. VibeOS will
  need stubs of both that load and answer nothing, before W2 runs a single test.
- Run under Wine on the host by `scripts/dev/winetest-run-host.sh`, which keeps
  every test's output and Wine's own summary line; a test's exit code is its
  failure count. The answers are kept in `tests/corpus/winetest-wine9.txt`.
- Every service the plan has to provide is listed, one by one, in
  [nt-services.md](nt-services.md): Wine 9.0's 237 `Nt*` system calls, each
  given to a step, to the registry service, to the security plan, or refused
  with its reason.

### What Wine answered (Wine 9.0 on WSL2, 2026-10-13)

58 tests: 25 of `ntdll`, 33 of `kernel32`. 52 end with no failure. Six do not,
and VibeOS is compared with the answer, not with a pass:

| Test | Wine's answer | What it means for the plan |
|---|---|---|
| `ntdll:exception` | stops at the 38th case of its fault table, no summary | W5 cannot take this file whole as its measure; its cases before the stop can |
| `kernel32:virtual` | stops; 679 failures printed, the process that printed them never summarised | W3 measures with `ntdll:virtual`, which Wine finishes, until this one is understood |
| `kernel32:loader` | 48 failures, all one check of section protections (`got 0x4, expected 0x8`) | W1's loader is compared per check, not per file |
| `kernel32:process` | 44 failures: console modes, a standard handle of a GUI program, one error code | W6 |
| `ntdll:wow64` | 4 failures | 32-bit programs: not planned |
| `kernel32:console` | 1 failure | W7 |

A test that starts copies of itself prints one summary line per process; the
run script takes the parent's (the largest) and calls a log whose printed
failures outnumber its summaries incomplete - taking the last line, as the first
version did, reported `kernel32:virtual` as a child's "0 failures".

W0 is done. Next: W1, the loader and a program of ours that prints "hello".
