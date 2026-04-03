#include "vibeos/object.h"

static int handle_alloc_internal(vibeos_handle_table_t *table, uint32_t rights, vibeos_object_type_t object_type, uint32_t object_id, uint32_t *out_handle) {
    uint32_t i;
    if (!table || !out_handle) {
        return -1;
    }
    for (i = 0; i < VIBEOS_MAX_HANDLES; i++) {
        if (!table->entries[i].in_use) {
            table->entries[i].in_use = 1;
            table->entries[i].id = table->next_id++;
            table->entries[i].rights = rights;
            table->entries[i].object_type = (uint32_t)object_type;
            table->entries[i].object_id = object_id;
            *out_handle = table->entries[i].id;
            return 0;
        }
    }
    return -1;
}

int vibeos_handle_table_init(vibeos_handle_table_t *table) {
    uint32_t i;
    if (!table) {
        return -1;
    }
    table->next_id = 1;
    for (i = 0; i < VIBEOS_MAX_HANDLES; i++) {
        table->entries[i].id = 0;
        table->entries[i].rights = 0;
        table->entries[i].in_use = 0;
        table->entries[i].object_type = VIBEOS_OBJECT_NONE;
        table->entries[i].object_id = 0;
    }
    return 0;
}

int vibeos_handle_alloc(vibeos_handle_table_t *table, uint32_t rights, uint32_t *out_handle) {
    return handle_alloc_internal(table, rights, VIBEOS_OBJECT_NONE, 0, out_handle);
}

int vibeos_handle_alloc_object(vibeos_handle_table_t *table, uint32_t rights, vibeos_object_type_t object_type, uint32_t object_id, uint32_t *out_handle) {
    if (object_type == VIBEOS_OBJECT_NONE || object_id == 0) {
        return -1;
    }
    return handle_alloc_internal(table, rights, object_type, object_id, out_handle);
}

int vibeos_handle_close(vibeos_handle_table_t *table, uint32_t handle) {
    uint32_t i;
    if (!table || handle == 0) {
        return -1;
    }
    for (i = 0; i < VIBEOS_MAX_HANDLES; i++) {
        if (table->entries[i].in_use && table->entries[i].id == handle) {
            table->entries[i].in_use = 0;
            table->entries[i].id = 0;
            table->entries[i].rights = 0;
            table->entries[i].object_type = VIBEOS_OBJECT_NONE;
            table->entries[i].object_id = 0;
            return 0;
        }
    }
    return -1;
}

int vibeos_handle_rights(const vibeos_handle_table_t *table, uint32_t handle, uint32_t *out_rights) {
    uint32_t i;
    if (!table || !out_rights || handle == 0) {
        return -1;
    }
    for (i = 0; i < VIBEOS_MAX_HANDLES; i++) {
        if (table->entries[i].in_use && table->entries[i].id == handle) {
            *out_rights = table->entries[i].rights;
            return 0;
        }
    }
    return -1;
}

int vibeos_handle_has_rights(const vibeos_handle_table_t *table, uint32_t handle, uint32_t required_rights) {
    uint32_t rights = 0;
    if (vibeos_handle_rights(table, handle, &rights) != 0) {
        return 0;
    }
    return ((rights & required_rights) == required_rights) ? 1 : 0;
}

int vibeos_handle_object(const vibeos_handle_table_t *table, uint32_t handle, vibeos_object_type_t *out_object_type, uint32_t *out_object_id) {
    uint32_t i;
    if (!table || !out_object_type || !out_object_id || handle == 0) {
        return -1;
    }
    for (i = 0; i < VIBEOS_MAX_HANDLES; i++) {
        if (table->entries[i].in_use && table->entries[i].id == handle) {
            *out_object_type = (vibeos_object_type_t)table->entries[i].object_type;
            *out_object_id = table->entries[i].object_id;
            return 0;
        }
    }
    return -1;
}
