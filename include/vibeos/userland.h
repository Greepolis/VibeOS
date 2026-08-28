#ifndef VIBEOS_USERLAND_H
#define VIBEOS_USERLAND_H

#include <stdint.h>

/* Fixed-width records are shared by the native loader, init and diagnostics. */
#define VIBEOS_NATIVE_ABI_MAJOR 1u
#define VIBEOS_NATIVE_ABI_MINOR 0u
#define VIBEOS_NATIVE_PATH_MAX 96u
#define VIBEOS_NATIVE_SERVICE_NAME_MAX 32u
#define VIBEOS_NATIVE_MAX_DEPENDENCIES 16u

typedef enum vibeos_process_exit_reason {
    VIBEOS_PROCESS_EXIT_NORMAL = 0,
    VIBEOS_PROCESS_EXIT_SIGNAL = 1,
    VIBEOS_PROCESS_EXIT_FAULT = 2,
    VIBEOS_PROCESS_EXIT_KILLED = 3,
    VIBEOS_PROCESS_EXIT_TIMEOUT = 4
} vibeos_process_exit_reason_t;

typedef enum vibeos_native_service_state {
    VIBEOS_NATIVE_SERVICE_STOPPED = 0,
    VIBEOS_NATIVE_SERVICE_STARTING = 1,
    VIBEOS_NATIVE_SERVICE_RUNNING = 2,
    VIBEOS_NATIVE_SERVICE_FAILED = 3,
    VIBEOS_NATIVE_SERVICE_STOPPING = 4
} vibeos_native_service_state_t;

typedef enum vibeos_native_restart_policy {
    VIBEOS_NATIVE_RESTART_NEVER = 0,
    VIBEOS_NATIVE_RESTART_ON_FAILURE = 1,
    VIBEOS_NATIVE_RESTART_ALWAYS = 2
} vibeos_native_restart_policy_t;

typedef struct vibeos_process_start_info {
    uint32_t abi_major;
    uint32_t abi_minor;
    uint32_t struct_size;
    uint32_t flags;
    uint32_t parent_pid;
    uint32_t process_group_id;
    uint32_t session_id;
    uint32_t capability_token;
    uint32_t argc;
    uint32_t envc;
    uint64_t argv_ptr;
    uint64_t envp_ptr;
    uint64_t startup_metadata_ptr;
    char image_path[VIBEOS_NATIVE_PATH_MAX];
} vibeos_process_start_info_t;

typedef struct vibeos_service_manifest {
    uint32_t abi_major;
    uint32_t abi_minor;
    uint32_t struct_size;
    uint32_t service_id;
    uint32_t dependency_mask;
    uint32_t flags;
    uint32_t restart_policy;
    uint32_t restart_limit;
    uint32_t startup_timeout_ms;
    uint32_t health_timeout_ms;
    char name[VIBEOS_NATIVE_SERVICE_NAME_MAX];
    char image_path[VIBEOS_NATIVE_PATH_MAX];
} vibeos_service_manifest_t;

typedef struct vibeos_service_runtime_snapshot {
    uint32_t abi_major;
    uint32_t abi_minor;
    uint32_t struct_size;
    uint32_t service_id;
    uint32_t pid;
    uint32_t restart_count;
    uint32_t last_exit_code;
    uint32_t last_exit_reason;
    uint32_t state;
    uint32_t health_failures;
    uint64_t started_at_ticks;
    uint64_t last_transition_ticks;
} vibeos_service_runtime_snapshot_t;

int vibeos_process_start_info_validate(const vibeos_process_start_info_t *info);
int vibeos_service_manifest_validate(const vibeos_service_manifest_t *manifest,
                                     uint32_t manifest_count,
                                     uint32_t index);
int vibeos_service_snapshot_validate(const vibeos_service_runtime_snapshot_t *snapshot);

#endif
