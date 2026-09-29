#!/usr/bin/env python3
"""Generate docs/abi/syscalls.md: every x86-64 Linux syscall, its state here, its phase.

    python3 scripts/dev/make-syscall-table.py [unistd_64.h]

The numbers come from the host's asm/unistd_64.h; the state from the kernel's own
rows (kernel/abi/linux/*.c), so it is read rather than remembered. The phase of
each missing number, and the gap of each partial one, are the plan's
(docs/abi/phases.md) and live in the tables below until phase A1 replaces this
script with the registry the dispatcher reads.
"""
import glob
import os
import re
import sys

R = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
HDR = sys.argv[1] if len(sys.argv) > 1 else "/usr/include/x86_64-linux-gnu/asm/unistd_64.h"

allnr = []
for line in open(HDR, encoding="utf-8"):
    m = re.match(r"#define __NR_(\w+)\s+(\d+)", line)
    if m:
        allnr.append((int(m.group(2)), m.group(1)))
allnr.sort()

rows = {}
for f in glob.glob(os.path.join(R, "kernel", "abi", "linux", "*.c")):
    for m in re.finditer(r"^\s*X\((\d+),\s*(\w+),", open(f, encoding="utf-8").read(), re.M):
        rows[int(m.group(1))] = (m.group(2), os.path.basename(f), "")

# Known partial implementations, verified in the source on 2026-09-29.
PARTIAL = {
    "open": "no working directory: paths resolve from the root; 4 descriptor slots",
    "openat": "dirfd must be AT_FDCWD; no working directory",
    "getcwd": "answers \"/\": there is one directory",
    "mmap": "anonymous only; MAP_FIXED and file-backed mappings refused",
    "ioctl": "ENOTTY for everything: no terminal device",
    "sendfile": "row answers ENOSYS on purpose; callers fall back to read/write",
    "rseq": "ENOSYS on purpose: the libc takes its fallback",
    "clone": "thread and fork shapes; vfork-like sharing refused",
    "setuid": "one user; accepted without a credential model",
    "setgid": "one user; accepted without a credential model",
    "readv": "files and the console; no socket scatter",
    "writev": "files and the console; no socket gather",
    "prlimit64": "reports limits; setting them is not enforced",
    "futex": "WAIT and WAKE; no requeue, wake_op or PI",
    "uname": "fixed answer",
    "getdents64": "one directory stream per descriptor, FAT names",
}

P = {}


def phase(ph, names, note=""):
    for n in names.split():
        assert n not in P, n
        P[n] = (ph, note)


phase("L1", "stat lstat access pread64 pwrite64 fcntl flock fsync fdatasync truncate ftruncate "
      "getdents chdir fchdir rename rmdir creat link symlink readlink chmod fchmod chown fchown "
      "lchown umask utime utimes mknod statfs fstatfs readahead mkdirat mknodat fchownat "
      "futimesat unlinkat renameat renameat2 linkat symlinkat fchmodat fchmodat2 faccessat "
      "faccessat2 utimensat fallocate dup3 preadv pwritev preadv2 pwritev2 statx "
      "copy_file_range close_range openat2 sync syncfs sync_file_range fadvise64")
phase("L1", "setxattr lsetxattr fsetxattr getxattr lgetxattr fgetxattr listxattr llistxattr "
      "flistxattr removexattr lremovexattr fremovexattr",
      "ENOTSUP from the filesystem until one stores attributes")
phase("L2", "pause nanosleep getitimer alarm setitimer getrlimit setrlimit getrusage times "
      "gettimeofday clock_getres clock_nanosleep timer_create timer_settime timer_gettime "
      "timer_getoverrun timer_delete waitid getpgid rt_sigpending rt_sigtimedwait "
      "rt_sigqueueinfo rt_sigsuspend rt_tgsigqueueinfo sigaltstack restart_syscall "
      "execveat clone3 pidfd_open pidfd_send_signal getpriority setpriority personality")
phase("L2", "setreuid setregid getgroups setgroups setresuid getresuid setresgid getresgid "
      "setfsuid setfsgid", "the credential model arrives here")
phase("L2", "clock_settime settimeofday adjtimex clock_adjtime", "EPERM without privilege; real only once there is one")
phase("L3", "mremap msync mincore madvise mlock munlock mlockall munlockall mlock2 memfd_create",
      "with file-backed and shared mappings (the other half of mmap)")
phase("L4", "poll select pselect6 ppoll epoll_create epoll_create1 epoll_ctl epoll_wait "
      "epoll_pwait epoll_pwait2 eventfd eventfd2 timerfd_create timerfd_settime "
      "timerfd_gettime signalfd signalfd4 inotify_init inotify_init1 inotify_add_watch "
      "inotify_rm_watch")
phase("L5", "sendmsg recvmsg shutdown getsockname getpeername socketpair setsockopt getsockopt "
      "accept4 recvmmsg sendmmsg", "and AF_UNIX sockets")
phase("L6", "sched_setparam sched_getparam sched_setscheduler sched_getscheduler "
      "sched_get_priority_max sched_get_priority_min sched_rr_get_interval sched_setaffinity "
      "sched_getaffinity sched_setattr sched_getattr get_robust_list membarrier getcpu "
      "futex_waitv futex_wake futex_wait futex_requeue ioprio_set ioprio_get")
phase("L7", "shmget shmat shmctl shmdt semget semop semctl semtimedop msgget msgsnd msgrcv "
      "msgctl mq_open mq_unlink mq_timedsend mq_timedreceive mq_notify mq_getsetattr "
      "splice tee vmsplice")
phase("L8", "syslog sethostname setdomainname mount umount2 pivot_root chroot reboot swapon "
      "swapoff vhangup")
phase("L9", "capget capset seccomp landlock_create_ruleset landlock_add_rule "
      "landlock_restrict_self", "the security goals of docs/vision.md")
phase("D", "mbind set_mempolicy get_mempolicy migrate_pages move_pages set_mempolicy_home_node",
      "NUMA: one node here")
phase("D", "io_setup io_destroy io_getevents io_submit io_cancel io_pgetevents",
      "legacy AIO: ENOSYS, and libcs fall back to threads")
phase("D", "io_uring_setup io_uring_enter io_uring_register", "a second I/O model; after L4")
phase("D", "unshare setns", "namespaces: after the credential model")
phase("D", "open_tree move_mount fsopen fsconfig fsmount fspick mount_setattr statmount listmount",
      "the new mount API: after L8")
phase("D", "fanotify_init fanotify_mark name_to_handle_at open_by_handle_at quotactl quotactl_fd "
      "acct cachestat process_madvise process_mrelease process_vm_readv process_vm_writev "
      "kcmp pidfd_getfd ptrace add_key request_key keyctl lsm_get_self_attr "
      "lsm_set_self_attr lsm_list_modules pkey_mprotect pkey_alloc pkey_free",
      "no program in the corpus asks; revisit when one does")
phase("R", "uselib create_module get_kernel_syms query_module nfsservctl getpmsg putpmsg "
      "afs_syscall tuxcall security vserver epoll_ctl_old epoll_wait_old _sysctl "
      "lookup_dcookie ustat sysfs remap_file_pages", "removed or never implemented by Linux itself")
phase("R", "init_module finit_module delete_module kexec_load kexec_file_load bpf "
      "perf_event_open userfaultfd iopl ioperm modify_ldt set_thread_area get_thread_area "
      "map_shadow_stack memfd_secret",
      "EPERM or ENOSYS by decision: loading code into the kernel, raw ports, or a facility this kernel does not have")

TITLES = {
    "L1": "files and paths", "L2": "processes, credentials, time", "L3": "memory",
    "L4": "event loops", "L5": "sockets", "L6": "threads and scheduling", "L7": "IPC",
    "L8": "system administration", "L9": "security", "D": "deferred", "R": "refused by decision",
}

out = []
counts = {}
missing_phase = []
for n, name in allnr:
    if n in rows:
        rname, rfile, handler = rows[n]
        assert rname == name, (n, rname, name)
        if name in PARTIAL:
            state = "partial"
            note = PARTIAL[name]
        else:
            state = "done"
            note = ""
        FINISH = {"open": "L1", "openat": "L1", "getcwd": "L1", "getdents64": "L1",
                  "sendfile": "L1", "ioctl": "L1", "mmap": "L3", "futex": "L6",
                  "clone": "L6", "rseq": "R", "setuid": "L2", "setgid": "L2",
                  "prlimit64": "L2", "readv": "L5", "writev": "L5", "uname": "L8"}
        ph = "done" if state == "done" else "A2 → " + FINISH[name]
        where = "`%s`" % rfile
    else:
        if name not in P:
            missing_phase.append(name)
            continue
        state = "missing"
        ph, note = P[name]
        where = ""
    counts[(state, ph)] = counts.get((state, ph), 0) + 1
    out.append((n, name, state, ph, where, note))

assert not missing_phase, missing_phase
# Rows for numbers the header does not have (VibeOS's own, >= 1000).
for n, (rname, rfile, handler) in sorted(rows.items()):
    if n >= 1000:
        out.append((n, rname, "done", "native", "`%s`" % rfile, "VibeOS's own, outside the Linux number space"))

by_state = {}
for (st, ph), c in counts.items():
    by_state[st] = by_state.get(st, 0) + c
by_phase = {}
for n, name, st, ph, where, note in out:
    if st == "missing":
        by_phase[ph] = by_phase.get(ph, 0) + 1

lines = []
lines.append("# Every Linux x86-64 syscall")
lines.append("")
lines.append("All %d numbers in `asm/unistd_64.h` (Linux 6.x: 0-334 and 424-461), each with its" % len(allnr))
lines.append("state in this kernel on 2026-09-29 and the phase of [phases.md](phases.md) that")
lines.append("owns it. Generated by `scripts/dev/make-syscall-table.py` from the kernel's own")
lines.append("rows (`kernel/abi/linux/*.c`), so the")
lines.append("*state* column is read, not remembered; phase A1 turns this table into the checked")
lines.append("registry the dispatcher reads.")
lines.append("")
lines.append("| State | Count |")
lines.append("| --- | --- |")
for st in ("done", "partial", "missing"):
    lines.append("| %s | %d |" % (st, by_state.get(st, 0)))
lines.append("")
lines.append("Missing, by phase:")
lines.append("")
lines.append("| Phase | Syscalls |")
lines.append("| --- | --- |")
for ph in ("L1", "L2", "L3", "L4", "L5", "L6", "L7", "L8", "L9", "D", "R"):
    lines.append("| %s - %s | %d |" % (ph, TITLES[ph], by_phase.get(ph, 0)))
lines.append("")
lines.append("`partial` rows are refactored in A2 and finished in the phase that owns their")
lines.append("missing half (for example `mmap`'s file-backed mappings in L3). `R` rows are")
lines.append("answered with a fixed errno and counted as *refused*, never as *missing*.")
lines.append("")
lines.append("| Nr | Syscall | State | Phase | Row | Note |")
lines.append("| --- | --- | --- | --- | --- | --- |")
for n, name, st, ph, where, note in out:
    lines.append("| %d | `%s` | %s | %s | %s | %s |" % (n, name, st, ph, where, note))
open(os.path.join(R, "docs", "abi", "syscalls.md"), "w", encoding="utf-8",
     newline="\n").write("\n".join(lines) + "\n")
print("docs/abi/syscalls.md: %d numbers, %s" % (len(allnr), by_state))
