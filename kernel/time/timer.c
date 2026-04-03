#include "vibeos/timer.h"

int vibeos_timer_init(vibeos_timer_t *timer, uint32_t tick_hz) {
    if (!timer || tick_hz == 0) {
        return -1;
    }
    timer->ticks = 0;
    timer->tick_hz = tick_hz;
    return 0;
}

void vibeos_timer_tick(vibeos_timer_t *timer) {
    if (timer) {
        timer->ticks++;
    }
}

uint64_t vibeos_timer_ticks(const vibeos_timer_t *timer) {
    if (!timer) {
        return 0;
    }
    return timer->ticks;
}
