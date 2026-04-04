#ifndef VIBEOS_INTERRUPTS_H
#define VIBEOS_INTERRUPTS_H

#include <stdint.h>

#define VIBEOS_MAX_IRQS 256u

typedef void (*vibeos_irq_handler_t)(uint32_t irq, void *ctx);

typedef struct vibeos_interrupt_controller {
    vibeos_irq_handler_t handlers[VIBEOS_MAX_IRQS];
    void *contexts[VIBEOS_MAX_IRQS];
    uint64_t irq_count[VIBEOS_MAX_IRQS];
    uint8_t masked[VIBEOS_MAX_IRQS];
    uint8_t enabled;
} vibeos_interrupt_controller_t;

void vibeos_intc_init(vibeos_interrupt_controller_t *intc);
int vibeos_intc_register(vibeos_interrupt_controller_t *intc, uint32_t irq, vibeos_irq_handler_t handler, void *ctx);
int vibeos_intc_dispatch(vibeos_interrupt_controller_t *intc, uint32_t irq);
uint64_t vibeos_intc_counter(const vibeos_interrupt_controller_t *intc, uint32_t irq);
int vibeos_intc_mask(vibeos_interrupt_controller_t *intc, uint32_t irq);
int vibeos_intc_unmask(vibeos_interrupt_controller_t *intc, uint32_t irq);
int vibeos_intc_set_enabled(vibeos_interrupt_controller_t *intc, uint32_t enabled);
int vibeos_intc_is_masked(const vibeos_interrupt_controller_t *intc, uint32_t irq);

#endif
