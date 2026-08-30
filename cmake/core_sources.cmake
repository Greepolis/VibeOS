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
    kernel/mm/stats.c
    kernel/mm/frame.c
    kernel/mm/usage.c
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
    kernel/fs/storage.c
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
    user/init/userland_abi.c
    user/servicemgr/service_manager.c
    user/servicemgr/supervisor.c
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
    boot/bootloader_core.c
)

# The TLS adapter is built into its own target so hosted crypto can never be
# linked into the freestanding kernel image. It compiles with or without the
# audited dependency, reporting TLS as unavailable when it is absent.
set(VIBEOS_TLS_SOURCES
    user/net/tls_adapter.c
)

# The freestanding half of the kernel image: hardware bring-up that host tests
# never link. Kept here for the same reason as the lists above - it was written
# out by hand in CMakeLists.txt, which made it a second place to remember, and
# the two lists do not fail loudly when they disagree.
set(VIBEOS_ARCH_X86_64_SOURCES
    kernel/arch/x86_64/arch_hw.c
    kernel/arch/x86_64/apic.c
    kernel/arch/x86_64/elf_load.c
    kernel/arch/x86_64/blk.c
    kernel/arch/x86_64/ahci.c
    kernel/arch/x86_64/virtio_blk.c
    kernel/arch/x86_64/virtio_net.c
    kernel/arch/x86_64/fat.c
    kernel/arch/x86_64/fat_vfs.c
    kernel/arch/x86_64/keyboard.c
    kernel/arch/x86_64/fb.c
    kernel/arch/x86_64/mouse.c
    kernel/arch/x86_64/gui.c
)

# The receive path, as the fuzzer must link it. net_policy.c belongs here
# because ip_input calls into it for every packet: leaving it out is what broke
# the CI fuzz build while a local check.sh stayed green, and stubbing it would
# have fuzzed a shape of the code that does not ship.
set(VIBEOS_FUZZ_INET_SOURCES
    tests/fuzz/fuzz_inet_input.c
    kernel/net/inet.c
    kernel/net/net_policy.c
)
