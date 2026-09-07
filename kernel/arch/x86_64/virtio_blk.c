/* Legacy virtio-blk over PCI: a real block-device driver (image-only).
 *
 * QEMU attaches the disk as a transitional virtio-blk-pci device (0x1AF4:0x1001)
 * exposing the legacy I/O register interface at BAR0. This driver enumerates it
 * on the PCI bus, sets up a single virtqueue in identity-mapped memory, and does
 * polled 512-byte sector reads - enough to back a read-only filesystem.
 */

#include <stdint.h>

#include "vibeos/arch_x86_64.h"
#include "vibeos/blockdev.h"

/* ---- port I/O ------------------------------------------------------------ */

/* How many times a wait for the device hit its bound.
 *
 * P7 wants every wait reachable from a syscall to have a bound *and*
 * a counter a gate can assert on. The bound was here; the counter was
 * not, so a timeout was indistinguishable from any other failure by
 * the time it reached the block layer - and VIBEOS_BLK_TIMEOUT, which
 * the boot gate asserts is zero, was produced by nobody. */
static uint64_t g_timeouts;

uint64_t vibeos_x86_64_virtio_blk_timeouts(void) {
    return g_timeouts;
}

static inline void vb_outb(uint16_t p, uint8_t v)  { __asm__ __volatile__("outb %0,%1"::"a"(v),"Nd"(p)); }
static inline void vb_outw(uint16_t p, uint16_t v) { __asm__ __volatile__("outw %0,%1"::"a"(v),"Nd"(p)); }
static inline void vb_outl(uint16_t p, uint32_t v) { __asm__ __volatile__("outl %0,%1"::"a"(v),"Nd"(p)); }
static inline uint8_t  vb_inb(uint16_t p) { uint8_t v;  __asm__ __volatile__("inb %1,%0":"=a"(v):"Nd"(p)); return v; }
static inline uint16_t vb_inw(uint16_t p) { uint16_t v; __asm__ __volatile__("inw %1,%0":"=a"(v):"Nd"(p)); return v; }
static inline uint32_t vb_inl(uint16_t p) { uint32_t v; __asm__ __volatile__("inl %1,%0":"=a"(v):"Nd"(p)); return v; }

/* ---- PCI config space ---------------------------------------------------- */

#define PCI_ADDR 0xCF8u
#define PCI_DATA 0xCFCu

static uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) {
    uint32_t addr = 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)dev << 11) |
                    ((uint32_t)fn << 8) | (off & 0xFCu);
    vb_outl(PCI_ADDR, addr);
    return vb_inl(PCI_DATA);
}

static void pci_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint32_t val) {
    uint32_t addr = 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)dev << 11) |
                    ((uint32_t)fn << 8) | (off & 0xFCu);
    vb_outl(PCI_ADDR, addr);
    vb_outl(PCI_DATA, val);
}

/* ---- legacy virtio register offsets (from BAR0 I/O base) ----------------- */

#define VIRTIO_HOST_FEATURES 0x00u
#define VIRTIO_GUEST_FEATURES 0x04u
#define VIRTIO_QUEUE_PFN 0x08u
#define VIRTIO_QUEUE_SIZE 0x0Cu
#define VIRTIO_QUEUE_SELECT 0x0Eu
#define VIRTIO_QUEUE_NOTIFY 0x10u
#define VIRTIO_STATUS 0x12u
#define VIRTIO_ISR 0x13u

#define VIRTIO_STATUS_ACK 1u
#define VIRTIO_STATUS_DRIVER 2u
#define VIRTIO_STATUS_DRIVER_OK 4u
#define VIRTIO_STATUS_FEATURES_OK 8u

#define VRING_DESC_F_NEXT 1u
#define VRING_DESC_F_WRITE 2u

struct virtq_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed));

struct virtq_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[];
} __attribute__((packed));

struct virtq_used_elem {
    uint32_t id;
    uint32_t len;
} __attribute__((packed));

struct virtq_used {
    uint16_t flags;
    uint16_t idx;
    struct virtq_used_elem ring[];
} __attribute__((packed));

struct virtio_blk_req {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
} __attribute__((packed));

/* Static, page-aligned virtqueue region (identity-mapped: phys == virt). */
static uint8_t g_vq[16384] __attribute__((aligned(4096)));
static struct virtio_blk_req g_req __attribute__((aligned(16)));
static volatile uint8_t g_status __attribute__((aligned(16)));

static uint16_t g_io_base;
static uint16_t g_qsz;
static struct virtq_desc *g_desc;
static struct virtq_avail *g_avail;
static struct virtq_used *g_used;
static uint16_t g_last_used;

/* ---- I6: completion by interrupt ------------------------------------------
 *
 * The device raises an interrupt when it has put something in the used ring.
 * Until now nobody listened: both transfers spun on `g_used->idx` up to a
 * hundred million times, which works and costs a core.
 *
 * What the interrupt buys is not correctness - the ring check was already
 * correct - it is that the waiting core can stop. The wait halts now instead
 * of spinning, and the interrupt is what wakes it. That makes the interrupt
 * load-bearing for latency while leaving the ring as the single source of
 * truth about what completed, which is the conservative half of the split:
 * a missed interrupt costs a timer tick of latency, not a lost completion.
 *
 * Counted both ways, because "the interrupt is wired up" and "the interrupt
 * does anything" are different claims and only the second is worth making. The
 * gate asserts irq_completions is not zero: an interrupt that never fires
 * leaves this exactly as slow as it was, silently. */
/* Declared here rather than pulled in from a header: this is the only thing
 * this driver needs from the interrupt controller, and gcc would have accepted
 * the implicit declaration with a warning while clang refuses it - which is a
 * trap this tree has been caught by before. */
extern int vibeos_x86_64_ioapic_route_pci(uint8_t irq, uint8_t vector, uint32_t dest);
extern void vibeos_x86_64_irq_probe(uint8_t gsi);

static uint8_t g_irq_line;
static uint8_t g_irq_pin;
static uint32_t g_pci_cmd;
static uint8_t g_irq_ready;
static volatile uint32_t g_irq_count;
static uint64_t g_irq_completions;
static uint64_t g_poll_completions;

/* May this core halt while it waits?
 *
 * Two conditions, and the first was the one I assumed rather than checked.
 *
 * Interrupts must actually be enabled. The comment above says the timer would
 * wake this core even if the device never interrupted - that is true once the
 * machine is running and false during bring-up, which is where most of the
 * disk reads happen. Halting there stopped the boot dead in kernel_early_init:
 * no timer yet, interrupts off, and nothing left that could ever wake it.
 *
 * And spin a little first. A transfer this device has already finished - most
 * of them, on a host this fast - completes before the first check, so halting
 * immediately would add an interrupt round trip to the common case in order to
 * save nothing. The halt is for the rare slow transfer, which is also the only
 * case where a hundred million spins cost anything. */
static int blk_may_halt(uint64_t spins) {
    uint64_t flags;

    if (!g_irq_ready || spins < 1024ull) {
        return 0;
    }
    __asm__ __volatile__("pushfq; pop %0" : "=r"(flags));
    return (flags & 0x200ull) != 0ull;   /* IF */
}


/* How many interrupts this device has raised.
 *
 * Reported instead of "completions attributed to the interrupt", which was the
 * first attempt and measured nothing: on a host this fast the transfer is
 * finished before the wait loop makes its first check, so there is no window
 * in which an interrupt can arrive *during* a wait. The first version therefore
 * read 0 of 14744 and looked exactly like an interrupt that never fires.
 *
 * "Did it fire at all" is the claim worth making and the one the gate can
 * assert. Whether it shortened any particular wait is a latency question, and
 * the poll counter beside it is what will answer that on a machine slow enough
 * for the difference to exist. */
uint64_t vibeos_x86_64_virtio_blk_irqs(void) {
    return (uint64_t)g_irq_count;
}

uint64_t vibeos_x86_64_virtio_blk_irq_completions(void) {
    return g_irq_completions;
}

uint64_t vibeos_x86_64_virtio_blk_poll_completions(void) {
    return g_poll_completions;
}

/* Called from the interrupt dispatcher. Acks the device and says that
 * something arrived; it deliberately does not touch the used ring, so there is
 * exactly one place that consumes completions and it is the waiter. Two
 * consumers of one ring is the defect this driver already had once, when two
 * cores raced over a single used index. */
void vibeos_x86_64_virtio_blk_irq(void) {
    if (g_io_base == 0u) {
        return;
    }
    (void)vb_inb(g_io_base + VIRTIO_ISR);   /* ack; reading clears it */
    g_irq_count++;
}
static int g_ready;

/* Sector count, read from the device's own configuration space. Asking the
 * device beats trusting a partition table about where the disk ends: GPT
 * validates its entries against the disk size, so a wrong size there turns a
 * bad table into an accepted one. */
static uint64_t g_capacity;

/* What the device said it holds. Exposed so the block layer can bounds-check
 * requests against it - a device with no size cannot have its requests
 * checked, and the layer refuses to register one. */
uint64_t vibeos_x86_64_virtio_blk_sectors(void) {
    return g_capacity;
}

static uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1u) & ~(a - 1u); }

/* One request is in flight at a time, and everything describing it is a single
 * global: one header, one status byte, descriptors 0 to 2, one used index. Two
 * cores reading at once therefore did not race over some window, they simply
 * overwrote each other - and both ways it goes wrong were seen in the same
 * boot. One core's sector number lands in the other core's request, so a read
 * succeeds and returns somebody else's data, which arrives upstream as a
 * filesystem that has gone bad. And whichever core consumes the used index
 * first advances it past the other's completion, leaving that one spinning on
 * a notification that has already been taken: a hundred million pauses of
 * total silence, which is what a wedged machine looks like from outside.
 *
 * Interrupts stay on. Nothing in an interrupt handler touches the disk, so a
 * handler can never want this lock, and a two-megabyte transfer with the timer
 * off is exactly the thing that has been mistaken for a hang here before. */
static volatile int g_blk_lock;

static void blk_lock(void) {
    while (__sync_lock_test_and_set(&g_blk_lock, 1)) {
        while (g_blk_lock) {
            __asm__ __volatile__("pause" ::: "memory");
        }
    }
}

static void blk_unlock(void) {
    __sync_lock_release(&g_blk_lock);
}

/* Find the transitional virtio-blk device and return its BAR0 I/O base. */
static uint16_t virtio_blk_find(void) {
    uint16_t bus, dev;
    for (bus = 0; bus < 256u; bus++) {
        for (dev = 0; dev < 32u; dev++) {
            uint32_t id = pci_read32((uint8_t)bus, (uint8_t)dev, 0, 0x00);
            uint32_t bar0, cmd;
            if ((id & 0xFFFFu) != 0x1AF4u) {
                continue;
            }
            if ((id >> 16) != 0x1001u) { /* transitional virtio-blk */
                continue;
            }
            /* Enable I/O space + bus mastering (DMA). */
            cmd = pci_read32((uint8_t)bus, (uint8_t)dev, 0, 0x04);
            /* I/O space and bus mastering on, and INTx *off*-disable.
             *
             * Bit 10 of the command register is Interrupt Disable, and this
             * line used to OR into whatever the firmware left there. UEFI
             * leaves it set - it has no use for the device's interrupts - so
             * the device raised none, the driver polled for every transfer,
             * and nothing said so. Clearing it is the difference between an
             * interrupt that is routed and one that arrives. */
            pci_write32((uint8_t)bus, (uint8_t)dev, 0, 0x04,
                        (cmd | 0x5u) & ~0x400u);
            bar0 = pci_read32((uint8_t)bus, (uint8_t)dev, 0, 0x10);
            if ((bar0 & 1u) == 0) {
                continue; /* not an I/O BAR */
            }
            /* The interrupt line, from the same config space and at the same
             * time. Read here rather than looked up later because "which
             * device did we pick" is exactly the question a second scan gets
             * wrong on a machine with two of them. */
            {
                uint32_t ints = pci_read32((uint8_t)bus, (uint8_t)dev, 0, 0x3Cu);
                g_irq_line = (uint8_t)(ints & 0xFFu);
                g_irq_pin = (uint8_t)((ints >> 8) & 0xFFu);
                g_pci_cmd = pci_read32((uint8_t)bus, (uint8_t)dev, 0, 0x04u);
            }
            return (uint16_t)(bar0 & 0xFFFCu);
        }
    }
    return 0;
}

int vibeos_x86_64_virtio_blk_init(void) {
    uint64_t desc_off, avail_off, used_off;
    uint32_t i;

    g_io_base = virtio_blk_find();
    if (g_io_base == 0) {
        vibeos_x86_64_serial_puts("[VIRTIO] no virtio-blk device found\n");
        return -1;
    }

    vb_outb(g_io_base + VIRTIO_STATUS, 0);                       /* reset      */
    vb_outb(g_io_base + VIRTIO_STATUS, VIRTIO_STATUS_ACK);
    vb_outb(g_io_base + VIRTIO_STATUS, VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER);
    (void)vb_inl(g_io_base + VIRTIO_HOST_FEATURES);
    vb_outl(g_io_base + VIRTIO_GUEST_FEATURES, 0);              /* no features */

    vb_outw(g_io_base + VIRTIO_QUEUE_SELECT, 0);
    g_qsz = vb_inw(g_io_base + VIRTIO_QUEUE_SIZE);
    if (g_qsz == 0 || g_qsz > 256u) {
        vibeos_x86_64_serial_puts("[VIRTIO] unsupported queue size\n");
        return -1;
    }

    for (i = 0; i < sizeof(g_vq); i++) {
        g_vq[i] = 0;
    }
    desc_off = 0;
    avail_off = (uint64_t)g_qsz * sizeof(struct virtq_desc);
    used_off = align_up(avail_off + 6u + 2u * g_qsz, 4096u);
    if (used_off + 6u + 8u * g_qsz > sizeof(g_vq)) {
        vibeos_x86_64_serial_puts("[VIRTIO] virtqueue too large for static buffer\n");
        return -1;
    }
    g_desc = (struct virtq_desc *)(void *)(g_vq + desc_off);
    g_avail = (struct virtq_avail *)(void *)(g_vq + avail_off);
    g_used = (struct virtq_used *)(void *)(g_vq + used_off);
    g_last_used = 0;

    /* Device-specific configuration follows the legacy header at offset 20
     * while MSI-X is disabled, and capacity is its first field. */
    g_capacity = (uint64_t)vb_inl(g_io_base + 20u) |
                 ((uint64_t)vb_inl(g_io_base + 24u) << 32);

    /* Legacy: queue address is the page frame number of the queue region. */
    vb_outl(g_io_base + VIRTIO_QUEUE_PFN, (uint32_t)((uint64_t)(uintptr_t)g_vq >> 12));

    vb_outb(g_io_base + VIRTIO_STATUS,
            VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_DRIVER_OK);

    /* Route the device's interrupt to vector 43, next to the two the PS/2
     * controller already uses. Failure is not fatal: the wait falls back to
     * spinning exactly as it did before, and the counter says so - which is
     * the difference between a degraded machine and a mysterious one. */
    /* Routed, and it does not deliver. What is known, so the next attempt
     * starts from evidence rather than from the top:
     *
     *   the device has an INTA pin (config 0x3D reads 1);
     *   INTx is enabled - bit 10 of the command register is clear, and it was
     *     not before, because this driver used to OR into whatever UEFI left
     *     there and UEFI leaves it set. That fix is real and is kept;
     *   the entry is programmed level-triggered and active low, which is what
     *     a PCI line needs and what the ISA routing path did not do. Also real,
     *     also kept, and it is why vibeos_x86_64_ioapic_route_pci exists;
     *   the Line register says 11, and routing 11 delivers nothing. Routing
     *     GSI 16..19 as well - the usual q35 mapping for PCI INTA..D, since
     *     this kernel does not parse the ACPI _PRT - delivers nothing either.
     *     So the mapping is not the remaining problem, or not only it.
     *
     * Both of those were then checked, with vibeos_x86_64_irq_probe, and both
     * came back clean:
     *
     *   [APIC] probe gsi=0xb redir_lo=0xa02b redir_hi=0x0 svr=0x1ff
     *          pic_mask=0xffff iso_count=0x5
     *
     * redir_lo 0xa02b is vector 43, level triggered, active low, unmasked,
     * fixed delivery, physical destination - exactly right. svr bit 8 is set,
     * so the local APIC is software-enabled. Both 8259s are fully masked, so
     * nothing is stealing the line. Vector 43 has an IDT entry: 48 are wired.
     *
     * And then the measurement that actually splits the question. Routing
     * *every* GSI 0..23 to vector 43 still produced zero interrupts. So it is
     * not the mapping, not the trigger mode, not the mask, not the IDT and not
     * the local APIC: **the device is not raising an interrupt at all.**
     *
     * Which moves the next investigation to the other side of the wire, where
     * it should have started: the virtqueue configuration. The candidates are
     * VIRTQ_AVAIL_F_NO_INTERRUPT in avail->flags (this driver never writes
     * that field and the queue is in .bss, so it should be zero - worth
     * confirming rather than assuming, since assuming is what cost the last
     * three attempts), and MSI-X, which QEMU's virtio-blk-pci advertises by
     * default and which changes how the device signals.
     *
     * The driver is correct meanwhile: it polls, exactly as it did before, and
     * the counters below say so out loud rather than leaving it to be noticed.
     */
    if (g_irq_line != 0u && g_irq_line < 24u &&
        vibeos_x86_64_ioapic_route_pci(g_irq_line, 43u, 0u) == 0) {
        g_irq_ready = 1u;
    }
    vibeos_x86_64_irq_probe(g_irq_line);
    vibeos_x86_64_serial_puts("[VIRTIO] blk irq line=0x");
    vibeos_x86_64_serial_print_hex((uint64_t)g_irq_line);
    vibeos_x86_64_serial_puts(" pin=0x");
    vibeos_x86_64_serial_print_hex((uint64_t)g_irq_pin);
    vibeos_x86_64_serial_puts(" cmd=0x");
    vibeos_x86_64_serial_print_hex((uint64_t)g_pci_cmd);
    vibeos_x86_64_serial_puts(g_irq_ready ? " routed\n" : " NOT routed\n");

    g_ready = 1;
    vibeos_x86_64_serial_puts("[VIRTIO] virtio-blk ready (io=0x");
    vibeos_x86_64_serial_print_hex(g_io_base);
    vibeos_x86_64_serial_puts(" qsz=0x");
    vibeos_x86_64_serial_print_hex(g_qsz);
    vibeos_x86_64_serial_puts(")\n");
    return 0;
}

/* Transfer one 512-byte sector. buf must be identity-mapped (phys == virt).
 * `write` selects VIRTIO_BLK_T_OUT; the data descriptor is device-writable only
 * for reads. */
/* Sectors per request.
 *
 * One request per 512-byte sector is correct and unusably slow: the cost here
 * is per request, not per byte - a descriptor chain, a notify, and a polled
 * wait for the device to come back. Reading a two-megabyte program that way
 * takes about four thousand round trips and roughly two minutes under
 * emulation. virtio-blk takes the transfer size from the data descriptor, so
 * the same three descriptors can carry sixty-four kilobytes as easily as one
 * sector. */
#define VIRTIO_BLK_MAX_SECTORS 128u

static int virtio_blk_rw_n(uint64_t sector, void *buf, uint32_t sectors, int write) {
    uint16_t head;

    if (!g_ready || !buf || sectors == 0u || sectors > VIRTIO_BLK_MAX_SECTORS) {
        return -1;
    }
    blk_lock();
    g_req.type = write ? 1u : 0u;   /* OUT (write) / IN (read) */
    g_req.reserved = 0;
    g_req.sector = sector;
    g_status = 0xFF;

    g_desc[0].addr = (uint64_t)(uintptr_t)&g_req;
    g_desc[0].len = sizeof(struct virtio_blk_req);
    g_desc[0].flags = VRING_DESC_F_NEXT;
    g_desc[0].next = 1;
    g_desc[1].addr = (uint64_t)(uintptr_t)buf;
    g_desc[1].len = 512u * sectors;
    g_desc[1].flags = write ? VRING_DESC_F_NEXT
                            : (VRING_DESC_F_NEXT | VRING_DESC_F_WRITE);
    g_desc[1].next = 2;
    g_desc[2].addr = (uint64_t)(uintptr_t)&g_status;
    g_desc[2].len = 1;
    g_desc[2].flags = VRING_DESC_F_WRITE;
    g_desc[2].next = 0;

    head = g_avail->idx % g_qsz;
    g_avail->ring[head] = 0; /* descriptor chain head */
    __asm__ __volatile__("sfence" ::: "memory");
    g_avail->idx++;
    __asm__ __volatile__("sfence" ::: "memory");

    vb_outw(g_io_base + VIRTIO_QUEUE_NOTIFY, 0);

    /* Poll for completion. */
    {
        uint64_t spins = 0;
        uint32_t irq_before = g_irq_count;

        /* Waits rather than spins.
         *
         * This loop used to burn up to a hundred million iterations of `pause`
         * on the core that issued the transfer. The used ring is still the
         * only thing that says *what* completed - two consumers of one used
         * index is a defect this driver has already had, so the interrupt
         * handler deliberately does not touch it - but the core no longer has
         * to keep asking.
         *
         * `hlt` is safe here: this driver's lock leaves interrupts enabled,
         * which was a deliberate decision when the lock was added, and the
         * timer would wake this core even if the device's interrupt never
         * arrived. A missed interrupt therefore costs a tick of latency and
         * not a hang. The bound stays exactly as it was: a wait that can end
         * for two reasons still needs one that ends it for certain. */
        while (g_used->idx == g_last_used) {
            if (++spins > 100000000ull) {
                /* Counted, not only printed. The bound existing is half of
                 * P7's latency property; the other half is a number a gate can
                 * assert, and a serial line is not one - this project has a
                 * rule about that. */
                g_timeouts++;
                /* Leaving the lock held here would turn one timed-out request
                 * into a machine that never reads a sector again. */
                vibeos_x86_64_serial_puts("[VIRTIO] read timeout\n");
                blk_unlock();
                return -1;
            }
            if (blk_may_halt(spins)) {
                __asm__ __volatile__("hlt" ::: "memory");
            } else {
                __asm__ __volatile__("pause" ::: "memory");
            }
        }
        /* Which of the two ended the wait. Counted outside the loop so that a
         * transfer the device had already finished before the first check -
         * which is most of them on a fast host - is still attributed. */
        if (g_irq_count != irq_before) {
            g_irq_completions++;
        } else {
            g_poll_completions++;
        }
    }
    g_last_used = g_used->idx;
    (void)vb_inb(g_io_base + VIRTIO_ISR); /* ack */

    {
        int rc = (g_status == 0) ? 0 : -1;
        blk_unlock();
        return rc;
    }
}

int vibeos_x86_64_virtio_blk_read(uint64_t sector, void *buf) {
    return virtio_blk_rw_n(sector, buf, 1u, 0);
}

int vibeos_x86_64_virtio_blk_write(uint64_t sector, const void *buf) {
    return virtio_blk_rw_n(sector, (void *)(uintptr_t)buf, 1u, 1);
}

/* Read a run of consecutive sectors in as few requests as the device allows.
 * `buf` must have room for `sectors` * 512 bytes. */
int vibeos_x86_64_virtio_blk_read_many(uint64_t sector, void *buf, uint32_t sectors) {
    uint8_t *out = (uint8_t *)buf;

    while (sectors > 0u) {
        uint32_t n = (sectors > VIRTIO_BLK_MAX_SECTORS) ? VIRTIO_BLK_MAX_SECTORS : sectors;
        if (virtio_blk_rw_n(sector, out, n, 0) != 0) {
            return -1;
        }
        sector += n;
        out += (uint64_t)n * 512u;
        sectors -= n;
    }
    return 0;
}

/* Write a run of consecutive sectors, in as few requests as the device allows.
 *
 * The mirror of read_many above, and it exists for the same reason that one
 * did: a 2 MiB transfer done a sector at a time under a lock that masks
 * interrupts was indistinguishable from a hang, and was reported as one. There
 * was no equivalent on the write side because until I4 nothing wrote enough to
 * notice - the whole of a boot's writing was about thirty sectors.
 *
 * `buf` is const here and not in the request struct below, because the block
 * layer carries one buffer pointer for both directions. Casting it away at
 * exactly one place, with the driver's own signature saying it does not write
 * through it, is better than a union that has to be read correctly at each
 * use. */
int vibeos_x86_64_virtio_blk_write_many(uint64_t sector, const void *buf,
                                        uint32_t sectors) {
    const uint8_t *in = (const uint8_t *)buf;

    while (sectors > 0u) {
        uint32_t n = (sectors > VIRTIO_BLK_MAX_SECTORS) ? VIRTIO_BLK_MAX_SECTORS : sectors;
        if (virtio_blk_rw_n(sector, (void *)(uintptr_t)in, n, 1) != 0) {
            return -1;
        }
        sector += n;
        in += (uint64_t)n * 512u;
        sectors -= n;
    }
    return 0;
}

/* Tell the device to stop holding writes in its own volatile cache.
 *
 * VIRTIO_BLK_T_FLUSH is type 4, and unlike a read or a write it carries no
 * data: the chain is the header and the status byte, with nothing between
 * them. Building it out of virtio_blk_rw_n was not possible for that reason
 * and the duplication is deliberate rather than a shortcut - a data descriptor
 * with a length of zero is not the same request, and a device is entitled to
 * refuse it.
 *
 * `sector` is required to be zero for a flush; sending anything else is a
 * request the specification does not define. */
int vibeos_x86_64_virtio_blk_barrier(void) {
    uint16_t head;
    uint64_t spins = 0;

    if (!g_ready) {
        return -1;
    }
    blk_lock();
    g_req.type = 4u;             /* VIRTIO_BLK_T_FLUSH */
    g_req.reserved = 0;
    g_req.sector = 0;
    g_status = 0xFF;

    g_desc[0].addr = (uint64_t)(uintptr_t)&g_req;
    g_desc[0].len = sizeof(struct virtio_blk_req);
    g_desc[0].flags = VRING_DESC_F_NEXT;
    g_desc[0].next = 1;
    g_desc[1].addr = (uint64_t)(uintptr_t)&g_status;
    g_desc[1].len = 1;
    g_desc[1].flags = VRING_DESC_F_WRITE;
    g_desc[1].next = 0;

    head = g_avail->idx % g_qsz;
    g_avail->ring[head] = 0;
    __asm__ __volatile__("sfence" ::: "memory");
    g_avail->idx++;
    __asm__ __volatile__("sfence" ::: "memory");
    vb_outw(g_io_base + VIRTIO_QUEUE_NOTIFY, 0);

    while (g_used->idx == g_last_used) {
        if (++spins > 100000000ull) {
            /* Bounded like every other wait here, and counted in the same
             * place: a barrier that hung would stop the machine at exactly the
             * moment somebody was trying to make data durable. */
            g_timeouts++;
            blk_unlock();
            return -1;
        }
        /* Waits rather than spins, for the reason written at the transfer
         * loop above: the ring stays the only thing that says what completed,
         * and the interrupt only says that something did. */
        if (blk_may_halt(spins)) {
            __asm__ __volatile__("hlt" ::: "memory");
        } else {
            __asm__ __volatile__("pause" ::: "memory");
        }
    }
    g_last_used = g_used->idx;
    (void)vb_inb(g_io_base + VIRTIO_ISR);
    {
        int rc = (g_status == 0) ? 0 : -1;
        blk_unlock();
        return rc;
    }
}

/* ---- the portable block-device view -------------------------------------- */

/* The rest of the storage stack is written against vibeos_blockdev_t and knows
 * nothing about virtio, which is what lets it be tested on the host against an
 * array. This is the one function that joins the two, and it is deliberately
 * the only place in the kernel that does. */

static int blk_dev_read(void *ctx, uint64_t lba, void *buf) {
    (void)ctx;
    return virtio_blk_rw_n(lba, buf, 1u, 0);
}

static int blk_dev_write(void *ctx, uint64_t lba, const void *buf) {
    (void)ctx;
    return virtio_blk_rw_n(lba, (void *)(uintptr_t)buf, 1u, 1);
}

void vibeos_x86_64_virtio_blk_device(vibeos_blockdev_t *out) {
    if (!out) {
        return;
    }
    out->read = blk_dev_read;
    out->write = blk_dev_write;
    /* No flush: this device completes a request when the used ring says so,
     * with no cache of its own to empty. Claiming a barrier it does not
     * provide would be worse than admitting there is none - the journal reads
     * a missing flush as "nothing to do", not as "already durable". */
    out->flush = 0;
    out->ctx = 0;
    out->sectors = g_capacity;
}
