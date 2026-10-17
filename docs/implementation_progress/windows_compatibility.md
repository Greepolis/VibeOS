# Windows Compatibility Progress

Status: W2 done - Wine's own tests run on our ntdll, msvcrt and kernel32
Last review: 2026-10-17

## The plan (2026-10-13)

[../abi/windows.md](../abi/windows.md). The decision this file used to say came
first is made: an NT personality in the kernel, beside Linux's and over the same
services interface, with our own `ntdll.dll` above it (written from scratch, built
with mingw-w64), handles and objects in a portable object manager in the kernel,
and Wine's own conformance tests as the oracle. Steps W0-W7: the oracle, the PE
loader, objects and waits, memory, files, threads and exceptions, processes, then
`kernel32` as the tests ask.

## What exists (W2, 2026-10-17)

- `kernel/core/pe.c`: the PE32+ parser and relocation, its answer in the ELF
  mapper's segment form; `tests/kernel/pe_tests.c` against an image mingw-w64
  linked.
- `kernel/abi/nt_services.def`, `kernel/abi/abi_nt.c`, `kernel/abi/nt/`: every
  NT service, the registry and the dispatcher; the object manager (handles are
  descriptors with access masks, a flat namespace of whole paths), events,
  mutants, semaphores, both waits, `NtDelayExecution`, `NtOpenFile` and image
  sections; `tests/kernel/nt_abi_tests.c`.
- `kernel/arch/x86_64/pe_load.c` and the `swapgs` paths in `isr.S`: exec of a PE
  image, moved when it cannot be placed where it asks, DLL views, and a `%gs`
  base per task.
- `user/win/ntdll`: the system call stubs, the loader (DLLs from `C:\win`,
  `DllMain`, named stubs for what no DLL exports, `Ldr*` for kernel32), and the
  Rtl runtime, host-tested in `tests/win/ntrtl_tests.c`.
- `user/win/msvcrt` and `user/win/kernel32`: the C runtime Wine's tests import,
  and kernel32 as far as the tests reach before W3. `user32` and `advapi32` are
  still placeholders that export nothing.
- `scripts/dev/winetest-run.sh`: Wine's tests on VibeOS, against Wine's answers.

## Where the oracle stands

`ntdll:om`, `ntdll:sync` and `kernel32:sync` run and report their failures,
and each ends at something W5 provides (threads, timers, keyed events) - see
[../abi/windows.md](../abi/windows.md), "What the oracle says at the end of
W2".

## Next checkpoint

- W3: memory - `NtAllocateVirtualMemory` and its family, lazy `.bss`, a heap
  that grows - measured by `ntdll:virtual` and `kernel32:virtual`. It also
  removes the memory pressure under which the winetest boots met M-070.
