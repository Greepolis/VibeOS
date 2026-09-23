/* Host tests for the device registry (C7, kernel/io/device.c). Each check names
 * the property it stands for. */

#include <stdio.h>
#include <string.h>

#include "vibeos/device.h"
#include "vibeos/mbz.h"

int test_device(void);

static int g_locks, g_unlocks, g_held, g_bad;

static void t_lock(void) {
    g_locks++;
    if (g_held != 0) {
        g_bad = 1;
    }
    g_held++;
}

static void t_unlock(void) {
    g_unlocks++;
    if (g_held <= 0) {
        g_bad = 1;
        return;
    }
    g_held--;
}

static int expect(int cond, const char *what) {
    if (!cond) {
        printf("FAIL:device %s\n", what);
    }
    return cond;
}

/* ---- fake devices ------------------------------------------------------------- */

static char g_trace[256];

static void trace(const char *s) {
    size_t n = strlen(g_trace);
    if (n + strlen(s) + 1u < sizeof(g_trace)) {
        strcat(g_trace, s);
    }
}

static vibeos_dev_env_t g_seen_env;
static int g_probe_held = -1;
static int g_selftest_held;

static int probe_ok(const vibeos_dev_env_t *env) {
    g_seen_env = *env;
    g_probe_held = g_held;
    trace("P+");
    return 0;
}

static int probe_absent(const vibeos_dev_env_t *env) {
    (void)env;
    trace("P-");
    return -1;
}

static int irq_a(void) { trace("Ia"); return VIBEOS_DEV_IRQ_INPUT; }
static int irq_b(void) { trace("Ib"); return 0; }
static int irq_c(void) { trace("Ic"); return 0; }
static int irq_v(void) { trace("Iv"); return 0; }

static void selftest_a(void) { g_selftest_held = g_held; trace("Sa"); }
static void selftest_b(void) { trace("Sb"); }

static int report_held;
static void report_a(vibeos_dev_write_fn out, void *ctx) {
    vibeos_devline_t l;
    report_held = g_held;
    trace("Ra");
    vibeos_devline_start(&l, "[A] n=");
    vibeos_devline_hex(&l, 0xABu);
    vibeos_devline_end(&l, out, ctx);
}

static void report_b(vibeos_dev_write_fn out, void *ctx) {
    (void)out;
    (void)ctx;
    trace("Rb");
}

/* A keyboard-like device: a queue of characters, and injection. */
static const char *g_keys;
static int keys_getc(void) {
    if (!g_keys || !*g_keys) {
        return -1;
    }
    return (unsigned char)*g_keys++;
}
static int keys_inject(const char *s) { g_keys = s; return 0; }

/* A second keyboard, later in the table, so order is observable. */
static const char *g_keys2;
static int keys2_getc(void) {
    if (!g_keys2 || !*g_keys2) {
        return -1;
    }
    return (unsigned char)*g_keys2++;
}

static int pointer_seven(int32_t *x, int32_t *y, uint32_t *b) {
    if (x) { *x = 7; }
    if (y) { *y = 8; }
    if (b) { *b = 1; }
    return 0;
}
static int pointer_nine(int32_t *x, int32_t *y, uint32_t *b) {
    if (x) { *x = 9; }
    if (y) { *y = 9; }
    if (b) { *b = 0; }
    return 0;
}

static const vibeos_input_ops_t ops_keys = { keys_getc, keys_inject, 0 };
static const vibeos_input_ops_t ops_keys2 = { keys2_getc, 0, 0 };
static const vibeos_input_ops_t ops_ptr7 = { 0, 0, pointer_seven };
static const vibeos_input_ops_t ops_ptr9 = { 0, 0, pointer_nine };

/* keyboard: no probe (always present), line 1, reports.
 * absent mouse: probe fails, line 12, has a pointer that must not be used.
 * mouse: probe succeeds, line 12, pointer.
 * keyboard2: no probe, line 1, no irq handler.
 * net: not input, no irq, present.
 * disk: no legacy line, a handler for its vector, present. */
static const vibeos_device_t d_kbd = {
    "kbd", VIBEOS_DEV_INPUT, 1, 0, irq_a, selftest_a, report_a, &ops_keys };
static const vibeos_device_t d_absent = {
    "absent-mouse", VIBEOS_DEV_INPUT, 12, probe_absent, irq_b, selftest_b, report_b, &ops_ptr9 };
static const vibeos_device_t d_mouse = {
    "mouse", VIBEOS_DEV_INPUT, 12, probe_ok, irq_c, 0, 0, &ops_ptr7 };
static const vibeos_device_t d_kbd2 = {
    "kbd2", VIBEOS_DEV_INPUT, 1, 0, 0, 0, 0, &ops_keys2 };
static const vibeos_device_t d_net = {
    "net", VIBEOS_DEV_NET, VIBEOS_DEVICE_NO_IRQ, 0, 0, 0, 0, 0 };
/* A PCI-style device: no legacy line, its interrupt arrives on its vector. */
static const vibeos_device_t d_disk = {
    "disk", VIBEOS_DEV_BLOCK, VIBEOS_DEVICE_NO_IRQ, 0, irq_v, 0, 0, 0 };

/* Network devices: one that fails its probe, one missing an operation, one whole. */
static const uint8_t g_mac_a[6] = { 2, 0, 0, 0, 0, 0xA };
static const uint8_t g_mac_b[6] = { 2, 0, 0, 0, 0, 0xB };
static const uint8_t *mac_a(void) { return g_mac_a; }
static const uint8_t *mac_b(void) { return g_mac_b; }
static int net_send(const void *f, uint32_t n) { (void)f; (void)n; return 0; }
static int net_recv(void *o, uint32_t c) { (void)o; (void)c; return 0; }
static const vibeos_net_ops_t ops_net_a = { mac_a, net_send, net_recv };
static const vibeos_net_ops_t ops_net_partial = { mac_b, net_send, 0 };
static const vibeos_net_ops_t ops_net_b = { mac_b, net_send, net_recv };
static const vibeos_device_t d_net_absent = {
    "net-absent", VIBEOS_DEV_NET, VIBEOS_DEVICE_NO_IRQ, probe_absent, 0, 0, 0, &ops_net_a };
static const vibeos_device_t d_net_partial = {
    "net-partial", VIBEOS_DEV_NET, VIBEOS_DEVICE_NO_IRQ, 0, 0, 0, 0, &ops_net_partial };
static const vibeos_device_t d_net_ok = {
    "net-ok", VIBEOS_DEV_NET, VIBEOS_DEVICE_NO_IRQ, probe_ok, 0, 0, 0, &ops_net_b };

static char g_out[512];
static int t_out(void *ctx, const char *line, uint32_t len) {
    (void)ctx;
    if (strlen(g_out) + len + 1u < sizeof(g_out)) {
        strncat(g_out, line, len);
    }
    return 0;
}

int test_device(void) {
    const vibeos_device_t *table[] = { &d_kbd, &d_absent, &d_mouse, &d_kbd2, &d_net, &d_disk };
    const vibeos_device_t *holey[] = { &d_kbd, 0 };
    static const vibeos_device_t *many[VIBEOS_DEVICE_MAX + 1u];
    vibeos_dev_env_t env;
    uint64_t sealed_before;
    int lines[8];
    uint32_t n, i;
    int32_t x = 0, y = 0;
    uint32_t b = 0;

    g_locks = g_unlocks = g_held = g_bad = 0;
    vibeos_device_set_lock(t_lock, t_unlock);
    vibeos_device_reset();

    /* ---- a table the build got wrong is refused whole ----------------------- */
    if (!expect(vibeos_device_set_table(holey, 2u) == -1 && vibeos_device_count() == 0u,
                "a table with an empty entry is refused, not taken in part")) { return -1; }
    for (i = 0; i <= VIBEOS_DEVICE_MAX; i++) {
        many[i] = &d_net;
    }
    if (!expect(vibeos_device_set_table(many, VIBEOS_DEVICE_MAX + 1u) == -1 &&
                vibeos_device_count() == 0u,
                "more drivers than the registry holds are refused, not truncated")) { return -1; }

    /* ---- the table, and who is present before anything is probed -------------- */
    if (!expect(vibeos_device_set_table(table, 6u) == 0 && vibeos_device_count() == 6u,
                "the table is taken")) { return -1; }
    if (!expect(vibeos_device_present(0) && !vibeos_device_present(2) &&
                vibeos_device_present(4),
                "a device with no probe is present from registration; one with a "
                "probe is not until it answers")) { return -1; }

    /* ---- routing needs every declared line, before any probe ------------------ */
    n = vibeos_device_isa_lines(lines, 8u);
    if (!expect(n == 2u && ((lines[0] == 1 && lines[1] == 12) || (lines[0] == 12 && lines[1] == 1)),
                "each declared legacy line once, whether or not its device is present - "
                "a probe raises the line, and an unrouted interrupt is lost")) { return -1; }

    /* ---- the probe ------------------------------------------------------------- */
    memset(&env, 0, sizeof(env));
    env.fb_width = 1280u;
    env.fb_height = 800u;
    g_trace[0] = 0;
    if (!expect(vibeos_device_probe_all(&env) == 5u,
                "five present: two without a probe, the mouse that answered, the net, "
                "the disk")) { return -1; }
    if (!expect(strcmp(g_trace, "P-P+") == 0 && g_seen_env.fb_width == 1280u,
                "every device with a probe is asked, in table order, and sees the "
                "machine's description")) { return -1; }
    if (!expect(g_probe_held == 0,
                "a probe runs with the lock free: the lock masks interrupts, and the "
                "keyboard's probe waits for one to prove its line is routed")) { return -1; }
    if (!expect(!vibeos_device_present(1) && vibeos_device_present(2),
                "a probe that fails leaves its device absent")) { return -1; }

    /* ---- sealed: late registration and a second probe are refused and counted - */
    sealed_before = vibeos_mbz_count(VIBEOS_MBZ_DEVICE_AFTER_SEAL);
    if (!expect(vibeos_device_set_table(table, 1u) == -1 && vibeos_device_count() == 6u,
                "the table cannot be replaced once readers run without a lock")) { return -1; }
    if (!expect(vibeos_device_probe_all(&env) == 0u,
                "a second probe is refused")) { return -1; }
    if (!expect(vibeos_mbz_count(VIBEOS_MBZ_DEVICE_AFTER_SEAL) == sealed_before + 2u,
                "both are must-be-zero hits")) { return -1; }

    /* ---- interrupts ------------------------------------------------------------ */
    g_trace[0] = 0;
    if (!expect(vibeos_device_irq(12) == 0 && strcmp(g_trace, "IbIc") == 0,
                "a line reaches every registered device on it, present or not: a "
                "mouse that failed its probe still has to take its byte off the port")) { return -1; }
    g_trace[0] = 0;
    if (!expect(vibeos_device_irq(1) == VIBEOS_DEV_IRQ_INPUT && strcmp(g_trace, "Ia") == 0,
                "the handlers' flags come back, and a device with no handler is "
                "skipped")) { return -1; }

    /* ---- vectors ------------------------------------------------------------------ */
    if (!expect(g_seen_env.vector == VIBEOS_DEVICE_VECTOR_BASE + 2u,
                "a probe is handed its slot's vector - the mouse is slot 2 - so no "
                "driver picks one by hand")) { return -1; }
    g_trace[0] = 0;
    if (!expect(vibeos_device_irq_vector(VIBEOS_DEVICE_VECTOR_BASE + 5u) == 0 &&
                strcmp(g_trace, "Iv") == 0,
                "a registry vector reaches the device in that slot and no other")) { return -1; }
    g_trace[0] = 0;
    if (!expect(vibeos_device_irq_vector(VIBEOS_DEVICE_VECTOR_BASE + 6u) == -1 &&
                vibeos_device_irq_vector(VIBEOS_DEVICE_VECTOR_BASE + 3u) == -1 &&
                vibeos_device_irq_vector(VIBEOS_DEVICE_VECTOR_BASE + 0u) == -1 &&
                vibeos_device_irq_vector(VIBEOS_DEVICE_VECTOR_BASE - 1u) == -1 &&
                vibeos_device_irq_vector(VIBEOS_DEVICE_VECTOR_BASE + VIBEOS_DEVICE_VECTORS) == -1 &&
                g_trace[0] == 0,
                "a stray says so: past the table, a device with no handler, a device "
                "on a legacy line (its vector is not its interrupt), and either side "
                "of the range - and calls nobody")) { return -1; }
    g_trace[0] = 0;
    if (!expect(vibeos_device_irq(5) == 0 && g_trace[0] == 0,
                "a line nobody declared reaches nobody")) { return -1; }

    /* ---- the report ------------------------------------------------------------ */
    g_trace[0] = 0;
    g_out[0] = 0;
    {
        int before = g_locks;
        vibeos_device_report_all(t_out, 0);
        if (!expect(g_locks == before + 1, "the self-tests run under the module's lock")) { return -1; }
    }
    if (!expect(strcmp(g_trace, "SaSbRaRb") == 0,
                "every self-test first, then every report, for every registered device "
                "- an absent one included: a self-test exercises the driver's logic, and "
                "the mouse's proof ran unconditionally before it was registered")) {
        printf("  trace: %s\n", g_trace);
        return -1;
    }
    if (!expect(g_selftest_held == 1 && report_held == 0,
                "self-tests inside the lock, reports outside it: a report goes to the "
                "serial port")) { return -1; }
    if (!expect(strcmp(g_out, "[A] n=0x00000000000000ab\n") == 0,
                "a report line is whole, hex at sixteen digits, and ends its line")) {
        printf("  got: %s", g_out);
        return -1;
    }

    /* ---- the input class ------------------------------------------------------- */
    g_keys = "hi";
    g_keys2 = "Z";
    if (!expect(vibeos_input_getc() == 'h' && vibeos_input_getc() == 'i' &&
                vibeos_input_getc() == 'Z' && vibeos_input_getc() == -1,
                "getc drains present input devices in table order, then says none")) { return -1; }
    if (!expect(vibeos_input_inject("ok") == 0 && vibeos_input_getc() == 'o',
                "inject goes to the first device that accepts it")) { return -1; }
    if (!expect(vibeos_input_pointer(&x, &y, &b) == 0 && x == 7 && y == 8 && b == 1u,
                "the pointer is the first *present* device's - the absent mouse's "
                "answer is never used")) { return -1; }

    /* ---- the network class ------------------------------------------------------- */
    /* d_net above is present with no operations at all: not an interface. */
    if (!expect(vibeos_net_device() == 0,
                "a network device with no operations is not handed out")) { return -1; }
    vibeos_device_reset();
    {
        const vibeos_device_t *nets[] = { &d_net_absent, &d_net_partial, &d_net_ok };
        const vibeos_net_ops_t *nd;
        if (!expect(vibeos_device_set_table(nets, 3u) == 0 &&
                    vibeos_device_probe_all(&env) == 2u, "three network devices, two present")) {
            return -1;
        }
        nd = vibeos_net_device();
        if (!expect(nd == &ops_net_b && nd->mac()[5] == 0xBu,
                    "the interface is the first present one with every operation - not "
                    "the one that failed its probe (no queues to send on) and not one "
                    "missing recv (a stack that can never receive)")) { return -1; }
    }

    /* ---- nothing registered ------------------------------------------------------ */
    vibeos_device_reset();
    if (!expect(vibeos_input_getc() == -1 && vibeos_input_inject("x") == -1 && vibeos_net_device() == 0 &&
                vibeos_input_pointer(0, 0, 0) == -1 && vibeos_device_irq(1) == 0,
                "with no table every service says so rather than inventing a device")) { return -1; }
    {
        uint64_t before = vibeos_mbz_count(VIBEOS_MBZ_DEVICE_AFTER_SEAL);
        if (!expect(vibeos_device_probe_all(&env) == 0u &&
                    vibeos_mbz_count(VIBEOS_MBZ_DEVICE_AFTER_SEAL) == before + 1u,
                    "a probe before any table is refused and counted")) { return -1; }
    }

    /* ---- a report line that does not fit keeps its newline --------------------- */
    {
        vibeos_devline_t l;
        vibeos_devline_start(&l, "");
        for (i = 0; i < 400u; i++) {
            vibeos_devline_str(&l, "x");
        }
        g_out[0] = 0;
        vibeos_devline_end(&l, t_out, 0);
        if (!expect(l.n == sizeof(l.s) - 1u && l.s[l.n - 1u] == '\n',
                    "a cut line still ends its line")) { return -1; }
    }

    if (!expect(!g_bad && g_held == 0 && g_locks == g_unlocks,
                "every lock released once and never taken twice")) { return -1; }
    vibeos_device_set_lock(0, 0);
    vibeos_device_reset();
    return 0;
}
