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
    rq->slots[rq->head] = 0;
    rq->head = (rq->head + 1u) % VIBEOS_MAX_THREADS;
    rq->count--;
    return thread;
}

static int runqueue_contains_tid(const vibeos_runqueue_t *rq, uint32_t tid) {
    size_t i;
    size_t idx;
    if (!rq || tid == 0) {
        return 0;
    }
    for (i = 0; i < rq->count; i++) {
        vibeos_thread_t *thread;
        idx = (rq->head + i) % VIBEOS_MAX_THREADS;
        thread = rq->slots[idx];
        if (thread && thread->id == tid) {
            return 1;
        }
    }
    return 0;
}

static int runqueue_remove_tid(vibeos_runqueue_t *rq, uint32_t tid, vibeos_thread_t **out_thread) {
    size_t i;
    size_t read_idx;
    size_t write_idx;
    int removed = 0;
    if (!rq || tid == 0 || rq->count == 0) {
        return -1;
    }
    if (out_thread) {
        *out_thread = 0;
    }
    read_idx = rq->head;
    write_idx = rq->head;
    for (i = 0; i < rq->count; i++) {
        vibeos_thread_t *thread = rq->slots[read_idx];
        read_idx = (read_idx + 1u) % VIBEOS_MAX_THREADS;
        if (!removed && thread && thread->id == tid) {
            removed = 1;
            if (out_thread) {
                *out_thread = thread;
            }
            continue;
        }
        rq->slots[write_idx] = thread;
        write_idx = (write_idx + 1u) % VIBEOS_MAX_THREADS;
    }
    if (!removed) {
        return -1;
    }
    rq->count--;
    rq->tail = (rq->head + rq->count) % VIBEOS_MAX_THREADS;
    rq->slots[rq->tail] = 0;
    return 0;
}

static int sched_find_slot_index(const vibeos_scheduler_t *sched, uint32_t tid, uint32_t *out_index) {
    uint32_t i;
    if (!sched || !out_index || tid == 0) {
        return -1;
    }
    for (i = 0; i < VIBEOS_MAX_THREADS; i++) {
        if (sched->tracked_threads[i].in_use && sched->tracked_threads[i].tid == tid) {
            *out_index = i;
            return 0;
        }
    }
    return -1;
}

static int sched_find_free_slot(vibeos_scheduler_t *sched, uint32_t *out_index) {
    uint32_t i;
    if (!sched || !out_index) {
        return -1;
    }
    for (i = 0; i < VIBEOS_MAX_THREADS; i++) {
        if (!sched->tracked_threads[i].in_use) {
            *out_index = i;
            return 0;
        }
    }
    return -1;
}

static void sched_clear_slot(vibeos_sched_thread_runtime_t *slot) {
    if (!slot) {
        return;
    }
    slot->in_use = 0;
    slot->tid = 0;
    slot->thread = 0;
    slot->cpu_id = 0;
    slot->last_cpu_id = 0;
    slot->state = VIBEOS_SCHED_THREAD_ABSENT;
    slot->enqueue_count = 0;
    slot->dequeue_count = 0;
    slot->wait_begin_count = 0;
    slot->wait_end_count = 0;
    slot->migrations = 0;
}

static void sched_recount_blocked(vibeos_scheduler_t *sched) {
    uint32_t i;
    size_t blocked = 0;
    if (!sched) {
        return;
    }
    for (i = 0; i < VIBEOS_MAX_THREADS; i++) {
        if (sched->tracked_threads[i].in_use && sched->tracked_threads[i].state == VIBEOS_SCHED_THREAD_BLOCKED) {
            blocked++;
        }
    }
    sched->blocked_count = blocked;
}

static int sched_track_slot(vibeos_scheduler_t *sched, vibeos_thread_t *thread, uint32_t cpu_id, uint32_t *out_index) {
    uint32_t idx = 0;
    if (!sched || !thread || thread->id == 0 || sched->cpu_count == 0) {
        return -1;
    }
    if (sched_find_slot_index(sched, thread->id, &idx) == 0) {
        vibeos_sched_thread_runtime_t *slot = &sched->tracked_threads[idx];
        slot->thread = thread;
        if (cpu_id < sched->cpu_count) {
            slot->cpu_id = cpu_id;
            slot->last_cpu_id = cpu_id;
        }
        if (slot->state == VIBEOS_SCHED_THREAD_ABSENT) {
            slot->state = VIBEOS_SCHED_THREAD_RUNNABLE;
        }
        if (out_index) {
            *out_index = idx;
        }
        return 0;
    }
    if (sched_find_free_slot(sched, &idx) != 0) {
        return -1;
    }
    sched->tracked_threads[idx].in_use = 1;
    sched->tracked_threads[idx].tid = thread->id;
    sched->tracked_threads[idx].thread = thread;
    sched->tracked_threads[idx].cpu_id = cpu_id % sched->cpu_count;
    sched->tracked_threads[idx].last_cpu_id = cpu_id % sched->cpu_count;
    sched->tracked_threads[idx].state = VIBEOS_SCHED_THREAD_RUNNABLE;
    sched->tracked_threads[idx].enqueue_count = 0;
    sched->tracked_threads[idx].dequeue_count = 0;
    sched->tracked_threads[idx].wait_begin_count = 0;
    sched->tracked_threads[idx].wait_end_count = 0;
    sched->tracked_threads[idx].migrations = 0;
    sched->tracked_count++;
    if (out_index) {
        *out_index = idx;
    }
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

int vibeos_sched_init(vibeos_scheduler_t *sched, uint32_t cpu_count) {
    uint32_t i;
    if (!sched || cpu_count == 0 || cpu_count > VIBEOS_MAX_CPUS) {
        return -1;
    }
    sched->cpu_count = cpu_count;
    for (i = 0; i < VIBEOS_MAX_CPUS; i++) {
        sched->runqueues[i].head = 0;
        sched->runqueues[i].tail = 0;
        sched->runqueues[i].count = 0;
        sched->preemptions[i] = 0;
        sched->waits_timed_out[i] = 0;
        sched->waits_woken[i] = 0;
    }
    for (i = 0; i < VIBEOS_MAX_THREADS; i++) {
        sched_clear_slot(&sched->tracked_threads[i]);
    }
    sched->tracked_count = 0;
    sched->blocked_count = 0;
    sched->wait_begin_total = 0;
    sched->wait_end_total = 0;
    sched->wait_requeues = 0;
    sched->wait_requeue_failures = 0;
    return 0;
}

int vibeos_sched_track_thread(vibeos_scheduler_t *sched, vibeos_thread_t *thread, uint32_t preferred_cpu_id) {
    if (!sched || !thread || sched->cpu_count == 0) {
        return -1;
    }
    if (vibeos_sched_normalize_thread(thread) != 0) {
        return -1;
    }
    return sched_track_slot(sched, thread, preferred_cpu_id % sched->cpu_count, 0);
}

int vibeos_sched_untrack_thread(vibeos_scheduler_t *sched, uint32_t tid) {
    uint32_t slot_idx = 0;
    uint32_t cpu_id;
    if (!sched || tid == 0) {
        return -1;
    }
    if (sched_find_slot_index(sched, tid, &slot_idx) != 0) {
        return -1;
    }
    for (cpu_id = 0; cpu_id < sched->cpu_count; cpu_id++) {
        vibeos_thread_t *removed = 0;
        (void)runqueue_remove_tid(&sched->runqueues[cpu_id], tid, &removed);
    }
    if (sched->tracked_threads[slot_idx].state == VIBEOS_SCHED_THREAD_BLOCKED && sched->blocked_count > 0) {
        sched->blocked_count--;
    }
    sched_clear_slot(&sched->tracked_threads[slot_idx]);
    if (sched->tracked_count > 0) {
        sched->tracked_count--;
    }
    return 0;
}

int vibeos_sched_enqueue(vibeos_scheduler_t *sched, vibeos_thread_t *thread) {
    uint32_t cpu_id;
    uint32_t slot_idx = 0;
    vibeos_sched_thread_runtime_t *slot;
    if (!sched || !thread || sched->cpu_count == 0) {
        return -1;
    }
    if (vibeos_sched_normalize_thread(thread) != 0) {
        return -1;
    }
    cpu_id = thread->cpu_hint % sched->cpu_count;
    if (sched_track_slot(sched, thread, cpu_id, &slot_idx) != 0) {
        return -1;
    }
    slot = &sched->tracked_threads[slot_idx];
    if (runqueue_contains_tid(&sched->runqueues[cpu_id], thread->id)) {
        if (slot->state == VIBEOS_SCHED_THREAD_BLOCKED && sched->blocked_count > 0) {
            sched->blocked_count--;
        }
        slot->state = VIBEOS_SCHED_THREAD_RUNNABLE;
        slot->cpu_id = cpu_id;
        slot->last_cpu_id = cpu_id;
        return 0;
    }
    if (runqueue_push(&sched->runqueues[cpu_id], thread) != 0) {
        return -1;
    }
    if (slot->state == VIBEOS_SCHED_THREAD_BLOCKED && sched->blocked_count > 0) {
        sched->blocked_count--;
    }
    slot->state = VIBEOS_SCHED_THREAD_RUNNABLE;
    slot->cpu_id = cpu_id;
    slot->last_cpu_id = cpu_id;
    slot->enqueue_count++;
    return 0;
}

int vibeos_sched_enqueue_balanced(vibeos_scheduler_t *sched, vibeos_thread_t *thread, uint32_t *out_cpu_id) {
    uint32_t cpu_id = 0;
    uint32_t slot_idx = 0;
    vibeos_sched_thread_runtime_t *slot;
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
    if (sched_track_slot(sched, thread, cpu_id, &slot_idx) != 0) {
        return -1;
    }
    slot = &sched->tracked_threads[slot_idx];
    if (!runqueue_contains_tid(&sched->runqueues[cpu_id], thread->id)) {
        if (runqueue_push(&sched->runqueues[cpu_id], thread) != 0) {
            return -1;
        }
        slot->enqueue_count++;
    }
    if (slot->state == VIBEOS_SCHED_THREAD_BLOCKED && sched->blocked_count > 0) {
        sched->blocked_count--;
    }
    slot->state = VIBEOS_SCHED_THREAD_RUNNABLE;
    slot->cpu_id = cpu_id;
    slot->last_cpu_id = cpu_id;
    if (out_cpu_id) {
        *out_cpu_id = cpu_id;
    }
    return 0;
}

vibeos_thread_t *vibeos_sched_next(vibeos_scheduler_t *sched, uint32_t cpu_id) {
    vibeos_thread_t *thread;
    uint32_t slot_idx = 0;
    if (!sched || cpu_id >= sched->cpu_count) {
        return 0;
    }
    thread = runqueue_pop(&sched->runqueues[cpu_id]);
    if (!thread) {
        return 0;
    }
    if (sched_find_slot_index(sched, thread->id, &slot_idx) == 0) {
        vibeos_sched_thread_runtime_t *slot = &sched->tracked_threads[slot_idx];
        if (slot->state == VIBEOS_SCHED_THREAD_BLOCKED && sched->blocked_count > 0) {
            sched->blocked_count--;
        }
        slot->state = VIBEOS_SCHED_THREAD_RUNNING;
        slot->cpu_id = cpu_id;
        slot->last_cpu_id = cpu_id;
        slot->dequeue_count++;
    }
    return thread;
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

int vibeos_sched_wait_begin(vibeos_scheduler_t *sched, uint32_t tid, uint32_t *out_cpu_id) {
    uint32_t slot_idx = 0;
    uint32_t prev_cpu = 0;
    uint32_t cpu_id;
    vibeos_sched_thread_runtime_t *slot;
    if (!sched || tid == 0) {
        return -1;
    }
    if (sched_find_slot_index(sched, tid, &slot_idx) != 0) {
        return -1;
    }
    slot = &sched->tracked_threads[slot_idx];
    prev_cpu = slot->cpu_id;
    if (slot->state != VIBEOS_SCHED_THREAD_BLOCKED) {
        if (slot->state == VIBEOS_SCHED_THREAD_RUNNABLE) {
            for (cpu_id = 0; cpu_id < sched->cpu_count; cpu_id++) {
                vibeos_thread_t *removed = 0;
                if (runqueue_remove_tid(&sched->runqueues[cpu_id], tid, &removed) == 0) {
                    prev_cpu = cpu_id;
                    slot->dequeue_count++;
                    break;
                }
            }
        }
        slot->state = VIBEOS_SCHED_THREAD_BLOCKED;
        slot->cpu_id = prev_cpu;
        slot->last_cpu_id = prev_cpu;
        slot->wait_begin_count++;
        sched->wait_begin_total++;
        sched->blocked_count++;
    }
    if (out_cpu_id) {
        *out_cpu_id = prev_cpu;
    }
    return 0;
}

int vibeos_sched_wait_end(vibeos_scheduler_t *sched, uint32_t tid, uint32_t preferred_cpu_id, uint32_t *out_cpu_id) {
    uint32_t slot_idx = 0;
    uint32_t target_cpu = 0;
    uint32_t previous_cpu = 0;
    vibeos_sched_thread_runtime_t *slot;
    if (!sched || tid == 0) {
        return -1;
    }
    if (sched_find_slot_index(sched, tid, &slot_idx) != 0) {
        return -1;
    }
    slot = &sched->tracked_threads[slot_idx];
    if (!slot->thread) {
        return -1;
    }
    if (slot->state != VIBEOS_SCHED_THREAD_BLOCKED) {
        if (out_cpu_id) {
            *out_cpu_id = slot->cpu_id;
        }
        return 0;
    }
    previous_cpu = slot->cpu_id;
    if (preferred_cpu_id < sched->cpu_count) {
        target_cpu = preferred_cpu_id;
    } else if (slot->cpu_id < sched->cpu_count) {
        target_cpu = slot->cpu_id;
    } else if (slot->last_cpu_id < sched->cpu_count) {
        target_cpu = slot->last_cpu_id;
    } else if (vibeos_sched_least_loaded_cpu(sched, &target_cpu) != 0) {
        sched->wait_requeue_failures++;
        return -1;
    }
    if (!runqueue_contains_tid(&sched->runqueues[target_cpu], tid)) {
        if (runqueue_push(&sched->runqueues[target_cpu], slot->thread) != 0) {
            sched->wait_requeue_failures++;
            return -1;
        }
        sched->wait_requeues++;
        slot->enqueue_count++;
    }
    if (previous_cpu < sched->cpu_count && previous_cpu != target_cpu) {
        slot->migrations++;
    }
    slot->state = VIBEOS_SCHED_THREAD_RUNNABLE;
    slot->cpu_id = target_cpu;
    slot->last_cpu_id = target_cpu;
    slot->wait_end_count++;
    sched->wait_end_total++;
    if (sched->blocked_count > 0) {
        sched->blocked_count--;
    }
    if (out_cpu_id) {
        *out_cpu_id = target_cpu;
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

size_t vibeos_sched_tracked_threads(const vibeos_scheduler_t *sched) {
    if (!sched) {
        return 0;
    }
    return sched->tracked_count;
}

size_t vibeos_sched_blocked_threads(const vibeos_scheduler_t *sched) {
    if (!sched) {
        return 0;
    }
    return sched->blocked_count;
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

int vibeos_sched_thread_runtime_get(const vibeos_scheduler_t *sched, uint32_t tid, vibeos_sched_thread_runtime_state_t *out_state, uint32_t *out_cpu_id, uint64_t *out_wait_begin_count, uint64_t *out_wait_end_count, uint64_t *out_migrations) {
    uint32_t slot_idx = 0;
    const vibeos_sched_thread_runtime_t *slot;
    if (!sched || tid == 0) {
        return -1;
    }
    if (sched_find_slot_index(sched, tid, &slot_idx) != 0) {
        return -1;
    }
    slot = &sched->tracked_threads[slot_idx];
    if (out_state) {
        *out_state = slot->state;
    }
    if (out_cpu_id) {
        *out_cpu_id = slot->cpu_id;
    }
    if (out_wait_begin_count) {
        *out_wait_begin_count = slot->wait_begin_count;
    }
    if (out_wait_end_count) {
        *out_wait_end_count = slot->wait_end_count;
    }
    if (out_migrations) {
        *out_migrations = slot->migrations;
    }
    return 0;
}

int vibeos_sched_wait_transition_summary(const vibeos_scheduler_t *sched, uint64_t *out_wait_begin, uint64_t *out_wait_end, uint64_t *out_requeues, uint64_t *out_requeue_failures) {
    if (!sched || !out_wait_begin || !out_wait_end || !out_requeues || !out_requeue_failures) {
        return -1;
    }
    *out_wait_begin = sched->wait_begin_total;
    *out_wait_end = sched->wait_end_total;
    *out_requeues = sched->wait_requeues;
    *out_requeue_failures = sched->wait_requeue_failures;
    return 0;
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
    for (i = 0; i < VIBEOS_MAX_THREADS; i++) {
        if (sched->tracked_threads[i].in_use) {
            sched->tracked_threads[i].enqueue_count = 0;
            sched->tracked_threads[i].dequeue_count = 0;
            sched->tracked_threads[i].wait_begin_count = 0;
            sched->tracked_threads[i].wait_end_count = 0;
            sched->tracked_threads[i].migrations = 0;
        }
    }
    sched->wait_begin_total = 0;
    sched->wait_end_total = 0;
    sched->wait_requeues = 0;
    sched->wait_requeue_failures = 0;
    sched_recount_blocked(sched);
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

int vibeos_sched_age_cpu(vibeos_scheduler_t *sched, uint32_t cpu_id, uint32_t bonus_ticks, uint32_t max_timeslice, uint32_t *out_aged_threads) {
    size_t i;
    size_t idx;
    uint32_t aged = 0;
    vibeos_runqueue_t *rq;
    if (!sched || !out_aged_threads || cpu_id >= sched->cpu_count || bonus_ticks == 0 || max_timeslice == 0) {
        return -1;
    }
    rq = &sched->runqueues[cpu_id];
    for (i = 0; i < rq->count; i++) {
        vibeos_thread_t *thread;
        uint32_t boosted;
        idx = (rq->head + i) % VIBEOS_MAX_THREADS;
        thread = rq->slots[idx];
        if (!thread) {
            continue;
        }
        boosted = thread->timeslice_ticks + bonus_ticks;
        if (boosted < thread->timeslice_ticks || boosted > max_timeslice) {
            boosted = max_timeslice;
        }
        if (boosted != thread->timeslice_ticks) {
            thread->timeslice_ticks = boosted;
            aged++;
        }
    }
    *out_aged_threads = aged;
    return 0;
}

int vibeos_sched_age_all(vibeos_scheduler_t *sched, uint32_t bonus_ticks, uint32_t max_timeslice, uint32_t *out_aged_threads) {
    uint32_t cpu_id;
    uint32_t aged_total = 0;
    if (!sched || !out_aged_threads || bonus_ticks == 0 || max_timeslice == 0) {
        return -1;
    }
    for (cpu_id = 0; cpu_id < sched->cpu_count; cpu_id++) {
        uint32_t aged = 0;
        if (vibeos_sched_age_cpu(sched, cpu_id, bonus_ticks, max_timeslice, &aged) != 0) {
            return -1;
        }
        aged_total += aged;
    }
    *out_aged_threads = aged_total;
    return 0;
}
