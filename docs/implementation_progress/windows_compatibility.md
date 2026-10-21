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

## What W3 added (2026-10-20 to 2026-10-21)

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
- `user/win/ntdll/rtl/heap.c`: the process heap grows a segment at a time on
  NtAllocateVirtualMemory instead of living in a 512 KiB arena; msvcrt's
  malloc is on it. Host-tested with a randomised soak in
  `tests/win/ntrtl_tests.c`; hello.exe makes it grow at boot.
- `kernel/arch/x86_64/mm_bridge.c` (`hw_map_elf_image`): an image's zero-fill
  `.bss` in the high window is left to the page fault; the boot counts the
  pages (`pages_deferred`).
- `kernel/abi/nt/section.c`: data sections - of memory alone, shared by their
  views, or of a file, copied - NtOpenSection, NtQuerySection and
  NtUnmapViewOfSection; kernel32's file-mapping functions over them.

- The reviews' memory findings: an image section read by its description's
  node (M-106), one address space shared as an object between CLONE_VM
  processes (`vibeos_mm_t`, M-100), and kernel stacks with guard pages and the
  double fault on an IST stack (M-108).

## Where the oracle stands

`ntdll:om`, `ntdll:sync` and `kernel32:sync` run and report their failures,
and each ends at something W5 provides (threads, timers, keyed events) - see
[../abi/windows.md](../abi/windows.md), "What the oracle says at the end of
W2". `kernel32:virtual` runs without a failure until CreateProcessA (W6);
`ntdll:virtual` passes `test_NtAllocateVirtualMemory` and stops at the
placeholders of NtAllocateVirtualMemoryEx (W3 step 9).

## Next checkpoint

- W4: files - NtCreateFile, reading and writing, directories - measured by
  `ntdll:file`, `ntdll:directory` and `kernel32:file`. Left from W3:
  NtAllocateVirtualMemoryEx with its placeholders, and ZeroBits.
