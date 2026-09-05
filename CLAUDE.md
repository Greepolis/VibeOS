# Working on VibeOS

Notes for whoever picks this up next, including me. Facts that were expensive
to learn and are not visible from the code.

## Build and verify

Everything goes through `scripts/dev/` (see its README). The short version:

```bash
bash scripts/dev/check.sh all build-gcc-Release   # build, host tests, boot
bash scripts/dev/repeat-boot.sh build-gcc-Release 6
bash scripts/dev/trace-linux-binary.sh /usr/bin/busybox ls -1
python3 scripts/dev/catch-hang.py build-gcc-Release 5
python3 scripts/dev/screenshot.py build-gcc-Release screen.ppm 60
```

Builds live in `build-<compiler>-<config>`. On Windows, run them through WSL:
`wsl -d Ubuntu -e /bin/bash -c "cd /mnt/c/.../VibeOS; ..."`. The default WSL
distro here is docker-desktop, so `-d Ubuntu` is not optional.

## Rules that were learned the hard way

**Verify under the same configuration CI uses.** Roughly twenty clean local
runs once validated a KVM-accelerated setup while CI ran pure TCG. Forcing
`-accel tcg` reproduced the failure in two runs out of three. Every dev script
here forces TCG for that reason.

**One clean boot proves nothing.** A lost TLS base, two CPUs on one task, and a
console deadlock each reproduced about one boot in three. Use `repeat-boot.sh`.

**Break the test before believing it.** Several "fixes" here were verified by
sabotaging the thing they fixed and confirming the gate went red.

**A green build is not a build.** Twice, a failing compile left a stale image
and the boot "passed" on the previous binary. `check.sh` prints the return code
first for this reason.

**A failed boot is evidence, not a score.** The instinct is to re-run and
compare counts; that is the slowest possible way to learn anything here, and it
has been done repeatedly. Every failed boot already leaves a trap dump with
`err`, `rip` and `cr2`, a crash record naming the program, the task
counters, `wedge_report` output in the summary, and the serial log kept by
`repeat-boot.sh`. Read those first. One failing log has repeatedly said more
than twenty-four boots of pass/fail.

A worked example: a phase measured 17/24 against a parent's 21/24 and was
treated as a possible regression. One line of the failure log - `err=0x14`,
`cr2 == rip`, a user instruction fetch on a missing page - identified it as the
*pre-existing* defect already written up in
`docs/implementation_progress/boot_repeatability.md`. The count said "maybe a
regression"; the log said "the known bug, again".

**Never delete the evidence before reading it.** `rm -f repeat-fail-*` at the
start of a run is convenient and has twice destroyed the logs of a failure that
then had to be reproduced. Read them, or move them aside; do not clear them
because the next run is about to start.

**Never rebuild while a boot run is in flight.** `repeat-boot.sh` does not hold
the binary it is testing, so a rebuild half-way through means the run measured
two different kernels and its number means nothing. This has invalidated three
separate runs, each about half an hour.

**Check a criterion against the baseline before using it to judge a change.**
The memory manager's P2 was declared unfinished for a day and a half against
"48 boots with no failure", a number no revision of this project has ever met -
the machine has a pre-existing intermittent failure of roughly one boot in
eight, so the criterion measured the background rather than the phase. One
24-boot run of the parent commit settles it, and `scripts/dev/bisect-boot.sh`
exists to make that cheap.

**Read the CI log before theorising.** A hang I diagnosed twice from the wrong
end was named outright in the CI output: glibc printed the failing assertion.

**A panic prints a backtrace now.** `hw_backtrace` walks the frame pointers
and prints raw return addresses; `scripts/dev/symbolize.py` turns a serial log
into function names and source lines with addr2line. The addresses are raw on
purpose - nearest-preceding-symbol naming against a live guest is frequently
wrong here, and a confidently wrong name is worse than a number. The kernel
image is built `-fno-omit-frame-pointer -g` for this; the debug info never
reaches the guest.

**Instrument rather than infer.** `catch-hang.py` resolves a hung core's RIP,
but almost everything in `arch_hw.c` is `static`, so nearest-preceding-symbol
names are frequently wrong. When it matters, make the kernel print where it is.

**A panic must stop the machine, not a core.** `hw_panic` ended with
`for (;;) { cli; hlt; }`, and its own message said `halting on cpu 2` - which
was read for weeks as though it said `halting`. The other three cores carried
on, the log kept moving, and the guest went quiet seconds later somewhere with
no connection to the fault. That is the whole silent-wedge family, and it is why
so many investigations here ended at a plausible pointer with no mechanism
behind it: the evidence and the failure were separated by however long it took
the survivors to need the core that had died. A panic now parks every core on
its next timer interrupt, and the boot gate reports `guest_panicked` with the
reason rather than `guest_wedged`.

**A field written on one path and read on all of them will leak whatever was
there.** `p->interp_base` was set only when an interpreter was mapped and read
unconditionally when the startup block was built, so every program without an
interpreter was handed a stale kernel pointer as `AT_BASE` - uninitialised
kernel stack for an execve, the previous tenant's value for a recycled slot. A
static PIE relocates itself against `AT_BASE`, so musl dereferenced it. Clear
such a field in the function that owns the contract, not at each caller.

**Symbolise against the artifacts, not against a local rebuild.** The nightly
uploads the kernel ELF and every guest binary, which is the only way to follow
this file\'s own rule about naming an address - a local build has different
layout and the guest binaries are stripped. Two defects here were found in
minutes from an artifact bundle after days of local sweeps said nothing.

**A layer that is serialised by accident is not serialised.** `backing.c`
had no lock at all, and it did not matter for months because the page cache had
exactly one caller and that caller ran under `g_exec_lock`. Adding a
second caller turned a table with linear probing and a clock hand into a
concurrent one, and two cores placing entries at once left one entry's frame
beside another entry's key - so a lookup *hit* and returned the pages of a
different file. From outside it looked like a program handed somebody else's
text, and it took four attempts to find, three of which produced confident wrong
answers. When adding a caller to a layer, ask what was holding it together
before, not only what the layer does.

Fourth time this project has needed a `set_lock`: frames, the region
pool, and now the page cache. If a layer has mutable statics and more than one
possible caller, it locks itself - "remember to hold the lock" is not a property
a compiler checks. And give it **its own** lock: the cache allocates frames, so
handing it the frame layer's lock would have deadlocked on the first miss.

**The count was never the argument.** Mapping image pages from the page cache
measured 21/24 twice against an "off" baseline reported as 24/24 - which was
itself wrong, because the same baseline also measured 23 and 22. Two boots of
difference proves very little. What separated the two states was that a specific
fault appeared with the change and never without it: BusyBox, the same rip, the
same fault address, byte-identical twice. When comparing two states, look for a
signature that is present in one and absent in the other before reaching for the
ratio.

**A gcc build is not a build either.** gcc accepts an implicit function
declaration with a warning; clang rejects it. A file lifted out of
`arch_hw.c` that forgot `#include "vibeos/arch_x86_64.h"`
compiled green locally and failed the CI job that uses clang. `check.sh`
now builds the kernel with clang too, and breaking the include again reproduces
the CI errors exactly.

The tell was there and was read as noise: the local warning count moved from 0
to 4. A warning count that moves is the build telling you something. Treat
`warnings=` the way this file already says to treat `rc=`.

**A lock does not remove a wait, it shares it.** virtio-net's transmit spun up
to fifty million times for a completion - always far longer than the boot gate
waits for output, and for years that only stalled the core doing the
transmitting. Putting the queue behind a lock, which it needed, turned one stuck
transmit into every core parked in `hw_spin_lock` behind it. The wedge
report showed exactly that: one core in the driver, three queued.

When adding a lock around something that already waited, look at what the wait
costs now that others are behind it. The bound is two million now - still orders
of magnitude of headroom, and a failed frame instead of a failed machine.

**Section banners in `arch_hw.c` describe where somebody stopped
writing, not what follows.** The banner "delivering a signal" covers signals,
the memory-diagnostic walk, the panic summary, the crash dumper and the whole
Linux syscall table - 915 lines, of which the signal code is 185. Measure a
section by its functions before planning around its size.

## Sharp edges in the code

**Syscall arguments typed `int` arrive zero-extended.** `mov $-100, %edi`
delivers `0x00000000ffffff9c`. Read them through `VIBEOS_ARG_INT()`; comparing
all 64 bits against `AT_FDCWD` or `-1` never matches.

**Linux `sigset_t` bits are numbered from zero**: bit 0 is signal 1. This
kernel numbers them by signal. Convert only at the boundary
(`hw_sigset_from_user` / `hw_sigset_to_user`).

**A wait status is not an exit code.** Exit code in the high byte, signal
number in the low seven bits. `128 + sig` is what a shell prints, not what the
kernel stores.

**Copy-on-write touches three places that do not look related**: the fault
handler must accept kernel-mode writes (the kernel writes user buffers with
CR0.WP set), `hw_user_range_ok` must treat a COW page as writable, and a second
fork must preserve the COW bit rather than treating the page as plain
read-only.

**Two user windows exist.** VibeOS programs link at `0x8000000000`; Linux
programs link at `0x400000`, inside the kernel's identity map. The low window's
physical range is reserved out of the allocator so nothing of the kernel's can
live where a process shadows it. Address policy lives in `hw_user_addr_ok`.

**`argv[0]` is not the path.** BusyBox becomes twenty commands by looking up
the name it was invoked under, and FAT stores names upper case with an
extension. The shell supplies the name; the path only opens the file.

**Spinlocks mask interrupts.** Anything slow under one is slow with the timer
off. The 2 MB FAT read under `g_exec_lock` was indistinguishable from a hang
until the block driver learned multi-sector transfers.

**A failed FAT lookup used to look like the end of a file.** `fat_next_cluster`
has to return a cluster number, so it reported a failed table read as the
end-of-chain marker - the same value a healthy last cluster returns - and the
reader then returned the size the directory claimed. A single flaky sector
therefore produced a short file that said it was complete, and execve parsed
whatever the previous program had left in the shared staging buffer.
`g_fat_chain_error` and a byte count that reflects what was actually copied
are what stop that; do not go back to returning the declared size.

**One virtqueue, one request struct, no lock.** `virtio_blk_rw_n` used a
single global header, status byte, descriptor chain and used-index for every
request. Two cores reading at once did not race over a window, they overwrote
each other: one core's sector number landed in the other's request, so a read
returned somebody else's data and succeeded. And whichever core took the used
index first left the other spinning on a completion already consumed. It has
a lock now, with interrupts left on - nothing in an interrupt handler touches
the disk.

**The intermittent boot wedge was exit announcing before it tore down.**
Found and fixed. Roughly one boot in three used to go completely silent; the
fix took sixteen boots with one unrelated failure, and putting the old ordering
back reproduced it immediately with the same signature. Worth reading if a
similar silence ever returns, because almost nothing about it was visible from
the code.

`hw_task_exit` used to set the dying task ZOMBIE and wake its parent *first*,
and destroy the address space several steps later. Between those two, the
parent can reap the slot - that is what a zombie is for - and a fork on another
core can take it and build a new address space in it. The late teardown then
frees the *new* tenant's page tables. The new process keeps running on a CR3
whose top-level page has gone back to the allocator, and `hw_free_page` stores
its freelist link at offset zero of the page it reclaims, which in a PML4 is
the kernel's own entry. The next core to install that CR3 stops on the
instruction that loads it, with no kernel mapped to fault from - so no panic,
no output, nothing. Exit now tears down first and announces afterwards, and
clears `cr3` while it is at it.

The same shape was fixed in the reaper: it published a slot as FREE and then
kept writing to the task. Publish last, take what you need first.

How it was actually found, since three careful readings of the code were wrong:

1. A guest that has stopped cannot report on itself, so ask QEMU's monitor.
   `scripts/dev/wedge_report.py` runs on every wedge and gives per-core RIP,
   CR3 and a frame walk done from the host, symbolised with addr2line.
2. That put a core on the CR3 write in `hw_task_load_cpu_state` every time,
   holding a PML4 with no kernel in it. A guard there turned the silence into a
   named panic.
3. The guard was widened until it stopped being ambiguous: task state, whether
   the slot had been recycled, where the cr3 was last set, and finally which
   caller last destroyed which address space. That last field named
   `hw_task_exit` and printed a page identical to the live task's cr3.

Keep the guard. It costs one read per context switch and it is the difference
between a machine that stops and a machine that says why.

**Anything the kernel writes on a task's behalf must happen before the
context switch.** exit clears the word a joiner sleeps on; that word lives in
the dying task's address space, so once exit has picked the next task and
loaded its CR3, the write goes somewhere else and the range check refuses it
for the honest reason that there is no current user task. pthread_join then
waits forever on a wake the kernel politely declined to send. The range check
says which of its reasons it hit now - `hw_user_range_why` - because "not
writable" without a reason is a dead end.

**Two log lines are not one fact.** A diagnostic split across two hw_log calls
came back interleaved from different cores and read as a contradiction: a page
table walk that found nothing wrong, printed next to a refusal. Anything meant
to be read together has to be written in one call.

**Threads work; the console lock is why finding out took so long.** Neither
`hw_log_emit` nor the `read()` echo path held it, so lines from two cores
interleaved mid-word - which split the very markers the boot gate matches on
and reported failures that had not happened. Twice I measured instability that
was really corrupted output, and once I measured a stale image because I tailed
`check.sh` to two lines and cut off the `rc=1`. Sixteen boots out of sixteen
pass with four threads running.

**A PROT_NONE mapping is a mapping, not a refusal.** A C library builds a
thread stack by asking for stack plus guard as one PROT_NONE region and then
mprotecting the usable part - so refusing PROT_NONE makes pthread_create fail
before it ever reaches clone(). The pages are allocated and mapped without
PTE_USER, which faults from ring 3 exactly as a guard should. Reserving address
space without backing it was tried first and could not be told apart from a
range munmap had just freed: both are "unmapped inside the arena", and mprotect
began accepting an address the ABI self-test requires it to refuse.

**A line in the serial log is not a check.** The ring-3 ABI self-test printed
"abi: mmap/mprotect/munmap wrong" for an entire session while the boot gate
stayed green, because the line was collected into the summary and never
asserted on. It is asserted now, and the assertion was confirmed to go red by
removing the check it protects.

**A weak symbol only works in the same file as its caller.** The Windows CI
job builds the portable core with mingw, and PE/COFF does not resolve a weak
definition living in a different object from the code that calls it: the Linux
build linked and said nothing, the Windows one failed with `undefined`
`reference`. Every weak stub that works in this kernel - in `serial.c`,
`kmain.c`, `usage.c` - happens to sit in the same translation unit as
its caller, which is why the pattern looked safe when it was copied. Use a
registration function instead, as `vibeos_frame_set_lock` and
`vibeos_task_view_set_source` do: it is portable, and it says who supplies
what rather than leaving it to link order.

**Definition order bites repeatedly.** This is one 5000-line C file; a helper
used above its definition compiles as an implicit declaration and then fails
with a confusing "static declaration follows non-static declaration".

**A service that exits is not a service that crashes.** The supervisor's
manifest had a service that exits zero and one that exits non-zero every time,
which covered clean stops and bounded restarts, and the boot gate asserted all
of it. What none of them did was fault. A cooperative death never reaches the
trap handler, so "the kernel stays up after a service crashes" was gated, green
and false: the trap handler panicked on *any* fault, under a comment saying
KILL_CURRENT had no meaning because there were no user processes on metal. That
had been true when it was written. One null dereference from any unprivileged
program halted the machine.

It surfaced sideways. A boot wedged in the BusyBox phase; the trap dump gave a
#GP at a ring-3 address, and addr2line against the right binary put it on the
`hlt` inside musl's `__stack_chk_fail` stub - a stack-check failure taking the
whole machine down instead of one process. (Against the *wrong* binary it named
`fflush`, confidently, because every Linux program here links at 0x400000.
Check which program the task was running before believing an address.)

The fix is one branch - a ring-3 fault kills the task with the signal Linux
reports for that vector - and the service that proves it, `svc-crash`,
dereferences null. Removing the branch turns the boot from green to wedged,
which is how it was confirmed.

That fix then exposed one in init: a task killed by a signal carries the signal
in the low seven bits of the wait status and leaves the exit-code byte zero, so
an init that reads only the code byte reports a segfault as a clean stop. The
crashing service came back STOPPED. The wait-status rule above is not trivia.

**A sabotage run whose verify script is missing scores every case red**, which
is indistinguishable from every case working. The verify script lived in /tmp,
WSL cleaned it, and eight cases "passed" having proved nothing - the tell was
that the reason lines had gone quiet. It lives in `scripts/dev/verify-boot.sh`
now, it prints the gate's own reason, and a sabotage run should start by
checking that the unmodified tree still passes.

**The driver nobody runs is the one that ships broken.** The kernel could
only talk to virtio-blk, which is what QEMU offers and no desktop hypervisor
does. The VM images imported, booted, and then every exec failed - the files
were on the disk and there was no way to read them. UEFI does the reading up to
ExitBootServices, so the bootloader worked and hid it, and what was left was an
absence rather than a failure, which is quieter. CI ran one controller, so the
gap was invisible for as long as nobody tried the artifact by hand.

There is an AHCI driver now, and the smoke test takes `VIBEOS_SMOKE_DISK=ahci`
so both controllers are gated. Match a controller by PCI class, not by device
id: VirtualBox emulates an ICH8, QEMU an ICH9, VMware something else again.

Two of its five sabotage cases turn the boot red. The other three - the AHCI
mode bit, a PRDT count that must be one less than the byte count, and marking
the register window uncacheable - are requirements on real hardware that QEMU
does not enforce, so they are correct and unverifiable here. They are kept in
the case file as a comment saying so, because "no case exists" and "the case
exists and this environment cannot tell" are different things.

**Drive the real tool rather than reasoning about its format.** Three separate
defects kept the OVA from importing into VirtualBox, and all three were found
by running VBoxManage: making it write a .vbox settled where StorageControllers
belongs, and exporting an appliance from it revealed that vbox:uuid on the Disk
is written bare while the machine's Image uuid is written in braces. No amount
of reading the OVF specification would have produced that asymmetry. Also:
uuids sliced out of a SHA-256 digest are not uuids - VirtualBox parses the
malformed value to null instead of rejecting it, and then reports that an image
it can see does not exist.

**An unmatched unlock hands away a lock this core does not hold.**
`hw_log_emit` ended with `vibeos_x86_64_serial_unlock()` and never took the
lock. Two defects came out of that one missing line, and the second is far
worse than the first.

The log line itself was interleavable, because `serial_puts` and
`serial_print_hex` each lock and release on their own - a line assembled from
eight of them is eight critical sections, not one. That is the ordinary bug.

The real one: `__sync_lock_release` on a lock held by *another* core freed it
mid-line, a third writer walked in, and `irq_restore` re-enabled interrupts
inside somebody else's critical section. Output interleaved mid-word and split
the very markers the boot gate matches on, so the gate reported failures that
had not happened - about one boot in fourteen. That is the most expensive kind
of bug to chase, because every piece of evidence it produces is a lie: two of
the crashes investigated before finding it were not crashes at all, just a
marker cut in half.

If a lock is recursive and tracks an owner, count the lock and unlock calls per
file. `grep -c` found this in one command after three sessions of reasoning
about it did not.

**A core that cannot say who it is will claim to be core 0.**
`vibeos_x86_64_cpu_id` read the per-CPU pointer out of KERNEL_GS_BASE and
returned 0 when that MSR was still zero - which it is on every core until its
per-CPU area is installed. So several cores answered "cpu 0" at once and the
console lock's `owner == me` recursion check believed them. The initial APIC id
from CPUID leaf 1 is unique and valid from the first instruction.

**boots.sh does not rebuild.** Sixteen green boots once "confirmed" a fix whose
source had never been compiled. Build first, and check `rc=` - the same trap as
`check.sh`, wearing different clothes.

**A refcount that several cores touch has to be atomic.** The
copy-on-write frame counters were `(*slot)++` and `(*slot)--` on a byte, called
from every core: two processes forking at the same moment share frames their
common ancestor left copy-on-write, while a teardown elsewhere decrements the
same byte. A lost increment leaves the count one owner short, and one owner
short means the frame is freed while somebody is still running from it. From
outside that is a musl program tripping over its own malloc free list, about
one boot in thirty-two, in a different program each time.

Compare-exchange now, saturating at 255 rather than wrapping: a frame that is
never reclaimed is a leak you can measure, and one freed early is not.

**Symbolise against the binary the faulting task was actually running.** The
same fault was attributed to three different things before that rule was
followed: `fflush` (wrong binary), then argv/auxv construction (right binary,
but the address was read as if it were in startup code), and finally
`nontrivial_free` - musl's own free list, which is a completely different bug
from the first two guesses. Every Linux program here links at 0x400000, so an
address alone identifies nothing.

**invlpg is one core's opinion.** `hw_share_user_leaf` revokes write
permission to mark a page copy-on-write and then invalidated the calling core's
TLB - which is complete only if no other core is running that address space.
Threads make that false: a thread of the same process on another core keeps a
cached writable entry and writes straight through into the page the child has
just been given a share of. No fault, no copy, and the damage appears much
later in whatever program the page ends up serving.

fork sends an IPI (vector 0xFE) and waits for each target to reload CR3 - once
per fork, not once per page. The wait is the point: when it returns, nobody can
still be writing through the permission just revoked.

Send it only to the cores running that address space. Broadcasting to all of
them timed out about three boots in thirty-two, and the reason is worth
remembering: `syscall` clears IF (SFMASK is 0x200), so a core inside a syscall
cannot answer an IPI until it returns to ring 3 - and with this much serial
output, that is most cores most of the time. A single-threaded fork now sends
nothing at all, which is nearly all of them.

The copy-on-write fault needs it too, and for a different reason: the copy
changes the page's *physical address*, so a thread on another core keeps the
old frame and silently stops sharing memory with the rest of its process. That
one hangs rather than corrupts - a worker spinning on a flag never sees the
store that sets it.

It has a second half that is easy to miss. Once one core resolves the
copy-on-write fault, another thread can still fault on a stale entry for a page
that is now plainly writable. The fault handler saw no COW bit and let the task
be killed for a violation it had not committed, so it has to recognise the
already-writable case and just invalidate.

**Gate the mechanism when you cannot gate the bug.** A corruption that appears
one boot in thirty cannot be asserted on in a single boot, and every earlier
claim here that it was "fixed" rested on a handful of green boots. What a boot
*can* assert is that fork told the other cores at all: `COW_STATS` carries
`tlb_shootdowns` and `tlb_acks`, and the gate fails if the count is zero, if
acknowledgements come back short, or if a shootdown timed out. A shootdown that
silently never fires leaves the bug exactly as it was, with every boot green -
which is the failure this counter exists to make impossible.

**A freed page keeps its old contents, so a use-after-free reads plausible
data.** That is why every hard memory bug here surfaced a long way from its
cause: musl tripping over its own malloc free list, init printing a pointer
where a pid belonged. Freed pages are poisoned now with a non-canonical
pattern, so the first dereference faults instead of the tenth one corrupting
something, and the poison is re-checked when the page is handed out again -
which catches a write to a page that was already free, at the moment it is
discovered rather than whenever it happens to matter.

**Refuse an unlock from a core that does not hold the lock, and count it.**
The console lock now does, and the boot gate asserts the count is zero. One
missing `serial_lock()` cost two investigations into crashes that were a marker
cut in half.

**Take the crash state at the crash.** A faulting process used to leave two
numbers behind, rip and cr2. Every hard bug here was then diagnosed by going
back for the registers, the stack, and which binary the task was running - and
by then the process is gone. `hw_fault_kill_current_user` records all of it
into a ring of four, and `crash` on the kernel console prints the last one.
The record names the executable, because an address alone identifies nothing
when every Linux program links at 0x400000.

**Check the log is trustworthy before concluding anything from it.** Every
assertion in the boot gate reads the serial log, so one write cut into another
can invent a failure or hide a real one - and for a session it did, costing two
investigations into crashes that were a marker cut in half. `interleaved_lines`
now runs first, before any other check. On its first run it found four
multi-part kernel messages assembled without bracketing.

The check carries self-tests, including a line it must *not* flag: a ring-3
write with no trailing newline leaves the physical line open, and the next
kernel line legitimately continues it. The first version called that a defect
and cried wolf immediately. A detector that reports healthy behaviour is a
detector people learn to ignore.

**A multi-part message is several critical sections unless it is bracketed.**
`serial_puts` and `serial_print_hex` each take the console lock on their own,
so a line built from six of them is six of them. The tell in the log is a hex
field cut off right after its `0x`.

**Randomised churn is only useful if the seed is printed.** `svc-stress` runs
120 rounds of fork, mmap, copy-on-write and pipe work in an order that differs
every boot, checks the result of each one, and prints the seed on its first
line so a failure can be replayed with `EFI/BOOT/SVC_STRS.ELF <seed>`. It found
a copy-on-write defect on its first serious run - the first bug here to arrive
with a reproduction recipe rather than a story.

**A gate that crashes leaves the last run's verdict lying around.**
`check.sh` runs the boot gate as `python3 ... > /dev/null 2>&1` and then greps
`qemu-cli-summary.txt`. A syntax error in the gate therefore reads as a pass:
the script dies, the summary file still holds the previous run's
`reason=cli_and_network_verified`, and `check.sh` prints it. Three runs passed
here having executed nothing. Same family as "a green build is not a build",
and the tell was the same - a timestamp that had not moved. Check that
`qemu-cli-serial.log` is newer than the change being tested.

**Everything gets tracked in the docs.** Not "significant work" - anything.
A macro-area row in `docs/implementation_progress.md`, a detail file under
`docs/implementation_progress/`, and `scripts/dev/make-book-summary.py` re-run
so the new file reaches `docs/SUMMARY.md` and the published book. A detail file
that is not in SUMMARY.md exists for whoever finds it by accident.

The table and its detail files have drifted apart twice, and the second time
the table contradicted a file it linked to - it said dynamically linked Linux
binaries were refused while the file recorded them running and gated. Whoever
changes a status changes both.

**munmap has to consult the reference count, like teardown does.** It freed
the frame behind every page it unmapped, outright. That is correct only for a
page nobody else has, and after a fork that is the rare case - so unmapping a
copy-on-write page put it back on the freelist while another process was still
running from it.

This one defect had been chased three times from the far end and presented as
something different each time: a musl program tripping over its own malloc
bins, init printing a pointer where a pid belonged, a forked child reading back
a value it had not written. Every investigation ended at a plausible-looking
garbage pointer with no mechanism behind it.

What closed it was two detectors meeting. The stress run produced the failure
on demand instead of one boot in thirty, and the free-page poison said what the
wrong bytes *were*, so the message named the mechanism: "this is the kernel's
free-page poison: the page was reclaimed while still mapped here". The fix was
one call. That is the argument for building detectors before chasing the next
rare bug by hand.

**BOOT_OK is printed after userland has finished, not before it starts.**
`entry.s` calls `vibeos_x86_64_hw_early_init`, and despite the name that
function runs the whole machine - descriptor tables, paging, SMP, drivers,
filesystem, init, every service, the shell - and only then is `vibeos_kmain`
entered to bring up the portable subsystems and print `BOOT_OK`.

The cost of not knowing this is high and was paid in full: the boot gate waits
for `BOOT_OK`, so until it arrives the reported phase stays at the last
*bootloader* marker. A hang anywhere in userland therefore reads as
`phase=bootloader_exit_boot_services`, and a whole session went into looking
for a firmware bug that did not exist. `phase=busybox_cat` was the same
illusion - the guest was stuck in `ping`, several commands later.

The gate now has `kernel_early_init`, `userland_running` and
`userland_finished` phases. The ordering itself is still wrong: the portable
kernel in `kernel/core/` is initialised after the machine has already done all
its work on the arch layer's structures, which is also why it can be
host-tested and yet have no runtime role.

**A thread has no reaper, so it must not be left as a zombie.** `waitpid`
matches on `ppid`, and a thread inherits its *creator's* parent rather than
becoming its child - so the thing that joins it cannot wait for it, and nothing
else is looking. Every thread that exited held its slot for the rest of the
boot. The failure surfaced nowhere near the threads: a program that created and
joined eight of them leaked eight slots, and several commands later the shell's
fork was refused on a machine with plenty of memory. Joining never needed the
zombie - exit clears the word the joiner sleeps on and wakes the futex.

Two things that cost more than the fix:

*A slot released outside the transition table stays RUNNING.* Calling
`hw_task_release` directly is `running -> free`, which the table refuses -
correctly - and logs one `ILLEGAL` line per thread. So the first attempt
replaced a leaked zombie with a task stuck RUNNING, which is worse and quieter.
Go through ZOMBIE. Without that table the change would have looked like it
worked.

*One boot argued the opposite of the truth.* Raising the task table from 24 to
32 measured a broken boot and 24 measured a working one, so 32 was written down
as the cause. It was not - the leak was, and with the leak fixed 32 boots clean
8/8. Two single boots on a machine with a known intermittent failure decided a
question they cannot decide. That is this file's own rule about counts, applied
to a sample of one, and it was applied by the person who wrote the rule down.

**Assert on a program having *started*, not on its binary being present.**
A test binary ships on the media whether or not the boot script runs it, so a
gate keyed on the file reported a missing result as a failure while nothing had
been asked to produce one.

**A test can be right about the outcome and wrong about the mechanism.**
Three times in one stretch of memory-manager work, a sabotage case walked
straight through a green test - and each time the test was asserting the
correct property about an arrangement in which the defect could not show:

- "only the first holder is checked for writability" passed, because the test
  mapped the writable holder last and the reverse map inserts at the head, so
  the writable one was examined first anyway. It maps in both orders now.
- "page-in accepts a present entry" passed, because with the guard removed
  page-in derives a slot number from the frame address, which is far outside
  the swap area, so the transfer failed for a different reason. The test now
  asserts that no read was *attempted*.
- "the anonymous tier is asked for the whole amount, not the shortfall" passed,
  because with no clean tier in the harness the shortfall and the request were
  the same number. The harness now supplies part of the request from the clean
  tier so the two differ.

None of these were found by reading. A test whose arrangement hides the defect
is indistinguishable from a test that works, and only breaking the code on
purpose tells them apart. This is why the sabotage cases are worth their cost:
they test the tests.

**The free count and the free list are separate things.** A release that
decrements one without linking into the other leaves a machine that believes it
has memory and cannot hand any out. An exhaustion test that checked only the
count passed a sabotage that broke the list - "the count came back exactly" was
true and meant nothing. Allocate everything a second time.

**"Configured and consulted by nobody" is the defect this project produces most
often.** Three separate instances in one session: the scheduler computed a time
slice and re-picked on every tick so nothing read it; the memory watermarks
were set at boot and never consulted from the allocation path; and reclaim grew
an anonymous tier that nothing called. Each mechanism was correct in isolation
and connected to nothing.

The tell is the same every time: a number that is written and never read. When
adding one, write the assertion that would fail if the caller disappeared -
`ticks > runs` for the quantum, an admission refusal for the watermarks, an
end-to-end test for the tier.

**A sabotage case that turns the suite red is not the same as one your test
caught.** Breaking the frame free list segfaults an earlier test group, so the
suite fails loudly and `FAIL:test_acceptance` never prints. That is worth
recording as what it is rather than counting among the cases that pass - the
same distinction the AHCI cases carry between "no case exists" and "this
environment cannot tell".

**A detector that reads uninitialised state invents the bug it was looking
for.** The frame layer poisons a page on release and checks it on hand-out, and
`frame_check_poison` correctly skips a frame that has never been freed - it
asks the descriptor's `VIBEOS_FRAME_WAS_FREED` flag. But `vibeos_frame_init`
never initialised `flags`, and `frame_push_free` preserves that one bit on
purpose, so on a table full of the bump allocator's leftovers it preserved
*garbage*: every frame whose stale byte had bit 0x10 set claimed a release that
never happened, and the check then judged its virgin contents. 3019 reported
use-after-frees in a boot with none.

It cost a phase because the symptom moved with something that looks like a
memory bug and is not. Shrinking the two exec staging buffers made it appear on
every boot; shrinking either alone was clean; wasting the freed six megabytes
made it vanish with the buffers still small. All true, and all explained by
those sizes deciding where the descriptor table lands in physical memory. Three
readings concluded "something writes to a frame it has already released", and
that was written into the source beside the constant.

What closed it was making the detector say *what* rather than *how often*. The
counter reports a number; a watch reporting which frame, which word, what value
and who released it answered on the first boot: `found=0 freed_by=0`, at
`base + reserved_prefix` exactly - the pool's very first frames. A frame nobody
released is not a use-after-free. **When a detector fires, the first question is
whether the detector is right**, and it cannot be answered by a count.

The existing test for this had the property exactly right and could not see the
defect, because its `setup()` memsets the descriptor table and the kernel does
not. Fourth instance of "right about the outcome, wrong about the mechanism".
The new one fills the table with 0xFF instead - not with the bit the layer uses,
so the test does not encode the implementation it is checking.

Two smaller traps in the same session, both of which this file already warns
about and both of which were walked into anyway: the diagnostic deadlocked the
machine it was explaining, because a watch that runs inside a layer's lock
called the public accessor that takes it (the boot stopped mid-line, at
`owners=0x`); and `[HW] exec staging buffer: 4 MiB` was a string literal, so it
kept saying 4 MiB with the constant set to 64 KiB, and the first run briefly
read as "the built image did not boot". **A banner that names a number a
constant decides will eventually name the wrong one** - print the constant.

**A check that has been red since before you arrived is a check nobody is
reading.** `check-mm-layering.sh` allows three calls to the bootstrap allocator.
The reverse map's node pool was deliberately moved ahead of `vibeos_frame_init`
a phase earlier, making it four, and the limit was never raised - so the guard
sat failing and unread until a full `check.sh all`. Raise such a limit as a
decision, in the same change that earns it.

## Verification that exists

The boot gate (`scripts/qemu-cli-smoke-linux.py`) asserts state, not markers:
DHCP lease non-zero, four cores online, no unexpected fault, a TCP round trip
to a host echo server, the unmodified Linux binary ran, BusyBox dispatched an
applet and did file work, the interactive shell ran, signals were delivered.

It waits for `VIBEOS_SELFTEST_DONE` before driving the kernel CLI, because the
two run concurrently and a slower build used to get its script cut short.

The GUI is **not** gated - `screenshot.py` is run by hand.

Static, position-independent and dynamically linked Linux binaries all run and
are all gated. A dynamic one has its interpreter mapped into the same address
space and entered first; `AT_ENTRY` stays the program's own entry, which is how
the interpreter knows where to jump when it is done. The interpreter path is
translated in the kernel - a musl binary asks for /lib/ld-musl-x86_64.so.1 and
FAT has neither that directory nor a name that long - so the loader lives at
EFI/BOOT/LDMUSL.SO. That substitution is a stand-in for a filesystem layout,
and if the layout ever becomes real it should be deleted, not generalised.

## Tone of the code

Comments explain why, not what. If a line exists because of a specific failure,
the comment says which failure. Several of them are the only record of a bug
that took hours to find.
