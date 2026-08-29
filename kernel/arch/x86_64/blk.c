/* Which block driver the filesystem talks to.
 *
 * fat.c used to call vibeos_x86_64_virtio_blk_read by name, from nine places.
 * That was honest while virtio was the only driver, and it is exactly why the
 * VM images shipped for months without anyone noticing they could not read
 * their own disk: QEMU offers virtio-blk, VirtualBox and VMware offer AHCI,
 * and the bootloader hid the difference because UEFI does the reading up to
 * ExitBootServices. After that the kernel had no disk at all - not a failure,
 * an absence, which is quieter.
 *
 * So the filesystem asks for "the disk" and this decides which one that is.
 * There is deliberately no probing here: a driver that came up binds itself,
 * and the first one to bind wins. Ordering lives at the call site in
 * arch_hw.c, where it can be read.
 */

#include <stdint.h>

#include "vibeos/arch_x86_64.h"

static int (*g_read)(uint64_t lba, void *buf);
static int (*g_read_many)(uint64_t lba, void *buf, uint32_t sectors);
static int (*g_write)(uint64_t lba, const void *buf);
static const char *g_name = "none";

void vibeos_x86_64_blk_bind(const char *name,
                            int (*read)(uint64_t, void *),
                            int (*read_many)(uint64_t, void *, uint32_t),
                            int (*write)(uint64_t, const void *)) {
    if (g_read != 0) {
        return;   /* first one to come up owns the disk */
    }
    g_name = name;
    g_read = read;
    g_read_many = read_many;
    g_write = write;
}

const char *vibeos_x86_64_blk_name(void) {
    return g_name;
}

int vibeos_x86_64_blk_present(void) {
    return g_read != 0;
}

int vibeos_x86_64_blk_read(uint64_t lba, void *buf) {
    if (!g_read) {
        return -1;
    }
    return g_read(lba, buf);
}

int vibeos_x86_64_blk_read_many(uint64_t lba, void *buf, uint32_t sectors) {
    if (g_read_many) {
        return g_read_many(lba, buf, sectors);
    }
    /* A driver without a multi-sector path still works, just slowly. Falling
     * back here rather than refusing means a new driver is useful before it is
     * finished. */
    if (!g_read) {
        return -1;
    }
    {
        uint32_t i;
        uint8_t *out = (uint8_t *)buf;
        for (i = 0; i < sectors; i++) {
            if (g_read(lba + i, out + (uint64_t)i * 512ull) != 0) {
                return -1;
            }
        }
    }
    return 0;
}

int vibeos_x86_64_blk_write(uint64_t lba, const void *buf) {
    if (!g_write) {
        return -1;
    }
    return g_write(lba, buf);
}
