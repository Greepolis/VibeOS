# Windows Compatibility Progress

Status: Not started (a two-entry translation table, no runtime)
Last review: 2026-08-27

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

- Everything above. No part of it is in progress.
- A decision that comes first: whether Windows compatibility is a subsystem
  runtime in user space (the architecture's stated intent) or is deferred until
  the native system is further along. The roadmap has M15 as a *console*
  compatibility milestone, which is a far narrower promise than this file's
  title, and that narrowing is deliberate.

## Next checkpoint

- None scheduled. M15 (Windows console compatibility) is the first commitment
  that touches this area, and it is not started.
