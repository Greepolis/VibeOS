# create_boot_image.cmake - Generate bootable kernel image for M1
# This is a placeholder for the boot image generation pipeline.
# For M1, we create a simple ELF wrapper; future phases will evolve this to disk image.

set(ARTIFACTS_DIR "${CMAKE_BINARY_DIR}/artifacts")
set(KERNEL_ELF "${ARTIFACTS_DIR}/vibeos_kernel.elf")
set(BOOT_IMAGE "${ARTIFACTS_DIR}/vibeos_boot.img")

# For M1/M2, the boot image is still derived from the kernel ELF.
# Keep the original ELF available for CI/tooling and generate boot.img as a copy.
if(EXISTS "${KERNEL_ELF}")
    file(COPY_FILE "${KERNEL_ELF}" "${BOOT_IMAGE}" ONLY_IF_DIFFERENT)
    message(STATUS "Boot image created: ${BOOT_IMAGE}")
else()
    message(WARNING "Kernel ELF not found at ${KERNEL_ELF}")
endif()

# Generate a manifest of boot artifacts
set(BOOT_MANIFEST "${ARTIFACTS_DIR}/boot_manifest.txt")
string(TIMESTAMP BOOT_ARTIFACTS_GENERATED_UTC "%Y-%m-%dT%H:%M:%SZ" UTC)
file(WRITE "${BOOT_MANIFEST}"
    "VibeOS Boot Artifacts Manifest\n"
    "Generated (UTC): ${BOOT_ARTIFACTS_GENERATED_UTC}\n"
    "Kernel ELF: ${KERNEL_ELF}\n"
    "Boot Image: ${BOOT_IMAGE}\n"
)
