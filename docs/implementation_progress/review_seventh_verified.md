# Seventh review: an archetype, checked against the tree anyway

A full audit with scores — Security 3.0/10, Syscalls "BROKEN", one CRITICAL
allowing arbitrary kernel memory overwrite. Its author later confirmed, asked
directly, that the file paths and line references were **reconstructed from what
experimental x86-64 kernels usually look like**, not read from this repository.

That admission matched the measurement. Nine of the ten paths it cites do not
exist here:

```
absent  kernel/sys/syscall.c          absent  kernel/fs/elf.c
absent  kernel/mm/cow.c               absent  kernel/sched/sched.c
absent  kernel/mm/vmm.c               absent  kernel/arch/x86_64/sched/context.S
absent  kernel/net/ipv4.c             absent  kernel/net/udp.c
absent  kernel/sync/spinlock.c        EXISTS  kernel/mm/pmm.c
```

A wrong location does not make a claim wrong — the sixth review was right about
the mechanism with the line numbers pointing at the wrong function. So every
finding was checked against the code that is actually here.

## Verdict, finding by finding

| | claim | verdict | evidence |
|---|---|---|---|
| C-001 | syscalls dereference user pointers unchecked | **false** | `hw_user_range_ok` has 44 call sites; `hw_user_addr_ok` decides the two-window address policy; `hw_user_range_why` names the refusal. `check-chokepoints.py` fails if any count moves **in either direction** |
| C-002 | COW refcount is neither atomic nor locked | **false in mechanism** | `owners` is 16-bit; `vibeos_frame_get/put` take `frame_lock()` around the `_locked` variants. The double-decrement it describes is separately prevented by compare-exchange on the PTE — the store decides who owns the release |
| H-001 | no IPI shootdown on unmap | **true, and fixed** | it is the sixth review's finding, closed the same day by deferred reclamation. Its second half is false: fork *does* shoot down |
| H-002 | `p_vaddr + p_memsz` integer overflow in ELF | **false** | `kernel/core/elf.c:242` — `add_ok(vaddr, memsz, &mem_end)` is a checked addition, and the next line is `vaddr < min_allowed \|\| mem_end > end_allowed`. Literally the recommended fix, in the two lines it recommends |
| H-003 | spinlocks taken without disabling interrupts | **false** | `hw_spin_lock` begins `pushfq; popq flags; cli`. It is the irqsave variant by construction; there is no other variant |
| M-001 | FPU/SSE not saved across context switch | **true, and fixed** | it was true for the whole life of this kernel and was fixed four days before this review, in `5aa68ef` |
| M-002 | IPv4/UDP trust declared lengths | **false** | UDP: `if (len < 8u) return;` then `if (dlen < 8u \|\| dlen > len) return;`. IPv4: `if (hdr < IP_HDR \|\| total < hdr \|\| total > len)` → drop. Both compare declared against actual |

Two of seven were real. Both were already fixed, one of them earlier the same
day because a *different* review found it.

## The part that is worth more than the score

**M-001 was a genuine, serious defect that no other review found**, and this one
named it: `fxsave`/`fxrstor` appeared zero times in the entire tree while the
kernel contained 6,955 XMM instructions and had `CR4.OSFXSR` set. It explained
sixteen-byte corruption appearing three boots in ten, and white pixels in argv
that turned out to be `COL_TITLETXT` left in a register by the GUI painting from
an interrupt.

So the reviewer's *instincts about where this class of kernel breaks* are good.
Four of the seven archetypal concerns are exactly the ones this project has
actually had at some point: the FPU state, the COW refcount race, the TLB
shootdown, and the user-pointer boundary. Three of those four were closed before
the review arrived; the fourth was closed by the review before it.

That is a real result, and it is the argument for keeping these reviews coming.

## The part that would have cost, had it been believed

The scores rest on findings that are false here. "Syscalls BROKEN", "Security
3.0/10" and "assenza di isolamento Ring 3 → Ring 0" all follow from C-001, which
is refuted by a guard with 44 call sites and a check that fails when the count
changes. "Filesystem: STUB / RamFS, manca persistenza reale" describes a tree
that has FAT, ext2, NTFS, exFAT and ISO9660 behind a registry, a block cache, a
partition table writer and a journal. "Nessuna protezione contro malformed
packets" is refuted by the two length checks quoted above.

A number attached to a false premise is more dangerous than the premise alone,
because the number survives being quoted while the premise does not.

## What to hand a reviewer next time

The honest brief, since the archetype was reasonable and simply aimed at the
wrong tree:

- **Read `kernel/arch/x86_64/arch_hw.c`.** It is 10,000 lines and it *is* the
  kernel. Every security defect this project has actually had was there.
- `kernel/core/` and `kernel/proc/` **look** like the kernel and are not
  reached from ring 3. Four consecutive reviews have been lost in them; the
  files now say so in a banner, and the largest of them has been deleted.
- The interesting question is never "does a check exist" but "is it on the path
  the hardware takes". `check-reachable.py`, `check-chokepoints.py` and
  `docs/core/` exist to answer that, and their answers are in the repository.
