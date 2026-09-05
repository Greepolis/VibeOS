/* AHCI (SATA) block driver, read/write, polled.
 *
 * Why this exists: the kernel could only talk to virtio-blk, which QEMU
 * provides and no desktop hypervisor does. The appliances imported into
 * VirtualBox, booted, and then every exec failed - the files were on the disk
 * and the kernel had no way to read them. UEFI does the reading up to
 * ExitBootServices, so the bootloader worked and hid it.
 *
 * The controller is found by PCI class rather than by device id. VirtualBox
 * emulates an ICH8 (8086:2829), QEMU an ICH9 (8086:2922), VMware something
 * else again, and all three are the same standard controller - matching ids
 * would mean a table that is wrong on the next hypervisor.
 *
 * Polled, not interrupt-driven, and deliberately: the filesystem calls this
 * from inside g_exec_lock, with interrupts masked, so a completion interrupt
 * could never arrive. That is the same reason the multi-sector path matters -
 * see the note about the 2 MB read under a spinlock in CLAUDE.md.
 */

#include <stdint.h>

#include "vibeos/arch_x86_64.h"

/* ---- PCI config space ---------------------------------------------------- */

#define PCI_ADDR 0xCF8u
#define PCI_DATA 0xCFCu

static inline void ah_outl(uint16_t p, uint32_t v) {
    __asm__ __volatile__("outl %0,%1" :: "a"(v), "Nd"(p));
}
static inline uint32_t ah_inl(uint16_t p) {
    uint32_t v;
    __asm__ __volatile__("inl %1,%0" : "=a"(v) : "Nd"(p));
    return v;
}

static uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) {
    uint32_t addr = 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)dev << 11) |
                    ((uint32_t)fn << 8) | (off & 0xFCu);
    ah_outl(PCI_ADDR, addr);
    return ah_inl(PCI_DATA);
}

static void pci_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint32_t val) {
    uint32_t addr = 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)dev << 11) |
                    ((uint32_t)fn << 8) | (off & 0xFCu);
    ah_outl(PCI_ADDR, addr);
    ah_outl(PCI_DATA, val);
}

/* ---- HBA and port registers ---------------------------------------------- */

#define HBA_GHC      0x04u
#define HBA_GHC_AE   0x80000000u
#define HBA_PI       0x0Cu

#define PORT_BASE(p) (0x100u + (uint32_t)(p) * 0x80u)
#define PxCLB   0x00u
#define PxCLBU  0x04u
#define PxFB    0x08u
#define PxFBU   0x0Cu
#define PxIS    0x10u
#define PxCMD   0x18u
#define PxTFD   0x20u
#define PxSIG   0x24u
#define PxSSTS  0x28u
#define PxSERR  0x30u
#define PxCI    0x38u

#define PxCMD_ST  0x0001u
#define PxCMD_FRE 0x0010u
#define PxCMD_FR  0x4000u
#define PxCMD_CR  0x8000u

#define ATA_CMD_READ_DMA_EX  0x25u
#define ATA_CMD_WRITE_DMA_EX 0x35u
#define ATA_CMD_FLUSH_EX     0xEAu
#define ATA_CMD_IDENTIFY     0xECu
#define ATA_DEV_BUSY 0x80u
#define ATA_DEV_DRQ  0x08u

/* ---- DMA structures ------------------------------------------------------ */

/* Physical == virtual below 4 GiB (the kernel identity-maps it), and these are
 * kernel .bss, so a pointer is a bus address. The alignment requirements are
 * the controller's, not ours: 1 KiB for the command list, 256 B for the
 * received-FIS area, 128 B for a command table. */

typedef struct {
    uint16_t flags;        /* cfl (bits 0-4), plus A/W/P */
    uint16_t prdtl;
    volatile uint32_t prdbc;
    uint32_t ctba;
    uint32_t ctbau;
    uint32_t reserved[4];
} hba_cmd_header_t;

typedef struct {
    uint32_t dba;
    uint32_t dbau;
    uint32_t reserved;
    uint32_t dbc;          /* bit 31 = interrupt on completion; count is n-1 */
} hba_prdt_entry_t;

typedef struct {
    uint8_t cfis[64];
    uint8_t acmd[16];
    uint8_t reserved[48];
    hba_prdt_entry_t prdt[8];
} hba_cmd_table_t;

static hba_cmd_header_t g_cmd_list[32] __attribute__((aligned(1024)));
static uint8_t g_fis[256] __attribute__((aligned(256)));
static hba_cmd_table_t g_cmd_table __attribute__((aligned(128)));

/* A bounce buffer, because the caller's is not guaranteed to be anywhere the
 * controller can reach and this driver must not quietly DMA somewhere else. */
static uint8_t g_bounce[64u * 512u] __attribute__((aligned(4096)));

static volatile uint8_t *g_abar;
static uint32_t g_port = 0xFFFFFFFFu;
static int g_ready;

/* One controller, one command slot, one bounce buffer. virtio-blk shipped
 * without this and two cores did not race over a window, they overwrote each
 * other's requests and each returned the other's data, successfully. */
static volatile int g_lock;

static void ahci_lock(void) {
    while (__atomic_test_and_set(&g_lock, __ATOMIC_ACQUIRE)) {
        __asm__ __volatile__("pause");
    }
}

static void ahci_unlock(void) {
    __atomic_clear(&g_lock, __ATOMIC_RELEASE);
}

static uint32_t mmio_read(uint32_t off) {
    return *(volatile uint32_t *)(g_abar + off);
}

static void mmio_write(uint32_t off, uint32_t value) {
    *(volatile uint32_t *)(g_abar + off) = value;
}

static uint32_t port_read(uint32_t off) {
    return mmio_read(PORT_BASE(g_port) + off);
}

static void port_write(uint32_t off, uint32_t value) {
    mmio_write(PORT_BASE(g_port) + off, value);
}

/* ---- bring-up ------------------------------------------------------------ */

/* Find an AHCI controller by class code and return its ABAR, or 0. */
static uint64_t ahci_find(void) {
    uint16_t bus, dev;
    uint8_t fn;

    for (bus = 0; bus < 256u; bus++) {
        for (dev = 0; dev < 32u; dev++) {
            for (fn = 0; fn < 8u; fn++) {
                uint32_t id = pci_read32((uint8_t)bus, (uint8_t)dev, fn, 0x00);
                uint32_t cls, cmd, bar5;

                if ((id & 0xFFFFu) == 0xFFFFu) {
                    if (fn == 0) {
                        break;   /* no device here at all */
                    }
                    continue;
                }
                cls = pci_read32((uint8_t)bus, (uint8_t)dev, fn, 0x08);
                /* class 0x01 mass storage, subclass 0x06 SATA, prog-if 0x01
                 * AHCI 1.0. Matching this rather than a vendor/device pair is
                 * what makes the same driver work on QEMU, VirtualBox and
                 * VMware, which emulate three different chips. */
                if (((cls >> 24) & 0xFFu) != 0x01u ||
                    ((cls >> 16) & 0xFFu) != 0x06u ||
                    ((cls >> 8) & 0xFFu) != 0x01u) {
                    continue;
                }
                /* Memory space + bus mastering. Without the second the
                 * controller reads no descriptors and simply never completes,
                 * which looks like a hang rather than a refusal. */
                cmd = pci_read32((uint8_t)bus, (uint8_t)dev, fn, 0x04);
                pci_write32((uint8_t)bus, (uint8_t)dev, fn, 0x04, cmd | 0x6u);

                bar5 = pci_read32((uint8_t)bus, (uint8_t)dev, fn, 0x24);
                if ((bar5 & 1u) != 0u) {
                    continue;   /* an I/O BAR: not the ABAR */
                }
                return (uint64_t)(bar5 & 0xFFFFFFF0u);
            }
        }
    }
    return 0;
}

/* Stop the port's engines so its pointers can be moved. */
static int port_stop(void) {
    uint32_t spin = 0;

    port_write(PxCMD, port_read(PxCMD) & ~(uint32_t)PxCMD_ST);
    port_write(PxCMD, port_read(PxCMD) & ~(uint32_t)PxCMD_FRE);
    while ((port_read(PxCMD) & (PxCMD_CR | PxCMD_FR)) != 0u) {
        if (++spin > 1000000u) {
            return -1;
        }
    }
    return 0;
}

static void port_start(void) {
    uint32_t spin = 0;
    while ((port_read(PxCMD) & PxCMD_CR) != 0u && ++spin < 1000000u) {
    }
    port_write(PxCMD, port_read(PxCMD) | PxCMD_FRE);
    port_write(PxCMD, port_read(PxCMD) | PxCMD_ST);
}

int vibeos_x86_64_ahci_init(void) {
    uint64_t abar = ahci_find();
    uint32_t ports, p;

    if (abar == 0) {
        vibeos_x86_64_serial_puts("[AHCI] no AHCI controller on the PCI bus\n");
        return -1;
    }

    /* The identity map covers the first 4 GiB with 2 MiB write-back pages, and
     * this range is registers rather than memory: reads have side effects and
     * must not be served from a cache line. Marking it uncacheable is what
     * makes the completion poll below observe the controller instead of a
     * stale copy of it. */
    vibeos_x86_64_mark_uncacheable(abar, 0x2000ull);

    g_abar = (volatile uint8_t *)(uintptr_t)abar;
    mmio_write(HBA_GHC, mmio_read(HBA_GHC) | HBA_GHC_AE);

    ports = mmio_read(HBA_PI);
    for (p = 0; p < 32u; p++) {
        uint32_t ssts, det, ipm, sig;

        if ((ports & (1u << p)) == 0u) {
            continue;
        }
        g_port = p;
        ssts = port_read(PxSSTS);
        det = ssts & 0x0Fu;
        ipm = (ssts >> 8) & 0x0Fu;
        /* det 3: a device is there and the link is up. ipm 1: it is awake.
         * Anything else is a port that exists in the register and has nothing
         * usable behind it. */
        if (det != 3u || ipm != 1u) {
            continue;
        }
        sig = port_read(PxSIG);
        if (sig != 0x00000101u) {
            continue;   /* not a plain SATA disk (0xEB140101 is ATAPI) */
        }

        if (port_stop() != 0) {
            continue;
        }
        {
            uint32_t i;
            for (i = 0; i < sizeof(g_fis); i++) {
                g_fis[i] = 0;
            }
            for (i = 0; i < sizeof(g_cmd_list) / sizeof(g_cmd_list[0]); i++) {
                g_cmd_list[i].flags = 0;
                g_cmd_list[i].prdtl = 0;
                g_cmd_list[i].prdbc = 0;
                g_cmd_list[i].ctba = 0;
                g_cmd_list[i].ctbau = 0;
            }
        }
        port_write(PxCLB, (uint32_t)(uintptr_t)g_cmd_list);
        port_write(PxCLBU, 0);
        port_write(PxFB, (uint32_t)(uintptr_t)g_fis);
        port_write(PxFBU, 0);
        port_write(PxSERR, 0xFFFFFFFFu);   /* write-1-to-clear */
        port_write(PxIS, 0xFFFFFFFFu);
        port_start();

        g_ready = 1;
        vibeos_x86_64_serial_puts("[AHCI] port ready: 0x");
        vibeos_x86_64_serial_print_hex((uint64_t)p);
        vibeos_x86_64_serial_puts(" abar=0x");
        vibeos_x86_64_serial_print_hex(abar);
        vibeos_x86_64_serial_puts("\n");
        return 0;
    }

    vibeos_x86_64_serial_puts("[AHCI] controller found but no SATA disk on any port\n");
    g_port = 0xFFFFFFFFu;
    return -1;
}

/* ---- transfers ----------------------------------------------------------- */

/* Build and run one command. Returns 0 on success. */
/* One command, whatever it is.
 *
 * Generalised from a read/write-only version so that IDENTIFY does not have to
 * duplicate forty lines of FIS construction. Two things this file has already
 * learned the hard way are in that construction - the PRDT byte count is one
 * less than the bytes, and the error bit has to be checked inside the wait
 * rather than after it - and a second copy would have to learn them again. */
static int ahci_cmd(uint8_t command, uint64_t lba, uint32_t sectors,
                    int write, uint32_t bytes) {
    hba_cmd_header_t *hdr = &g_cmd_list[0];
    hba_cmd_table_t *tbl = &g_cmd_table;
    uint8_t *fis = tbl->cfis;
    uint32_t i, spin;

    for (i = 0; i < sizeof(g_cmd_table); i++) {
        ((uint8_t *)tbl)[i] = 0;
    }

    hdr->flags = (uint16_t)((5u & 0x1Fu) |          /* H2D FIS is 5 dwords */
                            (write ? 0x40u : 0u));  /* W: guest -> device  */
    hdr->prdtl = 1;
    hdr->prdbc = 0;
    hdr->ctba = (uint32_t)(uintptr_t)tbl;
    hdr->ctbau = 0;

    tbl->prdt[0].dba = (uint32_t)(uintptr_t)g_bounce;
    tbl->prdt[0].dbau = 0;
    /* The count is bytes minus one, and a driver that writes the plain byte
     * count transfers one byte too many - which on a 512-byte read means the
     * sector after it, silently. */
    tbl->prdt[0].dbc = bytes - 1u;

    fis[0] = 0x27;                       /* register FIS, host to device */
    fis[1] = 0x80;                       /* C: this is a command         */
    fis[2] = command;
    fis[4] = (uint8_t)(lba & 0xFFu);
    fis[5] = (uint8_t)((lba >> 8) & 0xFFu);
    fis[6] = (uint8_t)((lba >> 16) & 0xFFu);
    fis[7] = 0x40;                       /* LBA mode */
    fis[8] = (uint8_t)((lba >> 24) & 0xFFu);
    fis[9] = (uint8_t)((lba >> 32) & 0xFFu);
    fis[10] = (uint8_t)((lba >> 40) & 0xFFu);
    fis[12] = (uint8_t)(sectors & 0xFFu);
    fis[13] = (uint8_t)((sectors >> 8) & 0xFFu);

    /* Wait for the port to go idle before handing it anything. */
    spin = 0;
    while ((port_read(PxTFD) & (ATA_DEV_BUSY | ATA_DEV_DRQ)) != 0u) {
        if (++spin > 1000000u) {
            return -1;
        }
    }

    port_write(PxIS, 0xFFFFFFFFu);
    port_write(PxCI, 1u);   /* slot 0 */

    spin = 0;
    for (;;) {
        if ((port_read(PxCI) & 1u) == 0u) {
            break;
        }
        /* TFD.ERR, checked inside the loop rather than after it: a command
         * that fails leaves CI set on some controllers, and waiting for a bit
         * that will never clear is a hang rather than an error. */
        if ((port_read(PxTFD) & 0x01u) != 0u) {
            return -1;
        }
        if (++spin > 20000000u) {
            return -1;
        }
    }
    if ((port_read(PxTFD) & 0x01u) != 0u) {
        return -1;
    }
    return 0;
}

static int ahci_xfer(uint64_t lba, uint32_t sectors, int write) {
    return ahci_cmd(write ? (uint8_t)ATA_CMD_WRITE_DMA_EX
                          : (uint8_t)ATA_CMD_READ_DMA_EX,
                    lba, sectors, write, sectors * 512u);
}

/* How big the disk is, or 0 if the device would not say.
 *
 * Nothing asked until now, and that is the defect rather than an omission:
 * every AHCI read has been unbounded, so an LBA past the end of the medium
 * went straight to the controller. The block layer refuses to register a
 * device that cannot state its size, because a device with no size cannot
 * have its requests bounds-checked - and an unchecked bound is the difference
 * between an error and a disk written at the wrong offset.
 *
 * LBA48 first, LBA28 second, and both are read rather than assuming the
 * first: word 83 bit 10 says whether the 48-bit fields mean anything, and a
 * driver that reads them unconditionally gets a plausible number from a disk
 * that never filled them in. */
static uint64_t ahci_identify_sectors(void) {
    const uint8_t *id = g_bounce;
    uint64_t lba48;
    uint32_t lba28;
    uint32_t i;

    for (i = 0; i < 512u; i++) {
        g_bounce[i] = 0;
    }
    if (ahci_cmd((uint8_t)ATA_CMD_IDENTIFY, 0, 0, 0, 512u) != 0) {
        return 0;
    }

    /* Little-endian 16-bit words, assembled by hand: this runs before
     * anything has established that the structure is aligned or that the
     * compiler will not widen a load across a word boundary. */
    if ((((uint32_t)id[83 * 2] | ((uint32_t)id[83 * 2 + 1] << 8)) & (1u << 10))
            != 0u) {
        lba48 = 0;
        for (i = 0; i < 8u; i++) {
            lba48 |= (uint64_t)id[100 * 2 + i] << (8u * i);
        }
        if (lba48 != 0ull) {
            return lba48;
        }
    }
    lba28 = 0;
    for (i = 0; i < 4u; i++) {
        lba28 |= (uint32_t)id[60 * 2 + i] << (8u * i);
    }
    return (uint64_t)lba28;
}

uint64_t vibeos_x86_64_ahci_sectors(void) {
    if (!g_ready) {
        return 0;
    }
    return ahci_identify_sectors();
}

int vibeos_x86_64_ahci_read_many(uint64_t lba, void *buf, uint32_t sectors) {
    uint8_t *out = (uint8_t *)buf;
    uint32_t done = 0;

    if (!g_ready || sectors == 0u) {
        return -1;
    }
    ahci_lock();
    while (done < sectors) {
        uint32_t chunk = sectors - done;
        uint32_t i;

        if (chunk > sizeof(g_bounce) / 512u) {
            chunk = sizeof(g_bounce) / 512u;
        }
        if (ahci_xfer(lba + done, chunk, 0) != 0) {
            ahci_unlock();
            return -1;
        }
        for (i = 0; i < chunk * 512u; i++) {
            out[(uint64_t)done * 512ull + i] = g_bounce[i];
        }
        done += chunk;
    }
    ahci_unlock();
    return 0;
}

int vibeos_x86_64_ahci_read(uint64_t lba, void *buf) {
    return vibeos_x86_64_ahci_read_many(lba, buf, 1u);
}

int vibeos_x86_64_ahci_write(uint64_t lba, const void *buf) {
    const uint8_t *in = (const uint8_t *)buf;
    uint32_t i;
    int rc;

    if (!g_ready) {
        return -1;
    }
    ahci_lock();
    for (i = 0; i < 512u; i++) {
        g_bounce[i] = in[i];
    }
    rc = ahci_xfer(lba, 1u, 1);
    ahci_unlock();
    return rc;
}
