#include "vibeos/interrupts.h"
#include "vibeos/timer.h"

static void timer_irq_handler(uint32_t irq, void *ctx) {
    vibeos_timer_t *timer = (vibeos_timer_t *)ctx;
    if (timer) {
        (void)vibeos_timer_on_irq(timer, irq);
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
    intc->dispatch_denied_bad_irq = 0;
    intc->dispatch_denied_unhandled = 0;
    intc->dispatch_denied_masked = 0;
    intc->dispatch_denied_disabled = 0;
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
    if (!intc) {
        return -1;
    }
    if (irq >= VIBEOS_MAX_IRQS) {
        intc->dispatch_denied_bad_irq++;
        return -1;
    }
    if (!intc->enabled) {
        intc->dispatch_denied_disabled++;
        return -1;
    }
    if (intc->masked[irq]) {
        intc->dispatch_denied_masked++;
        return -1;
    }
    if (!intc->handlers[irq]) {
        intc->dispatch_denied_unhandled++;
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
    if (vibeos_timer_bind_backend(timer, VIBEOS_TIMER_BACKEND_IRQ, irq, 1) != 0) {
        return -1;
    }
    return vibeos_intc_register(intc, irq, timer_irq_handler, timer);
}

int vibeos_intc_denied_counters(const vibeos_interrupt_controller_t *intc, uint64_t *out_bad_irq, uint64_t *out_unhandled, uint64_t *out_masked, uint64_t *out_disabled) {
    if (!intc || !out_bad_irq || !out_unhandled || !out_masked || !out_disabled) {
        return -1;
    }
    *out_bad_irq = intc->dispatch_denied_bad_irq;
    *out_unhandled = intc->dispatch_denied_unhandled;
    *out_masked = intc->dispatch_denied_masked;
    *out_disabled = intc->dispatch_denied_disabled;
    return 0;
}

int vibeos_intc_counters_reset(vibeos_interrupt_controller_t *intc) {
    uint32_t i;
    if (!intc) {
        return -1;
    }
    for (i = 0; i < VIBEOS_MAX_IRQS; i++) {
        intc->irq_count[i] = 0;
    }
    intc->dispatch_denied_bad_irq = 0;
    intc->dispatch_denied_unhandled = 0;
    intc->dispatch_denied_masked = 0;
    intc->dispatch_denied_disabled = 0;
    return 0;
}
