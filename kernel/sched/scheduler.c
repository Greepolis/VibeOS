#include "vibeos/scheduler.h"

static int runqueue_push(vibeos_runqueue_t *rq, vibeos_thread_t *thread) {
    if (rq->count >= VIBEOS_MAX_THREADS) {
        return -1;
    }
    rq->slots[rq->tail] = thread;
    rq->tail = (rq->tail + 1u) % VIBEOS_MAX_THREADS;
    rq->count++;
    return 0;
}

uint32_t vibeos_sched_default_timeslice(vibeos_thread_class_t klass) {
    switch (klass) {
        case VIBEOS_THREAD_BACKGROUND:
            return 8;
        case VIBEOS_THREAD_NORMAL:
            return 4;
        case VIBEOS_THREAD_INTERACTIVE:
            return 2;
        case VIBEOS_THREAD_REALTIME:
            return 1;
        default:
            return 4;
    }
}

int vibeos_sched_normalize_thread(vibeos_thread_t *thread) {
    if (!thread) {
        return -1;
    }
    if (thread->timeslice_ticks == 0) {
        thread->timeslice_ticks = vibeos_sched_default_timeslice(thread->klass);
    }
    return 0;
}

static vibeos_thread_t *runqueue_pop(vibeos_runqueue_t *rq) {
    vibeos_thread_t *thread;
    if (rq->count == 0) {
        return 0;
    }
    thread = rq->slots[rq->head];
    rq->head = (rq->head + 1u) % VIBEOS_MAX_THREADS;
    rq->count--;
    return thread;
}

int vibeos_sched_init(vibeos_scheduler_t *sched, uint32_t cpu_count) {
    uint32_t i;
    if (!sched || cpu_count == 0 || cpu_count > VIBEOS_MAX_CPUS) {
        return -1;
    }
    sched->cpu_count = cpu_count;
    for (i = 0; i < cpu_count; i++) {
        sched->runqueues[i].head = 0;
        sched->runqueues[i].tail = 0;
        sched->runqueues[i].count = 0;
        sched->preemptions[i] = 0;
        sched->waits_timed_out[i] = 0;
        sched->waits_woken[i] = 0;
    }
    return 0;
}

int vibeos_sched_enqueue(vibeos_scheduler_t *sched, vibeos_thread_t *thread) {
    uint32_t cpu_id;
    if (!sched || !thread || sched->cpu_count == 0) {
        return -1;
    }
    if (vibeos_sched_normalize_thread(thread) != 0) {
        return -1;
    }
    cpu_id = thread->cpu_hint % sched->cpu_count;
    return runqueue_push(&sched->runqueues[cpu_id], thread);
}

int vibeos_sched_enqueue_balanced(vibeos_scheduler_t *sched, vibeos_thread_t *thread, uint32_t *out_cpu_id) {
    uint32_t cpu_id = 0;
    if (!sched || !thread) {
        return -1;
    }
    if (vibeos_sched_normalize_thread(thread) != 0) {
        return -1;
    }
    if (vibeos_sched_least_loaded_cpu(sched, &cpu_id) != 0) {
        return -1;
    }
    thread->cpu_hint = cpu_id;
    if (runqueue_push(&sched->runqueues[cpu_id], thread) != 0) {
        return -1;
    }
    if (out_cpu_id) {
        *out_cpu_id = cpu_id;
    }
    return 0;
}

vibeos_thread_t *vibeos_sched_next(vibeos_scheduler_t *sched, uint32_t cpu_id) {
    if (!sched || cpu_id >= sched->cpu_count) {
        return 0;
    }
    return runqueue_pop(&sched->runqueues[cpu_id]);
}

int vibeos_sched_tick(vibeos_scheduler_t *sched, vibeos_thread_t *running, uint32_t cpu_id) {
    if (!sched || !running || cpu_id >= sched->cpu_count) {
        return -1;
    }
    if (running->timeslice_ticks > 0) {
        running->timeslice_ticks--;
    }
    if (running->timeslice_ticks == 0) {
        sched->preemptions[cpu_id]++;
        return 1;
    }
    return 0;
}

uint64_t vibeos_sched_preemptions(const vibeos_scheduler_t *sched, uint32_t cpu_id) {
    if (!sched || cpu_id >= sched->cpu_count) {
        return 0;
    }
    return sched->preemptions[cpu_id];
}

int vibeos_sched_note_wait_timeout(vibeos_scheduler_t *sched, uint32_t cpu_id) {
    if (!sched || cpu_id >= sched->cpu_count) {
        return -1;
    }
    sched->waits_timed_out[cpu_id]++;
    return 0;
}

int vibeos_sched_note_wait_wake(vibeos_scheduler_t *sched, uint32_t cpu_id) {
    if (!sched || cpu_id >= sched->cpu_count) {
        return -1;
    }
    sched->waits_woken[cpu_id]++;
    return 0;
}

uint64_t vibeos_sched_wait_timeouts(const vibeos_scheduler_t *sched, uint32_t cpu_id) {
    if (!sched || cpu_id >= sched->cpu_count) {
        return 0;
    }
    return sched->waits_timed_out[cpu_id];
}

uint64_t vibeos_sched_wait_wakes(const vibeos_scheduler_t *sched, uint32_t cpu_id) {
    if (!sched || cpu_id >= sched->cpu_count) {
        return 0;
    }
    return sched->waits_woken[cpu_id];
}

size_t vibeos_sched_runqueue_depth(const vibeos_scheduler_t *sched, uint32_t cpu_id) {
    if (!sched || cpu_id >= sched->cpu_count) {
        return 0;
    }
    return sched->runqueues[cpu_id].count;
}

size_t vibeos_sched_runnable_threads(const vibeos_scheduler_t *sched) {
    size_t total = 0;
    uint32_t i;
    if (!sched) {
        return 0;
    }
    for (i = 0; i < sched->cpu_count; i++) {
        total += sched->runqueues[i].count;
    }
    return total;
}

int vibeos_sched_least_loaded_cpu(const vibeos_scheduler_t *sched, uint32_t *out_cpu_id) {
    uint32_t i;
    uint32_t best_cpu = 0;
    size_t best_depth;
    if (!sched || !out_cpu_id || sched->cpu_count == 0) {
        return -1;
    }
    best_depth = sched->runqueues[0].count;
    for (i = 1; i < sched->cpu_count; i++) {
        if (sched->runqueues[i].count < best_depth) {
            best_depth = sched->runqueues[i].count;
            best_cpu = i;
        }
    }
    *out_cpu_id = best_cpu;
    return 0;
}

int vibeos_sched_cpu_count(const vibeos_scheduler_t *sched, uint32_t *out_cpu_count) {
    if (!sched || !out_cpu_count || sched->cpu_count == 0) {
        return -1;
    }
    *out_cpu_count = sched->cpu_count;
    return 0;
}

uint64_t vibeos_sched_preemptions_total(const vibeos_scheduler_t *sched) {
    uint32_t i;
    uint64_t total = 0;
    if (!sched) {
        return 0;
    }
    for (i = 0; i < sched->cpu_count; i++) {
        total += sched->preemptions[i];
    }
    return total;
}

uint64_t vibeos_sched_wait_timeouts_total(const vibeos_scheduler_t *sched) {
    uint32_t i;
    uint64_t total = 0;
    if (!sched) {
        return 0;
    }
    for (i = 0; i < sched->cpu_count; i++) {
        total += sched->waits_timed_out[i];
    }
    return total;
}

uint64_t vibeos_sched_wait_wakes_total(const vibeos_scheduler_t *sched) {
    uint32_t i;
    uint64_t total = 0;
    if (!sched) {
        return 0;
    }
    for (i = 0; i < sched->cpu_count; i++) {
        total += sched->waits_woken[i];
    }
    return total;
}

int vibeos_sched_counters_reset(vibeos_scheduler_t *sched) {
    uint32_t i;
    if (!sched || sched->cpu_count == 0) {
        return -1;
    }
    for (i = 0; i < sched->cpu_count; i++) {
        sched->preemptions[i] = 0;
        sched->waits_timed_out[i] = 0;
        sched->waits_woken[i] = 0;
    }
    return 0;
}

int vibeos_sched_counter_summary(const vibeos_scheduler_t *sched, uint64_t *out_preemptions, uint64_t *out_wait_timeouts, uint64_t *out_wait_wakes, size_t *out_runnable, uint32_t *out_cpu_count) {
    if (!sched || !out_preemptions || !out_wait_timeouts || !out_wait_wakes || !out_runnable || !out_cpu_count || sched->cpu_count == 0) {
        return -1;
    }
    *out_preemptions = vibeos_sched_preemptions_total(sched);
    *out_wait_timeouts = vibeos_sched_wait_timeouts_total(sched);
    *out_wait_wakes = vibeos_sched_wait_wakes_total(sched);
    *out_runnable = vibeos_sched_runnable_threads(sched);
    *out_cpu_count = sched->cpu_count;
    return 0;
}
