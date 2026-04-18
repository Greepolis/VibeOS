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

typedef enum vibeos_handle_lifecycle_event {
    VIBEOS_HANDLE_EVENT_ALLOC = 1,
    VIBEOS_HANDLE_EVENT_CLOSE = 2,
    VIBEOS_HANDLE_EVENT_REVOKE = 3
} vibeos_handle_lifecycle_event_t;

typedef struct vibeos_handle_entry {
    uint32_t id;
    uint32_t rights;
    uint32_t in_use;
    uint32_t object_type;
    uint32_t object_id;
    uint32_t origin_pid;
    uint32_t origin_handle;
} vibeos_handle_entry_t;

typedef void (*vibeos_handle_lifecycle_hook_t)(vibeos_handle_lifecycle_event_t event, const vibeos_handle_entry_t *entry, void *ctx);

typedef struct vibeos_handle_table {
    vibeos_handle_entry_t entries[VIBEOS_MAX_HANDLES];
    uint32_t next_id;
    uint32_t max_handles;
    uint64_t alloc_failures;
    vibeos_handle_lifecycle_hook_t lifecycle_hook;
    void *lifecycle_hook_ctx;
} vibeos_handle_table_t;

int vibeos_handle_table_init(vibeos_handle_table_t *table);
int vibeos_handle_alloc(vibeos_handle_table_t *table, uint32_t rights, uint32_t *out_handle);
int vibeos_handle_alloc_object(vibeos_handle_table_t *table, uint32_t rights, vibeos_object_type_t object_type, uint32_t object_id, uint32_t *out_handle);
int vibeos_handle_close(vibeos_handle_table_t *table, uint32_t handle);
int vibeos_handle_rights(const vibeos_handle_table_t *table, uint32_t handle, uint32_t *out_rights);
int vibeos_handle_object(const vibeos_handle_table_t *table, uint32_t handle, vibeos_object_type_t *out_object_type, uint32_t *out_object_id);
int vibeos_handle_set_provenance(vibeos_handle_table_t *table, uint32_t handle, uint32_t origin_pid, uint32_t origin_handle);
int vibeos_handle_provenance(const vibeos_handle_table_t *table, uint32_t handle, uint32_t *out_origin_pid, uint32_t *out_origin_handle);
int vibeos_handle_has_rights(const vibeos_handle_table_t *table, uint32_t handle, uint32_t required_rights);
int vibeos_handle_set_quota(vibeos_handle_table_t *table, uint32_t max_handles);
int vibeos_handle_stats(const vibeos_handle_table_t *table, uint32_t *out_active_handles, uint32_t *out_max_handles, uint64_t *out_alloc_failures);
int vibeos_handle_count_by_type(const vibeos_handle_table_t *table, vibeos_object_type_t object_type, uint32_t *out_count);
int vibeos_handle_set_lifecycle_hook(vibeos_handle_table_t *table, vibeos_handle_lifecycle_hook_t hook, void *ctx);
int vibeos_handle_revoke_origin(vibeos_handle_table_t *table, uint32_t table_pid, uint32_t root_pid, uint32_t root_handle, vibeos_object_type_t object_type_filter, uint32_t rights_mask_filter, uint32_t *out_revoked);

#endif
