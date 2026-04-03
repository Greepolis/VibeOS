#ifndef VIBEOS_OBJECT_H
#define VIBEOS_OBJECT_H

#include <stdint.h>

#define VIBEOS_MAX_HANDLES 1024u
#define VIBEOS_HANDLE_RIGHT_READ   (1u << 0)
#define VIBEOS_HANDLE_RIGHT_WRITE  (1u << 1)
#define VIBEOS_HANDLE_RIGHT_SIGNAL (1u << 2)
#define VIBEOS_HANDLE_RIGHT_MANAGE (1u << 3)

typedef enum vibeos_object_type {
    VIBEOS_OBJECT_NONE = 0,
    VIBEOS_OBJECT_PROCESS = 1,
    VIBEOS_OBJECT_THREAD = 2
} vibeos_object_type_t;

typedef struct vibeos_handle_entry {
    uint32_t id;
    uint32_t rights;
    uint32_t in_use;
    uint32_t object_type;
    uint32_t object_id;
} vibeos_handle_entry_t;

typedef struct vibeos_handle_table {
    vibeos_handle_entry_t entries[VIBEOS_MAX_HANDLES];
    uint32_t next_id;
} vibeos_handle_table_t;

int vibeos_handle_table_init(vibeos_handle_table_t *table);
int vibeos_handle_alloc(vibeos_handle_table_t *table, uint32_t rights, uint32_t *out_handle);
int vibeos_handle_alloc_object(vibeos_handle_table_t *table, uint32_t rights, vibeos_object_type_t object_type, uint32_t object_id, uint32_t *out_handle);
int vibeos_handle_close(vibeos_handle_table_t *table, uint32_t handle);
int vibeos_handle_rights(const vibeos_handle_table_t *table, uint32_t handle, uint32_t *out_rights);
int vibeos_handle_object(const vibeos_handle_table_t *table, uint32_t handle, vibeos_object_type_t *out_object_type, uint32_t *out_object_id);
int vibeos_handle_has_rights(const vibeos_handle_table_t *table, uint32_t handle, uint32_t required_rights);

#endif
