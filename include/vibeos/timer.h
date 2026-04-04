#ifndef VIBEOS_TIMER_H
#define VIBEOS_TIMER_H

#include <stdint.h>

typedef struct vibeos_timer {
    uint64_t ticks;
    uint32_t tick_hz;
    uint64_t last_armed_deadline_tick;
} vibeos_timer_t;

int vibeos_timer_init(vibeos_timer_t *timer, uint32_t tick_hz);
void vibeos_timer_tick(vibeos_timer_t *timer);
uint64_t vibeos_timer_ticks(const vibeos_timer_t *timer);
uint64_t vibeos_timer_ticks_to_ns(const vibeos_timer_t *timer, uint64_t ticks);
uint64_t vibeos_timer_ticks_to_ms(const vibeos_timer_t *timer, uint64_t ticks);
int vibeos_timer_arm_deadline(vibeos_timer_t *timer, uint64_t deadline_tick);
int vibeos_timer_deadline_expired(const vibeos_timer_t *timer, uint64_t now_tick);

#endif
