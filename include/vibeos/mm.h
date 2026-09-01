#ifndef VIBEOS_MM_H
#define VIBEOS_MM_H

#include <stddef.h>
#include <stdint.h>

#include "vibeos/boot.h"

typedef struct vibeos_pmm {
    uintptr_t base;
    size_t size_bytes;
    size_t offset_bytes;
    size_t page_size;
} vibeos_pmm_t;

int vibeos_pmm_init(vibeos_pmm_t *pmm, uintptr_t base, size_t size_bytes, size_t page_size);
int vibeos_pmm_pick_usable_region(const vibeos_boot_info_t *boot_info, size_t page_size, uintptr_t *out_base, size_t *out_size);
int vibeos_pmm_init_from_boot_info(vibeos_pmm_t *pmm, const vibeos_boot_info_t *boot_info, size_t page_size);
/* Take a physical range out of the allocator.
 *
 * This exists because a virtual address can be claimed by a process while the
 * same physical address is in use by the kernel. The kernel reaches its own
 * memory through an identity map, so if a process maps a user page at virtual
 * 0x400000 - where Linux links its executables - then while that process is
 * current, the kernel's identity view of physical 0x400000 is gone, replaced
 * by the user's page. If anything of the kernel's happened to live there, it
 * is silently reading and writing user memory instead.
 *
 * Reserving the range makes that impossible rather than unlikely.
 *
 * Must be called before the first allocation. The allocator is a bump
 * allocator over one region, so a reservation that splits the region keeps
 * the larger side; a reservation covering all of it fails. */
int vibeos_pmm_reserve(vibeos_pmm_t *pmm, uintptr_t base, size_t size);

void *vibeos_pmm_alloc_page(vibeos_pmm_t *pmm);
void *vibeos_pmm_alloc_pages(vibeos_pmm_t *pmm, size_t page_count);
size_t vibeos_pmm_allocated_pages(const vibeos_pmm_t *pmm);
size_t vibeos_pmm_remaining(const vibeos_pmm_t *pmm);

#endif
