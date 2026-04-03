#ifndef VIBEOS_ARCH_X86_64_H
#define VIBEOS_ARCH_X86_64_H

#include <stdint.h>

#define VIBEOS_X86_64_IDT_ENTRIES 256u
#define VIBEOS_X86_64_TIMER_IRQ 32u

typedef struct vibeos_x86_64_idt {
    uint8_t present[VIBEOS_X86_64_IDT_ENTRIES];
} vibeos_x86_64_idt_t;

int vibeos_x86_64_idt_init(vibeos_x86_64_idt_t *idt);
int vibeos_x86_64_idt_set(vibeos_x86_64_idt_t *idt, uint32_t vector);
int vibeos_x86_64_timer_vector(void);

#endif
