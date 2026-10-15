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

## W1 (2026-10-14): done

A PE program built by mingw-w64 runs on our own `ntdll`, from a boot's shell
like any program: `/win/hello.exe` checks its TEB through `%gs`, its PEB, and
each W1 service, and prints `NT_HELLO_OK` through `NtWriteFile`. The gate
requires the line.

What it took, in the order it was built:

1. **A `%gs` for each task** (`0d83e8e`). The kernel kept its per-CPU block in
   GS.base for good and never ran `swapgs`; a Windows program reaches its TEB
   through `%gs`. Every entry swaps when GS.base is not inside the kernel image
   - deciding by GS.base, not the saved CS, so an `iretq` that faults after the
   exit's swap still finds the kernel's block - and every exit to ring 3 swaps
   back with interrupts off. Linux's `ARCH_SET_GS` works on top of it.
2. **A PE32+ parser** (`f2c598d`, `kernel/core/pe.c`) that describes an image in
   the ELF parser's segment form, so the architecture places it with the mapper
   it has. Tested against an image mingw-w64 linked, compared with what mingw's
   `objdump` says of it.
3. **The NT personality**: `kernel/abi/nt_services.def` (all 237 services, the
   number being the line), a registry checked against the rows at boot in both
   directions, a dispatcher that reads Windows' convention - four arguments in
   `r10`, `rdx`, `r8`, `r9`, the rest from the caller's stack at `+0x28`,
   judged before they are read - and the six W1 services. A service with no row
   is `STATUS_NOT_IMPLEMENTED`, counted and named on the console; the gate
   allows exactly one, the probe `hello.exe` asks for on purpose.
4. **Loading**: exec of a file that starts `MZ` maps the program below the
   stack, `/win/ntdll.dll` above the heap, and five pages for the PEB, the
   process parameters and the TEB just below the mapping area; binds the task to
   the NT personality; and starts the thread at ntdll's entry with the PEB in
   `rcx` and the TEB as its `%gs`. Our ntdll resolves the program's imports
   against its own exports and calls its entry.
5. **Our ntdll** (`user/win/ntdll`): one stub per service, generated from the
   same `.def` the kernel is checked against, and `layout_check.c`, which holds
   `include/vibeos/nt_layout.h` against mingw-w64's `winternl.h` and `winnt.h`
   at every build.

Decisions W1 made that later steps inherit, each said where it is made:

- **A handle is a descriptor** for now: `(fd + 1) * 4`, so the standard handles
  are 4, 8 and 12. W2's object manager decides whether NT handles get a table
  of their own or share the descriptor table, which already holds typed,
  counted objects.
- **No relocation**: an image is placed where its header says, inside the
  window it is allowed, or refused (`pe_needs_relocation`). Wine's test
  executables are linked at `0x140000000` and need it, so it is W2's first
  piece of work, before a single test runs.
- **`/win`** is where the DLL and the program live, provisionally: the
  filesystem's hierarchy is the roadmap's next item.
- **One process, one thread**: `NtTerminateProcess` and
  `NtQueryInformationProcess` answer for the caller only; the parent of an NT
  program is a Linux one, so the kernel builds the process parameters itself
  from `argv` until W6's `NtCreateUserProcess`.

Measured: host tests (`nt_abi_tests.c`, `pe_tests.c`), sabotage on the host
(`core-pe.txt` 8/8, `nt-dispatch.txt`, `nt-services.txt`, `nt-start.txt` 13/13)
and at boot (`arch-gs-boot.txt` 2/2, `nt-hello-boot.txt` 1/1), check, six boots.

Next: W2 - base relocation, stub `user32` and `advapi32` for the tests' imports,
then the object manager, handles and waits, measured by `ntdll:om`,
`ntdll:sync` and `kernel32:sync`.

## W2 (2026-10-15): planned, in progress

What W0's measurement says W2 has to be. Wine's `ntdll_test.exe` is an
eight-megabyte image linked at `0x140000000`, and it imports 318 functions
from five DLLs - 141 from `kernel32`, 113 from `ntdll` (most of them `Rtl*`,
not system calls), 44 from `msvcrt`, 13 from `advapi32`, 7 from `user32`;
`kernel32_test.exe` imports 612. So before the first test can run, three things
the plan placed later have to come first:

1. **Base relocation.** `0x140000000` is no user address here. The kernel
   places an image it cannot put where it asks at `USER_BASE + 8 MiB` (an NT
   process has no `brk`) and applies its relocations as each page is filled -
   one `.reloc` block per page, a value straddling two pages read from the file
   and written in halves. Tested against mingw's own image and `objdump`'s list
   of its relocations; `hello.exe` is linked at mingw's default base, so every
   boot relocates it.
2. **Loading DLLs from disk.** On Windows that is ntdll's loader calling
   `NtOpenFile`, `NtCreateSection(SEC_IMAGE)` and `NtMapViewOfSection` - so a
   minimal form of each comes forward from W3 and W4, and the image mapping is
   the kernel's, relocation included.
3. **DLLs that load before they are complete.** A missing export is bound to a
   stub that, when called, prints `unimplemented: kernel32.CreateFileA` and ends
   the process. The tests then say which function to write next, as LTP did for
   Linux. `msvcrt.dll` - formatting, strings, a heap, `argv` - is written in
   full now, being the test harness's own runtime.

**Handles are descriptors** (decided here; W1 left it open). An NT handle
names an entry of the process's existing descriptor table, `(fd + 1) * 4`;
the entry's spare 32 bits hold the handle's access mask. Events, mutants,
semaphores and sections become description types, as eventfd and pidfd already
are, with an NT extension to their operations: whether the object is signalled
for a waiter, and what acquiring it does. Reference counting, closing, fork and
the wait machinery are then the ones seven phases of Linux work have tested,
not a second set; named objects are a namespace of names onto descriptions.

Order: relocation; the object manager's core (types, `NtClose`,
`NtDuplicateObject`, the namespace); `NtOpenFile` and image sections; ntdll's
loader; events, mutants, semaphores and the waits; then `ntdll:om` and
`ntdll:sync` run.
