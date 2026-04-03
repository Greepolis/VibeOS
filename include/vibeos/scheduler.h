#ifndef VIBEOS_SCHEDULER_H
#define VIBEOS_SCHEDULER_H

#include <stddef.h>
#include <stdint.h>

#define VIBEOS_MAX_CPUS 64u
#define VIBEOS_MAX_THREADS 1024u

typedef enum vibeos_thread_class {
    VIBEOS_THREAD_BACKGROUND = 0,
    VIBEOS_THREAD_NORMAL = 1,
    VIBEOS_THREAD_INTERACTIVE = 2,
    VIBEOS_THREAD_REALTIME = 3
} vibeos_thread_class_t;

typedef struct vibeos_thread {
    uint32_t id;
    uint32_t cpu_hint;
    vibeos_thread_class_t klass;
    uint32_t timeslice_ticks;
} vibeos_thread_t;

typedef struct vibeos_runqueue {
    vibeos_thread_t *slots[VIBEOS_MAX_THREADS];
    size_t head;
    size_t tail;
    size_t count;
} vibeos_runqueue_t;

typedef struct vibeos_scheduler {
    uint32_t cpu_count;
    vibeos_runqueue_t runqueues[VIBEOS_MAX_CPUS];
    uint64_t preemptions[VIBEOS_MAX_CPUS];
} vibeos_scheduler_t;

int vibeos_sched_init(vibeos_scheduler_t *sched, uint32_t cpu_count);
int vibeos_sched_enqueue(vibeos_scheduler_t *sched, vibeos_thread_t *thread);
vibeos_thread_t *vibeos_sched_next(vibeos_scheduler_t *sched, uint32_t cpu_id);
int vibeos_sched_tick(vibeos_scheduler_t *sched, vibeos_thread_t *running, uint32_t cpu_id);
uint64_t vibeos_sched_preemptions(const vibeos_scheduler_t *sched, uint32_t cpu_id);

#endif
