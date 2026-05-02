# create_boot_image.cmake - Generate EFI boot media layout artifacts

set(ARTIFACTS_DIR "${CMAKE_BINARY_DIR}/artifacts")
set(KERNEL_ELF "${ARTIFACTS_DIR}/vibeos_kernel.elf")
set(BOOTLOADER_EFI "${ARTIFACTS_DIR}/bootloader.efi")
set(LEGACY_BOOT_IMAGE "${ARTIFACTS_DIR}/vibeos_boot.img")
set(EFI_ROOT_DIR "${ARTIFACTS_DIR}/efi_root")
set(EFI_BOOT_DIR "${EFI_ROOT_DIR}/EFI/BOOT")
set(EFI_BOOTX64 "${EFI_BOOT_DIR}/BOOTX64.EFI")
set(EFI_KERNEL "${EFI_BOOT_DIR}/VIBEOSKR.ELF")
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
file(SHA256 "${EFI_ARCHIVE}" EFI_ARCHIVE_SHA256)

set(BOOT_MANIFEST "${ARTIFACTS_DIR}/boot_manifest.txt")
string(TIMESTAMP BOOT_ARTIFACTS_GENERATED_UTC "%Y-%m-%dT%H:%M:%SZ" UTC)
file(SIZE "${EFI_BOOTX64}" BOOTLOADER_SIZE)
file(SIZE "${EFI_KERNEL}" KERNEL_SIZE)
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
    "archive=${EFI_ARCHIVE}\n"
    "\n"
    "[integrity]\n"
    "bootloader_size=${BOOTLOADER_SIZE}\n"
    "kernel_size=${KERNEL_SIZE}\n"
    "archive_size=${ARCHIVE_SIZE}\n"
    "bootloader_sha256=${BOOTLOADER_SHA256}\n"
    "kernel_sha256=${KERNEL_SHA256}\n"
    "archive_sha256=${EFI_ARCHIVE_SHA256}\n"
)

message(STATUS "EFI media root created at: ${EFI_ROOT_DIR}")
