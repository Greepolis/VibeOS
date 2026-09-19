#ifndef VIBEOS_LINUX_INTERNAL_H
#define VIBEOS_LINUX_INTERNAL_H

/* What the files in kernel/abi/linux share with each other.
 *
 * The Linux syscall handlers used to live in arch_hw.c as file-scope statics. They
 * still reach the architecture through arch_hw_internal.h - a task, its address
 * space, its descriptors are x86-64 objects today - and this header is only what
 * the handlers themselves pass between files. */

#include "../../arch/x86_64/arch_hw_internal.h"
#include <stdint.h>
#include "vibeos/arch_x86_64.h"
#include "vibeos/trap.h"
#include "vibeos/boot.h"
#include "vibeos/mm.h"
#include "vibeos/inet.h"
#include "vibeos/elf.h"
#include "vibeos/services.h"
#include "vibeos/exec_stats.h"
#include "vibeos/account.h"
#include "vibeos/forkguard.h"
#include "vibeos/sched_policy.h"
#include "vibeos/pageinfo.h"
#include "vibeos/rmap.h"
#include "vibeos/reclaim.h"
#include "vibeos/mbz.h"
#include "vibeos/abi.h"
#include "vibeos/abi_linux.h"
#include "vibeos/ceildiv.h"
#include "vibeos/blkdev.h"
#include "vibeos/io_stats.h"
#include "vibeos/blockdev.h"
#include "vibeos/partition.h"
#include "vibeos/parttab.h"
#include "vibeos/ext2.h"
#include "vibeos/iso9660.h"
#include "vibeos/exfat.h"
#include "vibeos/ntfs.h"
#include "vibeos/logsink.h"
#include "vibeos/storage.h"
#include "vibeos/swapmap.h"
#include "vibeos/anon.h"
#include "vibeos/swaparea.h"
#include "vibeos/log.h"
#include "vibeos/mm_model.h"
#include "vibeos/frame.h"
#include "vibeos/vmspace.h"
#include "vibeos/vma.h"
#include "vibeos/backing.h"
#include "vibeos/task_stats.h"
#include "vibeos/task.h"
#include "vibeos/runq.h"
#include "vibeos/lifetime.h"
#include "vibeos/vfs.h"

#define VIBEOS_ARG_INT(v)  ((int)(uint32_t)(v))

extern char g_exec_cached[128];
extern long g_exec_cached_len;
extern uint32_t g_exec_cached_id;
void hw_exec_cache_drop(void);
long hw_sys_pipe2(uint64_t fds_uptr, uint64_t flags);
long hw_sys_dup2(uint64_t oldfd, uint64_t newfd);
long hw_sys_write(uint64_t fd, uint64_t buf, uint64_t len);
long hw_sys_read(uint64_t fd, uint64_t buf, uint64_t len);
long hw_sys_open(uint64_t path_uptr, uint64_t flags);
long hw_sys_close(uint64_t fd);
long hw_sys_lseek(uint64_t fd, uint64_t off, uint64_t whence);
long hw_sys_getdents64(uint64_t fd, uint64_t buf, uint64_t len);
long hw_sys_unlink(uint64_t path_uptr);
long hw_sys_mkdir(uint64_t path_uptr);
void hw_mm_lock(hw_procstate_t *ps);
void hw_mm_unlock(hw_procstate_t *ps);
long hw_sys_brk(uint64_t addr);
long hw_sys_mmap(uint64_t addr, uint64_t len, uint64_t prot,
                        uint64_t flags, uint64_t fd);
long hw_sys_mprotect(uint64_t addr, uint64_t len, uint64_t prot);
long hw_sys_munmap(uint64_t addr, uint64_t len);
long hw_sys_fork(const vibeos_x86_64_isr_frame_t *frame);
long hw_sys_clone_thread(const vibeos_x86_64_isr_frame_t *frame,
                                uint64_t flags, uint64_t child_stack,
                                uint64_t ptid, uint64_t ctid, uint64_t tls);
long hw_sys_waitpid(uint64_t want_pid, uint64_t status_ptr,
                           uint64_t options);
long hw_sys_execve(vibeos_x86_64_isr_frame_t *frame, uint64_t path_uptr,
                          uint64_t argv_uptr, uint64_t envp_uptr);
long hw_sys_fstat(uint64_t fd, uint64_t ubuf);
long hw_sys_newfstatat(uint64_t dirfd, uint64_t path_uptr, uint64_t ubuf,
                              uint64_t flags);
long hw_sys_openat(uint64_t dirfd, uint64_t path_uptr, uint64_t flags);
long hw_sys_getcwd(uint64_t ubuf, uint64_t size);
long hw_sys_readlinkat(uint64_t dirfd, uint64_t path_uptr, uint64_t ubuf,
                              uint64_t bufsz);
long hw_sys_prctl(uint64_t op, uint64_t arg);
long hw_sys_pageinfo(uint64_t va, uint64_t out_uptr);
long hw_sys_kill(uint64_t target_pid, uint64_t sig);
long hw_sys_setpgid(uint64_t requested_pid, uint64_t requested_pgid);
long hw_sys_setsid(void);
long hw_sys_getsid(uint64_t requested_pid);
long hw_sys_tkill(uint64_t target_tid, uint64_t sig);
long hw_sys_tgkill(uint64_t target_tgid, uint64_t target_tid,
                          uint64_t sig);
long hw_sys_rt_sigaction(uint64_t sig, uint64_t act_uptr, uint64_t old_uptr);
long hw_sys_rt_sigprocmask(uint64_t how, uint64_t set_uptr, uint64_t old_uptr);
long hw_sys_setresid(uint64_t id);
long hw_sys_arch_prctl(uint64_t code, uint64_t addr);
long hw_sys_ioctl(uint64_t fd, uint64_t req, uint64_t arg);
long hw_sys_writev(uint64_t fd, uint64_t iov_uptr, uint64_t iovcnt);
long hw_sys_readv(uint64_t fd, uint64_t iov_uptr, uint64_t iovcnt);
long hw_sys_uname(uint64_t buf);
long hw_sys_clock_gettime(uint64_t clk, uint64_t ts_uptr);
long hw_sys_time(uint64_t tptr);
long hw_sys_prlimit64(uint64_t resource, uint64_t new_uptr, uint64_t old_uptr);
long hw_sys_futex(uint64_t addr, uint64_t op, uint64_t val);
long hw_sys_rt_sigreturn(vibeos_x86_64_isr_frame_t *frame);
long vibeos_x86_64_linux_syscall(vibeos_x86_64_isr_frame_t *frame,
                                 uint64_t nr, uint64_t a1, uint64_t a2, uint64_t a3);

#endif
