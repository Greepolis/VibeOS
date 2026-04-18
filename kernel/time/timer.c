#include "vibeos/timer.h"

int vibeos_timer_init(vibeos_timer_t *timer, uint32_t tick_hz) {
    if (!timer || tick_hz == 0) {
        return -1;
    }
    timer->ticks = 0;
    timer->tick_hz = tick_hz;
    timer->last_armed_deadline_tick = 0;
    timer->backend = VIBEOS_TIMER_BACKEND_HOST;
    timer->irq_vector = 0;
    timer->irq_ticks_divider = 1;
    timer->irq_tick_accum = 0;
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

int vibeos_timer_bind_backend(vibeos_timer_t *timer, vibeos_timer_backend_t backend, uint32_t irq_vector, uint32_t irq_ticks_divider) {
    if (!timer) {
        return -1;
    }
    if (backend != VIBEOS_TIMER_BACKEND_HOST && backend != VIBEOS_TIMER_BACKEND_IRQ) {
        return -1;
    }
    if (backend == VIBEOS_TIMER_BACKEND_IRQ && (irq_vector == 0 || irq_ticks_divider == 0)) {
        return -1;
    }
    timer->backend = (uint32_t)backend;
    timer->irq_vector = (backend == VIBEOS_TIMER_BACKEND_IRQ) ? irq_vector : 0;
    timer->irq_ticks_divider = (backend == VIBEOS_TIMER_BACKEND_IRQ) ? irq_ticks_divider : 1;
    timer->irq_tick_accum = 0;
    return 0;
}

int vibeos_timer_on_irq(vibeos_timer_t *timer, uint32_t irq_vector) {
    if (!timer || timer->backend != VIBEOS_TIMER_BACKEND_IRQ || timer->irq_vector == 0) {
        return -1;
    }
    if (irq_vector != timer->irq_vector) {
        return 0;
    }
    timer->irq_tick_accum++;
    if (timer->irq_tick_accum >= timer->irq_ticks_divider) {
        timer->irq_tick_accum = 0;
        vibeos_timer_tick(timer);
        return 1;
    }
    return 0;
}

int vibeos_timer_backend_info(const vibeos_timer_t *timer, vibeos_timer_backend_t *out_backend, uint32_t *out_irq_vector, uint32_t *out_irq_ticks_divider) {
    if (!timer || !out_backend || !out_irq_vector || !out_irq_ticks_divider) {
        return -1;
    }
    *out_backend = (vibeos_timer_backend_t)timer->backend;
    *out_irq_vector = timer->irq_vector;
    *out_irq_ticks_divider = timer->irq_ticks_divider;
    return 0;
}
