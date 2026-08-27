# macOS Compatibility Progress

Status: Not started (a two-entry translation table, no runtime)
Last review: 2026-08-27

## What exists

`user/compat/macos/macos_compat.c`, twenty-four lines, the same shape as the
Windows one: a syscall-number translation function knowing two entries, with
placeholder targets, incrementing a denied counter for everything else. Nothing
calls it at runtime.

Tracked here so that a file existing does not read as work having happened.

## What "started" would require

- **Mach-O loading**, and with it fat binaries and two live architectures.
- **The BSD syscall layer *and* Mach traps** - ports, tasks, threads and IPC are
  not a translation of anything in this kernel; they are a different model of
  what an operating system is.
- **dyld and the shared cache.** macOS binaries are dynamically linked as a rule,
  and the system libraries they need are not distributable, which is the
  constraint that has no technical answer.

## Pending

- Everything above. The docs have long said macOS support is deferred "due to
  complexity and ecosystem constraints"; the second half of that phrase is the
  operative one, and it is a licensing problem rather than an engineering
  schedule.

## Next checkpoint

- None. This area is deliberately dormant and should stay that way until the
  native system and the Linux layer are considerably further along.
