# Memory Management

## Goals

- strong isolation between protection domains
- predictable page fault and mapping behavior
- scalable memory allocation on multicore systems
- support for compatibility runtimes with different memory semantics

## Physical memory management

### Early phase

- firmware-provided memory map ingestion
- boot-time region classification
- early frame allocator for bootstrap use

### Runtime phase

- buddy allocator or segmented frame allocator for general page allocation
- separate pools for DMA-capable memory when required
- accounting by zone and future NUMA node

## Virtual memory model

### Kernel space

- higher-half kernel mapping
- direct map for physical memory access
- guard pages around critical regions

### User space

- per-process address spaces
- demand paging support
- copy-on-write support for process creation and memory sharing
- mapped objects for files, anonymous memory, and shared memory

## Memory objects

All mappings are backed by explicit memory objects:

- anonymous memory object
- file-backed memory object
- shared memory object
- device memory object

This simplifies accounting, rights management, and compatibility runtime integration.

## Kernel heap

The kernel heap should be layered:

- early bump allocator
- slab or object caches for fixed-size kernel objects
- general-purpose heap for variable allocations

The design should favor cache locality and deterministic behavior for hot object types.

## Userland allocation support

User processes receive kernel primitives for:

- reserving virtual address space
- committing and decommitting pages
- protection changes
- shared memory mapping
- memory-mapped file creation

These primitives are sufficient to emulate Linux `mmap`, Windows virtual memory APIs, and selected Darwin allocation behavior in user-space compatibility layers.

## Hardening

- W^X enforcement where compatible
- ASLR for kernel and user spaces as platform maturity allows
- stack guards
- guarded pools for sensitive allocations
- optional page poisoning and quarantine in debug builds

## Future enhancements

- transparent huge pages where beneficial
- compressed memory experiments
- NUMA-aware placement
- confidential-computing-friendly abstractions

## Current implementation snapshot

- VM runtime now supports partial unmap (`unmap_range`) with map splitting semantics.
- VM observability helpers expose total map count and total mapped bytes.
- read-only clone support is available (`clone_readonly`) to model early copy-on-write style address-space duplication.
