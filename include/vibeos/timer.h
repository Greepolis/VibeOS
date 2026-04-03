#ifndef VIBEOS_TIMER_H
#define VIBEOS_TIMER_H

#include <stdint.h>

typedef struct vibeos_timer {
    uint64_t ticks;
    uint32_t tick_hz;
} vibeos_timer_t;

int vibeos_timer_init(vibeos_timer_t *timer, uint32_t tick_hz);
void vibeos_timer_tick(vibeos_timer_t *timer);
uint64_t vibeos_timer_ticks(const vibeos_timer_t *timer);

#endif
