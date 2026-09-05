# P4 step 3 — the staging buffer stops being a requirement

`execve` read every program into a 4 MiB staging buffer, and the interpreter
into another 2 MiB one, so that a parser could look at the first hundred bytes
and a handful of pages could be copied out. Six megabytes of kernel memory, and
a whole-file read on every exec, to serve two things that need a few kilobytes
between them.

This step removes the *need* for it. Both consumers now ask for bytes instead of
indexing a buffer.

## The page fill

`vibeos_elf_fill_page_via` takes a reader. A page of virtual address maps to at
most one contiguous file range per segment, so it issues at most one read per
segment that covers the page — not one per byte, and not one for the whole file.

The kernel's reader serves those bytes from the page cache, which is where they
already are: X-P2 maps most image pages straight out of it, and the ones that
have to be copied were the only reason the file was in the staging buffer at
all. A range can straddle two cache pages, and the loop that handles it is not
decoration — a segment's file offset has no reason to be page-aligned, and a
version assuming one lookup per read would corrupt every page whose data
crossed a boundary, which is most of them and only for some programs.

A read that fails leaves the rest of the page **zeroed rather than
half-written**. A partially filled page of program text is indistinguishable
from a correct one until it is executed.

The pointer form is kept as a reader over memory, so the host tests and the
bootloader — which have the whole file to hand — do not have to invent a
callback. Same code path, deliberately: two fills that could drift apart is how
the copied pages and the mapped pages come to disagree about a program.

## The parse

Everything the parser needs is in the first few kilobytes — the ELF header and
the program headers — with exactly one exception: the interpreter path, which
is the contents of a PT_INTERP segment at an arbitrary file offset. That single
access is the entire reason `image` had to be the whole file.

`vibeos_elf_parse_read` takes a reader for it. Pass null and it behaves exactly
as before, taking the path from the buffer and refusing a range that reaches
past it.

One thing moved while doing this: the check that the path is terminated inside
its own segment now happens *after* the copy rather than before it. With a
reader the bytes are not addressable until they have been fetched, and checking
a different copy from the one that gets used is how the two come to disagree.

## What is not done, and what it is waiting for

**The buffers are still there, and still 6 MiB.** Nothing needs them now, but
shrinking them is blocked on something outside this phase.

`hw_read_file_cached` returns one number that means two things: how long the
file is, and how many bytes it put in the buffer. Today those are the same
because the buffer holds the whole file. Shrink the buffer and they diverge —
and the parse needs the *file* length for its bounds checks while the buffer
only holds the headers. Passing the staged count would make the parser refuse
every segment past the window, which is every segment.

Separating them is a change to the I/O path, and that path's truncation
semantics are the subject of one of this project's most expensive lessons: a
FAT read that reported a short read as a complete file, so `execve` parsed
whatever the previous program had left in the shared buffer. `g_fat_chain_error`
and a byte count that reflects what was actually copied exist because of it.

So this belongs to the disk-and-I/O refactor rather than to the memory manager,
and doing it here would mean changing that function's contract in passing —
which is how that defect happened the first time.

What this phase leaves behind is that the *dependency* is gone: the staging
buffer is an optimisation now, not a requirement. When the I/O layer can say
"the file is N bytes, I gave you M of them", both buffers can shrink to a
header window and the refusal for a file whose headers exceed it is one line.

---

## Update, after I3 of docs/io/

The I/O layer can say it now: the read reports the file's length and stages
only what fits, `vibeos_elf_header_extent` says how much of a file the parser
needs, and `hw_proc_create` takes both numbers with the signature saying which
is which. The refusal for a file whose headers exceed the window is one line,
as predicted.

**The buffers still do not shrink, and the reason changed.** It is no longer a
design problem; it is a defect with a reproduction.

Setting both windows to 64 KiB works and runs every program on the media, and
turns the gate red with `mm_poison_hits=3019` deterministically. Shrinking
either window alone is clean; only both together fail; and wasting the six
megabytes the two shrinks free makes the poison disappear while the windows
stay small. So the shrink changes only *which frames the allocator hands out*,
and something writes to a frame it has already released.

That is the use-after-free this subsystem has been chasing at roughly one boot
in sixteen — now reproducible on demand, 3019 times a boot. Written up in
[io_read_contract.md](io_read_contract.md), with the recipe recorded in
`arch_hw.c` beside the constant somebody would change.


---

## Closed

The windows are 64 KiB each and P4 step 3 is done. What held it up was not a
use-after-free: `vibeos_frame_init` never initialised the descriptor flags, so
the poison detector judged virgin frames and reported 3019 corruptions in a boot
with none. The staging sizes only decided where that descriptor table landed in
physical memory, which is why the symptom tracked them so convincingly.

Written up in [mm_frame_init_flags.md](mm_frame_init_flags.md), with the
measurement that settled it and the two tools that were wrong on the way.
