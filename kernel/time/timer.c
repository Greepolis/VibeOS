#include "vibeos/timer.h"

int vibeos_timer_init(vibeos_timer_t *timer, uint32_t tick_hz) {
    if (!timer || tick_hz == 0) {
        return -1;
    }
    timer->ticks = 0;
    timer->tick_hz = tick_hz;
    timer->last_armed_deadline_tick = 0;
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

uint64_t vibeos_timer_ticks_to_ns(const vibeos_timer_t *timer, uint64_t ticks) {
    if (!timer || timer->tick_hz == 0) {
        return 0;
    }
    return (ticks * 1000000000ull) / (uint64_t)timer->tick_hz;
}

uint64_t vibeos_timer_ticks_to_ms(const vibeos_timer_t *timer, uint64_t ticks) {
    if (!timer || timer->tick_hz == 0) {
        return 0;
    }
    return (ticks * 1000ull) / (uint64_t)timer->tick_hz;
}

int vibeos_timer_arm_deadline(vibeos_timer_t *timer, uint64_t deadline_tick) {
    if (!timer || deadline_tick == 0) {
        return -1;
    }
    timer->last_armed_deadline_tick = deadline_tick;
    return 0;
}

int vibeos_timer_deadline_expired(const vibeos_timer_t *timer, uint64_t now_tick) {
    if (!timer || timer->last_armed_deadline_tick == 0) {
        return -1;
    }
    return (now_tick >= timer->last_armed_deadline_tick) ? 1 : 0;
}
