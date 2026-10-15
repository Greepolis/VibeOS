/* RTL_HANDLE_TABLE (docs/abi/windows.md W2): a fixed array of equal-sized entries
 * with a free list, whose entries a program holds by address.
 *
 * Windows reserves address space for MaxHandleCount entries and commits it as
 * the table fills. There is no virtual memory service yet (W3), so the whole
 * reservation is taken from the Rtl arena on first use - never earlier, so a
 * table that is initialised and never used costs nothing - and "committing" is
 * carving the next run of entries onto the free list. The entries never move,
 * which is the property callers depend on.
 *
 * An entry is free while its first word is the address of the next free entry
 * (or null). RTL_HANDLE_ALLOCATED, bit 0 of that word, is set by the CALLER after
 * RtlAllocateHandle returns; until it is, the entry does not count as valid for
 * RtlIsValidIndexHandle or RtlFreeHandle. A free-list pointer is aligned, so the
 * bit is never set by accident. */

#include "rtl_internal.h"

#define COMMIT_RUN 16u

static vibeos_rtl_handle_t *entry_at(const vibeos_rtl_handle_table_t *t, uint32_t i) {
    return (vibeos_rtl_handle_t *)((uint8_t *)t->CommittedHandles + (size_t)i * t->HandleSize);
}

void RtlInitializeHandleTable(uint32_t max_handle_count, uint32_t handle_size,
                              vibeos_rtl_handle_table_t *table) {
    memset(table, 0, sizeof(*table));
    table->MaxHandleCount = max_handle_count;
    table->HandleSize = handle_size;
}

/* Take the next run of the reservation onto the free list. False if the
 * reservation is used up or cannot be made. */
static int commit_more(vibeos_rtl_handle_table_t *t) {
    uint8_t *cur;
    uint32_t run, i;
    size_t total;

    if (t->HandleSize < sizeof(void *) || t->MaxHandleCount == 0) {
        return 0;                    /* an entry must hold the free-list link */
    }
    if (!t->CommittedHandles) {
        total = (size_t)t->MaxHandleCount * t->HandleSize;
        if (total / t->HandleSize != t->MaxHandleCount) {
            return 0;
        }
        t->CommittedHandles = (vibeos_rtl_handle_t *)vibeos_rtl_alloc(total);
        if (!t->CommittedHandles) {
            return 0;
        }
        memset(t->CommittedHandles, 0, total);
        t->UnCommittedHandles = t->CommittedHandles;
        t->MaxReservedHandles = (vibeos_rtl_handle_t *)((uint8_t *)t->CommittedHandles + total);
    }
    cur = (uint8_t *)t->UnCommittedHandles;
    if (cur >= (uint8_t *)t->MaxReservedHandles) {
        return 0;
    }
    run = (uint32_t)(((uint8_t *)t->MaxReservedHandles - cur) / t->HandleSize);
    if (run > COMMIT_RUN) {
        run = COMMIT_RUN;
    }
    /* Chain the run in address order, so the first allocation is the first
     * entry and indices come out 0, 1, 2... as Windows' do. */
    for (i = 0; i < run; i++) {
        vibeos_rtl_handle_t *e = (vibeos_rtl_handle_t *)(cur + (size_t)i * t->HandleSize);

        e->Next = i + 1 < run ? (vibeos_rtl_handle_t *)(cur + (size_t)(i + 1) * t->HandleSize)
                              : t->FreeHandles;
    }
    t->FreeHandles = (vibeos_rtl_handle_t *)cur;
    t->UnCommittedHandles = (vibeos_rtl_handle_t *)(cur + (size_t)run * t->HandleSize);
    return 1;
}

vibeos_rtl_handle_t *RtlAllocateHandle(vibeos_rtl_handle_table_t *table, uint32_t *index) {
    vibeos_rtl_handle_t *e;

    if (!table->FreeHandles && !commit_more(table)) {
        return 0;
    }
    e = table->FreeHandles;
    table->FreeHandles = e->Next;
    e->Next = 0;                     /* the link out, and not yet allocated */
    if (index) {
        *index = (uint32_t)(((uint8_t *)e - (uint8_t *)table->CommittedHandles) / table->HandleSize);
    }
    return e;
}

/* An entry of this table that its owner has marked allocated. */
static int is_valid(const vibeos_rtl_handle_table_t *t, const vibeos_rtl_handle_t *h) {
    const uint8_t *p = (const uint8_t *)h;

    if (!t->CommittedHandles || p < (const uint8_t *)t->CommittedHandles ||
        p >= (const uint8_t *)t->UnCommittedHandles) {
        return 0;
    }
    if ((size_t)(p - (const uint8_t *)t->CommittedHandles) % t->HandleSize != 0) {
        return 0;                    /* inside an entry, not at its start */
    }
    return ((uintptr_t)h->Next & VIBEOS_RTL_HANDLE_ALLOCATED) != 0;
}

vibeos_boolean_t RtlFreeHandle(vibeos_rtl_handle_table_t *table, vibeos_rtl_handle_t *handle) {
    if (!is_valid(table, handle)) {
        return 0;
    }
    /* The next owner finds a clean entry, as the first did. */
    memset(handle, 0, table->HandleSize);
    handle->Next = table->FreeHandles;
    table->FreeHandles = handle;
    return 1;
}

vibeos_boolean_t RtlIsValidIndexHandle(const vibeos_rtl_handle_table_t *table, uint32_t index,
                                       vibeos_rtl_handle_t **handle) {
    vibeos_rtl_handle_t *e;

    if (!table->CommittedHandles || index >= table->MaxHandleCount) {
        return 0;
    }
    e = entry_at(table, index);
    if (!is_valid(table, e)) {
        return 0;
    }
    if (handle) {
        *handle = e;
    }
    return 1;
}

vibeos_ntstatus_t RtlDestroyHandleTable(vibeos_rtl_handle_table_t *table) {
    /* Windows releases the reservation and answers NtFreeVirtualMemory's status. */
    if (table->CommittedHandles) {
        vibeos_rtl_free(table->CommittedHandles);
    }
    table->FreeHandles = 0;
    table->CommittedHandles = 0;
    table->UnCommittedHandles = 0;
    table->MaxReservedHandles = 0;
    return VIBEOS_RTL_ST(VIBEOS_STATUS_SUCCESS);
}
