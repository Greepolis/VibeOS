#ifndef VIBEOS_TIMER_H
#define VIBEOS_TIMER_H

#include <stdint.h>

typedef enum vibeos_timer_backend {
    VIBEOS_TIMER_BACKEND_HOST = 0,
    VIBEOS_TIMER_BACKEND_IRQ = 1
} vibeos_timer_backend_t;

typedef struct vibeos_timer {
    uint64_t ticks;
    uint32_t tick_hz;
    uint64_t last_armed_deadline_tick;
    uint32_t backend;
    uint32_t irq_vector;
    uint32_t irq_ticks_divider;
    uint32_t irq_tick_accum;
} vibeos_timer_t;

int vibeos_timer_init(vibeos_timer_t *timer, uint32_t tick_hz);
void vibeos_timer_tick(vibeos_timer_t *timer);
uint64_t vibeos_timer_ticks(const vibeos_timer_t *timer);
uint64_t vibeos_timer_ticks_to_ns(const vibeos_timer_t *timer, uint64_t ticks);
uint64_t vibeos_timer_ticks_to_ms(const vibeos_timer_t *timer, uint64_t ticks);
int vibeos_timer_arm_deadline(vibeos_timer_t *timer, uint64_t deadline_tick);
int vibeos_timer_deadline_expired(const vibeos_timer_t *timer, uint64_t now_tick);
int vibeos_timer_bind_backend(vibeos_timer_t *timer, vibeos_timer_backend_t backend, uint32_t irq_vector, uint32_t irq_ticks_divider);
int vibeos_timer_on_irq(vibeos_timer_t *timer, uint32_t irq_vector);
int vibeos_timer_backend_info(const vibeos_timer_t *timer, vibeos_timer_backend_t *out_backend, uint32_t *out_irq_vector, uint32_t *out_irq_ticks_divider);

#endif
