/* Storage and I/O bring-up: what the machine does with its disks at boot.
 *
 * Cut out of arch_hw.c, which had grown to eleven thousand lines - two and a
 * half thousand of them added in the few days this plan was supposed to be
 * making it smaller. The extraction plan says the length of that file is the
 * completion criterion and not a side effect, and this is the first cut that
 * pays back debt created after the plan was written rather than before it.
 *
 * What lives here: swap bring-up, the write proof, the on-disk log sink and
 * its console tail, the filesystem images that had never been mounted, the
 * scratch device, the volume scan and the mount report. All of it runs once,
 * from the boot path, and none of it is reached from a syscall - which is why
 * it separates cleanly and why it should not have been in there in the first
 * place.
 *
 * The seam is deliberately narrow: eight names in from arch_hw.c and seven
 * out. They are declared in arch_hw_internal.h rather than duplicated here,
 * for the reason cut 1 established - a second declaration is a second thing to
 * keep in step.
 */

#include "vibeos/arch_x86_64.h"
#include "vibeos/blkdev.h"
#include "vibeos/exfat.h"
#include "vibeos/ext2.h"
#include "vibeos/anon.h"
#include "vibeos/frame.h"
#include "vibeos/io_stats.h"
#include "vibeos/iso9660.h"
#include "vibeos/logsink.h"
#include "vibeos/ntfs.h"
#include "vibeos/parttab.h"
#include "vibeos/partition.h"
#include "vibeos/reclaim.h"
#include "vibeos/storage.h"
#include "vibeos/swaparea.h"
#include "vibeos/swapmap.h"
#include "vibeos/vfs.h"

#include "arch_hw_internal.h"

/* Supplied by fat_vfs.c, which registers the FAT driver. Declared here rather
 * than in the shared header because only this file uses them, and a name in a
 * shared header is a name every future file has to reason about. */
extern int (*g_fat_driver_probe)(vibeos_blockcache_t *cache, uint64_t first_lba);
extern int (*g_fat_driver_format)(vibeos_blockcache_t *cache, uint64_t first_lba,
                                  uint64_t sectors);

/* ---- swap ---------------------------------------------------------------- */

/* The block move the swap area is given, and the only way it reaches a disk.
 *
 * Routed through vibeos_blk_read/write rather than the driver, so a swap
 * transfer is bounds-checked against the device exactly like every other
 * request and shows up in the same counters. A swap path with its own private
 * road to the hardware would be a second definition of what a block request
 * means, which is how this project got its worst bugs. */
static int hw_swap_block(void *ctx, uint32_t device, uint64_t lba,
                         uint32_t count, void *buf, int write) {
    (void)ctx;
    /* The wrappers return 0 or -1; the reason lands in the io stats, which the
     * boot gate already asserts. Nothing is added here, deliberately - a swap
     * transfer that failed is not a different kind of failure from any other
     * block request, and giving it its own vocabulary would be a second
     * definition of what a block error means. */
    if (write) {
        return vibeos_blk_write(device, lba, count, buf) == 0 ? 0 : -1;
    }
    return vibeos_blk_read(device, lba, count, buf) == 0 ? 0 : -1;
}

/* ---- writes that are proved (I4 step 2) -----------------------------------
 *
 * A boot writes to this disk - about thirty sectors, through the shell's
 * `mkdir DOCS` - and until now *nothing checked any of it*. That is worse than
 * not writing at all: the machine modifies a medium with no evidence that what
 * it wrote is what comes back, and a defect there is silent until some later
 * boot cannot mount.
 *
 * So: write a file, read it back, compare byte for byte.
 *
 * ## What the pattern is, and why it is not a constant
 *
 * Each byte carries its own offset. A constant survives every interesting
 * failure this can have - a write that landed one sector early, a read that
 * returned a neighbouring sector, a multi-sector transfer that lost its last
 * sector and left the previous contents - because all of those hand back bytes
 * that are equal to what was expected. An offset-dependent pattern fails all
 * of them, and says *where*.
 *
 * The size crosses a sector boundary and is not a multiple of one. A file that
 * is exactly N sectors never exercises the tail, and the tail is where a
 * length confused with a byte count shows up - which this project has already
 * had once, in the FAT reader that returned the size the directory claimed.
 *
 * ## What it does not prove
 *
 * That the bytes reached the *medium*. Everything here could be served from
 * the block cache, and with I2's write-through policy the device was written
 * too - but this check cannot tell the difference. Only a reboot can, and that
 * is step 3.
 */
/* At the root, and not in a subdirectory, for a reason about the medium
 * rather than about the kernel.
 *
 * This runs at mount time, and at mount time the root holds exactly EFI and
 * STARTUP.NSH: the `docs` directory staged on the host is not presented to the
 * guest at all, and DOCS/NOTES.TXT exists later only because the boot script
 * creates it. The first version wrote to DOCS/ and was refused for the honest
 * reason that the parent did not exist yet - which the refusal now says in
 * those words.
 *
 * It is not a weaker test. The property being checked is that bytes written to
 * this medium come back, and the root exercises the same allocation, the same
 * chain walk and the same directory update. A subdirectory adds a second
 * directory lookup and nothing else, and I4b brings the volume work that would
 * make it worth testing separately. */
#define HW_WRITE_PROOF_PATH  "WRPROOF.BIN"
#define HW_WRITE_PROOF_BYTES 1300u

static uint8_t g_write_proof[HW_WRITE_PROOF_BYTES];

static uint8_t hw_write_proof_byte(uint32_t i) {
    /* Two terms, so neither a shift of the whole file nor a swap of two
     * sectors can produce a matching run. */
    return (uint8_t)((i * 7u) ^ (i >> 8) ^ 0x5Au);
}

void hw_write_proof(void) {
    const char *verdict = "not attempted";
    uint32_t i;
    long n;

    for (i = 0; i < HW_WRITE_PROOF_BYTES; i++) {
        g_write_proof[i] = hw_write_proof_byte(i);
    }
    if (vibeos_fs_write_file(&g_rootfs, HW_WRITE_PROOF_PATH, g_write_proof,
                             HW_WRITE_PROOF_BYTES) < 0) {
        vibeos_x86_64_serial_lock();
        vibeos_x86_64_serial_puts("[IO] WRITE_PROOF write refused: ");
        vibeos_x86_64_serial_puts(vibeos_x86_64_fat_write_why());
        vibeos_x86_64_serial_puts("\n");
        vibeos_x86_64_serial_unlock();
        verdict = "FAILED: write refused";
    } else if (vibeos_blk_barrier((uint32_t)vibeos_x86_64_blk_device()) != 0) {
        /* Asked for, and the answer believed. A barrier the device refused
         * means the bytes may still be in its volatile cache, so a read that
         * follows proves nothing about the medium - and reporting OK here
         * would be the check lying about the one thing it exists to say. */
        verdict = "FAILED: the device would not give a barrier";
    } else {
        /* Cleared first, so a read that returns nothing at all cannot pass by
         * leaving the buffer holding what was just written to it. */
        for (i = 0; i < HW_WRITE_PROOF_BYTES; i++) {
            g_write_proof[i] = 0;
        }
        n = vibeos_fs_read_file(&g_rootfs, HW_WRITE_PROOF_PATH, g_write_proof,
                                HW_WRITE_PROOF_BYTES);
        if (n != (long)HW_WRITE_PROOF_BYTES) {
            verdict = "FAILED: read gave the wrong length";
        } else {
            int bad = -1;
            for (i = 0; i < HW_WRITE_PROOF_BYTES; i++) {
                if (g_write_proof[i] != hw_write_proof_byte(i)) {
                    bad = (int)i;
                    break;
                }
            }
            verdict = (bad < 0) ? "OK" : "FAILED: contents differ";
            if (bad >= 0) {
                vibeos_x86_64_serial_lock();
                vibeos_x86_64_serial_puts("[IO] WRITE_PROOF first_bad_offset=0x");
                vibeos_x86_64_serial_print_hex((uint64_t)(uint32_t)bad);
                vibeos_x86_64_serial_puts(" got=0x");
                vibeos_x86_64_serial_print_hex((uint64_t)g_write_proof[bad]);
                vibeos_x86_64_serial_puts(" want=0x");
                vibeos_x86_64_serial_print_hex(
                    (uint64_t)hw_write_proof_byte((uint32_t)bad));
                vibeos_x86_64_serial_puts("\n");
                vibeos_x86_64_serial_unlock();
            }
        }
    }

    vibeos_x86_64_serial_lock();
    vibeos_x86_64_serial_puts("[IO] WRITE_PROOF bytes=0x");
    vibeos_x86_64_serial_print_hex((uint64_t)HW_WRITE_PROOF_BYTES);
    vibeos_x86_64_serial_puts(" ");
    vibeos_x86_64_serial_puts(verdict);
    vibeos_x86_64_serial_puts("\n");
    vibeos_x86_64_serial_unlock();
}

/* Every mount, once everything has attached.
 *
 * A function rather than a block inside one of the bring-ups, because it has
 * to run after all of them and the first version ran inside the scratch one -
 * which reported one mount on a machine that had two, and would have reported
 * two on a machine that has three. */
/* The tail of the on-disk log, newest first.
 *
 * Printed rather than returned: the caller is a console command and the point
 * is to be read by a person looking at a machine that has just come back.
 *
 * Records from *this* boot are shown too, because there is no clean line
 * between them - the sequence numbers are continuous across a reset, which is
 * the property that makes the medium worth having. The sequence is printed so
 * a reader can see where one machine stopped and the next started. */
void vibeos_x86_64_logdisk_tail(uint32_t want) {
    vibeos_logsink_record_t r;
    uint32_t i;
    uint32_t shown = 0;

    vibeos_x86_64_serial_lock();
    vibeos_x86_64_serial_puts("[LOGDISK] newest first, capacity=0x");
    vibeos_x86_64_serial_print_hex(vibeos_logsink_capacity());
    vibeos_x86_64_serial_puts("\n");
    vibeos_x86_64_serial_unlock();

    for (i = 0; i < want; i++) {
        char text[VIBEOS_LOGSINK_PAYLOAD + 1u];
        uint32_t k;

        if (vibeos_logsink_read(i, &r) != 0) {
            break;
        }
        for (k = 0; k < r.len && k < VIBEOS_LOGSINK_PAYLOAD; k++) {
            text[k] = (r.text[k] >= 32u && r.text[k] < 127u)
                    ? (char)r.text[k] : '.';
        }
        text[k] = 0;
        vibeos_x86_64_serial_lock();
        vibeos_x86_64_serial_puts("[LOGDISK] seq=0x");
        vibeos_x86_64_serial_print_hex(r.seq);
        vibeos_x86_64_serial_puts(" ");
        vibeos_x86_64_serial_puts(text);
        vibeos_x86_64_serial_puts("\n");
        vibeos_x86_64_serial_unlock();
        shown++;
    }

    vibeos_x86_64_serial_lock();
    vibeos_x86_64_serial_puts("[LOGDISK] shown=0x");
    vibeos_x86_64_serial_print_hex(shown);
    vibeos_x86_64_serial_puts(" written=0x");
    vibeos_x86_64_serial_print_hex(vibeos_logsink_stats()->records_written);
    vibeos_x86_64_serial_puts(" failed=0x");
    vibeos_x86_64_serial_print_hex(vibeos_logsink_stats()->write_failed);
    vibeos_x86_64_serial_puts(" truncated=0x");
    vibeos_x86_64_serial_print_hex(vibeos_logsink_stats()->truncated);
    vibeos_x86_64_serial_puts("\n");
    vibeos_x86_64_serial_unlock();
}

void hw_mount_report(void) {
    uint32_t k;
    for (k = 0; k < vibeos_fs_mount_count(); k++) {
        vibeos_x86_64_serial_lock();
        vibeos_x86_64_serial_puts("[IO] MOUNTED at=");
        vibeos_x86_64_serial_puts(vibeos_fs_mount_path(k));
        vibeos_x86_64_serial_puts(" type=");
        vibeos_x86_64_serial_puts(vibeos_fs_type(vibeos_fs_mount_at(k)));
        vibeos_x86_64_serial_puts("\n");
        vibeos_x86_64_serial_unlock();
    }
}

/* ---- the kernel log, on a medium that outlives the machine (I5b) ----------
 *
 * The second disk, not the boot one. A log that lives on the filesystem is
 * unwritable exactly when it is most wanted, and that is not a hypothetical
 * here: every hard defect in this project was diagnosed from a serial log, and
 * on an appliance with no serial cable a wedge currently leaves nothing at all.
 *
 * Straight at the block layer, never through the block cache. A cache holding
 * the last few lines when the power goes is the one failure this cannot have.
 */
static int g_logsink_dev = -1;

static int hw_logsink_read(void *ctx, uint64_t lba, void *buf) {
    (void)ctx;
    return (g_logsink_dev < 0)
         ? -1 : vibeos_blk_read((uint32_t)g_logsink_dev, lba, 1u, buf);
}

static int hw_logsink_read_many(void *ctx, uint64_t lba, void *buf,
                                uint32_t sectors) {
    (void)ctx;
    return (g_logsink_dev < 0)
         ? -1 : vibeos_blk_read((uint32_t)g_logsink_dev, lba, sectors, buf);
}

static int hw_logsink_write(void *ctx, uint64_t lba, const void *buf) {
    (void)ctx;
    return (g_logsink_dev < 0)
         ? -1 : vibeos_blk_write((uint32_t)g_logsink_dev, lba, 1u, buf);
}

void hw_logsink_bringup(void) {
    const char *verdict = "no second disk on this machine";
    vibeos_logsink_dev_t dev;
    vibeos_logsink_record_t prev;
    vibeos_blk_driver_t info;
    int have_prev = 0;
    int dev_no;

    /* Adapter 1: the second disk that bound. Adapter 0 is the boot disk and is
     * deliberately not eligible - a log on the medium the machine is running
     * from is the arrangement this phase exists to stop. */
    dev_no = (vibeos_x86_64_blk_adapter_count() > 1u)
           ? vibeos_x86_64_blk_adapter_device(1u) : -1;
    if (dev_no >= 0 && vibeos_blk_info((uint32_t)dev_no, &info) == 0) {
        g_logsink_dev = dev_no;
        vibeos_logsink_set_cpu_id(vibeos_x86_64_cpu_id);
        dev.read = hw_logsink_read;
        dev.read_many = hw_logsink_read_many;
        dev.write = hw_logsink_write;
        dev.ctx = 0;
        dev.sectors = info.sectors;

        verdict = "FAILED: attach";
        if (vibeos_logsink_attach(&dev) == 0) {
            /* What the previous machine left, read *before* this boot writes
             * anything - otherwise the newest record is this boot own and the
             * check proves only that a write followed by a read works. */
            have_prev = (vibeos_logsink_read(0, &prev) == 0);

            verdict = "FAILED: write";
            if (vibeos_logsink_write("VIBEOS boot mark", 16u) == 0) {
                verdict = "OK";
            }
        }
    }

    vibeos_x86_64_serial_lock();
    vibeos_x86_64_serial_puts("[IO] LOGSINK result=");
    vibeos_x86_64_serial_puts(verdict);
    vibeos_x86_64_serial_puts(" capacity=0x");
    vibeos_x86_64_serial_print_hex(vibeos_logsink_capacity());
    vibeos_x86_64_serial_puts(" prev_seq=0x");
    vibeos_x86_64_serial_print_hex(vibeos_logsink_stats()->highest_seq_seen);
    vibeos_x86_64_serial_puts(" written=0x");
    vibeos_x86_64_serial_print_hex(vibeos_logsink_stats()->records_written);
    vibeos_x86_64_serial_puts(" failed=0x");
    vibeos_x86_64_serial_print_hex(vibeos_logsink_stats()->write_failed);
    vibeos_x86_64_serial_puts(" bad=0x");
    vibeos_x86_64_serial_print_hex(vibeos_logsink_stats()->bad_records);
    /* The previous boot last line, which is the entire point of the feature.
     * "previous=none" on a fresh medium, and the gate reads the difference
     * between the two runs rather than either one on its own. */
    vibeos_x86_64_serial_puts(" previous=");
    if (have_prev) {
        uint32_t k;
        for (k = 0; k < prev.len && k < 64u; k++) {
            char c[2];
            c[0] = (prev.text[k] >= 32u && prev.text[k] < 127u)
                 ? (char)prev.text[k] : '.';
            c[1] = 0;
            vibeos_x86_64_serial_puts(c);
        }
    } else {
        vibeos_x86_64_serial_puts("none");
    }
    vibeos_x86_64_serial_puts("\n");
    vibeos_x86_64_serial_unlock();
}

/* ---- filesystems that had never run (I5) ----------------------------------
 *
 * ext2 and ISO9660, mounted from images the *host's* own mkfs tools built, and
 * read from.
 *
 * The point of the phase is that these drivers had never been given anything
 * to parse. An image this project wrote itself would only prove the driver and
 * the writer agree with each other - and they would, because the same
 * misreading of the layout goes into both. The one artefact neither side of
 * the test controls is the one worth mounting.
 *
 * Attached through the loop device rather than as second disks, because a
 * second physical device needs virtio-blk to stop being a singleton.
 *
 * The file each image carries has its own offset in every byte, so a read that
 * returned the wrong block cannot match - which a file full of a constant
 * would.
 *
 * A table rather than one function per filesystem. The first version was one
 * function, and the second filesystem would have been a copy of it with four
 * names changed - which is how a check ends up asserted for one member of a
 * family and not the others.
 */
#define HW_FSIMAGE_SLOTS 8u

typedef struct hw_fsimage {
    const char *name;
    const char *image;
    const char *at;
    /* The file to read back, or 0 when this filesystem image cannot be given
     * one. exFAT is the case: exfatprogs ships no tool that writes into an
     * image without mounting it, and mounting needs root and FUSE. Mounting a
     * real mkfs.exfat volume and reading its root is still far more than that
     * driver had ever done; claiming a byte comparison that did not happen
     * would be worse than the gap, so the row says so and the boot reports
     * "OK (no marker)" rather than "OK". */
    const char *marker;
    int (*mount)(struct hw_fsimage *e);
    const vibeos_fs_ops_t *(*ops)(void);
    void *fs;

    uint8_t slot_data[HW_FSIMAGE_SLOTS][512];
    vibeos_block_slot_t slots[HW_FSIMAGE_SLOTS];
    vibeos_blockdev_t dev;
    vibeos_blockcache_t bc;
    vibeos_fsmount_t mnt;
    int device;
} hw_fsimage_t;

static vibeos_ext2_t g_ext2;
static vibeos_iso9660_t g_iso;
static vibeos_ntfs_t g_ntfs;
static vibeos_exfat_t g_exfat;

static int hw_fsimage_read(void *ctx, uint64_t lba, void *buf) {
    hw_fsimage_t *e = (hw_fsimage_t *)ctx;
    return (e == 0 || e->device < 0)
         ? -1 : vibeos_blk_read((uint32_t)e->device, lba, 1u, buf);
}

/* The loop device is read-only, and says so here rather than dropping the
 * write. A device that accepts a write it does not perform tells the caller
 * its bytes are safe. */
static int hw_fsimage_write(void *ctx, uint64_t lba, const void *buf) {
    (void)ctx; (void)lba; (void)buf;
    return -1;
}

/* One wrapper each, rather than casting the mount functions to a common type:
 * the three drivers take different filesystem structs, and a function-pointer
 * cast that happens to work is exactly the kind of thing this file's own notes
 * say goes wrong quietly. */
static int hw_mount_ext2(hw_fsimage_t *e) {
    return vibeos_ext2_mount(&g_ext2, &e->bc, 0ull);
}

static int hw_mount_iso9660(hw_fsimage_t *e) {
    return vibeos_iso9660_mount(&g_iso, &e->bc, 0ull);
}

static int hw_mount_ntfs(hw_fsimage_t *e) {
    return vibeos_ntfs_mount(&g_ntfs, &e->bc, 0ull);
}

static int hw_mount_exfat(hw_fsimage_t *e) {
    return vibeos_exfat_mount(&g_exfat, &e->bc, 0ull);
}

static hw_fsimage_t g_fsimages[] = {
    { "ext2",    "EFI/BOOT/EXT2.IMG", "/ext2", "HELLO.TXT",
      hw_mount_ext2,    vibeos_ext2_ops,    &g_ext2,
      {{0}}, {{0}}, {0}, {0}, {0}, -1 },
    { "iso9660", "EFI/BOOT/ISO.IMG",  "/iso",  "HELLO.TXT",
      hw_mount_iso9660, vibeos_iso9660_ops, &g_iso,
      {{0}}, {{0}}, {0}, {0}, {0}, -1 },
    { "ntfs",    "EFI/BOOT/NTFS.IMG", "/ntfs", "HELLO.TXT",
      hw_mount_ntfs,    vibeos_ntfs_ops,    &g_ntfs,
      {{0}}, {{0}}, {0}, {0}, {0}, -1 },
    { "exfat",   "EFI/BOOT/EXFAT.IMG", "/exfat", 0,
      hw_mount_exfat,   vibeos_exfat_ops,   &g_exfat,
      {{0}}, {{0}}, {0}, {0}, {0}, -1 },
};

static long g_fsimage_got;

static void hw_fsimage_bringup(hw_fsimage_t *e) {
    static uint8_t rd[4096];
    const char *verdict = "no image";
    uint64_t sectors = 0;
    int dev;
    uint32_t i;

    /* Cleared by the function that owns the contract, not at each place that
     * sets it. The exFAT row never reads a file, so without this it reported
     * the *previous* row byte count - a field written on one path and read on
     * all of them, which is a trap this project has paid for before. */
    g_fsimage_got = 0;

    dev = vibeos_x86_64_loop_attach(e->image, &sectors);
    if (dev < 0) {
        /* Say which refusal it was. The first version reported every one of
         * them as "no image on this medium", and for a boot that sentence was
         * false: two images were on the disk and the loop device had run out
         * of slots. */
        verdict = vibeos_x86_64_loop_why();
    }
    if (dev >= 0) {
        e->device = dev;
        for (i = 0; i < HW_FSIMAGE_SLOTS; i++) {
            e->slots[i].data = e->slot_data[i];
        }
        e->dev.read = hw_fsimage_read;
        e->dev.write = hw_fsimage_write;
        e->dev.flush = 0;
        e->dev.ctx = e;
        e->dev.sectors = sectors;

        verdict = "FAILED: cache";
        if (vibeos_blockcache_init(&e->bc, &e->dev, e->slots,
                                   HW_FSIMAGE_SLOTS) == 0) {
            verdict = "FAILED: mount";
            if (e->mount(e) == 0 &&
                vibeos_fs_mount(&e->mnt, e->ops(), e->fs, e->name) == 0) {
                long got;

                if (e->marker == 0) {
                    /* Mounted, and there is nothing to read back. Said
                     * distinctly rather than folded into OK: the gate should
                     * be able to tell a driver that parsed a superblock from
                     * one that also returned a file. */
                    verdict = "OK (no marker)";
                    (void)vibeos_fs_attach(e->at, &e->mnt);
                    goto report;
                }
                verdict = "FAILED: read";
                for (i = 0; i < sizeof(rd); i++) {
                    rd[i] = 0;
                }
                got = vibeos_fs_read_file(&e->mnt, e->marker, rd, sizeof(rd));
                /* A short read and a missing file are different failures and
                 * used to arrive as the same word. */
                g_fsimage_got = got;
                if (got == (long)sizeof(rd)) {
                    int same = 1;
                    for (i = 0; i < sizeof(rd); i++) {
                        uint8_t want = (uint8_t)(((i * 7u) ^ (i >> 8) ^ 0x5Au));
                        if (rd[i] != want) {
                            same = 0;
                            break;
                        }
                    }
                    verdict = same ? "OK" : "FAILED: contents differ";
                    if (same) {
                        (void)vibeos_fs_attach(e->at, &e->mnt);
                    }
                }
            }
        }
    }

report:
    vibeos_x86_64_serial_lock();
    vibeos_x86_64_serial_puts("[IO] FSIMAGE name=");
    vibeos_x86_64_serial_puts(e->name);
    vibeos_x86_64_serial_puts(" sectors=0x");
    vibeos_x86_64_serial_print_hex(sectors);
    vibeos_x86_64_serial_puts(" got=0x");
    vibeos_x86_64_serial_print_hex((uint64_t)g_fsimage_got);
    vibeos_x86_64_serial_puts(" result=");
    vibeos_x86_64_serial_puts(verdict);
    vibeos_x86_64_serial_puts("\n");
    vibeos_x86_64_serial_unlock();
}

void hw_fsimages_bringup(void) {
    uint32_t i;
    for (i = 0; i < sizeof(g_fsimages) / sizeof(g_fsimages[0]); i++) {
        hw_fsimage_bringup(&g_fsimages[i]);
    }
}

/* ---- a scratch device, and what it is for (I4c) ---------------------------
 *
 * I4c writes partition tables. The plan says, at the top of its own section,
 * that this is the one phase that can lose a user's data - so it gets a device
 * that is not anybody's disk.
 *
 * ## Why RAM and not a second disk
 *
 * A second QEMU disk was the first plan and it needs something else first:
 * every piece of virtio-blk's state is a global - the queue, the descriptors,
 * the request struct, the lock - so a second device means making that driver
 * per-instance. That is a real refactor of *the driver the machine boots
 * from*, and doing it inside a phase about writing partition tables would mean
 * two risky changes verified by one result.
 *
 * A RAM-backed device gives the same coverage where it matters. What I4c has
 * to prove is that the partition writer, the block layer and the cache do the
 * right thing; the driver underneath only moves sectors, and I4 already proves
 * that it moves them correctly. What this does *not* prove is that a real
 * medium keeps a table across a power cut, and that was never in reach of a
 * single boot anyway - it is the same gap I4 step 3 has.
 *
 * ## It is registered, so it is a real device
 *
 * Not a special case threaded through the writer. It goes through
 * vibeos_blk_register like any driver, gets a device number, and is
 * bounds-checked by the same code - which means the test exercises the path a
 * real disk would take rather than a shortcut built for it.
 */
/* 4 MiB.
 *
 * Sized by the format test rather than by the partition test. FAT12, FAT16 and
 * FAT32 are the same header and a reader tells them apart by cluster count
 * alone, so a partition small enough to land under 4085 clusters is silently a
 * FAT12 volume however carefully the fields were filled in. Six thousand
 * sectors puts it comfortably inside FAT16, and the formatter refuses anything
 * that would not. */
#define HW_SCRATCH_SECTORS 8192u

static uint8_t g_scratch[HW_SCRATCH_SECTORS][512];
static int g_scratch_device = -1;

static int hw_scratch_submit(void *ctx, vibeos_blk_request_t *req) {
    uint32_t i;

    (void)ctx;
    for (i = 0; i < req->sectors; i++) {
        uint64_t lba = req->lba + i;
        uint8_t *p = (uint8_t *)req->buf + (uint64_t)i * 512ull;
        if (lba >= HW_SCRATCH_SECTORS) {
            /* Reached only if the layer above let it through, which it does
             * not - the check is here because a driver that trusts its caller
             * is one bad caller away from writing past its own array. */
            req->sectors_done = i;
            req->result = VIBEOS_BLK_OUT_OF_RANGE;
            return -1;
        }
        if (req->write) {
            uint32_t k;
            for (k = 0; k < 512u; k++) {
                g_scratch[lba][k] = p[k];
            }
        } else {
            uint32_t k;
            for (k = 0; k < 512u; k++) {
                p[k] = g_scratch[lba][k];
            }
        }
    }
    req->sectors_done = req->sectors;
    return 0;
}

static int hw_scratch_read(void *ctx, uint64_t lba, void *buf) {
    (void)ctx;
    return (g_scratch_device < 0)
         ? -1
         : vibeos_blk_read((uint32_t)g_scratch_device, lba, 1u, buf);
}

static int hw_scratch_write(void *ctx, uint64_t lba, const void *buf) {
    (void)ctx;
    return (g_scratch_device < 0)
         ? -1
         : vibeos_blk_write((uint32_t)g_scratch_device, lba, 1u, buf);
}

static int hw_scratch_flush(void *ctx) {
    (void)ctx;
    return 0;   /* RAM has no volatile cache below it */
}

/* Partition the scratch device, read the table back, and say so.
 *
 * The round trip is the test: a writer that produces a table only it can read
 * is indistinguishable from a correct one until another tool looks at the
 * disk, and by then the disk is somebody's. So the write goes through
 * vibeos_parttab_write_mbr and the read back through the ordinary
 * vibeos_partition_parse_mbr, with no shared state between them.
 */
static uint8_t g_scratch_slot_data[8][512];
static vibeos_block_slot_t g_scratch_slots[8];
static vibeos_blockdev_t g_scratch_dev;
static vibeos_blockcache_t g_scratch_bc;

void hw_scratch_bringup(void) {
    vibeos_blk_driver_t drv;
    uint32_t device = 0;
    const char *verdict = "not attempted";
    uint32_t i;

    for (i = 0; i < 8u; i++) {
        g_scratch_slots[i].data = g_scratch_slot_data[i];
    }
    drv.name = "scratch-ram";
    drv.sector_bytes = 512u;
    drv.sectors = HW_SCRATCH_SECTORS;
    drv.submit = hw_scratch_submit;
    drv.barrier = 0;      /* nothing below it holds writes */
    drv.ctx = 0;
    if (vibeos_blk_register(&drv, &device) != 0) {
        return;
    }
    g_scratch_device = (int)device;

    g_scratch_dev.read = hw_scratch_read;
    g_scratch_dev.write = hw_scratch_write;
    g_scratch_dev.flush = hw_scratch_flush;
    g_scratch_dev.ctx = 0;
    g_scratch_dev.sectors = HW_SCRATCH_SECTORS;
    if (vibeos_blockcache_init(&g_scratch_bc, &g_scratch_dev,
                               g_scratch_slots, 8u) != 0) {
        return;
    }

    {
        vibeos_parttable_t want;
        vibeos_parttab_guard_t guard;
        uint32_t sum = 0;
        vibeos_parttab_result_t r;
        int table_ok = 0;

        for (i = 0; i < sizeof(want); i++) {
            ((uint8_t *)&want)[i] = 0;
        }
        for (i = 0; i < sizeof(guard); i++) {
            ((uint8_t *)&guard)[i] = 0;
        }
        /* A signature in the sector the table will share, so the round trip
         * also proves the writer edited it rather than replacing it - the
         * failure that quietly unbootables a disk it was asked to
         * repartition. */
        {
            uint8_t sec[512];
            for (i = 0; i < 512u; i++) {
                sec[i] = (uint8_t)(i ^ 0x3Cu);
            }
            (void)vibeos_blockcache_write(&g_scratch_bc, 0, sec);
            (void)vibeos_blockcache_flush(&g_scratch_bc);
        }

        want.count = 2;
        want.entry[0].first_lba = 64;   want.entry[0].sector_count = 6000;
        want.entry[0].mbr_type = 0x06u;   /* FAT16 */
        want.entry[1].first_lba = 6144; want.entry[1].sector_count = 1024;
        want.entry[1].mbr_type = 0x0Cu;

        if (vibeos_parttab_checksum(&g_scratch_bc, HW_SCRATCH_SECTORS,
                                    &sum) != 0) {
            verdict = "FAILED: no checksum";
        } else {
            r = vibeos_parttab_write_mbr(&g_scratch_bc, HW_SCRATCH_SECTORS,
                                         &want, &guard, sum);
            if (r != VIBEOS_PARTTAB_OK) {
                verdict = vibeos_parttab_result_name(r);
            } else {
                vibeos_parttable_t back;
                int protective = 0;
                uint8_t sec[512];

                verdict = "FAILED: sector 0 unreadable";
                if (vibeos_blockcache_read(&g_scratch_bc, 0, sec) == 0 &&
                    vibeos_partition_parse_mbr(sec, &back, &protective) == 0) {
                    int entries_ok = (back.count == 2u) &&
                                     (back.entry[0].first_lba == 64ull) &&
                                     (back.entry[0].sector_count == 6000ull) &&
                                     (back.entry[1].first_lba == 6144ull) &&
                                     (back.entry[1].sector_count == 1024ull);
                    /* And the bytes the table does not own are untouched: a
                     * writer that rebuilds sector 0 makes a disk unbootable
                     * while doing exactly what it was asked. */
                    int rest_ok = 1;
                    for (i = 0; i < 446u; i++) {
                        if (sec[i] != (uint8_t)(i ^ 0x3Cu)) {
                            rest_ok = 0;
                            break;
                        }
                    }
                    /* Three outcomes, decided by two flags rather than by
                     * inspecting the string that was set last - which is what
                     * the first version did, and which would have reported the
                     * wrong one the moment a message was reworded. */
                    if (!entries_ok) {
                        verdict = "FAILED: the table read back differs";
                    } else if (!rest_ok) {
                        verdict = "FAILED: the rest of sector 0 was destroyed";
                    } else {
                        verdict = "OK";
                        table_ok = 1;
                    }
                }
            }
        }

        /* Format the first partition, and check the result the way anything
         * else would: by probing it.
         *
         * Not by mounting it. This driver's state is a single global, so
         * mounting the scratch volume would unmount the one the machine is
         * running from - which is the structural limit already recorded in
         * io_mounts.md, and a much larger change than a formatter. Probing
         * proves the bytes on the medium are a FAT16 volume that a reader will
         * recognise, which is what the format op is responsible for; mounting
         * and writing a file waits for the driver to stop being a singleton.
         *
         * The probe is the same one the volume scan uses, with no shared state
         * with the formatter. A formatter checked by its own idea of what it
         * wrote proves nothing. */
        /* A flag, not the first letter of a message. The previous line of
         * this function already had to stop deciding an outcome by inspecting
         * the string it had set, because a reworded message would have changed
         * the decision. */
        if (table_ok) {
            const char *fverdict = "not attempted";
            if (!g_fat_driver_format) {
                fverdict = "FAILED: no format op";
            } else if (g_fat_driver_format(&g_scratch_bc, 64ull, 6000ull) != 0) {
                fverdict = "FAILED: format refused";
            } else if (!g_fat_driver_probe ||
                       g_fat_driver_probe(&g_scratch_bc, 64ull) != 0) {
                fverdict = "FAILED: the probe did not recognise it";
            } else {
                /* Mount it, write a file, read it back. This is the phase's
                 * own "done when", and until the driver stopped being a
                 * singleton it could not be attempted: mounting this volume
                 * would have unmounted the one the machine is running from.
                 *
                 * The read back goes through the mount table's resolver, not
                 * through the mount handle directly, so what is proved is that
                 * a path under /vol1 reaches this volume and not the root -
                 * which is the property the table exists for and the one a
                 * first-match resolver would get wrong. */
                static vibeos_fsmount_t s_scratch_mnt;
                void *vol = vibeos_x86_64_fat_mount_volume(&g_scratch_bc, 64u);

                fverdict = "FAILED: mount";
                if (vol && vibeos_fs_mount(&s_scratch_mnt, vibeos_x86_64_fat_ops(),
                                           vol, "fat") == 0 &&
                    vibeos_fs_attach("/vol1", &s_scratch_mnt) == 0) {
                    static uint8_t s_wr[600];
                    static uint8_t s_rd[600];
                    vibeos_fsmount_t *m = 0;
                    const char *tail = 0;
                    uint32_t k;

                    for (k = 0; k < sizeof(s_wr); k++) {
                        s_wr[k] = (uint8_t)((k * 5u) ^ 0xA7u);
                    }
                    fverdict = "FAILED: write";
                    if (vibeos_fs_write_file(&s_scratch_mnt, "HELLO.BIN",
                                             s_wr, sizeof(s_wr)) ==
                        (long)sizeof(s_wr)) {
                        fverdict = "FAILED: resolve";
                        if (vibeos_fs_resolve("/vol1/HELLO.BIN", &m, &tail) == 0 &&
                            m == &s_scratch_mnt) {
                            long got;
                            for (k = 0; k < sizeof(s_rd); k++) {
                                s_rd[k] = 0;
                            }
                            got = vibeos_fs_read_file(m, tail, s_rd,
                                                      sizeof(s_rd));
                            fverdict = "FAILED: read back";
                            if (got == (long)sizeof(s_rd)) {
                                int same = 1;
                                for (k = 0; k < sizeof(s_rd); k++) {
                                    if (s_rd[k] != s_wr[k]) {
                                        same = 0;
                                        break;
                                    }
                                }
                                fverdict = same ? "OK"
                                                : "FAILED: contents differ";
                            }
                        }
                    }
                }
            }
            vibeos_x86_64_serial_lock();
            vibeos_x86_64_serial_puts("[IO] FORMAT fs=fat first_lba=0x40 result=");
            vibeos_x86_64_serial_puts(fverdict);
            vibeos_x86_64_serial_puts("\n");
            vibeos_x86_64_serial_unlock();
        }

        vibeos_x86_64_serial_lock();
        vibeos_x86_64_serial_puts("[IO] PARTTAB scratch_device=0x");
        vibeos_x86_64_serial_print_hex((uint64_t)device);
        vibeos_x86_64_serial_puts(" round_trip=");
        vibeos_x86_64_serial_puts(verdict);
        vibeos_x86_64_serial_puts("\n");
        vibeos_x86_64_serial_unlock();
    }
}

/* ---- volumes (I4b step 1 and 2) -------------------------------------------
 *
 * What is actually on this disk, read at boot and said out loud.
 *
 * The partition reader, the GPT parser and the volume scan were all written,
 * host-tested and sabotage-verified, and had never executed a line on a
 * booting machine - the same state the swap stack and the block cache were in
 * before this week. `vibeos_storage_scan` was defined and called by nobody.
 *
 * It reads through the block cache the filesystem already uses, and not one of
 * its own. Two caches over one device is precisely the arrangement I2 spent a
 * phase removing, and standing a second one up here to avoid a two-line
 * accessor would have put it straight back.
 *
 * ## What this does not do yet
 *
 * Mount anything. The scan will claim volumes for drivers that have never run
 * on this machine either, and mounting a second filesystem needs the mount
 * table of step 4 - there is one global mount today, which is the structural
 * reason only one filesystem can run. So this reports and stops, which is the
 * "done when" of steps 1 and 2 and honestly not of 3 and 4.
 */
static vibeos_storage_t g_storage;

void hw_volumes_bringup(void) {
    vibeos_blockcache_t *bc = vibeos_x86_64_fat_cache();
    uint64_t sectors = 0;
    uint32_t i;

    if (!bc) {
        return;
    }
    /* Before the scan, or the scan has nothing to offer this volume to. */
    vibeos_x86_64_fat_register_driver();
    {
        vibeos_blk_driver_t info;
        int dev = vibeos_x86_64_blk_device();
        if (dev >= 0 && vibeos_blk_info((uint32_t)dev, &info) == 0) {
            sectors = info.sectors;
        }
    }
    /* Zero means the size is unknown, and the scan then declines to parse a
     * GPT - correctly, because every one of its checks is against a size. The
     * driver had to state its capacity to register at all since I1, so this
     * should not happen; if it ever does, the line below says so by reporting
     * an MBR-only result on a disk that has a GPT. */
    if (vibeos_storage_scan(&g_storage, bc, sectors) != 0) {
        vibeos_x86_64_serial_lock();
        vibeos_x86_64_serial_puts("[IO] VOLUMES scan refused\n");
        vibeos_x86_64_serial_unlock();
        return;
    }

    vibeos_io_stats()->volumes_found += g_storage.volume_count;
    vibeos_io_stats()->mounts += g_storage.mounted_count;
    vibeos_io_stats()->probe_rejected +=
        g_storage.volume_count - g_storage.mounted_count;

    vibeos_x86_64_serial_lock();
    vibeos_x86_64_serial_puts("[IO] VOLUMES table=");
    vibeos_x86_64_serial_puts(g_storage.table.is_gpt ? "gpt" : "mbr");
    vibeos_x86_64_serial_puts(" partitions=0x");
    vibeos_x86_64_serial_print_hex((uint64_t)g_storage.table.count);
    vibeos_x86_64_serial_puts(" volumes=0x");
    vibeos_x86_64_serial_print_hex((uint64_t)g_storage.volume_count);
    vibeos_x86_64_serial_puts(" mounted=0x");
    vibeos_x86_64_serial_print_hex((uint64_t)g_storage.mounted_count);
    vibeos_x86_64_serial_puts(" disk_sectors=0x");
    vibeos_x86_64_serial_print_hex(sectors);
    vibeos_x86_64_serial_puts("\n");
    vibeos_x86_64_serial_unlock();

    /* Every mounted volume goes in the table, and the first one is the root.
     *
     * The syscalls still take g_rootfs directly - moving them onto the
     * resolver is the change that makes a second mount reachable, and doing it
     * in the same step as building the table would mean neither is verified.
     * What the table gives today is the ability to *say* what is mounted,
     * which is what the phase's "done when" asks for and what a machine with
     * one global mount could not do at all. */
    for (i = 0; i < g_storage.volume_count && i < VIBEOS_STORAGE_MAX_VOLUMES; i++) {
        char at[VIBEOS_FS_MOUNT_PATH_MAX];
        uint32_t w = 0;

        if (!vibeos_fs_is_mounted(&g_storage.volume[i].mount)) {
            continue;
        }
        if (vibeos_fs_mount_count() == 0u) {
            at[w++] = '/';
        } else {
            /* /vol1, /vol2, ... A name a person can type, and one this build
             * can generate without a formatter. */
            at[w++] = '/'; at[w++] = 'v'; at[w++] = 'o'; at[w++] = 'l';
            at[w++] = (char)('0' + (i % 10u));
        }
        at[w] = 0;
        if (vibeos_fs_attach(at, &g_storage.volume[i].mount) != 0) {
            vibeos_x86_64_serial_lock();
            vibeos_x86_64_serial_puts("[IO] MOUNT refused at ");
            vibeos_x86_64_serial_puts(at);
            vibeos_x86_64_serial_puts("\n");
            vibeos_x86_64_serial_unlock();
        }
    }

    {
        uint32_t k;
        for (k = 0; k < vibeos_fs_mount_count(); k++) {
            vibeos_x86_64_serial_lock();
            vibeos_x86_64_serial_puts("[IO] MOUNT at=");
            vibeos_x86_64_serial_puts(vibeos_fs_mount_path(k));
            vibeos_x86_64_serial_puts(" type=");
            vibeos_x86_64_serial_puts(
                vibeos_fs_type(vibeos_fs_mount_at(k)));
            vibeos_x86_64_serial_puts("\n");
            vibeos_x86_64_serial_unlock();
        }
    }

    /* One line per volume, each bracketed on its own: a run of them assembled
     * as one critical section would hold the console across several device
     * reads, and this project has a rule about how long a lock that masks
     * interrupts may be held. */
    for (i = 0; i < g_storage.volume_count && i < VIBEOS_PART_MAX; i++) {
        const vibeos_partition_t *p = &g_storage.table.entry[i];
        vibeos_x86_64_serial_lock();
        vibeos_x86_64_serial_puts("[IO] VOLUME idx=0x");
        vibeos_x86_64_serial_print_hex((uint64_t)i);
        vibeos_x86_64_serial_puts(" first_lba=0x");
        vibeos_x86_64_serial_print_hex(g_storage.volume[i].first_lba);
        vibeos_x86_64_serial_puts(" sectors=0x");
        vibeos_x86_64_serial_print_hex(
            (i < g_storage.table.count) ? p->sector_count : sectors);
        vibeos_x86_64_serial_puts(" kind=");
        vibeos_x86_64_serial_puts(
            (i < g_storage.table.count)
                ? vibeos_partition_kind_name(p->kind) : "whole-disk");
        vibeos_x86_64_serial_puts(" fs=");
        vibeos_x86_64_serial_puts(g_storage.volume[i].fs_name
                                  ? g_storage.volume[i].fs_name : "none");
        vibeos_x86_64_serial_puts("\n");
        vibeos_x86_64_serial_unlock();
    }
}

/* Give swap somewhere to write, if this medium has anywhere.
 *
 * Called after the volume is mounted, because until then there is no device
 * and no way to resolve a path. The whole of P5 - the swap map, page-out,
 * page-in, reclaim's anonymous tier - was built, host-tested and
 * sabotage-verified above this, and had never once run on a booting machine.
 *
 * Every step here can decline, and each declines differently on purpose:
 * "there is no swap file" and "there is a swap file and it is unusable" are
 * different states, and only one of them is somebody's mistake.
 *
 * The refusal that matters is fragmentation. The swap file lives on the same
 * volume as everything else, so an area that spanned a gap in the chain would
 * not fail - it would write a page of some process's memory over another
 * file's data, and the damage would surface at the next boot as a program that
 * is quietly wrong. vibeos_x86_64_fat_file_extent reports whether the chain is
 * one run and this refuses it if it is not, rather than hoping. */
void hw_swap_bringup(void) {
    vibeos_swap_area_t area;
    uint64_t first = 0, sectors = 0;
    int contiguous = 0;
    uint32_t slots = 0;
    const char *why = "no swap file on this medium";
    int dev = vibeos_x86_64_blk_device();

    {
        uint8_t *z = (uint8_t *)&area;
        unsigned k;
        for (k = 0; k < sizeof(area); k++) {
            z[k] = 0;
        }
    }
    area.kind = VIBEOS_SWAP_NONE;
    area.origin = "EFI/BOOT/SWAPFILE.BIN";

    if (dev >= 0 &&
        vibeos_x86_64_fat_file_extent("EFI/BOOT/SWAPFILE.BIN",
                                      &first, &sectors, &contiguous) == 0) {
        if (!contiguous) {
            /* Declined, not worked around. Following an extent list belongs in
             * the swap area layer and is a later change; guessing here would
             * put the guess in the one place that must not have one. */
            why = "swap file is fragmented; refused";
        } else {
            area.kind = VIBEOS_SWAP_FILE;
            area.device = (uint32_t)dev;
            area.first_sector = first;
            area.sectors = sectors;
            area.contiguous = 1;
            why = "swap file accepted";
        }
    }

    slots = vibeos_swaparea_configure(&area, hw_swap_block, 0, g_swap_bitmap,
                                      (uint32_t)sizeof(g_swap_bitmap));
    if (slots == 0u && area.kind != VIBEOS_SWAP_NONE) {
        why = "swap area refused by the swap layer";
    }
    if (slots > 0u) {
        /* Does a page actually survive the trip?
         *
         * Everything below here was host-tested against a memory-backed
         * device, which proves the arithmetic and proves nothing about this
         * machine's disk. The interesting failures are the ones a model cannot
         * have: a driver that reports a write it did not do, a medium that
         * reads back zeroes, an area pointed somewhere it does not own.
         *
         * Two checks, and the second is the one that matters.
         *
         * One slot is taken, filled with a pattern that includes its own
         * offset - a constant would survive a read that returned the wrong
         * sector, as long as that sector had been written too - sent out, read
         * back into a different page, and compared.
         *
         * That alone would be a weak check, and saying so is the point: a
         * write and a read that use the same wrong address agree perfectly.
         * An area whose first sector is off by a cluster passes it, while
         * quietly writing pages of memory over another file.
         *
         * So the bytes are then looked for through the *filesystem* - the file
         * is read by name, which resolves its own chain and never consults
         * area.first_sector. If the pattern is not at the front of
         * SWAPFILE.BIN, the area is not pointing at the swap file, whatever
         * the round trip said. That is the check that would have caught the
         * defect this layer's header says it exists to prevent.
         *
         * A boot check rather than a host test because the point is the parts a
         * host test cannot reach, and it is cheap: two 4 KiB transfers, once.
         *
         * "Gate the mechanism when you cannot gate the bug": reclaim's
         * anonymous tier only runs under memory pressure, which an ordinary
         * boot never reaches, so without this the entire swap path could stop
         * working and every boot would stay green. */
        void *out = hw_alloc_page();
        void *back = hw_alloc_page();
        uint32_t slot = 0;
        const char *verdict = "swap round trip not attempted";

        if (out && back && vibeos_swap_alloc(&slot) == 0) {
            uint64_t *w = (uint64_t *)out;
            const uint64_t *r = (const uint64_t *)back;
            uint32_t i;
            int bad = -1;

            for (i = 0; i < 4096u / 8u; i++) {
                w[i] = 0x5761705465737430ull ^ ((uint64_t)i << 8);
            }
            for (i = 0; i < 4096u / 8u; i++) {
                ((uint64_t *)back)[i] = 0ull;
            }
            if (vibeos_swap_write(slot, out) != 0) {
                verdict = "swap round trip FAILED: write";
            } else if (vibeos_swap_read(slot, back) != 0) {
                verdict = "swap round trip FAILED: read";
            } else {
                for (i = 0; i < 4096u / 8u; i++) {
                    if (r[i] != (0x5761705465737430ull ^ ((uint64_t)i << 8))) {
                        bad = (int)i;
                        break;
                    }
                }
                verdict = (bad < 0) ? "swap round trip OK"
                                    : "swap round trip FAILED: contents";
            }
            /* The independent half. Read through the filesystem, which
             * walks the chain from the directory entry and knows nothing about
             * where the swap area thinks it is.
             *
             * Only meaningful on a first read of this file: a cached copy from
             * earlier in the boot would be stale, because the swap write went
             * straight to the block layer. Nothing reads SWAPFILE.BIN before
             * this, and if anything ever does, this check has to move ahead of
             * it rather than be believed. */
            if (bad < 0) {
                /* A positional read, because vibeos_fs_read_file reads a whole
                 * file and this one is eight megabytes. The first version
                 * asked for the whole thing into a 4 KiB page and reported
                 * "file unreadable", which was true and was about the buffer
                 * rather than about swap. */
                uint32_t fc = 0, fsz = 0;
                long n = -1;

                if (vibeos_x86_64_fat_open("EFI/BOOT/SWAPFILE.BIN", &fc, &fsz) == 0) {
                    n = vibeos_x86_64_fat_read_at(fc, fsz, 0u, back, 4096u);
                }
                if (n < 4096) {
                    verdict = "swap round trip FAILED: file unreadable";
                } else {
                    for (i = 0; i < 4096u / 8u; i++) {
                        if (r[i] != (0x5761705465737430ull ^ ((uint64_t)i << 8))) {
                            bad = (int)i;
                            break;
                        }
                    }
                    if (bad >= 0) {
                        verdict = "swap round trip FAILED: area is not the file";
                    }
                }
            }
            (void)vibeos_swap_free(slot);
        }
        if (out) {
            hw_free_page_why(out, "swap_selftest");
        }
        if (back) {
            hw_free_page_why(back, "swap_selftest");
        }
        vibeos_x86_64_serial_lock();
        vibeos_x86_64_serial_puts("[MM] SWAP_ROUNDTRIP ");
        vibeos_x86_64_serial_puts(verdict);
        vibeos_x86_64_serial_puts("\n");
        vibeos_x86_64_serial_unlock();

        /* Only now, and this is the whole point of the phase.
         *
         * Registering the anonymous tier without an area would give reclaim a
         * source that can never succeed, and `skipped_no_swap` would stop
         * counting what having no swap costs - a number going quiet because
         * the thing it measures became invisible, not because it stopped
         * happening. With no area, no source: reclaim keeps saying so. */
        vibeos_anon_set_map(hw_frame_identity_map);
        vibeos_reclaim_set_anon_source(vibeos_anon_reclaim);
    }

    /* One call under the console lock: a line built from several is several
     * critical sections, and this project has had a gate report failures that
     * were a marker cut in half. */
    vibeos_x86_64_serial_lock();
    vibeos_x86_64_serial_puts("[MM] SWAP_AREA slots=0x");
    vibeos_x86_64_serial_print_hex((uint64_t)slots);
    vibeos_x86_64_serial_puts(" first_lba=0x");
    vibeos_x86_64_serial_print_hex(area.first_sector);
    vibeos_x86_64_serial_puts(" sectors=0x");
    vibeos_x86_64_serial_print_hex(area.sectors);
    vibeos_x86_64_serial_puts(" contiguous=0x");
    vibeos_x86_64_serial_print_hex((uint64_t)area.contiguous);
    vibeos_x86_64_serial_puts(" (");
    vibeos_x86_64_serial_puts(why);
    vibeos_x86_64_serial_puts(")\n");
    vibeos_x86_64_serial_unlock();
}

