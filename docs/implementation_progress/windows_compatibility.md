# Windows Compatibility Progress

Status: W1 done - a PE program runs on our ntdll
Last review: 2026-10-14

## The plan (2026-10-13)

[../abi/windows.md](../abi/windows.md). The decision this file used to say came
first is made: an NT personality in the kernel, beside Linux's and over the same
services interface, with our own `ntdll.dll` above it (written from scratch, built
with mingw-w64), handles and objects in a portable object manager in the kernel,
and Wine's own conformance tests as the oracle. Steps W0-W7: the oracle, the PE
loader, objects and waits, memory, files, threads and exceptions, processes, then
`kernel32` as the tests ask.

## What exists (W1, 2026-10-14)

- `kernel/core/pe.c`: the PE32+ parser, its answer in the ELF mapper's segment
  form; `tests/kernel/pe_tests.c` against an image mingw-w64 linked.
- `kernel/abi/nt_services.def`, `kernel/abi/abi_nt.c`, `kernel/abi/nt/`: every
  NT service, the registry, the dispatcher, and NtTerminateProcess,
  NtWriteFile, NtQueryInformationProcess, NtQuerySystemInformation,
  NtQuerySystemTime and NtQueryPerformanceCounter; `tests/kernel/nt_abi_tests.c`.
- `kernel/arch/x86_64/pe_load.c` and the `swapgs` entry and exit paths in
  `isr.S`: exec of a PE image, and a `%gs` base per task.
- `user/win/ntdll` and `user/win/hello`: our ntdll and the first Windows
  program, staged at `/win` on the boot volume.

The old placeholder - `user/compat/windows/windows_compat.c`, a function that
mapped two of a Windows build's syscall numbers onto nothing - is removed now
that a loader exists, as this file said it would be.

## Pending

- W0 is done: the 58 tests of `ntdll` and `kernel32` built by Wine's own build
  system (`scripts/dev/winetest-build.sh`), run under Wine 9.0 on the host
  (`winetest-run-host.sh`), the answers in `tests/corpus/winetest-wine9.txt`;
  every NT service listed with its step in
  [../abi/nt-services.md](../abi/nt-services.md).
- W1 is done: see [../abi/windows.md](../abi/windows.md), "W1".
- W2 has begun: our own `msvcrt.dll` (`user/win/msvcrt`) is built and tested -
  see [../abi/windows.md](../abi/windows.md), "W2: msvcrt". Nothing loads it
  yet; the loader that maps DLLs from disk is the next piece.
- W2 onwards: as the plan lists them.

## Next checkpoint

- W2: base relocation, stub `user32` and `advapi32`, then the object manager -
  the first of Wine's tests (`ntdll:om`, `ntdll:sync`, `kernel32:sync`) run.
