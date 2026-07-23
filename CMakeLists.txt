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
        kernel/arch/x86_64/isr.S
        kernel/arch/x86_64/arch_hw.c
        kernel/arch/x86_64/elf_load.c
        # Freestanding mem* runtime: the kernel image links -nostdlib, and some
        # compilers (notably Clang) lower struct copies to memcpy/memset calls
        # that GCC inlines. Provide the implementations so the image links on
        # every toolchain, not just the one that happened to inline them.
        boot/freestanding_runtime.c
    )

    # Real hardware bring-up sources (image-only, never linked into host tests).
    target_compile_options(vibeos_kernel PRIVATE
        -ffreestanding
        -fno-stack-protector
        -mno-red-zone
        -Wall
        -Wextra
        -Wpedantic
    )

    # --- User program: a standalone freestanding ELF embedded into the kernel ---
    # Built as a static, position-dependent ELF64 (linked at a fixed low vaddr),
    # then turned into a C byte array the kernel's ELF loader parses at runtime.
    add_executable(vibeos_user_hello user/prog/hello.c)
    target_compile_options(vibeos_user_hello PRIVATE
        -ffreestanding -fno-stack-protector -fno-pic -mno-red-zone
        -fno-strict-aliasing -Wall -Wextra
    )
    target_compile_definitions(vibeos_user_hello PRIVATE VIBEOS_USE_SYSCALL_INSN=1)
    target_link_options(vibeos_user_hello PRIVATE
        -nostdlib -no-pie
        -Wl,-e,_start
        -Wl,-T,${CMAKE_CURRENT_SOURCE_DIR}/user/prog/user.ld
    )
    set_target_properties(vibeos_user_hello PROPERTIES
        LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/user/prog/user.ld"
    )

    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    set(VIBEOS_USER_BLOB "${CMAKE_BINARY_DIR}/generated/user_hello_elf.c")
    add_custom_command(
        OUTPUT "${VIBEOS_USER_BLOB}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/generated"
        COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/bin2c.py"
            $<TARGET_FILE:vibeos_user_hello>
            "${VIBEOS_USER_BLOB}"
            vibeos_user_hello_elf
        DEPENDS vibeos_user_hello "${CMAKE_CURRENT_SOURCE_DIR}/tools/bin2c.py"
        COMMENT "Embedding user program ELF into the kernel image"
        VERBATIM
    )
    target_sources(vibeos_kernel PRIVATE "${VIBEOS_USER_BLOB}")

    # Second user program: a looping task for the preemptive scheduler demo.
    add_executable(vibeos_user_task user/prog/task.c)
    target_compile_options(vibeos_user_task PRIVATE
        -ffreestanding -fno-stack-protector -fno-pic -mno-red-zone
        -fno-strict-aliasing -Wall -Wextra
    )
    target_compile_definitions(vibeos_user_task PRIVATE VIBEOS_USE_SYSCALL_INSN=1)
    target_link_options(vibeos_user_task PRIVATE
        -nostdlib -no-pie
        -Wl,-e,_start
        -Wl,-T,${CMAKE_CURRENT_SOURCE_DIR}/user/prog/user.ld
    )
    set_target_properties(vibeos_user_task PROPERTIES
        LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/user/prog/user.ld"
    )
    set(VIBEOS_TASK_BLOB "${CMAKE_BINARY_DIR}/generated/user_task_elf.c")
    add_custom_command(
        OUTPUT "${VIBEOS_TASK_BLOB}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/generated"
        COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/bin2c.py"
            $<TARGET_FILE:vibeos_user_task>
            "${VIBEOS_TASK_BLOB}"
            vibeos_user_task_elf
        DEPENDS vibeos_user_task "${CMAKE_CURRENT_SOURCE_DIR}/tools/bin2c.py"
        COMMENT "Embedding scheduler task ELF into the kernel image"
        VERBATIM
    )
    target_sources(vibeos_kernel PRIVATE "${VIBEOS_TASK_BLOB}")

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
    # UEFI freestanding compilation and EFI production. GCC builds a freestanding
    # shared ELF and converts it to PE with objcopy; Clang emits a native UEFI PE
    # directly via the Windows target + LLD, because the objcopy ELF->PE path
    # produces a Clang image OVMF rejects at LoadImage (see
    # docs/implementation_progress/bootloader.md).
    set(VIBEOS_BOOTLOADER_EFI "${VIBEOS_ARTIFACTS_DIR}/bootloader.efi")

    if(CMAKE_C_COMPILER_ID STREQUAL "Clang")
        target_compile_options(vibeos_bootloader_uefi PRIVATE
            --target=x86_64-unknown-windows-msvc  # MS x64 ABI matches UEFI; emits COFF
            -ffreestanding
            -fno-stack-protector
            -mno-red-zone
            -mno-stack-arg-probe   # avoid MSVC __chkstk probes under -nostdlib
            -fno-strict-aliasing
            -Wall
            -Wextra
        )
        target_link_options(vibeos_bootloader_uefi PRIVATE
            --target=x86_64-unknown-windows-msvc
            -fuse-ld=lld
            -nostdlib
            -Wl,-subsystem:efi_application
            -Wl,-entry:efi_main
            -Wl,-nodefaultlib
        )
        # LLD emits a valid PE32+ directly; drop it straight into artifacts/ as
        # bootloader.efi (no objcopy conversion needed).
        set_target_properties(vibeos_bootloader_uefi PROPERTIES
            OUTPUT_NAME bootloader
            SUFFIX .efi
            RUNTIME_OUTPUT_DIRECTORY "${VIBEOS_ARTIFACTS_DIR}"
        )
        add_custom_target(vibeos_bootloader_efi DEPENDS vibeos_bootloader_uefi)
    else()
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
        # GNU ld (Linux) doesn't support --subsystem; objcopy handles PE subsystem.
        if(MINGW)
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

        target_link_libraries(vibeos_bootloader_uefi PRIVATE gcc)

        find_program(VIBEOS_OBJCOPY_TOOL
            NAMES llvm-objcopy objcopy x86_64-w64-mingw32-objcopy
        )
        if(NOT VIBEOS_OBJCOPY_TOOL)
            message(FATAL_ERROR "objcopy-compatible tool not found (required for EFI bootloader conversion)")
        endif()
        find_package(Python3 COMPONENTS Interpreter REQUIRED)

        set(VIBEOS_BOOTLOADER_ELF "${VIBEOS_ARTIFACTS_DIR}/bootloader.elf")

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
    endif()

    # Build EFI media layout in artifacts/efi_root with BOOTX64.EFI + kernel payload.
    add_custom_target(vibeos_boot_image
        COMMAND ${CMAKE_COMMAND} -E echo "Creating EFI media layout..."
        COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/create_boot_image.cmake
        DEPENDS vibeos_kernel_elf vibeos_bootloader_efi
        COMMENT "Generating bootable kernel image"
    )
    
    # Consolidated target: build all boot artifacts
    add_custom_target(vibeos_image ALL DEPENDS vibeos_boot_image vibeos_kernel_elf vibeos_bootloader_efi)

    # Optional: importable VM images (esp.img/.vdi/.vmdk/.iso). Not part of ALL;
    # build explicitly with `cmake --build <dir> --target vibeos_vm_images`.
    # Linux/WSL only (uses qemu-img and, for the .iso, xorriso).
    if(CMAKE_HOST_UNIX)
        add_custom_target(vibeos_vm_images
            COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/scripts/make-vm-images-linux.sh ${CMAKE_BINARY_DIR}
            DEPENDS vibeos_image
            COMMENT "Building importable VM images (esp.img/.vdi/.vmdk/.iso)"
            USES_TERMINAL
        )
    endif()
endif()

# Export toolchain information for scripts
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/vibeos_toolchain_info.cmake.in
    ${VIBEOS_ARTIFACTS_DIR}/vibeos_toolchain_info.cmake
    @ONLY
)
