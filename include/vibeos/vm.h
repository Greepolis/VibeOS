#ifndef VIBEOS_VM_H
#define VIBEOS_VM_H

#include <stddef.h>
#include <stdint.h>

#define VIBEOS_VM_MAX_MAPS 256u

typedef enum vibeos_vm_perm {
    VIBEOS_VM_PERM_READ = 1u << 0,
    VIBEOS_VM_PERM_WRITE = 1u << 1,
    VIBEOS_VM_PERM_EXEC = 1u << 2
} vibeos_vm_perm_t;

typedef struct vibeos_vm_map {
    uintptr_t va;
    uintptr_t pa;
    size_t size;
    uint32_t perms;
} vibeos_vm_map_t;

typedef struct vibeos_address_space {
    vibeos_vm_map_t maps[VIBEOS_VM_MAX_MAPS];
    size_t map_count;
} vibeos_address_space_t;

int vibeos_vm_init(vibeos_address_space_t *aspace);
int vibeos_vm_map(vibeos_address_space_t *aspace, uintptr_t va, uintptr_t pa, size_t size, uint32_t perms);
int vibeos_vm_unmap(vibeos_address_space_t *aspace, uintptr_t va, size_t size);
int vibeos_vm_protect(vibeos_address_space_t *aspace, uintptr_t va, size_t size, uint32_t perms);
const vibeos_vm_map_t *vibeos_vm_lookup(const vibeos_address_space_t *aspace, uintptr_t va);

#endif
