#ifndef VIBEOS_MM_H
#define VIBEOS_MM_H

#include <stddef.h>
#include <stdint.h>

typedef struct vibeos_pmm {
    uintptr_t base;
    size_t size_bytes;
    size_t offset_bytes;
    size_t page_size;
} vibeos_pmm_t;

int vibeos_pmm_init(vibeos_pmm_t *pmm, uintptr_t base, size_t size_bytes, size_t page_size);
void *vibeos_pmm_alloc_page(vibeos_pmm_t *pmm);
size_t vibeos_pmm_remaining(const vibeos_pmm_t *pmm);

#endif
