# C7 - the registries the next refactor needs

Status, 2026-09-23: **done** - the registry, proved on the input devices; the
network interface; the disk drivers and the registered filesystem; the display.
The "done when" - the GUI registered with its own lock, counters, a must-be-zero
and a case file - is met. The blast-radius objective is met for every device
row and not for two others; see the table at the end.

The plan (`docs/core/phases.md`): character, input, network and display devices
register the way block devices do, `check-blast-radius.py` reports **1** for
every row, and the GUI is registered rather than wired in, with its own lock,
counters, a must-be-zero and a case file.

## Where it started

`check-blast-radius.py` measured every driver kind at 3 or 4: adding one meant
editing, besides its own file and its line in the build,

- an init call in `arch_hw.c` (or `io_bringup.c`),
- a declaration in `arch_x86_64.h`,
- a branch in the interrupt handler naming its vector,
- and a print of its counters in `kmain.c`, with a weak default beside it.

Block devices had a registry (`vibeos_blk_register`) and it bought nothing
measurable: everything *around* it named each driver. `include/vibeos/drivers.h`
had a portable "driver framework" that no running code consulted.

## Step 1: the registry, proved on the input devices

`include/vibeos/device.h`, `kernel/io/device.c`.

- **A driver is a descriptor in its own file**: name, class, legacy interrupt
  line, `probe`, `irq`, `selftest`, `report`, and its class's operations.
  `VIBEOS_DEVICE(desc)` places a pointer to it in the `vibeos_devices` linker
  section; `kernel.ld` collects the section between two symbols and
  `arch_hw.c` hands the table to the registry. Nothing names a driver.
- **The registry does what `arch_hw.c` and `kmain.c` did by name**: routes every
  declared legacy line (before any probe - a probe raises its line, and the
  mouse's acknowledgements were exactly that), probes, dispatches an interrupt to
  every device registered on its line (present or not - a PS/2 mouse that failed
  its probe still has to take its byte off the port), runs self-tests and prints
  each driver's counters on a line of its own, and serves the input class:
  `vibeos_input_getc`, `vibeos_input_inject`, `vibeos_input_pointer`. The
  console reads a character and the display reads the pointer without knowing
  either is PS/2.
- **Set once, probed once, then sealed.** The readers - interrupt dispatch among
  them - take no lock, because the data no longer changes; a second table or a
  second probe is refused and counted (`VIBEOS_MBZ_DEVICE_AFTER_SEAL`). The
  report takes the module's own lock for the self-tests.
- **Probes run outside the lock.** The lock masks interrupts, and the keyboard's
  probe waits for one (below). A host test fails if a probe ever sees the lock
  held.

The keyboard and the mouse moved onto it. Every symbol either exported is gone;
the keyboard's counters left the middle of a `kmain.c` line for a `[KBD]` line of
their own, the mouse's left `arch_hw.c` for a `[MOUSE]` line from its driver, and
the mouse's `packets` counter - counted since the driver was written and read by
nobody - is on that line now.

**Input device: 4 -> 1.** The one edit left is the file's line in the build.

## What building it found

**Nobody types in CI, so the keyboard's interrupt path had never run in a boot.**
Two boot sabotages - do not route the legacy lines; do not wake a reader when an
input interrupt arrives - went **NOT RED**. The reason was not a weak assertion:
the boot's script is *injected* into the keyboard's ring, which raises no
interrupt, so IRQ1's routing, the dispatch and the wake-up were exercised by no
boot at all. The first had also been true before C7, under the name of the
driver.

The keyboard's probe now proves the path: it asks the PS/2 controller to put a
byte in its output buffer as though the keyboard had sent it (command `0xD2`,
with a key-release code the driver ignores), which raises IRQ1 exactly as a
keystroke does, and waits - bounded - for it. `[KBD] ... irq_proved=` says it
arrived; `[DEV] ... input_irq_wakes=` says the dispatch woke readers because of
it. The gate asserts both (`kbd_irq_unproven`, `input_irq_wake_unproven`), and the
two sabotages are red by those names now. If the interrupt never comes, the probe
takes the byte back off the port - otherwise the mouse's probe, next, would read a
keyboard byte as its own acknowledgement.

**The first version probed under the lock**, where that interrupt could never
arrive on the core that was waiting for it: the lock masks interrupts, and the
line is routed to that core. Caught by reading the code before the boot; the
host test and `io-device.txt` hold it now.

**KEEP in the linker script does nothing today** - the image is not linked with
`--gc-sections` - and the first draft of its comment said otherwise. It is there
for the day it is; `[DEV] registered=` is asserted (`device_table_short`), so a
section that loses its entries is named either way.

## How it is proved

- Host test `tests/kernel/device_tests.c`: refusal of a table with a hole or too
  many entries; presence before and after probing; every declared line routed;
  probes in order, outside the lock, seeing the machine's description; the seal
  and its must-be-zero; interrupt dispatch to every registered device on a line,
  and to nobody on an undeclared one; self-tests under the lock and reports
  outside it, for absent devices too; the input class in table order, the pointer
  only from a present device; report lines whole and newline-terminated.
- `scripts/dev/cases/io-device.txt` (13 cases, host; 9-10 from step 2, 11-13 from step 3a): all red by name.
- `scripts/dev/cases/io-device-boot.txt` (4 cases, boot; the fourth from step 3a): an empty table
  (`verdict=boot_volume_missing`: no disk driver, so no volume; it was a
  "wedge" until the kernel said so); no wake; no routing.
- `core-observability-modules.txt`: the three mouse cases, re-pointed at the
  driver, red by name.

## Step 2: the network interface

A `NET` class (`vibeos_net_ops_t`: `mac`, `send`, `recv`) and
`vibeos_net_device()`, which hands the stack the first *present* network device
with every operation - not one whose probe failed (no queues: every frame
swallowed) and not one without `recv`. virtio-net's init is its probe, and
`hw_net_bringup` asks the registry for the result instead of naming the driver.

**Network interface: 4 -> 1.** What it had been: `arch_hw.c` declared six of the
driver's functions and called four; `kmain.c` printed its timeout count through a
weak default; and two accessors - the transmit/receive frame counts and `ready` -
were declared and called by nobody. The frame counts are on the driver's own
`[VNET]` line now (a boot sends about ten and receives about nine), next to
`net_tx_timeouts`, which left kmain's `[IO] WAITS` line; the gate reads each
where it is and still calls either one missing `io_wait_counters_missing`.

Proved by the host test (a failed or incomplete network device is never handed
out; `io-device.txt` cases 9-10) and on the boot (`io-device-net.txt`: the
timeout leaving the report line is `io_wait_counters_missing`; a device reported
absent takes the lease and the TCP round trip down with it, by name).

## Step 3a: the disk drivers

A `BLOCK` class (`vibeos_block_ops_t`: `read`, `read_many`, `write`,
`write_many`, `barrier`, `sectors`, `timeouts`). AHCI and virtio-blk are
descriptors; their init is their probe, and `arch_hw.c` binds every present
block device in table order through one loop instead of initialising each by
name and spelling out seven of its functions.

**Interrupt vectors are handed out, not chosen.** A PCI device has no legacy
line to declare, so the descriptor's `isa_irq` is `VIBEOS_DEVICE_NO_IRQ` and
the registry gives each table slot a vector of its own - slot *i* gets
`VIBEOS_DEVICE_VECTOR_BASE + i` (48..63), passed to the probe in
`env->vector`, one stub each in `isr.S`. The disks used to route their lines to
42 and 43, literals chosen by hand *inside the legacy range*, each with its own
branch in the interrupt handler. `vibeos_device_irq_vector` dispatches to the
slot; anything it cannot place is `stray_vectors` on the `[DEV]` line, a
must-be-zero the gate asserts (`device_stray_vector`). Two rules in that
dispatch, each with a host test and a sabotage case:

- it does **not** require the device to be present: a disk's probe is its init,
  which routes the line and then issues commands that complete by interrupt -
  before the probe has returned and the slot is marked;
- a device on a legacy line is never dispatched a vector: it was handed one it
  does not use, and a stray would otherwise become a keyboard read of port 0x60.

The probes moved after the kernel log exists (the disk drivers log). The
counters left kmain's `tlbq` line - four weak defaults and four prints - for
each driver's own report line, `[AHCI] ahci_irqs=.. timeouts=..` and
`[VBLK] blk_irqs=.. blk_irq_completions=.. blk_poll_completions=.. timeouts=..`;
the gate's patterns were never anchored to the line, so they read the new ones
unchanged. `[DEV] registered=` must now be at least 5 (keyboard, mouse,
virtio-net and both disks are always built).

**Block driver: 4 -> 1.** One visible change: the adapters now bind in link order,
AHCI first (`[BLK] disks=.. disk=ahci:0 disk=virtio-blk:1`). That was checked
rather than assumed: which disk the machine boots from has been decided by
mounting since I5, not by bind order, and the boot gate is green on both
controllers.

Proved by the host test (`env.vector == BASE + slot`; a vector reaches that slot
and no other; strays past the table, without a handler, on a legacy line and
either side of the range call nobody; `io-device.txt` cases 11-13) and on the
boot, every case red:

- `io-device-block.txt` (AHCI, 3 cases): its line routed to a registry vector
  nobody was given is `device_stray_vector` - about 5.9 million of them, because
  a level-triggered line whose handler never runs is never acknowledged - and
  `ahci_interrupt_never_fired`; the vector ignored (the driver polls, and the
  disk still works, which is exactly why only the counter can say so) is
  `ahci_interrupt_never_fired`; the count leaving its line is
  `ahci_irq_counter_missing`.
- `io-device-block-vblk.txt` (virtio-blk, 2 cases): `blk_interrupt_never_fired`
  and `blk_completion_counters_missing`.
- `io-device-boot.txt` case 4, the bind loop stopping after the first disk:
  `verdict=boot_volume_missing`. It first went red as a wedge - see below.

### "Wedged" was a machine that had finished

That case first went red as `missing:VIBEOS_SELFTEST_DONE verdict=guest_wedged`,
and so did the other two ways of losing the boot volume (case 1, an empty
device table; `io-device-fs.txt` case 1, a FAT driver that does not declare
itself). The serial log says otherwise. With only AHCI bound the kernel printed
`[BLK] boot volume on none`, ran the built-in init, finished userland at once and
sat at `vibeos>`. Nothing was hung. The gate was waiting for a self-test that
lives on the disk that was not there, and after 45 seconds of an idle prompt it
called the quiet a wedge and sent `wedge_report` to look for a hung core.

Carrying on without a volume is kept on purpose: the built-in init and the
kernel console are worth having on a machine whose disk this kernel cannot read,
which is the VirtualBox story in CLAUDE.md. What changed is that the kernel says
so as a stage failure,
`[BLK] BOOT_VOLUME_FAIL: no disk carries a mountable volume tried=..`, and the
gate turns that into its own verdict, `boot_volume_missing`, ahead of
`guest_wedged` and without the wedge report. All three cases now go red with
that verdict. The same sabotage without the marker is `guest_wedged`, which is
the before.

`tried`, not the existing `rejected`: `rejected` counts adapters passed over on
the way to one that mounted, so when none mounts it is one short - the failing
boot printed `rejected=0x0` with one disk tried.

## Step 3b: the registered filesystem, without its init call

FAT had a registry - `vibeos_storage_register` - and still cost four files:
`io_bringup.c` called a register function by name, `arch_x86_64.h` declared it
beside an ops accessor and two exported function pointers (the probe and the
formatter, for the I4c exercise), and `arch_hw.c` declared the boot-volume mount
`extern`. Now the driver declares itself with `VIBEOS_FS_DRIVER` into a linker
section of its own; `arch_hw.c` registers whatever is there, once, at boot,
before anything is mounted; and every caller reaches it through the table -
`vibeos_storage_driver(name)` for the two that choose a filesystem rather than
scan for one (the boot volume, which UEFI requires to be FAT, and the exercise
that formats one), the scan for everything else. The storage core still takes
registrations rather than walking the section, because PE/COFF has no
`__start_`/`__stop_` symbols and the Windows job builds that core.

**Filesystem (registered): 4 -> 1.** "Filesystem (direct)" - ext2, NTFS, exFAT
and ISO9660, compiled into `storage.c`'s probe table - is still 4.

Proved by a host test the registry never had (`test_storage_driver_registry`:
whole-name lookup, a driver with no mount refused, reset) with two cases in
`storage.txt`, and on the boot by `io-device-fs.txt`: the driver not declaring
itself is `verdict=boot_volume_missing` (it was a "wedge"; see step 3a); a
mount that ignores the volume it is given is
red by name, `format:FAILED:_also_on_the_root` - after two fixes, below.

### What the sabotage found: a check that had been blind for seventeen days

The second case went NOT RED, and the reason was two defects stacked.

1. **The exercise could not see it.** It formats a scratch volume, mounts it at
   `/vol1`, writes a file and reads it back through the resolver - which proves
   `/vol1` reaches *the scratch mount* and that the mount round-trips. A mount of
   the boot volume does both, so the file went onto the disk the machine runs
   from and came back intact. It now also checks that the file is *not* visible
   at `/`, and says `FAILED: also on the root`.
2. **The gate could not see that.** Instrumented, the sabotaged boot printed
   `result=FAILED: also on the root` and the gate still passed: the assertion was
   `"result=OK" not in text`, anywhere in the log, and `[IO] LOGSINK result=OK`
   is on every boot. Both lines date from 2026-09-06 - the FORMAT check in I4c
   step 3, the log sink in I5b later the same day - so from then on no FORMAT
   failure could be reported. The gate reads the FORMAT line's own verdict now.
   The other two whole-log `..=OK` tokens the gate uses (`round_trip=OK`,
   `tls=ok`) were checked against a log and appear on one line each.

## Step 4: the display - the phase's "done when"

Before C7 the GUI was wired in: `arch_hw.c` declared five of its functions,
initialised it from the framebuffer setup, repainted it from the timer, printed
its counters and kept its canary, and `serial.c` named its putc through a weak
default. It had **no lock**, and two of its three callers run on any core: the
timer's repaint, and putc from the console write path - which is not always
under the console lock (kmain's hex printer writes a character at a time with
none). Two cores in putc could both pass `row < ROWS` before either scrolled,
and the second wrote a row past the end of the grid, into whatever the linker
put after it. The phase calls it the prime suspect for corrupting another
process's memory; whether it ever did is not known, and now cannot happen
silently.

It is `kernel/io/gui.c` now, a DISPLAY-class device, with the seven parts of
`docs/core/architecture.md`:

1. **A header**, `include/vibeos/gui.h`.
2. **All of its state in one file**, in two kinds with one rule each: the
   terminal grid and the counters under the lock, every access; the back
   buffer, the screen and the pointer's last position owned by whoever holds
   `g_ticking`, which only the repaint takes, by exchange. The repaint copies
   the grid under the lock and draws from the copy outside it, so a console
   write never waits for a window to be blitted with interrupts off.
3. **Its own lock**, a `vibeos_dev_lock_t`. The lock *operations* are
   registered once, by the machine, for every driver
   (`vibeos_device_set_lock_ops`): a portable driver cannot build a lock, and
   registering each driver's lock by name from the arch would have put the
   driver back in `arch_hw.c`. The provider has one extra duty: `lock` returns
   -1 instead of waiting when the caller already holds it - the machine answers
   from the spinlock's owning CPU - so a panic printing from inside the GUI is
   dropped and counted (`reentered`) instead of waiting on its own core.
4. **Counters with two must-be-zeros**: `term_overrun` (a grid write out of
   bounds, refused) and `guard_broken` (the canary). The canary is the GUI's
   own now, written directly after the pixels and examined on **every
   repaint**, not once at the end of the boot; `guard_checks` says it ran.
5. **An init that refuses by name**: no framebuffer; too large; too small; a
   back buffer with no room for the canary; already active.
6. **The registry's DISPLAY class**: the console writes through
   `vibeos_display_putc` and the timer repaints through `vibeos_display_tick`;
   neither names a driver.
7. **Case files**: `io-gui.txt` (host), `io-gui-torture.txt` (the model and
   threads), `io-gui-boot.txt` and `io-gui-backbuf.txt` (the gate).

The font became `include/vibeos/font8x8.h`, a header: a constant table has no
state, and as a `.c` module it would be judged by `check-subsystem` for a lock
and a must-be-zero it has no use for.

**Display: 3 -> 1.** Nothing outside `gui.c` and its header names the GUI.

### What building it found

- **A screen under about 30 pixels high wrote off the end of the buffer.** The
  text area's height is `window - 26`; on a 30-pixel screen that is `25 - 26`,
  which is 0xFFFFFFFF, and the clamp `if (y + h > g_h)` wrapped `y + h` back
  under the screen - so the fill ran on. Width had the same shape below 10
  pixels. No real screen is that small; `init` accepted any. It refuses them by
  name now, and every clamp compares before it adds.
- **The character that wrapped the last row was lost.** The old putc wrapped,
  found the row past the end, skipped the write and scrolled afterwards.
- **A machine with no mouse never showed console text**: the repaint returned
  before the text when there was no pointer.
- **The pointer left fragments on the desktop.** After a text repaint the old
  code forgot the pointer's position ("whatever was under the pointer is
  gone"), which is true only inside the text window. The torture's first run
  found it, comparing the screen with the composition pixel by pixel; its
  sabotage case puts the old line back.
- **A registered driver in a static archive is not linked.** Moving `gui.c` to
  `kernel/io` put it in the core library the image links as an archive, and an
  archive member is extracted only when something references it - which, for a
  driver that registers, nothing does. The boot came up with
  `registered=0x5`, the text console took the framebuffer, and the gate said
  `gui_counters_missing`. Such drivers are listed in
  `VIBEOS_KERNEL_DRIVER_SOURCES`, compiled into the image directly and into the
  host library for the tests; the gate's floor is six devices.
- **A GUI that refused passed the gate.** Every GUI check was conditional on
  `[GUI] desktop up`. The gate now fails `gui_refused:<reason>` when the text
  console had to take the framebuffer.
- **`[FB] no framebuffer; console is serial-only`** was printed on every boot
  that had a desktop - the text console's "else" was reached whenever the GUI
  had taken the screen. Three cases, three lines.
- **`backbuf_shared` and `backbuf_lost` were must-be-zero and asserted by
  nobody.** `guard_broken` led their line and `check-mustbezero-asserted`
  reads only the first name after the word; moving the canary exposed them.

### How it is proved

- `tests/kernel/gui_tests.c`, in the host suite: the refusals by name with
  nothing written, the canary written and checked on the repaint and counted
  once, the terminal's rows, wrap and scroll, the wrap on the last row, the
  pointer drawn and not baked into the composition, reentrancy, the lock
  paired, and the display class choosing the first present display with both
  operations.
- `tests/kernel/gui_torture.c` (`vibeos_gui_torture <seed> [rounds]
  [threads]`): geometry at the edges of what init accepts, with both buffers
  fenced; the terminal against a model that keeps a list of lines rather than a
  grid; the screen against the composition, and the pointer visible; a flipped
  canary word; a print from inside the lock; then writers and two racing
  repaints on real threads, with every character accounted for. A short run in
  `check.sh`; the nightly job `gui-torture` runs 200 seeds under ASan and
  UBSan and the threads under ThreadSanitizer.
- The gate: `gui_counters_missing`, `gui_term_overrun`,
  `gui_guard_never_checked`, `gui_refused`, `gui_backbuf_shared`,
  `gui_backbuf_lost`, and `device_table_short` at six.

Every case red, and all but one by name:

- `io-gui.txt` (6, host): each by the test that names it. Removing the
  minimum screen accepted a 30-pixel screen and did **not** overrun: the
  clamps now compare before they add, so the two defences each hold alone.
- `io-gui-torture.txt` (4): no lock at all - the GUI as it was - is
  `71094 characters sent, 37897 counted` from the thread phase; the pointer
  fragment is "screen differs from the composition"; a pointer not redrawn is
  "not on the screen"; an uncounted scroll disagrees with the model. The first
  version of the no-lock case removed the lock and kept the unlock, and went red
  on the single-threaded pairing check instead - which proved the wrong thing,
  so it was rewritten.
- `io-gui-boot.txt` (6) and `io-gui-backbuf.txt` (2): `gui_counters_missing`,
  `gui_guard_never_checked`, `gui_term_overrun=483`,
  `gui_refused:back_buffer_missing_or_too_small_for_the_screen_and_its_canary`,
  `gui_reported_nothing`, `gui_terminal_empty`, `gui_backbuf_shared=1000`,
  `gui_backbuf_lost=1000`.
- `io-device.txt` case 14, a display with no repaint handed out: red by name.
  Its first run was red as a crash - the test called the repaint before
  checking which display it had - and the test was reordered.

Not yet run: the Windows (mingw) build of the core now contains the first
VIBEOS_DEVICE descriptor in the portable library (`gui.c`); the section
attribute there is untested on PE/COFF until CI runs it. The ThreadSanitizer
nightly job is new and has not run either.

`check.sh all` green, warnings 0 on gcc and clang, repeat-boot 6/6.

## Where C7 leaves the rows

| Extension point | Before C7 | Now |
|---|---|---|
| input device | 4 | 1 |
| network interface | 4 | 1 |
| block driver | 4 | 1 |
| filesystem (registered) | 4 | 1 |
| display | 3 | 1 |
| filesystem (direct) | 4 | 4 |
| syscall | 2 | 2 |

The objective said every row. Two are not 1, and neither is a device: ext2,
NTFS, exFAT and ISO9660 are compiled into `storage.c`'s probe table with a
member each in `vibeos_volume_t`, and a syscall is 2 by a recorded decision (its
row, and its declaration in `abi.h`). Moving the four filesystems onto
`VIBEOS_FS_DRIVER` is the same work step 3b did for FAT, times four; it is left
for the refactor that needs it rather than folded into this one.
