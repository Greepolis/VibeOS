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

**The intermittent boot wedge is still open, and here is what is known.**
Roughly one boot in four or five goes completely silent, always partway
through the filesystem work (`ls`, `cat`, the second BusyBox exec), never with
a fault or a panic. `catch-hang.py` is not useful for it: the RIPs it reports
are the CLI idle in `serial_readc`. `scripts/dev/until-wedge.sh` keeps the
serial log of the boot that failed, which is the part `repeat-boot.sh` drops.

The strongest lead is a lock-protocol mismatch that is written down in fat.c's
own comment. It says callers are syscalls running with interrupts masked, so
the holder cannot be preempted - but execve takes `g_exec_lock` preemptibly on
purpose and reads the image through `g_fs_lock`, so the holder *is* preemptible
for the length of a two-megabyte read. A core waiting on `g_fs_lock` with
interrupts masked never reaches the scheduler, and cannot give the preempted
holder a CPU to finish on. That predicts exactly this: silence, no fault, only
under filesystem load. Enabling interrupts while waiting was tried and made it
*worse* - a wedge earlier in boot and more often - so the analysis is not the
whole story and the change was not kept. Do not re-apply it without measuring.

**Definition order bites repeatedly.** This is one 5000-line C file; a helper
used above its definition compiles as an implicit declaration and then fails
with a confusing "static declaration follows non-static declaration".

## Verification that exists

The boot gate (`scripts/qemu-cli-smoke-linux.py`) asserts state, not markers:
DHCP lease non-zero, four cores online, no unexpected fault, a TCP round trip
to a host echo server, the unmodified Linux binary ran, BusyBox dispatched an
applet and did file work, the interactive shell ran, signals were delivered.

It waits for `VIBEOS_SELFTEST_DONE` before driving the kernel CLI, because the
two run concurrently and a slower build used to get its script cut short.

The GUI is **not** gated - `screenshot.py` is run by hand. Dynamic binaries are
refused, so "runs Linux programs" means static ones.

## Tone of the code

Comments explain why, not what. If a line exists because of a specific failure,
the comment says which failure. Several of them are the only record of a bug
that took hours to find.
