#include "vibeos/mm.h"

int vibeos_pmm_init(vibeos_pmm_t *pmm, uintptr_t base, size_t size_bytes, size_t page_size) {
    if (!pmm || size_bytes == 0 || page_size == 0) {
        return -1;
    }
    pmm->base = base;
    pmm->size_bytes = size_bytes;
    pmm->offset_bytes = 0;
    pmm->page_size = page_size;
    return 0;
}

void *vibeos_pmm_alloc_page(vibeos_pmm_t *pmm) {
    uintptr_t addr;
    if (!pmm || pmm->offset_bytes + pmm->page_size > pmm->size_bytes) {
        return 0;
    }
    addr = pmm->base + pmm->offset_bytes;
    pmm->offset_bytes += pmm->page_size;
    return (void *)addr;
}

size_t vibeos_pmm_remaining(const vibeos_pmm_t *pmm) {
    if (!pmm || pmm->offset_bytes > pmm->size_bytes) {
        return 0;
    }
    return pmm->size_bytes - pmm->offset_bytes;
}
