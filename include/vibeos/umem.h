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
#include "vibeos/file.h"

/* The first region overlapping [base, base + len), or null. */
vibeos_vma_t *vibeos_umem_region_in(vibeos_procstate_t *ps, uint64_t base, uint64_t len);

/* Does every page of [addr, end) lie in some region? */
int vibeos_umem_covered(vibeos_procstate_t *ps, uint64_t addr, uint64_t end);

/* RLIMIT_AS: may `more` bytes be added to what the process's regions describe? */
int vibeos_umem_as_room(const vibeos_procstate_t *ps, uint64_t more);

/* Address space for `bytes` that nothing occupies, at a multiple of `align` (a
 * power of two, at least a page), or 0. */
uint64_t vibeos_umem_place(vibeos_procstate_t *ps, uint64_t bytes, uint64_t align);

/* The same, inside [lo, hi]: no byte below `lo` or above `hi` - Windows'
 * MEM_ADDRESS_REQUIREMENTS. vibeos_umem_place is this with the whole window. */
uint64_t vibeos_umem_place_in(vibeos_procstate_t *ps, uint64_t bytes, uint64_t align,
                              uint64_t lo, uint64_t hi);

/* Take [addr, end) out of task `me`'s address space: the regions, then the
 * pages. 0, or a negative errno (-ENOMEM when a region could not be split). */
long vibeos_umem_unmap(int me, vibeos_procstate_t *ps, uint64_t addr, uint64_t end);

/* A page a region promises and nothing has made yet: in a region made on first
 * touch (W3), with no entry. mprotect and mlock ask, because "mapped" no
 * longer means "there" - an image's .bss is such a region until it is reached. */
int vibeos_umem_untouched(int me, vibeos_procstate_t *ps, uint64_t va);

/* Make such a page now, as the first touch would: 0 if it is there afterwards
 * (made, or there already, or not a page a region promises), -1 if no frame
 * could be had. */
int vibeos_umem_make(int me, vibeos_procstate_t *ps, uint64_t va);

/* A file on /tmp that nothing names, gone with its last reference; `stem` is
 * its path's prefix ("/tmp/.shm-"). Null with *err set when none could be made.
 * Not under the mm lock: it walks a path. */
vibeos_file_t *vibeos_umem_hidden_file(const char *stem, long *err);

/* Map `len` bytes of file `f` from `off` at [base, base+len) of task `me`, and
 * describe them with a region like `t` (its protection, spelling and
 * allocation; the backing is set here). Shared: the file's own pages, which a
 * store reaches - only a filesystem that keeps its files in pages can give
 * them, -ENODEV otherwise; pages past the file's end are left out. Private:
 * pages of the process's own, filled from the file, zeros past its end. All or
 * nothing: 0, or a negative errno with nothing mapped. Under the mm lock. */
long vibeos_umem_map_file(int me, vibeos_procstate_t *ps, uint64_t base, uint64_t len,
                          vibeos_file_t *f, uint64_t off, int shared, const vibeos_vma_t *t);

/* Give the pages of [addr, end) that exist the protection `prot`: present
 * ones, swapped ones, copy-on-write ones as the page-table layer keeps each.
 * The region list is the caller's to have changed first. */
void vibeos_umem_protect_pages(int me, uint64_t addr, uint64_t end, vibeos_prot_t prot);

#endif
