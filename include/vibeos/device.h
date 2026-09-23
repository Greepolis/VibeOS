#ifndef VIBEOS_DEVICE_H
#define VIBEOS_DEVICE_H

#include <stdint.h>

/* C7: devices register; nothing names them.
 *
 * Block devices had a registry (vibeos_blk_register) and it did not buy what a
 * registry is for: adding a driver still meant an init call in arch_hw.c, a
 * declaration in arch_x86_64.h, a line of its counters in kmain.c and an entry in
 * the build - four files, the same as having no registry at all
 * (check-blast-radius.py measured it). The seam was real and the bring-up around
 * it named every driver by hand.
 *
 * Here a driver is a descriptor in its own file, placed in a linker section with
 * VIBEOS_DEVICE(). The core finds every descriptor in the section and does the
 * rest - routes its interrupt line, probes it, dispatches its interrupts, runs
 * its self-test, prints its counters, and serves its class's operations to
 * whoever asks (the console reads a character without knowing it came from a
 * PS/2 keyboard). Adding a driver is the new file and its line in the build.
 *
 * The table is set once, at boot, before any other core runs, and probed once;
 * after that it is sealed and only read. Registering or probing after the seal is
 * refused and counted (VIBEOS_MBZ_DEVICE_AFTER_SEAL) - "read-only after boot" is a
 * property the code checks, not one it hopes for. The reads - interrupt dispatch,
 * the console's getc - take no lock, because they may run in an interrupt and the
 * data they read no longer changes. The two operations that do run late (the
 * report, and the self-tests it runs) take the module's own lock
 * (vibeos_device_set_lock), so two cores printing counters cannot interleave a
 * driver's self-test with itself. */

#define VIBEOS_DEVICE_MAX 32u
#define VIBEOS_DEVICE_NO_IRQ (-1)

typedef enum {
    VIBEOS_DEV_INPUT = 1,
    VIBEOS_DEV_NET,
    VIBEOS_DEV_BLOCK,
    VIBEOS_DEV_DISPLAY,
    VIBEOS_DEV_CHAR
} vibeos_dev_class_t;

/* What a probe may need to know about the machine. Zero where absent. */
typedef struct {
    uint64_t fb_base;
    uint32_t fb_width;
    uint32_t fb_height;
} vibeos_dev_env_t;

/* One complete line, as every other writer in the kernel takes them. */
typedef int (*vibeos_dev_write_fn)(void *ctx, const char *line, uint32_t len);

/* An interrupt handler's answer: whether input became available, so the
 * architecture can wake whatever is blocked reading it. */
#define VIBEOS_DEV_IRQ_INPUT 1

typedef struct {
    int (*getc)(void);                              /* a byte, or -1 when none   */
    int (*inject)(const char *s);                   /* as if typed; may be 0     */
    int (*pointer)(int32_t *x, int32_t *y, uint32_t *buttons);  /* 0 = present; may be 0 */
} vibeos_input_ops_t;

/* A network interface: an Ethernet frame in, an Ethernet frame out. */
typedef struct {
    const uint8_t *(*mac)(void);                    /* six bytes                  */
    int (*send)(const void *frame, uint32_t len);   /* 0 when the device took it  */
    int (*recv)(void *out, uint32_t cap);           /* a frame's length, 0 = none */
} vibeos_net_ops_t;

typedef struct vibeos_device {
    const char *name;
    vibeos_dev_class_t cls;
    /* A legacy (ISA) interrupt line, routed before anything is probed - a probe
     * that talks to its device raises interrupts, and an unrouted one is lost -
     * or VIBEOS_DEVICE_NO_IRQ. */
    int isa_irq;
    /* 0 when the device is there and ready. May be 0: then it is always present. */
    int (*probe)(const vibeos_dev_env_t *env);
    /* Called for every interrupt on isa_irq, present or not: a device that is not
     * ready still has to take its byte off the port. Returns VIBEOS_DEV_IRQ_* flags. */
    int (*irq)(void);
    /* Proves a counter can move (see CLAUDE.md, "a test that cannot fail").
     * Run under the module's lock just before the report. May be 0. */
    void (*selftest)(void);
    /* The driver's counters, as complete lines. May be 0. */
    void (*report)(vibeos_dev_write_fn out, void *ctx);
    /* Class operations: a vibeos_input_ops_t for VIBEOS_DEV_INPUT, a
     * vibeos_net_ops_t for VIBEOS_DEV_NET. */
    const void *ops;
} vibeos_device_t;

/* Place a descriptor in the table. Used in the driver's own file, once. The
 * section is collected by the kernel image's linker script; host tests build
 * their tables by hand and never use this. */
#define VIBEOS_DEVICE(desc) \
    static const vibeos_device_t *const vibeos_device_entry_##desc \
        __attribute__((used, section("vibeos_devices"))) = &(desc)

void vibeos_device_set_lock(void (*lock)(void), void (*unlock)(void));

/* Boot: the table, then the probe, which seals it. Both refuse (and count) a
 * second call. Tests: vibeos_device_reset() first. */
int vibeos_device_set_table(const vibeos_device_t *const *table, uint32_t count);
uint32_t vibeos_device_probe_all(const vibeos_dev_env_t *env);   /* devices present */
void vibeos_device_reset(void);

uint32_t vibeos_device_count(void);
const vibeos_device_t *vibeos_device_at(uint32_t index);
int vibeos_device_present(uint32_t index);

/* Every legacy line some registered device declared, for routing; the count. */
uint32_t vibeos_device_isa_lines(int *lines, uint32_t cap);

/* An interrupt arrived on legacy line `line`. The OR of the handlers' flags. */
int vibeos_device_irq(int line);

/* Self-tests, then reports, of every registered device - present or not; the
 * driver decides what it has to say when absent - in table order. */
void vibeos_device_report_all(vibeos_dev_write_fn out, void *ctx);

/* The input class, served from whichever present devices provide it. */
int vibeos_input_getc(void);
int vibeos_input_inject(const char *s);   /* -1 when nothing accepts injected input */
int vibeos_input_pointer(int32_t *x, int32_t *y, uint32_t *buttons);  /* -1: no pointer */

/* The network class: the first present network device, or 0 when there is none.
 * One interface is what the stack drives today; the registry does not decide
 * that, the caller does. */
const vibeos_net_ops_t *vibeos_net_device(void);

/* Building a report line without a C library: text and 16-digit hex, always
 * terminated, never overrun. */
typedef struct {
    char s[256];
    uint32_t n;
} vibeos_devline_t;

void vibeos_devline_start(vibeos_devline_t *l, const char *text);
void vibeos_devline_str(vibeos_devline_t *l, const char *text);
void vibeos_devline_hex(vibeos_devline_t *l, uint64_t v);       /* "0x" + 16 digits */
void vibeos_devline_end(vibeos_devline_t *l, vibeos_dev_write_fn out, void *ctx);

#endif
