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
