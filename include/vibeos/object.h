#ifndef VIBEOS_OBJECT_H
#define VIBEOS_OBJECT_H

#include <stdint.h>

#define VIBEOS_MAX_HANDLES 1024u

typedef struct vibeos_handle_entry {
    uint32_t id;
    uint32_t rights;
    uint32_t in_use;
} vibeos_handle_entry_t;

typedef struct vibeos_handle_table {
    vibeos_handle_entry_t entries[VIBEOS_MAX_HANDLES];
    uint32_t next_id;
} vibeos_handle_table_t;

int vibeos_handle_table_init(vibeos_handle_table_t *table);
int vibeos_handle_alloc(vibeos_handle_table_t *table, uint32_t rights, uint32_t *out_handle);
int vibeos_handle_close(vibeos_handle_table_t *table, uint32_t handle);
int vibeos_handle_rights(const vibeos_handle_table_t *table, uint32_t handle, uint32_t *out_rights);

#endif
