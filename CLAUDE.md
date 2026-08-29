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
