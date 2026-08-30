# Memory Management: Architecture

*Part of the [memory management rewrite plan](README.md): [architecture](architecture.md) · [observability](observability.md) · [maintainability](maintainability.md) · [phases](phases.md) · [decisions and risks](decisions.md).*
Five layers, each depending only on the one below it.

```
  L4  policy          reclaim, LRU, pressure, eviction order
  L3  backing stores  anonymous | page cache (files) | swap
  L2  regions (VMA)   what a process asked for: base, length, prot, backing
  L1  address spaces  the only writer of page tables; map/unmap/protect/clone
  L0  frames          physical page ownership, counts, state, poison
```

The current code is L1 and L0 tangled together with fragments of L2 implied by
`brk`/`mmap` cursors, and nothing above.

### L0 — Frames

One descriptor per physical frame, in an array indexed by frame number. This is
the structure everything later hangs from, so it is worth getting right now
rather than growing it under pressure.

```c
typedef struct vibeos_frame {
    uint16_t owners;      /* address spaces mapping it; 0 = free */
    uint8_t  state;       /* FREE | ALLOCATED | RESERVED | PAGE_TABLE | CACHE */
    uint8_t  flags;       /* PINNED | DIRTY | REFERENCED | SWAP_BACKED */
    uint32_t backing;     /* index into the backing store, or 0 */
    uint32_t lru_next;    /* reclaim lists, unused until L4 exists */
    uint32_t lru_prev;
} vibeos_frame_t;         /* 16 bytes */
```

At 4 KiB frames that is 16 bytes per 4096, or 0.4% of RAM: 1.7 MiB for the
440 MiB this kernel sees. The current byte-per-frame table costs 107 KiB and
cannot express any of the above.

`state` is what makes the two mistakes of §1 impossible: a page-table page and
an identity-split entry are not the same thing as a user frame, and the layer
knows which it is holding rather than guessing from a hardware bit.

**Interface.**

```c
uint64_t vibeos_frame_alloc(vibeos_frame_state_t state);
void     vibeos_frame_get(uint64_t phys);        /* one more owner */
int      vibeos_frame_put(uint64_t phys);        /* one fewer; non-zero when freed */
uint32_t vibeos_frame_owners(uint64_t phys);
int      vibeos_frame_reserve(uint64_t base, uint64_t len);
```

Rules that are the point of the layer:

- `vibeos_frame_put` on a frame with no owners is an error, reported, and does
  not free. The safe direction is a leak.
- A frame outside the table is never freed. Counted separately.
- Freed frames are poisoned and the poison verified on reuse, as today.
- The free list is private. `hw_free_page` disappears as a public name.

### L1 — Address spaces

The only code that writes a page table entry.

```c
int vibeos_vmspace_create(vibeos_vmspace_t *out);
int vibeos_vmspace_destroy(vibeos_vmspace_t *as);
int vibeos_vmspace_map(vibeos_vmspace_t *as, uint64_t va, uint64_t pa,
                       vibeos_prot_t prot, vibeos_map_flags_t flags);
int vibeos_vmspace_unmap(vibeos_vmspace_t *as, uint64_t va);
int vibeos_vmspace_protect(vibeos_vmspace_t *as, uint64_t va, vibeos_prot_t prot);
int vibeos_vmspace_clone_cow(vibeos_vmspace_t *dst, vibeos_vmspace_t *src);
int vibeos_vmspace_fault(vibeos_vmspace_t *as, uint64_t va, uint32_t error);
```

`map` takes the frame reference. `unmap` and `destroy` release it. Nothing else
calls `vibeos_frame_get`.

**The ownership bit.** x86-64 leaves bits 9-11 of a PTE available to software.
One of them becomes `PTE_OWNED`: set by `vibeos_vmspace_map`, never by anything
else. Teardown releases exactly the entries carrying it. The identity-split
entries do not carry it and are skipped without needing to reason about
`PTE_USER`; a `PROT_NONE` guard page carries it and is released correctly.

This single bit is what closes the defect in §1. It is listed here rather than
applied as a patch because it only works if `map` is the only writer - which is
the rest of L1.

**Two windows, one implementation.** The low-window special case
(splitting the identity map, un-sharing the global tables) becomes an internal
detail of `vibeos_vmspace_map`, chosen by address, rather than a second public
function every caller must remember to pick.

**TLB coherence** belongs here too. `map`, `unmap` and `protect` know whether
they widened or narrowed permissions, which is exactly the information needed to
decide whether a shootdown is required - the caller does not have to know. The
open problem recorded in
[diagnostics.md](implementation_progress/diagnostics.md) - that a synchronous
shootdown stalls because `syscall` masks interrupts - is addressed at the same
place, once, instead of at every call site.

### L2 — Regions

A process's memory as it asked for it, rather than as it happens to be mapped.

```c
typedef struct vibeos_vma {
    uint64_t base, len;
    vibeos_prot_t prot;
    vibeos_backing_t backing;   /* ANON | FILE | SHARED */
    uint32_t backing_id;        /* file/inode handle, or 0 */
    uint64_t backing_offset;
    struct vibeos_vma *next;
} vibeos_vma_t;
```

This is what `munmap` should consult instead of walking page tables and freeing
whatever it finds - the walk is the reason `munmap` was freeing shared frames in
the first place. It is also the prerequisite for everything in L3: a fault can
only be resolved from a backing store if something remembers what backs the
address.

Immediate wins beyond correctness: guard pages become a region with no backing
rather than a mapping trick; `brk` and `mmap` stop being two cursors that could
in principle collide; and `/proc/self/maps` becomes possible.

### L3 — Backing stores

Where a page comes from when it is not there. Three implementations behind one
interface:

```c
typedef struct vibeos_backing_ops {
    int (*fault_in)(void *ctx, uint64_t offset, uint64_t *out_phys);
    int (*write_back)(void *ctx, uint64_t offset, uint64_t phys);
    int (*release)(void *ctx, uint64_t offset);
} vibeos_backing_ops_t;
```

- **Anonymous** - allocate a zeroed frame. Exists today, implicitly.
- **Page cache** - frames holding file contents, keyed by (file, offset).
  A fault either finds the page already resident (**hit**) or reads it from the
  block layer (**miss**). This is also what `execve` should use instead of the
  4 MiB staging buffer it copies through today, and what the existing block
  cache in `kernel/fs/` should be merged into rather than duplicated.
- **Swap** - frames written to a backing device and reclaimed. Needs L4 to
  decide *which* frames, and a swap map to record where each went.

### L4 — Policy

Reclaim. LRU lists (the `lru_next`/`lru_prev` fields exist from L0 so the lists
can be added without touching the frame descriptor again), a watermark, and an
eviction order: clean cache pages first, then dirty cache pages written back,
then anonymous pages to swap. Pinned frames - page tables, DMA buffers, the
kernel's own - are never candidates.

