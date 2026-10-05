# Every Linux x86-64 syscall

All 373 Linux x86-64 numbers (0-334 and 424-461), each with the answer this
kernel gives it and the phase of [phases.md](phases.md) that owns it. Generated
by `scripts/dev/make-syscall-table.py` from the registry,
`kernel/abi/linux_syscalls.def` - the same file the kernel reads to answer a
number with no row, and `check-syscall-checks.py` holds every row against. Edit
the registry, not this page; `check.sh` fails while the two differ.

| State | Count | What the kernel answers |
| --- | --- | --- |
| done | 183 | through its row |
| partial | 33 | through its row, with the gap named |
| missing | 73 | ENOSYS, and the boot gate fails naming the number |
| deferred | 50 | ENOSYS, and the boot gate fails naming the number |
| refused | 34 | its errno, counted as refused - expected, never a failure |

Without a row, by phase:

| Phase | Syscalls |
| --- | --- |
| L1 - files and paths | 0 |
| L2 - processes, credentials, time | 0 |
| L3 - memory | 0 |
| L4 - event loops | 4 |
| L5 - sockets | 11 |
| L6 - threads and scheduling | 20 |
| L7 - IPC | 21 |
| L8 - system administration | 11 |
| L9 - security | 6 |
| D - deferred | 50 |
| R - refused | 34 |

A `partial` row names its gap and the phase that closes it. VibeOS's own two
calls, outside the Linux number space, are listed last.

| Nr | Syscall | State | Phase | Errno | Row | Note |
| --- | --- | --- | --- | --- | --- | --- |
| 0 | `read` | done | - | - | `fs.c` |  |
| 1 | `write` | done | - | - | `fs.c` |  |
| 2 | `open` | done | - | - | `fs.c` |  |
| 3 | `close` | done | - | - | `fs.c` |  |
| 4 | `stat` | done | - | - | `fs.c` |  |
| 5 | `fstat` | done | - | - | `fs.c` |  |
| 6 | `lstat` | done | - | - | `fs.c` |  |
| 7 | `poll` | done | - | - | `poll.c` |  |
| 8 | `lseek` | done | - | - | `fs.c` |  |
| 9 | `mmap` | partial | L3 | - | `mm.c` | a shared mapping of a file on a filesystem that keeps no pages (FAT) is refused: ENODEV |
| 10 | `mprotect` | done | - | - | `mm.c` |  |
| 11 | `munmap` | done | - | - | `mm.c` |  |
| 12 | `brk` | done | - | - | `mm.c` |  |
| 13 | `rt_sigaction` | done | - | - | `sig.c` |  |
| 14 | `rt_sigprocmask` | done | - | - | `sig.c` |  |
| 15 | `rt_sigreturn` | done | - | - | `sig.c` |  |
| 16 | `ioctl` | partial | L1 | - | `fs.c` | the console's modes, size and process group, the random devices' entropy count, and the descriptor requests; ISIG, VTIME and the other terminal requests (TCFLSH, TIOCSCTTY, ...) are not honoured |
| 17 | `pread64` | done | - | - | `fs.c` |  |
| 18 | `pwrite64` | done | - | - | `fs.c` |  |
| 19 | `readv` | partial | L5 | - | `fs.c` | files and the console; no socket scatter |
| 20 | `writev` | partial | L5 | - | `fs.c` | files and the console; no socket gather |
| 21 | `access` | done | - | - | `names.c` |  |
| 22 | `pipe` | done | - | - | `fs.c` |  |
| 23 | `select` | done | - | - | `poll.c` |  |
| 24 | `sched_yield` | done | - | - | `proc.c` |  |
| 25 | `mremap` | partial | L3 | - | `mm.c` | only a private anonymous mapping grows; a zero old length and MREMAP_DONTUNMAP are refused |
| 26 | `msync` | done | - | - | `mm.c` |  |
| 27 | `mincore` | done | - | - | `mm.c` |  |
| 28 | `madvise` | partial | L3 | - | `mm.c` | MADV_DONTNEED leaves a private page of a file as the program left it, where Linux shows the file again |
| 29 | `shmget` | missing | L7 | ENOSYS |  |  |
| 30 | `shmat` | missing | L7 | ENOSYS |  |  |
| 31 | `shmctl` | missing | L7 | ENOSYS |  |  |
| 32 | `dup` | done | - | - | `fs.c` |  |
| 33 | `dup2` | done | - | - | `fs.c` |  |
| 34 | `pause` | done | - | - | `sig.c` |  |
| 35 | `nanosleep` | done | - | - | `misc.c` |  |
| 36 | `getitimer` | done | - | - | `timer.c` |  |
| 37 | `alarm` | done | - | - | `timer.c` |  |
| 38 | `setitimer` | done | - | - | `timer.c` |  |
| 39 | `getpid` | done | - | - | `proc.c` |  |
| 40 | `sendfile` | done | - | - | `fs.c` |  |
| 41 | `socket` | done | - | - | `net.c` |  |
| 42 | `connect` | done | - | - | `net.c` |  |
| 43 | `accept` | done | - | - | `net.c` |  |
| 44 | `sendto` | done | - | - | `net.c` |  |
| 45 | `recvfrom` | done | - | - | `net.c` |  |
| 46 | `sendmsg` | missing | L5 | ENOSYS |  | and AF_UNIX sockets |
| 47 | `recvmsg` | missing | L5 | ENOSYS |  | and AF_UNIX sockets |
| 48 | `shutdown` | missing | L5 | ENOSYS |  | and AF_UNIX sockets |
| 49 | `bind` | done | - | - | `net.c` |  |
| 50 | `listen` | done | - | - | `net.c` |  |
| 51 | `getsockname` | missing | L5 | ENOSYS |  | and AF_UNIX sockets |
| 52 | `getpeername` | missing | L5 | ENOSYS |  | and AF_UNIX sockets |
| 53 | `socketpair` | missing | L5 | ENOSYS |  | and AF_UNIX sockets |
| 54 | `setsockopt` | missing | L5 | ENOSYS |  | and AF_UNIX sockets |
| 55 | `getsockopt` | missing | L5 | ENOSYS |  | and AF_UNIX sockets |
| 56 | `clone` | partial | L6 | - | `proc.c` | thread and fork shapes; vfork-like sharing refused |
| 57 | `fork` | done | - | - | `proc.c` |  |
| 58 | `vfork` | done | - | - | `proc.c` |  |
| 59 | `execve` | done | - | - | `proc.c` |  |
| 60 | `exit` | done | - | - | `proc.c` |  |
| 61 | `wait4` | done | - | - | `proc.c` |  |
| 62 | `kill` | done | - | - | `sig.c` |  |
| 63 | `uname` | partial | L8 | - | `misc.c` | fixed answer |
| 64 | `semget` | missing | L7 | ENOSYS |  |  |
| 65 | `semop` | missing | L7 | ENOSYS |  |  |
| 66 | `semctl` | missing | L7 | ENOSYS |  |  |
| 67 | `shmdt` | missing | L7 | ENOSYS |  |  |
| 68 | `msgget` | missing | L7 | ENOSYS |  |  |
| 69 | `msgsnd` | missing | L7 | ENOSYS |  |  |
| 70 | `msgrcv` | missing | L7 | ENOSYS |  |  |
| 71 | `msgctl` | missing | L7 | ENOSYS |  |  |
| 72 | `fcntl` | done | - | - | `fs.c` |  |
| 73 | `flock` | done | - | - | `fs.c` |  |
| 74 | `fsync` | done | - | - | `fs.c` |  |
| 75 | `fdatasync` | done | - | - | `fs.c` |  |
| 76 | `truncate` | done | - | - | `fs.c` |  |
| 77 | `ftruncate` | done | - | - | `fs.c` |  |
| 78 | `getdents` | done | - | - | `fs.c` |  |
| 79 | `getcwd` | done | - | - | `fs.c` |  |
| 80 | `chdir` | done | - | - | `fs.c` |  |
| 81 | `fchdir` | done | - | - | `fs.c` |  |
| 82 | `rename` | done | - | - | `names.c` |  |
| 83 | `mkdir` | done | - | - | `fs.c` |  |
| 84 | `rmdir` | done | - | - | `names.c` |  |
| 85 | `creat` | done | - | - | `fs.c` |  |
| 86 | `link` | done | - | - | `names.c` |  |
| 87 | `unlink` | done | - | - | `fs.c` |  |
| 88 | `symlink` | done | - | - | `names.c` |  |
| 89 | `readlink` | done | - | - | `fs.c` |  |
| 90 | `chmod` | done | - | - | `names.c` |  |
| 91 | `fchmod` | done | - | - | `names.c` |  |
| 92 | `chown` | done | - | - | `names.c` |  |
| 93 | `fchown` | done | - | - | `names.c` |  |
| 94 | `lchown` | done | - | - | `names.c` |  |
| 95 | `umask` | done | - | - | `fs.c` |  |
| 96 | `gettimeofday` | done | - | - | `timer.c` |  |
| 97 | `getrlimit` | done | - | - | `limits.c` |  |
| 98 | `getrusage` | partial | L2 | - | `limits.c` | CPU time only, all of it user time; no RSS, faults or switches |
| 99 | `sysinfo` | done | - | - | `misc.c` |  |
| 100 | `times` | partial | L2 | - | `timer.c` | all CPU time is user time, and a thread that has exited takes its time with it |
| 101 | `ptrace` | deferred | D | ENOSYS |  | no program in the corpus asks; revisit when one does |
| 102 | `getuid` | done | - | - | `proc.c` |  |
| 103 | `syslog` | missing | L8 | ENOSYS |  |  |
| 104 | `getgid` | done | - | - | `proc.c` |  |
| 105 | `setuid` | done | - | - | `proc.c` |  |
| 106 | `setgid` | done | - | - | `proc.c` |  |
| 107 | `geteuid` | done | - | - | `proc.c` |  |
| 108 | `getegid` | done | - | - | `proc.c` |  |
| 109 | `setpgid` | done | - | - | `proc.c` |  |
| 110 | `getppid` | done | - | - | `proc.c` |  |
| 111 | `getpgrp` | done | - | - | `proc.c` |  |
| 112 | `setsid` | done | - | - | `proc.c` |  |
| 113 | `setreuid` | done | - | - | `proc.c` |  |
| 114 | `setregid` | done | - | - | `proc.c` |  |
| 115 | `getgroups` | done | - | - | `proc.c` |  |
| 116 | `setgroups` | partial | L2 | - | `proc.c` | thirty-two supplementary groups, where Linux keeps 65536: more is EINVAL |
| 117 | `setresuid` | done | - | - | `proc.c` |  |
| 118 | `getresuid` | done | - | - | `proc.c` |  |
| 119 | `setresgid` | done | - | - | `proc.c` |  |
| 120 | `getresgid` | done | - | - | `proc.c` |  |
| 121 | `getpgid` | done | - | - | `proc.c` |  |
| 122 | `setfsuid` | done | - | - | `proc.c` |  |
| 123 | `setfsgid` | done | - | - | `proc.c` |  |
| 124 | `getsid` | done | - | - | `proc.c` |  |
| 125 | `capget` | missing | L9 | ENOSYS |  | the security goals of docs/vision.md |
| 126 | `capset` | missing | L9 | ENOSYS |  | the security goals of docs/vision.md |
| 127 | `rt_sigpending` | done | - | - | `sig.c` |  |
| 128 | `rt_sigtimedwait` | done | - | - | `sig.c` |  |
| 129 | `rt_sigqueueinfo` | partial | L2 | - | `sig.c` | a real-time signal sent twice before it is taken is delivered once: Linux queues each, this kernel keeps one per signal |
| 130 | `rt_sigsuspend` | done | - | - | `sig.c` |  |
| 131 | `sigaltstack` | done | - | - | `sig.c` |  |
| 132 | `utime` | done | - | - | `names.c` |  |
| 133 | `mknod` | partial | L1 | - | `names.c` | regular files only: a FIFO, a socket or a device node is EPERM - no filesystem here holds one |
| 134 | `uselib` | refused | R | ENOSYS |  | removed or never implemented by Linux itself |
| 135 | `personality` | partial | L2 | - | `limits.c` | kept and reported; no flag changes what the kernel does |
| 136 | `ustat` | refused | R | ENOSYS |  | removed or never implemented by Linux itself |
| 137 | `statfs` | done | - | - | `names.c` |  |
| 138 | `fstatfs` | done | - | - | `names.c` |  |
| 139 | `sysfs` | refused | R | ENOSYS |  | removed or never implemented by Linux itself |
| 140 | `getpriority` | done | - | - | `limits.c` |  |
| 141 | `setpriority` | done | - | - | `limits.c` |  |
| 142 | `sched_setparam` | missing | L6 | ENOSYS |  |  |
| 143 | `sched_getparam` | missing | L6 | ENOSYS |  |  |
| 144 | `sched_setscheduler` | missing | L6 | ENOSYS |  |  |
| 145 | `sched_getscheduler` | missing | L6 | ENOSYS |  |  |
| 146 | `sched_get_priority_max` | missing | L6 | ENOSYS |  |  |
| 147 | `sched_get_priority_min` | missing | L6 | ENOSYS |  |  |
| 148 | `sched_rr_get_interval` | missing | L6 | ENOSYS |  |  |
| 149 | `mlock` | done | - | - | `mm.c` |  |
| 150 | `munlock` | done | - | - | `mm.c` |  |
| 151 | `mlockall` | partial | L3 | - | `mm.c` | MCL_FUTURE is accepted and a mapping made afterwards is not locked |
| 152 | `munlockall` | done | - | - | `mm.c` |  |
| 153 | `vhangup` | missing | L8 | ENOSYS |  |  |
| 154 | `modify_ldt` | refused | R | ENOSYS |  | no LDT: 64-bit programs do not use one |
| 155 | `pivot_root` | missing | L8 | ENOSYS |  |  |
| 156 | `_sysctl` | refused | R | ENOSYS |  | removed or never implemented by Linux itself |
| 157 | `prctl` | done | - | - | `proc.c` |  |
| 158 | `arch_prctl` | done | - | - | `proc.c` |  |
| 159 | `adjtimex` | partial | R | - | `timer.c` | reads the clock's state; setting it is refused, EPERM, as clock_settime is |
| 160 | `setrlimit` | partial | L2 | - | `limits.c` | NOFILE, FSIZE, DATA, NPROC and CPU are enforced; the rest are kept and reported |
| 161 | `chroot` | missing | L8 | ENOSYS |  |  |
| 162 | `sync` | done | - | - | `fs.c` |  |
| 163 | `acct` | deferred | D | ENOSYS |  | no program in the corpus asks; revisit when one does |
| 164 | `settimeofday` | partial | R | - | `timer.c` | judges its arguments; setting the clock is refused, EPERM, as clock_settime is |
| 165 | `mount` | missing | L8 | ENOSYS |  |  |
| 166 | `umount2` | missing | L8 | ENOSYS |  |  |
| 167 | `swapon` | missing | L8 | ENOSYS |  |  |
| 168 | `swapoff` | missing | L8 | ENOSYS |  |  |
| 169 | `reboot` | missing | L8 | ENOSYS |  |  |
| 170 | `sethostname` | missing | L8 | ENOSYS |  |  |
| 171 | `setdomainname` | missing | L8 | ENOSYS |  |  |
| 172 | `iopl` | refused | R | EPERM |  | no raw I/O port access from user space |
| 173 | `ioperm` | refused | R | EPERM |  | no raw I/O port access from user space |
| 174 | `create_module` | refused | R | ENOSYS |  | removed or never implemented by Linux itself |
| 175 | `init_module` | refused | R | EPERM |  | no loadable modules: code enters this kernel only by being built into it |
| 176 | `delete_module` | refused | R | EPERM |  | no loadable modules: code enters this kernel only by being built into it |
| 177 | `get_kernel_syms` | refused | R | ENOSYS |  | removed or never implemented by Linux itself |
| 178 | `query_module` | refused | R | ENOSYS |  | removed or never implemented by Linux itself |
| 179 | `quotactl` | deferred | D | ENOSYS |  | no program in the corpus asks; revisit when one does |
| 180 | `nfsservctl` | refused | R | ENOSYS |  | removed or never implemented by Linux itself |
| 181 | `getpmsg` | refused | R | ENOSYS |  | removed or never implemented by Linux itself |
| 182 | `putpmsg` | refused | R | ENOSYS |  | removed or never implemented by Linux itself |
| 183 | `afs_syscall` | refused | R | ENOSYS |  | removed or never implemented by Linux itself |
| 184 | `tuxcall` | refused | R | ENOSYS |  | removed or never implemented by Linux itself |
| 185 | `security` | refused | R | ENOSYS |  | removed or never implemented by Linux itself |
| 186 | `gettid` | done | - | - | `proc.c` |  |
| 187 | `readahead` | done | - | - | `fs.c` |  |
| 188 | `setxattr` | done | - | - | `names.c` |  |
| 189 | `lsetxattr` | done | - | - | `names.c` |  |
| 190 | `fsetxattr` | done | - | - | `names.c` |  |
| 191 | `getxattr` | done | - | - | `names.c` |  |
| 192 | `lgetxattr` | done | - | - | `names.c` |  |
| 193 | `fgetxattr` | done | - | - | `names.c` |  |
| 194 | `listxattr` | done | - | - | `names.c` |  |
| 195 | `llistxattr` | done | - | - | `names.c` |  |
| 196 | `flistxattr` | done | - | - | `names.c` |  |
| 197 | `removexattr` | done | - | - | `names.c` |  |
| 198 | `lremovexattr` | done | - | - | `names.c` |  |
| 199 | `fremovexattr` | done | - | - | `names.c` |  |
| 200 | `tkill` | done | - | - | `sig.c` |  |
| 201 | `time` | done | - | - | `misc.c` |  |
| 202 | `futex` | partial | L6 | - | `futex.c` | WAIT (with a timeout) and WAKE, private or shared; no requeue, wake_op, bitset or PI |
| 203 | `sched_setaffinity` | missing | L6 | ENOSYS |  |  |
| 204 | `sched_getaffinity` | missing | L6 | ENOSYS |  |  |
| 205 | `set_thread_area` | refused | R | ENOSYS |  | i386 thread storage; x86-64 programs use arch_prctl |
| 206 | `io_setup` | deferred | D | ENOSYS |  | legacy AIO: ENOSYS, and libcs fall back to threads |
| 207 | `io_destroy` | deferred | D | ENOSYS |  | legacy AIO: ENOSYS, and libcs fall back to threads |
| 208 | `io_getevents` | deferred | D | ENOSYS |  | legacy AIO: ENOSYS, and libcs fall back to threads |
| 209 | `io_submit` | deferred | D | ENOSYS |  | legacy AIO: ENOSYS, and libcs fall back to threads |
| 210 | `io_cancel` | deferred | D | ENOSYS |  | legacy AIO: ENOSYS, and libcs fall back to threads |
| 211 | `get_thread_area` | refused | R | ENOSYS |  | i386 thread storage; x86-64 programs use arch_prctl |
| 212 | `lookup_dcookie` | refused | R | ENOSYS |  | removed or never implemented by Linux itself |
| 213 | `epoll_create` | done | - | - | `epoll.c` |  |
| 214 | `epoll_ctl_old` | refused | R | ENOSYS |  | removed or never implemented by Linux itself |
| 215 | `epoll_wait_old` | refused | R | ENOSYS |  | removed or never implemented by Linux itself |
| 216 | `remap_file_pages` | refused | R | ENOSYS |  | removed or never implemented by Linux itself |
| 217 | `getdents64` | done | - | - | `fs.c` |  |
| 218 | `set_tid_address` | done | - | - | `proc.c` |  |
| 219 | `restart_syscall` | done | - | - | `timer.c` |  |
| 220 | `semtimedop` | missing | L7 | ENOSYS |  |  |
| 221 | `fadvise64` | done | - | - | `fs.c` |  |
| 222 | `timer_create` | done | - | - | `timer.c` |  |
| 223 | `timer_settime` | done | - | - | `timer.c` |  |
| 224 | `timer_gettime` | done | - | - | `timer.c` |  |
| 225 | `timer_getoverrun` | done | - | - | `timer.c` |  |
| 226 | `timer_delete` | done | - | - | `timer.c` |  |
| 227 | `clock_settime` | refused | R | EPERM |  | the clock is the timer's uptime; there is nothing to set it from |
| 228 | `clock_gettime` | done | - | - | `misc.c` |  |
| 229 | `clock_getres` | done | - | - | `timer.c` |  |
| 230 | `clock_nanosleep` | done | - | - | `misc.c` |  |
| 231 | `exit_group` | done | - | - | `proc.c` |  |
| 232 | `epoll_wait` | done | - | - | `epoll.c` |  |
| 233 | `epoll_ctl` | done | - | - | `epoll.c` |  |
| 234 | `tgkill` | done | - | - | `sig.c` |  |
| 235 | `utimes` | done | - | - | `names.c` |  |
| 236 | `vserver` | refused | R | ENOSYS |  | removed or never implemented by Linux itself |
| 237 | `mbind` | deferred | D | ENOSYS |  | NUMA: one node here |
| 238 | `set_mempolicy` | deferred | D | ENOSYS |  | NUMA: one node here |
| 239 | `get_mempolicy` | deferred | D | ENOSYS |  | NUMA: one node here |
| 240 | `mq_open` | missing | L7 | ENOSYS |  |  |
| 241 | `mq_unlink` | missing | L7 | ENOSYS |  |  |
| 242 | `mq_timedsend` | missing | L7 | ENOSYS |  |  |
| 243 | `mq_timedreceive` | missing | L7 | ENOSYS |  |  |
| 244 | `mq_notify` | missing | L7 | ENOSYS |  |  |
| 245 | `mq_getsetattr` | missing | L7 | ENOSYS |  |  |
| 246 | `kexec_load` | refused | R | EPERM |  | no kexec: the machine is booted by the bootloader, not by itself |
| 247 | `waitid` | partial | L2 | - | `proc.c` | stopped and continued children are never reported |
| 248 | `add_key` | deferred | D | ENOSYS |  | no program in the corpus asks; revisit when one does |
| 249 | `request_key` | deferred | D | ENOSYS |  | no program in the corpus asks; revisit when one does |
| 250 | `keyctl` | deferred | D | ENOSYS |  | no program in the corpus asks; revisit when one does |
| 251 | `ioprio_set` | missing | L6 | ENOSYS |  |  |
| 252 | `ioprio_get` | missing | L6 | ENOSYS |  |  |
| 253 | `inotify_init` | missing | L4 | ENOSYS |  |  |
| 254 | `inotify_add_watch` | missing | L4 | ENOSYS |  |  |
| 255 | `inotify_rm_watch` | missing | L4 | ENOSYS |  |  |
| 256 | `migrate_pages` | deferred | D | ENOSYS |  | NUMA: one node here |
| 257 | `openat` | done | - | - | `fs.c` |  |
| 258 | `mkdirat` | done | - | - | `fs.c` |  |
| 259 | `mknodat` | partial | L1 | - | `names.c` | regular files only: a FIFO, a socket or a device node is EPERM - no filesystem here holds one |
| 260 | `fchownat` | done | - | - | `names.c` |  |
| 261 | `futimesat` | done | - | - | `names.c` |  |
| 262 | `newfstatat` | done | - | - | `fs.c` |  |
| 263 | `unlinkat` | done | - | - | `fs.c` |  |
| 264 | `renameat` | done | - | - | `names.c` |  |
| 265 | `linkat` | done | - | - | `names.c` |  |
| 266 | `symlinkat` | done | - | - | `names.c` |  |
| 267 | `readlinkat` | done | - | - | `fs.c` |  |
| 268 | `fchmodat` | done | - | - | `names.c` |  |
| 269 | `faccessat` | done | - | - | `names.c` |  |
| 270 | `pselect6` | done | - | - | `poll.c` |  |
| 271 | `ppoll` | done | - | - | `poll.c` |  |
| 272 | `unshare` | deferred | D | ENOSYS |  | namespaces: after the credential model |
| 273 | `set_robust_list` | done | - | - | `proc.c` |  |
| 274 | `get_robust_list` | missing | L6 | ENOSYS |  |  |
| 275 | `splice` | missing | L7 | ENOSYS |  |  |
| 276 | `tee` | missing | L7 | ENOSYS |  |  |
| 277 | `sync_file_range` | done | - | - | `fs.c` |  |
| 278 | `vmsplice` | missing | L7 | ENOSYS |  |  |
| 279 | `move_pages` | deferred | D | ENOSYS |  | NUMA: one node here |
| 280 | `utimensat` | done | - | - | `names.c` |  |
| 281 | `epoll_pwait` | done | - | - | `epoll.c` |  |
| 282 | `signalfd` | done | - | - | `events.c` |  |
| 283 | `timerfd_create` | done | - | - | `events.c` |  |
| 284 | `eventfd` | done | - | - | `events.c` |  |
| 285 | `fallocate` | partial | L1 | - | `fs.c` | the size is guaranteed, not the space; only mode 0 and KEEP_SIZE |
| 286 | `timerfd_settime` | done | - | - | `events.c` |  |
| 287 | `timerfd_gettime` | done | - | - | `events.c` |  |
| 288 | `accept4` | missing | L5 | ENOSYS |  | and AF_UNIX sockets |
| 289 | `signalfd4` | done | - | - | `events.c` |  |
| 290 | `eventfd2` | done | - | - | `events.c` |  |
| 291 | `epoll_create1` | done | - | - | `epoll.c` |  |
| 292 | `dup3` | done | - | - | `fs.c` |  |
| 293 | `pipe2` | done | - | - | `fs.c` |  |
| 294 | `inotify_init1` | missing | L4 | ENOSYS |  |  |
| 295 | `preadv` | done | - | - | `fs.c` |  |
| 296 | `pwritev` | done | - | - | `fs.c` |  |
| 297 | `rt_tgsigqueueinfo` | partial | L2 | - | `sig.c` | as rt_sigqueueinfo: one pending instance per signal |
| 298 | `perf_event_open` | refused | R | ENOSYS |  | no performance-counter interface: a facility this kernel does not have |
| 299 | `recvmmsg` | missing | L5 | ENOSYS |  | and AF_UNIX sockets |
| 300 | `fanotify_init` | deferred | D | ENOSYS |  | no program in the corpus asks; revisit when one does |
| 301 | `fanotify_mark` | deferred | D | ENOSYS |  | no program in the corpus asks; revisit when one does |
| 302 | `prlimit64` | partial | L2 | - | `limits.c` | as setrlimit |
| 303 | `name_to_handle_at` | deferred | D | ENOSYS |  | no program in the corpus asks; revisit when one does |
| 304 | `open_by_handle_at` | deferred | D | ENOSYS |  | no program in the corpus asks; revisit when one does |
| 305 | `clock_adjtime` | partial | R | - | `timer.c` | reads the realtime clock's state; setting it is refused, EPERM, as clock_settime is |
| 306 | `syncfs` | done | - | - | `fs.c` |  |
| 307 | `sendmmsg` | missing | L5 | ENOSYS |  | and AF_UNIX sockets |
| 308 | `setns` | deferred | D | ENOSYS |  | namespaces: after the credential model |
| 309 | `getcpu` | missing | L6 | ENOSYS |  |  |
| 310 | `process_vm_readv` | deferred | D | ENOSYS |  | no program in the corpus asks; revisit when one does |
| 311 | `process_vm_writev` | deferred | D | ENOSYS |  | no program in the corpus asks; revisit when one does |
| 312 | `kcmp` | deferred | D | ENOSYS |  | no program in the corpus asks; revisit when one does |
| 313 | `finit_module` | refused | R | EPERM |  | no loadable modules: code enters this kernel only by being built into it |
| 314 | `sched_setattr` | missing | L6 | ENOSYS |  |  |
| 315 | `sched_getattr` | missing | L6 | ENOSYS |  |  |
| 316 | `renameat2` | partial | L1 | - | `names.c` | RENAME_EXCHANGE and RENAME_WHITEOUT refused |
| 317 | `seccomp` | missing | L9 | ENOSYS |  | the security goals of docs/vision.md |
| 318 | `getrandom` | done | - | - | `misc.c` |  |
| 319 | `memfd_create` | partial | L3 | - | `fs.c` | no seals: MFD_ALLOW_SEALING is accepted and F_ADD_SEALS refused; the file has a name under /tmp while open |
| 320 | `kexec_file_load` | refused | R | EPERM |  | no kexec: the machine is booted by the bootloader, not by itself |
| 321 | `bpf` | refused | R | ENOSYS |  | no in-kernel bytecode: a facility this kernel does not have |
| 322 | `execveat` | done | - | - | `proc.c` |  |
| 323 | `userfaultfd` | refused | R | ENOSYS |  | no user-space fault handling: a facility this kernel does not have |
| 324 | `membarrier` | missing | L6 | ENOSYS |  |  |
| 325 | `mlock2` | done | - | - | `mm.c` |  |
| 326 | `copy_file_range` | done | - | - | `fs.c` |  |
| 327 | `preadv2` | partial | L1 | - | `fs.c` | RWF_ flags refused |
| 328 | `pwritev2` | partial | L1 | - | `fs.c` | RWF_ flags refused |
| 329 | `pkey_mprotect` | deferred | D | ENOSYS |  | no program in the corpus asks; revisit when one does |
| 330 | `pkey_alloc` | deferred | D | ENOSYS |  | no program in the corpus asks; revisit when one does |
| 331 | `pkey_free` | deferred | D | ENOSYS |  | no program in the corpus asks; revisit when one does |
| 332 | `statx` | done | - | - | `names.c` |  |
| 333 | `io_pgetevents` | deferred | D | ENOSYS |  | legacy AIO: ENOSYS, and libcs fall back to threads |
| 334 | `rseq` | partial | R | - | `proc.c` | ENOSYS on purpose: the libc takes its fallback |
| 424 | `pidfd_send_signal` | done | - | - | `sig.c` |  |
| 425 | `io_uring_setup` | deferred | D | ENOSYS |  | a second I/O model; after L4 |
| 426 | `io_uring_enter` | deferred | D | ENOSYS |  | a second I/O model; after L4 |
| 427 | `io_uring_register` | deferred | D | ENOSYS |  | a second I/O model; after L4 |
| 428 | `open_tree` | deferred | D | ENOSYS |  | the new mount API: after L8 |
| 429 | `move_mount` | deferred | D | ENOSYS |  | the new mount API: after L8 |
| 430 | `fsopen` | deferred | D | ENOSYS |  | the new mount API: after L8 |
| 431 | `fsconfig` | deferred | D | ENOSYS |  | the new mount API: after L8 |
| 432 | `fsmount` | deferred | D | ENOSYS |  | the new mount API: after L8 |
| 433 | `fspick` | deferred | D | ENOSYS |  | the new mount API: after L8 |
| 434 | `pidfd_open` | done | - | - | `sig.c` |  |
| 435 | `clone3` | partial | L2 | - | `proc.c` | as clone: no CLONE_VM process, no set_tid, no cgroup |
| 436 | `close_range` | partial | L6 | - | `fs.c` | CLOSE_RANGE_UNSHARE in a process with threads refused: a thread cannot hold a table of its own |
| 437 | `openat2` | partial | L1 | - | `names.c` | resolve: only NO_MAGICLINKS; BENEATH, IN_ROOT, NO_XDEV and NO_SYMLINKS answer ENOSYS |
| 438 | `pidfd_getfd` | deferred | D | ENOSYS |  | no program in the corpus asks; revisit when one does |
| 439 | `faccessat2` | done | - | - | `names.c` |  |
| 440 | `process_madvise` | deferred | D | ENOSYS |  | no program in the corpus asks; revisit when one does |
| 441 | `epoll_pwait2` | done | - | - | `epoll.c` |  |
| 442 | `mount_setattr` | deferred | D | ENOSYS |  | the new mount API: after L8 |
| 443 | `quotactl_fd` | deferred | D | ENOSYS |  | no program in the corpus asks; revisit when one does |
| 444 | `landlock_create_ruleset` | missing | L9 | ENOSYS |  | the security goals of docs/vision.md |
| 445 | `landlock_add_rule` | missing | L9 | ENOSYS |  | the security goals of docs/vision.md |
| 446 | `landlock_restrict_self` | missing | L9 | ENOSYS |  | the security goals of docs/vision.md |
| 447 | `memfd_secret` | refused | R | ENOSYS |  | no secret memory areas |
| 448 | `process_mrelease` | deferred | D | ENOSYS |  | no program in the corpus asks; revisit when one does |
| 449 | `futex_waitv` | missing | L6 | ENOSYS |  |  |
| 450 | `set_mempolicy_home_node` | deferred | D | ENOSYS |  | NUMA: one node here |
| 451 | `cachestat` | deferred | D | ENOSYS |  | no program in the corpus asks; revisit when one does |
| 452 | `fchmodat2` | done | - | - | `names.c` |  |
| 453 | `map_shadow_stack` | refused | R | ENOSYS |  | no shadow stacks |
| 454 | `futex_wake` | missing | L6 | ENOSYS |  |  |
| 455 | `futex_wait` | missing | L6 | ENOSYS |  |  |
| 456 | `futex_requeue` | missing | L6 | ENOSYS |  |  |
| 457 | `statmount` | deferred | D | ENOSYS |  | the new mount API: after L8 |
| 458 | `listmount` | deferred | D | ENOSYS |  | the new mount API: after L8 |
| 459 | `lsm_get_self_attr` | deferred | D | ENOSYS |  | no program in the corpus asks; revisit when one does |
| 460 | `lsm_set_self_attr` | deferred | D | ENOSYS |  | no program in the corpus asks; revisit when one does |
| 461 | `lsm_list_modules` | deferred | D | ENOSYS |  | no program in the corpus asks; revisit when one does |
| 1000 | `netctl` | done | NATIVE | - | `net.c` | VibeOS's own, outside the Linux number space |
| 1001 | `pageinfo` | done | NATIVE | - | `mm.c` | VibeOS's own, outside the Linux number space |
