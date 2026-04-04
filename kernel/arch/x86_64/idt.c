#include "vibeos/arch_x86_64.h"

int vibeos_x86_64_idt_init(vibeos_x86_64_idt_t *idt) {
    uint32_t i;
    if (!idt) {
        return -1;
    }
    for (i = 0; i < VIBEOS_X86_64_IDT_ENTRIES; i++) {
        idt->present[i] = 0;
    }
    return 0;
}

int vibeos_x86_64_idt_set(vibeos_x86_64_idt_t *idt, uint32_t vector) {
    if (!idt || vector >= VIBEOS_X86_64_IDT_ENTRIES) {
        return -1;
    }
    idt->present[vector] = 1;
    return 0;
}

int vibeos_x86_64_timer_vector(void) {
    return (int)VIBEOS_X86_64_TIMER_IRQ;
}

int vibeos_x86_64_validate_boot_environment(uint32_t feature_flags) {
    uint32_t required = VIBEOS_X86_64_FEATURE_SSE2 | VIBEOS_X86_64_FEATURE_NX;
    return ((feature_flags & required) == required) ? 0 : -1;
}
