#include "vibeos/vm.h"

static int map_overlap(const vibeos_vm_map_t *a, const vibeos_vm_map_t *b) {
    uintptr_t a_end = 0;
    uintptr_t b_end = 0;
    if (!a || !b || a->size == 0 || b->size == 0) {
        return 0;
    }
    if (a->va > UINTPTR_MAX - a->size || b->va > UINTPTR_MAX - b->size) {
        return 1;
    }
    a_end = a->va + a->size;
    b_end = b->va + b->size;
    return (a->va < b_end && b->va < a_end) ? 1 : 0;
}

static int map_is_valid(const vibeos_vm_map_t *map) {
    if (!map || map->size == 0) {
        return 0;
    }
    if (map->va > UINTPTR_MAX - map->size) {
        return 0;
    }
    if (map->pa > UINTPTR_MAX - map->size) {
        return 0;
    }
    return 1;
}

int vibeos_vm_init(vibeos_address_space_t *aspace) {
    if (!aspace) {
        return -1;
    }
    aspace->map_count = 0;
    return 0;
}

int vibeos_vm_map(vibeos_address_space_t *aspace, uintptr_t va, uintptr_t pa, size_t size, uint32_t perms) {
    size_t i;
    vibeos_vm_map_t candidate;
    if (!aspace || size == 0 || aspace->map_count >= VIBEOS_VM_MAX_MAPS) {
        return -1;
    }
    candidate.va = va;
    candidate.pa = pa;
    candidate.size = size;
    candidate.perms = perms;
    if (!map_is_valid(&candidate)) {
        return -1;
    }

    for (i = 0; i < aspace->map_count; i++) {
        if (map_overlap(&candidate, &aspace->maps[i])) {
            return -1;
        }
    }
    aspace->maps[aspace->map_count++] = candidate;
    return 0;
}

int vibeos_vm_unmap(vibeos_address_space_t *aspace, uintptr_t va, size_t size) {
    size_t i;
    if (!aspace || size == 0) {
        return -1;
    }
    for (i = 0; i < aspace->map_count; i++) {
        if (aspace->maps[i].va == va && aspace->maps[i].size == size) {
            aspace->maps[i] = aspace->maps[aspace->map_count - 1];
            aspace->map_count--;
            return 0;
        }
    }
    return -1;
}

int vibeos_vm_unmap_range(vibeos_address_space_t *aspace, uintptr_t va, size_t size) {
    size_t i;
    if (!aspace || size == 0) {
        return -1;
    }
    for (i = 0; i < aspace->map_count; i++) {
        uintptr_t start = aspace->maps[i].va;
        uintptr_t end = start + aspace->maps[i].size;
        uintptr_t cut_start = va;
        uintptr_t cut_end = 0;
        if (va > UINTPTR_MAX - size) {
            return -1;
        }
        cut_end = va + size;
        if (!(cut_start < end && start < cut_end)) {
            continue;
        }
        if (cut_start <= start && cut_end >= end) {
            aspace->maps[i] = aspace->maps[aspace->map_count - 1];
            aspace->map_count--;
            return 0;
        }
        if (cut_start <= start && cut_end < end) {
            aspace->maps[i].va = cut_end;
            aspace->maps[i].pa += (cut_end - start);
            aspace->maps[i].size = end - cut_end;
            return 0;
        }
        if (cut_start > start && cut_end >= end) {
            aspace->maps[i].size = cut_start - start;
            return 0;
        }
        if (aspace->map_count >= VIBEOS_VM_MAX_MAPS) {
            return -1;
        }
        aspace->maps[aspace->map_count] = aspace->maps[i];
        aspace->maps[aspace->map_count].va = cut_end;
        aspace->maps[aspace->map_count].pa += (cut_end - start);
        aspace->maps[aspace->map_count].size = end - cut_end;
        aspace->maps[i].size = cut_start - start;
        aspace->map_count++;
        return 0;
    }
    return -1;
}

int vibeos_vm_protect(vibeos_address_space_t *aspace, uintptr_t va, size_t size, uint32_t perms) {
    size_t i;
    if (!aspace || size == 0) {
        return -1;
    }
    for (i = 0; i < aspace->map_count; i++) {
        if (aspace->maps[i].va == va && aspace->maps[i].size == size) {
            aspace->maps[i].perms = perms;
            return 0;
        }
    }
    return -1;
}

const vibeos_vm_map_t *vibeos_vm_lookup(const vibeos_address_space_t *aspace, uintptr_t va) {
    size_t i;
    if (!aspace) {
        return 0;
    }
    for (i = 0; i < aspace->map_count; i++) {
        uintptr_t start = aspace->maps[i].va;
        uintptr_t end = 0;
        if (!map_is_valid(&aspace->maps[i])) {
            continue;
        }
        end = start + aspace->maps[i].size;
        if (va >= start && va < end) {
            return &aspace->maps[i];
        }
    }
    return 0;
}

size_t vibeos_vm_map_count(const vibeos_address_space_t *aspace) {
    if (!aspace) {
        return 0;
    }
    return aspace->map_count;
}

size_t vibeos_vm_total_mapped(const vibeos_address_space_t *aspace) {
    size_t i;
    size_t total = 0;
    if (!aspace) {
        return 0;
    }
    for (i = 0; i < aspace->map_count; i++) {
        total += aspace->maps[i].size;
    }
    return total;
}

int vibeos_vm_clone_readonly(vibeos_address_space_t *dst, const vibeos_address_space_t *src) {
    size_t i;
    if (!dst || !src || src->map_count > VIBEOS_VM_MAX_MAPS) {
        return -1;
    }
    dst->map_count = src->map_count;
    for (i = 0; i < src->map_count; i++) {
        dst->maps[i] = src->maps[i];
        dst->maps[i].perms &= ~(uint32_t)VIBEOS_VM_PERM_WRITE;
    }
    return 0;
}

int vibeos_vm_validate(const vibeos_address_space_t *aspace) {
    size_t i;
    size_t j;
    if (!aspace || aspace->map_count > VIBEOS_VM_MAX_MAPS) {
        return -1;
    }
    for (i = 0; i < aspace->map_count; i++) {
        if (!map_is_valid(&aspace->maps[i])) {
            return -1;
        }
        for (j = i + 1; j < aspace->map_count; j++) {
            if (!map_is_valid(&aspace->maps[j])) {
                return -1;
            }
            if (map_overlap(&aspace->maps[i], &aspace->maps[j])) {
                return -1;
            }
        }
    }
    return 0;
}

int vibeos_vm_compact(vibeos_address_space_t *aspace, uint32_t *out_merged) {
    size_t i;
    size_t j;
    uint32_t merged = 0;
    if (!aspace || !out_merged) {
        return -1;
    }
    if (vibeos_vm_validate(aspace) != 0) {
        return -1;
    }
    for (i = 0; i < aspace->map_count; i++) {
        uintptr_t i_end;
        if (!map_is_valid(&aspace->maps[i])) {
            return -1;
        }
        i_end = aspace->maps[i].va + aspace->maps[i].size;
        for (j = i + 1; j < aspace->map_count; j++) {
            uintptr_t i_pa_end;
            if (!map_is_valid(&aspace->maps[j])) {
                return -1;
            }
            i_pa_end = aspace->maps[i].pa + aspace->maps[i].size;
            if (aspace->maps[i].perms == aspace->maps[j].perms &&
                i_end == aspace->maps[j].va &&
                i_pa_end == aspace->maps[j].pa) {
                aspace->maps[i].size += aspace->maps[j].size;
                aspace->maps[j] = aspace->maps[aspace->map_count - 1];
                aspace->map_count--;
                merged++;
                if (i > 0) {
                    i--;
                }
                break;
            }
        }
    }
    *out_merged = merged;
    return 0;
}
