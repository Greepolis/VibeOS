/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Model-based torture of Linux file, memory and process syscalls through the real handler table.
 *
 * Every operation enters linux_syscall() on ksvc_fake, just as a guest task
 * enters it through the architecture trampoline. The oracle owns its own file
 * bytes, open descriptions, descriptor map and shared offsets; it never reads
 * the fake filesystem or the kernel's descriptor table.
 *
 *     vibeos_abi_torture <seed> <rounds>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>

#include "ksvc_fake.h"
#include "vibeos/abi_linux.h"
#ifdef VIBEOS_TEST_NT_ABI
#include "vibeos/abi_nt.h"
#include "vibeos/nt_layout.h"
#endif
#include "vibeos/linux_exports.h"
#include "vibeos/linux_layout.h"

#define MODEL_FDS 1024u
#define MODEL_DESCS 1024u
#define MODEL_FILE_CAP 8192u
#define INITIAL_FILE_SIZE 96u

/* x86-64 Linux syscall numbers used by this model. Keep names beside the
 * model so a changed literal cannot silently change what an operation means. */
enum {
    ABI_NR_READ = 0,
    ABI_NR_WRITE = 1,
    ABI_NR_OPEN = 2,
    ABI_NR_CLOSE = 3,
    ABI_NR_LSEEK = 8,
    ABI_NR_PREAD64 = 17,
    ABI_NR_DUP = 32,
    ABI_NR_DUP2 = 33,
    ABI_NR_PIPE2 = 293,
    ABI_NR_DUP3 = 292,
    ABI_NR_FCNTL = 72,
    ABI_NR_CLOSE_RANGE = 436,
    ABI_NR_PWRITE64 = 18,
    ABI_NR_READV = 19,
    ABI_NR_WRITEV = 20,
    ABI_NR_PREADV = 295,
    ABI_NR_PWRITEV = 296,
    ABI_NR_OPENAT = 257,
    ABI_NR_MKDIRAT = 258,
    ABI_NR_UNLINKAT = 263,
    ABI_NR_FTRUNCATE = 77,
    ABI_NR_FSTAT = 5,
    ABI_NR_NEWFSTATAT = 262,
    ABI_NR_GETDENTS64 = 217,
    ABI_NR_GETCWD = 79,
    ABI_NR_CHDIR = 80,
    ABI_NR_LINKAT = 265,
    ABI_NR_SYMLINKAT = 266,
    ABI_NR_READLINKAT = 267,
    ABI_NR_MMAP = 9,
    ABI_NR_MPROTECT = 10,
    ABI_NR_MUNMAP = 11,
    ABI_NR_FORK = 57,
    ABI_NR_WAIT4 = 61,
    ABI_NR_RENAMEAT2 = 316
};

#define ABI_O_NONBLOCK 0x800u
#define ABI_O_CLOEXEC 0x80000u
#define ABI_FD_CLOEXEC 1u
#define ABI_CLOSE_RANGE_CLOEXEC 4u
#define ABI_PIPE_CAPACITY 65536u
#define ABI_AT_FDCWD 0xFFFFFF9Cu
#define ABI_AT_REMOVEDIR 0x200u
#define ABI_O_CREAT 0x40u
#define ABI_O_EXCL 0x80u
#define ABI_O_TRUNC 0x200u
#define ABI_O_APPEND 0x400u
#define ABI_O_DIRECTORY 0x10000u
#define ABI_O_WRONLY 1u
#define ABI_O_RDWR 2u
#define ABI_RENAME_NOREPLACE 1u
#define ABI_F_DUPFD 0u
#define ABI_F_GETFD 1u
#define ABI_F_SETFD 2u
#define ABI_F_DUPFD_CLOEXEC 1030u
#define ABI_WNOHANG 1u

typedef struct {
    int used;
    uint32_t refs;
    uint64_t pos;
} model_desc_t;

static uint64_t g_rng;
static uint64_t g_seed;
static uint8_t g_data[MODEL_FILE_CAP];
static uint32_t g_size;
static int16_t g_fd[MODEL_FDS];
static uint8_t g_fdflags[MODEL_FDS];
static model_desc_t g_desc[MODEL_DESCS];

static uint64_t rnd64(void) {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return g_rng;
}

static int fail(unsigned round, const char *why) {
    printf("ABI_TORTURE_FAIL seed=%llu round=%u: %s\n",
           (unsigned long long)g_seed, round, why);
    return 1;
}

static long call_sys(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2,
                     uint64_t a3) {
    return kf_call(linux_syscall, nr, a0, a1, a2, a3, 0, 0, 0);
}

static long call_sys5(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2,
                      uint64_t a3, uint64_t a4) {
    return kf_call(linux_syscall, nr, a0, a1, a2, a3, a4, 0, 0);
}

static long call_sys6(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2,
                      uint64_t a3, uint64_t a4, uint64_t a5) {
    return kf_call(linux_syscall, nr, a0, a1, a2, a3, a4, a5, 0);
}

static int live_fd_count(void) {
    uint32_t i;
    int n = 0;
    for (i = 3u; i < MODEL_FDS; i++) n += g_fd[i] >= 0;
    return n;
}

static int choose_live_fd(void) {
    uint32_t start = 3u + (uint32_t)(rnd64() % (MODEL_FDS - 3u));
    uint32_t k;
    for (k = 0; k < MODEL_FDS - 3u; k++) {
        uint32_t fd = 3u + (start - 3u + k) % (MODEL_FDS - 3u);
        if (g_fd[fd] >= 0) return (int)fd;
    }
    return -1;
}

static int alloc_fd(uint32_t minimum) {
    uint32_t fd;
    if (minimum < 3u) minimum = 3u;
    for (fd = minimum; fd < MODEL_FDS; fd++) {
        if (g_fd[fd] == -1) return (int)fd;
    }
    return -1;
}

static int alloc_desc(void) {
    uint32_t i;
    for (i = 0; i < MODEL_DESCS; i++) {
        if (!g_desc[i].used) {
            g_desc[i].used = 1;
            g_desc[i].refs = 0;
            g_desc[i].pos = 0;
            return (int)i;
        }
    }
    return -1;
}

static void drop_fd(uint32_t fd) {
    int d = g_fd[fd];
    if (d >= 0) {
        if (--g_desc[d].refs == 0u) {
            memset(&g_desc[d], 0, sizeof(g_desc[d]));
        }
    }
    g_fd[fd] = -1;
    g_fdflags[fd] = 0u;
}

static int open_model(uint64_t path, unsigned round) {
    int fd = alloc_fd(3u), d;
    long got;
    if (fd < 0) return 0;
    d = alloc_desc();
    if (d < 0) return fail(round, "reference description table exhausted");
    got = call_sys(ABI_NR_OPEN, path, 2u /* O_RDWR */, 0u, 0u);
    if (got != fd) return fail(round, "open returned a descriptor different from the model");
    g_fd[fd] = (int16_t)d;
    g_fdflags[fd] = 0u;
    g_desc[d].refs = 1u;
    return 0;
}

static int check_offsets(unsigned round);

static int check_file(unsigned round, uint64_t buffer) {
    int fd = choose_live_fd();
    uint32_t expected;
    long got;
    if (fd < 0) return 0;
    expected = g_size;
    memset(kf_uptr(buffer), 0xA5, MODEL_FILE_CAP);
    got = call_sys(ABI_NR_PREAD64, (uint64_t)fd, buffer, expected, 0u);
    if (got != (long)expected ||
        memcmp(kf_uptr(buffer), g_data, expected) != 0) {
        return fail(round, "pread bytes differ from the independent file model");
    }
    /* pread64 reads at an explicit offset and must leave every shared open-file
     * description position unchanged, including aliases created by dup/dup2. */
    if (check_offsets(round)) return 1;
    return 0;
}

static int check_offsets(unsigned round) {
    uint32_t fd;
    for (fd = 3u; fd < MODEL_FDS; fd++) {
        long got;
        int d = g_fd[fd];
        if (d < 0) continue;
        got = call_sys(ABI_NR_LSEEK, fd, 0u, 1u, 0u); /* lseek(fd, 0, SEEK_CUR) */
        if (got < 0 || (uint64_t)got != g_desc[d].pos) {
            return fail(round, "descriptor position differs from shared-description model");
        }
    }
    return 0;
}


/* A pipe is not a regular file: it is a byte queue with independent read and
 * write ends. Keep the expected bytes here, not in the fake pipe's ring. */
static int test_pipe_model(unsigned round) {
    static const uint8_t payload[] = {
        0x13u, 0x27u, 0x39u, 0x4Bu, 0x5Du, 0x6Fu, 0x71u, 0x82u,
        0x94u, 0xA6u, 0xB8u, 0xCAu
    };
    uint64_t fds_uptr = kf_ualloc(2u * sizeof(int));
    uint64_t in = kf_ualloc(sizeof(payload));
    uint64_t out = kf_ualloc(sizeof(payload));
    uint64_t large_in = kf_ualloc(ABI_PIPE_CAPACITY + 32u);
    uint64_t large_out = kf_ualloc(ABI_PIPE_CAPACITY);
    uint32_t i;
    int *fds;
    if (!fds_uptr || !in || !out || !large_in || !large_out) {
        return fail(round, "pipe test user memory allocation");
    }
    fds = (int *)kf_uptr(fds_uptr);
    memcpy(kf_uptr(in), payload, sizeof(payload));

    if (call_sys(ABI_NR_PIPE2, fds_uptr, 0u, 0u, 0u) != 0) {
        return fail(round, "pipe2 failed to create a blocking pipe");
    }
    if (call_sys(ABI_NR_WRITE, (uint64_t)fds[1], in, sizeof(payload), 0u) != (long)sizeof(payload)) {
        return fail(round, "pipe write length differs from byte-queue model");
    }
    memset(kf_uptr(out), 0, sizeof(payload));
    if (call_sys(ABI_NR_READ, (uint64_t)fds[0], out, 5u, 0u) != 5 ||
        memcmp(kf_uptr(out), payload, 5u) != 0) {
        return fail(round, "pipe partial read did not return the queue prefix");
    }
    memset(kf_uptr(out), 0, sizeof(payload));
    if (call_sys(ABI_NR_READ, (uint64_t)fds[0], out, sizeof(payload), 0u) != 7 ||
        memcmp(kf_uptr(out), payload + 5u, 7u) != 0) {
        return fail(round, "pipe second read did not return the remaining queue bytes");
    }
    if (call_sys(ABI_NR_CLOSE, (uint64_t)fds[1], 0u, 0u, 0u) != 0 ||
        call_sys(ABI_NR_READ, (uint64_t)fds[0], out, 1u, 0u) != 0) {
        return fail(round, "pipe did not report EOF after its last writer closed");
    }
    if (call_sys(ABI_NR_CLOSE, (uint64_t)fds[0], 0u, 0u, 0u) != 0) {
        return fail(round, "pipe read-end close failed");
    }

    if (call_sys(ABI_NR_PIPE2, fds_uptr, ABI_O_NONBLOCK, 0u, 0u) != 0) {
        return fail(round, "pipe2 rejected O_NONBLOCK");
    }
    if (call_sys(ABI_NR_READ, (uint64_t)fds[0], out, 1u, 0u) != -VIBEOS_EAGAIN) {
        return fail(round, "empty nonblocking pipe read did not return EAGAIN");
    }
    /* Fill the pipe past capacity. A nonblocking write must return the
     * accepted prefix, not lose it or claim that the whole buffer was queued. */
    for (i = 0; i < ABI_PIPE_CAPACITY + 32u; i++) {
        ((uint8_t *)kf_uptr(large_in))[i] = (uint8_t)(i * 29u + 7u);
    }
    if (call_sys(ABI_NR_WRITE, (uint64_t)fds[1], large_in,
                 ABI_PIPE_CAPACITY + 32u, 0u) != ABI_PIPE_CAPACITY ||
        call_sys(ABI_NR_CLOSE, (uint64_t)fds[1], 0u, 0u, 0u) != 0) {
        return fail(round, "nonblocking pipe write did not return the capacity-limited prefix");
    }
    memset(kf_uptr(large_out), 0, ABI_PIPE_CAPACITY);
    if (call_sys(ABI_NR_READ, (uint64_t)fds[0], large_out, ABI_PIPE_CAPACITY, 0u) != ABI_PIPE_CAPACITY ||
        memcmp(kf_uptr(large_out), kf_uptr(large_in), ABI_PIPE_CAPACITY) != 0 ||
        call_sys(ABI_NR_READ, (uint64_t)fds[0], out, 1u, 0u) != 0) {
        return fail(round, "nonblocking pipe did not preserve and drain the accepted prefix before EOF");
    }
    if (call_sys(ABI_NR_CLOSE, (uint64_t)fds[0], 0u, 0u, 0u) != 0) {
        return fail(round, "nonblocking pipe read-end close failed");
    }
    return 0;
}


/* Descriptor flags belong to descriptor numbers, while offsets belong to
 * shared open descriptions. This model deliberately tracks them separately. */
static int test_descriptor_semantics(unsigned round) {
    int d = g_fd[3];
    long got;
    if (d < 0) return fail(round, "descriptor semantics probe has no source fd");

    got = call_sys(ABI_NR_DUP3, 3u, 10u, ABI_O_CLOEXEC, 0u);
    if (got != 10) return fail(round, "dup3 did not install the requested descriptor");
    g_fd[10] = (int16_t)d;
    g_fdflags[10] = ABI_FD_CLOEXEC;
    g_desc[d].refs++;
    if (call_sys(ABI_NR_FCNTL, 10u, ABI_F_GETFD, 0u, 0u) != ABI_FD_CLOEXEC) {
        return fail(round, "dup3 did not preserve FD_CLOEXEC on its new descriptor");
    }
    if (call_sys(ABI_NR_DUP3, 3u, 3u, 0u, 0u) != -VIBEOS_EINVAL) {
        return fail(round, "dup3(oldfd, oldfd) did not return EINVAL");
    }

    got = call_sys(ABI_NR_FCNTL, 3u, ABI_F_DUPFD, 7u, 0u);
    if (got != 7) return fail(round, "fcntl(F_DUPFD) did not choose the lowest free fd at or above min");
    g_fd[7] = (int16_t)d;
    g_fdflags[7] = 0u;
    g_desc[d].refs++;

    got = call_sys(ABI_NR_FCNTL, 3u, ABI_F_DUPFD_CLOEXEC, 8u, 0u);
    if (got != 8) return fail(round, "fcntl(F_DUPFD_CLOEXEC) did not choose the requested minimum");
    g_fd[8] = (int16_t)d;
    g_fdflags[8] = ABI_FD_CLOEXEC;
    g_desc[d].refs++;
    if (call_sys(ABI_NR_FCNTL, 8u, ABI_F_GETFD, 0u, 0u) != ABI_FD_CLOEXEC) {
        return fail(round, "F_DUPFD_CLOEXEC did not set the descriptor flag");
    }

    if (call_sys(ABI_NR_CLOSE_RANGE, 7u, 8u, ABI_CLOSE_RANGE_CLOEXEC, 0u) != 0) {
        return fail(round, "close_range(CLOEXEC) failed");
    }
    if (call_sys(ABI_NR_FCNTL, 7u, ABI_F_GETFD, 0u, 0u) != ABI_FD_CLOEXEC ||
        call_sys(ABI_NR_FCNTL, 8u, ABI_F_GETFD, 0u, 0u) != ABI_FD_CLOEXEC) {
        return fail(round, "close_range(CLOEXEC) did not mark every descriptor in range");
    }
    g_fdflags[7] = g_fdflags[8] = ABI_FD_CLOEXEC;

    if (call_sys(ABI_NR_CLOSE_RANGE, 7u, 8u, 0u, 0u) != 0) {
        return fail(round, "close_range did not close the requested descriptor range");
    }
    drop_fd(7u);
    drop_fd(8u);
    if (call_sys(ABI_NR_CLOSE, 10u, 0u, 0u, 0u) != 0) {
        return fail(round, "close of dup3 descriptor failed");
    }
    drop_fd(10u);
    return check_offsets(round);
}


/* Linux's per-process descriptor limit is 1024. Fill every slot with aliases
 * of one open description, prove the next install is EMFILE, then free and
 * reuse a middle slot. */
static int test_fd_limit(unsigned round) {
    const int description = g_fd[3];
    uint32_t fd;
    long got;
    if (description < 0) return fail(round, "fd-limit probe has no source descriptor");

    for (fd = 5u; fd < MODEL_FDS; fd++) {
        int expected = alloc_fd(3u);
        if (expected != (int)fd) return fail(round, "reference fd table did not expose the next free slot");
        got = call_sys(ABI_NR_DUP, 3u, 0u, 0u, 0u);
        if (got != (long)fd) return fail(round, "dup failed before the 1024-descriptor limit");
        g_fd[fd] = (int16_t)description;
        g_fdflags[fd] = 0u;
        g_desc[description].refs++;
    }
    if (call_sys(ABI_NR_DUP, 3u, 0u, 0u, 0u) != -VIBEOS_EMFILE) {
        return fail(round, "descriptor table exhaustion did not return EMFILE");
    }

    if (call_sys(ABI_NR_CLOSE, 512u, 0u, 0u, 0u) != 0) {
        return fail(round, "closing a middle descriptor at capacity failed");
    }
    drop_fd(512u);
    got = call_sys(ABI_NR_DUP, 3u, 0u, 0u, 0u);
    if (got != 512) return fail(round, "freed middle descriptor was not reused first");
    g_fd[512] = (int16_t)description;
    g_fdflags[512] = 0u;
    g_desc[description].refs++;

    for (fd = 5u; fd < MODEL_FDS; fd++) {
        if (g_fd[fd] >= 0) {
            if (call_sys(ABI_NR_CLOSE, fd, 0u, 0u, 0u) != 0) {
                return fail(round, "closing a descriptor after the capacity probe failed");
            }
            drop_fd(fd);
        }
    }
    return check_offsets(round);
}


typedef struct {
    uint64_t base;
    uint64_t len;
} abi_iovec_t;

/* Positional calls must not move the open description's offset. Vector calls
 * must concatenate/split bytes in iovec order while ordinary readv/writev do
 * advance that shared offset. */
static int test_vector_io(unsigned round) {
    static const uint8_t positional[] = {0xD1u, 0xD2u, 0xD3u};
    static const uint8_t first[] = {0x11u, 0x22u, 0x33u};
    static const uint8_t second[] = {0x44u, 0x55u, 0x66u, 0x77u};
    uint64_t pwrite_buf = kf_ualloc(sizeof(positional));
    uint64_t first_buf = kf_ualloc(sizeof(first));
    uint64_t second_buf = kf_ualloc(sizeof(second));
    uint64_t out_first = kf_ualloc(sizeof(first));
    uint64_t out_second = kf_ualloc(sizeof(second));
    uint64_t iov_uptr = kf_ualloc(2u * sizeof(abi_iovec_t));
    abi_iovec_t *iov;
    uint64_t pos;
    int d = g_fd[3];
    long got;
    if (d < 0 || !pwrite_buf || !first_buf || !second_buf || !out_first ||
        !out_second || !iov_uptr) {
        return fail(round, "vector I/O model allocation");
    }
    pos = g_desc[d].pos;
    memcpy(kf_uptr(pwrite_buf), positional, sizeof(positional));
    memcpy(kf_uptr(first_buf), first, sizeof(first));
    memcpy(kf_uptr(second_buf), second, sizeof(second));

    if (call_sys(ABI_NR_PWRITE64, 3u, pwrite_buf, sizeof(positional), 20u) !=
        (long)sizeof(positional)) {
        return fail(round, "pwrite64 did not write the requested positional bytes");
    }
    memcpy(g_data + 20u, positional, sizeof(positional));
    if (check_offsets(round)) return 1;
    memset(kf_uptr(out_first), 0, sizeof(first));
    if (call_sys(ABI_NR_PREAD64, 3u, out_first, sizeof(positional), 20u) !=
        (long)sizeof(positional) || memcmp(kf_uptr(out_first), positional, sizeof(positional)) != 0) {
        return fail(round, "pwrite64 bytes were not visible to pread64");
    }
    if (check_offsets(round)) return 1;

    iov = (abi_iovec_t *)kf_uptr(iov_uptr);
    iov[0].base = first_buf;
    iov[0].len = sizeof(first);
    iov[1].base = second_buf;
    iov[1].len = sizeof(second);
    got = call_sys(ABI_NR_WRITEV, 3u, iov_uptr, 2u, 0u);
    if (got != (long)(sizeof(first) + sizeof(second))) {
        return fail(round, "writev did not report the concatenated vector length");
    }
    memcpy(g_data + pos, first, sizeof(first));
    memcpy(g_data + pos + sizeof(first), second, sizeof(second));
    g_desc[d].pos += sizeof(first) + sizeof(second);
    if (g_desc[d].pos > g_size) g_size = (uint32_t)g_desc[d].pos;

    if (call_sys(ABI_NR_LSEEK, 3u, pos, 0u, 0u) != (long)pos) {
        return fail(round, "lseek before readv did not restore the model position");
    }
    g_desc[d].pos = pos;
    iov[0].base = out_first;
    iov[0].len = sizeof(first);
    iov[1].base = out_second;
    iov[1].len = sizeof(second);
    got = call_sys(ABI_NR_READV, 3u, iov_uptr, 2u, 0u);
    if (got != (long)(sizeof(first) + sizeof(second)) ||
        memcmp(kf_uptr(out_first), first, sizeof(first)) != 0 ||
        memcmp(kf_uptr(out_second), second, sizeof(second)) != 0) {
        return fail(round, "readv did not split the file bytes across vectors in order");
    }
    g_desc[d].pos += sizeof(first) + sizeof(second);
    if (check_offsets(round)) return 1;

    iov[0].base = first_buf;
    iov[0].len = sizeof(first);
    iov[1].base = second_buf;
    iov[1].len = sizeof(second);
    got = call_sys(ABI_NR_PWRITEV, 3u, iov_uptr, 2u, 40u);
    if (got != (long)(sizeof(first) + sizeof(second))) {
        return fail(round, "pwritev did not write both positional vectors");
    }
    memcpy(g_data + 40u, first, sizeof(first));
    memcpy(g_data + 40u + sizeof(first), second, sizeof(second));
    if (check_offsets(round)) return 1;

    memset(kf_uptr(out_first), 0, sizeof(first));
    memset(kf_uptr(out_second), 0, sizeof(second));
    iov[0].base = out_first;
    iov[0].len = sizeof(first);
    iov[1].base = out_second;
    iov[1].len = sizeof(second);
    got = call_sys(ABI_NR_PREADV, 3u, iov_uptr, 2u, 40u);
    if (got != (long)(sizeof(first) + sizeof(second)) ||
        memcmp(kf_uptr(out_first), first, sizeof(first)) != 0 ||
        memcmp(kf_uptr(out_second), second, sizeof(second)) != 0) {
        return fail(round, "preadv did not read positional vectors in order");
    }
    return check_offsets(round);
}


/* Exercise cwd and path resolution through a real directory descriptor. The
 * expected file contents are owned by this test, not queried from tmpfs. */
static int test_path_model(unsigned round) {
    static const uint8_t initial[] = {'o', 'l', 'd'};
    static const uint8_t first[] = {'A'};
    static const uint8_t appended[] = {'B'};
    static const uint8_t expected[] = {'A', 'B'};
    uint64_t dir_path = kf_ualloc(sizeof("/tmp/abi-model-dir"));
    uint64_t file_path = kf_ualloc(sizeof("item"));
    uint64_t other_path = kf_ualloc(sizeof("other"));
    uint64_t renamed_path = kf_ualloc(sizeof("renamed"));
    uint64_t in = kf_ualloc(sizeof(initial));
    uint64_t out = kf_ualloc(sizeof(initial));
    uint64_t stat_uptr = kf_ualloc(sizeof(linux_stat_t));
    uint64_t dirents_uptr = kf_ualloc(512u);
    uint64_t cwd_uptr = kf_ualloc(64u);
    uint64_t link_target = kf_ualloc(sizeof("item"));
    uint64_t symlink_path = kf_ualloc(sizeof("item-link"));
    uint64_t hardlink_path = kf_ualloc(sizeof("item-hard"));
    uint64_t cross_path = kf_ualloc(sizeof("cross-mount"));
    uint64_t root_file_path = kf_ualloc(sizeof("/model"));
    uint64_t dot_path = kf_ualloc(sizeof("."));
    uint64_t linkbuf = kf_ualloc(16u);
    linux_stat_t *st;
    int dirfd, original, truncfd, appendfd, truncated_check_fd, otherfd, verifyfd;
    if (!dir_path || !file_path || !other_path || !renamed_path || !in || !out ||
        !stat_uptr || !dirents_uptr || !cwd_uptr || !link_target || !symlink_path ||
        !hardlink_path || !cross_path || !root_file_path || !dot_path || !linkbuf) {
        return fail(round, "path model user memory allocation");
    }
    memcpy(kf_uptr(dir_path), "/tmp/abi-model-dir", sizeof("/tmp/abi-model-dir"));
    memcpy(kf_uptr(file_path), "item", sizeof("item"));
    memcpy(kf_uptr(other_path), "other", sizeof("other"));
    memcpy(kf_uptr(renamed_path), "renamed", sizeof("renamed"));
    memcpy(kf_uptr(link_target), "item", sizeof("item"));
    memcpy(kf_uptr(symlink_path), "item-link", sizeof("item-link"));
    memcpy(kf_uptr(hardlink_path), "item-hard", sizeof("item-hard"));
    memcpy(kf_uptr(cross_path), "cross-mount", sizeof("cross-mount"));
    memcpy(kf_uptr(root_file_path), "/model", sizeof("/model"));
    memcpy(kf_uptr(dot_path), ".", sizeof("."));

    if (call_sys(ABI_NR_MKDIRAT, ABI_AT_FDCWD, dir_path, 0700u, 0u) != 0) {
        return fail(round, "mkdirat did not create the model directory");
    }
    /* cwd is process state, not an alias for the directory descriptor. */
    dirfd = (int)call_sys(ABI_NR_OPENAT, ABI_AT_FDCWD, dir_path, ABI_O_DIRECTORY, 0u);
    if (dirfd < 0) return fail(round, "openat did not return a directory descriptor");
    if (call_sys(ABI_NR_CHDIR, dir_path, 0u, 0u, 0u) != 0 ||
        call_sys(ABI_NR_GETCWD, cwd_uptr, 64u, 0u, 0u) != (long)sizeof("/tmp/abi-model-dir") ||
        strcmp((const char *)kf_uptr(cwd_uptr), "/tmp/abi-model-dir") != 0 ||
        call_sys(ABI_NR_GETCWD, cwd_uptr, 3u, 0u, 0u) != -VIBEOS_ERANGE) {
        return fail(round, "chdir/getcwd did not preserve the absolute working directory or ERANGE");
    }
    /* The same name must resolve through cwd and through the explicit dirfd. */
    st = (linux_stat_t *)kf_uptr(stat_uptr);
    if (call_sys(ABI_NR_FSTAT, (uint64_t)dirfd, stat_uptr, 0u, 0u) != 0 ||
        (st->st_mode & 0170000u) != 0040000u) {
        return fail(round, "fstat did not report a directory descriptor's type");
    }
    original = (int)call_sys(ABI_NR_OPENAT, (uint64_t)dirfd, file_path,
                             ABI_O_CREAT | ABI_O_EXCL | ABI_O_RDWR | ABI_O_CLOEXEC, 0600u);
    if (original < 0) return fail(round, "openat(dirfd, relative, O_CREAT|O_EXCL) failed");
    if (call_sys(ABI_NR_FCNTL, (uint64_t)original, ABI_F_GETFD, 0u, 0u) != ABI_FD_CLOEXEC ||
        call_sys(ABI_NR_FCNTL, (uint64_t)original, ABI_F_SETFD, 0u, 0u) != 0 ||
        call_sys(ABI_NR_FCNTL, (uint64_t)original, ABI_F_GETFD, 0u, 0u) != 0 ||
        call_sys(ABI_NR_FCNTL, (uint64_t)original, ABI_F_SETFD, ABI_FD_CLOEXEC, 0u) != 0 ||
        call_sys(ABI_NR_FCNTL, (uint64_t)original, ABI_F_GETFD, 0u, 0u) != ABI_FD_CLOEXEC) {
        return fail(round, "O_CLOEXEC and fcntl(F_SETFD/F_GETFD) did not maintain descriptor flags");
    }

    memcpy(kf_uptr(in), initial, sizeof(initial));
    if (call_sys(ABI_NR_WRITE, (uint64_t)original, in, sizeof(initial), 0u) !=
        (long)sizeof(initial)) {
        return fail(round, "initial relative-path file write failed");
    }
    verifyfd = (int)call_sys(ABI_NR_OPENAT, ABI_AT_FDCWD, file_path, 0u, 0u);
    if (verifyfd < 0 || call_sys(ABI_NR_PREAD64, (uint64_t)verifyfd, out, sizeof(initial), 0u) !=
        (long)sizeof(initial) || memcmp(kf_uptr(out), initial, sizeof(initial)) != 0 ||
        call_sys(ABI_NR_CLOSE, (uint64_t)verifyfd, 0u, 0u, 0u) != 0 ||
        call_sys(ABI_NR_CHDIR, file_path, 0u, 0u, 0u) != -VIBEOS_ENOTDIR) {
        return fail(round, "cwd-relative lookup or chdir(non-directory) semantics differ");
    }
    /* Enumeration is checked against the known one-level directory model. */
    /* Symlink contents are raw bytes: readlinkat must not append a NUL. */
    memset(kf_uptr(linkbuf), 0xCC, 16u);
    if (call_sys(ABI_NR_SYMLINKAT, link_target, (uint64_t)dirfd, symlink_path, 0u) != 0 ||
        call_sys(ABI_NR_READLINKAT, (uint64_t)dirfd, symlink_path, linkbuf, 16u) != 4 ||
        memcmp(kf_uptr(linkbuf), "item", 4u) != 0 ||
        ((uint8_t *)kf_uptr(linkbuf))[4] != 0xCC ||
        call_sys(ABI_NR_READLINKAT, (uint64_t)dirfd, symlink_path, linkbuf, 2u) != 2 ||
        memcmp(kf_uptr(linkbuf), "it", 2u) != 0 ||
        call_sys(ABI_NR_READLINKAT, (uint64_t)dirfd, symlink_path, linkbuf, 0u) != -VIBEOS_EINVAL ||
        call_sys(ABI_NR_READLINKAT, (uint64_t)dirfd, file_path, linkbuf, 16u) != -VIBEOS_EINVAL ||
        call_sys(ABI_NR_SYMLINKAT, link_target, (uint64_t)dirfd, symlink_path, 0u) != -VIBEOS_EEXIST) {
        return fail(round, "symlinkat/readlinkat target bytes, truncation or errors differ");
    }
    verifyfd = (int)call_sys(ABI_NR_OPENAT, (uint64_t)dirfd, symlink_path, 0u, 0u);
    if (verifyfd < 0 || call_sys(ABI_NR_PREAD64, (uint64_t)verifyfd, out, sizeof(initial), 0u) !=
        (long)sizeof(initial) || memcmp(kf_uptr(out), initial, sizeof(initial)) != 0 ||
        call_sys(ABI_NR_CLOSE, (uint64_t)verifyfd, 0u, 0u, 0u) != 0) {
        return fail(round, "opening a relative symlink did not follow its target");
    }

    /* A hard link shares inode identity and is refused across mount points. */
    if (call_sys5(ABI_NR_LINKAT, (uint64_t)dirfd, file_path, (uint64_t)dirfd,
                  hardlink_path, 0u) != 0 ||
        call_sys5(ABI_NR_LINKAT, ABI_AT_FDCWD, root_file_path, (uint64_t)dirfd,
                  cross_path, 0u) != -VIBEOS_EXDEV ||
        call_sys5(ABI_NR_LINKAT, (uint64_t)dirfd, dot_path, (uint64_t)dirfd,
                  cross_path, 0u) != -VIBEOS_EPERM) {
        return fail(round, "linkat creation or cross-mount refusal differs");
    }
    st = (linux_stat_t *)kf_uptr(stat_uptr);
    if (call_sys(ABI_NR_FSTAT, (uint64_t)original, stat_uptr, 0u, 0u) != 0 ||
        st->st_nlink != 2u) {
        return fail(round, "linkat did not increase the source inode link count");
    }
    verifyfd = (int)call_sys(ABI_NR_OPENAT, (uint64_t)dirfd, hardlink_path, 0u, 0u);
    if (verifyfd < 0 || call_sys(ABI_NR_PREAD64, (uint64_t)verifyfd, out, sizeof(initial), 0u) !=
        (long)sizeof(initial) || memcmp(kf_uptr(out), initial, sizeof(initial)) != 0 ||
        call_sys(ABI_NR_CLOSE, (uint64_t)verifyfd, 0u, 0u, 0u) != 0) {
        return fail(round, "hard link did not expose the source inode contents");
    }
    {
        long nread = call_sys(ABI_NR_GETDENTS64, (uint64_t)dirfd, dirents_uptr, 512u, 0u);
        uint64_t at = 0;
        int saw_dot = 0, saw_dotdot = 0, saw_item = 0;
        if (nread <= 0) return fail(round, "getdents64 returned no entries for the model directory");
        while (at < (uint64_t)nread) {
            linux_dirent64_t *de = (linux_dirent64_t *)((uint8_t *)kf_uptr(dirents_uptr) + at);
            if (de->d_reclen < sizeof(linux_dirent64_t) || at + de->d_reclen > (uint64_t)nread) {
                return fail(round, "getdents64 returned an invalid record boundary");
            }
            if (strcmp(de->d_name, ".") == 0) saw_dot = 1;
            else if (strcmp(de->d_name, "..") == 0) saw_dotdot = 1;
            else if (strcmp(de->d_name, "item") == 0 && de->d_type == 8u) saw_item = 1;
            at += de->d_reclen;
        }
        if (!saw_dot || !saw_dotdot || !saw_item) {
            return fail(round, "getdents64 entries differ from the independent directory-tree model");
        }
    }
    if (call_sys(ABI_NR_OPENAT, (uint64_t)dirfd, file_path,
                 ABI_O_CREAT | ABI_O_EXCL | ABI_O_RDWR, 0600u) != -VIBEOS_EEXIST) {
        return fail(round, "openat O_CREAT|O_EXCL did not reject an existing path");
    }

    /* NOREPLACE must leave both names and their bytes untouched. */
    otherfd = (int)call_sys(ABI_NR_OPENAT, (uint64_t)dirfd, other_path,
                            ABI_O_CREAT | ABI_O_EXCL | ABI_O_RDWR, 0600u);
    if (otherfd < 0) return fail(round, "creating rename destination failed");
    ((uint8_t *)kf_uptr(in))[0] = 'Z';
    if (call_sys(ABI_NR_WRITE, (uint64_t)otherfd, in, 1u, 0u) != 1 ||
        call_sys5(ABI_NR_RENAMEAT2, (uint64_t)dirfd, file_path, (uint64_t)dirfd,
                  other_path, ABI_RENAME_NOREPLACE) != -VIBEOS_EEXIST) {
        return fail(round, "renameat2 RENAME_NOREPLACE did not reject an existing destination");
    }
    verifyfd = (int)call_sys(ABI_NR_OPENAT, (uint64_t)dirfd, file_path, 0u, 0u);
    if (verifyfd < 0 || call_sys(ABI_NR_PREAD64, (uint64_t)verifyfd, out, sizeof(initial), 0u) !=
        (long)sizeof(initial) || memcmp(kf_uptr(out), initial, sizeof(initial)) != 0) {
        return fail(round, "failed RENAME_NOREPLACE changed the source path");
    }
    (void)call_sys(ABI_NR_CLOSE, (uint64_t)verifyfd, 0u, 0u, 0u);
    verifyfd = (int)call_sys(ABI_NR_OPENAT, (uint64_t)dirfd, other_path, 0u, 0u);
    if (verifyfd < 0 || call_sys(ABI_NR_PREAD64, (uint64_t)verifyfd, out, 1u, 0u) != 1 ||
        ((uint8_t *)kf_uptr(out))[0] != 'Z') {
        return fail(round, "failed RENAME_NOREPLACE changed the destination path");
    }
    (void)call_sys(ABI_NR_CLOSE, (uint64_t)verifyfd, 0u, 0u, 0u);
    if (call_sys5(ABI_NR_RENAMEAT2, (uint64_t)dirfd, file_path, (uint64_t)dirfd,
                  renamed_path, 0u) != 0 ||
        call_sys(ABI_NR_OPENAT, (uint64_t)dirfd, file_path, 0u, 0u) != -VIBEOS_ENOENT) {
        return fail(round, "renameat2 did not move the source to its new relative name");
    }
    verifyfd = (int)call_sys(ABI_NR_OPENAT, (uint64_t)dirfd, renamed_path, 0u, 0u);
    if (verifyfd < 0 || call_sys(ABI_NR_PREAD64, (uint64_t)verifyfd, out, sizeof(initial), 0u) !=
        (long)sizeof(initial) || memcmp(kf_uptr(out), initial, sizeof(initial)) != 0) {
        return fail(round, "renamed path does not expose the original file contents");
    }
    (void)call_sys(ABI_NR_CLOSE, (uint64_t)verifyfd, 0u, 0u, 0u);

    /* Truncation and append are visible through already-open descriptions. */
    truncfd = (int)call_sys(ABI_NR_OPENAT, (uint64_t)dirfd, renamed_path,
                            ABI_O_TRUNC | ABI_O_RDWR, 0600u);
    if (truncfd < 0 || call_sys(ABI_NR_PREAD64, (uint64_t)original, out, 1u, 0u) != 0) {
        return fail(round, "openat O_TRUNC did not empty the already-open file");
    }
    memcpy(kf_uptr(in), first, sizeof(first));
    if (call_sys(ABI_NR_WRITE, (uint64_t)truncfd, in, sizeof(first), 0u) != 1) {
        return fail(round, "write after O_TRUNC failed");
    }

    appendfd = (int)call_sys(ABI_NR_OPENAT, (uint64_t)dirfd, renamed_path,
                             ABI_O_WRONLY | ABI_O_APPEND, 0u);
    if (appendfd < 0 || call_sys(ABI_NR_LSEEK, (uint64_t)appendfd, 0u, 0u, 0u) != 0) {
        return fail(round, "openat O_APPEND or its lseek setup failed");
    }
    memcpy(kf_uptr(in), appended, sizeof(appended));
    if (call_sys(ABI_NR_WRITE, (uint64_t)appendfd, in, sizeof(appended), 0u) != 1) {
        return fail(round, "O_APPEND write failed");
    }
    memset(kf_uptr(out), 0, sizeof(expected));
    if (call_sys(ABI_NR_PREAD64, (uint64_t)original, out, sizeof(expected), 0u) !=
        (long)sizeof(expected) || memcmp(kf_uptr(out), expected, sizeof(expected)) != 0) {
        return fail(round, "O_APPEND overwrote the file instead of extending its end");
    }
    if (call_sys(ABI_NR_FSTAT, (uint64_t)original, stat_uptr, 0u, 0u) != 0 ||
        st->st_size != 2 || (st->st_mode & 0170000u) != 0100000u) {
        return fail(round, "fstat did not report the regular file's size and type");
    }
    if (call_sys(ABI_NR_NEWFSTATAT, (uint64_t)dirfd, renamed_path, stat_uptr, 0u) != 0 ||
        st->st_size != 2 || (st->st_mode & 0170000u) != 0100000u) {
        return fail(round, "newfstatat relative to dirfd did not report file size and type");
    }
    if (call_sys(ABI_NR_FTRUNCATE, (uint64_t)original, 1u, 0u, 0u) != 0) {
        return fail(round, "ftruncate did not shrink the open file");
    }
    truncated_check_fd = (int)call_sys(ABI_NR_OPENAT, (uint64_t)dirfd, renamed_path, 0u, 0u);
    memset(kf_uptr(out), 0, sizeof(expected));
    if (truncated_check_fd < 0 ||
        call_sys(ABI_NR_PREAD64, (uint64_t)truncated_check_fd, out, sizeof(expected), 0u) != 1 ||
        ((uint8_t *)kf_uptr(out))[0] != expected[0] ||
        call_sys(ABI_NR_FSTAT, (uint64_t)truncated_check_fd, stat_uptr, 0u, 0u) != 0 ||
        st->st_size != 1) {
        return fail(round, "ftruncate did not change the filesystem-visible size");
    }
    if (call_sys(ABI_NR_CLOSE, (uint64_t)truncated_check_fd, 0u, 0u, 0u) != 0) {
        return fail(round, "closing ftruncate verification descriptor failed");
    }

    if (call_sys(ABI_NR_UNLINKAT, (uint64_t)dirfd, renamed_path, 0u, 0u) != 0 ||
        call_sys(ABI_NR_UNLINKAT, (uint64_t)dirfd, other_path, 0u, 0u) != 0 ||
        call_sys(ABI_NR_UNLINKAT, (uint64_t)dirfd, hardlink_path, 0u, 0u) != 0 ||
        call_sys(ABI_NR_UNLINKAT, (uint64_t)dirfd, symlink_path, 0u, 0u) != 0) {
        return fail(round, "unlinkat relative to dirfd failed");
    }
    if (call_sys(ABI_NR_CLOSE, (uint64_t)appendfd, 0u, 0u, 0u) != 0 ||
        call_sys(ABI_NR_CLOSE, (uint64_t)truncfd, 0u, 0u, 0u) != 0 ||
        call_sys(ABI_NR_CLOSE, (uint64_t)original, 0u, 0u, 0u) != 0 ||
        call_sys(ABI_NR_CLOSE, (uint64_t)otherfd, 0u, 0u, 0u) != 0 ||
        call_sys(ABI_NR_CLOSE, (uint64_t)dirfd, 0u, 0u, 0u) != 0) {
        return fail(round, "closing path-model descriptors failed");
    }
    {
        uint64_t root_path = kf_ualloc(sizeof("/"));
        if (!root_path) return fail(round, "root path allocation failed");
        memcpy(kf_uptr(root_path), "/", sizeof("/"));
        if (call_sys(ABI_NR_CHDIR, root_path, 0u, 0u, 0u) != 0 ||
            call_sys(ABI_NR_GETCWD, cwd_uptr, 64u, 0u, 0u) != 2 ||
            strcmp((const char *)kf_uptr(cwd_uptr), "/") != 0) {
            return fail(round, "chdir back to root did not reset getcwd");
        }
    }
    if (call_sys(ABI_NR_UNLINKAT, ABI_AT_FDCWD, dir_path, ABI_AT_REMOVEDIR, 0u) != 0) {
        return fail(round, "unlinkat(AT_REMOVEDIR) did not remove the model directory");
    }
    return 0;
}

/* A second directory level exercises path walking through nested dirfds. */
static int test_nested_path_model(unsigned round) {
    static const uint8_t bytes[] = {'T', 'R', 'E', 'E'};
    uint64_t root = kf_ualloc(sizeof("/tmp/abi-tree-model"));
    uint64_t sub = kf_ualloc(sizeof("sub"));
    uint64_t leaf = kf_ualloc(sizeof("leaf"));
    uint64_t data = kf_ualloc(sizeof(bytes));
    uint64_t dents = kf_ualloc(512u);
    uint64_t stat_uptr = kf_ualloc(sizeof(linux_stat_t));
    linux_stat_t *st = (linux_stat_t *)kf_uptr(stat_uptr);
    int rootfd, subfd, filefd;
    long nread;
    uint64_t at;
    int saw_leaf = 0;

    if (!root || !sub || !leaf || !data || !dents || !stat_uptr) {
        return fail(round, "nested path model allocation failed");
    }
    memcpy(kf_uptr(root), "/tmp/abi-tree-model", sizeof("/tmp/abi-tree-model"));
    memcpy(kf_uptr(sub), "sub", sizeof("sub"));
    memcpy(kf_uptr(leaf), "leaf", sizeof("leaf"));
    memcpy(kf_uptr(data), bytes, sizeof(bytes));
    if (call_sys(ABI_NR_MKDIRAT, ABI_AT_FDCWD, root, 0700u, 0u) != 0) {
        return fail(round, "mkdirat did not create the model root");
    }
    rootfd = (int)call_sys(ABI_NR_OPENAT, ABI_AT_FDCWD, root, ABI_O_DIRECTORY, 0u);
    if (rootfd < 0 || call_sys(ABI_NR_MKDIRAT, (uint64_t)rootfd, sub, 0700u, 0u) != 0) {
        return fail(round, "mkdirat through a directory descriptor failed");
    }
    subfd = (int)call_sys(ABI_NR_OPENAT, (uint64_t)rootfd, sub, ABI_O_DIRECTORY, 0u);
    filefd = subfd < 0 ? -1 : (int)call_sys(ABI_NR_OPENAT, (uint64_t)subfd, leaf,
                                          ABI_O_CREAT | ABI_O_EXCL | ABI_O_RDWR, 0600u);
    if (subfd < 0 || filefd < 0 ||
        call_sys(ABI_NR_WRITE, (uint64_t)filefd, data, sizeof(bytes), 0u) != (long)sizeof(bytes) ||
        call_sys(ABI_NR_NEWFSTATAT, (uint64_t)subfd, leaf, stat_uptr, 0u) != 0 ||
        st->st_size != (int64_t)sizeof(bytes) || (st->st_mode & 0170000u) != 0100000u) {
        return fail(round, "nested openat or newfstatat did not reach the leaf file");
    }
    nread = call_sys(ABI_NR_GETDENTS64, (uint64_t)subfd, dents, 512u, 0u);
    if (nread <= 0) {
        return fail(round, "nested getdents64 returned no entries");
    }
    for (at = 0; at < (uint64_t)nread;) {
        linux_dirent64_t *de = (linux_dirent64_t *)((uint8_t *)kf_uptr(dents) + at);
        if (de->d_reclen < sizeof(linux_dirent64_t) || at + de->d_reclen > (uint64_t)nread) {
            return fail(round, "nested getdents64 returned an invalid record boundary");
        }
        if (strcmp(de->d_name, "leaf") == 0 && de->d_type == 8u) {
            saw_leaf = 1;
        }
        at += de->d_reclen;
    }
    if (!saw_leaf ||
        call_sys(ABI_NR_UNLINKAT, (uint64_t)rootfd, sub, ABI_AT_REMOVEDIR, 0u) != -VIBEOS_ENOTEMPTY ||
        call_sys(ABI_NR_CLOSE, (uint64_t)filefd, 0u, 0u, 0u) != 0 ||
        call_sys(ABI_NR_UNLINKAT, (uint64_t)subfd, leaf, 0u, 0u) != 0 ||
        call_sys(ABI_NR_CLOSE, (uint64_t)subfd, 0u, 0u, 0u) != 0 ||
        call_sys(ABI_NR_UNLINKAT, (uint64_t)rootfd, sub, ABI_AT_REMOVEDIR, 0u) != 0 ||
        call_sys(ABI_NR_CLOSE, (uint64_t)rootfd, 0u, 0u, 0u) != 0 ||
        call_sys(ABI_NR_UNLINKAT, ABI_AT_FDCWD, root, ABI_AT_REMOVEDIR, 0u) != 0) {
        return fail(round, "nested directory enumeration, non-empty refusal or cleanup differs");
    }
    return 0;
}

/* The memory oracle is a sorted interval set, independent of the fake page tables. */
#define ABI_VM_MAX_INTERVALS 16u
typedef struct {
    uint64_t start;
    uint64_t end;
    uint32_t prot;
} abi_vm_interval_t;

static int vm_model_replace(abi_vm_interval_t *v, uint32_t *count,
                            uint64_t start, uint64_t end, uint32_t prot,
                            int replace) {
    abi_vm_interval_t next[ABI_VM_MAX_INTERVALS];
    uint32_t n = 0, i, j;
    for (i = 0; i < *count; i++) {
        abi_vm_interval_t old = v[i];
        if (old.end <= start || old.start >= end) {
            if (n == ABI_VM_MAX_INTERVALS) return -1;
            next[n++] = old;
            continue;
        }
        if (!replace) return -1;
        if (old.start < start) {
            if (n == ABI_VM_MAX_INTERVALS) return -1;
            next[n++] = (abi_vm_interval_t){old.start, start, old.prot};
        }
        if (old.end > end) {
            if (n == ABI_VM_MAX_INTERVALS) return -1;
            next[n++] = (abi_vm_interval_t){end, old.end, old.prot};
        }
    }
    if (n == ABI_VM_MAX_INTERVALS) return -1;
    next[n++] = (abi_vm_interval_t){start, end, prot};
    for (i = 0; i < n; i++) for (j = i + 1u; j < n; j++) {
        if (next[j].start < next[i].start) {
            abi_vm_interval_t t = next[i]; next[i] = next[j]; next[j] = t;
        }
    }
    memcpy(v, next, n * sizeof(next[0]));
    *count = n;
    return 0;
}

static int vm_model_protect(abi_vm_interval_t *v, uint32_t *count,
                            uint64_t start, uint64_t end, uint32_t prot) {
    abi_vm_interval_t next[ABI_VM_MAX_INTERVALS];
    uint64_t covered = start;
    uint32_t n = 0, i, j;
    for (i = 0; i < *count; i++) {
        abi_vm_interval_t old = v[i];
        uint64_t lo, hi;
        if (old.end <= start || old.start >= end) {
            if (n == ABI_VM_MAX_INTERVALS) return -1;
            next[n++] = old;
            continue;
        }
        lo = old.start > start ? old.start : start;
        hi = old.end < end ? old.end : end;
        if (lo > covered) return -1;
        if (old.start < lo) {
            if (n == ABI_VM_MAX_INTERVALS) return -1;
            next[n++] = (abi_vm_interval_t){old.start, lo, old.prot};
        }
        if (n == ABI_VM_MAX_INTERVALS) return -1;
        next[n++] = (abi_vm_interval_t){lo, hi, prot};
        if (old.end > hi) {
            if (n == ABI_VM_MAX_INTERVALS) return -1;
            next[n++] = (abi_vm_interval_t){hi, old.end, old.prot};
        }
        if (hi > covered) covered = hi;
    }
    if (covered != end) return -1;
    for (i = 0; i < n; i++) for (j = i + 1u; j < n; j++) {
        if (next[j].start < next[i].start) {
            abi_vm_interval_t t = next[i]; next[i] = next[j]; next[j] = t;
        }
    }
    memcpy(v, next, n * sizeof(next[0]));
    *count = n;
    return 0;
}

static int vm_model_unmap(abi_vm_interval_t *v, uint32_t *count,
                          uint64_t start, uint64_t end) {
    abi_vm_interval_t next[ABI_VM_MAX_INTERVALS];
    uint64_t covered = start;
    uint32_t n = 0, i;
    for (i = 0; i < *count; i++) {
        abi_vm_interval_t old = v[i];
        if (old.end <= start || old.start >= end) {
            if (n == ABI_VM_MAX_INTERVALS) return -1;
            next[n++] = old;
            continue;
        }
        if (old.start > covered) return -1;
        if (old.start < start) {
            if (n == ABI_VM_MAX_INTERVALS) return -1;
            next[n++] = (abi_vm_interval_t){old.start, start, old.prot};
        }
        if (old.end > end) {
            if (n == ABI_VM_MAX_INTERVALS) return -1;
            next[n++] = (abi_vm_interval_t){end, old.end, old.prot};
        }
        if (old.end > covered) covered = old.end;
    }
    if (covered < end) return -1;
    memcpy(v, next, n * sizeof(next[0]));
    *count = n;
    return 0;
}

static int vm_model_access(const abi_vm_interval_t *v, uint32_t count,
                           uint64_t address, uint32_t need) {
    uint32_t i;
    for (i = 0; i < count; i++) {
        if (address >= v[i].start && address < v[i].end)
            return (v[i].prot & need) == need;
    }
    return 0;
}

/* Exercise real page-table changes through mmap, mprotect and munmap. */
static int test_memory_model(unsigned round) {
    const uint64_t base = 0x30000000ull;
    const uint64_t page = 4096u;
    const uint64_t map_flags = 0x100022u; /* MAP_FIXED_NOREPLACE | PRIVATE | ANONYMOUS */
    const uint64_t fixed_flags = 0x32u;    /* MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS */
    const uint64_t anonymous_only = 0x20u; /* MAP_ANONYMOUS without PRIVATE/SHARED */
    const uint64_t rw = 3u;               /* PROT_READ | PROT_WRITE */
    const uint8_t first[] = {0x11u, 0x22u, 0x33u, 0x44u};
    const uint8_t zero[sizeof(first)] = {0};
    const uint8_t second[] = {0xA1u, 0xB2u, 0xC3u, 0xD4u};
    const uint8_t file_data[] = {0x51u, 0x62u, 0x73u, 0x84u};
    const uint8_t private_data[] = {0x91u, 0x92u, 0x93u, 0x94u};
    const uint8_t shared_data[] = {0xA1u, 0xA2u, 0xA3u, 0xA4u};
    const uint64_t private_file_flags = 0x100002u; /* MAP_FIXED_NOREPLACE | MAP_PRIVATE */
    const uint64_t shared_file_flags = 0x100001u;  /* MAP_FIXED_NOREPLACE | MAP_SHARED */
    uint64_t file_path = kf_ualloc(sizeof("/tmp/abi-map-model"));
    uint64_t filebuf = kf_ualloc(16u);
    uint8_t got[sizeof(first)];
    abi_vm_interval_t vm[ABI_VM_MAX_INTERVALS];
    uint32_t vm_count = 0;
    long fd;

    if (!file_path || !filebuf) {
        return fail(round, "file-mapping test allocation failed");
    }
    memcpy(kf_uptr(file_path), "/tmp/abi-map-model", sizeof("/tmp/abi-map-model"));
    memcpy(kf_uptr(filebuf), file_data, sizeof(file_data));

    if (call_sys6(ABI_NR_MMAP, base, page * 2u, rw, map_flags, ~0ull, 0u) != (long)base ||
        vm_model_replace(vm, &vm_count, base, base + page * 2u, rw, 0) != 0 ||
        !vm_model_access(vm, vm_count, base, 3u) ||
        !vm_model_access(vm, vm_count, base + page, 3u)) {
        return fail(round, "mmap did not reserve the requested two-page interval");
    }
    if (call_sys6(ABI_NR_MMAP, base, page, rw, map_flags, ~0ull, 0u) != -VIBEOS_EEXIST ||
        call_sys(ABI_NR_MUNMAP, base + 1u, page, 0u, 0u) != -VIBEOS_EINVAL ||
        call_sys(ABI_NR_MPROTECT, base + 1u, page, 1u, 0u) != -VIBEOS_EINVAL) {
        return fail(round, "mmap overlap or unaligned range validation differs");
    }
    if (kf_poke(base, first, sizeof(first)) != 0 ||
        kf_poke(base + page, second, sizeof(second)) != 0 ||
        kf_peek(base, got, sizeof(got)) != 0 || memcmp(got, first, sizeof(first)) != 0 ||
        kf_peek(base + page, got, sizeof(got)) != 0 || memcmp(got, second, sizeof(second)) != 0) {
        return fail(round, "anonymous mmap pages did not preserve independent bytes");
    }
    /* MAP_FIXED replaces an existing mapping at the exact address. The new
     * anonymous page must be zero-filled, not retain the old page's bytes. */
    if (call_sys6(ABI_NR_MMAP, base, page, rw, fixed_flags, ~0ull, 0u) != (long)base ||
        vm_model_replace(vm, &vm_count, base, base + page, rw, 1) != 0 ||
        !vm_model_access(vm, vm_count, base, 3u) ||
        !vm_model_access(vm, vm_count, base + page, 3u) ||
        kf_peek(base, got, sizeof(got)) != 0 || memcmp(got, zero, sizeof(zero)) != 0 ||
        kf_poke(base, first, sizeof(first)) != 0) {
        return fail(round, "MAP_FIXED did not replace the existing page with fresh anonymous memory");
    }
    if (call_sys6(ABI_NR_MMAP, base + 1u, page, rw, fixed_flags, ~0ull, 0u) != -VIBEOS_EINVAL ||
        call_sys6(ABI_NR_MMAP, 0u, page, rw, anonymous_only, ~0ull, 0u) != -VIBEOS_EINVAL) {
        return fail(round, "mmap accepted an unaligned MAP_FIXED address or missing mapping type");
    }
    if (call_sys(ABI_NR_MPROTECT, base + page, page, 1u, 0u) != 0 ||
        vm_model_protect(vm, &vm_count, base + page, base + page * 2u, 1u) != 0 ||
        !vm_model_access(vm, vm_count, base + page, 1u) ||
        vm_model_access(vm, vm_count, base + page, 2u) ||
        kf_poke(base + page, first, sizeof(first)) == 0 ||
        kf_peek(base + page, got, sizeof(got)) != 0 || memcmp(got, second, sizeof(second)) != 0) {
        return fail(round, "mprotect(PROT_READ) did not revoke writes while preserving reads");
    }
    if (call_sys(ABI_NR_MPROTECT, base + page, page, rw, 0u) != 0 ||
        vm_model_protect(vm, &vm_count, base + page, base + page * 2u, rw) != 0 ||
        !vm_model_access(vm, vm_count, base + page, 3u) ||
        kf_poke(base + page, first, sizeof(first)) != 0 ||
        kf_peek(base + page, got, sizeof(got)) != 0 || memcmp(got, first, sizeof(first)) != 0) {
        return fail(round, "mprotect did not restore write access");
    }
    if (call_sys(ABI_NR_MUNMAP, base + page, page, 0u, 0u) != 0 ||
        vm_model_unmap(vm, &vm_count, base + page, base + page * 2u) != 0 ||
        vm_model_access(vm, vm_count, base + page, 1u) ||
        kf_peek(base + page, got, sizeof(got)) == 0 ||
        kf_peek(base, got, sizeof(got)) != 0 || memcmp(got, first, sizeof(first)) != 0) {
        return fail(round, "munmap did not remove only the selected page");
    }
    if (call_sys(ABI_NR_MUNMAP, base, page, 0u, 0u) != 0 ||
        vm_model_unmap(vm, &vm_count, base, base + page) != 0 ||
        vm_count != 0u || vm_model_access(vm, vm_count, base, 1u) ||
        kf_peek(base, got, sizeof(got)) == 0 ||
        call_sys6(ABI_NR_MMAP, 0u, 0u, rw, map_flags, ~0ull, 0u) != -VIBEOS_EINVAL) {
        return fail(round, "munmap did not release the remaining page or zero-length mmap was accepted");
    }

    /* A private file mapping owns a copy; a shared tmpfs mapping aliases the
     * file's page in both directions. */
    fd = call_sys(ABI_NR_OPENAT, ABI_AT_FDCWD, file_path,
                  ABI_O_CREAT | ABI_O_EXCL | ABI_O_RDWR, 0600u);
    if (fd < 0 || call_sys(ABI_NR_WRITE, (uint64_t)fd, filebuf, sizeof(file_data), 0u) !=
        (long)sizeof(file_data) ||
        call_sys6(ABI_NR_MMAP, base, page, rw, private_file_flags, (uint64_t)fd, 0u) !=
        (long)base ||
        vm_model_replace(vm, &vm_count, base, base + page, rw, 0) != 0 ||
        !vm_model_access(vm, vm_count, base, 1u) ||
        kf_peek(base, got, sizeof(file_data)) != 0 ||
        memcmp(got, file_data, sizeof(file_data)) != 0 ||
        kf_poke(base, private_data, sizeof(private_data)) != 0 ||
        call_sys(ABI_NR_PREAD64, (uint64_t)fd, filebuf, sizeof(file_data), 0u) !=
        (long)sizeof(file_data) ||
        memcmp(kf_uptr(filebuf), file_data, sizeof(file_data)) != 0 ||
        call_sys(ABI_NR_MUNMAP, base, page, 0u, 0u) != 0 ||
        vm_model_unmap(vm, &vm_count, base, base + page) != 0) {
        return fail(round, "MAP_PRIVATE did not copy file bytes or isolate private writes");
    }
    if (call_sys6(ABI_NR_MMAP, base, page, rw, shared_file_flags, (uint64_t)fd, 0u) !=
        (long)base ||
        vm_model_replace(vm, &vm_count, base, base + page, rw, 0) != 0 ||
        !vm_model_access(vm, vm_count, base, 3u) ||
        kf_poke(base, shared_data, sizeof(shared_data)) != 0 ||
        call_sys(ABI_NR_PREAD64, (uint64_t)fd, filebuf, sizeof(file_data), 0u) !=
        (long)sizeof(file_data) ||
        memcmp(kf_uptr(filebuf), shared_data, sizeof(shared_data)) != 0) {
        return fail(round, "MAP_SHARED writes were not visible through file reads");
    }
    memcpy(kf_uptr(filebuf), file_data, sizeof(file_data));
    if (call_sys(ABI_NR_PWRITE64, (uint64_t)fd, filebuf, sizeof(file_data), 0u) !=
        (long)sizeof(file_data) ||
        kf_peek(base, got, sizeof(file_data)) != 0 ||
        memcmp(got, file_data, sizeof(file_data)) != 0 ||
        call_sys(ABI_NR_MUNMAP, base, page, 0u, 0u) != 0 ||
        vm_model_unmap(vm, &vm_count, base, base + page) != 0 ||
        call_sys(ABI_NR_CLOSE, (uint64_t)fd, 0u, 0u, 0u) != 0 ||
        call_sys(ABI_NR_UNLINKAT, ABI_AT_FDCWD, file_path, 0u, 0u) != 0) {
        return fail(round, "MAP_SHARED file writes were not visible through the mapping or cleanup failed");
    }
    return 0;
}

/* fork copies descriptor flags but shares the underlying descriptions. */
static int test_fork_model(unsigned round) {
    const int parent = ks_current();
    const uint64_t cow_base = 0x31000000ull;
    const uint64_t cow_flags = 0x100022u; /* MAP_FIXED_NOREPLACE | PRIVATE | ANONYMOUS */
    const uint8_t parent_bytes[] = {0x31u, 0x32u, 0x33u, 0x34u};
    const uint8_t child_bytes[] = {0x41u, 0x42u, 0x43u, 0x44u};
    uint8_t cow_got[sizeof(parent_bytes)];
    uint64_t pipe_uptr = kf_ualloc(2u * sizeof(int));
    int *pipefd = (int *)kf_uptr(pipe_uptr);
    int child = -1;
    uint32_t i;
    long pid;

    if (!pipe_uptr ||
        call_sys6(ABI_NR_MMAP, cow_base, 4096u, 3u, cow_flags, ~0ull, 0u) != (long)cow_base ||
        kf_poke(cow_base, parent_bytes, sizeof(parent_bytes)) != 0 ||
        call_sys(ABI_NR_LSEEK, 3u, 0u, 0u, 0u) != 0 ||
        call_sys(ABI_NR_FCNTL, 3u, ABI_F_SETFD, ABI_FD_CLOEXEC, 0u) != 0 ||
        call_sys(ABI_NR_PIPE2, pipe_uptr, ABI_O_NONBLOCK, 0u, 0u) != 0) {
        return fail(round, "fork descriptor setup failed");
    }
    g_desc[g_fd[3]].pos = 0u;
    g_fdflags[3] = ABI_FD_CLOEXEC;
    if (pipefd[0] < 0 || pipefd[1] < 0) {
        return fail(round, "pipe2 did not return both descriptors for fork");
    }
    pid = call_sys(ABI_NR_FORK, 0u, 0u, 0u, 0u);
    if (pid <= 0) {
        return fail(round, "fork did not return a child pid");
    }
    for (i = 0; i < ks_slots(); i++) {
        if ((int)i != parent && ks_id((int)i)->pid == (uint32_t)pid) {
            child = (int)i;
            break;
        }
    }
    if (child < 0) {
        return fail(round, "fork child was not published in the task table");
    }

    /* The parent closed its writer; the child's inherited writer keeps the
     * empty nonblocking pipe from reaching EOF. */
    if (call_sys(ABI_NR_CLOSE, (uint64_t)pipefd[1], 0u, 0u, 0u) != 0 ||
        call_sys(ABI_NR_READ, (uint64_t)pipefd[0], kf_ualloc(1u), 1u, 0u) != -VIBEOS_EAGAIN) {
        return fail(round, "fork did not inherit the child's pipe writer");
    }

    kf_set_current(child);
    if (call_sys(ABI_NR_FCNTL, 3u, ABI_F_GETFD, 0u, 0u) != ABI_FD_CLOEXEC ||
        call_sys(ABI_NR_FCNTL, 3u, ABI_F_SETFD, 0u, 0u) != 0) {
        return fail(round, "fork did not copy the descriptor table and its flags");
    }
    if (kf_poke(cow_base, child_bytes, sizeof(child_bytes)) != 0 ||
        kf_peek(cow_base, cow_got, sizeof(cow_got)) != 0 ||
        memcmp(cow_got, child_bytes, sizeof(child_bytes)) != 0) {
        return fail(round, "child write did not resolve its private COW page");
    }
    {
        uint64_t byte = kf_ualloc(1u);
        if (!byte || call_sys(ABI_NR_READ, 3u, byte, 1u, 0u) != 1 ||
            *(uint8_t *)kf_uptr(byte) != g_data[0]) {
            return fail(round, "child did not inherit the parent's open file description");
        }
    }

    kf_set_current(parent);
    if (kf_peek(cow_base, cow_got, sizeof(cow_got)) != 0 ||
        memcmp(cow_got, parent_bytes, sizeof(parent_bytes)) != 0) {
        return fail(round, "child COW write changed the parent's private page");
    }
    if (call_sys(ABI_NR_FCNTL, 3u, ABI_F_GETFD, 0u, 0u) != ABI_FD_CLOEXEC) {
        return fail(round, "child F_SETFD unexpectedly changed the parent's descriptor flags");
    }
    {
        uint64_t byte = kf_ualloc(1u);
        if (!byte || call_sys(ABI_NR_READ, 4u, byte, 1u, 0u) != 1 ||
            *(uint8_t *)kf_uptr(byte) != g_data[1]) {
            return fail(round, "parent and child did not share the open-description offset");
        }
        g_desc[g_fd[3]].pos = 2u;
        if (call_sys(ABI_NR_READ, 3u, byte, 1u, 0u) != 1 ||
            *(uint8_t *)kf_uptr(byte) != g_data[2]) {
            return fail(round, "parent read after fork did not continue the shared offset");
        }
        g_desc[g_fd[3]].pos = 3u;
    }
    if (check_offsets(round) ||
        call_sys(ABI_NR_READ, (uint64_t)pipefd[0], kf_ualloc(1u), 1u, 0u) != -VIBEOS_EAGAIN ||
        call_sys(ABI_NR_WAIT4, (uint64_t)pid, 0u, ABI_WNOHANG, 0u) != 0) {
        return fail(round, "fork child lifetime or WNOHANG behavior differs");
    }
    if (call_sys(ABI_NR_CLOSE, (uint64_t)pipefd[0], 0u, 0u, 0u) != 0 ||
        call_sys(ABI_NR_MUNMAP, cow_base, 4096u, 0u, 0u) != 0) {
        return fail(round, "closing the inherited pipe or unmapping the parent's COW page failed");
    }
    kf_set_current(child);
    if (kf_peek(cow_base, cow_got, sizeof(cow_got)) != 0 ||
        memcmp(cow_got, child_bytes, sizeof(child_bytes)) != 0) {
        return fail(round, "parent munmap removed the child's independent COW page");
    }
    kf_set_current(parent);
    return 0;
}

#ifdef VIBEOS_TEST_NT_ABI
/* The fake kernel entry is personality-neutral: drive the NT dispatcher
 * directly and prove that an NT handle closes the same underlying descriptor
 * without pretending the Linux dispatcher exercised the NT personality. */
static int test_nt_close_model(unsigned round, uint64_t probe_buffer) {
    const uint64_t handle = ((uint64_t)4u + 1u) * 4u;
    kf_outcome_t outcome = KF_PANICKED;
    long got;

    uint64_t stack = kf_ualloc(0x28u + 5u * sizeof(uint64_t));
    uint64_t iosb_u = kf_ualloc(sizeof(vibeos_nt_iosb_t));
    uint64_t *stack_args;
    uint64_t duplicate = 0u;
    vibeos_nt_iosb_t *iosb;
    uint8_t expected, written;
    uint64_t write_at;
    if (!stack || !iosb_u) return fail(round, "NT I/O stack allocation");
    stack_args = (uint64_t *)kf_uptr(stack + 0x28u);
    iosb = (vibeos_nt_iosb_t *)kf_uptr(iosb_u);
    vibeos_nt_abi_init();

    /* NT synchronous I/O must share the Linux open-description offset and
     * bytes. Read one byte at the current position, then overwrite the next. */
    memset(stack_args, 0, 5u * sizeof(uint64_t));
    stack_args[0] = iosb_u;       /* IoStatusBlock */
    stack_args[1] = probe_buffer; /* Buffer */
    stack_args[2] = 1u;           /* Length */
    kf_next_sp(stack);
    got = kf_call(nt_syscall, VIBEOS_NT_NR_NtReadFile, handle, 0u, 0u, 0u,
                  0u, 0u, &outcome);
    expected = g_data[g_desc[g_fd[4]].pos];
    if (outcome != KF_RETURNED || got != (long)VIBEOS_STATUS_SUCCESS ||
        iosb->status != VIBEOS_STATUS_SUCCESS || iosb->information != 1u ||
        *(uint8_t *)kf_uptr(probe_buffer) != expected) {
        return fail(round, "NT NtReadFile did not read the shared descriptor position");
    }
    g_desc[g_fd[4]].pos++;

    write_at = g_desc[g_fd[4]].pos;
    written = (uint8_t)(expected ^ 0x5Au);
    *(uint8_t *)kf_uptr(probe_buffer) = written;
    kf_next_sp(stack);
    got = kf_call(nt_syscall, VIBEOS_NT_NR_NtWriteFile, handle, 0u, 0u, 0u,
                  0u, 0u, &outcome);
    if (outcome != KF_RETURNED || got != (long)VIBEOS_STATUS_SUCCESS ||
        iosb->status != VIBEOS_STATUS_SUCCESS || iosb->information != 1u) {
        printf("NT_WRITE_DIAG outcome=%u got=%ld iosb_status=%llu iosb_info=%llu\\n",
               (unsigned)outcome, got, (unsigned long long)iosb->status,
               (unsigned long long)iosb->information);
        return fail(round, "NT NtWriteFile did not report a successful one-byte write");
    }
    /* Observe the shared offset through the Linux ABI, not through the
     * independent model's bookkeeping. The NT call must advance it once. */
    if (call_sys(ABI_NR_LSEEK, 4u, 0u, VIBEOS_SEEK_CUR, 0u) != (long)(write_at + 1u)) {
        return fail(round, "NT NtWriteFile did not advance the shared descriptor offset");
    }
    /* Read back through positional I/O: it must expose the byte written by
     * NT without moving the shared offset observed above. */
    *(uint8_t *)kf_uptr(probe_buffer) = (uint8_t)~written;
    if (call_sys(ABI_NR_PREAD64, 4u, probe_buffer, 1u, write_at) != 1 ||
        *(uint8_t *)kf_uptr(probe_buffer) != written ||
        call_sys(ABI_NR_LSEEK, 4u, 0u, VIBEOS_SEEK_CUR, 0u) != (long)(write_at + 1u)) {
        return fail(round, "NT NtWriteFile bytes or positional offset differ from the independent model");
    }
    g_data[write_at] = written;
    g_desc[g_fd[4]].pos = write_at + 1u;

    stack_args[0] = 0u; /* DesiredAccess: SAME_ACCESS supplies the source mask. */
    stack_args[1] = 0u; /* HandleAttributes */
    stack_args[2] = VIBEOS_DUPLICATE_SAME_ACCESS;

    /* Duplicate the Linux descriptor through NT. The resulting handle must
     * name a second reference in the same descriptor table. */
    kf_next_sp(stack);
    got = kf_call(nt_syscall, VIBEOS_NT_NR_NtDuplicateObject,
                  VIBEOS_NT_CURRENT_PROCESS, handle, VIBEOS_NT_CURRENT_PROCESS,
                  probe_buffer, 0u, 0u, &outcome);
    if (outcome != KF_RETURNED || got != (long)VIBEOS_STATUS_SUCCESS) {
        return fail(round, "NT NtDuplicateObject did not duplicate a live handle");
    }
    memcpy(&duplicate, kf_uptr(probe_buffer), sizeof(duplicate));
    if (duplicate != 24u) {
        return fail(round, "NT NtDuplicateObject did not allocate the next descriptor handle");
    }
    outcome = KF_PANICKED;
    got = kf_call(nt_syscall, VIBEOS_NT_NR_NtClose, duplicate,
                  0u, 0u, 0u, 0u, 0u, &outcome);
    if (outcome != KF_RETURNED || got != (long)VIBEOS_STATUS_SUCCESS ||
        call_sys(ABI_NR_READ, 5u, probe_buffer, 1u, 0u) != -VIBEOS_EBADF) {
        return fail(round, "closing the NT duplicate did not release its descriptor");
    }

    got = kf_call(nt_syscall, VIBEOS_NT_NR_NtClose, handle,
                  0u, 0u, 0u, 0u, 0u, &outcome);
    if (outcome != KF_RETURNED || got != (long)VIBEOS_STATUS_SUCCESS) {
        return fail(round, "NT NtClose did not close the duplicated descriptor");
    }
    drop_fd(4u);
    if (call_sys(ABI_NR_READ, 4u, probe_buffer, 1u, 0u) != -VIBEOS_EBADF) {
        return fail(round, "NT NtClose left the Linux descriptor installed");
    }

    outcome = KF_PANICKED;
    got = kf_call(nt_syscall, VIBEOS_NT_NR_NtClose, handle,
                  0u, 0u, 0u, 0u, 0u, &outcome);
    if (outcome != KF_RETURNED || got != (long)VIBEOS_STATUS_INVALID_HANDLE) {
        return fail(round, "NT NtClose did not reject a stale handle");
    }
    outcome = KF_PANICKED;
    got = kf_call(nt_syscall, VIBEOS_NT_NR_NtClose, VIBEOS_NT_CURRENT_PROCESS,
                  0u, 0u, 0u, 0u, 0u, &outcome);
    if (outcome != KF_RETURNED || got != (long)VIBEOS_STATUS_SUCCESS) {
        return fail(round, "NT NtClose did not accept the current-process pseudo-handle");
    }

    /* Restore the alias the Linux randomized model expects after this
     * cross-personality interoperability probe. */
    got = call_sys(ABI_NR_DUP, 3u, 0u, 0u, 0u);
    if (got != 4) return fail(round, "Linux descriptor alias was not reusable after NT close");
    g_fd[4] = g_fd[3];
    g_fdflags[4] = 0u;
    g_desc[g_fd[3]].refs++;
    return 0;
}

#endif

static int run_seed(uint64_t seed, unsigned rounds) {
    uint64_t path, buffer, write_buffer;
    uint32_t i, r;
    int slot;

    g_seed = seed;
    g_rng = seed ? seed : 0x9E3779B97F4A7C15ull;
    printf("ABI_TORTURE_SEED seed=%llu rounds=%u\n",
           (unsigned long long)g_seed, rounds);
    kf_reset();
    vibeos_linux_abi_init();
    slot = kf_spawn(42u, 42u);
    if (slot < 0) return fail(0, "fake task creation");
    kf_set_current(slot);
    memset(g_fd, 0xFF, sizeof(g_fd));
    memset(g_fdflags, 0, sizeof(g_fdflags));
    g_fd[0] = g_fd[1] = g_fd[2] = -2; /* standard descriptors are occupied */
    memset(g_desc, 0, sizeof(g_desc));
    g_size = INITIAL_FILE_SIZE;
    for (i = 0; i < g_size; i++) g_data[i] = (uint8_t)rnd64();
    kf_fs_add("/model", g_data, g_size, 0);
    path = kf_ualloc(16u);
    buffer = kf_ualloc(MODEL_FILE_CAP);
    write_buffer = kf_ualloc(16u);
    if (!path || !buffer || !write_buffer) return fail(0, "fake user memory allocation");
    memcpy(kf_uptr(path), "model", 6u);
    if (open_model(path, 0u)) return 1;
    if (test_pipe_model(0u)) return 1;

    /* A deterministic shared-offset probe: duplicating at a non-zero offset
     * must not reset it or create a private position for the new descriptor. */
    if (call_sys(ABI_NR_LSEEK, 3u, 5u, 0u, 0u) != 5) {
        return fail(0, "initial lseek did not set the shared position");
    }
    g_desc[g_fd[3]].pos = 5u;
    {
        long duplicated = call_sys(ABI_NR_DUP, 3u, 0u, 0u, 0u);
        if (duplicated != 4) return fail(0, "initial dup did not choose descriptor four");
        g_fd[4] = g_fd[3];
        g_desc[g_fd[3]].refs++;
        if (check_offsets(0u)) return 1;
    }
    {
        long got = call_sys(ABI_NR_READ, 4u, buffer, 1u, 0u);
        if (got != 1 || *(uint8_t *)kf_uptr(buffer) != g_data[5]) {
            return fail(0, "duplicate did not read from the shared non-zero offset");
        }
        g_desc[g_fd[3]].pos = 6u;
        if (check_offsets(0u)) return 1;
    }
    if (test_descriptor_semantics(0u)) return 1;
    if (test_fd_limit(0u)) return 1;
    if (test_vector_io(0u)) return 1;
    if (test_path_model(0u)) return 1;
    if (test_nested_path_model(0u)) return 1;
    if (test_memory_model(0u)) return 1;
    if (test_fork_model(0u)) return 1;
#ifdef VIBEOS_TEST_NT_ABI
    if (test_nt_close_model(0u, write_buffer)) return 1;
#endif
    /* The original model resumes with only its tracked descriptors 3 and 4. */

    for (r = 0; r < rounds; r++) {
        uint64_t x = rnd64();
        int fd = choose_live_fd();
        uint32_t op = (uint32_t)(x % 8u);

        if (fd < 0 || op == 0u) {
            if (open_model(path, r)) return 1;
        } else if (op == 1u) {
            int newfd = alloc_fd(3u);
            int d = g_fd[fd];
            if (newfd >= 0) {
                long got = call_sys(ABI_NR_DUP, (uint64_t)fd, 0u, 0u, 0u);
                if (got != newfd) return fail(r, "dup did not choose the lowest free descriptor");
                g_fd[newfd] = (int16_t)d;
                g_desc[d].refs++;
            }
        } else if (op == 2u) {
            uint32_t target = 3u + (uint32_t)(rnd64() % (MODEL_FDS - 3u));
            int old_d = g_fd[fd];
            long got = call_sys(ABI_NR_DUP2, (uint64_t)fd, target, 0u, 0u);
            if ((uint32_t)fd == target) {
                if (got != (long)target) return fail(r, "dup2(old, old) did not return the same descriptor");
            } else {
                if (got != (long)target) return fail(r, "dup2 did not install the requested descriptor");
                if (g_fd[target] >= 0) drop_fd(target);
                g_fd[target] = (int16_t)old_d;
                g_desc[old_d].refs++;
            }
        } else if (op == 3u) {
            if (call_sys(ABI_NR_CLOSE, (uint64_t)fd, 0u, 0u, 0u) != 0) {
                return fail(r, "close refused a live descriptor");
            }
            drop_fd((uint32_t)fd);
        } else if (op == 4u) {
            int d = g_fd[fd];
            uint32_t whence = (uint32_t)((x >> 8) % 3u);
            int64_t off = (int64_t)((rnd64() % 25u)) - 8;
            int64_t base = whence == 0u ? 0
                         : whence == 1u ? (int64_t)g_desc[d].pos
                                        : (int64_t)g_size;
            int64_t want = base + off;
            long got = call_sys(ABI_NR_LSEEK, (uint64_t)fd, (uint64_t)off, whence, 0u);
            if (want < 0) {
                if (got != -VIBEOS_EINVAL) return fail(r, "negative seek did not return EINVAL");
            } else {
                if (got != (long)want) return fail(r, "lseek result differs from independent offset model");
                g_desc[d].pos = (uint64_t)want;
            }
        } else if (op == 5u) {
            int d = g_fd[fd];
            uint32_t count = (uint32_t)(rnd64() % 13u);
            uint32_t want = 0;
            long got;
            if (g_desc[d].pos < g_size) {
                want = g_size - (uint32_t)g_desc[d].pos;
                if (want > count) want = count;
            }
            memset(kf_uptr(buffer), 0xCC, 16u);
            got = call_sys(ABI_NR_READ, (uint64_t)fd, buffer, count, 0u);
            if (got != (long)want ||
                (want && memcmp(kf_uptr(buffer), g_data + g_desc[d].pos, want) != 0)) {
                return fail(r, "read result or bytes differ from independent file model");
            }
            g_desc[d].pos += want;
        } else if (op == 6u) {
            int d = g_fd[fd];
            uint8_t bytes[12];
            uint32_t count = 1u + (uint32_t)(rnd64() % 8u);
            uint32_t want = count;
            long got;
            uint32_t k;
            if (g_desc[d].pos >= MODEL_FILE_CAP) {
                want = 0;
            } else if (want > MODEL_FILE_CAP - g_desc[d].pos) {
                want = MODEL_FILE_CAP - (uint32_t)g_desc[d].pos;
            }
            for (k = 0; k < count; k++) bytes[k] = (uint8_t)rnd64();
            memcpy(kf_uptr(write_buffer), bytes, count);
            got = call_sys(ABI_NR_WRITE, (uint64_t)fd, write_buffer, count, 0u);
            if (want == 0u) {
                if (got != -VIBEOS_ENOSPC) return fail(r, "write past fake volume did not return ENOSPC");
            } else {
                if (got != (long)want) {
                    printf("ABI_WRITE_MISMATCH fd=%d desc=%d pos=%llu count=%u want=%u got=%ld size=%u\n",
                           fd, d, (unsigned long long)g_desc[d].pos, count, want, got, g_size);
                    return fail(r, "write result differs from independent file model");
                }
                if (g_desc[d].pos > g_size) {
                    memset(g_data + g_size, 0, (size_t)(g_desc[d].pos - g_size));
                }
                memcpy(g_data + g_desc[d].pos, bytes, want);
                g_desc[d].pos += want;
                if (g_desc[d].pos > g_size) g_size = (uint32_t)g_desc[d].pos;
            }
        } else {
            uint32_t unused = 3u + (uint32_t)(rnd64() % (MODEL_FDS - 3u));
            if (g_fd[unused] == -1 &&
                call_sys(ABI_NR_READ, unused, buffer, 1u, 0u) != -VIBEOS_EBADF) {
                return fail(r, "read on a free descriptor did not return EBADF");
            }
        }

        if ((r % 97u) == 0u) {
            if (check_offsets(r) || check_file(r, buffer)) return 1;
        }
    }
    if (check_offsets(rounds) || check_file(rounds, buffer)) return 1;
    printf("ABI_TORTURE_OK seed=%llu rounds=%u fds=%d bytes=%u\n",
           (unsigned long long)seed, rounds, live_fd_count(), g_size);
    kf_reset(); /* release fake descriptor tables and backing pages */
    return 0;
}

int main(int argc, char **argv) {
    uint64_t seed = argc > 1 ? strtoull(argv[1], 0, 0) : 1u;
    unsigned rounds = argc > 2 ? (unsigned)strtoul(argv[2], 0, 0) : 10000u;
    return run_seed(seed, rounds);
}
