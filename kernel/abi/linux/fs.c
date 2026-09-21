/* Linux ABI: files, descriptors and pipes.
 *
 * What a program does once it is running: look at files, read directories, ask
 * who it is, talk through pipes and the console. The list came from tracing
 * BusyBox rather than from reasoning about it.
 *
 * Lifted out of arch_hw.c (C4 stage 3), moved as it was. */

#include "linux_internal.h"


/* VIBEOS_HW_MAX_TASKS is in arch_hw_internal.h. */

/* Open-file table entry. Reads stream straight off the filesystem; writes are
 * buffered and committed to disk on close (the FAT writer stores whole files). */
/* VIBEOS_HW_MAX_FDS is in arch_hw_internal.h. */
#define VIBEOS_HW_MAX_DIR_ENTRIES 4096u

/* The Linux errno values are in arch_hw_internal.h. */
#define VIBEOS_TIOCGPGRP 0x540Fu

#define VIBEOS_TIOCSPGRP 0x5410u

/* Linux x86-64 syscall numbers we implement. */
/* VibeOS-specific: network control. Deliberately outside the Linux number
 * space, so it can never collide with a real syscall we implement later. */

/* Numbers a real C runtime reaches for before it runs any of the program.
 * Taken from arch/x86/entry/syscalls/syscall_64.tbl, not from memory. */

/* What a real program needs once it is past startup and doing work. Taken from
 * a strace of BusyBox running echo, cat, ls, pwd and wc - see
 * scripts/dev/trace-linux-binary.sh. */

/* clone() flags that decide whether this is a fork or a thread. */

/* openat/newfstatat interpret a relative path against this directory fd. There
 * is no per-process working directory here, so it is the only value accepted.
 *
 * Reading it needs care. Arguments the Linux ABI types as `int` arrive in the
 * low half of a register, and writing a 32-bit register zeroes the upper half:
 * a caller doing `mov $-100, %edi` delivers 0x00000000ffffff9c, not
 * 0xffffffffffffff9c. Comparing the full 64 bits against -100 therefore never
 * matches, and every relative open fails with ENOSYS - which is exactly what
 * BusyBox reported as "can't open: Function not implemented". Read the low 32
 * bits and sign-extend, as the kernel this ABI belongs to does. */
#define AT_FDCWD           (-100)

#define AT_EMPTY_PATH      0x1000

/* struct stat, x86-64 layout. Byte offsets rather than a struct definition
 * because the layout is the ABI: it is fixed by Linux, not by this compiler. */
#define STAT_SIZE          144u

#define STAT_OFF_MODE       24u

#define STAT_OFF_NLINK      16u

#define STAT_OFF_UID        28u

#define STAT_OFF_GID        32u

#define STAT_OFF_SIZE       48u

#define STAT_OFF_BLKSIZE    56u

#define STAT_OFF_BLOCKS     64u

#define STAT_OFF_INO         8u

#define S_IFREG 0100000u

#define S_IFDIR 0040000u

#define S_IFCHR 0020000u

/* Per-process open-file table helpers. fds 0-2 are the console; 3+ are files. */
hw_fd_t *hw_fd_get(uint64_t fd) {
    if (g_current_task < 0 || fd < 3u || fd >= 3u + VIBEOS_HW_MAX_FDS) {
        return 0;
    }
    return vibeos_fdtable_get(&g_tasks[g_current_task].files, fd);
}

/* Socket-backed descriptors are served by these (defined with the socket
 * syscalls below), so read/write work on a connection like any other stream. */
long hw_net_recv(hw_fd_t *f, uint64_t buf, uint64_t len);

long hw_net_send(hw_fd_t *f, uint64_t buf, uint64_t len);

static long hw_pipe_read(hw_fd_t *f, uint64_t buf, uint64_t len) {
    hw_pipe_t *pp = &g_pipes[f->pipe];
    uint8_t *dst = (uint8_t *)(uintptr_t)buf;

    for (;;) {
        uint64_t copied = 0;
        int faulted = 0;
        int eof = 0;

        /* In contiguous runs of the ring, each through the fault-tolerant copy,
         * and consumed only once copied: the caller may have slept below with
         * the buffer validated, and a sibling can have unmapped it since
         * (H-010). */
        hw_spin_lock_named(&g_pipe_lock, __func__);
        while (copied < len && pp->count > 0u) {
            uint64_t run = VIBEOS_HW_PIPE_BYTES - pp->head;
            if (run > pp->count) {
                run = pp->count;
            }
            if (run > len - copied) {
                run = len - copied;
            }
            if (vibeos_uaccess_copy(dst + copied, &pp->buf[pp->head], run) != 0) {
                faulted = 1;
                break;
            }
            copied += run;
            pp->head = (uint32_t)((pp->head + run) % VIBEOS_HW_PIPE_BYTES);
            pp->count -= (uint32_t)run;
        }
        /* End of file is decided here, in the same critical section that found
         * the buffer empty (M-003, the read half). It used to be decided after
         * the unlock, reading `writers` unguarded: a writer could enqueue and
         * close in between, and the reader returned end-of-file with those bytes
         * still in the buffer - the shape of `ls | wc -l` in the boot script. */
        eof = (copied == 0u && !faulted && pp->writers == 0u);
        hw_spin_unlock(&g_pipe_lock);
        if (faulted && copied == 0u) {
            return -VIBEOS_EFAULT;
        }
        if (copied > 0u) {
            hw_keyboard_wake();   /* a blocked writer may now have room */
            return (long)copied;
        }
        if (eof) {
            return 0;   /* end of file: empty, and nobody can ever write again */
        }
        /* Nothing yet, and somebody could still write. Park instead of
         * spinning, so the writer actually gets a chance to run - unless a
         * signal needs acting on. This wait never marks the task BLOCKED, so
         * the timer returns it here each tick and the check cannot be missed. */
        if (g_current_task >= 0 && hw_signal_interrupts(g_current_task)) {
            return -VIBEOS_EINTR;
        }
        hw_sched_point("block");
        __asm__ __volatile__("sti; hlt" ::: "memory");
    }
}

static long hw_pipe_write(hw_fd_t *f, uint64_t buf, uint64_t len) {
    hw_pipe_t *pp = &g_pipes[f->pipe];
    const uint8_t *src = (const uint8_t *)(uintptr_t)buf;
    uint64_t written = 0;

    while (written < len) {
        uint64_t before = written;

        {
            int faulted = 0;

            /* The same, in the other direction: a writer that blocked on a full
             * pipe reads its buffer again after waking (H-010). */
            hw_spin_lock_named(&g_pipe_lock, __func__);
            /* Writing into a pipe nobody will read. Linux raises SIGPIPE and
             * returns EPIPE; with no handler the default action ends the
             * process, which is what stops a pipeline from filling memory
             * after its reader has gone.
             *
             * Tested under the lock that enqueues (M-003, the write half): it
             * was tested before taking it, so the last reader could close in
             * between and the bytes were accepted for nobody, with no signal. */
            if (pp->readers == 0u) {
                hw_spin_unlock(&g_pipe_lock);
                if (g_current_task >= 0) {
                    (void)hw_signal_raise(g_current_task, VIBEOS_SIGPIPE);
                }
                return written > 0u ? (long)written : -VIBEOS_EPIPE;
            }
            while (written < len && pp->count < VIBEOS_HW_PIPE_BYTES) {
                uint64_t room = VIBEOS_HW_PIPE_BYTES - pp->count;
                uint64_t run = VIBEOS_HW_PIPE_BYTES - pp->tail;
                if (run > room) {
                    run = room;
                }
                if (run > len - written) {
                    run = len - written;
                }
                if (vibeos_uaccess_copy(&pp->buf[pp->tail], src + written, run) != 0) {
                    faulted = 1;
                    break;
                }
                written += run;
                pp->tail = (uint32_t)((pp->tail + run) % VIBEOS_HW_PIPE_BYTES);
                pp->count += (uint32_t)run;
            }
            hw_spin_unlock(&g_pipe_lock);
            if (faulted) {
                return written > 0u ? (long)written : -VIBEOS_EFAULT;
            }
        }
        if (written > before) {
            hw_keyboard_wake();   /* a blocked reader now has data */
            continue;
        }
        /* Full, and a signal needs acting on: report what was written, or
         * EINTR if nothing was. Same shape as the read side, and the same
         * reason the check cannot be missed. */
        if (g_current_task >= 0 && hw_signal_interrupts(g_current_task)) {
            return written > 0u ? (long)written : -VIBEOS_EINTR;
        }
        hw_sched_point("block");
        __asm__ __volatile__("sti; hlt" ::: "memory");
    }
    return (long)written;
}

/* pipe2(): two descriptors onto one buffer, read end first. */
static long hw_sys_pipe2(uint64_t fds_uptr, uint64_t flags) {
    hw_task_t *t;
    int slot = -1, rfd = -1, wfd = -1;
    int i;

    (void)flags;   /* O_CLOEXEC has no meaning without an exec-close list */
    if (g_current_task < 0 || !g_tasks[g_current_task].id.is_user) {
        return -VIBEOS_EINVAL;
    }
    t = &g_tasks[g_current_task];

    hw_spin_lock_named(&g_pipe_lock, __func__);
    for (i = 0; i < VIBEOS_HW_MAX_PIPES; i++) {
        if (!g_pipes[i].used) {
            g_pipes[i].used = 1;
            g_pipes[i].readers = 1;
            g_pipes[i].writers = 1;
            g_pipes[i].head = 0;
            g_pipes[i].tail = 0;
            g_pipes[i].count = 0;
            slot = i;
            break;
        }
    }
    hw_spin_unlock(&g_pipe_lock);
    if (slot < 0) {
        return -VIBEOS_EMFILE;
    }

    for (i = 0; i < VIBEOS_HW_MAX_FDS && (rfd < 0 || wfd < 0); i++) {
        if (t->files.fds[i].used) {
            continue;
        }
        {
            hw_fd_t *f = &t->files.fds[i];
            uint32_t z;
            for (z = 0; z < (uint32_t)sizeof(*f); z++) {
                ((uint8_t *)(void *)f)[z] = 0;
            }
            f->net_sock = -1;
            f->pipe = slot;
            f->writable = (rfd < 0) ? 0 : 1;
            f->used = 1;
        }
        if (rfd < 0) {
            rfd = 3 + i;
        } else {
            wfd = 3 + i;
        }
    }
    if (rfd < 0 || wfd < 0) {
        hw_spin_lock_named(&g_pipe_lock, __func__);
        g_pipes[slot].used = 0;
        hw_spin_unlock(&g_pipe_lock);
        if (rfd >= 0) {
            t->files.fds[rfd - 3].used = 0;
        }
        return -VIBEOS_EMFILE;
    }
    {
        int kfds[2];
        kfds[0] = rfd;
        kfds[1] = wfd;
        /* Copied out fault-safe: a sibling munmap between the range check and
         * this write would fault in ring 0. On fault the pipe and both fds are
         * already allocated, so roll them back rather than leak them (uaccess
         * follow-up to 6a94a32). */
        if (vibeos_uaccess_copy((void *)(uintptr_t)fds_uptr, kfds, sizeof(kfds)) != 0) {
            t->files.fds[rfd - 3].used = 0;
            t->files.fds[wfd - 3].used = 0;
            hw_spin_lock_named(&g_pipe_lock, __func__);
            g_pipes[slot].used = 0;
            hw_spin_unlock(&g_pipe_lock);
            return -VIBEOS_EFAULT;
        }
    }
    return 0;
}

/* dup2(): make newfd refer to whatever oldfd refers to.
 *
 * This is how a shell attaches a pipe to a program's standard input or output
 * without the program knowing. Only descriptors 0, 1 and 2 can be targets
 * here: the console is not an entry in the table, so redirecting one means
 * remembering that the entry now stands in for it. */
static long hw_sys_dup2(uint64_t oldfd, uint64_t newfd) {
    hw_task_t *t;
    hw_fd_t *src;

    if (g_current_task < 0 || !g_tasks[g_current_task].id.is_user) {
        return -VIBEOS_EINVAL;
    }
    t = &g_tasks[g_current_task];
    if (oldfd == newfd) {
        return (long)newfd;
    }
    src = hw_fd_get(oldfd);
    if (!src) {
        return -VIBEOS_EBADF;
    }
    if (newfd >= 3u) {
        hw_fd_t *dst = (newfd < 3u + VIBEOS_HW_MAX_FDS)
                       ? &t->files.fds[newfd - 3u] : 0;
        if (!dst) {
            return -VIBEOS_EBADF;
        }
        if (dst->used) {
            hw_pipe_release(dst);
            dst->used = 0;
        }
        *dst = *src;
        if (dst->pipe >= 0) {
            hw_spin_lock_named(&g_pipe_lock, __func__);
            if (dst->writable) {
                g_pipes[dst->pipe].writers++;
            } else {
                g_pipes[dst->pipe].readers++;
            }
            hw_spin_unlock(&g_pipe_lock);
        }
        return (long)newfd;
    }
    /* Redirecting a standard descriptor: the branch above returned for every
     * other value, so newfd is 0, 1 or 2 here and re-checking that only looks
     * like a bound. */
    hw_pipe_release(&t->files.std[newfd]);
    t->files.std[newfd] = *src;
    if (t->files.std[newfd].pipe >= 0) {
        hw_spin_lock_named(&g_pipe_lock, __func__);
        if (t->files.std[newfd].writable) {
            g_pipes[t->files.std[newfd].pipe].writers++;
        } else {
            g_pipes[t->files.std[newfd].pipe].readers++;
        }
        hw_spin_unlock(&g_pipe_lock);
    }
    return (long)newfd;
}

static long hw_sys_write(uint64_t fd, uint64_t buf, uint64_t len) {
    const char *p = (const char *)(uintptr_t)buf;
    uint64_t i;

    if (g_current_task >= 0 && vibeos_fdtable_redirect(&g_tasks[g_current_task].files, fd)) {
        hw_fd_t *r = vibeos_fdtable_redirect(&g_tasks[g_current_task].files, fd);
        if (r->pipe >= 0) {
            return hw_pipe_write(r, buf, len);
        }
    }
    if (fd >= 3u) { /* a file: buffer the bytes, committed on close */
        hw_fd_t *f = hw_fd_get(fd);
        uint64_t i2;
        if (!f) {
            return -VIBEOS_EBADF;
        }
        if (f->pipe >= 0) {
            return hw_pipe_write(f, buf, len);
        }
        if (f->net_sock >= 0) {
            return hw_net_send(f, buf, len);
        }
        if (!f->writable) {
            return -VIBEOS_EBADF;
        }
        for (i2 = 0; i2 < len; i2++) {
            if (f->wlen >= VIBEOS_HW_WBUF) {
                break;
            }
            f->wbuf[f->wlen++] = (uint8_t)p[i2];
        }
        f->dirty = 1;
        return (long)i2;
    }
    if (fd != 1u && fd != 2u) {
        return -VIBEOS_EBADF;
    }
    /* A text write whose leading bytes read as NUL, reported at the moment it
     * happens rather than reconstructed afterwards.
     *
     * Three boots in ten produced `write(ring3): <16 NULs>=120` where the
     * program had written "STRESS_OK rounds=120" from a buffer on its own
     * stack. The kernel reads that buffer directly - there is no copy to blame
     * - so the page it can see holds zeros where the process wrote.
     *
     * Two very different defects produce that, and nothing recorded so far
     * separates them: either the process's store went to a frame this mapping
     * no longer points at, or the store never landed. So the bytes are read a
     * second time. A difference means the page is moving under the kernel; the
     * same zeros twice means they were already zero when the syscall began.
     *
     * Deliberately not a range check on the whole buffer: the signature is
     * leading NULs followed by real text, and a detector that fires on any NUL
     * anywhere would catch every program that writes a binary byte. This
     * project has a rule about detectors that report healthy behaviour. */
    if (len >= 8u && p[0] == 0 && p[len - 1u] != 0) {
        uint64_t a = 0ull, b = 0ull;
        uint32_t k;
        for (k = 0; k < 8u; k++) {
            a |= (uint64_t)(uint8_t)p[k] << (k * 8u);
        }
        for (k = 0; k < 8u; k++) {
            b |= (uint64_t)(uint8_t)p[k] << (k * 8u);
        }
        g_ring3_write_nul++;
        vibeos_x86_64_serial_lock();
        vibeos_x86_64_serial_puts("[MM] RING3_WRITE_NUL task=0x");
        vibeos_x86_64_serial_print_hex((uint64_t)(int64_t)g_current_task);
        vibeos_x86_64_serial_puts(" va=0x");
        vibeos_x86_64_serial_print_hex(buf);
        vibeos_x86_64_serial_puts(" len=0x");
        vibeos_x86_64_serial_print_hex(len);
        vibeos_x86_64_serial_puts(" first8=0x");
        vibeos_x86_64_serial_print_hex(a);
        vibeos_x86_64_serial_puts(" again=0x");
        vibeos_x86_64_serial_print_hex(b);
        vibeos_x86_64_serial_puts(" cpu=0x");
        vibeos_x86_64_serial_print_hex((uint64_t)vibeos_x86_64_cpu_id());
        vibeos_x86_64_serial_puts(" cr3=0x");
        vibeos_x86_64_serial_print_hex(hw_read_cr3());
        vibeos_x86_64_serial_puts(" tail=0x");
        vibeos_x86_64_serial_print_hex((uint64_t)(uint8_t)p[len - 1u]);
        /* Every copy-on-write fault this boot took on the corrupted page, in
         * the same critical section as the line above: the two are one fact,
         * and a diagnostic split across calls comes back interleaved from
         * different cores and reads as a contradiction. */
        {
            uint64_t page = buf & ~0xFFFull;
            uint32_t k2;
            for (k2 = 0; k2 < HW_COW_RING; k2++) {
                if (g_cow_ring[k2].va == 0ull ||
                    (g_cow_ring[k2].va & ~0xFFFull) != page) {
                    continue;
                }
                vibeos_x86_64_serial_puts(" | fault err=0x");
                vibeos_x86_64_serial_print_hex(g_cow_ring[k2].err);
                vibeos_x86_64_serial_puts(" rip=0x");
                vibeos_x86_64_serial_print_hex(g_cow_ring[k2].rip);
                vibeos_x86_64_serial_puts(" pid=0x");
                vibeos_x86_64_serial_print_hex(g_cow_ring[k2].pid);
                vibeos_x86_64_serial_puts(" ok=0x");
                vibeos_x86_64_serial_print_hex(g_cow_ring[k2].handled);
                vibeos_x86_64_serial_puts(" cpu=0x");
                vibeos_x86_64_serial_print_hex(g_cow_ring[k2].cpu);
            }
        }
        vibeos_x86_64_serial_puts("\n");
        vibeos_x86_64_serial_unlock();
    }

    /* User output goes to both consoles: the serial line (logs, CI) and the
     * display framebuffer (what a user in front of the machine sees). */
    vibeos_x86_64_serial_lock();
    vibeos_x86_64_serial_puts("[HW][SYS] write(ring3): ");
    for (i = 0; i < len; i++) {
        char c = p[i];
        if (c == '\n') {
            vibeos_x86_64_serial_putc('\r');
        }
        vibeos_x86_64_serial_putc(c);
        hw_console_echo(c);
    }
    vibeos_x86_64_serial_unlock();
    return (long)len;
}

/* read(0, ...): blocking keyboard read. Returns after at least one character;
 * blocks (BLOCKED + wait_input) until the keyboard IRQ enqueues input and wakes
 * us. The cli window makes the check-and-block race-free against the IRQ. */
static long hw_sys_read(uint64_t fd, uint64_t buf, uint64_t len) {
    uint8_t *dst = (uint8_t *)(uintptr_t)buf;

    if (len == 0u) {
        return 0;
    }
    if (g_current_task >= 0 && vibeos_fdtable_redirect(&g_tasks[g_current_task].files, fd)) {
        hw_fd_t *r = vibeos_fdtable_redirect(&g_tasks[g_current_task].files, fd);
        if (r->pipe >= 0) {
            return hw_pipe_read(r, buf, len);
        }
    }
    if (fd >= 3u) { /* a file: stream from the filesystem */
        hw_fd_t *f = hw_fd_get(fd);
        long n;
        if (!f) {
            return -VIBEOS_EBADF;
        }
        if (f->pipe >= 0) {
            return hw_pipe_read(f, buf, len);
        }
        if (f->net_sock >= 0) {
            return hw_net_recv(f, buf, len);
        }
        {
            vibeos_fs_node_t node;
            node.id = f->cluster;
            node.size = f->size;
            node.is_dir = f->isdir;
            n = vibeos_fs_read_at(&g_rootfs, &node, f->pos, dst, (uint32_t)len);
        }
        if (n > 0) {
            f->pos += (uint64_t)n;
        }
        return n;
    }
    if (fd != 0u) {
        return -VIBEOS_EBADF;
    }
    for (;;) {
        uint64_t copied = 0;
        int c;

        __asm__ __volatile__("cli");
        c = hw_console_getc();
        if (c >= 0) {
            /* Line discipline: echo what was typed and let backspace erase the
             * previous character before the line is handed to the program. */
            while (copied < len && c >= 0) {
                if (c == '\b' || c == 127) {
                    if (copied > 0) {
                        copied--;
                        vibeos_x86_64_serial_puts("\b \b");
                        hw_console_echo('\b');
                    }
                    c = hw_console_getc();
                    continue;
                }
                {
                    /* The line waits in this loop for keystrokes; the buffer
                     * can be unmapped under it (H-010). */
                    uint8_t ch = (uint8_t)c;
                    if (vibeos_uaccess_copy(dst + copied, &ch, 1u) != 0) {
                        __asm__ __volatile__("sti");
                        return copied > 0u ? (long)copied : -VIBEOS_EFAULT;
                    }
                    copied++;
                }
                /* Under the console lock, like every other writer. Echoing
                 * without it lets a character land in the middle of another
                 * core's write() - which does not merely look untidy: it
                 * splits the markers the boot gate matches on, so a passing
                 * run reports a failure that never happened. */
                vibeos_x86_64_serial_lock();
                if (c == '\n') {
                    vibeos_x86_64_serial_putc('\r');
                }
                vibeos_x86_64_serial_putc((char)c);
                vibeos_x86_64_serial_unlock();
                hw_console_echo((char)c);
                if ((uint8_t)c == '\n') {
                    break; /* line-oriented: stop at newline */
                }
                c = hw_console_getc();
            }
            __asm__ __volatile__("sti");
            return (long)copied;
        }
        if (g_current_task >= 0) {
            g_tasks[g_current_task].id.wait_input = 1;
            (void)hw_task_set_state(g_current_task, HW_TASK_BLOCKED, __func__);
            /* Blocked first and asked second, so a signal raised in between
             * finds the task BLOCKED and wakes it. hw_signal_raise already
             * cleared wait_input for this, and nothing ever read it here. */
            if (hw_signal_interrupts(g_current_task)) {
                g_tasks[g_current_task].id.wait_input = 0;
                (void)hw_task_set_state(g_current_task, HW_TASK_READY, __func__);
                HW_TASK_MARK(g_current_task, ready_by, "read_interrupted");
                __asm__ __volatile__("sti");
                return -VIBEOS_EINTR;
            }
        }
        hw_sched_point("block");
        __asm__ __volatile__("sti; hlt" ::: "memory");
    }
}

/* open(path, flags): resolve a file (or directory) and take an fd. With a write
 * flag the file is created/truncated on close from the buffered bytes. */
static long hw_sys_open(uint64_t path_uptr, uint64_t flags) {
    char path[64];
    hw_task_t *t;
    int i, k;

    if (g_current_task < 0 || !g_tasks[g_current_task].id.is_user) {
        return -VIBEOS_EINVAL;
    }
    if (hw_copy_user_string(path_uptr, path, sizeof(path)) != 0) {
        return -VIBEOS_EFAULT;
    }
    t = &g_tasks[g_current_task];
    i = vibeos_fdtable_free_index(&t->files);
    if (i < 0) {
        return -VIBEOS_EMFILE;
    }
    {
        hw_fd_t *f = &t->files.fds[i];
        int writable = ((flags & 1u) != 0u) || ((flags & 0100u) != 0u); /* O_WRONLY|O_CREAT */
        uint32_t cluster = 0;
        uint64_t size = 0;
        vibeos_fs_node_t node;
        int node_is_dir = 0;

        if (!writable) {
            if (vibeos_fs_lookup(&g_rootfs, path, &node) != 0) {
                return -VIBEOS_ENOENT;
            }
            cluster = (uint32_t)node.id;
            size = node.size;
            node_is_dir = node.is_dir;
        }
        for (k = 0; k < (int)sizeof(f->name) - 1 && path[k]; k++) {
            f->name[k] = path[k];
        }
        f->name[k] = 0;
        f->cluster = cluster;
        f->size = size;
        f->pos = 0;
        f->dir_index = 0;
        /* Not a pipe. The field has to be set explicitly: descriptor slots are
         * recycled, so an uninitialised value here is whatever the previous
         * occupant left, and a stale pipe index sends every read and write on
         * this file into the pipe path - where it waits for a writer that does
         * not exist. */
        f->pipe = -1;
        /* The filesystem already answered this during lookup; asking twice
         * would put a FAT-specific question back in the syscall layer. */
        f->isdir = node_is_dir;
        f->wlen = 0;
        f->dirty = 0;
        f->writable = writable;
        f->net_sock = -1;
        f->used = 1;
    }
    return 3 + i;
}

/* The socket syscalls moved to linux_socket.c. None of what they do is
 * architecture: reading a sockaddr out of user memory and blocking until a
 * connection arrives is Linux ABI translation over kernel/net/inet.c, and it
 * sat beside the GDT only because that is where this file started. */


/* close(fd): commit buffered writes to the filesystem and release the slot. */
static long hw_sys_close(uint64_t fd) {
    hw_fd_t *f = hw_fd_get(fd);
    long rc = 0;

    if (fd < 3u && g_current_task >= 0) {
        /* Closing a redirected standard descriptor drops the redirection. */
        hw_fd_t *r = vibeos_fdtable_redirect(&g_tasks[g_current_task].files, fd);
        if (r) {
            hw_pipe_release(r);
            r->used = 0;
            return 0;
        }
    }
    if (!f) {
        return -VIBEOS_EBADF;
    }
    if (f->pipe >= 0) {
        hw_pipe_release(f);
        f->used = 0;
        return 0;
    }
    if (f->net_sock >= 0) {
        hw_spin_lock_named(&g_net_lock, __func__);
        (void)vibeos_inet_close(&g_net, f->net_sock);
        hw_spin_unlock(&g_net_lock);
        f->net_sock = -1;
        f->used = 0;
        return 0;
    }
    if (f->writable && f->dirty) {
        /* The volume changed, so a staged image may no longer match the file
         * it came from. Dropping it here is the whole basis for trusting the
         * cache: a rewritten program must not keep running as its old self. */
        hw_exec_cache_drop();
        if (vibeos_fs_write_file(&g_rootfs, f->name, f->wbuf, f->wlen) < 0) {
            rc = -VIBEOS_EIO;
        }
    }
    f->used = 0;
    return rc;
}

static long hw_sys_lseek(uint64_t fd, uint64_t off, uint64_t whence) {
    hw_fd_t *f = hw_fd_get(fd);
    uint64_t base;

    if (!f) {
        return -VIBEOS_EBADF;
    }
    base = (whence == 1u) ? f->pos : ((whence == 2u) ? f->size : 0u);
    f->pos = base + off;
    return (long)f->pos;
}

/* getdents64(fd, buf, len): fill Linux dirent64 records from the directory the
 * fd was opened on, so user space can list a directory. */
static long hw_sys_getdents64(uint64_t fd, uint64_t buf, uint64_t len) {
    hw_fd_t *f = hw_fd_get(fd);
    uint8_t *out = (uint8_t *)(uintptr_t)buf;
    uint64_t used = 0;
    uint32_t records = 0;

    if (!f) {
        return -VIBEOS_EBADF;
    }
    if (!f->isdir) {
        return -VIBEOS_ENOTDIR;
    }
    /* A bounded syscall must not spin forever if a filesystem backend returns
     * a cyclic directory stream or fails to advance its cursor. */
    while (records < 256u && f->dir_index < VIBEOS_HW_MAX_DIR_ENTRIES) {
        char name[16];
        uint32_t fsize = 0;
        int is_dir = 0, n = 0;
        uint16_t reclen;

        {
            uint64_t entry_size = 0;
            if (vibeos_fs_list(&g_rootfs, f->name, f->dir_index, name,
                                sizeof(name), &entry_size, &is_dir) != 0) {
                break; /* end of directory */
            }
            fsize = (uint32_t)entry_size;
            (void)fsize;   /* getdents64 reports names and kinds, not sizes */
        }
        while (name[n]) {
            n++;
        }
        reclen = (uint16_t)((19 + n + 1 + 7) & ~7); /* 8+8+2+1 header, 8-aligned */
        if (used + reclen > len) {
            break;
        }
        {
            uint8_t *rec = out + used;
            int k;
            for (k = 0; k < reclen; k++) {
                rec[k] = 0;
            }
            rec[16] = (uint8_t)(reclen & 0xFFu);
            rec[17] = (uint8_t)(reclen >> 8);
            rec[18] = is_dir ? 4u : 8u; /* DT_DIR / DT_REG */
            for (k = 0; k < n; k++) {
                rec[19 + k] = (uint8_t)name[k];
            }
        }
        used += reclen;
        f->dir_index++;
        records++;
    }
    return (long)used;
}

/* unlink(path) / mkdir(path): filesystem mutations from user space. */
static long hw_sys_unlink(uint64_t path_uptr) {
    char path[64];
    if (hw_copy_user_string(path_uptr, path, sizeof(path)) != 0) {
        return -VIBEOS_EFAULT;
    }
    return (vibeos_fs_unlink(&g_rootfs, path) == 0) ? 0 : -VIBEOS_ENOENT;
}

static long hw_sys_mkdir(uint64_t path_uptr) {
    char path[64];
    if (hw_copy_user_string(path_uptr, path, sizeof(path)) != 0) {
        return -VIBEOS_EFAULT;
    }
    return (vibeos_fs_mkdir(&g_rootfs, path) == 0) ? 0 : -VIBEOS_EIO;
}

static void hw_stat_wr64(uint64_t base, uint32_t off, uint64_t v) {
    uint8_t *p = (uint8_t *)(uintptr_t)(base + off);
    uint32_t i;
    for (i = 0; i < 8u; i++) {
        p[i] = (uint8_t)(v >> (8u * i));
    }
}

static void hw_stat_wr32(uint64_t base, uint32_t off, uint32_t v) {
    uint8_t *p = (uint8_t *)(uintptr_t)(base + off);
    uint32_t i;
    for (i = 0; i < 4u; i++) {
        p[i] = (uint8_t)(v >> (8u * i));
    }
}

/* Fill a struct stat the caller can believe.
 *
 * The mode matters more than it looks: a libc decides how to buffer a stream
 * from it, and a program decides whether to recurse from it. Reporting a
 * regular file for a directory does not fail here - it fails later, inside the
 * program, doing something that made sense given what it was told. */
static long hw_write_stat(uint64_t ubuf, uint32_t mode, uint64_t size, uint64_t ino) {
    uint8_t kbuf[STAT_SIZE];
    uint64_t kbase = (uint64_t)(uintptr_t)kbuf;
    uint32_t i;

    /* Assemble the whole struct in the kernel and copy it out once. Filling the
     * user buffer field by field would fault in ring 0 if a sibling munmaps it
     * between the range check and any of these writes (uaccess follow-up to
     * 6a94a32, same class as H-026). */
    for (i = 0; i < STAT_SIZE; i++) {
        kbuf[i] = 0;
    }
    hw_stat_wr64(kbase, STAT_OFF_INO, ino);
    hw_stat_wr64(kbase, STAT_OFF_NLINK, 1);
    hw_stat_wr32(kbase, STAT_OFF_MODE, mode);
    hw_stat_wr32(kbase, STAT_OFF_UID, 0);
    hw_stat_wr32(kbase, STAT_OFF_GID, 0);
    hw_stat_wr64(kbase, STAT_OFF_SIZE, size);
    hw_stat_wr64(kbase, STAT_OFF_BLKSIZE, 512);
    hw_stat_wr64(kbase, STAT_OFF_BLOCKS, vibeos_ceil_div_u64(size, 512ull));
    if (vibeos_uaccess_copy((void *)(uintptr_t)ubuf, kbuf, STAT_SIZE) != 0) {
        return -VIBEOS_EFAULT;
    }
    return 0;
}

static long hw_sys_fstat(uint64_t fd, uint64_t ubuf) {
    hw_fd_t *f;

    if (fd < 3u) {
        /* The console. Character device, and deliberately not a terminal -
         * the same answer ioctl gives. */
        return hw_write_stat(ubuf, S_IFCHR | 0620u, 0, fd + 1u);
    }
    f = hw_fd_get(fd);
    if (!f) {
        return -VIBEOS_EBADF;
    }
    if (f->net_sock >= 0) {
        return hw_write_stat(ubuf, S_IFCHR | 0600u, 0, fd + 1u);
    }
    if (f->isdir) {
        return hw_write_stat(ubuf, S_IFDIR | 0755u, 0,
                             f->cluster ? f->cluster : fd + 1u);
    }
    return hw_write_stat(ubuf, S_IFREG | 0644u, f->size, f->cluster ? f->cluster : fd + 1u);
}

/* newfstatat(dirfd, path, buf, flags): stat by name, or by fd when the path is
 * empty and AT_EMPTY_PATH is set. Relative paths resolve against the volume
 * root, which is the only directory there is. */
static long hw_sys_newfstatat(uint64_t dirfd, uint64_t path_uptr, uint64_t ubuf,
                              uint64_t flags) {
    char path[64];
    uint32_t cluster = 0;
    uint64_t size = 0;   /* st_size is 64-bit; do not narrow node.size (M-018) */

    if (hw_copy_user_string(path_uptr, path, sizeof(path)) != 0) {
        return -VIBEOS_EFAULT;
    }
    if (path[0] == 0) {
        if ((flags & AT_EMPTY_PATH) == 0) {
            return -VIBEOS_ENOENT;
        }
        return hw_sys_fstat(dirfd, ubuf);
    }
    if (VIBEOS_ARG_INT(dirfd) != AT_FDCWD && dirfd < 3u) {
        return -VIBEOS_EBADF;
    }
    /* The root of the volume, however it is spelled. */
    if ((path[0] == '/' && path[1] == 0) || (path[0] == '.' && path[1] == 0)) {
        return hw_write_stat(ubuf, S_IFDIR | 0755u, 0, 1);
    }
    {
        /* Directory or file? The answer changes what a program does, not just
         * what it prints: ls given a directory lists it and given a file names
         * it, so reporting the wrong one produces a plausible wrong result
         * rather than an error. The filesystem decides; how it decides is its
         * business. */
        vibeos_fs_node_t node;
        if (vibeos_fs_lookup(&g_rootfs, path, &node) != 0) {
            return -VIBEOS_ENOENT;
        }
        cluster = (uint32_t)node.id;
        size = node.size;
        if (node.is_dir) {
            return hw_write_stat(ubuf, S_IFDIR | 0755u, 0, cluster ? cluster : 2u);
        }
    }
    return hw_write_stat(ubuf, S_IFREG | 0644u, size, cluster ? cluster : 2u);
}

/* openat(): the modern spelling of open. Only AT_FDCWD is accepted, because a
 * directory fd would have to mean something and here it cannot. */
static long hw_sys_openat(uint64_t dirfd, uint64_t path_uptr, uint64_t flags) {
    if (VIBEOS_ARG_INT(dirfd) != AT_FDCWD) {
        return -VIBEOS_ENOSYS;
    }
    return hw_sys_open(path_uptr, flags);
}

/* getcwd(): there is one directory. Saying so is accurate; inventing a path
 * would make a program build filenames that do not resolve. */
static long hw_sys_getcwd(uint64_t ubuf, uint64_t size) {
    if (size < 2u) {
        return -VIBEOS_ERANGE;
    }
    {
        /* Fault-safe copy out: a sibling munmap between the check and the write
         * would fault in ring 0 (uaccess follow-up to 6a94a32). */
        char kcwd[2];
        kcwd[0] = '/';
        kcwd[1] = 0;
        if (vibeos_uaccess_copy((void *)(uintptr_t)ubuf, kcwd, 2) != 0) {
            return -VIBEOS_EFAULT;
        }
    }
    return 2;   /* Linux returns the length including the terminator */
}

/* readlinkat(): the only symlink that exists here is the one a program uses to
 * find itself, and it is answered from what execve was actually given rather
 * than from a made-up path. Everything else is not a link, which is what
 * EINVAL means. */
static long hw_sys_readlinkat(uint64_t dirfd, uint64_t path_uptr, uint64_t ubuf,
                              uint64_t bufsz) {
    char path[64];
    const char *self;
    uint64_t n = 0;

    (void)dirfd;
    if (hw_copy_user_string(path_uptr, path, sizeof(path)) != 0) {
        return -VIBEOS_EFAULT;
    }
    if (!(path[0] == '/' && path[1] == 'p' && path[2] == 'r' && path[3] == 'o' &&
          path[4] == 'c' && path[5] == '/' && path[6] == 's' && path[7] == 'e' &&
          path[8] == 'l' && path[9] == 'f' && path[10] == '/' && path[11] == 'e' &&
          path[12] == 'x' && path[13] == 'e' && path[14] == 0)) {
        return -VIBEOS_EINVAL;
    }
    if (g_current_task < 0) {
        return -VIBEOS_EINVAL;
    }
    self = g_tasks[g_current_task].proc.exe_path;
    while (self[n]) {
        n++;
    }
    if (n == 0) {
        return -VIBEOS_ENOENT;
    }
    if (n > bufsz) {
        n = bufsz;
    }
    if (!linux_user_ok(ubuf, n, 1)) {
        return -VIBEOS_EFAULT;
    }
    /* self is a kernel string; copy out fault-safe so a sibling munmap between
     * the check and the write cannot fault in ring 0 (uaccess follow-up). */
    if (vibeos_uaccess_copy((void *)(uintptr_t)ubuf, self, n) != 0) {
        return -VIBEOS_EFAULT;
    }
    return (long)n;   /* not terminated, as Linux does not terminate it */
}

/* ioctl(): there is no terminal device here. ENOTTY is not a shortcut, it is
 * the truthful answer - and it is the answer a libc uses to decide that
 * stdout is a file or a pipe and should be block buffered. */
static long hw_sys_ioctl(uint64_t fd, uint64_t req, uint64_t arg) {
    if (fd >= 3u && !hw_fd_get(fd)) {
        return -VIBEOS_EBADF;
    }
    if (fd < 3u && req == VIBEOS_TIOCGPGRP) {
        if (g_current_task < 0) {
            return -VIBEOS_EFAULT;
        }
        {
            uint32_t v = g_console_foreground_pgid;
            if (vibeos_uaccess_copy((void *)(uintptr_t)arg, &v, sizeof(v)) != 0) {
                return -VIBEOS_EFAULT;   /* H-025 */
            }
        }
        return 0;
    }
    if (fd < 3u && req == VIBEOS_TIOCSPGRP) {
        uint32_t pgid;
        int group;
        if (g_current_task < 0) {
            return -VIBEOS_EFAULT;
        }
        if (vibeos_uaccess_copy(&pgid, (const void *)(uintptr_t)arg,
                                sizeof(pgid)) != 0) {
            return -VIBEOS_EFAULT;   /* H-025 */
        }
        group = hw_task_by_pid(pgid);
        if (group < 0 || g_tasks[group].id.sid != g_tasks[g_current_task].id.sid) {
            return -VIBEOS_EPERM;
        }
        g_console_foreground_pgid = g_tasks[group].id.pgid;
        return 0;
    }
    return -VIBEOS_ENOTTY;
}

/* writev()/readv(): scatter-gather over the existing single-buffer paths. The
 * iovec array is itself user memory, so it is validated like any other user
 * pointer before being walked. */
typedef struct {
    uint64_t base;
    uint64_t len;
} hw_iovec_t;

static long hw_sys_writev(uint64_t fd, uint64_t iov_uptr, uint64_t iovcnt) {
    long total = 0;
    uint64_t i;

    if (iovcnt > 1024u) {
        return -VIBEOS_EINVAL;   /* Linux caps this at UIO_MAXIOV */
    }
    for (i = 0; i < iovcnt; i++) {
        hw_iovec_t v;
        long n;
        /* Each descriptor copied into the kernel before base/len are read: the
         * array-wide range check and these reads are two instants, and a
         * sibling munmap of the array page faults in ring 0 otherwise (H-020). */
        if (vibeos_uaccess_copy(&v, (const void *)(uintptr_t)
                (iov_uptr + i * sizeof(hw_iovec_t)), sizeof(v)) != 0) {
            return total > 0 ? total : -VIBEOS_EFAULT;
        }
        if (v.len == 0u) {
            continue;
        }
        if (!linux_user_ok(v.base, v.len, 0)) {
            return total > 0 ? total : -VIBEOS_EFAULT;
        }
        n = hw_sys_write(fd, v.base, v.len);
        if (n < 0) {
            return total > 0 ? total : n;
        }
        total += n;
        if ((uint64_t)n < v.len) {
            break;   /* a short write ends the call, as it does on Linux */
        }
    }
    return total;
}

static long hw_sys_readv(uint64_t fd, uint64_t iov_uptr, uint64_t iovcnt) {
    long total = 0;
    uint64_t i;

    if (iovcnt > 1024u) {
        return -VIBEOS_EINVAL;
    }
    for (i = 0; i < iovcnt; i++) {
        hw_iovec_t v;
        long n;
        /* See writev: the iovec is copied in before base/len are read (H-020). */
        if (vibeos_uaccess_copy(&v, (const void *)(uintptr_t)
                (iov_uptr + i * sizeof(hw_iovec_t)), sizeof(v)) != 0) {
            return total > 0 ? total : -VIBEOS_EFAULT;
        }
        if (v.len == 0u) {
            continue;
        }
        if (!linux_user_ok(v.base, v.len, 1)) {
            return total > 0 ? total : -VIBEOS_EFAULT;
        }
        n = hw_sys_read(fd, v.base, v.len);
        if (n < 0) {
            return total > 0 ? total : n;
        }
        total += n;
        if ((uint64_t)n < v.len) {
            break;
        }
    }
    return total;
}

/* A child inherits its parent's descriptors: the table is copied, and every pipe end
 * in it gains an owner. Missing that is the other way a pipeline hangs - the reader
 * waits for an end of file that never arrives because a count went wrong. fork and
 * clone both did this by hand, two copies of the same twenty lines. */
void hw_fds_inherit(hw_task_t *child, const hw_task_t *parent) {
    uint32_t i;

    /* Task slots are recycled, so the child's table is whatever the previous
     * occupant left; it is overwritten, not added to. */
    vibeos_fdtable_copy(&child->files, &parent->files);
    hw_spin_lock_named(&g_pipe_lock, __func__);
    for (i = 0; i < vibeos_fdtable_count(); i++) {
        const hw_fd_t *cf = vibeos_fdtable_entry(&child->files, i);
        if (cf->used && cf->pipe >= 0) {
            if (cf->writable) {
                g_pipes[cf->pipe].writers++;
            } else {
                g_pipes[cf->pipe].readers++;
            }
        }
    }
    hw_spin_unlock(&g_pipe_lock);
}

/* dup() is dup2() onto the lowest free descriptor. */
static long linux_sys_dup(uint64_t oldfd) {
    hw_task_t *dt;
    int i;

    if (g_current_task < 0) {
        return -VIBEOS_EINVAL;
    }
    dt = &g_tasks[g_current_task];
    i = vibeos_fdtable_free_index(&dt->files);
    if (i < 0) {
        return -VIBEOS_EMFILE;
    }
    return hw_sys_dup2(oldfd, (uint64_t)(VIBEOS_FD_FIRST + (uint32_t)i));
}

/* ---- the syscalls this file implements ---------------------------------------
 *
 *   sendfile  every caller has to cope with it failing, and does: a read-and-write
 *             loop is the documented fallback. Refusing is therefore free, while
 *             serving it would mean a second copy of the file and console paths
 *             purely to move bytes between kernel buffers.
 *   pipe      is pipe2 with no flags. */
#define LINUX_FS_SYSCALLS(X) \
    X(0,   read,       READ,        PTRS(OUT_BUF(1, 2)), hw_sys_read(ARG(0), ARG(1), ARG(2))) \
    X(1,   write,      WRITE,       PTRS(IN_BUF(1, 2)), hw_sys_write(ARG(0), ARG(1), ARG(2))) \
    X(2,   open,       OPEN,        NOPTR, hw_sys_open(ARG(0), ARG(1))) \
    X(3,   close,      CLOSE,       NOPTR, hw_sys_close(ARG(0))) \
    X(5,   fstat,      FSTAT,       PTRS(OUT(1, STAT_SIZE)), hw_sys_fstat(ARG(0), ARG(1))) \
    X(8,   lseek,      LSEEK,       NOPTR, hw_sys_lseek(ARG(0), ARG(1), ARG(2))) \
    X(16,  ioctl,      IOCTL,       PTRS(OUT_IF(1, VIBEOS_TIOCGPGRP, 2, sizeof(uint32_t)), IN_IF(1, VIBEOS_TIOCSPGRP, 2, sizeof(uint32_t))), hw_sys_ioctl(ARG(0), ARG(1), ARG(2))) \
    X(19,  readv,      READV,       PTRS(IN_VEC(1, 2, sizeof(hw_iovec_t), 1024)), hw_sys_readv(ARG(0), ARG(1), ARG(2))) \
    X(20,  writev,     WRITEV,      PTRS(IN_VEC(1, 2, sizeof(hw_iovec_t), 1024)), hw_sys_writev(ARG(0), ARG(1), ARG(2))) \
    X(22,  pipe,       PIPE,        PTRS(OUT(0, 8)), hw_sys_pipe2(ARG(0), 0)) \
    X(32,  dup,        DUP,         NOPTR, linux_sys_dup(ARG(0))) \
    X(33,  dup2,       DUP2,        NOPTR, hw_sys_dup2(ARG(0), ARG(1))) \
    X(40,  sendfile,   SENDFILE,    NOPTR, -VIBEOS_ENOSYS) \
    X(79,  getcwd,     GETCWD,      PTRS(OUT(0, 2)), hw_sys_getcwd(ARG(0), ARG(1))) \
    X(83,  mkdir,      MKDIR,       NOPTR, hw_sys_mkdir(ARG(0))) \
    X(87,  unlink,     UNLINK,      NOPTR, hw_sys_unlink(ARG(0))) \
    X(217, getdents64, GETDENTS,    PTRS(OUT_BUF(1, 2)), hw_sys_getdents64(ARG(0), ARG(1), ARG(2))) \
    X(257, openat,     OPEN_AT,     NOPTR, hw_sys_openat(ARG(0), ARG(1), ARG(2))) \
    X(262, newfstatat, STAT_AT,     PTRS(OUT(2, STAT_SIZE)), hw_sys_newfstatat(ARG(0), ARG(1), ARG(2), ARG(3))) \
    X(267, readlinkat, READLINK_AT, NOPTR, hw_sys_readlinkat(ARG(0), ARG(1), ARG(2), ARG(3))) \
    X(293, pipe2,      PIPE2,       PTRS(OUT(0, 8)), hw_sys_pipe2(ARG(0), ARG(1)))

LINUX_DEFINE_SYSCALLS(fs, LINUX_FS_SYSCALLS)
