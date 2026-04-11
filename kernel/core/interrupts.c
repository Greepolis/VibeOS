#include "vibeos/interrupts.h"
#include "vibeos/timer.h"

static void timer_irq_handler(uint32_t irq, void *ctx) {
    vibeos_timer_t *timer = (vibeos_timer_t *)ctx;
    (void)irq;
    if (timer) {
        vibeos_timer_tick(timer);
    }
}

void vibeos_intc_init(vibeos_interrupt_controller_t *intc) {
    uint32_t i;
    if (!intc) {
        return;
    }
    for (i = 0; i < VIBEOS_MAX_IRQS; i++) {
        intc->handlers[i] = 0;
        intc->contexts[i] = 0;
        intc->irq_count[i] = 0;
        intc->masked[i] = 0;
    }
    intc->enabled = 1;
}

int vibeos_intc_register(vibeos_interrupt_controller_t *intc, uint32_t irq, vibeos_irq_handler_t handler, void *ctx) {
    if (!intc || irq >= VIBEOS_MAX_IRQS || !handler) {
        return -1;
    }
    intc->handlers[irq] = handler;
    intc->contexts[irq] = ctx;
    return 0;
}

int vibeos_intc_dispatch(vibeos_interrupt_controller_t *intc, uint32_t irq) {
    if (!intc || irq >= VIBEOS_MAX_IRQS || !intc->handlers[irq] || !intc->enabled || intc->masked[irq]) {
        return -1;
    }
    intc->irq_count[irq]++;
    intc->handlers[irq](irq, intc->contexts[irq]);
    return 0;
}

uint64_t vibeos_intc_counter(const vibeos_interrupt_controller_t *intc, uint32_t irq) {
    if (!intc || irq >= VIBEOS_MAX_IRQS) {
        return 0;
    }
    return intc->irq_count[irq];
}

int vibeos_intc_mask(vibeos_interrupt_controller_t *intc, uint32_t irq) {
    if (!intc || irq >= VIBEOS_MAX_IRQS) {
        return -1;
    }
    intc->masked[irq] = 1;
    return 0;
}

int vibeos_intc_unmask(vibeos_interrupt_controller_t *intc, uint32_t irq) {
    if (!intc || irq >= VIBEOS_MAX_IRQS) {
        return -1;
    }
    intc->masked[irq] = 0;
    return 0;
}

int vibeos_intc_set_enabled(vibeos_interrupt_controller_t *intc, uint32_t enabled) {
    if (!intc) {
        return -1;
    }
    intc->enabled = enabled ? 1u : 0u;
    return 0;
}

int vibeos_intc_is_masked(const vibeos_interrupt_controller_t *intc, uint32_t irq) {
    if (!intc || irq >= VIBEOS_MAX_IRQS) {
        return -1;
    }
    return intc->masked[irq] ? 1 : 0;
}

int vibeos_intc_bind_timer_irq(vibeos_interrupt_controller_t *intc, struct vibeos_timer *timer, uint32_t irq) {
    if (!intc || !timer) {
        return -1;
    }
    return vibeos_intc_register(intc, irq, timer_irq_handler, timer);
}
