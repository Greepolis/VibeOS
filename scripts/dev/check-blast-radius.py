#!/usr/bin/env python3
"""How many existing files must be edited to add one thing of a known kind?

`docs/core/` calls this the plan's real progress metric, and the reason is in
this project's own history: line count has been the stated completion criterion
twice and failed twice, both times because the file grew while the document
naming its length sat unchanged. Blast radius is the property that was actually
wanted, and unlike a line count it cannot be satisfied by moving code.

> A subsystem is extended by registering into it, never by editing it.

## How it is measured

For each extension point a **witness** is named - an implementation that already
exists - together with the prefix its exported symbols share. The radius is the
number of files that name any of those symbols, **excluding the witness's own
`.c` and `.h`**, plus the CMake source list if the witness is listed there
(adding a file to the build is a real edit of an existing file).

That is a count of the places somebody adding the *next* implementation would
have to go. It is measured, not asserted, which matters here: this check was
written to enforce a table in `docs/core/architecture.md` and the first run
contradicted it. See "what the first run found" below.

## What it cannot judge

Textual occurrence, not reference: a name in a comment counts, and a call
reached through a function pointer does not. Both are stated rather than worked
around - a tool that guessed would produce the confidently wrong answer this
project distrusts, and every symbol here is named directly today.

It also cannot tell a *necessary* edit from an incidental one. `serial.c` names
a GUI symbol because the console draws to the framebuffer, which is a real
dependency and not a registration failure. The number's job is to move when the
structure changes, not to be a verdict.

## What the first run found

The plan's table said a filesystem costs **1** file and cited four filesystems
arriving without their layer being touched. The four - ext2, ntfs, exfat,
iso9660 - do **not** use `vibeos_storage_register`. They are named directly in
`g_probes[]` in `kernel/fs/storage.c`, each with a mount wrapper there and a
member in `vibeos_volume_t` in `include/vibeos/storage.h`. Adding one the way
they were added edits four existing files.

The registration seam is real and exactly one driver uses it - FAT - and that
driver measures **4 as well**, because somebody still calls the register
function, declares it, mounts through it and lists the file. So the seam does
not currently buy anything: a registry whose members need an init call has the
same floor as no registry at all.

Nothing in this tree measures 1. The plan's table said two rows did.

So the claim was folklore in the document that named it the progress metric -
which is the defect this project produces most often, committed by the plan that
warns about it. The numbers below are the measurement, and the first honest
statement of what C6 has to achieve: a seam a driver joins **without anybody
editing a bring-up path**.

That also corrects a hand measurement made while writing this file. FAT was
counted as 2 by grepping `vibeos_x86_64_fat_vfs`, which misses `_ops` and
`_register_driver` - the two symbols that *are* the seam. The check contradicted
its author on the first run, which is the only reason the number is 4 here.

Usage: check-blast-radius.py [--list]
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

SCOPE = (("kernel",), ("include",), ("cmake",))

# extension point -> (witness .c, symbol regex, declared radius, why)
#
# The matcher is a regex per point rather than a shared prefix, because a prefix
# is not always a module. `vibeos_x86_64_fat_` matches both the VFS driver in
# fat_vfs.c and the raw FAT reader in fat.c, which are two different things; the
# first version of this file used the prefix and reported a radius of 7 for a
# module whose real answer is 2. Name what the witness exports.
#
# The declared number is today's measurement and may only go DOWN. It goes down
# by moving an implementation onto a registration seam; it goes up only as a
# recorded decision, in the change that earns it.
POINTS = {
    "filesystem (direct)": (
        "kernel/fs/ext2.c", r"vibeos_ext2_[A-Za-z0-9_]+", 4,
        "named in g_probes[] in storage.c, with a mount wrapper there and a "
        "member in vibeos_volume_t. This is the path four of the five "
        "filesystems took, and the one the plan claimed cost 1 file."),
    "filesystem (registered)": (
        "kernel/arch/x86_64/fat_vfs.c",
        r"vibeos_x86_64_fat_(?:vfs_[A-Za-z0-9_]+|ops|register_driver)", 4,
        "the seam exists and exactly one driver uses it - and it still costs "
        "4, because somebody has to call the register function (arch_hw.c), "
        "declare it (arch_x86_64.h), mount through it (io_bringup.c) and list "
        "the file. A registry whose members still need an init call has a "
        "floor no lower than one that has no registry at all."),
    "block driver": (
        "kernel/arch/x86_64/ahci.c",
        r"vibeos_x86_64_ahci_[A-Za-z0-9_]+|ahci_dev_[A-Za-z0-9_]+", 1,
        "**1 since C7 step 3**: AHCI and virtio-blk are BLOCK-class "
        "descriptors; the probe is the init, the operations go over in a "
        "vibeos_block_ops_t that arch_hw.c binds in table order, the interrupt "
        "vector is handed out by the registry, and the counters are the "
        "driver's own report line. Was 4 - an init call and seven functions "
        "spelled out in arch_hw.c, a declaration block in arch_x86_64.h, and "
        "kmain.c printing their counters through weak defaults."),
    "network interface": (
        "kernel/arch/x86_64/virtio_net.c",
        r"vibeos_x86_64_virtio_net_[A-Za-z0-9_]+", 1,
        "**1 since C7 step 2**: virtio-net is a NET-class descriptor; its init "
        "is its probe, its counters its own report line, and the stack drives "
        "whichever interface vibeos_net_device() returns. Was 4 - arch_hw.c "
        "declared six of its functions and called four, kmain.c printed its "
        "timeout count through a weak default, and two accessors (the frame "
        "counts, `ready`) were declared and called by nobody."),
    "syscall": (
        "kernel/abi/pageinfo.c",
        r"VIBEOS_OP_PAGEINFO|X\(PAGEINFO|hw_sys_pageinfo", 2,
        "the last operation added, as the witness (a stand-in path; only its "
        "basename and include/vibeos/pageinfo.h are excluded). abi.h declares "
        "the operation and its checks; the file that holds the handler carries "
        "the row that registers it (number, name, operation, call) beside the "
        "handler.\n"
        "      **This is 2, and it was 1 before C4 (with no declaration at all), "
        "4 after stage 1 and a deliberate 6 after stage 3.** Stage 2 took it "
        "from 6 to 2 by making a syscall a *row in the file that implements it* "
        "instead of a case in a central dispatcher plus a declaration between "
        "the two: nothing else names it. A third edit exists that this count "
        "cannot see because it is not kernel code - its line in "
        "scripts/dev/linux-syscall-numbers.txt, the independent statement of the "
        "number that check-syscall-checks.py holds every row against. That is "
        "deliberate: the number is stated twice so that a typo cannot pass "
        "unnoticed."),
    "input device": (
        "kernel/arch/x86_64/keyboard.c",
        r"vibeos_x86_64_keyboard_[A-Za-z0-9_]+", 1,
        "**1 since C7**: the driver is a descriptor in its own file, collected "
        "from a linker section (VIBEOS_DEVICE, include/vibeos/device.h); the "
        "registry routes its line, probes it, dispatches its interrupts, runs "
        "its self-test, prints its counters and serves getc/inject/pointer to "
        "the console and the display. What is left is the file's line in the "
        "build. The keyboard exports nothing; neither does the mouse.\n"
        "      History, kept because it is the argument for the registry: no "
        "registry; arch_hw.c named it. Was 2, and went UP to 4 in C2 - "
        "which is a decision with an argument, recorded here rather than an "
        "edit to make the check quiet.\n"
        "      Giving the keyboard its first must-be-zero counter cost two "
        "files that had nothing to do with the keyboard: an accessor declared "
        "in arch_x86_64.h and a print in kmain.c. That is not a keyboard "
        "problem. **Observability itself has a blast radius here, and it is 2 "
        "per module** - there is no seam a module registers its statistics "
        "into, so every counter C2 adds makes the structure C6 has to fix "
        "slightly worse.\n"
        "      Raising the number is the honest move because the cost is real. "
        "The check did its job: it named a structural regression within hours "
        "of being written, in a change whose entire purpose was to improve "
        "the thing it measures. A stats registration seam would take this row "
        "back to 2 and take the next thirty counters with it."),
    "display": (
        "kernel/arch/x86_64/gui.c", r"vibeos_x86_64_gui_[A-Za-z0-9_]+", 3,
        "no registry; arch_hw.c and serial.c both name it. The serial one is "
        "a real dependency - the console draws to the framebuffer."),
}


def sources():
    out = []
    for group in SCOPE:
        for d in group:
            for base, _, names in os.walk(os.path.join(ROOT, d)):
                if "build" in base or ".git" in base:
                    continue
                for n in names:
                    if n.endswith((".c", ".h", ".cmake")):
                        out.append(os.path.join(base, n))
    return out


def main():
    listing = "--list" in sys.argv
    files = sources()

    bad = []
    for point in sorted(POINTS):
        witness, sym, want, why = POINTS[point]
        own = {
            os.path.normpath(os.path.join(ROOT, witness)),
            os.path.normpath(os.path.join(
                ROOT, "include", "vibeos",
                os.path.basename(witness)[:-2] + ".h")),
        }
        pat = re.compile(r"\b(?:" + sym + r")")

        touched = []
        for f in files:
            if os.path.normpath(f) in own:
                continue
            try:
                t = open(f, encoding="utf-8", errors="replace").read()
            except OSError:
                continue
            if pat.search(t):
                touched.append(os.path.relpath(f, ROOT).replace("\\", "/"))

        # Adding a file to the build edits the source list. That is an edit of
        # an existing file and it belongs in the count.
        cm = os.path.join(ROOT, "cmake", "core_sources.cmake")
        if os.path.exists(cm):
            base = os.path.basename(witness)
            if base in open(cm, encoding="utf-8", errors="replace").read():
                rel = "cmake/core_sources.cmake"
                if rel not in touched:
                    touched.append(rel)

        got = len(touched)
        if listing:
            print("  %-26s %d (declared %d)  %s"
                  % (point, got, want, " ".join(sorted(touched))))
        if got != want:
            bad.append((point, got, want, why, sorted(touched)))

    if bad:
        for point, got, want, why, touched in bad:
            direction = "up from" if got > want else "DOWN from"
            print("  %s: radius %d, %s the declared %d" % (point, got, direction, want))
            print("      %s" % why)
            print("      files: %s" % " ".join(touched))
        # Advice first, verdict last. check.sh reads these with `| tail -1`, so
        # a verdict with anything printed after it is a verdict that does not
        # reach the summary - which is how this check's first real failure
        # showed up as an advice line and a RED with no reason beside it. All
        # four checks in this family had the shape; all four were fixed.
        print("      A radius going down is the point of the refactor - update "
              "the number here in the same commit. A radius going up needs an "
              "argument, not an edit.")
        print("blast-radius=FAIL moved=%d" % len(bad))
        return 1
    total = sum(POINTS[p][2] for p in POINTS)
    print("blast-radius=ok points=%d total=%d" % (len(POINTS), total))
    return 0


if __name__ == "__main__":
    sys.exit(main())
