/* Legacy virtio-net over PCI: the machine's network interface (image-only).
 *
 * QEMU attaches `-device virtio-net-pci` as a transitional device
 * (0x1AF4:0x1000) with the legacy I/O register interface at BAR0. Two
 * virtqueues carry frames: queue 0 receive, queue 1 transmit. Every buffer
 * lives in identity-mapped memory so the device's DMA addresses are the same
 * addresses the kernel uses.
 *
 * Receive is polled rather than interrupt-driven: the stack is pumped from the
 * timer tick, which keeps the driver free of any interaction with the scheduler
 * locks and makes its behaviour identical on every core.
 */

#include <stdint.h>

#include "vibeos/arch_x86_64.h"

/* ---- port I/O ------------------------------------------------------------ */

static inline void vn_outb(uint16_t p, uint8_t v)  { __asm__ __volatile__("outb %0,%1"::"a"(v),"Nd"(p)); }
static inline void vn_outw(uint16_t p, uint16_t v) { __asm__ __volatile__("outw %0,%1"::"a"(v),"Nd"(p)); }
static inline void vn_outl(uint16_t p, uint32_t v) { __asm__ __volatile__("outl %0,%1"::"a"(v),"Nd"(p)); }
static inline uint8_t  vn_inb(uint16_t p) { uint8_t v;  __asm__ __volatile__("inb %1,%0":"=a"(v):"Nd"(p)); return v; }
static inline uint16_t vn_inw(uint16_t p) { uint16_t v; __asm__ __volatile__("inw %1,%0":"=a"(v):"Nd"(p)); return v; }
static inline uint32_t vn_inl(uint16_t p) { uint32_t v; __asm__ __volatile__("inl %1,%0":"=a"(v):"Nd"(p)); return v; }

#define PCI_ADDR 0xCF8u
#define PCI_DATA 0xCFCu

static uint32_t vn_pci_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) {
    uint32_t addr = 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)dev << 11) |
                    ((uint32_t)fn << 8) | (off & 0xFCu);
    vn_outl(PCI_ADDR, addr);
    return vn_inl(PCI_DATA);
}

static void vn_pci_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint32_t val) {
    uint32_t addr = 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)dev << 11) |
                    ((uint32_t)fn << 8) | (off & 0xFCu);
    vn_outl(PCI_ADDR, addr);
    vn_outl(PCI_DATA, val);
}

/* ---- legacy virtio registers --------------------------------------------- */

#define VIRTIO_HOST_FEATURES 0x00u
#define VIRTIO_GUEST_FEATURES 0x04u
#define VIRTIO_QUEUE_PFN 0x08u
#define VIRTIO_QUEUE_SIZE 0x0Cu
#define VIRTIO_QUEUE_SELECT 0x0Eu
#define VIRTIO_QUEUE_NOTIFY 0x10u
#define VIRTIO_STATUS 0x12u
#define VIRTIO_ISR 0x13u
#define VIRTIO_NET_CFG_MAC 0x14u    /* device config: 6 MAC bytes */

#define VIRTIO_STATUS_ACK 1u
#define VIRTIO_STATUS_DRIVER 2u
#define VIRTIO_STATUS_DRIVER_OK 4u

#define VIRTIO_NET_F_MAC (1u << 5)

#define VRING_DESC_F_NEXT 1u
#define VRING_DESC_F_WRITE 2u

struct vq_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed));

struct vq_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[];
} __attribute__((packed));

struct vq_used_elem {
    uint32_t id;
    uint32_t len;
} __attribute__((packed));

struct vq_used {
    uint16_t flags;
    uint16_t idx;
    struct vq_used_elem ring[];
} __attribute__((packed));

typedef struct {
    struct vq_desc *desc;
    struct vq_avail *avail;
    struct vq_used *used;
    uint16_t size;
    uint16_t last_used;
} vnet_queue_t;

/* virtio-net prepends a header to every frame; 10 bytes without merged rx
 * buffers, which we do not negotiate. */
#define VNET_HDR 10u
#define VNET_RX_BUFS 16u
#define VNET_BUF 2048u

static uint8_t g_rxq_mem[16384] __attribute__((aligned(4096)));
static uint8_t g_txq_mem[16384] __attribute__((aligned(4096)));
static uint8_t g_rx_buf[VNET_RX_BUFS][VNET_BUF] __attribute__((aligned(16)));
static uint8_t g_tx_buf[VNET_BUF] __attribute__((aligned(16)));

static vnet_queue_t g_rx;
static vnet_queue_t g_tx;
static uint16_t g_net_io;
static uint8_t g_mac[6];
static int g_net_ready;
static uint64_t g_tx_frames;
static uint64_t g_rx_frames;

static uint64_t vn_align_up(uint64_t v, uint64_t a) { return (v + a - 1u) & ~(a - 1u); }

static uint16_t virtio_net_find(void) {
    uint16_t bus, dev;
    for (bus = 0; bus < 256u; bus++) {
        for (dev = 0; dev < 32u; dev++) {
            uint32_t id = vn_pci_read32((uint8_t)bus, (uint8_t)dev, 0, 0x00);
            uint32_t bar0, cmd;
            if ((id & 0xFFFFu) != 0x1AF4u || (id >> 16) != 0x1000u) {
                continue;   /* transitional virtio-net only */
            }
            cmd = vn_pci_read32((uint8_t)bus, (uint8_t)dev, 0, 0x04);
            vn_pci_write32((uint8_t)bus, (uint8_t)dev, 0, 0x04, cmd | 0x5u); /* I/O + DMA */
            bar0 = vn_pci_read32((uint8_t)bus, (uint8_t)dev, 0, 0x10);
            if ((bar0 & 1u) == 0u) {
                continue;
            }
            return (uint16_t)(bar0 & 0xFFFCu);
        }
    }
    return 0;
}

/* Lay a virtqueue out inside `mem` and hand its page frame to the device. */
static int vnet_queue_setup(vnet_queue_t *q, uint8_t *mem, uint32_t mem_len, uint16_t index) {
    uint64_t avail_off, used_off;
    uint32_t i;

    vn_outw(g_net_io + VIRTIO_QUEUE_SELECT, index);
    q->size = vn_inw(g_net_io + VIRTIO_QUEUE_SIZE);
    if (q->size == 0u || q->size > 256u) {
        return -1;
    }
    for (i = 0; i < mem_len; i++) {
        mem[i] = 0;
    }
    avail_off = (uint64_t)q->size * sizeof(struct vq_desc);
    used_off = vn_align_up(avail_off + 6u + 2u * q->size, 4096u);
    if (used_off + 6u + 8u * q->size > mem_len) {
        return -1;
    }
    q->desc = (struct vq_desc *)(void *)mem;
    q->avail = (struct vq_avail *)(void *)(mem + avail_off);
    q->used = (struct vq_used *)(void *)(mem + used_off);
    q->last_used = 0;
    vn_outl(g_net_io + VIRTIO_QUEUE_PFN, (uint32_t)((uint64_t)(uintptr_t)mem >> 12));
    return 0;
}

/* Publish one receive buffer to the device. */
static void vnet_rx_publish(uint16_t i) {
    uint16_t slot;
    g_rx.desc[i].addr = (uint64_t)(uintptr_t)g_rx_buf[i];
    g_rx.desc[i].len = VNET_BUF;
    g_rx.desc[i].flags = VRING_DESC_F_WRITE;
    g_rx.desc[i].next = 0;
    slot = (uint16_t)(g_rx.avail->idx % g_rx.size);
    g_rx.avail->ring[slot] = i;
    __asm__ __volatile__("sfence" ::: "memory");
    g_rx.avail->idx++;
}

int vibeos_x86_64_virtio_net_init(void) {
    uint32_t host_features, guest_features;
    uint16_t i;

    g_net_io = virtio_net_find();
    if (g_net_io == 0u) {
        vibeos_x86_64_serial_puts("[VNET] no virtio-net device found\n");
        return -1;
    }

    vn_outb(g_net_io + VIRTIO_STATUS, 0);
    vn_outb(g_net_io + VIRTIO_STATUS, VIRTIO_STATUS_ACK);
    vn_outb(g_net_io + VIRTIO_STATUS, VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER);

    /* Accept only the MAC feature: everything else (checksum offload, merged
     * receive buffers, GSO) would change the header or the buffer contract. */
    host_features = vn_inl(g_net_io + VIRTIO_HOST_FEATURES);
    guest_features = host_features & VIRTIO_NET_F_MAC;
    vn_outl(g_net_io + VIRTIO_GUEST_FEATURES, guest_features);

    if (guest_features & VIRTIO_NET_F_MAC) {
        for (i = 0; i < 6u; i++) {
            g_mac[i] = vn_inb((uint16_t)(g_net_io + VIRTIO_NET_CFG_MAC + i));
        }
    } else {
        /* Locally administered fallback address. */
        g_mac[0] = 0x52; g_mac[1] = 0x54; g_mac[2] = 0x00;
        g_mac[3] = 0x12; g_mac[4] = 0x34; g_mac[5] = 0x56;
    }

    if (vnet_queue_setup(&g_rx, g_rxq_mem, (uint32_t)sizeof(g_rxq_mem), 0) != 0 ||
        vnet_queue_setup(&g_tx, g_txq_mem, (uint32_t)sizeof(g_txq_mem), 1) != 0) {
        vibeos_x86_64_serial_puts("[VNET] virtqueue setup failed\n");
        return -1;
    }

    for (i = 0; i < VNET_RX_BUFS && i < g_rx.size; i++) {
        vnet_rx_publish(i);
    }
    __asm__ __volatile__("sfence" ::: "memory");
    vn_outw(g_net_io + VIRTIO_QUEUE_NOTIFY, 0);

    vn_outb(g_net_io + VIRTIO_STATUS,
            VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_DRIVER_OK);
    g_net_ready = 1;

    vibeos_x86_64_serial_lock();
    vibeos_x86_64_serial_puts("[VNET] virtio-net ready io=0x");
    vibeos_x86_64_serial_print_hex(g_net_io);
    vibeos_x86_64_serial_puts(" mac=");
    for (i = 0; i < 6u; i++) {
        vibeos_x86_64_serial_print_hex(g_mac[i]);
        if (i < 5u) {
            vibeos_x86_64_serial_puts(":");
        }
    }
    vibeos_x86_64_serial_puts("\n");
    vibeos_x86_64_serial_unlock();
    return 0;
}

const uint8_t *vibeos_x86_64_virtio_net_mac(void) {
    return g_mac;
}

int vibeos_x86_64_virtio_net_ready(void) {
    return g_net_ready;
}

/* One virtqueue, one descriptor, one staging buffer - and, until now, no lock.
 *
 * The same defect virtio-blk had, in the same shape, never applied here. Every
 * transmit uses g_tx_buf, g_tx.desc[0] and g_tx.last_used, and publishes with a
 * plain `g_tx.avail->idx++` on a 16-bit field. Two cores sending at once do not
 * race over a window, they overwrite each other: one core's frame lands in the
 * other's descriptor, the available index skips or repeats, and whichever core
 * reads the used index first leaves the other spinning on a completion that has
 * already been consumed.
 *
 * That last part is what a wedge looks like from outside. The report put CPU#0
 * inside the loop below with three other cores alive, and QEMU said it out loud
 * on its own stderr: "Guest says index 65535 is available" - which is exactly
 * what a lost increment on a uint16_t produces.
 *
 * Interrupts stay on, as in the block driver: nothing in an interrupt handler
 * transmits, so this lock can never be wanted from one. */
static volatile int g_tx_lock;

static void tx_lock(void) {
    while (__sync_lock_test_and_set(&g_tx_lock, 1)) {
        while (g_tx_lock) {
            __asm__ __volatile__("pause" ::: "memory");
        }
    }
}

static void tx_unlock(void) {
    __sync_lock_release(&g_tx_lock);
}

/* Transmit one Ethernet frame. Blocks until the device consumes the descriptor,
 * which under QEMU is immediate. */
int vibeos_x86_64_virtio_net_send(const void *frame, uint32_t len) {
    uint16_t slot;
    uint32_t i;
    uint64_t spins = 0;

    if (!g_net_ready || !frame || len == 0u || len + VNET_HDR > VNET_BUF) {
        return -1;
    }
    tx_lock();
    for (i = 0; i < VNET_HDR; i++) {
        g_tx_buf[i] = 0;                 /* no offload flags, no GSO */
    }
    for (i = 0; i < len; i++) {
        g_tx_buf[VNET_HDR + i] = ((const uint8_t *)frame)[i];
    }

    g_tx.desc[0].addr = (uint64_t)(uintptr_t)g_tx_buf;
    g_tx.desc[0].len = VNET_HDR + len;
    g_tx.desc[0].flags = 0;              /* device-readable */
    g_tx.desc[0].next = 0;

    slot = (uint16_t)(g_tx.avail->idx % g_tx.size);
    g_tx.avail->ring[slot] = 0;
    __asm__ __volatile__("sfence" ::: "memory");
    g_tx.avail->idx++;
    __asm__ __volatile__("sfence" ::: "memory");
    vn_outw(g_net_io + VIRTIO_QUEUE_NOTIFY, 1);

    while (g_tx.used->idx == g_tx.last_used) {
        if (++spins > 50000000ull) {
            /* Giving the lock back matters more than the frame does: holding it
             * here would turn one timed-out transmit into a permanently dead
             * network, which is the shape of failure this whole change exists
             * to remove. */
            tx_unlock();
            vibeos_x86_64_serial_puts("[VNET] transmit timeout\n");
            return -1;
        }
        __asm__ __volatile__("pause" ::: "memory");
    }
    g_tx.last_used = g_tx.used->idx;
    (void)vn_inb(g_net_io + VIRTIO_ISR);
    g_tx_frames++;
    tx_unlock();
    return 0;
}

/* Pull one received frame, if any. Returns its length, 0 when the queue is
 * empty. The virtio-net header is stripped. */
int vibeos_x86_64_virtio_net_recv(void *out, uint32_t cap) {
    struct vq_used_elem *e;
    uint16_t slot, id;
    uint32_t len, i;

    if (!g_net_ready || !out) {
        return 0;
    }
    if (g_rx.used->idx == g_rx.last_used) {
        return 0;
    }
    slot = (uint16_t)(g_rx.last_used % g_rx.size);
    e = &g_rx.used->ring[slot];
    id = (uint16_t)e->id;
    len = e->len;
    g_rx.last_used++;

    if (id >= VNET_RX_BUFS || len <= VNET_HDR) {
        len = 0;
    } else {
        len -= VNET_HDR;
        if (len > cap) {
            len = cap;
        }
        for (i = 0; i < len; i++) {
            ((uint8_t *)out)[i] = g_rx_buf[id][VNET_HDR + i];
        }
        g_rx_frames++;
    }

    /* Hand the buffer straight back to the device. */
    if (id < VNET_RX_BUFS) {
        vnet_rx_publish(id);
        __asm__ __volatile__("sfence" ::: "memory");
        vn_outw(g_net_io + VIRTIO_QUEUE_NOTIFY, 0);
    }
    return (int)len;
}

void vibeos_x86_64_virtio_net_stats(uint64_t *out_tx, uint64_t *out_rx) {
    if (out_tx) {
        *out_tx = g_tx_frames;
    }
    if (out_rx) {
        *out_rx = g_rx_frames;
    }
}
