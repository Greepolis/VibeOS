# Windows Compatibility Progress

Status: W0 done - planned, the oracle answered; nothing runs on VibeOS yet
Last review: 2026-10-13

## The plan (2026-10-13)

[../abi/windows.md](../abi/windows.md). The decision this file used to say came
first is made: an NT personality in the kernel, beside Linux's and over the same
services interface, with our own `ntdll.dll` above it (written from scratch, built
with mingw-w64), handles and objects in a portable object manager in the kernel,
and Wine's own conformance tests as the oracle. Steps W0-W7: the oracle, the PE
loader, objects and waits, memory, files, threads and exceptions, processes, then
`kernel32` as the tests ask.

The two-entry table below is not part of it and goes when W1's loader lands: its
syscall numbers are a Windows build's, which the plan does not use.

## What exists

`user/compat/windows/windows_compat.c`, twenty-four lines. It is a function
that maps a foreign syscall number to a native one, and it knows two of them:

| Foreign | Mapped to |
| --- | --- |
| `0x26` NtOpenFile | handle-alloc placeholder |
| `0xC0` NtCreateUserProcess | process-spawn placeholder |

Anything else increments a denied counter and returns an error. The targets are
placeholders by their own comments, not implementations. Nothing calls this at
runtime: the kernel enables only the Linux target in the compatibility runtime
at boot.

This file is tracked here so it stops reading as progress. It is a shape that
records an intention, and the honest status of Windows compatibility is that it
has not been started.

## What "started" would require

Listed because the gap between the table above and a running program is easy to
underestimate:

- **PE/COFF loading.** A different executable format from ELF, with imports
  resolved by name through an import address table.
- **The NT syscall surface**, which is undocumented, versioned, and not the API
  applications are written against. Practical Windows compatibility targets
  Win32 - `kernel32`, `user32`, `ntdll` - which means implementing libraries,
  not a syscall table.
- **A registry, a Win32 object namespace, and handle semantics** that do not map
  onto file descriptors.
- **A path model** with drive letters, backslashes and case-insensitive but
  case-preserving names.

Wine is the reference for the size of this, and it is not a syscall table.

## Pending

- W0 is done: the 58 tests of `ntdll` and `kernel32` built by Wine's own build
  system (`scripts/dev/winetest-build.sh`), run under Wine 9.0 on the host
  (`winetest-run-host.sh`), the answers in `tests/corpus/winetest-wine9.txt`;
  every NT service listed with its step in
  [../abi/nt-services.md](../abi/nt-services.md).
- W1 onwards: as the plan lists them.

## Next checkpoint

- W1: a PE program of ours prints "hello" through our own `ntdll`.
