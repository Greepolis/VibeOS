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

## What W3 has added so far (2026-10-20)

- Regions with a state (`include/vibeos/vma.h`): present, committed and made
  on first touch, or reserved with no access; each carries its allocation's
  base and protection and the protection as Windows spelled it. The page
  fault makes a committed page (`hw_stack_grow` in `mm_bridge.c`), and the
  user range check accepts one nobody has touched yet.
- `kernel/abi/mem/umem.c`: placing, releasing and RLIMIT_AS for every
  personality, lifted out of Linux's mm.c.
- `kernel/abi/nt/memory.c`: NtAllocateVirtualMemory, NtFreeVirtualMemory,
  NtProtectVirtualMemory and NtQueryVirtualMemory; kernel32's VirtualAlloc,
  VirtualFree, VirtualProtect and VirtualQuery with their `Ex` forms. Host
  tests in `tests/kernel/nt_abi_tests.c` (`t_virtual_memory`); hello.exe uses
  them at boot. Gaps are listed in [../abi/windows.md](../abi/windows.md), W3
  step 2.

## Where the oracle stands

`ntdll:om`, `ntdll:sync` and `kernel32:sync` run and report their failures,
and each ends at something W5 provides (threads, timers, keyed events) - see
[../abi/windows.md](../abi/windows.md), "What the oracle says at the end of
W2".

## Next checkpoint

- W3: memory - the rest of it: a heap that grows, lazy `.bss`, data
  sections, and the reviews' M-106, M-100 and M-108 - measured by `ntdll:virtual` and `kernel32:virtual`. It also
  removes the memory pressure under which the winetest boots met M-070.
