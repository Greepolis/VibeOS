#ifndef VIBEOS_UMEM_H
#define VIBEOS_UMEM_H

/* A process's address space as every personality changes it
 * (kernel/abi/mem/umem.c). Linux's mmap and munmap and NT's
 * NtAllocateVirtualMemory and NtFreeVirtualMemory ask the same questions -
 * what is in this range, where is there room, may the process have more -
 * and take a range out the same way. They were Linux's own until W3 needed
 * them a second time.
 *
 * Every function here runs under the process's mm lock (ks_mm_lock). */

#include <stdint.h>

#include "vibeos/procstate.h"
#include "vibeos/vma.h"

/* The first region overlapping [base, base + len), or null. */
vibeos_vma_t *vibeos_umem_region_in(vibeos_procstate_t *ps, uint64_t base, uint64_t len);

/* Does every page of [addr, end) lie in some region? */
int vibeos_umem_covered(vibeos_procstate_t *ps, uint64_t addr, uint64_t end);

/* RLIMIT_AS: may `more` bytes be added to what the process's regions describe? */
int vibeos_umem_as_room(const vibeos_procstate_t *ps, uint64_t more);

/* Address space for `bytes` that nothing occupies, at a multiple of `align` (a
 * power of two, at least a page), or 0. */
uint64_t vibeos_umem_place(vibeos_procstate_t *ps, uint64_t bytes, uint64_t align);

/* Take [addr, end) out of task `me`'s address space: the regions, then the
 * pages. 0, or a negative errno (-ENOMEM when a region could not be split). */
long vibeos_umem_unmap(int me, vibeos_procstate_t *ps, uint64_t addr, uint64_t end);

/* Give the pages of [addr, end) that exist the protection `prot`: present
 * ones, swapped ones, copy-on-write ones as the page-table layer keeps each.
 * The region list is the caller's to have changed first. */
void vibeos_umem_protect_pages(int me, uint64_t addr, uint64_t end, vibeos_prot_t prot);

#endif
