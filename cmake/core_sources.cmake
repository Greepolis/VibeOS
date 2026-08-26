# Single source of truth for the two portable libraries.
#
# These lists used to be written twice: here, and again by hand inside
# scripts/run-tests.ps1 for its manual compiler fallback. The copies drifted
# three separate times (log.c, inet.c, tls_adapter.c), and each time the
# Windows job failed to link long after the real change had landed.
#
# The PowerShell fallback now parses this file instead of carrying its own
# copy, so adding a file here is enough. Keep the plain
# `set(<NAME> <paths>)` shape: that script reads it with a simple parser and
# does not evaluate CMake syntax.

set(VIBEOS_KERNEL_CORE_SOURCES
    kernel/core/kmain.c
    kernel/core/elf.c
    kernel/fs/fat_chain.c
    kernel/fs/blockcache.c
    kernel/fs/vfs.c
    kernel/fs/partition.c
    kernel/fs/ext2.c
    kernel/fs/iso9660.c
    kernel/fs/exfat.c
    kernel/fs/ntfs.c
    kernel/fs/journal.c
    kernel/mm/pmm.c
    kernel/net/inet.c
    kernel/net/net_policy.c
    kernel/mm/vm.c
    kernel/object/handle_table.c
    kernel/proc/process.c
    kernel/sched/scheduler.c
    kernel/ipc/event.c
    kernel/ipc/channel.c
    kernel/ipc/handle_transfer.c
    kernel/ipc/waitset.c
    kernel/core/interrupts.c
    kernel/core/log.c
    kernel/core/policy.c
    kernel/core/syscall_policy.c
    kernel/core/security.c
    kernel/core/syscall.c
    kernel/time/timer.c
    kernel/arch/x86_64/trap.c
    kernel/arch/x86_64/idt.c
    kernel/arch/x86_64/boot_stub.c
    kernel/arch/x86_64/serial.c
)

set(VIBEOS_USER_CORE_SOURCES
    user/init/init_system.c
    user/servicemgr/service_manager.c
    user/servicemgr/service_ipc.c
    user/devmgr/device_manager.c
    user/devmgr/driver_host.c
    user/drivers/driver_framework.c
    user/fs/vfs_service.c
    user/fs/vfs_ops.c
    user/net/network_service.c
    user/net/socket.c
    user/compat/compat_runtime.c
    user/compat/linux/linux_compat.c
    user/compat/windows/windows_compat.c
    user/compat/macos/macos_compat.c
    user/lib/user_api.c
    boot/bootloader_stub.c
)

# The TLS adapter is built into its own target so hosted crypto can never be
# linked into the freestanding kernel image. It compiles with or without the
# audited dependency, reporting TLS as unavailable when it is absent.
set(VIBEOS_TLS_SOURCES
    user/net/tls_adapter.c
)
