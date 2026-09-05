# Devices: discovery, drivers and peripherals

Status: Proposed
Separated from [docs/io/](../io/README.md) deliberately. Storage is one
consumer of device discovery; the network is another and the peripherals are a
third, so putting enumeration inside the storage plan would make two of its
three users depend on a plan that is not about them.

**This is a survey, not a plan yet.** The phases are deliberately not written:
storage comes first, and a second detailed plan running beside it would invite
working on both. What is here is the set of facts a plan would be built on,
recorded now because they were found now and would otherwise be found again.

## What is actually there

A survey, not an impression.

**There is no PCI layer.** `pci_read32` and a bus scan are written out three
times — in `ahci.c`, `virtio_blk.c` and `virtio_net.c` — and `0xCF8` is defined
in each of them. Every future driver would write it a fourth time.

This is the same shape as the three parallel read paths the storage plan opens
with, and it has the same consequence: three copies of one fact, which is how
they come to disagree. The AHCI driver already carries a comment about matching
by PCI class rather than device id, because VirtualBox emulates an ICH8 and
QEMU an ICH9 — that knowledge lives in one driver and nothing else can use it.

**AHCI does not know how big its disk is.** virtio-blk reads its capacity from
the device configuration and keeps it; AHCI never issues IDENTIFY. So every
AHCI read today is unbounded: an LBA past the end of the medium goes straight
to the controller. The block layer built in I0 refuses to register a device
that cannot say its size, which makes this a prerequisite rather than an
improvement.

**The peripherals are legacy, and there is no USB at all.** PS/2 keyboard and
mouse, a framebuffer, a GUI on top of it, and the serial port. Nothing
enumerates a bus to find them — they are at fixed addresses because they always
have been. That is fine for what they are, and it is worth writing down that
"external peripherals" on this machine currently means "the two devices IBM put
on a PC in 1984".

**Interrupts are wired by hand.** Each driver knows its own vector. There is no
allocation, no sharing, and nothing that could route an MSI.

## The goal

**Production ready.** A machine that says which hardware it found and which it
could not drive, rather than failing later in a way that looks like a missing
file. This project has that failure in its history: the appliances imported and
booted and then every exec failed, because the kernel could only talk to
virtio-blk and nothing said so.

**Observable.** A boot that lists what is on the bus, what claimed it, and what
was left unclaimed. Today a device nobody drives is invisible.

**Maintainable.** One PCI implementation. One place that knows how a driver is
matched to a device.

**Extensible.** Adding a driver means writing a table and a match rule.
Adding USB later means adding a bus, not editing the drivers that already work.

## Where the boundary with storage is

The line is between *finding* a device and *driving* one.

**PCI enumeration is here.** It serves storage, the network and eventually the
peripherals, so putting it in the storage plan would make two of its three
users depend on a plan that is not about them.

**A block driver knowing its own capacity is storage**, and is already needed:
the block layer built in I0 refuses to register a device that cannot say how
big it is, because a device with no size cannot have its requests
bounds-checked. AHCI issuing IDENTIFY is therefore part of I1, not of this.

## What a plan here would not do

It would not implement USB. It would make USB *possible* by giving a bus
something to plug into — but a host controller, a device stack and a
mass-storage class driver is its own project, and putting a large unknown
beside phases that are measurable is how a plan stops being one.
