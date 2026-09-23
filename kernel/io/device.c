/* The device registry. See include/vibeos/device.h for what it is for; the
 * comments here are about how. */

#include "vibeos/device.h"
#include "vibeos/mbz.h"

static void (*g_lock)(void);
static void (*g_unlock)(void);

static const vibeos_device_t *g_table[VIBEOS_DEVICE_MAX];
static uint32_t g_count;
static uint8_t g_present[VIBEOS_DEVICE_MAX];
static int g_table_set;
static int g_probing;
static volatile int g_sealed;

static void lock(void) {
    if (g_lock) {
        g_lock();
    }
}

static void unlock(void) {
    if (g_unlock) {
        g_unlock();
    }
}

void vibeos_device_set_lock(void (*l)(void), void (*u)(void)) {
    g_lock = l;
    g_unlock = u;
}

void vibeos_device_reset(void) {
    uint32_t i;

    lock();
    for (i = 0; i < VIBEOS_DEVICE_MAX; i++) {
        g_table[i] = 0;
        g_present[i] = 0;
    }
    g_count = 0;
    g_table_set = 0;
    g_probing = 0;
    g_sealed = 0;
    unlock();
}

int vibeos_device_set_table(const vibeos_device_t *const *table, uint32_t count) {
    uint32_t i;

    lock();
    if (g_table_set || g_sealed || g_probing) {
        unlock();
        vibeos_mbz_hit(VIBEOS_MBZ_DEVICE_AFTER_SEAL, (uint64_t)count);
        return -1;
    }
    /* A table the build produced wrongly - more entries than the registry holds,
     * or a hole - is refused whole rather than taken in part: half the drivers
     * silently missing is the failure a registry exists to make impossible. */
    if (!table || count > VIBEOS_DEVICE_MAX) {
        unlock();
        return -1;
    }
    for (i = 0; i < count; i++) {
        if (!table[i] || !table[i]->name) {
            unlock();
            return -1;
        }
    }
    for (i = 0; i < count; i++) {
        g_table[i] = table[i];
        /* No probe means nothing to find out: present from registration. */
        g_present[i] = table[i]->probe ? 0u : 1u;
    }
    g_count = count;
    g_table_set = 1;
    unlock();
    return 0;
}

uint32_t vibeos_device_probe_all(const vibeos_dev_env_t *env) {
    static const vibeos_dev_env_t none;
    uint32_t i, present = 0;

    lock();
    if (!g_table_set || g_sealed || g_probing) {
        unlock();
        vibeos_mbz_hit(VIBEOS_MBZ_DEVICE_AFTER_SEAL, 0u);
        return 0;
    }
    g_probing = 1;
    unlock();
    /* The probes run outside the lock, and that is not a shortcut. The lock
     * masks interrupts, and a probe may need one to arrive: the keyboard's
     * proves its interrupt path by making the controller raise IRQ1 and waiting
     * for it - on this core, where the line is routed. Under the lock it could
     * never arrive. Boot is single-threaded here, and g_probing refuses a second
     * caller while it runs. */
    for (i = 0; i < g_count; i++) {
        if (g_table[i]->probe) {
            /* Each probe gets the machine's description and its own vector:
             * the slot's, handed out here so no driver chooses one. */
            vibeos_dev_env_t mine = env ? *env : none;
            mine.vector = (i < VIBEOS_DEVICE_VECTORS) ? VIBEOS_DEVICE_VECTOR_BASE + i : 0u;
            g_present[i] = (g_table[i]->probe(&mine) == 0) ? 1u : 0u;
        }
        present += g_present[i];
    }
    /* From here the table and the present flags are only read, and the readers
     * take no lock - an interrupt handler among them. */
    lock();
    __atomic_store_n(&g_sealed, 1, __ATOMIC_RELEASE);
    unlock();
    return present;
}

uint32_t vibeos_device_count(void) {
    return g_count;
}

const vibeos_device_t *vibeos_device_at(uint32_t index) {
    return index < g_count ? g_table[index] : 0;
}

int vibeos_device_present(uint32_t index) {
    return index < g_count ? (int)g_present[index] : 0;
}

uint32_t vibeos_device_isa_lines(int *lines, uint32_t cap) {
    uint32_t i, j, n = 0;

    for (i = 0; i < g_count; i++) {
        int line = g_table[i]->isa_irq;
        int seen = 0;

        if (line < 0) {
            continue;
        }
        for (j = 0; j < n; j++) {
            if (lines[j] == line) {
                seen = 1;
            }
        }
        if (!seen && n < cap) {
            lines[n++] = line;
        }
    }
    return n;
}

int vibeos_device_irq(int line) {
    uint32_t i;
    int flags = 0;

    /* Every registered device on the line, present or not: a PS/2 mouse that
     * failed its probe still has to have its byte taken off the port, or the
     * controller stops delivering. */
    for (i = 0; i < g_count; i++) {
        if (g_table[i]->isa_irq == line && g_table[i]->irq) {
            flags |= g_table[i]->irq();
        }
    }
    return flags;
}

int vibeos_device_irq_vector(uint32_t vector) {
    uint32_t i;

    if (vector < VIBEOS_DEVICE_VECTOR_BASE ||
        vector >= VIBEOS_DEVICE_VECTOR_BASE + VIBEOS_DEVICE_VECTORS) {
        return -1;
    }
    i = vector - VIBEOS_DEVICE_VECTOR_BASE;
    /* Not "present": a disk's probe is its init, which routes the line and then
     * issues commands that complete by interrupt - before the probe has
     * returned and the slot has been marked. A device on a legacy line was
     * handed a vector it does not use, so anything arriving there is a stray. */
    if (i >= g_count || !g_table[i]->irq || g_table[i]->isa_irq >= 0) {
        return -1;
    }
    return g_table[i]->irq();
}

/* ---- locks for drivers ------------------------------------------------------ */

static int (*g_dev_lock_op)(vibeos_dev_lock_t *l);
static void (*g_dev_unlock_op)(vibeos_dev_lock_t *l);

void vibeos_device_set_lock_ops(int (*lock)(vibeos_dev_lock_t *l),
                                void (*unlock)(vibeos_dev_lock_t *l)) {
    g_dev_lock_op = lock;
    g_dev_unlock_op = unlock;
}

int vibeos_dev_lock(vibeos_dev_lock_t *l) {
    return g_dev_lock_op ? g_dev_lock_op(l) : 0;
}

void vibeos_dev_unlock(vibeos_dev_lock_t *l) {
    if (g_dev_unlock_op) {
        g_dev_unlock_op(l);
    }
}

/* Present, and able to do both things a display is asked for. Lock-free, like
 * every other reader: the console writes through this from any core, and from
 * inside interrupts. */
static const vibeos_display_ops_t *display_ops(void) {
    uint32_t i;

    for (i = 0; i < g_count; i++) {
        const vibeos_display_ops_t *op;
        if (g_table[i]->cls != VIBEOS_DEV_DISPLAY || !g_present[i]) {
            continue;
        }
        op = (const vibeos_display_ops_t *)g_table[i]->ops;
        if (op && op->putc && op->tick) {
            return op;
        }
    }
    return 0;
}

int vibeos_display_present(void) {
    return display_ops() != 0;
}

void vibeos_display_putc(char c) {
    const vibeos_display_ops_t *op = display_ops();
    if (op) {
        op->putc(c);
    }
}

void vibeos_display_tick(void) {
    const vibeos_display_ops_t *op = display_ops();
    if (op) {
        op->tick();
    }
}

void vibeos_device_report_all(vibeos_dev_write_fn out, void *ctx) {
    uint32_t i;

    if (!out) {
        return;
    }
    /* Every registered device, present or not. A self-test exercises the
     * driver's own logic - the mouse's feeds its packet decoder - and proves a
     * counter can move whether or not the hardware answered; the mouse's ran
     * unconditionally before it was registered, and a registry that quietly
     * dropped it on a machine with no PS/2 mouse would have dropped the proof
     * with it. A driver with nothing to say when absent says nothing. */
    lock();
    for (i = 0; i < g_count; i++) {
        if (g_table[i]->selftest) {
            g_table[i]->selftest();
        }
    }
    unlock();
    /* The lines outside the lock: the device is the serial port, and a slow
     * device under a lock serialises everybody behind it. */
    for (i = 0; i < g_count; i++) {
        if (g_table[i]->report) {
            g_table[i]->report(out, ctx);
        }
    }
}

/* ---- the input class ---------------------------------------------------- */

static const vibeos_input_ops_t *input_ops(uint32_t i) {
    if (g_table[i]->cls != VIBEOS_DEV_INPUT || !g_present[i]) {
        return 0;
    }
    return (const vibeos_input_ops_t *)g_table[i]->ops;
}

int vibeos_input_getc(void) {
    uint32_t i;

    for (i = 0; i < g_count; i++) {
        const vibeos_input_ops_t *op = input_ops(i);
        if (op && op->getc) {
            int c = op->getc();
            if (c >= 0) {
                return c;
            }
        }
    }
    return -1;
}

int vibeos_input_inject(const char *s) {
    uint32_t i;

    for (i = 0; i < g_count; i++) {
        const vibeos_input_ops_t *op = input_ops(i);
        if (op && op->inject) {
            return op->inject(s);
        }
    }
    return -1;
}

int vibeos_input_pointer(int32_t *x, int32_t *y, uint32_t *buttons) {
    uint32_t i;

    for (i = 0; i < g_count; i++) {
        const vibeos_input_ops_t *op = input_ops(i);
        if (op && op->pointer && op->pointer(x, y, buttons) == 0) {
            return 0;
        }
    }
    return -1;
}

/* ---- the network class ---------------------------------------------------- */

const vibeos_net_ops_t *vibeos_net_device(void) {
    uint32_t i;

    for (i = 0; i < g_count; i++) {
        const vibeos_net_ops_t *op = (const vibeos_net_ops_t *)g_table[i]->ops;
        /* A device that failed its probe has no queues to send on: handing it
         * out would give the stack an interface that swallows every frame. */
        if (g_table[i]->cls == VIBEOS_DEV_NET && g_present[i] &&
            op && op->mac && op->send && op->recv) {
            return op;
        }
    }
    return 0;
}

/* ---- report lines --------------------------------------------------------- */

static void put_c(vibeos_devline_t *l, char c) {
    if (l->n + 1u < sizeof(l->s)) {
        l->s[l->n++] = c;
    }
    l->s[l->n] = 0;
}

void vibeos_devline_str(vibeos_devline_t *l, const char *text) {
    while (text && *text) {
        put_c(l, *text++);
    }
}

void vibeos_devline_start(vibeos_devline_t *l, const char *text) {
    l->n = 0;
    l->s[0] = 0;
    vibeos_devline_str(l, text);
}

void vibeos_devline_hex(vibeos_devline_t *l, uint64_t v) {
    static const char hex[] = "0123456789abcdef";
    int i;

    put_c(l, '0');
    put_c(l, 'x');
    for (i = 15; i >= 0; i--) {
        put_c(l, hex[(v >> (i * 4)) & 0xFu]);
    }
}

void vibeos_devline_end(vibeos_devline_t *l, vibeos_dev_write_fn out, void *ctx) {
    /* The newline is forced in even when the line is full: two lines run
     * together is what the gate's interleaving check calls a defect. */
    if (l->n + 2u > sizeof(l->s)) {
        l->n = (uint32_t)sizeof(l->s) - 2u;
    }
    l->s[l->n++] = '\n';
    l->s[l->n] = 0;
    if (out) {
        (void)out(ctx, l->s, l->n);
    }
}
