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
| H-003 | H | futex validates a user pointer, then reads it after a spinning lock | **verified-open** | a ring-0 fault panics. Exception-table attempt reverted - [uaccess_recovery_open.md](uaccess_recovery_open.md). Same fix as H-010 |
| H-004 | H | two region-list heads into one pool across threads | fixed `ffa8463` | [core_c5_process_state.md](core_c5_process_state.md) |
| H-005 | H | exit_group ended only the calling thread | fixed `ffa8463` | same file |
| M-003 (a) | M | signal dispositions copied per thread | fixed `ffa8463` | same file |
| M-003 (b) | M | pipe write tests `readers` outside `g_pipe_lock`; read half worse | **verified-open** | [pipe_eof_race_open.md](pipe_eof_race_open.md). Deferred until C5 settles |
| H-006 | H | exec from a thread left siblings running and the wrong id | fixed `8a3903c` | THREADS_C5_EXEC, red first |
| M-004 | M | tkill/tgkill could not reach non-leader threads | fixed `d6d6b1b` | reported again later; already closed |
| H-007 | H | pid resolved to a slot index, used unlocked (ABA); lookups matched slots being built | fixed `da50ce6` | **no red test**: the window does not reproduce in a boot; stated in the commit |
| M-005 | M | TCP RST accepted without a sequence check; also closed listeners | fixed `b4a65ad` | RFC 5961 3.2; host test red first |
| M-006 | M | mmap `len + 0xFFF` wraps to zero pages; munmap/mprotect aligned end wraps | fixed `a40aab0` | red in the ABI self-test, split-verified |
| H-008 | H | TCP ISN = clock (`0x1000 + now_ms`, listen `0x2000 + now_ms`) | fixed (commit "net: TCP initial sequence numbers come from a secret") | RFC 6528 with SipHash over one stack secret; host test red first. **The secret is weak under QEMU TCG**: no RDRAND, so it comes from the TSC mix and the boot log says so. Real entropy is its own open item |
| H-009 | H | DHCP: predictable xid; ACK not bound to the chosen server, offer or chaddr; renewals from anyone | **verified-open** | after H-008 (shares the secret). Client can narrow, not close: DISCOVER is broadcast |
| M-007 | M | DNS: predictable id, fixed source port 0xC353, no server/port/question check | fixed (commit "net: a DNS answer is believed only if it answers the query sent") | server, port 53, response bit, id and the single question checked; id and port from the stack secret per query. Host test red first - **the first version was red for the wrong reason** (it captured an ARP request) and was corrected and re-proved red at the right step |
| H-010 | H | read() on a pipe, recv(), recvfrom() validate the buffer, block, then write it after wake-up; a sibling's munmap makes it a ring-0 fault -> panic | **verified-open** | `hw_pipe_read` (write inside the blocking loop), `hw_net_recv`, `hw_sys_recvfrom`; **also the console read** (same shape, not in the report). Same class as H-003: needs fault-safe copy_to/from_user |
| M-008 | M | any ARP overwrites the cache entry, gateway included | **verified-open** | `arp_input` calls `arp_insert` unconditionally. ARP has no authentication: limit unsolicited updates, protect the gateway entry |
| M-009 | M | exFAT contiguous file: `first + index` wraps in 32 bits to a valid cluster; `first_cluster` from the entry never range-checked | **verified-open** | `exfat_nth_cluster` (`return first + index`), parse takes `rd32(stream + 20)` as is, `exfat_read_cluster` checks only the wrapped value. **Also, not in the report:** `read_at` truncates `(offset + done) / cluster_bytes` to 32 bits, so a large declared size wraps the index even with a valid first cluster. Host exFAT tests exist, so the red test is deterministic |
| M-010 | M | NTFS run-list: `len_size`/`off_size` up to 15 bytes, shifted `<< (8 * i)` into 64-bit types - undefined behaviour | **verified-open** | `ntfs_next_run` checks only that the bytes fit. **More than reported:** the sign extension `-((int64_t)1 << (8 * off_size))` is already undefined at `off_size == 8`, the largest *legal* size; shifting a byte into the sign bit of `int64_t` is undefined too. The reviewer's second candidate is also real: `vcn < seen + run.length` overflows on a crafted length |
| H-011 | H | NTFS `off + attr_len > size` wraps in 32 bits, so a huge resident `$DATA` value is accepted and `read_at` copies past `rec[]` - **kernel stack contents to user space** | **verified-open, next** | `ntfs_find_attr` (`off + attr_len`), `ntfs_data_attr` (`at + attr_len`); the value checks then trust that length; `rec[VIBEOS_NTFS_MFT_RECORD_MAX]` is a local in lookup, read_at and a third function; `read_at` copies `attr.value[offset + i]`. Host NTFS tests exist. Fix `len > size - off` after `off <= size`, then **audit every filesystem parser for `offset + length > size`** |

Checked by the reviewer and **not** promoted, recorded so nobody re-reports them:
the futex waiter slot is kept until its own waiter returns (the earlier ABA is
closed), and `ARCH_SET_GS` is refused because `%gs` holds per-CPU kernel state.

## Found internally while verifying the above

| What | Status | Evidence / next step |
|---|---|---|
| hw_task_exit makes `next` current with interrupts possibly on; a timer there saves the dying task's kernel frame as next's context | fixed (commit "core: exit switches tasks with interrupts off") | must-be-zero counter `exit_switch_irq_on` red at 4 per boot, 0 with `cli`; twelve boots 12/12. Whether it was the cause of the four-worker crash family and of the running->running transition is **not yet shown** - that family ran about one boot in eight to ten. [boot_repeatability.md](boot_repeatability.md) |
| Boot gate hung 48 minutes on a guest that had panicked | **open** | `wait_for` and `wedge_report` are both bounded; cause unknown. Wedge report kept in `.boot-evidence/wedge-probe-boot5.txt`, boot log lost |
| `rmap_mismatch=1` with `rmap_audit_torn=0` on a Release boot | **open** | by CLAUDE.md's rule a real mismatch, not the detector. `.boot-evidence/probe-20260911-133143-boot3.log` |

## Order of work

Incoming findings first - the user's stated priority - and the core plan is not
dropped:

0. **H-011 first**: it discloses kernel memory, and its fix is a pattern to sweep
   through every filesystem parser, M-009 and M-010 included.
1. H-009 (on the stack secret H-008 added). The exit-window fix, H-008 and M-007 are done.
2. **H-010 + H-003 together**: a fault-safe user copy is the one fix for both,
   and it is also C5's "real exception table" item. The first attempt failed on
   `&&label` losing its base; see uaccess_recovery_open.md before retrying.
3. M-008, M-009 (exFAT), M-010 (NTFS), M-003 (b) pipes.
4. Core plan C5, remaining: fork not atomic against its own threads
   (address-space lock); descriptors per thread; a must-be-zero check for
   `hw_procstate_t`.
5. The two open tooling/defect rows above.
