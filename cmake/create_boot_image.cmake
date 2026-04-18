# create_boot_image.cmake - Generate bootable kernel image for M1
# This is a placeholder for the boot image generation pipeline.
# For M1, we create a simple ELF wrapper; future phases will evolve this to disk image.

set(ARTIFACTS_DIR "${CMAKE_BINARY_DIR}/artifacts")
set(KERNEL_ELF "${ARTIFACTS_DIR}/vibeos_kernel.elf")
set(BOOT_IMAGE "${ARTIFACTS_DIR}/vibeos_boot.img")

# For M1, the boot image is a symbolic link or copy of the kernel ELF
# Real bootloader integration will follow in M2/M3
if(EXISTS "${KERNEL_ELF}")
    # Create a naming convention for boot image (ELF stub for now)
    file(COPY "${KERNEL_ELF}" DESTINATION "${ARTIFACTS_DIR}")
    file(RENAME "${ARTIFACTS_DIR}/vibeos_kernel.elf" "${BOOT_IMAGE}")
    message(STATUS "Boot image created: ${BOOT_IMAGE}")
else()
    message(WARNING "Kernel ELF not found at ${KERNEL_ELF}")
endif()

# Generate a manifest of boot artifacts
set(BOOT_MANIFEST "${ARTIFACTS_DIR}/boot_manifest.txt")
file(WRITE "${BOOT_MANIFEST}"
    "VibeOS Boot Artifacts Manifest\n"
    "Generated: $(date)\n"
    "Kernel ELF: ${KERNEL_ELF}\n"
    "Boot Image: ${BOOT_IMAGE}\n"
)
