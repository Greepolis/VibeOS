# The user-access recovery

**Status: FIXED (2026-09-11)** - H-003 and H-010. The first attempt, and why it
failed, is kept below: it is the reason the second one looks the way it does.

## The defect

Validating a user pointer and dereferencing it are two instants, and nothing
makes the pair atomic: `hw_user_range_ok` walks the page tables, and a sibling
thread on another core can munmap the address before the read happens. A
ring-0 page fault panics, so an unprivileged multi-threaded process could stop
the machine by racing against its own memory.

H-003 found it on the futex path, where `g_futex_lock` can spin between the
check and the read. H-010 found the wider shape: a syscall that validates its
buffer and then **blocks** - pipe read and write, the console read, socket
receive - leaves the window open for as long as it sleeps.

## The fix

`kernel/arch/x86_64/uaccess.S` holds `vibeos_uaccess_copy(dst, src, len)`: one
`rep movsb`, bracketed by `vibeos_uaccess_copy_begin` and `_end`, and a
recovery point `vibeos_uaccess_copy_fixup` that returns -1. When the trap
handler takes a ring-0 page fault that copy-on-write did not resolve, at a rip
inside that range, on a user address, it resumes at the recovery point. The
stack is as the fault left it - inside a leaf, before any push - so `ret`
returns to the caller with the error. A kernel address faulting there still
panics.

Every site the two findings named goes through it, and a few they did not:

- futex: the word compared under `g_futex_lock`;
- exit: the join word written to `clear_child_tid`;
- pipes: read and write, in contiguous runs of the ring buffer, consuming or
  producing only what was actually copied;
- the console read, per character;
- sockets: `recv`, `recvfrom`, `send`, `sendto`. The portable stack writes
  straight into whatever pointer it is given and cannot use an arch-layer
  routine, so data goes through a kernel bounce buffer under `g_net_lock`.

Other validate-then-use sites remain - there are dozens - and each has the same
theoretical window against a concurrent munmap. They do not block, so the
window is a few instructions; they are the next place to apply the routine,
not a claim that the class is gone.

## How it is known to work

Bring-up forces a copy from the top of the high user window, which nothing
maps, before any task runs, and prints `uaccess recovery ok` if it came back as
an error. The boot gate asserts the line (`uaccess_recovery_not_proven`).

**The first red was not a red.** On the unfixed kernel the probe used a plain C
copy into a local nothing read afterwards; the compiler removed it, and the
probe reported a *successful* read of an unmapped address. The gate went red,
for a reason that had nothing to do with the defect - exactly the failure the
previous attempt's "three green boots" had been. The real red is a sabotage on
the fixed kernel, the recovery branch switched off: the probe's copy takes a ring-0 page fault (vector 0xe, cs 0x8, rip 0x4000413 inside uaccess.S) and the gate reports guest_panicked (.boot-evidence/sabotage-uaccess.log).

Fixed: `uaccess recovery ok`, `check.sh all` green, twelve boots 12/12.

`check-chokepoints.py` moved `hw_user_addr_ok` from 2 to 4 call sites - the
handler asks it before resuming - and the count was updated with that reason.
It also showed that `.f.sh` printed that check without asserting it; it asserts
every check it prints now.

## The first attempt, and why it did not work

An exception table with one entry per core: `hw_user_read32` recorded a
recovery point with the GNU address-of-label extension, and the trap handler
rewrote `frame->rip` to it. The recovered `rip` was `0x000000aa020` with the
kernel linked at `0x4000000`: under this freestanding link `&&label` produced a
section-relative offset, and the handler resumed at an unmapped low address.
Assembly symbols are ordinary relocations - the same kind every function
pointer in the kernel is - which is why the fix is assembly.

That attempt measured 3/3 green before a probe existed, because its only caller
always had the page mapped. A probe that runs from bring-up turned it into a
`#UD` in one run; the first version of the probe ran from `vibeos_kmain`, after
userland, with a CR3 that could belong to a torn-down address space, and the
machine went silent instead. Bring-up, in the kernel's own address space, is
where a probe like this belongs.
