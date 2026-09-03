# create_boot_image.cmake - Generate EFI boot media layout artifacts

set(ARTIFACTS_DIR "${CMAKE_BINARY_DIR}/artifacts")
set(KERNEL_ELF "${ARTIFACTS_DIR}/vibeos_kernel.elf")
set(BOOTLOADER_EFI "${ARTIFACTS_DIR}/bootloader.efi")
set(LEGACY_BOOT_IMAGE "${ARTIFACTS_DIR}/vibeos_boot.img")
set(EFI_ROOT_DIR "${ARTIFACTS_DIR}/efi_root")
set(EFI_BOOT_DIR "${EFI_ROOT_DIR}/EFI/BOOT")
set(EFI_BOOTX64 "${EFI_BOOT_DIR}/BOOTX64.EFI")
set(EFI_KERNEL "${EFI_BOOT_DIR}/VIBEOSKR.ELF")
set(EFI_STARTUP_NSH "${EFI_ROOT_DIR}/startup.nsh")
set(EFI_ARCHIVE "${ARTIFACTS_DIR}/vibeos_efi_root.tar")

if(NOT EXISTS "${KERNEL_ELF}")
    message(FATAL_ERROR "Kernel ELF not found at ${KERNEL_ELF}")
endif()

if(NOT EXISTS "${BOOTLOADER_EFI}")
    message(FATAL_ERROR "Bootloader EFI not found at ${BOOTLOADER_EFI}")
endif()

file(REMOVE_RECURSE "${EFI_ROOT_DIR}")
file(MAKE_DIRECTORY "${EFI_BOOT_DIR}")

# Real EFI filesystem layout (QEMU OVMF-compatible virtual FAT tree)
file(COPY_FILE "${BOOTLOADER_EFI}" "${EFI_BOOTX64}" ONLY_IF_DIFFERENT)
file(COPY_FILE "${KERNEL_ELF}" "${EFI_KERNEL}" ONLY_IF_DIFFERENT)
file(WRITE "${EFI_STARTUP_NSH}"
    "fs0:\\EFI\\BOOT\\BOOTX64.EFI\n"
)

# Ship the init user program on the EFI media so the bootloader can load it from
# the filesystem and hand it to the kernel as the initrd module.
# PID 1 is the native init. The bring-up workload it supervises ships beside
# it under its own name - it used to *be* INIT.ELF, which is why there was no
# process left to supervise anything.
set(USER_INIT_ELF "${CMAKE_BINARY_DIR}/vibeos_user_init")
if(NOT VIBEOS_USE_NATIVE_INIT)
    set(USER_INIT_ELF "${CMAKE_BINARY_DIR}/vibeos_user_hello")
endif()

# The supervised services named in init's manifest.
foreach(svc_pair "vibeos_user_svc_ok:SVC_OK.ELF" "vibeos_user_svc_flap:SVC_FLAP.ELF" "vibeos_user_svc_crash:SVC_CRSH.ELF" "vibeos_user_svc_stress:SVC_STRS.ELF" "vibeos_user_svc_bomb:SVC_BOMB.ELF")
    string(REPLACE ":" ";" svc_parts "${svc_pair}")
    list(GET svc_parts 0 svc_target)
    list(GET svc_parts 1 svc_name)
    if(EXISTS "${CMAKE_BINARY_DIR}/${svc_target}")
        file(COPY_FILE "${CMAKE_BINARY_DIR}/${svc_target}"
             "${EFI_BOOT_DIR}/${svc_name}" ONLY_IF_DIFFERENT)
    endif()
endforeach()

set(SELFTEST_ELF "${CMAKE_BINARY_DIR}/vibeos_user_hello")
if(EXISTS "${SELFTEST_ELF}")
    file(COPY_FILE "${SELFTEST_ELF}" "${EFI_BOOT_DIR}/SELFTEST.ELF" ONLY_IF_DIFFERENT)
endif()
if(EXISTS "${USER_INIT_ELF}")
    file(COPY_FILE "${USER_INIT_ELF}" "${EFI_BOOT_DIR}/INIT.ELF" ONLY_IF_DIFFERENT)
    message(STATUS "EFI media includes init program: EFI/BOOT/INIT.ELF (native=${VIBEOS_USE_NATIVE_INIT})")
else()
    message(STATUS "Init program not found at ${USER_INIT_ELF}; kernel will use its built-in copy")
endif()

set(NATIVE_INIT_ELF "${CMAKE_BINARY_DIR}/vibeos_user_init")
if(EXISTS "${NATIVE_INIT_ELF}")
    file(COPY_FILE "${NATIVE_INIT_ELF}" "${EFI_BOOT_DIR}/NATIVE_INIT.ELF" ONLY_IF_DIFFERENT)
    message(STATUS "EFI media includes native init candidate: EFI/BOOT/NATIVE_INIT.ELF")
endif()

# A second program on disk, for the init program to exec().
set(USER_SH_ELF "${CMAKE_BINARY_DIR}/vibeos_user_sh")
if(EXISTS "${USER_SH_ELF}")
    file(COPY_FILE "${USER_SH_ELF}" "${EFI_BOOT_DIR}/SH.ELF" ONLY_IF_DIFFERENT)
    message(STATUS "EFI media includes shell: EFI/BOOT/SH.ELF")
endif()

set(USER_NET_ELF "${CMAKE_BINARY_DIR}/vibeos_user_net")
if(EXISTS "${USER_NET_ELF}")
    file(COPY_FILE "${USER_NET_ELF}" "${EFI_BOOT_DIR}/NET.ELF" ONLY_IF_DIFFERENT)
    message(STATUS "EFI media includes network client: EFI/BOOT/NET.ELF")
endif()

set(USER_TASK_ELF "${CMAKE_BINARY_DIR}/vibeos_user_task")
if(EXISTS "${USER_TASK_ELF}")
    file(COPY_FILE "${USER_TASK_ELF}" "${EFI_BOOT_DIR}/TASK.ELF" ONLY_IF_DIFFERENT)
    message(STATUS "EFI media includes exec target: EFI/BOOT/TASK.ELF")
endif()

# An unmodified static Linux binary, if the build produced one. Nothing in it
# was written for VibeOS: it is a normal Linux program compiled by musl-gcc,
# and running it is the point of the whole Linux-ABI effort.
set(MUSL_ELF "${CMAKE_BINARY_DIR}/musl_hello")
if(EXISTS "${MUSL_ELF}")
    file(COPY_FILE "${MUSL_ELF}" "${EFI_BOOT_DIR}/MUSL.ELF" ONLY_IF_DIFFERENT)
    message(STATUS "EFI media includes an unmodified Linux binary: EFI/BOOT/MUSL.ELF")
endif()

# The same program built position-independent. An ET_DYN executable is placed
# by the loader rather than by the file, so this is the one that says whether
# VibeOS can do the placing.
set(PIE_ELF "${CMAKE_BINARY_DIR}/musl_pie")
if(EXISTS "${PIE_ELF}")
    file(COPY_FILE "${PIE_ELF}" "${EFI_BOOT_DIR}/PIE.ELF" ONLY_IF_DIFFERENT)
    message(STATUS "EFI media includes a position-independent Linux binary: EFI/BOOT/PIE.ELF")
endif()

# The dynamically linked one, plus the interpreter it names. Both are needed:
# a dynamic executable without its loader is a file the kernel can parse and
# refuse, which tests nothing about loading.
#
# The loader is copied under a name FAT can hold. The binary asks for
# /lib/ld-musl-x86_64.so.1, and the kernel translates that one path - the
# substitution is written in the kernel too, so it is visible from both ends
# rather than being a property of how the media happened to be built.
set(DYN_ELF "${CMAKE_BINARY_DIR}/musl_dynamic")
if(EXISTS "${DYN_ELF}")
    set(MUSL_LOADER "/usr/lib/x86_64-linux-musl/libc.so")
    if(EXISTS "${MUSL_LOADER}")
        file(COPY_FILE "${DYN_ELF}" "${EFI_BOOT_DIR}/DYN.ELF" ONLY_IF_DIFFERENT)
        file(COPY_FILE "${MUSL_LOADER}" "${EFI_BOOT_DIR}/LDMUSL.SO" ONLY_IF_DIFFERENT)
        message(STATUS "EFI media includes a dynamic Linux binary and its loader: EFI/BOOT/DYN.ELF")
    else()
        message(STATUS "musl loader not found; skipping the dynamic Linux binary")
    endif()
endif()

# Threads, through the library's own pthread implementation: one created and
# joined, then four at once contending for a mutex.
set(THR_ELF "${CMAKE_BINARY_DIR}/musl_threads")
if(EXISTS "${THR_ELF}")
    file(COPY_FILE "${THR_ELF}" "${EFI_BOOT_DIR}/THREADS.ELF" ONLY_IF_DIFFERENT)
    message(STATUS "EFI media includes a threaded Linux binary: EFI/BOOT/THREADS.ELF")
endif()

# fork() from a process that still has a thread running - what exercises the
# TLB shootdown. 8.3 names: TFORK, not MUSL_TFORK.
# Threads and exec together. 8.3 name: TEXEC.
set(TEXEC_ELF "${CMAKE_BINARY_DIR}/musl_texec")
if(EXISTS "${TEXEC_ELF}")
    file(COPY_FILE "${TEXEC_ELF}" "${EFI_BOOT_DIR}/TEXEC.ELF" ONLY_IF_DIFFERENT)
    message(STATUS "EFI media includes a thread-and-exec Linux binary: EFI/BOOT/TEXEC.ELF")
endif()

set(TFORK_ELF "${CMAKE_BINARY_DIR}/musl_tfork")
if(EXISTS "${TFORK_ELF}")
    file(COPY_FILE "${TFORK_ELF}" "${EFI_BOOT_DIR}/TFORK.ELF" ONLY_IF_DIFFERENT)
    message(STATUS "EFI media includes a threaded-fork Linux binary: EFI/BOOT/TFORK.ELF")
endif()

# A static BusyBox from the host, if one is installed. This is a real program
# doing real work - open a file, read a directory, write to stdout - rather than
# a test written to pass. It is not built here and nothing about it was chosen
# by this project.
find_program(VIBEOS_BUSYBOX busybox PATHS /usr/bin /bin NO_CACHE)
if(VIBEOS_BUSYBOX)
    file(READ "${VIBEOS_BUSYBOX}" BB_MAGIC LIMIT 4 HEX)
    if(BB_MAGIC STREQUAL "7f454c46")
        file(COPY_FILE "${VIBEOS_BUSYBOX}" "${EFI_BOOT_DIR}/BUSYBOX.ELF" ONLY_IF_DIFFERENT)
        message(STATUS "EFI media includes BusyBox: EFI/BOOT/BUSYBOX.ELF")
    endif()
endif()

# BusyBox decides which command it is from the name it was invoked under, so a
# real installation is one binary and a symlink per applet. FAT has no symlinks,
# so these are copies - the mechanism is the same, only the storage is dumber.
# Together with PATH this is what lets a real shell find real commands.
if(VIBEOS_BUSYBOX AND BB_MAGIC STREQUAL "7f454c46")
    foreach(applet CAT LS ECHO WC)
        file(COPY_FILE "${VIBEOS_BUSYBOX}" "${EFI_BOOT_DIR}/${applet}" ONLY_IF_DIFFERENT)
    endforeach()
    message(STATUS "EFI media includes BusyBox applets: cat, ls, echo, wc")
endif()

set(SIG_ELF "${CMAKE_BINARY_DIR}/musl_signal")
if(EXISTS "${SIG_ELF}")
    file(COPY_FILE "${SIG_ELF}" "${EFI_BOOT_DIR}/SIGNAL.ELF" ONLY_IF_DIFFERENT)
    message(STATUS "EFI media includes the signal test: EFI/BOOT/SIGNAL.ELF")
endif()

# Keep legacy kernel-as-image artifact for direct-loader probes.
file(COPY_FILE "${KERNEL_ELF}" "${LEGACY_BOOT_IMAGE}" ONLY_IF_DIFFERENT)

# Archive the EFI tree for artifact portability.
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E tar cf "${EFI_ARCHIVE}" --format=gnutar "EFI"
    WORKING_DIRECTORY "${EFI_ROOT_DIR}"
    RESULT_VARIABLE EFI_ARCHIVE_RESULT
)
if(NOT EFI_ARCHIVE_RESULT EQUAL 0)
    message(FATAL_ERROR "Failed to create EFI root archive: ${EFI_ARCHIVE}")
endif()

file(SHA256 "${EFI_BOOTX64}" BOOTLOADER_SHA256)
file(SHA256 "${EFI_KERNEL}" KERNEL_SHA256)
file(SHA256 "${EFI_STARTUP_NSH}" STARTUP_NSH_SHA256)
file(SHA256 "${EFI_ARCHIVE}" EFI_ARCHIVE_SHA256)

set(BOOT_MANIFEST "${ARTIFACTS_DIR}/boot_manifest.txt")
string(TIMESTAMP BOOT_ARTIFACTS_GENERATED_UTC "%Y-%m-%dT%H:%M:%SZ" UTC)
file(SIZE "${EFI_BOOTX64}" BOOTLOADER_SIZE)
file(SIZE "${EFI_KERNEL}" KERNEL_SIZE)
file(SIZE "${EFI_STARTUP_NSH}" STARTUP_NSH_SIZE)
file(SIZE "${EFI_ARCHIVE}" ARCHIVE_SIZE)
file(WRITE "${BOOT_MANIFEST}"
    "VibeOS Boot Artifacts Manifest\n"
    "Generated (UTC): ${BOOT_ARTIFACTS_GENERATED_UTC}\n"
    "\n"
    "[legacy]\n"
    "kernel_elf=${KERNEL_ELF}\n"
    "boot_image=${LEGACY_BOOT_IMAGE}\n"
    "\n"
    "[efi_media]\n"
    "root=${EFI_ROOT_DIR}\n"
    "bootloader=${EFI_BOOTX64}\n"
    "kernel=${EFI_KERNEL}\n"
    "startup_nsh=${EFI_STARTUP_NSH}\n"
    "archive=${EFI_ARCHIVE}\n"
    "\n"
    "[integrity]\n"
    "bootloader_size=${BOOTLOADER_SIZE}\n"
    "kernel_size=${KERNEL_SIZE}\n"
    "startup_nsh_size=${STARTUP_NSH_SIZE}\n"
    "archive_size=${ARCHIVE_SIZE}\n"
    "bootloader_sha256=${BOOTLOADER_SHA256}\n"
    "kernel_sha256=${KERNEL_SHA256}\n"
    "startup_nsh_sha256=${STARTUP_NSH_SHA256}\n"
    "archive_sha256=${EFI_ARCHIVE_SHA256}\n"
)

message(STATUS "EFI media root created at: ${EFI_ROOT_DIR}")
