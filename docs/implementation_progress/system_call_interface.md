# System Call Interface Progress

Status: In Progress (real ring-3 syscall entry verified in QEMU)
Last review: 2026-07-22

## Implemented
- **Real on-metal ring-3 -> ring-0 syscall entry** (`kernel/arch/x86_64/arch_hw.c` + `isr.S`), two paths into one Linux-ABI dispatcher (`vibeos_x86_64_linux_syscall`):
  - **Native `syscall`/`sysret`** (the real Linux x86-64 fast path): EFER.SCE enabled, STAR/LSTAR/SFMASK programmed, and an LSTAR trampoline that switches to a kernel stack, marshals the Linux ABI (nr in rax; args in rdi/rsi/rdx) into the C ABI, dispatches, and `sysretq`s back to ring 3.
  - **`int 0x80` gate** (DPL 3) as a legacy/bring-up path into the same dispatcher.
  - User GDT segments ordered data-then-code (for SYSRET) + a TSS with RSP0.
  - Linux syscall translation so far: `write`(1) -> serial, `getpid`(39), `exit`(60)/`exit_group`(231) -> return to kernel; unknown -> `-ENOSYS`.
  - Verified in QEMU (GCC and Clang): a **real user ELF** is loaded (see `kernel/arch/x86_64/elf_load.c`), run in ring 3, and prints via a native `syscall`, then `exit` returns cleanly to the kernel. This hardware front end will route into the portable dispatcher and the Linux personality (`user/compat/linux`) next.
- Central syscall dispatcher in `kernel/core/syscall.c` (portable model).
- ABI helper mappings in `include/vibeos/syscall_abi.h`.
- Rights/policy lookup layer in `kernel/core/syscall_policy.c`.
- Process/thread lifecycle syscall family.
- Scheduler observability and runtime-control syscall family.
- Waitset ownership/stats/wake-policy syscall paths.
- Security/audit/policy control syscalls with kernel-vs-user authorization checks.
- ABI version/compatibility syscalls (`VIBEOS_SYSCALL_ABI_VERSION_GET`, `VIBEOS_SYSCALL_ABI_COMPAT_CHECK`) with compatibility helpers in `syscall_abi.h`.

## Pending
- Stronger multi-tenant authorization model beyond relation-based checks.
- Syscall ABI versioning strategy with compatibility negotiation.
- Per-syscall latency telemetry and regression thresholds.

## Next checkpoint
- Extend ABI negotiation from major-version compatibility to per-feature negotiation for optional syscall families.
