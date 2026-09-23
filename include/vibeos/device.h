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

/* Interrupt vectors the registry hands out, one per table slot: slot i gets
 * VIBEOS_DEVICE_VECTOR_BASE + i, passed to its probe in env->vector. For a
 * device whose interrupt is a PCI line the driver routes itself, rather than a
 * legacy ISA line. Before C7 each driver hard-coded one - AHCI 42, virtio-blk 43,
 * inside the range the legacy lines use - and a second AHCI controller would have
 * had nowhere to go. Slots past the range get vector 0: no interrupt, polled. */
#define VIBEOS_DEVICE_VECTOR_BASE 48u
#define VIBEOS_DEVICE_VECTORS 16u

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
    /* This device's interrupt vector (see VIBEOS_DEVICE_VECTOR_BASE), set by
     * the registry for each probe; 0 when it has none. */
    uint32_t vector;
    /* Memory the machine set aside for a display to compose into, and its
     * size. A display driver has no allocator of its own at probe time. */
    void *fb_back;
    uint64_t fb_back_bytes;
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

/* A display: what the console writes to it, and the timer's repaint. putc is
 * called from the console write path on any core, and must not wait on a
 * screen being drawn. */
typedef struct {
    void (*putc)(char c);
    void (*tick)(void);
} vibeos_display_ops_t;

/* A disk: sectors of 512 bytes. What vibeos_x86_64_blk_bind takes, gathered so
 * the architecture binds every registered disk without naming a driver. */
typedef struct {
    int (*read)(uint64_t lba, void *buf);
    int (*read_many)(uint64_t lba, void *buf, uint32_t sectors);
    int (*write)(uint64_t lba, const void *buf);
    int (*write_many)(uint64_t lba, const void *buf, uint32_t sectors);
    int (*barrier)(void);
    uint64_t (*sectors)(void);
    uint64_t (*timeouts)(void);
} vibeos_block_ops_t;

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
     * vibeos_net_ops_t for VIBEOS_DEV_NET, a vibeos_block_ops_t for
     * VIBEOS_DEV_BLOCK, a vibeos_display_ops_t for VIBEOS_DEV_DISPLAY. */
    const void *ops;
} vibeos_device_t;

/* Place a descriptor in the table. Used in the driver's own file, once. The
 * section is collected by the kernel image's linker script; host tests build
 * their tables by hand and never use this. */
#define VIBEOS_DEVICE(desc) \
    static const vibeos_device_t *const vibeos_device_entry_##desc \
        __attribute__((used, section("vibeos_devices"))) = &(desc)

void vibeos_device_set_lock(void (*lock)(void), void (*unlock)(void));

/* A lock of a driver's own, from whoever has locks (C7).
 *
 * A driver with state that more than one core touches needs its own lock -
 * "its own", because a lock borrowed from a neighbour is a deadlock waiting for
 * the neighbour to call back. But a portable driver cannot build one: what a
 * lock is (mask interrupts, spin, name the holder when the wait is too long) is
 * the machine's to say. Before this, each driver's lock was registered by name
 * from the arch - which put the driver back in arch_hw.c, the file the registry
 * exists to keep drivers out of.
 *
 * So the machine registers the operations once, for every driver, and a driver
 * keeps the storage: `static vibeos_dev_lock_t g_lock = VIBEOS_DEV_LOCK("gui");`.
 * The storage is opaque and zero is unlocked; the name is what a deadlock
 * report prints. The operations must mask interrupts for the duration: an
 * interrupt handler that wants a lock its own core holds waits forever.
 *
 * `lock` returns -1, without waiting, when the caller already holds that lock -
 * the one wait that can never end. It is the provider's to say, because only
 * the provider knows who holds a lock: the machine's spinlocks record their
 * owning CPU. The case that exists is a panic printing from inside the display
 * while the display holds its lock: the print reaches the display again, and
 * without this it waits on its own core. A driver that sees -1 must not touch
 * what the lock protects.
 *
 * With nothing registered - a host test that does not care - locking does
 * nothing and always succeeds. */
typedef struct {
    uint64_t opaque[6];
    const char *name;
} vibeos_dev_lock_t;

#define VIBEOS_DEV_LOCK(n) { { 0 }, (n) }

void vibeos_device_set_lock_ops(int (*lock)(vibeos_dev_lock_t *l),
                                void (*unlock)(vibeos_dev_lock_t *l));
int vibeos_dev_lock(vibeos_dev_lock_t *l);     /* 0 held; -1 the caller held it already */
void vibeos_dev_unlock(vibeos_dev_lock_t *l);

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

/* An interrupt arrived on a vector the registry handed out. The handler's
 * flags, or -1 when no registered device owns that vector - a stray the caller
 * should count rather than ignore. */
int vibeos_device_irq_vector(uint32_t vector);

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

/* The display class: the first present display with both operations. The
 * console's every character and the timer's every tick come through here, so
 * neither names a driver - serial.c used to name the GUI's putc through a weak
 * default, and the timer called its repaint by name (C7). No display: nothing
 * happens, and vibeos_display_present() says so. */
int vibeos_display_present(void);
void vibeos_display_putc(char c);
void vibeos_display_tick(void);

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
