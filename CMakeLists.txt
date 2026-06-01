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

target_include_directories(vibeos_kernel_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_options(vibeos_kernel_core PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -ffreestanding
    -fno-stack-protector
    -mno-red-zone
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
    enable_testing()

    add_executable(vibeos_kernel_tests
        tests/kernel/kernel_tests.c
    )
    target_link_libraries(vibeos_kernel_tests PRIVATE vibeos_kernel_core vibeos_user_core)
    target_include_directories(vibeos_kernel_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    add_test(NAME vibeos-kernel-host COMMAND vibeos_kernel_tests)

    add_executable(vibeos_bootloader_tests
        tests/bootloader/uefi_bootloader_tests.c
        boot/bootloader_stub.c
        boot/uefi_file_io.c
        boot/uefi_memory.c
        boot/uefi_boot_info.c
        boot/uefi_boot_handoff.c
        boot/uefi_firmware.c
    )
    target_include_directories(vibeos_bootloader_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/boot
    )
    target_compile_options(vibeos_bootloader_tests PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )
    add_test(NAME vibeos-bootloader-host COMMAND vibeos_bootloader_tests)
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
        -Wl,-T,${CMAKE_CURRENT_SOURCE_DIR}/kernel/arch/x86_64/kernel.ld
    )
    set_target_properties(vibeos_kernel PROPERTIES
        LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/kernel/arch/x86_64/kernel.ld"
    )
    
    # Copy kernel ELF to artifacts directory
    add_custom_target(vibeos_kernel_elf
        COMMAND ${CMAKE_COMMAND} -E copy
            $<TARGET_FILE:vibeos_kernel>
            ${VIBEOS_ARTIFACTS_DIR}/vibeos_kernel.elf
        DEPENDS vibeos_kernel
        COMMENT "Copying kernel ELF image to artifacts/"
    )
    
    # UEFI Bootloader (M2 bring-up)
    add_executable(vibeos_bootloader_uefi
        boot/uefi_main.c
        boot/uefi_protocol.c
        boot/uefi_file_io.c
        boot/uefi_serial.c
        boot/uefi_memory.c
        boot/uefi_firmware.c
        boot/uefi_pe_loader.c
        boot/uefi_boot_info.c
        boot/uefi_boot_handoff.c
        boot/freestanding_runtime.c
        boot/bootloader_stub.c
    )
    
    target_include_directories(vibeos_bootloader_uefi PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/boot
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    # UEFI freestanding compilation flags
    target_compile_options(vibeos_bootloader_uefi PRIVATE
        -ffreestanding      # No hosted libs
        -fno-stack-protector
        -fshort-wchar       # UEFI uses 16-bit wchar
        -fPIC               # Relocatable image for UEFI loader
        -fno-plt
        -mno-red-zone
        -fno-strict-aliasing # UEFI compatibility
        -g0                 # Strip debug symbols (not applicable to freestanding bootloader)
        -Wall
        -Wextra
        -Wpedantic
    )
    
    # UEFI linker options for the intermediate image.
    # Final BOOTX64.EFI is generated via objcopy conversion+validation step below.
    # Note: GNU ld (Linux) doesn't support --subsystem; objcopy handles PE subsystem during conversion.
    # lld-link (MinGW) requires it; MINGW toolchain uses lld or compatible linker.
    if(MINGW)
        # Keep freestanding linkage on MinGW and bypass CRT startup requirements.
        target_link_options(vibeos_bootloader_uefi PRIVATE
            -nostdlib
            -Wl,--entry=efi_main
            -Wl,--subsystem,10
        )
    else()
        # Linux/WSL: build a freestanding shared ELF with relocations for objcopy -> EFI.
        target_link_options(vibeos_bootloader_uefi PRIVATE
            -nostdlib
            -Wl,--entry=efi_main
            -Wl,-shared
            -Wl,-Bsymbolic
            -Wl,-znocombreloc
            -Wl,--emit-relocs
        )
    endif()

    if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
        target_link_libraries(vibeos_bootloader_uefi PRIVATE gcc)
    elseif(CMAKE_C_COMPILER_ID STREQUAL "Clang")
        # Try to locate compiler-rt builtins for Clang.
        # These provide low-level compiler helper routines such as:
        #   __muldi3, __udivdi3, __ashldi3, etc.
        find_library(CLANG_RT_BUILTINS
            NAMES
                clang_rt.builtins-x86_64
                clang_rt.builtins
        )

        # Link compiler-rt only if available.
        if(CLANG_RT_BUILTINS)
            target_link_libraries(vibeos_bootloader_uefi PRIVATE
                ${CLANG_RT_BUILTINS}
            )
        endif()

        # Many Clang toolchains inject compiler builtins automatically,
        # even with -nostdlib. If unresolved builtin symbols appear,
        # install compiler-rt or provide the runtime library manually.
    endif()

    find_program(VIBEOS_OBJCOPY_TOOL
        NAMES llvm-objcopy objcopy x86_64-w64-mingw32-objcopy
    )
    if(NOT VIBEOS_OBJCOPY_TOOL)
        message(FATAL_ERROR "objcopy-compatible tool not found (required for EFI bootloader conversion)")
    endif()
    find_package(Python3 COMPONENTS Interpreter REQUIRED)

    set(VIBEOS_BOOTLOADER_ELF "${VIBEOS_ARTIFACTS_DIR}/bootloader.elf")
    set(VIBEOS_BOOTLOADER_EFI "${VIBEOS_ARTIFACTS_DIR}/bootloader.efi")

    add_custom_command(
        OUTPUT "${VIBEOS_BOOTLOADER_ELF}"
        COMMAND ${CMAKE_COMMAND} -E copy
            $<TARGET_FILE:vibeos_bootloader_uefi>
            "${VIBEOS_BOOTLOADER_ELF}"
        DEPENDS vibeos_bootloader_uefi
        COMMENT "Staging UEFI bootloader intermediate image"
        VERBATIM
    )

    add_custom_command(
        OUTPUT "${VIBEOS_BOOTLOADER_EFI}"
        COMMAND ${CMAKE_COMMAND}
            "-DINPUT_ELF=${VIBEOS_BOOTLOADER_ELF}"
            "-DOUTPUT_EFI=${VIBEOS_BOOTLOADER_EFI}"
            "-DOBJCOPY_TOOL=${VIBEOS_OBJCOPY_TOOL}"
            "-DPYTHON_EXECUTABLE=${Python3_EXECUTABLE}"
            "-DEFI_PE_FIXUP=${CMAKE_CURRENT_SOURCE_DIR}/tools/efi_pe_fixup.py"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/convert_efi_image.cmake"
        DEPENDS
            "${VIBEOS_BOOTLOADER_ELF}"
            "${CMAKE_CURRENT_SOURCE_DIR}/cmake/convert_efi_image.cmake"
        COMMENT "Converting bootloader intermediate image to validated EFI application"
        VERBATIM
    )

    add_custom_target(vibeos_bootloader_efi
        DEPENDS "${VIBEOS_BOOTLOADER_EFI}"
    )

    # Build EFI media layout in artifacts/efi_root with BOOTX64.EFI + kernel payload.
    add_custom_target(vibeos_boot_image
        COMMAND ${CMAKE_COMMAND} -E echo "Creating EFI media layout..."
        COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/create_boot_image.cmake
        DEPENDS vibeos_kernel_elf vibeos_bootloader_efi
        COMMENT "Generating bootable kernel image"
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
