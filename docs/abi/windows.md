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
| W3 | Memory: `NtAllocateVirtualMemory`, `Free`, `Protect`, `Query`; sections and views; and, from the reviews, an address space shared as one object between CLONE_VM processes (M-100), a page cache that knows files by node so a view maps from its handle (M-106), kernel stacks in a region of their own with guard pages and #PF/#DF on IST stacks (M-108) | `ntdll:virtual`, `kernel32:virtual` |
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

## W2 (2026-10-15 to 2026-10-17): done

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

### Done: base relocation (`8cdb675`)

`hello.exe` is linked at `0x140000000` and every boot moves it to
`USER_BASE + 8 MiB`; the kernel's log says `moved from 0x140000000`.

### Done: the object manager's core, events and waits (`f8dfd6f`)

`NtClose`, `NtDuplicateObject`, events and both waits, on the descriptor table
with access masks and a flat namespace of whole paths. Host-tested, 19 sabotage
cases red; no Windows program uses them at boot yet.

### Done: msvcrt.dll

Wine's test executables import their C runtime from `msvcrt.dll`, so VibeOS
ships one, written from the C standard and Microsoft's documentation and never
from another implementation: `user/win/msvcrt`, a freestanding x64 DLL
exporting exactly the 74 names the tests import (`msvcrt.def`), importing only
`NtWriteFile`, `NtTerminateProcess` and `NtQuerySystemTime` from our `ntdll`.
It is not loaded by anything yet.

- **The logic is portable C** (`crt_core.c`, `uint16_t` for a wide character,
  `int32_t` for msvcrt's `long`) that mingw-w64 builds into the DLL and the
  host builds into `vibeos_msvcrt_tests`; `msvcrt.c` is the Windows part -
  the exported names, the three standard streams (`__iob_func` returns an array
  whose element size is checked against mingw's `struct _iobuf`), the heap's
  16 MiB arena in `.bss`, the command line from the PEB.
- **msvcrt's printf, not C99's**: `%ld` is 32 bits, `%I64d`/`%Id` exist, `%p` is
  sixteen upper-case digits, `%n` is refused, exponents have three digits, a tie
  rounds away from zero, infinities are `1.#INF`, and `_vsnprintf` returns -1 for
  a truncated result and writes no NUL then. Where it agrees with C99 it is held
  against glibc's `snprintf` (about 5,950 generated cases per seed, the seed
  printed; floating point is expanded exactly and rounded once, which is the
  only way to agree at every precision).
- **Absent services are answered as a runtime with nothing behind it would**:
  no files until W4 (`fopen` fails, `fgets` finds nothing), no environment
  block (`getenv` finds nothing), no locking until W5 (the heap, `strtok`'s
  position and the generator's seed are statics).
- **Measured**: `vibeos_msvcrt_tests` (3.2 million checks), a layout check
  against mingw-w64's headers at every build, `check-msvcrt-exports.py`
  (exports equal the `.def`, imports are `ntdll.dll` alone), and sabotage
  (`cases/win-msvcrt.txt`, 20 of 20 red).

It runs: every Windows program at boot and every Wine test loads it (below).

### Done: image sections and ntdll's loader (`326cbfc`, `46a98a4`, `042f560`)

`NtOpenFile` maps `\??\C:\` onto the VFS root; `NtCreateSection(SEC_IMAGE)` and
`NtMapViewOfSection` place an image in the DLL window at 64 KiB granularity and
relocate it, answering `STATUS_IMAGE_NOT_AT_BASE` when it moved. ntdll's loader
brings DLLs in with them, depth first, runs `DllMain`, and binds an import no DLL
exports to one of 2048 stubs that prints `unimplemented: <dll>.<function>` and
ends the process. `stubcall.exe` calls one (`Beep`, which kernel32 will never
have) and the gate asserts the line. Wine's executables have seventeen sections,
so the image layout holds 24.

### Done: mutants, semaphores, Sleep (`45c97bc`, `a21935f`)

On the events' wait: a mutant is its owner's to take again and to release, its
count the signal state Windows reports; a semaphore refuses a release past its
maximum. `NtDelayExecution` waits on the same clock. Abandonment has its state and
status; the exit path that sets it is W5's.

### Done: our kernel32 (`3923ba0`, `ca080c1`, `395ae7c`)

`user/win/kernel32` replaces the placeholder: the Win32 calls as Nt calls -
named objects under `\Sessions\<n>\BaseNamedObjects` (the PEB says session 1, as
Wine's does), the waits in milliseconds, Sleep, the tick count, `GetModuleHandle`,
`GetProcAddress` and `LoadLibrary` through ntdll's `Ldr*`, the process heap
(`RtlAllocateHeap` and its family, on the Rtl arena), the TEB's 64 TLS slots, the
environment, the standard handles, `WriteFile`. Every failure sets the last error
through `RtlNtStatusToDosError`, whose pairs are the ones Wine's `error.c` checks
on Windows. Exports are `kernel32.def`'s, checked against the built DLL.

**Written ahead of the kernel, on purpose.** Wine's tests for W2 reach files and
threads early - `ntdll:om` a named pipe in its third test, both sync tests a thread
in their second - and a stub ends the test there. So `CreateFile` and
`CreateThread` exist now over `NtCreateFile` (W4) and `NtCreateThreadEx` (W5),
which the kernel answers `STATUS_NOT_IMPLEMENTED` and counts: the test is told the
call failed and goes on, its failures named, as a Linux program was told ENOSYS.
This is the rule for the steps after W2 as well: a kernel32 function may be written
before its service, never instead of it.

### What the oracle says at the end of W2

`scripts/dev/winetest-run.sh build-gcc-Release ntdll:om ntdll:sync kernel32:sync`
(2026-10-17):

| Test | Wine on Linux | VibeOS | Where it ends |
|---|---|---|---|
| `ntdll:om` | 7,347 tests, 0 failures | at least 38 failures | `CreateWaitableTimerA` (a timer object: W5's) |
| `ntdll:sync` | 250 tests, 0 failures | at least 39 failures | a segmentation fault after the keyed-event tests (keyed events: W5's) |
| `kernel32:sync` | 354,502 tests, 0 failures | at least 1 failure | `TerminateThread` (W5) |

The counts are lower bounds: the gate keeps the last forty lines of each test,
and a test that ends at a stub prints no summary. None of them finishes, and
none can in W2: each reaches threads, timers or keyed
events, which are W5's. What W2 promised - handles, events, mutants, semaphores,
the waits - is what they now exercise, and the failures they report before the
end are the work list for W5 together with the functions the stubs name.
`ntdll:rtlstr` runs far past W2's scope and reports `RtlDuplicateUnicodeString`
wrong for flags 2 and 3, then calls an Rtl function our ntdll does not export.

**What these boots also showed.** One winetest boot wedged after the tests, in
the corpus shell, with `SWAP_IN_FAILED` on a garbage slot and two
`SWAP_DOUBLE_RELEASE`: the open swap defect M-070
([review_findings_tracker.md](../implementation_progress/review_findings_tracker.md)),
not something W2 wrote. The NT processes make it likelier: an image's `.bss` is
filled when it is mapped, so `ntdll_test.exe` and its DLLs cost about ten
megabytes each before a line runs, and reclaim swaps under that. Lazy `.bss` and a
heap that grows are W3's, and W3 is where the pressure goes away. Evidence in
`.boot-evidence/w2-winetest/`.

### Closing W2

- CI: two defects that every CI job saw and no local check did - the Rtl host
  tests did not build without mingw-w64 (`41c2188`), and `stubcall.exe` called a
  function kernel32 had meanwhile been given (`0646d09`). check.sh ran neither
  path: it now runs the whole of ctest (`2a63723`), and the stub's function is one
  kernel32 is never going to have.
- Sabotage: host cases red for mutants, semaphores, delay, the heap functions and
  the status mapping; `nt-loader-boot.txt` run again after kernel32 - the stub
  returning goes red as named, unbound DLL imports red on the crash they cause, a
  missing export bound to address 0 hangs the boot instead of naming the fault.
- Boots: `check.sh all` green; `boots.sh` 6 runs, 5 clean and one in which the
  gate died before booting - three gates on one `efi_root` each taking the
  previous winetest run's executables off, two of them the same file. Fixed in
  the gate, then 6 of 6 with those executables put back on purpose.

## W3 (2026-10-20): planned, in progress

Measured by `ntdll:virtual` (Wine on Linux: 1,779 tests, 205 todo, 0 failures)
and `kernel32:virtual` (Wine itself does not finish it: 679 failures printed, no
summary - so the target there is to run as far as Wine does and fail where it
fails). Twenty-three services are W3's in the registry; the ones the tests reach
first are NtAllocateVirtualMemory, NtFreeVirtualMemory, NtProtectVirtualMemory,
NtQueryVirtualMemory, and the data sections.

**The mechanism W3 needs is one the memory manager was designed for and never
built.** `VIBEOS_BACKING_ANON` is documented as "zero-filled on first touch",
and only the user stack is: mmap maps every page at once. Windows' model has
two states mmap never needed:

- *reserved*: address space set aside with no pages and no access - a touch is
  an access violation, and the range is not free for anybody else's mapping;
- *committed*: pages promised, made on first touch.

Both are region states, not page-table states. CLAUDE.md records why: an
earlier attempt to reserve without backing could not be told apart from a range
munmap had just freed. A region that says what it is can be. And Windows asks
per region for what it was allocated as - its allocation base and protection -
which NtQueryVirtualMemory reports, and which therefore has to be kept.

Order, each a commit with host tests and sabotage cases:

1. **Demand-zero and reserved regions.** A region flag for "reserved" and one
   for "committed, made on touch"; the page fault maps a zeroed page in a
   committed region as `hw_stack_grow` does for the stack - which becomes one
   case of it - and refuses a reserved one. Every path that walks entries (fork,
   munmap, mprotect, page-out, the range check, mincore) is asked what it does
   with a region that has no pages yet. The allocation base and protection ride
   on the region, and regions of different allocations never merge.
   *Done:* `vibeos_vma_state_t` and `vibeos_vma_insert_alloc` /
   `vibeos_vma_set_state` in `kernel/mm/vma.c`; `hw_stack_grow` serves a
   DEMAND region with the region's own protection and leaves a RESERVED one to
   fault, and `hw_user_range_why` accepts an untouched DEMAND page as it does
   an untouched stack page. munmap, page-out and mincore skip empty entries
   already; mprotect changes the region and the page takes it when made; fork
   copies the region whole. Nothing creates such a region before step 2, so
   the boot cannot see it yet - host tests and sabotage cover the region list.
2. **The four services**, with their Windows rules: reservations at 64 KiB
   granularity, commits at page granularity inside a reservation, MEM_RESET,
   MEM_TOP_DOWN, release only of a whole reservation by its base, PAGE_* to
   region protection and back (PAGE_GUARD and PAGE_NOCACHE recorded), and
   MemoryBasicInformation as Windows fills it. kernel32's VirtualAlloc family
   over them.
   *Done:* `kernel/abi/nt/memory.c`, on `kernel/abi/mem/umem.c` - placing,
   releasing and RLIMIT_AS lifted out of Linux's mm.c, which asks the same
   questions. A region keeps the protection as Windows spelled it
   (`user_prot`), so PAGE_EXECUTE and PAGE_GUARD read back as given. kernel32
   has VirtualAlloc, VirtualFree, VirtualProtect and VirtualQuery with their
   `Ex` forms. The fake kernel makes a committed page on first touch as the
   machine does. hello.exe exercises it at boot: a store into a page nobody
   touched, the kernel's store into another, a reservation committed a page
   at a time, protected, decommitted and released; sabotaging either half of
   the demand fault turns the boot red. Not enforced: PAGE_GUARD and
   PAGE_NOCACHE (recorded and reported), MEM_TOP_DOWN (placed as anything
   else), ZeroBits (refused: the arena is above 4 GiB), MEM_WRITE_WATCH and
   MEM_LARGE_PAGES (STATUS_NOT_SUPPORTED).
3. **A heap that grows.** RtlCreateHeap and the process heap on
   NtAllocateVirtualMemory instead of ntdll's 512 KiB `.bss` arena; msvcrt's
   2 MiB arena likewise. This is what makes an NT process cost what it uses.
   *Done:* the heap takes segments of at least a megabyte from
   NtAllocateVirtualMemory (`vibeos_rtl_pages`) when nothing on its free list
   fits, and never gives them back; a pointer is the heap's only inside a
   segment. msvcrt's malloc is RtlAllocateHeap on the process heap, as
   Microsoft's is, and its own arena is gone - its randomised soak now runs
   against ntdll's heap, with segments handed out apart, below the others and
   not at all. hello.exe mallocs three megabytes at boot and frees a malloc'd
   block with HeapFree (`nt_heap_did_not_grow`). HeapCreate - a heap other
   than the process's - is still to come.
4. **Lazy `.bss` for images.** The mapper fills an image's zero-initialised
   tail as a committed demand-zero range instead of allocating it whole -
   ntdll_test.exe and its DLLs each cost about ten megabytes before a line ran,
   and the reclaim that caused is where the winetest boots met M-070.
   *Done:* a page no byte of the file reaches (`vibeos_elf_page_all_zero`) is a
   region made on first touch rather than a frame cleared at exec - in the high
   window only, where NT images and the native programs live. The low window,
   where Linux programs link, lies over the kernel's identity map: a page
   nobody mapped there is present for ring 0, a program's first store to it is
   a protection fault, and the first boot that deferred it there killed every
   static Linux program on its first store to .bss. A page a base relocation
   reaches is built at exec as before (`vibeos_pe_page_relocated`). mprotect
   takes an untouched page and mlock makes one, as Linux faults it in; the
   boot counts the pages left (`pages_deferred`) and fails at zero.
5. **Data sections.** SEC_COMMIT sections backed by an unnamed tmpfs file (as
   SysV shared memory is), file-backed views, NtOpenSection, NtQuerySection,
   NtUnmapViewOfSection.
   *Done:* a section of memory alone is a file on /tmp nothing names, and its
   views map that file's own pages - one memory, however many views. A file's
   section is read-only or copy-on-write, because NtOpenFile opens for reading
   alone until W4, and its views are the process's own copies: a later change
   to the file does not show through them, which is the gap. A view is an
   allocation based at itself, bounded by the section's protection and the
   handle's rights, placed at 64 KiB; NtUnmapViewOfSection takes it whole from
   any address in it, as Wine's does, and NtFreeVirtualMemory refuses it.
   kernel32 has CreateFileMapping, OpenFileMapping, MapViewOfFile(Ex) and
   UnmapViewOfFile. hello.exe stores through one view and reads through
   another at boot (`sec_shared`). Not yet: SectionImageInformation, a view
   placed at an address for an image, unmapping an image's view, SEC_RESERVE
   as anything but SEC_COMMIT.
6. **Views from the handle's file (M-106's remainder).** The page cache learns
   files by mount and node, so a view is mapped from the description its handle
   holds and never goes back to the name.
7. **One address space for CLONE_VM processes (M-100).** The region list, its
   lock and the break become one counted object that a CLONE_VM child shares
   with its parent instead of copying; threads already share it through the
   process state.
8. **Kernel stacks with guard pages (M-108).** A region of their own in every
   address space's kernel half, an unmapped page below each stack, and #PF and
   #DF on IST stacks so that an overflow faults on a stack that exists and the
   panic names the task. The canary stays as a second line.
9. **The oracle.** `ntdll:virtual` and `kernel32:virtual` through
   `winetest-run.sh`, against Wine's answers; the work list they leave.

Steps 1 to 5 are the NT personality's; 6 to 8 are the reviews' findings that
need the same memory work, scheduled here on 2026-10-19.
