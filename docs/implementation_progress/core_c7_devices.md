# C7 - the registries the next refactor needs

Status, 2026-09-23: **in progress - steps 1, 2 and 3a of 4 done** (the registry,
proved on the input devices; the network interface; the disk drivers).

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
- `scripts/dev/cases/io-device-boot.txt` (4 cases, boot; the fourth from step 3a): an empty table (red as
  a wedge: with no keyboard the boot's own script is never typed, and the boot
  stops before the counters that would name it); no wake; no routing.
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
- `io-device-boot.txt` case 4, the bind loop stopping after the first disk: red,
  **but as a wedge** (`missing:VIBEOS_SELFTEST_DONE`), not by name. With only
  AHCI bound the boot volume is never found, and the machine goes quiet instead
  of saying so. Recorded as what it is, like case 1: the gate goes red, the
  kernel does not explain itself.

## Next

3b. **Filesystems**: the registered FAT driver without its init call.
4. **Display**: the GUI registered, with its own lock, counters, a must-be-zero
   and a case file - the "done when" of the phase.
