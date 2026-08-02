# User Space Interface Progress

Status: In Progress (ring-3 shell runtime verified)
Last review: 2026-08-02

## Implemented
- User API syscall bridge in `user/lib/user_api.c`.
- Service-facing interface contracts in `include/vibeos/user_api.h`.
- Compatibility runtime scaffolding under `user/compat/*`.
- Baseline integration with process/security/syscall surfaces.
- Versioned user API contract and capability matrix exposure (`vibeos_user_api_contract`, `vibeos_user_api_capabilities`).
- Real ELF programs now run in ring 3 through the native syscall entry path. The runtime loads programs from the FAT boot volume and supports fork, `execve`, `waitpid` and exit.
- Programs start on a real System V startup stack, not a bare one: `vibeos_elf_build_stack` (`kernel/core/elf.c`) lays out `argc`, `argv`, `envp` and the auxiliary vector, and `user/prog/crt0.S` is a genuine assembly `_start` that converts that state into a C call. `AT_PHDR`, `AT_PHNUM`, `AT_PHENT`, `AT_ENTRY`, `AT_PAGESZ` and `AT_RANDOM` are all populated, and the user linker script now maps the ELF headers inside the first `PT_LOAD` so `AT_PHDR` points at a program header table that really exists in the address space. A ring-3 program verifies this on every boot (`auxv ok` in the serial log).
- `/BIN/SH` supplies the current interactive shell baseline with keyboard/serial input, line editing and file commands; framebuffer output is available in the runtime console path.

## Pending
- Running an unmodified static Linux binary end to end. The startup stack it needs is in place; what remains is the set of syscalls a real static musl or busybox actually calls on startup (`set_tid_address`, `rt_sigprocmask`, `ioctl(TCGETS)`, `writev`, `readlinkat`, `clock_gettime`, `mmap` with real flags). Stubbing all ~350 Linux syscalls is not the goal and would be worse than nothing; the goal is the ones a real binary reaches.
- Stable libc/runtime contract for richer userland.
- Expanded compatibility ABI translation depth (Linux/Windows/macOS targets).
- Tooling and diagnostics for userland API evolution.
- A stable libc/runtime boundary, executable format policy and user-space memory/error isolation.

## Next checkpoint
- Replace the fixed boot program selection with a supervised ring-3 init process and a stable userland runtime contract.
