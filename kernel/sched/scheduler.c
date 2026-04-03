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
    cpu_id = thread->cpu_hint % sched->cpu_count;
    return runqueue_push(&sched->runqueues[cpu_id], thread);
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
