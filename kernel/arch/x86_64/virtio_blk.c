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
            pci_write32((uint8_t)bus, (uint8_t)dev, 0, 0x04, cmd | 0x5u);
            bar0 = pci_read32((uint8_t)bus, (uint8_t)dev, 0, 0x10);
            if ((bar0 & 1u) == 0) {
                continue; /* not an I/O BAR */
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
        while (g_used->idx == g_last_used) {
            if (++spins > 100000000ull) {
                /* Leaving the lock held here would turn one timed-out request
                 * into a machine that never reads a sector again. */
                vibeos_x86_64_serial_puts("[VIRTIO] read timeout\n");
                blk_unlock();
                return -1;
            }
            __asm__ __volatile__("pause" ::: "memory");
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
