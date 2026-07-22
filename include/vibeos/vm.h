#ifndef VIBEOS_VM_H
#define VIBEOS_VM_H

#include <stddef.h>
#include <stdint.h>

#define VIBEOS_VM_MAX_MAPS 256u
#define VIBEOS_VM_PAGE_SIZE 4096u
#define VIBEOS_VM_USER_MIN ((uintptr_t)0x10000ull)
#define VIBEOS_VM_USER_MAX ((uintptr_t)0x00007fffffffffffull)
#define VIBEOS_VM_KERNEL_BASE ((uintptr_t)0xffff800000000000ull)

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

typedef struct vibeos_vm_context {
    vibeos_address_space_t *current;
    uint64_t switch_count;
} vibeos_vm_context_t;

int vibeos_vm_init(vibeos_address_space_t *aspace);
int vibeos_address_space_create(vibeos_address_space_t *aspace);
int vibeos_vm_map(vibeos_address_space_t *aspace, uintptr_t va, uintptr_t pa, size_t size, uint32_t perms);
int vibeos_vm_map_user(vibeos_address_space_t *aspace, uintptr_t va, uintptr_t pa, size_t size, uint32_t perms);
int vibeos_vm_map_kernel(vibeos_address_space_t *aspace, uintptr_t va, uintptr_t pa, size_t size, uint32_t perms);
int vibeos_vm_unmap(vibeos_address_space_t *aspace, uintptr_t va, size_t size);
int vibeos_vm_unmap_range(vibeos_address_space_t *aspace, uintptr_t va, size_t size);
int vibeos_vm_protect(vibeos_address_space_t *aspace, uintptr_t va, size_t size, uint32_t perms);
const vibeos_vm_map_t *vibeos_vm_lookup(const vibeos_address_space_t *aspace, uintptr_t va);
int vibeos_vm_validate_user_range(const vibeos_address_space_t *aspace, uintptr_t va, size_t size, uint32_t required_perms);
size_t vibeos_vm_map_count(const vibeos_address_space_t *aspace);
size_t vibeos_vm_total_mapped(const vibeos_address_space_t *aspace);
int vibeos_vm_clone_readonly(vibeos_address_space_t *dst, const vibeos_address_space_t *src);
int vibeos_vm_validate(const vibeos_address_space_t *aspace);
int vibeos_vm_compact(vibeos_address_space_t *aspace, uint32_t *out_merged);
int vibeos_vm_find_gap(const vibeos_address_space_t *aspace, uintptr_t min_va, size_t size, uintptr_t align, uintptr_t *out_va);
int vibeos_vm_context_init(vibeos_vm_context_t *ctx, vibeos_address_space_t *initial);
int vibeos_vm_switch_address_space(vibeos_vm_context_t *ctx, vibeos_address_space_t *next);

#endif
