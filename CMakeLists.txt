cmake_minimum_required(VERSION 3.21)
project(VibeOS C)

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
    # Kernel ELF image (primary artifact for emulator/hardware loading)
    add_custom_target(vibeos_kernel_elf
        COMMAND ${CMAKE_COMMAND} -E copy
            $<TARGET_FILE:vibeos_kernel_core>
            ${VIBEOS_ARTIFACTS_DIR}/vibeos_kernel.elf
        DEPENDS vibeos_kernel_core
        COMMENT "Copying kernel ELF image to artifacts/"
    )
    
    # Boot image stub (for M1: simple placeholder; will evolve to bootable disk image)
    add_custom_target(vibeos_boot_image
        COMMAND ${CMAKE_COMMAND} -E echo "Creating boot image stub..."
        COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/create_boot_image.cmake
        DEPENDS vibeos_kernel_elf
        COMMENT "Generating bootable kernel image"
    )
    
    # Consolidated target: build all boot artifacts
    add_custom_target(vibeos_image ALL DEPENDS vibeos_boot_image vibeos_kernel_elf)
endif()

# Export toolchain information for scripts
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/vibeos_toolchain_info.cmake.in
    ${VIBEOS_ARTIFACTS_DIR}/vibeos_toolchain_info.cmake
    @ONLY
)
