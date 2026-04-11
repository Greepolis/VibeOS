# Boot System

## Goals

- deterministic early boot path
- support for modern firmware on x86_64
- clear separation between bootloader responsibilities and kernel initialization
- smooth future transition to ARM64 platform bring-up

## Boot model

### Phase 0: firmware entry

Initial target firmware is UEFI. Legacy BIOS support is intentionally deferred to reduce complexity in early milestones.

### Phase 1: project bootloader

The project will eventually provide a custom bootloader with the following responsibilities:

- load kernel image and core modules
- collect memory map and firmware tables
- establish framebuffer and console handoff data
- load initial ramdisk or boot package
- verify image signatures in secure boot configurations
- pass structured boot information to the kernel

During bring-up, a simpler standards-compliant boot path may be used temporarily if it does not compromise the from-scratch kernel requirement.

Recommended staging:

- early bring-up may target a well-defined external boot protocol to shorten time-to-first-kernel
- the long-term architecture still assumes a project-owned bootloader and boot information contract
- no existing kernel code is reused; only the boot handoff convention may be borrowed temporarily

### Phase 2: kernel early init

- establish early serial and framebuffer logging
- parse boot information structure
- initialize physical memory manager
- create early page tables and higher-half kernel mapping
- enable interrupts after core descriptor tables are ready
- bring up bootstrap processor services

### Phase 3: SMP and service handoff

- discover and start application processors
- initialize scheduler on all cores
- start init service and system service manager
- transition from early allocators to full memory subsystem

## Early memory strategy

### Boot allocators

- bump allocator for pre-paging early boot
- early page allocator for initial mappings
- handoff to full physical frame allocator once memory regions are classified

### Address space model

- higher-half kernel mapping
- direct physical map for kernel internal memory operations
- per-process user address spaces with guarded kernel/user split

## Boot data contract

The bootloader passes a versioned boot information block containing:

- memory map
- CPU and topology data
- ACPI or equivalent firmware tables
- framebuffer information
- boot medium metadata
- init package descriptors
- security and measured boot state

## Risks and mitigation

- firmware diversity risk is mitigated by targeting UEFI first
- early memory corruption risk is reduced through strict phase separation
- boot ABI drift is controlled through versioned handoff structures

## Current implementation snapshot

- bootloader stub exposes boot-info validation and memory summary helpers.
- boot-info contract checks now reject zero-length memory regions and invalid base contracts before kernel bring-up.
- kernel bootstrap initializes security-audit state during early core initialization.
- bootloader helpers now expose region-type counters and overlap detection for memory-map sanity checks.
- sanitized boot-info builder now normalizes memory maps (supported-type filtering, base-order sort, adjacent-merge by type) before kernel handoff.
- boot contract now provides max physical address helper to support early direct-map sizing decisions.
- boot handoff metadata helpers now cover firmware table pointers (`ACPI`/`SMBIOS`), initrd boundaries, and framebuffer geometry with strict validation.
- boot validation now enforces memory-map placement rules for firmware pointers, initrd ranges, and framebuffer regions to prevent invalid handoff layouts.
- bootloader can now extract firmware tags into handoff fields/flags (`secure boot`, `measured boot`) and produce a PE32+ load plan for UEFI-style kernel image mapping.
