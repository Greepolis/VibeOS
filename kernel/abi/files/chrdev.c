/* Character devices (docs/abi/ L2 step 6): what a node of type S_IFCHR opens
 * as, chosen by its number (vibeos/devfs.h).
 *
 * /dev/null, /dev/zero, /dev/full, /dev/random and /dev/urandom are one type,
 * told apart by the number the description keeps; /dev/tty and /dev/console
 * are the console's description, because there is one terminal and every
 * process has it. A number nothing here knows is ENXIO, Linux's answer for a
 * device node with no driver behind it. */

#include "files_internal.h"
#include "vibeos/devfs.h"
#include "vibeos/random.h"

#define CHR_CHUNK 256u

/* Zeros or random bytes into the caller's buffer, a chunk at a time; a signal
 * ends a long read early with what was read, as it does in Linux. */
static long chr_fill(uint64_t buf, uint64_t len, int random) {
    uint8_t chunk[CHR_CHUNK];
    uint64_t done = 0;
    uint32_t i;
    long r = 0;

    for (i = 0; i < CHR_CHUNK; i++) {
        chunk[i] = 0;
    }
    while (done < len) {
        uint32_t n = len - done > CHR_CHUNK ? CHR_CHUNK : (uint32_t)(len - done);

        if (done > 0u && ks_current() >= 0 && ks_signal_interrupts(ks_current())) {
            break;
        }
        if (random) {
            vibeos_random_read(chunk, n);
        }
        if (vibeos_uaccess_copy((void *)(uintptr_t)(buf + done), chunk, n) != 0) {
            r = -VIBEOS_EFAULT;
            break;
        }
        done += n;
    }
    for (i = 0; i < CHR_CHUNK; i++) {
        ((volatile uint8_t *)chunk)[i] = 0;
    }
    return done > 0u ? (long)done : r;
}

static long chr_read(vibeos_file_t *f, uint64_t buf, uint64_t len) {
    switch (f->rdev) {
        case VIBEOS_DEV_NULL:
            return 0;
        case VIBEOS_DEV_RANDOM:
            /* /dev/random waits for the pool to be ready, as Linux's has since
             * 5.6, and never after; /dev/urandom never waits. */
            while (!vibeos_random_ready()) {
                if (f->flags & VIBEOS_O_NONBLOCK) {
                    return -VIBEOS_EAGAIN;
                }
                if (ks_current() >= 0 && ks_signal_interrupts(ks_current())) {
                    return -VIBEOS_RESTART_CALL;
                }
                ks_block_point();
            }
            return chr_fill(buf, len, 1);
        case VIBEOS_DEV_URANDOM:
            return chr_fill(buf, len, 1);
        default:
            return chr_fill(buf, len, 0);   /* zero and full read zeros */
    }
}

/* Writes are taken whole and kept by nobody - but bytes written to the random
 * devices are mixed into the pool, believed for nothing, as Linux mixes them.
 * /dev/full is the disk that is always full. */
static long chr_write(vibeos_file_t *f, uint64_t buf, uint64_t len) {
    uint8_t chunk[CHR_CHUNK];
    uint64_t done = 0;

    if (f->rdev == VIBEOS_DEV_FULL) {
        return len == 0u ? 0 : -VIBEOS_ENOSPC;
    }
    if (f->rdev != VIBEOS_DEV_RANDOM && f->rdev != VIBEOS_DEV_URANDOM) {
        return (long)len;
    }
    while (done < len) {
        uint32_t n = len - done > CHR_CHUNK ? CHR_CHUNK : (uint32_t)(len - done);

        if (vibeos_uaccess_copy(chunk, (const void *)(uintptr_t)(buf + done), n) != 0) {
            return done > 0u ? (long)done : -VIBEOS_EFAULT;
        }
        vibeos_random_add(chunk, n, 0);
        done += n;
    }
    return (long)done;
}

static long chr_pread(vibeos_file_t *f, uint64_t buf, uint64_t len, uint64_t off) {
    (void)off;
    return chr_read(f, buf, len);
}

static long chr_pwrite(vibeos_file_t *f, uint64_t buf, uint64_t len, uint64_t off) {
    (void)off;
    return chr_write(f, buf, len);
}

/* Seeking one of these succeeds and goes nowhere, as in Linux. */
static long chr_seek(vibeos_file_t *f, int64_t off, int whence) {
    (void)f;
    (void)off;
    (void)whence;
    return 0;
}

static int chr_stat(vibeos_file_t *f, vibeos_file_stat_t *out) {
    out->mode = VIBEOS_S_IFCHR | 0666u;
    out->size = 0;
    out->ino = f->node ? f->node : 2u;
    out->rdev = f->rdev;
    out->nlink = 1u;
    return 0;
}

const vibeos_file_ops_t vibeos_fops_chrdev = {
    .name = "chrdev",
    .read = chr_read,
    .write = chr_write,
    .seek = chr_seek,
    .stat = chr_stat,
    .pread = chr_pread,
    .pwrite = chr_pwrite,
};

vibeos_file_t *vibeos_open_chrdev(uint32_t rdev, uint32_t flags, long *err) {
    const vibeos_file_ops_t *ops;
    vibeos_file_t *f;

    switch (rdev) {
        case VIBEOS_DEV_NULL:
        case VIBEOS_DEV_ZERO:
        case VIBEOS_DEV_FULL:
        case VIBEOS_DEV_RANDOM:
        case VIBEOS_DEV_URANDOM:
            ops = &vibeos_fops_chrdev;
            break;
        case VIBEOS_DEV_TTY:
        case VIBEOS_DEV_CONSOLE:
            ops = &vibeos_fops_console;
            break;
        default:
            *err = -VIBEOS_ENXIO;
            return 0;
    }
    f = vibeos_file_alloc(ops, flags & ~(VIBEOS_O_CREAT | VIBEOS_O_EXCL | VIBEOS_O_TRUNC |
                                         VIBEOS_O_DIRECTORY | VIBEOS_O_NOFOLLOW));
    if (!f) {
        *err = -VIBEOS_ENFILE;
        return 0;
    }
    f->rdev = rdev;
    *err = 0;
    return f;
}
