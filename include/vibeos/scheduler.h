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
    uint64_t waits_timed_out[VIBEOS_MAX_CPUS];
    uint64_t waits_woken[VIBEOS_MAX_CPUS];
} vibeos_scheduler_t;

int vibeos_sched_init(vibeos_scheduler_t *sched, uint32_t cpu_count);
int vibeos_sched_enqueue(vibeos_scheduler_t *sched, vibeos_thread_t *thread);
int vibeos_sched_enqueue_balanced(vibeos_scheduler_t *sched, vibeos_thread_t *thread, uint32_t *out_cpu_id);
vibeos_thread_t *vibeos_sched_next(vibeos_scheduler_t *sched, uint32_t cpu_id);
int vibeos_sched_tick(vibeos_scheduler_t *sched, vibeos_thread_t *running, uint32_t cpu_id);
uint64_t vibeos_sched_preemptions(const vibeos_scheduler_t *sched, uint32_t cpu_id);
int vibeos_sched_note_wait_timeout(vibeos_scheduler_t *sched, uint32_t cpu_id);
int vibeos_sched_note_wait_wake(vibeos_scheduler_t *sched, uint32_t cpu_id);
uint64_t vibeos_sched_wait_timeouts(const vibeos_scheduler_t *sched, uint32_t cpu_id);
uint64_t vibeos_sched_wait_wakes(const vibeos_scheduler_t *sched, uint32_t cpu_id);
size_t vibeos_sched_runqueue_depth(const vibeos_scheduler_t *sched, uint32_t cpu_id);
size_t vibeos_sched_runnable_threads(const vibeos_scheduler_t *sched);
int vibeos_sched_least_loaded_cpu(const vibeos_scheduler_t *sched, uint32_t *out_cpu_id);
int vibeos_sched_cpu_count(const vibeos_scheduler_t *sched, uint32_t *out_cpu_count);
uint64_t vibeos_sched_preemptions_total(const vibeos_scheduler_t *sched);
uint64_t vibeos_sched_wait_timeouts_total(const vibeos_scheduler_t *sched);
uint64_t vibeos_sched_wait_wakes_total(const vibeos_scheduler_t *sched);
int vibeos_sched_counters_reset(vibeos_scheduler_t *sched);
int vibeos_sched_counter_summary(const vibeos_scheduler_t *sched, uint64_t *out_preemptions, uint64_t *out_wait_timeouts, uint64_t *out_wait_wakes, size_t *out_runnable, uint32_t *out_cpu_count);
uint32_t vibeos_sched_default_timeslice(vibeos_thread_class_t klass);
int vibeos_sched_normalize_thread(vibeos_thread_t *thread);

#endif
