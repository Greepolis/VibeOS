#include "vibeos/userland.h"

static uint32_t native_string_length(const char *s, uint32_t limit) {
    uint32_t i;
    if (!s) {
        return limit;
    }
    for (i = 0; i < limit; i++) {
        if (s[i] == '\0') {
            return i;
        }
    }
    return limit;
}

static int native_abi_version_ok(uint32_t major, uint32_t minor) {
    return major == VIBEOS_NATIVE_ABI_MAJOR && minor <= VIBEOS_NATIVE_ABI_MINOR;
}

int vibeos_process_start_info_validate(const vibeos_process_start_info_t *info) {
    if (!info || !native_abi_version_ok(info->abi_major, info->abi_minor) ||
        info->struct_size < sizeof(*info) || info->argc > 4096u || info->envc > 4096u ||
        info->image_path[0] == '\0' ||
        native_string_length(info->image_path, VIBEOS_NATIVE_PATH_MAX) >= VIBEOS_NATIVE_PATH_MAX) {
        return -1;
    }
    if (info->argv_ptr == 0 || (info->argc != 0 && info->argv_ptr == 0) ||
        (info->envc != 0 && info->envp_ptr == 0)) {
        return -1;
    }
    return 0;
}

int vibeos_service_manifest_validate(const vibeos_service_manifest_t *manifest,
                                     uint32_t manifest_count,
                                     uint32_t index) {
    uint32_t valid_mask;
    if (!manifest || manifest_count == 0 || manifest_count > VIBEOS_NATIVE_MAX_DEPENDENCIES ||
        index >= manifest_count || !native_abi_version_ok(manifest[index].abi_major, manifest[index].abi_minor) ||
        manifest[index].struct_size < sizeof(*manifest) || manifest[index].service_id == 0 ||
        manifest[index].name[0] == '\0' || manifest[index].image_path[0] == '\0' ||
        native_string_length(manifest[index].name, VIBEOS_NATIVE_SERVICE_NAME_MAX) >= VIBEOS_NATIVE_SERVICE_NAME_MAX ||
        native_string_length(manifest[index].image_path, VIBEOS_NATIVE_PATH_MAX) >= VIBEOS_NATIVE_PATH_MAX ||
        manifest[index].restart_policy > VIBEOS_NATIVE_RESTART_ALWAYS) {
        return -1;
    }
    valid_mask = (1u << manifest_count) - 1u;
    if ((manifest[index].dependency_mask & ~valid_mask) != 0 ||
        (manifest[index].dependency_mask & (1u << index)) != 0) {
        return -1;
    }
    return 0;
}

int vibeos_service_snapshot_validate(const vibeos_service_runtime_snapshot_t *snapshot) {
    if (!snapshot || !native_abi_version_ok(snapshot->abi_major, snapshot->abi_minor) ||
        snapshot->struct_size < sizeof(*snapshot) || snapshot->service_id == 0 ||
        snapshot->state > VIBEOS_NATIVE_SERVICE_STOPPING ||
        snapshot->last_exit_reason > VIBEOS_PROCESS_EXIT_TIMEOUT) {
        return -1;
    }
    return 0;
}
