# Writing a GPT, and which rule actually protects the disk (I4c)

I4c shipped with one gap: it could write an MBR and not a GPT. That is now
closed, and closing it produced a finding that changed what this file was going
to say.

## What it writes, and in what order

Five regions, in this order, with a flush between each:

1. the backup entry array
2. the backup header
3. the primary entry array
4. the primary header
5. the protective MBR

A flush between each because none of that ordering exists otherwise — the block
cache is write-back, so issuing writes in order orders nothing. That is the
same lesson `journal.c` records, and it is worth stating twice because the code
looks correct without the flushes.

The protective MBR is last on purpose. Its only job is to stop an MBR-only tool
from believing the disk is empty; writing it first would tell such a tool the
disk is spoken for before it actually is. It is an *edit* of sector 0, not a
rebuild, because that sector also holds boot code and a repartition must not
unbootable a disk it was only asked to repartition.

Partition GUIDs are derived from the disk GUID and the entry index rather than
invented. This kernel has no entropy source it could honestly call one, and a
uuid sliced out of a hash is not a uuid — that mistake cost three attempts at
the OVA, where VirtualBox parsed the malformed value to null and then reported
that an image it could see did not exist. Derived and documented beats random
and fictional.

## How it is verified

Not by writing a table and reading it back. The device model has a volatile
cache, as real drives do; a write is acknowledged immediately and sits there
until a flush, and the flush applies pending writes in an order the test
chooses. Power is cut after every possible number of landed writes, across
three flush orders — 240 arrangements — and after each one the disk is examined
as a fresh reader would examine it.

The invariant asserted is narrower than "all or nothing", and stating it
precisely is what made the results readable:

> **A GPT header that validates always describes an entry array that is
> actually on the disk.**

Plus: the boot code in sector 0 survives every prefix of the sequence, and a
primary never lands without its backup.

## The finding: the CRC protects the primary, the ordering protects the backup

The sweep was written believing the central danger was a header written before
the array it describes — a claim about bytes, made before the bytes exist,
producing a table that passes every check and points at rubbish.

Sabotaging exactly that caught **nothing**. On looking at why, the premise was
wrong. The header carries a CRC over the entry array. Written early, it
describes an array that is not there yet, so it does not validate, so a reader
rejects it and falls back to the backup — a recoverable state, not a lie. For
the sabotage to produce a lying header, the *previous* contents of the array
region would have to satisfy the new header's CRC, which is not something that
happens.

So the ordering between the primary header and its array is not load-bearing;
the CRC already is. What the ordering genuinely buys is the relationship
between the two copies — deleting the backup header write is caught
immediately, because then a primary can land alone and the disk carries two
tables that disagree with no way to say which is newer.

The ordering is kept because it costs nothing and is what the standard expects.
It is not kept because a test showed it mattered, and this file says so rather
than implying otherwise.

That is the third time in this project a test has been right about the outcome
and wrong about the mechanism. The difference this time is that the sabotage
was run *before* the claim was written down, so the claim never became folklore.

## Not journalled, deliberately

The plan for I5c assumed the partition-table writer would be the journal's
second customer — "two customers in two layers is what makes it a module rather
than a feature". Having built it, that is the wrong call for this writer.

A GPT already carries its own recovery scheme: two copies, each with a CRC over
the header and another over the entry array. A reader that checks those can
tell a good table from a torn one without help — and the sweep above
demonstrates it does. Putting a journal underneath would add a second recovery
mechanism that has to *agree* with the first, which is a second place that has
to be right; this project has spent whole phases removing second places.

What the journal is for is updates that have no such scheme of their own. The
second customer, when it arrives, should be one of those.
