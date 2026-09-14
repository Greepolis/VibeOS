# Review findings tracker

One place for every external-review finding and its state, so none is lost
between sessions. Updated whenever a finding arrives, is verified, or closes.

**Read the key first.** Reviewers number from 001 in every review, so `H-001`
names different defects in different files. A finding is identified here by
**review + ID**, and the IDs below are the reviewer's own, even where they collide.

Status words: **fixed** (commit named, red test first unless the row says why
not), **in progress**, **verified-open** (confirmed against the code, not yet
fixed), **false** (checked and refuted - evidence linked).

Earlier reviews are closed and written up: the sixth
([review_sixth_verified.md](review_sixth_verified.md)), the seventh
([review_seventh_verified.md](review_seventh_verified.md)) and those before
([external_reviews_verified.md](external_reviews_verified.md)).

## Eighth review (2026-09-10 onward, arriving one finding at a time)

| ID | Sev | Finding | Status | Evidence / next step |
|---|---|---|---|---|
| M-001 | M | brk shrink never unmapped | fixed `1dc5b67` | [mm_brk_mmap_leaks.md](mm_brk_mmap_leaks.md) |
| M-002 | M | mmap failure left pages mapped (both loops) | fixed `1dc5b67` + C5 | second loop found later; same file |
| H-003 | H | futex validates a user pointer, then reads it after a spinning lock | fixed (commit "core: a user copy that faults returns an error instead of panicking") | assembly copy with a recovery point in the trap handler; futex read and the exit join-word write use it. [uaccess_recovery_open.md](uaccess_recovery_open.md) |
| H-004 | H | two region-list heads into one pool across threads | fixed `ffa8463` | [core_c5_process_state.md](core_c5_process_state.md) |
| H-005 | H | exit_group ended only the calling thread | fixed `ffa8463` | same file |
| M-003 (a) | M | signal dispositions copied per thread | fixed `ffa8463` | same file |
| M-003 (b) | M | pipe write tests `readers` outside `g_pipe_lock`; read half worse | fixed (commit "core: a pipe decides end of file under the lock that holds its data") | both decisions moved into the critical section with the data they depend on. **No red test**: the windows are a few instructions between two cores. [pipe_eof_race_open.md](pipe_eof_race_open.md) |
| H-006 | H | exec from a thread left siblings running and the wrong id | fixed `8a3903c` | THREADS_C5_EXEC, red first |
| M-004 | M | tkill/tgkill could not reach non-leader threads | fixed `d6d6b1b` | reported again later; already closed |
| H-007 | H | pid resolved to a slot index, used unlocked (ABA); lookups matched slots being built | fixed `da50ce6` | **no red test**: the window does not reproduce in a boot; stated in the commit |
| M-005 | M | TCP RST accepted without a sequence check; also closed listeners | fixed `b4a65ad` | RFC 5961 3.2; host test red first |
| M-006 | M | mmap `len + 0xFFF` wraps to zero pages; munmap/mprotect aligned end wraps | fixed `a40aab0` | red in the ABI self-test, split-verified |
| H-008 | H | TCP ISN = clock (`0x1000 + now_ms`, listen `0x2000 + now_ms`) | fixed (commit "net: TCP initial sequence numbers come from a secret") | RFC 6528 with SipHash over one stack secret; host test red first. **The secret is weak under QEMU TCG**: no RDRAND, so it comes from the TSC mix and the boot log says so. Real entropy is its own open item |
| H-009 | H | DHCP: predictable xid; ACK not bound to the chosen server, offer or chaddr; renewals from anyone | fixed (commit "net: a DHCP reply belongs to this client's transaction with its server") | xid from the stack secret; server port, chaddr, server id; ACK only from the chosen server for the offered address, renewals for the address in use; **a NAK from anyone used to drop the lease** and now must come from the server. Red twice (as written, and with the xid check off). Not closable by a client: an on-segment attacker answering the broadcast DISCOVER first |
| M-007 | M | DNS: predictable id, fixed source port 0xC353, no server/port/question check | fixed (commit "net: a DNS answer is believed only if it answers the query sent") | server, port 53, response bit, id and the single question checked; id and port from the stack secret per query. Host test red first - **the first version was red for the wrong reason** (it captured an ARP request) and was corrected and re-proved red at the right step |
| H-010 | H | read() on a pipe, recv(), recvfrom() validate the buffer, block, then write it after wake-up; a sibling's munmap makes it a ring-0 fault -> panic | fixed (commit "core: a user copy that faults returns an error instead of panicking") | `hw_pipe_read` (write inside the blocking loop), `hw_net_recv`, `hw_sys_recvfrom`; **also the console read** (same shape, not in the report). Fixed with H-003, same commit: pipe read/write, console read, recv/recvfrom/send/sendto through the fault-tolerant copy (socket data via a kernel bounce buffer). Boot probe asserted by the gate; sabotage of the recovery branch turns it red |
| M-008 | M | any ARP overwrites the cache entry, gateway included | fixed (commit "net: a known neighbour's hardware address changes only by a reply the stack asked for") | an entry changes only through a reply to the request the stack sent; a request for this host may add, never change; stale entries are used and refreshed by a request. Host test red first. ARP has no authentication: racing the real reply to a request is still possible |
| M-009 | M | exFAT contiguous file: `first + index` wraps in 32 bits to a valid cluster; `first_cluster` from the entry never range-checked | fixed (commit "fs: cluster, run and length arithmetic from a volume cannot wrap") | `exfat_nth_cluster` (`return first + index`), parse takes `rd32(stream + 20)` as is, `exfat_read_cluster` checks only the wrapped value. **Also, not in the report:** `read_at` truncates `(offset + done) / cluster_bytes` to 32 bits, so a large declared size wraps the index even with a valid first cluster. Host exFAT tests exist, so the red test is deterministic |
| audit | M | exFAT, ext2, ISO9660 and NTFS `read_at`: `offset + len > node->size` after `offset < size` | fixed (commit "fs: cluster, run and length arithmetic from a volume cannot wrap") | found by the H-011 audit. Reachable only with a size declared near 2^64 and an offset within 4 GiB of it; then `len` is not trimmed and the read runs past the declared end (in exFAT, straight into M-009's index truncation). Fix with M-009/M-010: `len > size - offset` |
| M-010 | M | NTFS run-list: `len_size`/`off_size` up to 15 bytes, shifted `<< (8 * i)` into 64-bit types - undefined behaviour | fixed (commit "fs: cluster, run and length arithmetic from a volume cannot wrap") | `ntfs_next_run` checks only that the bytes fit. **More than reported:** the sign extension `-((int64_t)1 << (8 * off_size))` is already undefined at `off_size == 8`, the largest *legal* size; shifting a byte into the sign bit of `int64_t` is undefined too. The reviewer's second candidate is also real: `vcn < seen + run.length` overflows on a crafted length |
| H-011 | H | NTFS `off + attr_len > size` wraps in 32 bits, so a huge resident `$DATA` value is accepted and `read_at` copies past `rec[]` - **kernel stack contents to user space** | fixed (commit "fs: an NTFS attribute length cannot open its record up") | bounds are `len > size - off` after `off <= size` in `ntfs_find_attr` and `ntfs_data_attr`; host test red first. **Audit of every parser done:** the 8/16-bit record walks (ext2, ISO9660, NTFS index and run headers) cannot wrap; the four read paths below share a weaker form of the same pattern, recorded as its own row |
| H-012 | H | FAT: a first cluster from a directory entry is never range-checked; `fat_cluster_lba` is 32-bit and nothing bounds a sector by the partition - reads of other partitions' sectors | fixed (commit "fs: a FAT cluster from the disk names a sector of its own partition") - **worse than reported, and the write half fixed too** | `fat_cluster_lba` (`data_lba + (cluster - 2) * spc`, uint32) is checked nowhere; only `fat_next_cluster` checks `max_clusters`. The partition's total sectors is a local of the mount, never stored. **Writes too:** a parent directory's cluster comes from `fat_resolve` unchecked, `fat_dir_sector` turns it into a sector with `fat_cluster_lba`, and create, unlink and mkdir *rewrite* that sector with one 32-byte entry changed - a crafted subdirectory turns "create a file here" into a write to a sector of another partition. Data writes use freshly allocated clusters and are not affected. Fixed in `vibeos_fat_cluster_sector` (host-tested, red first): cluster range, 64-bit sector, partition bound; `fat_cluster_lba` returns 0 for a refused cluster and every read, list, directory-sector and write caller stops on it. Twelve boots 12/12 |
| H-013 | H | ext2: inode block pointers never checked against the volume; `blocks_count` not kept after mount, so a crafted pointer reads sectors past the partition | fixed (commit "fs: an ext2 block number names a block of its own volume") | `ext2_read_block` computed `part_lba + block * sectors` with no bound; `ext2_map_block` takes direct and indirect pointers as they come. **Also, not in the report:** `ext2_read_inode` takes the inode table block from the group descriptor unchecked, so a lookup alone can read past the volume. Fixed in the same place: the mount keeps `blocks_count` and `ext2_read_block` refuses any block at or past it. Host test red first, with a volume declared smaller than its device - an image whose volume fills the device would fail at the device and pass for the wrong reason |
| H-014 | H | TLB shootdown timeout returns and the caller proceeds, so a permission restriction "succeeds" while a target core may still hold the old translation | **already fixed - the finding read stale code** (`41fd9af`) | the finding quotes the timeout body (`store 0; tlb_timeouts++; hw_log(...)`) and stops one line early: it is immediately followed by `hw_panic("TLB shootdown timed out: a core still holds a translation the caller has already revoked")`. There is one shootdown implementation (`hw_tlb_shootdown`, the `g_be.shootdown` hook), and on timeout it fail-stops - exactly the finding's recommended fix. An earlier review found the old "return and continue" behaviour; the comment there records the whole reasoning. No live defect |
| H-015 | H | TLB quarantine overflow (>512 parked frames) falls back to an immediate `vibeos_frame_put` - the old racy release - so a frame can be recycled while another core holds a stale TLB entry; a single process can force it by munmapping >512 not-yet-quiescent pages | **verified-open** | `hw_tlbq_put`: the fallback is labelled "status quo: the old, racy release". The boot gate asserts `tlbq_overflow=0` so CI catches it, but that is not a runtime guarantee. Fix: on overflow do a synchronous shootdown for that frame's address space then put, or apply backpressure until a slot is quiescent - not the racy put |
| H-016 | H | `rt_sigaction` and `rt_sigprocmask` dereference the user pointer directly after `hw_user_range_ok` (old/act read+write); a sibling thread's munmap between check and access faults in ring 0 | fixed (`298b68d`) | same class as H-003/H-010 but thread-race, no blocking. `hw_sys_rt_sigaction` (old_uptr writes, act reads), `hw_sys_rt_sigprocmask` (old_uptr write, set read). Fix: `vibeos_uaccess_copy` |
| H-017 | H | `rt_sigaction` stores the handler (`act[0]`) as `sig_handler[sig]` with no canonicality/user check; `hw_signal_deliver` puts it in `frame->rip`, and iretq to a non-canonical rip #GPs in ring 0 -> kernel panic, from ring 3 via rt_sigaction + kill | fixed (`05fa793`, red test) | canonical rip stays canonical, so validate at install: `hw_user_addr_ok(handler)` (rejects non-canonical and wrong-window). A canonical-but-unmapped handler faults in ring 3, which is safe. Red test possible: non-canonical handler + signal panics today |
| H-018 | H | `rt_sigreturn` reads the sigframe with a direct deref after `hw_user_range_ok`, and restores `rip`/`rsp` from the user-controlled frame without a canonicality check -> iretq #GP in ring 0 | fixed (read `298b68d`, rip `05fa793`) | cs/ss/rflags ARE normalised (no privilege escalation), so the live defect is the non-canonical rip/rsp -> iretq fault, plus the TOCTOU read. Fix: copy the frame via `vibeos_uaccess_copy`, validate `restored.rip`/`rsp` canonical+user, SIGSEGV otherwise |
| H-019 | H | `hw_copy_user_string` validates each byte then derefs it directly; a sibling munmap between the two faults in ring 0. Used by execve, open, unlink, mkdir, stat/readlink | fixed (`298b68d`) | `int hw_copy_user_string(uptr,dst,max)`: `hw_user_range_ok(uptr+i,1,0)` then `dst[i]=*(uptr+i)`. Fix: fault-safe per-byte `vibeos_uaccess_copy`, keep `hw_user_range_ok` as the address-policy check |
| H-020 | H | `readv`/`writev` validate the whole iovec array once, then deref `v->len`/`v->base` per element; a sibling munmap of the array page faults in ring 0 | fixed (`298b68d`) | `hw_sys_writev`/`hw_sys_readv`. Fix: copy each `hw_iovec_t` via `vibeos_uaccess_copy` into a kernel struct before reading base/len |
| M-017 | M | signal 64 is accepted (`sig >= VIBEOS_HW_NSIG` where NSIG=65) but `1ull << sig` with sig=64 is undefined; the pending/blocked bitmask is 64-bit keyed by signal number, so signal 64 has no representable bit and can stay pending undelivered | **verified-open** | `hw_signal_raise` (`1ull << sig`), `hw_signal_next`, the KILL/STOP masks. Fix: one coherent representation - either cap at 63 representable, or key bits by `sig-1` (bits 0..63 for signals 1..64) across raise/next/sigset conversion |

**H-015..H-020 and M-017 arrived 2026-09-14, one at a time, all verified against
the code.** Four of them (H-016, H-018-read, H-019, H-020) are the "dozens of
validate-then-use sites" `uaccess_recovery_open.md` already flagged, now being
enumerated: one fault-safe-copy pass fixes them. H-017/H-018-rip are one theme
(a ring-3-controlled RIP reaching iretq); M-017 and H-015 stand alone.

Checked by the reviewer and **not** promoted, recorded so nobody re-reports them:
the futex waiter slot is kept until its own waiter returns (the earlier ABA is
closed); `ARCH_SET_GS` is refused because `%gs` holds per-CPU kernel state; and
`hw_sys_lseek` truncates the offset to 32 bits (the finding's own second item,
withheld by its author for want of a security chain - it stays here, unpromoted,
with the other validate-then-use sites).

## Found internally while verifying the above

| What | Status | Evidence / next step |
|---|---|---|
| hw_task_exit makes `next` current with interrupts possibly on; a timer there saves the dying task's kernel frame as next's context | fixed (commit "core: exit switches tasks with interrupts off") | must-be-zero counter `exit_switch_irq_on` red at 4 per boot, 0 with `cli`; twelve boots 12/12. **It was not the cause of the four-worker crash family, or not the only one: the same signature recurred after the fix** (`.boot-evidence/fail-20260911-154006-boot7.log`, rip `0x405b72`, free-page-poison panic). The window was real and stays closed; the family is open again. [boot_repeatability.md](boot_repeatability.md) |
| THREADS four-worker crash family (new thread faults in musl `start`, stack is the free-page poison) | **open - and the probe masks it** | with probes in the kernel: 0 crashes in 70 boots; without: 2 in 36 (`probe-20260911-174620-boot1`, `-boot35`). The quiet probe logged nothing and still masked it; what it kept was a page walk and a user read in `hw_task_load_cpu_state` when a thread starts from its initial context - so the window is at a thread's first start. The gate now snapshots every core from the QEMU monitor the instant the poison marker appears - no guest-side work (commit "gate: snapshot every core at a THREADS thread-fault"). Read the snapshot on the next crash. [boot_repeatability.md](boot_repeatability.md) |
| Boot gate hung 48 minutes on a guest that had panicked | fixed (commit "gate: the serial pump returns while a guest is still printing") | the serial pump looped until the socket was empty, so a guest printing without pause - faulting in its own panic path - kept it from returning, and the deadline and silence budget, checked outside it, never ran. `drain_available` returns after 0.25 s; its self-test hangs with the budget removed |
| `rmap_mismatch=1` with `rmap_audit_torn=0` | **partly fixed, recurred - reopened** (commit "mm: fork's audit counts the references the TLB quarantine holds") | an unmap removes the holder at once and the owner only when the quarantine drains, so a still-mapped frame has one owner more than holders, with the owner count still. The audit now adds references the quarantine actually holds; any other disagreement still counts. Host test red first. **But `rmap_mismatch=1` with `torn=0` recurred on a kernel that includes this fix** (`probe-20260911-200632-boot9`), so the quarantine was a real partial cause and not the whole one. Open. Next: when the audit finds a mismatch, log the frame and its holders, owners and quarantined count, so the disagreement is described rather than only counted |

## Order of work

Incoming findings first - the user's stated priority - and the core plan is not
dropped:

0. H-011 is done, and its audit found the read-path row above - to be fixed with
   M-009 and M-010 as one filesystem-bounds change.
0b. H-012 is done.
1. The network findings H-008, M-007 and H-009 are done, and so is the exit-window fix - which did not end the four-worker crash family; that family is open again.
2. H-010 + H-003 are done - and with them C5's "real exception table" item.
3. Every review finding is closed. What remains open is internal: the four-worker crash family.
4. Core plan C5, remaining: fork not atomic against its own threads
   (address-space lock); descriptors per thread; a must-be-zero check for
   `hw_procstate_t`.
5. The two open tooling/defect rows above.
