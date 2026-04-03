#include "vibeos/interrupts.h"

void vibeos_intc_init(vibeos_interrupt_controller_t *intc) {
    uint32_t i;
    if (!intc) {
        return;
    }
    for (i = 0; i < VIBEOS_MAX_IRQS; i++) {
        intc->handlers[i] = 0;
        intc->contexts[i] = 0;
        intc->irq_count[i] = 0;
    }
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
    if (!intc || irq >= VIBEOS_MAX_IRQS || !intc->handlers[irq]) {
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
