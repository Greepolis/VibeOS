cmake_minimum_required(VERSION 3.21)
project(VibeOS C ASM)

# Enable ASM language for assembly sources
enable_language(ASM)
set(CMAKE_ASM_COMPILE_OBJECT "<CMAKE_C_COMPILER> <DEFINES> <INCLUDES> <FLAGS> -o <OBJECT> -c <SOURCE>")

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Build options
option(VIBEOS_BUILD_TESTS "Build VibeOS host-side tests" ON)
option(VIBEOS_BUILD_KERNEL_IMAGE "Build bootable kernel image for emulator/hardware" ON)
option(VIBEOS_KERNEL_ONLY "Build kernel only (no user-space)" OFF)

# Artifact directories
set(VIBEOS_ARTIFACTS_DIR "${CMAKE_BINARY_DIR}/artifacts" CACHE PATH "Artifacts output directory")
file(MAKE_DIRECTORY "${VIBEOS_ARTIFACTS_DIR}")

# Compiler and target configuration
set(VIBEOS_TARGET_ARCH "x86_64" CACHE STRING "Target architecture")
set(VIBEOS_BUILD_TYPE "debug" CACHE STRING "Build type: debug, release, hardened")

add_library(vibeos_kernel_core STATIC
    kernel/core/kmain.c
    kernel/mm/pmm.c
    kernel/mm/vm.c
    kernel/object/handle_table.c
    kernel/proc/process.c
    kernel/sched/scheduler.c
    kernel/ipc/event.c
    kernel/ipc/channel.c
    kernel/ipc/handle_transfer.c
    kernel/ipc/waitset.c
    kernel/core/interrupts.c
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

target_include_directories(vibeos_kernel_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_options(vibeos_kernel_core PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

add_library(vibeos_user_core STATIC
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

target_include_directories(vibeos_user_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_options(vibeos_user_core PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

if(VIBEOS_BUILD_TESTS)
    add_executable(vibeos_kernel_tests
        tests/kernel/kernel_tests.c
    )
    target_link_libraries(vibeos_kernel_tests PRIVATE vibeos_kernel_core vibeos_user_core)
    target_include_directories(vibeos_kernel_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
endif()

# Image and boot artifact generation
if(VIBEOS_BUILD_KERNEL_IMAGE)
    # Kernel executable ELF (bootable by QEMU/hardware bootloader)
    # Links entry.s + kernel_core + user_core into a single executable
    add_executable(vibeos_kernel
        kernel/arch/x86_64/entry.s
    )
    
    target_link_libraries(vibeos_kernel PRIVATE vibeos_kernel_core vibeos_user_core)
    
    # Linker settings for kernel executable
    target_link_options(vibeos_kernel PRIVATE
        -no-pie             # Disable Position Independent Executable
        -nostdlib           # No standard library
        -Wl,--entry=_start  # Entry point is _start from entry.s
    )
    
    # Copy kernel ELF to artifacts directory
    add_custom_target(vibeos_kernel_elf
        COMMAND ${CMAKE_COMMAND} -E copy
            $<TARGET_FILE:vibeos_kernel>
            ${VIBEOS_ARTIFACTS_DIR}/vibeos_kernel.elf
        DEPENDS vibeos_kernel
        COMMENT "Copying kernel ELF image to artifacts/"
    )
    
    # Boot image stub (for M2: uses kernel ELF; will evolve to bootable disk image for M3+)
    add_custom_target(vibeos_boot_image
        COMMAND ${CMAKE_COMMAND} -E echo "Creating boot image stub..."
        COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/create_boot_image.cmake
        DEPENDS vibeos_kernel_elf
        COMMENT "Generating bootable kernel image"
    )
    
    # UEFI Bootloader (M2 bring-up)
    add_executable(vibeos_bootloader_uefi
        boot/uefi_main.c
        boot/uefi_protocol.c
        boot/uefi_serial.c
        boot/uefi_memory.c
        boot/uefi_firmware.c
        boot/bootloader_stub.c
    )
    
    target_include_directories(vibeos_bootloader_uefi PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/boot
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    
    # UEFI freestanding compilation flags
    target_compile_options(vibeos_bootloader_uefi PRIVATE
        -ffreestanding      # No hosted libs
        -fshort-wchar       # UEFI uses 16-bit wchar
        -Wall
        -Wextra
        -Wpedantic
    )
    
    # UEFI linker options
    # NOTE: --subsystem=efi-app requires cross-compiler (LLVM/Clang on Linux)
    # MinGW GCC on Windows doesn't support UEFI subsystem directly
    # Phase 1: Link as generic executable, full UEFI support in Phase 2+ with WSL/Linux toolchain
    target_link_options(vibeos_bootloader_uefi PRIVATE
        -nostdlib                       # No standard library
        -Wl,--entry=efi_main           # Entry point (will be PE entry when using UEFI toolchain)
    )
    
    # Copy bootloader to artifacts directory
    add_custom_target(vibeos_bootloader_efi
        COMMAND ${CMAKE_COMMAND} -E copy
            $<TARGET_FILE:vibeos_bootloader_uefi>
            ${VIBEOS_ARTIFACTS_DIR}/bootloader.efi
        DEPENDS vibeos_bootloader_uefi
        COMMENT "Copying UEFI bootloader to artifacts/"
    )
    
    # Consolidated target: build all boot artifacts
    add_custom_target(vibeos_image ALL DEPENDS vibeos_boot_image vibeos_kernel_elf vibeos_bootloader_efi)
endif()

# Export toolchain information for scripts
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/vibeos_toolchain_info.cmake.in
    ${VIBEOS_ARTIFACTS_DIR}/vibeos_toolchain_info.cmake
    @ONLY
)
