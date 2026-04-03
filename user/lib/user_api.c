#include "vibeos/kernel.h"
#include "vibeos/syscall.h"
#include "vibeos/user_api.h"

int vibeos_user_context_init(vibeos_user_context_t *ctx, uint32_t pid, uint32_t tid) {
    if (!ctx) {
        return -1;
    }
    ctx->pid = pid;
    ctx->tid = tid;
    return 0;
}

int vibeos_user_signal_boot_event(void *kernel_ptr) {
    vibeos_syscall_frame_t frame;
    vibeos_kernel_t *kernel = (vibeos_kernel_t *)kernel_ptr;
    if (!kernel) {
        return -1;
    }
    frame.id = VIBEOS_SYSCALL_EVENT_SIGNAL;
    frame.arg0 = 0;
    frame.arg1 = 0;
    frame.arg2 = 0;
    frame.result = -1;
    return (int)vibeos_syscall_dispatch(kernel, &frame);
}
