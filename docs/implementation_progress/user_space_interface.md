# User Space Interface Progress

Status: In Progress (unmodified static Linux binaries verified from ring 3)
Last review: 2026-08-25

## Implemented
- User API syscall bridge in `user/lib/user_api.c`.
- Service-facing interface contracts in `include/vibeos/user_api.h`.
- Compatibility runtime scaffolding under `user/compat/*`.
- Baseline integration with process/security/syscall surfaces.
- Versioned user API contract and capability matrix exposure (`vibeos_user_api_contract`, `vibeos_user_api_capabilities`).
- Real ELF programs now run in ring 3 through the native syscall entry path. The runtime loads programs from the FAT boot volume and supports fork, `execve`, `waitpid` and exit.
- Programs start on a real System V startup stack, not a bare one: `vibeos_elf_build_stack` (`kernel/core/elf.c`) lays out `argc`, `argv`, `envp` and the auxiliary vector, and `user/prog/crt0.S` is a genuine assembly `_start` that converts that state into a C call. `AT_PHDR`, `AT_PHNUM`, `AT_PHENT`, `AT_ENTRY`, `AT_PAGESZ` and `AT_RANDOM` are all populated, and the user linker script now maps the ELF headers inside the first `PT_LOAD` so `AT_PHDR` points at a program header table that really exists in the address space. A ring-3 program verifies this on every boot (`auxv ok` in the serial log).
- Every user program now enters through a real assembly `_start` (`user/prog/crt0.S`) and is written as an ordinary `vibeos_main(argc, argv, envp)`. This is not cosmetic: at entry `rsp` is aligned like a call site rather than a function frame, and the arguments are on the stack rather than in registers, so a C function as the entry point either misreads them or faults on the first aligned SSE store.
- `/BIN/SH` supplies the current interactive shell baseline with keyboard/serial input, line editing and file commands; framebuffer output is available in the runtime console path.
- **Unmodified static Linux binaries run end to end.** A static musl program and a static BusyBox - neither built by this project - load, reach `main`, and do real work. BusyBox dispatches on `argv[0]` (which is not the path: the shell supplies the name, the path only opens the file), runs `echo`, `cat` and `ls`, and its shell parses scripts, searches `PATH`, forks and execs. Gated: `busybox_did_not_run`, `busybox_applet_dispatch_failed`, `busybox_file_operations_failed`, `interactive_shell_did_not_run`, `shell_script_did_not_run`, `unmodified_linux_binary_did_not_run` and `linux_binary_got_wrong_argv` all fail the boot.
- **Pipelines run in that shell.** `pipe2`, `dup`, `dup2` and descriptor inheritance across `fork` are enough for `ls /EFI/BOOT | wc -l`; the boot self-test runs exactly that and the gate fails on `pipeline_did_not_complete`. Writing into a pipe with no reader raises `SIGPIPE`, which is what stops a pipeline filling memory after its reader has gone.
- **A signal reaches a real handler.** A program built against a real libc installs handlers, blocks and unblocks signals, ignores one and lets another take its default action; the return path goes through the libc's own trampoline and `rt_sigreturn`. Gated on `signal_delivery_broken`.
- `Ctrl-C` on the console raises `SIGINT` against the newest live user task. The signal itself is delivered like any other; picking the recipient by newest pid is the stand-in for process groups, which do not exist yet. Not covered by a gate.

## Pending
- Process groups and a controlling terminal, so `Ctrl-C` targets a job rather than the newest pid, and so `ioctl(TCGETS)` can become something other than `ENOTTY`.
- A test that exercises the `Ctrl-C` to `SIGINT` path, which is currently demonstrated rather than gated.
- Libraries with dependencies of their own. Dynamic executables load - the kernel places an `ET_DYN` image at a bias it chooses, maps the `PT_INTERP` interpreter alongside it and enters the interpreter - but one program and one interpreter is what has been shown, not a shared object that pulls in another.
- Stable libc/runtime contract for richer userland.
- Expanded compatibility ABI translation depth (Linux/Windows/macOS targets).
- Tooling and diagnostics for userland API evolution.
- A stable libc/runtime boundary, executable format policy and user-space memory/error isolation.

## Next checkpoint
- Replace the fixed boot program selection with a supervised ring-3 init process and a stable userland runtime contract.
