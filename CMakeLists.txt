cmake_minimum_required(VERSION 3.21)
project(VibeOS C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(VIBEOS_BUILD_TESTS "Build VibeOS host-side tests" ON)

add_library(vibeos_kernel_core STATIC
    kernel/core/kmain.c
    kernel/mm/pmm.c
    kernel/mm/vm.c
    kernel/object/handle_table.c
    kernel/proc/process.c
    kernel/sched/scheduler.c
    kernel/ipc/event.c
    kernel/ipc/channel.c
    kernel/ipc/waitset.c
    kernel/core/interrupts.c
    kernel/core/policy.c
    kernel/core/security.c
    kernel/core/syscall.c
    kernel/time/timer.c
    kernel/arch/x86_64/trap.c
    kernel/arch/x86_64/idt.c
    kernel/arch/x86_64/boot_stub.c
)

target_include_directories(vibeos_kernel_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_options(vibeos_kernel_core PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

add_library(vibeos_user_core STATIC
    user/init/init_system.c
    user/servicemgr/service_manager.c
    user/servicemgr/service_ipc.c
    user/devmgr/device_manager.c
    user/devmgr/driver_host.c
    user/drivers/driver_framework.c
    user/fs/vfs_service.c
    user/fs/vfs_ops.c
    user/net/network_service.c
    user/net/socket.c
    user/lib/user_api.c
    boot/bootloader_stub.c
)

target_include_directories(vibeos_user_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_options(vibeos_user_core PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

if(VIBEOS_BUILD_TESTS)
    add_executable(vibeos_kernel_tests
        tests/kernel/kernel_tests.c
    )
    target_link_libraries(vibeos_kernel_tests PRIVATE vibeos_kernel_core vibeos_user_core)
    target_include_directories(vibeos_kernel_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
endif()
