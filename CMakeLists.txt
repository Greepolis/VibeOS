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
# On by default now that init supervises the bring-up workload instead of
# replacing it: switching this off returns to the old arrangement, where
# PID 1 was the workload itself.
option(VIBEOS_USE_NATIVE_INIT "Select the native ring-3 init for EFI media" ON)
option(VIBEOS_ENABLE_TLS "Build the hosted Mbed TLS adapter" ON)
# The audited TLS dependency lives in a git submodule. A plain `git clone`
# does not fetch it, so a missing submodule degrades to a build without TLS
# rather than refusing to configure at all. Builds that must ship TLS (release
# gates, the Linux CI matrix) set VIBEOS_REQUIRE_TLS and fail loudly instead.
option(VIBEOS_REQUIRE_TLS "Fail configuration when the TLS dependency is unavailable" OFF)
# libFuzzer target over the network receive path. Clang only.
option(VIBEOS_BUILD_FUZZERS "Build the protocol fuzz targets (clang + libFuzzer)" OFF)

# Artifact directories
set(VIBEOS_ARTIFACTS_DIR "${CMAKE_BINARY_DIR}/artifacts" CACHE PATH "Artifacts output directory")
file(MAKE_DIRECTORY "${VIBEOS_ARTIFACTS_DIR}")

# Compiler and target configuration
set(VIBEOS_TARGET_ARCH "x86_64" CACHE STRING "Target architecture")
set(VIBEOS_BUILD_TYPE "debug" CACHE STRING "Build type: debug, release, hardened")

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/core_sources.cmake)

add_library(vibeos_kernel_core STATIC ${VIBEOS_KERNEL_CORE_SOURCES})

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



set(VIBEOS_TLS_ACTIVE OFF)
if(VIBEOS_ENABLE_TLS)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/mbedtls/CMakeLists.txt")
        set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
        set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
        set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
        add_subdirectory(third_party/mbedtls EXCLUDE_FROM_ALL)
        set(VIBEOS_TLS_ACTIVE ON)
    elseif(VIBEOS_REQUIRE_TLS)
        message(FATAL_ERROR
            "VIBEOS_REQUIRE_TLS is set but third_party/mbedtls is missing. "
            "Run: git submodule update --init --recursive")
    else()
        message(WARNING
            "third_party/mbedtls not present: building without TLS. "
            "Run: git submodule update --init --recursive")
    endif()
endif()

add_library(vibeos_user_core STATIC ${VIBEOS_USER_CORE_SOURCES})

target_include_directories(vibeos_user_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_options(vibeos_user_core PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    # This library is linked into the freestanding kernel image, so it has to
    # be built like the kernel is. Without this a function with a large enough
    # frame gets a stack-protector prologue and the image fails to link on
    # __stack_chk_fail - which does not exist there, and which nothing in the
    # library asked for. It surfaced the first time a change forced the kernel
    # to relink, not when the code was written, so it is the kind of breakage
    # that arrives long after its cause.
    -fno-stack-protector
)

# The TLS adapter is a HOSTED component and lives in its own target, never in
# vibeos_user_core. The kernel image links vibeos_user_core; keeping Mbed TLS
# out of that library is what guarantees a hosted crypto stack can never be
# dragged into the freestanding image by a future reference.
add_library(vibeos_tls STATIC ${VIBEOS_TLS_SOURCES})
target_include_directories(vibeos_tls PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(vibeos_tls PRIVATE -Wall -Wextra -Wpedantic)
if(VIBEOS_TLS_ACTIVE)
    # PUBLIC so anything linking the adapter compiles against the same view of
    # whether the audited dependency is present.
    target_compile_definitions(vibeos_tls PUBLIC VIBEOS_TLS_MBEDTLS=1)
    # The crypto library was renamed across the dependency's major versions:
    # Mbed TLS 3.x builds `mbedcrypto`, 4.x moved it into TF-PSA-Crypto and
    # builds `tfpsacrypto`. Ask which target actually exists instead of naming
    # one - CMake silently turns an unknown target name into a raw -l flag, so
    # a wrong guess is not a configure error but a link failure much later.
    if(TARGET tfpsacrypto)
        set(VIBEOS_TLS_CRYPTO_TARGET tfpsacrypto)
    else()
        set(VIBEOS_TLS_CRYPTO_TARGET mbedcrypto)
    endif()
    target_link_libraries(vibeos_tls PUBLIC mbedtls mbedx509 ${VIBEOS_TLS_CRYPTO_TARGET})
    message(STATUS "TLS: linking ${VIBEOS_TLS_CRYPTO_TARGET} from third_party/mbedtls")
endif()

if(VIBEOS_BUILD_FUZZERS)
    # The receive path is the only code fed bytes an attacker chooses, so it is
    # the one place a fuzzer earns its keep.
    add_executable(fuzz_inet_input ${VIBEOS_FUZZ_INET_SOURCES})
    target_include_directories(fuzz_inet_input PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_compile_options(fuzz_inet_input PRIVATE
        -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer -g
        -Wall -Wextra -Wpedantic
    )
    target_link_options(fuzz_inet_input PRIVATE -fsanitize=fuzzer,address,undefined)
endif()

if(VIBEOS_BUILD_TESTS)
    enable_testing()

    add_executable(vibeos_kernel_tests
        tests/kernel/kernel_tests.c
        tests/kernel/mm_stats_tests.c
        tests/kernel/frame_tests.c
        tests/kernel/vmspace_tests.c
        tests/kernel/vma_tests.c
        tests/kernel/backing_tests.c
        tests/kernel/rmap_tests.c
        tests/kernel/reclaim_tests.c
        tests/kernel/compact_tests.c
        tests/kernel/swap_tests.c
        tests/kernel/acceptance_tests.c
        tests/kernel/swaparea_tests.c
        tests/kernel/task_tests.c
        tests/kernel/runq_tests.c
        tests/kernel/lifetime_tests.c
        tests/kernel/loader_tests.c
        tests/kernel/account_tests.c
        tests/kernel/sched_policy_tests.c
        tests/kernel/forkguard_tests.c
    )
    target_link_libraries(vibeos_kernel_tests PRIVATE vibeos_kernel_core vibeos_user_core vibeos_tls)
    target_include_directories(vibeos_kernel_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    add_test(NAME vibeos-kernel-host COMMAND vibeos_kernel_tests)

    # Randomised torture for the memory manager, checked against a reference
    # model kept in plain arrays. Its own executable rather than a test case
    # because it takes a seed and a round count: the nightly runs many seeds
    # under a sanitizer, and a failure is replayed by passing its seed back.
    add_executable(vibeos_mm_torture tests/kernel/mm_torture.c)
    target_link_libraries(vibeos_mm_torture PRIVATE vibeos_kernel_core)
    target_include_directories(vibeos_mm_torture PRIVATE /include)
    # One short run in the ordinary suite, so a change that breaks it is caught
    # before the nightly rather than by it.
    add_test(NAME vibeos-mm-torture COMMAND vibeos_mm_torture 1 2000)

    add_executable(vibeos_bootloader_tests
        tests/bootloader/uefi_bootloader_tests.c
        boot/bootloader_core.c
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
        kernel/arch/x86_64/ap_boot.S
        ${VIBEOS_ARCH_X86_64_SOURCES}
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
        # Keep the frame pointer. A release build drops it, and without the
        # chain a panic can report where the machine stopped but nothing about
        # how it got there - which for a fault inside one of two hundred static
        # helpers is most of the question. One register is a cheap price for a
        # backtrace on a kernel nobody can attach a debugger to.
        -fno-omit-frame-pointer
        # Debug info in the image on disk, never loaded at runtime: it costs
        # nothing to the guest and is what turns a panic's addresses into a
        # file and a line number through scripts/dev/symbolize.py.
        -g
        -Wall
        -Wextra
        -Wpedantic
    )

    # --- User program: a standalone freestanding ELF embedded into the kernel ---
    # Built as a static, position-dependent ELF64 (linked at a fixed low vaddr),
    # then turned into a C byte array the kernel's ELF loader parses at runtime.
    add_executable(vibeos_user_hello user/prog/hello.c user/prog/crt0.S)
    target_compile_options(vibeos_user_hello PRIVATE
        -ffreestanding -fno-stack-protector -fno-pic -mno-red-zone
        -mcmodel=large   # user space is linked above 4 GiB (its own PML4 slot)
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

    # Native init candidate. It is staged separately from the compatibility
    # bring-up program until the runtime acceptance gate selects it as PID 1.
    # Two supervised services, so the supervisor has something real to
    # supervise: one that finishes cleanly and one that fails every time.
    foreach(svc svc_ok svc_flap svc_crash svc_stress svc_bomb svc_press)
        add_executable(vibeos_user_${svc} user/prog/${svc}.c user/prog/crt0.S)
        target_compile_options(vibeos_user_${svc} PRIVATE
            -ffreestanding -fno-stack-protector -fno-pic -mno-red-zone
            -mcmodel=large -fno-strict-aliasing -Wall -Wextra
        )
        target_link_options(vibeos_user_${svc} PRIVATE
            -nostdlib -no-pie -Wl,-e,_start
            -Wl,-T,${CMAKE_CURRENT_SOURCE_DIR}/user/prog/user.ld
        )
        set_target_properties(vibeos_user_${svc} PROPERTIES
            LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/user/prog/user.ld"
        )
    endforeach()

    add_executable(vibeos_user_init user/prog/init.c user/prog/crt0.S)
    target_compile_options(vibeos_user_init PRIVATE
        -ffreestanding -fno-stack-protector -fno-pic -mno-red-zone
        -mcmodel=large -fno-strict-aliasing -Wall -Wextra
    )
    target_link_options(vibeos_user_init PRIVATE
        -nostdlib -no-pie -Wl,-e,_start
        -Wl,-T,${CMAKE_CURRENT_SOURCE_DIR}/user/prog/user.ld
    )
    set_target_properties(vibeos_user_init PROPERTIES
        LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/user/prog/user.ld"
    )

    # An unmodified static Linux binary, built by a real Linux toolchain against
    # a real libc, staged on the boot media as MUSL.ELF.
    #
    # This is the honest test of Linux compatibility: nothing about it knows
    # VibeOS exists. It is optional because musl-gcc is not present everywhere -
    # where it is missing the media simply lacks the file and the shell reports
    # that it cannot find it, which is a clear result rather than a build break.
    find_program(VIBEOS_MUSL_GCC musl-gcc)
    if(VIBEOS_MUSL_GCC)
        set(VIBEOS_MUSL_SRC "${CMAKE_CURRENT_SOURCE_DIR}/tests/linux/musl_hello.c")
        set(VIBEOS_MUSL_BIN "${CMAKE_BINARY_DIR}/musl_hello")
        add_custom_command(
            OUTPUT "${VIBEOS_MUSL_BIN}"
            COMMAND "${VIBEOS_MUSL_GCC}" -static -O2 -o "${VIBEOS_MUSL_BIN}" "${VIBEOS_MUSL_SRC}"
            DEPENDS "${VIBEOS_MUSL_SRC}"
            COMMENT "Building an unmodified static Linux binary with musl"
            VERBATIM
        )
        add_custom_target(vibeos_musl_hello ALL DEPENDS "${VIBEOS_MUSL_BIN}")

        # Signals, exercised by a program written against Linux rather than
        # against this kernel - the trampoline a handler returns through comes
        # from the C library, so only a real one tests the real path.
        set(VIBEOS_SIG_SRC "${CMAKE_CURRENT_SOURCE_DIR}/tests/linux/musl_signal.c")
        set(VIBEOS_SIG_BIN "${CMAKE_BINARY_DIR}/musl_signal")
        add_custom_command(
            OUTPUT "${VIBEOS_SIG_BIN}"
            COMMAND "${VIBEOS_MUSL_GCC}" -static -O2 -o "${VIBEOS_SIG_BIN}" "${VIBEOS_SIG_SRC}"
            DEPENDS "${VIBEOS_SIG_SRC}"
            COMMENT "Building the signal test as a static Linux binary"
            VERBATIM
        )
        add_custom_target(vibeos_musl_signal ALL DEPENDS "${VIBEOS_SIG_BIN}")

        # A position-independent executable. Same compiler, same library, one
        # linker flag apart - which is the point: the difference under test is
        # the loading, not the program.
        set(VIBEOS_PIE_SRC "${CMAKE_CURRENT_SOURCE_DIR}/tests/linux/musl_pie.c")
        set(VIBEOS_PIE_BIN "${CMAKE_BINARY_DIR}/musl_pie")
        add_custom_command(
            OUTPUT "${VIBEOS_PIE_BIN}"
            COMMAND "${VIBEOS_MUSL_GCC}" -static-pie -O2 -o "${VIBEOS_PIE_BIN}" "${VIBEOS_PIE_SRC}"
            DEPENDS "${VIBEOS_PIE_SRC}"
            COMMENT "Building a position-independent Linux binary with musl"
            VERBATIM
        )
        add_custom_target(vibeos_musl_pie ALL DEPENDS "${VIBEOS_PIE_BIN}")

        # And a dynamically linked one. This is the kind that needs an
        # interpreter loaded alongside it, which is the last step in running
        # ordinary Linux software rather than specially built software.
        set(VIBEOS_DYN_SRC "${CMAKE_CURRENT_SOURCE_DIR}/tests/linux/musl_dynamic.c")
        set(VIBEOS_DYN_BIN "${CMAKE_BINARY_DIR}/musl_dynamic")
        add_custom_command(
            OUTPUT "${VIBEOS_DYN_BIN}"
            COMMAND "${VIBEOS_MUSL_GCC}" -O2 -o "${VIBEOS_DYN_BIN}" "${VIBEOS_DYN_SRC}"
            DEPENDS "${VIBEOS_DYN_SRC}"
            COMMENT "Building a dynamically linked Linux binary with musl"
            VERBATIM
        )
        add_custom_target(vibeos_musl_dynamic ALL DEPENDS "${VIBEOS_DYN_BIN}")

        # Threads, through the library's own pthread implementation. Static so
        # that a failure is about clone and futex rather than about loading.
        set(VIBEOS_THR_SRC "${CMAKE_CURRENT_SOURCE_DIR}/tests/linux/musl_threads.c")
        set(VIBEOS_THR_BIN "${CMAKE_BINARY_DIR}/musl_threads")
        add_custom_command(
            OUTPUT "${VIBEOS_THR_BIN}"
            COMMAND "${VIBEOS_MUSL_GCC}" -static -O2 -o "${VIBEOS_THR_BIN}" "${VIBEOS_THR_SRC}"
            DEPENDS "${VIBEOS_THR_SRC}"
            COMMENT "Building a threaded Linux binary with musl"
            VERBATIM
        )
        add_custom_target(vibeos_musl_threads ALL DEPENDS "${VIBEOS_THR_BIN}")

        # fork() from a process that still has a thread running: the one shape
        # that needs a TLB shootdown, and the only thing in the boot that
        # produces it.
        set(VIBEOS_TFORK_SRC "${CMAKE_CURRENT_SOURCE_DIR}/tests/linux/musl_tfork.c")
        set(VIBEOS_TFORK_BIN "${CMAKE_BINARY_DIR}/musl_tfork")
        add_custom_command(
            OUTPUT "${VIBEOS_TFORK_BIN}"
            COMMAND "${VIBEOS_MUSL_GCC}" -static -O2 -o "${VIBEOS_TFORK_BIN}" "${VIBEOS_TFORK_SRC}"
            DEPENDS "${VIBEOS_TFORK_SRC}"
            COMMENT "Building a threaded-fork Linux binary with musl"
            VERBATIM
        )
        add_custom_target(vibeos_musl_tfork ALL DEPENDS "${VIBEOS_TFORK_BIN}")

        # Threads created and joined while children of the process exec: the
        # other half of the combination that produced the execve defect, and
        # the half nothing exercised. execve destroys the outgoing address
        # space, and for a long time it did so without asking whether a sibling
        # thread was still running on it.
        set(VIBEOS_TEXEC_SRC "${CMAKE_CURRENT_SOURCE_DIR}/tests/linux/musl_texec.c")
        set(VIBEOS_TEXEC_BIN "${CMAKE_BINARY_DIR}/musl_texec")
        add_custom_command(
            OUTPUT "${VIBEOS_TEXEC_BIN}"
            COMMAND "${VIBEOS_MUSL_GCC}" -static -O2 -o "${VIBEOS_TEXEC_BIN}" "${VIBEOS_TEXEC_SRC}" -lpthread
            DEPENDS "${VIBEOS_TEXEC_SRC}"
            COMMENT "Building a thread-and-exec Linux binary with musl"
            VERBATIM
        )
        add_custom_target(vibeos_musl_texec ALL DEPENDS "${VIBEOS_TEXEC_BIN}")
    else()
        message(STATUS "musl-gcc not found: the boot media will carry no unmodified Linux binary")
    endif()

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

    # Third user program: the shell (staged on media, exec-ed by init).
    add_executable(vibeos_user_sh user/prog/sh.c user/prog/crt0.S)
    target_compile_options(vibeos_user_sh PRIVATE
        -ffreestanding -fno-stack-protector -fno-pic -mno-red-zone
        -mcmodel=large
        -fno-strict-aliasing -Wall -Wextra
    )
    target_link_options(vibeos_user_sh PRIVATE
        -nostdlib -no-pie
        -Wl,-e,_start
        -Wl,-T,${CMAKE_CURRENT_SOURCE_DIR}/user/prog/user.ld
    )
    set_target_properties(vibeos_user_sh PROPERTIES
        LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/user/prog/user.ld"
    )

    # Fourth user program: a TCP client, staged on media as NET.ELF.
    add_executable(vibeos_user_net user/prog/net.c user/prog/crt0.S)
    target_compile_options(vibeos_user_net PRIVATE
        -ffreestanding -fno-stack-protector -fno-pic -mno-red-zone
        -mcmodel=large
        -fno-strict-aliasing -Wall -Wextra
    )
    target_link_options(vibeos_user_net PRIVATE
        -nostdlib -no-pie
        -Wl,-e,_start
        -Wl,-T,${CMAKE_CURRENT_SOURCE_DIR}/user/prog/user.ld
    )
    set_target_properties(vibeos_user_net PROPERTIES
        LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/user/prog/user.ld"
    )

    # Second user program: a looping task for the preemptive scheduler demo.
    add_executable(vibeos_user_task user/prog/task.c user/prog/crt0.S)
    target_compile_options(vibeos_user_task PRIVATE
        -ffreestanding -fno-stack-protector -fno-pic -mno-red-zone
        -mcmodel=large   # user space is linked above 4 GiB (its own PML4 slot)
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
        boot/bootloader_core.c
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
            -fno-omit-frame-pointer   # see the GCC branch below
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
            # Frame pointers and debug info, for the same reason the kernel has
            # them: this code runs before any kernel diagnostics exist, so if it
            # ever stops quietly the only things to work with are an address
            # from the emulator and whatever can resolve it. They cost nothing
            # in the shipped image - the objcopy step below copies an explicit
            # list of sections and .debug_* is not on it, so the debug info
            # stays in the intermediate ELF where addr2line can read it.
            -fno-omit-frame-pointer
            -g
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
        COMMAND ${CMAKE_COMMAND}
            -DVIBEOS_USE_NATIVE_INIT=${VIBEOS_USE_NATIVE_INIT}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/create_boot_image.cmake
        DEPENDS vibeos_kernel_elf vibeos_bootloader_efi vibeos_user_hello vibeos_user_init vibeos_user_task vibeos_user_sh vibeos_user_net
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
