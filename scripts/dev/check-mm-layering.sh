#!/usr/bin/env bash
# The layering check: nothing outside kernel/mm/ writes a page-table entry or
# takes a reference on a frame.
#
# This guards the property phase P2 established, and it exists because that
# property is the kind that erodes silently. Every defect this subsystem has
# produced came from a second place deciding, on its own, whether an address
# space owned a frame - and each of those places looked entirely reasonable
# where it was written. A grep run by the build is what stops the sixth one.
#
# Deliberately crude: a check somebody has to think about is a check that gets
# skipped. Adding a legitimate exception means adding it here, in the open,
# with a reason.
set -uo pipefail
cd "$(dirname "$0")/../.."

fail=0

# Page-table entry writes.
#
# Anchored at the start of the line, because `uint64_t *pte = ...` is a
# declaration and flagging it would make this cry wolf on every read of an
# entry. A detector that reports healthy code is one people learn to skip, and
# this project has already built one of those once.
#
# The static bring-up that builds the kernel's own identity map before any
# address space exists writes g_pml4/g_pdpt/g_pd, which are the kernel's tables
# and not a process's; those names are not matched here.
bad="$(grep -rnE '^[[:space:]]*(\*pte|pt\[[a-z]+\]|spt\[[a-z]+\])[[:space:]]*(\||&)?=[^=]' \
        --include=*.c kernel/ user/ 2>/dev/null | grep -v '^kernel/mm/' || true)"
if [ -n "$bad" ]; then
    echo "mm-layering: page-table entries written outside kernel/mm/:"
    echo "$bad" | head -10
    fail=1
fi

# Taking a reference. vibeos_vmspace_map is the only thing that may: the
# reference and the record of it are written together, which is the whole
# repair. Releasing one is still wrapped in the arch layer until the call sites
# are renamed, so only `get` is checked here.
#
# One call outside is legitimate (docs/abi/ L3): hw_tmpfs_page_hold. A shared
# mapping of a file maps the filesystem's own page, and the filesystem has to
# hold it for the mapper from inside its own lock - a truncate on another core
# frees the page the moment that lock is released, and mapping it there would
# mean allocating page tables with interrupts masked. It is a pin, given back as
# soon as the mapping has its own reference, the same shape as the one fork
# takes inside the layer. If a second is ever right, add it here deliberately.
bad="$(grep -rn 'vibeos_frame_get(' --include=*.c kernel/ user/ 2>/dev/null \
      | grep -v '^kernel/mm/' \
      | grep -v '^kernel/arch/x86_64/io_bringup.c:[0-9]*:    vibeos_frame_get((uint64_t)(uintptr_t)p);$' \
      || true)"
if [ "$(grep -c 'vibeos_frame_get(' kernel/arch/x86_64/io_bringup.c)" -gt 1 ]; then
    bad="$bad
kernel/arch/x86_64/io_bringup.c: more than the one hold this check allows"
fi
if [ -n "$bad" ]; then
    echo "mm-layering: a frame reference is taken outside kernel/mm/:"
    echo "$bad" | head -10
    fail=1
fi

# The bootstrap allocator is closed once the frame layer owns the region. A
# call added afterwards hands out frames the frame table believes are free,
# which is how the GUI back buffer came to be rendered over a process's memory
# - with no detector firing, because nothing had been freed early.
#
# Four calls are legitimate and all are inside hw_pmm_bringup: the two staging
# buffers, the descriptor table itself, and the reverse map's node pool - each
# taken before the layer starts. The pool was the fourth and this number was not
# raised with it, so the check sat red and unread; if a fifth is ever right,
# raise it here deliberately rather than discovering it the same way.
bad="$(grep -rnE 'vibeos_pmm_alloc[a-z_]*\(' --include=*.c kernel/ 2>/dev/null \
      | grep -v '^kernel/mm/' || true)"
count="$(printf '%s' "$bad" | grep -c . || true)"
if [ "$count" -gt 4 ]; then
    echo "mm-layering: the bootstrap allocator is called in $count places;"
    echo "  only hw_pmm_bringup may, and only before the frame layer starts:"
    echo "$bad" | head -10
    fail=1
fi

if [ "$fail" -eq 0 ]; then
    echo "mm-layering=ok"
else
    echo "mm-layering=FAIL"
fi
exit "$fail"
