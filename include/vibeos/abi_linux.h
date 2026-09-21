#ifndef VIBEOS_ABI_LINUX_H
#define VIBEOS_ABI_LINUX_H

/* What the Linux ABI needs to name that is not a syscall row.
 *
 * The syscall numbers are not here any more: each one is in the row that also
 * names its handler (the files under kernel/abi/linux, via LINUX_DEFINE_SYSCALLS), taken from
 * arch/x86/entry/syscalls/syscall_64.tbl. The two at 1000 and above are
 * VibeOS's own and deliberately outside the Linux number space, so they can
 * never collide with a real syscall implemented later. The host suite holds
 * every row's number to the name it should carry. */

/* clone() flags that decide whether it is a fork or a thread. */
#define CLONE_VM     0x00000100u
#define CLONE_FS     0x00000200u
#define CLONE_FILES  0x00000400u
#define CLONE_SIGHAND 0x00000800u
#define CLONE_THREAD 0x00010000u
#define CLONE_SYSVSEM 0x00040000u
#define CLONE_SETTLS 0x00080000u
#define CLONE_PARENT_SETTID  0x00100000u
#define CLONE_CHILD_CLEARTID 0x00200000u
#define CLONE_CHILD_SETTID   0x01000000u

/* A number no Linux kernel has: the boot asks for it on purpose, so the count of
 * unimplemented calls is seen moving (see [ABI] MUSTBEZERO). */
#define VIBEOS_ABI_PROBE_NR 1999u

#endif
