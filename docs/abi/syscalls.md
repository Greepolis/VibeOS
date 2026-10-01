# Every Linux x86-64 syscall

All 373 Linux x86-64 numbers (0-334 and 424-461), each with the answer this
kernel gives it and the phase of [phases.md](phases.md) that owns it. Generated
by `scripts/dev/make-syscall-table.py` from the registry,
`kernel/abi/linux_syscalls.def` - the same file the kernel reads to answer a
number with no row, and `check-syscall-checks.py` holds every row against. Edit
the registry, not this page; `check.sh` fails while the two differ.

| State | Count | What the kernel answers |
| --- | --- | --- |
| done | 75 | through its row |
| partial | 22 | through its row, with the gap named |
| missing | 193 | ENOSYS, and the boot gate fails naming the number |
| deferred | 50 | ENOSYS, and the boot gate fails naming the number |
| refused | 33 | its errno, counted as refused - expected, never a failure |

Without a row, by phase:

| Phase | Syscalls |
| --- | --- |
| L1 - files and paths | 46 |
| L2 - processes, credentials, time | 47 |
| L3 - memory | 10 |
| L4 - event loops | 21 |
| L5 - sockets | 11 |
| L6 - threads and scheduling | 20 |
| L7 - IPC | 21 |
| L8 - system administration | 11 |
| L9 - security | 6 |
| D - deferred | 50 |
| R - refused | 33 |

A `partial` row names its gap and the phase that closes it. VibeOS's own two
calls, outside the Linux number space, are listed last.

| Nr | Syscall | State | Phase | Errno | Row | Note |
| --- | --- | --- | --- | --- | --- | --- |
| 0 | `read` | done | - | - | `fs.c` |  |
| 1 | `write` | done | - | - | `fs.c` |  |
| 2 | `open` | done | - | - | `fs.c` |  |
| 3 | `close` | done | - | - | `fs.c` |  |
| 4 | `stat` | missing | L1 | ENOSYS |  |  |
| 5 | `fstat` | done | - | - | `fs.c` |  |
| 6 | `lstat` | missing | L1 | ENOSYS |  |  |
| 7 | `poll` | missing | L4 | ENOSYS |  |  |
| 8 | `lseek` | done | - | - | `fs.c` |  |
| 9 | `mmap` | partial | L3 | - | `mm.c` | anonymous only; MAP_FIXED and file-backed mappings refused |
| 10 | `mprotect` | done | - | - | `mm.c` |  |
| 11 | `munmap` | done | - | - | `mm.c` |  |
| 12 | `brk` | done | - | - | `mm.c` |  |
| 13 | `rt_sigaction` | done | - | - | `sig.c` |  |
| 14 | `rt_sigprocmask` | done | - | - | `sig.c` |  |
| 15 | `rt_sigreturn` | done | - | - | `sig.c` |  |
| 16 | `ioctl` | partial | L1 | - | `fs.c` | ENOTTY for everything: no terminal device |
| 17 | `pread64` | done | - | - | `fs.c` |  |
| 18 | `pwrite64` | partial | L1 | - | `fs.c` | on a filesystem that stores whole files (FAT) there is no offset to write at |
| 19 | `readv` | partial | L5 | - | `fs.c` | files and the console; no socket scatter |
| 20 | `writev` | partial | L5 | - | `fs.c` | files and the console; no socket gather |
| 21 | `access` | missing | L1 | ENOSYS |  |  |
| 22 | `pipe` | done | - | - | `fs.c` |  |
| 23 | `select` | missing | L4 | ENOSYS |  |  |
| 24 | `sched_yield` | done | - | - | `proc.c` |  |
| 25 | `mremap` | missing | L3 | ENOSYS |  | with file-backed and shared mappings (the other half of mmap) |
| 26 | `msync` | missing | L3 | ENOSYS |  | with file-backed and shared mappings (the other half of mmap) |
| 27 | `mincore` | missing | L3 | ENOSYS |  | with file-backed and shared mappings (the other half of mmap) |
| 28 | `madvise` | missing | L3 | ENOSYS |  | with file-backed and shared mappings (the other half of mmap) |
| 29 | `shmget` | missing | L7 | ENOSYS |  |  |
| 30 | `shmat` | missing | L7 | ENOSYS |  |  |
| 31 | `shmctl` | missing | L7 | ENOSYS |  |  |
| 32 | `dup` | done | - | - | `fs.c` |  |
| 33 | `dup2` | done | - | - | `fs.c` |  |
| 34 | `pause` | missing | L2 | ENOSYS |  |  |
| 35 | `nanosleep` | missing | L2 | ENOSYS |  |  |
| 36 | `getitimer` | missing | L2 | ENOSYS |  |  |
| 37 | `alarm` | missing | L2 | ENOSYS |  |  |
| 38 | `setitimer` | missing | L2 | ENOSYS |  |  |
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
| 72 | `fcntl` | partial | L1 | - | `fs.c` | no record locks: F_GETLK/F_SETLK answer ENOLCK |
| 73 | `flock` | missing | L1 | ENOSYS |  |  |
| 74 | `fsync` | done | - | - | `fs.c` |  |
| 75 | `fdatasync` | done | - | - | `fs.c` |  |
| 76 | `truncate` | partial | L1 | - | `fs.c` | FAT can only be emptied, not cut or grown |
| 77 | `ftruncate` | partial | L1 | - | `fs.c` | FAT can only be cut within what the descriptor wrote |
| 78 | `getdents` | missing | L1 | ENOSYS |  |  |
| 79 | `getcwd` | done | - | - | `fs.c` |  |
| 80 | `chdir` | done | - | - | `fs.c` |  |
| 81 | `fchdir` | done | - | - | `fs.c` |  |
| 82 | `rename` | missing | L1 | ENOSYS |  |  |
| 83 | `mkdir` | done | - | - | `fs.c` |  |
| 84 | `rmdir` | missing | L1 | ENOSYS |  |  |
| 85 | `creat` | done | - | - | `fs.c` |  |
| 86 | `link` | missing | L1 | ENOSYS |  |  |
| 87 | `unlink` | done | - | - | `fs.c` |  |
| 88 | `symlink` | missing | L1 | ENOSYS |  |  |
| 89 | `readlink` | missing | L1 | ENOSYS |  |  |
| 90 | `chmod` | missing | L1 | ENOSYS |  |  |
| 91 | `fchmod` | missing | L1 | ENOSYS |  |  |
| 92 | `chown` | missing | L1 | ENOSYS |  |  |
| 93 | `fchown` | missing | L1 | ENOSYS |  |  |
| 94 | `lchown` | missing | L1 | ENOSYS |  |  |
| 95 | `umask` | done | - | - | `fs.c` |  |
| 96 | `gettimeofday` | missing | L2 | ENOSYS |  |  |
| 97 | `getrlimit` | missing | L2 | ENOSYS |  |  |
| 98 | `getrusage` | missing | L2 | ENOSYS |  |  |
| 99 | `sysinfo` | done | - | - | `misc.c` |  |
| 100 | `times` | missing | L2 | ENOSYS |  |  |
| 101 | `ptrace` | deferred | D | ENOSYS |  | no program in the corpus asks; revisit when one does |
| 102 | `getuid` | done | - | - | `proc.c` |  |
| 103 | `syslog` | missing | L8 | ENOSYS |  |  |
| 104 | `getgid` | done | - | - | `proc.c` |  |
| 105 | `setuid` | partial | L2 | - | `proc.c` | one user; accepted without a credential model |
| 106 | `setgid` | partial | L2 | - | `proc.c` | one user; accepted without a credential model |
| 107 | `geteuid` | done | - | - | `proc.c` |  |
| 108 | `getegid` | done | - | - | `proc.c` |  |
| 109 | `setpgid` | done | - | - | `proc.c` |  |
| 110 | `getppid` | done | - | - | `proc.c` |  |
| 111 | `getpgrp` | done | - | - | `proc.c` |  |
| 112 | `setsid` | done | - | - | `proc.c` |  |
| 113 | `setreuid` | missing | L2 | ENOSYS |  | the credential model arrives here |
| 114 | `setregid` | missing | L2 | ENOSYS |  | the credential model arrives here |
| 115 | `getgroups` | missing | L2 | ENOSYS |  | the credential model arrives here |
| 116 | `setgroups` | missing | L2 | ENOSYS |  | the credential model arrives here |
| 117 | `setresuid` | missing | L2 | ENOSYS |  | the credential model arrives here |
| 118 | `getresuid` | missing | L2 | ENOSYS |  | the credential model arrives here |
| 119 | `setresgid` | missing | L2 | ENOSYS |  | the credential model arrives here |
| 120 | `getresgid` | missing | L2 | ENOSYS |  | the credential model arrives here |
| 121 | `getpgid` | missing | L2 | ENOSYS |  |  |
| 122 | `setfsuid` | missing | L2 | ENOSYS |  | the credential model arrives here |
| 123 | `setfsgid` | missing | L2 | ENOSYS |  | the credential model arrives here |
| 124 | `getsid` | done | - | - | `proc.c` |  |
| 125 | `capget` | missing | L9 | ENOSYS |  | the security goals of docs/vision.md |
| 126 | `capset` | missing | L9 | ENOSYS |  | the security goals of docs/vision.md |
| 127 | `rt_sigpending` | missing | L2 | ENOSYS |  |  |
| 128 | `rt_sigtimedwait` | missing | L2 | ENOSYS |  |  |
| 129 | `rt_sigqueueinfo` | missing | L2 | ENOSYS |  |  |
| 130 | `rt_sigsuspend` | missing | L2 | ENOSYS |  |  |
| 131 | `sigaltstack` | missing | L2 | ENOSYS |  |  |
| 132 | `utime` | missing | L1 | ENOSYS |  |  |
| 133 | `mknod` | missing | L1 | ENOSYS |  |  |
| 134 | `uselib` | refused | R | ENOSYS |  | removed or never implemented by Linux itself |
| 135 | `personality` | missing | L2 | ENOSYS |  |  |
| 136 | `ustat` | refused | R | ENOSYS |  | removed or never implemented by Linux itself |
| 137 | `statfs` | missing | L1 | ENOSYS |  |  |
| 138 | `fstatfs` | missing | L1 | ENOSYS |  |  |
| 139 | `sysfs` | refused | R | ENOSYS |  | removed or never implemented by Linux itself |
| 140 | `getpriority` | missing | L2 | ENOSYS |  |  |
| 141 | `setpriority` | missing | L2 | ENOSYS |  |  |
| 142 | `sched_setparam` | missing | L6 | ENOSYS |  |  |
| 143 | `sched_getparam` | missing | L6 | ENOSYS |  |  |
| 144 | `sched_setscheduler` | missing | L6 | ENOSYS |  |  |
| 145 | `sched_getscheduler` | missing | L6 | ENOSYS |  |  |
| 146 | `sched_get_priority_max` | missing | L6 | ENOSYS |  |  |
| 147 | `sched_get_priority_min` | missing | L6 | ENOSYS |  |  |
| 148 | `sched_rr_get_interval` | missing | L6 | ENOSYS |  |  |
| 149 | `mlock` | missing | L3 | ENOSYS |  | with file-backed and shared mappings (the other half of mmap) |
| 150 | `munlock` | missing | L3 | ENOSYS |  | with file-backed and shared mappings (the other half of mmap) |
| 151 | `mlockall` | missing | L3 | ENOSYS |  | with file-backed and shared mappings (the other half of mmap) |
| 152 | `munlockall` | missing | L3 | ENOSYS |  | with file-backed and shared mappings (the other half of mmap) |
| 153 | `vhangup` | missing | L8 | ENOSYS |  |  |
| 154 | `modify_ldt` | refused | R | ENOSYS |  | no LDT: 64-bit programs do not use one |
| 155 | `pivot_root` | missing | L8 | ENOSYS |  |  |
| 156 | `_sysctl` | refused | R | ENOSYS |  | removed or never implemented by Linux itself |
| 157 | `prctl` | done | - | - | `proc.c` |  |
| 158 | `arch_prctl` | done | - | - | `proc.c` |  |
| 159 | `adjtimex` | missing | L2 | ENOSYS |  | EPERM without privilege; real only once there is one |
| 160 | `setrlimit` | missing | L2 | ENOSYS |  |  |
| 161 | `chroot` | missing | L8 | ENOSYS |  |  |
| 162 | `sync` | done | - | - | `fs.c` |  |
| 163 | `acct` | deferred | D | ENOSYS |  | no program in the corpus asks; revisit when one does |
| 164 | `settimeofday` | missing | L2 | ENOSYS |  | EPERM without privilege; real only once there is one |
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
| 188 | `setxattr` | missing | L1 | ENOSYS |  | ENOTSUP from the filesystem until one stores attributes |
| 189 | `lsetxattr` | missing | L1 | ENOSYS |  | ENOTSUP from the filesystem until one stores attributes |
| 190 | `fsetxattr` | missing | L1 | ENOSYS |  | ENOTSUP from the filesystem until one stores attributes |
| 191 | `getxattr` | missing | L1 | ENOSYS |  | ENOTSUP from the filesystem until one stores attributes |
| 192 | `lgetxattr` | missing | L1 | ENOSYS |  | ENOTSUP from the filesystem until one stores attributes |
| 193 | `fgetxattr` | missing | L1 | ENOSYS |  | ENOTSUP from the filesystem until one stores attributes |
| 194 | `listxattr` | missing | L1 | ENOSYS |  | ENOTSUP from the filesystem until one stores attributes |
| 195 | `llistxattr` | missing | L1 | ENOSYS |  | ENOTSUP from the filesystem until one stores attributes |
| 196 | `flistxattr` | missing | L1 | ENOSYS |  | ENOTSUP from the filesystem until one stores attributes |
| 197 | `removexattr` | missing | L1 | ENOSYS |  | ENOTSUP from the filesystem until one stores attributes |
| 198 | `lremovexattr` | missing | L1 | ENOSYS |  | ENOTSUP from the filesystem until one stores attributes |
| 199 | `fremovexattr` | missing | L1 | ENOSYS |  | ENOTSUP from the filesystem until one stores attributes |
| 200 | `tkill` | done | - | - | `sig.c` |  |
| 201 | `time` | done | - | - | `misc.c` |  |
| 202 | `futex` | partial | L6 | - | `futex.c` | WAIT and WAKE; no requeue, wake_op or PI |
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
| 213 | `epoll_create` | missing | L4 | ENOSYS |  |  |
| 214 | `epoll_ctl_old` | refused | R | ENOSYS |  | removed or never implemented by Linux itself |
| 215 | `epoll_wait_old` | refused | R | ENOSYS |  | removed or never implemented by Linux itself |
| 216 | `remap_file_pages` | refused | R | ENOSYS |  | removed or never implemented by Linux itself |
| 217 | `getdents64` | partial | L1 | - | `fs.c` | one directory stream per descriptor; names cut at 15 bytes |
| 218 | `set_tid_address` | done | - | - | `proc.c` |  |
| 219 | `restart_syscall` | missing | L2 | ENOSYS |  |  |
| 220 | `semtimedop` | missing | L7 | ENOSYS |  |  |
| 221 | `fadvise64` | done | - | - | `fs.c` |  |
| 222 | `timer_create` | missing | L2 | ENOSYS |  |  |
| 223 | `timer_settime` | missing | L2 | ENOSYS |  |  |
| 224 | `timer_gettime` | missing | L2 | ENOSYS |  |  |
| 225 | `timer_getoverrun` | missing | L2 | ENOSYS |  |  |
| 226 | `timer_delete` | missing | L2 | ENOSYS |  |  |
| 227 | `clock_settime` | missing | L2 | ENOSYS |  | EPERM without privilege; real only once there is one |
| 228 | `clock_gettime` | done | - | - | `misc.c` |  |
| 229 | `clock_getres` | missing | L2 | ENOSYS |  |  |
| 230 | `clock_nanosleep` | missing | L2 | ENOSYS |  |  |
| 231 | `exit_group` | done | - | - | `proc.c` |  |
| 232 | `epoll_wait` | missing | L4 | ENOSYS |  |  |
| 233 | `epoll_ctl` | missing | L4 | ENOSYS |  |  |
| 234 | `tgkill` | done | - | - | `sig.c` |  |
| 235 | `utimes` | missing | L1 | ENOSYS |  |  |
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
| 247 | `waitid` | missing | L2 | ENOSYS |  |  |
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
| 259 | `mknodat` | missing | L1 | ENOSYS |  |  |
| 260 | `fchownat` | missing | L1 | ENOSYS |  |  |
| 261 | `futimesat` | missing | L1 | ENOSYS |  |  |
| 262 | `newfstatat` | done | - | - | `fs.c` |  |
| 263 | `unlinkat` | partial | L1 | - | `fs.c` | AT_REMOVEDIR refused: no filesystem here implements rmdir |
| 264 | `renameat` | missing | L1 | ENOSYS |  |  |
| 265 | `linkat` | missing | L1 | ENOSYS |  |  |
| 266 | `symlinkat` | missing | L1 | ENOSYS |  |  |
| 267 | `readlinkat` | done | - | - | `fs.c` |  |
| 268 | `fchmodat` | missing | L1 | ENOSYS |  |  |
| 269 | `faccessat` | missing | L1 | ENOSYS |  |  |
| 270 | `pselect6` | missing | L4 | ENOSYS |  |  |
| 271 | `ppoll` | missing | L4 | ENOSYS |  |  |
| 272 | `unshare` | deferred | D | ENOSYS |  | namespaces: after the credential model |
| 273 | `set_robust_list` | done | - | - | `proc.c` |  |
| 274 | `get_robust_list` | missing | L6 | ENOSYS |  |  |
| 275 | `splice` | missing | L7 | ENOSYS |  |  |
| 276 | `tee` | missing | L7 | ENOSYS |  |  |
| 277 | `sync_file_range` | done | - | - | `fs.c` |  |
| 278 | `vmsplice` | missing | L7 | ENOSYS |  |  |
| 279 | `move_pages` | deferred | D | ENOSYS |  | NUMA: one node here |
| 280 | `utimensat` | missing | L1 | ENOSYS |  |  |
| 281 | `epoll_pwait` | missing | L4 | ENOSYS |  |  |
| 282 | `signalfd` | missing | L4 | ENOSYS |  |  |
| 283 | `timerfd_create` | missing | L4 | ENOSYS |  |  |
| 284 | `eventfd` | missing | L4 | ENOSYS |  |  |
| 285 | `fallocate` | partial | L1 | - | `fs.c` | the size is guaranteed, not the space; only mode 0 and KEEP_SIZE |
| 286 | `timerfd_settime` | missing | L4 | ENOSYS |  |  |
| 287 | `timerfd_gettime` | missing | L4 | ENOSYS |  |  |
| 288 | `accept4` | missing | L5 | ENOSYS |  | and AF_UNIX sockets |
| 289 | `signalfd4` | missing | L4 | ENOSYS |  |  |
| 290 | `eventfd2` | missing | L4 | ENOSYS |  |  |
| 291 | `epoll_create1` | missing | L4 | ENOSYS |  |  |
| 292 | `dup3` | done | - | - | `fs.c` |  |
| 293 | `pipe2` | done | - | - | `fs.c` |  |
| 294 | `inotify_init1` | missing | L4 | ENOSYS |  |  |
| 295 | `preadv` | done | - | - | `fs.c` |  |
| 296 | `pwritev` | partial | L1 | - | `fs.c` | on a filesystem that stores whole files (FAT) there is no offset to write at |
| 297 | `rt_tgsigqueueinfo` | missing | L2 | ENOSYS |  |  |
| 298 | `perf_event_open` | refused | R | ENOSYS |  | no performance-counter interface: a facility this kernel does not have |
| 299 | `recvmmsg` | missing | L5 | ENOSYS |  | and AF_UNIX sockets |
| 300 | `fanotify_init` | deferred | D | ENOSYS |  | no program in the corpus asks; revisit when one does |
| 301 | `fanotify_mark` | deferred | D | ENOSYS |  | no program in the corpus asks; revisit when one does |
| 302 | `prlimit64` | partial | L2 | - | `proc.c` | reports limits; setting them is not enforced |
| 303 | `name_to_handle_at` | deferred | D | ENOSYS |  | no program in the corpus asks; revisit when one does |
| 304 | `open_by_handle_at` | deferred | D | ENOSYS |  | no program in the corpus asks; revisit when one does |
| 305 | `clock_adjtime` | missing | L2 | ENOSYS |  | EPERM without privilege; real only once there is one |
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
| 316 | `renameat2` | missing | L1 | ENOSYS |  |  |
| 317 | `seccomp` | missing | L9 | ENOSYS |  | the security goals of docs/vision.md |
| 318 | `getrandom` | done | - | - | `proc.c` |  |
| 319 | `memfd_create` | missing | L3 | ENOSYS |  | with file-backed and shared mappings (the other half of mmap) |
| 320 | `kexec_file_load` | refused | R | EPERM |  | no kexec: the machine is booted by the bootloader, not by itself |
| 321 | `bpf` | refused | R | ENOSYS |  | no in-kernel bytecode: a facility this kernel does not have |
| 322 | `execveat` | missing | L2 | ENOSYS |  |  |
| 323 | `userfaultfd` | refused | R | ENOSYS |  | no user-space fault handling: a facility this kernel does not have |
| 324 | `membarrier` | missing | L6 | ENOSYS |  |  |
| 325 | `mlock2` | missing | L3 | ENOSYS |  | with file-backed and shared mappings (the other half of mmap) |
| 326 | `copy_file_range` | done | - | - | `fs.c` |  |
| 327 | `preadv2` | partial | L1 | - | `fs.c` | RWF_ flags refused |
| 328 | `pwritev2` | partial | L1 | - | `fs.c` | RWF_ flags refused; on a filesystem that stores whole files (FAT) there is no offset to write at |
| 329 | `pkey_mprotect` | deferred | D | ENOSYS |  | no program in the corpus asks; revisit when one does |
| 330 | `pkey_alloc` | deferred | D | ENOSYS |  | no program in the corpus asks; revisit when one does |
| 331 | `pkey_free` | deferred | D | ENOSYS |  | no program in the corpus asks; revisit when one does |
| 332 | `statx` | missing | L1 | ENOSYS |  |  |
| 333 | `io_pgetevents` | deferred | D | ENOSYS |  | legacy AIO: ENOSYS, and libcs fall back to threads |
| 334 | `rseq` | partial | R | - | `proc.c` | ENOSYS on purpose: the libc takes its fallback |
| 424 | `pidfd_send_signal` | missing | L2 | ENOSYS |  |  |
| 425 | `io_uring_setup` | deferred | D | ENOSYS |  | a second I/O model; after L4 |
| 426 | `io_uring_enter` | deferred | D | ENOSYS |  | a second I/O model; after L4 |
| 427 | `io_uring_register` | deferred | D | ENOSYS |  | a second I/O model; after L4 |
| 428 | `open_tree` | deferred | D | ENOSYS |  | the new mount API: after L8 |
| 429 | `move_mount` | deferred | D | ENOSYS |  | the new mount API: after L8 |
| 430 | `fsopen` | deferred | D | ENOSYS |  | the new mount API: after L8 |
| 431 | `fsconfig` | deferred | D | ENOSYS |  | the new mount API: after L8 |
| 432 | `fsmount` | deferred | D | ENOSYS |  | the new mount API: after L8 |
| 433 | `fspick` | deferred | D | ENOSYS |  | the new mount API: after L8 |
| 434 | `pidfd_open` | missing | L2 | ENOSYS |  |  |
| 435 | `clone3` | missing | L2 | ENOSYS |  |  |
| 436 | `close_range` | partial | L1 | - | `fs.c` | CLOSE_RANGE_UNSHARE refused: no unshare(CLONE_FILES) |
| 437 | `openat2` | missing | L1 | ENOSYS |  |  |
| 438 | `pidfd_getfd` | deferred | D | ENOSYS |  | no program in the corpus asks; revisit when one does |
| 439 | `faccessat2` | missing | L1 | ENOSYS |  |  |
| 440 | `process_madvise` | deferred | D | ENOSYS |  | no program in the corpus asks; revisit when one does |
| 441 | `epoll_pwait2` | missing | L4 | ENOSYS |  |  |
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
| 452 | `fchmodat2` | missing | L1 | ENOSYS |  |  |
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
