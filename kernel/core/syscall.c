#include "vibeos/kernel.h"
#include "vibeos/syscall.h"

int64_t vibeos_syscall_dispatch(struct vibeos_kernel *kernel, vibeos_syscall_frame_t *frame) {
    if (!kernel || !frame) {
        return -1;
    }

    switch ((vibeos_syscall_id_t)frame->id) {
        case VIBEOS_SYSCALL_NOP:
            frame->result = 0;
            return 0;
        case VIBEOS_SYSCALL_EVENT_SIGNAL:
            vibeos_event_signal(&kernel->boot_event);
            frame->result = 0;
            return 0;
        case VIBEOS_SYSCALL_EVENT_CLEAR:
            vibeos_event_clear(&kernel->boot_event);
            frame->result = 0;
            return 0;
        case VIBEOS_SYSCALL_HANDLE_ALLOC:
        {
            uint32_t handle;
            if (vibeos_handle_alloc(&kernel->handles, (uint32_t)frame->arg0, &handle) != 0) {
                frame->result = -1;
                return -1;
            }
            frame->result = (int64_t)handle;
            return 0;
        }
        case VIBEOS_SYSCALL_HANDLE_CLOSE:
            if (vibeos_handle_close(&kernel->handles, (uint32_t)frame->arg0) != 0) {
                frame->result = -1;
                return -1;
            }
            frame->result = 0;
            return 0;
        case VIBEOS_SYSCALL_PROCESS_SPAWN:
        {
            uint32_t pid;
            if (vibeos_proc_spawn(&kernel->proc_table, (uint32_t)frame->arg0, &pid) != 0) {
                frame->result = -1;
                return -1;
            }
            frame->result = (int64_t)pid;
            return 0;
        }
        case VIBEOS_SYSCALL_THREAD_CREATE:
        {
            uint32_t tid;
            if (vibeos_thread_create(&kernel->proc_table, (uint32_t)frame->arg0, &tid) != 0) {
                frame->result = -1;
                return -1;
            }
            frame->result = (int64_t)tid;
            return 0;
        }
        default:
            frame->result = -1;
            return -1;
    }
}
