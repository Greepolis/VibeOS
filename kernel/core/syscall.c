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
        default:
            frame->result = -1;
            return -1;
    }
}
