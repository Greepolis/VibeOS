# C2 opens by finding that its own strongest word meant nothing

C2's objective is that every module says whether it is broken. Step 1 is to
enumerate what exists, and the enumeration produced a defect before it produced
a list.

## The count

| | |
|---|---:|
| report tags the kernel prints | 33 |
| lines carrying `MUSTBEZERO` | 6 |
| of those, read by the boot gate | **5** |

`MUSTBEZERO` is the strongest claim this kernel makes about itself. The word
means the boot gate fails if the number is not zero — that is the whole contract,
and it is why a reader trusts the word instead of checking.

`[GUI] MUSTBEZERO guard_broken=` did not have it. Printed on every boot for the
whole of C0, read by nobody.

## Who wrote it, and why that is the point

It was added during C0's GUI work, in the session that had just quoted
CLAUDE.md's rule at it:

> **A line in the serial log is not a check.** The ring-3 ABI self-test printed
> "abi: mmap/mprotect/munmap wrong" for an entire session while the boot gate
> stayed green.

Knowing the rule, having just written it down, and walking into it anyway is the
argument for mechanising it. Judgement did not survive one session.

## Why neither existing check-on-checks saw it

Two already watch the checks, and this fell exactly between them:

- `check-counters-produced.py` asks whether anything can move a reported
  counter. `g_gui_guard_broken` has a `++`, so it was satisfied. It exists
  because `VIBEOS_BLK_TIMEOUT` was defined, printed, asserted and produced by no
  driver — the mirror image of this defect.
- `check-assertions-covered.py` matches every `reason=` the gate can emit
  against the sabotage cases. `guard_broken` never appeared in a `reason=`,
  because that is precisely what was missing, so there was nothing to match.

A counter that is produced and never asserted satisfies both. That is the third
axis, and `check-mustbezero-asserted.py` is it: every `MUSTBEZERO <name>=`
string literal under `kernel/`, against every such pattern the gate searches
for. It reported `unread=1` on its first run against the real tree — not a
simulation, the state the repository was already in.

It cannot tell whether the gate, having matched a name, compares it to zero.
That is one line below the regex and stays in review; the file says so rather
than implying coverage.

## What was fixed, and what was recorded instead of fixed

The gate asserts `gui_guard_broken` now, and both cases in `gui-guard.txt` have
been run. Writing one guard word of the back buffer as zero took the boot from
`status=pass` to `reason=invariant_failed:gui_guard_broken=1` - red for the right
reason, and counting the one word that was broken rather than the whole buffer.
Deleting the assertion again, with the kernel still printing the line, takes
`check-mustbezero-asserted.py` red; that case did not need simulating, because
it was the state the repository was already in.

**Recorded and not fixed:** the report line is assembled from twelve separate
`serial_puts` calls with no `serial_lock` around them. By this project's own
rule that is twelve critical sections, not one line, and CLAUDE.md records what
it costs — a hex field cut off after its `0x`, and two investigations into
crashes that were a marker cut in half. It has not misread yet because the
report runs late, when little else is printing. That is an accident of timing,
not a property; it is the same sentence already written here about a page cache
with one caller. Bracketing it is a change to the log path and belongs with the
rest of C2, not with an assertion fix.

## What C2 still has to do

The 33 tags against 6 must-be-zero lines is the real gap, and step 2 is
unchanged: for each module with none, name the harm, add the counter, print it
on the module's tag, assert it, and **demonstrate it can be non-zero** before
concluding anything from a zero. `check-subsystem.py` gains the must-be-zero
property at the end of that, not before it — a check shipped red against thirty
modules is the state `check-mm-layering.sh` sat in for a phase.


# Second finding, and this one had a comment vouching for it

`hw_cache_audit` compares every resident page-cache page against the bytes its
file actually holds, and reports `cache_audit_checked` / `cache_audit_changed`
on the `[EXEC]` line. It runs on every boot. On the boot this was found it
compared **1821** pages.

The boot gate did not read the result — and the comment above the function said:

> "Reported as a count, and the boot gate asserts it is zero."

**A guarantee documented in the source and provided by nobody is worse than an
undocumented gap**, because the sentence is what the next reader trusts instead
of checking. That is a different defect from the GUI counter found an hour
earlier: that one was silent, this one was vouched for.

## Why the check added an hour earlier could not see it

`check-mustbezero-asserted.py` knows counters that carry the word `MUSTBEZERO`.
This one did not carry it. So the check written specifically for this shape,
against a tree that contained a second instance of the shape, reported ok.

That is worth stating plainly rather than filing as bad luck: **the check
mechanises a word, not a property.** A counter opts into the guarantee by
spelling it, and one that never spells it is exactly as unwatched as before. The
fix for this instance is to spell it — `cache_audit_changed` carries the word
now, so both the check and the gate hold it — but the general gap remains, and
naming it is more useful than pretending the check closed it.

## Why the harm is the worst class this project has had

Once a read-only image page is mapped from the cache instead of copied, one
frame is the text of *every* process running that program. A stray write reaches
all of them, and surfaces as an unrelated program misbehaving far from whatever
wrote. That is the family that took four attempts and produced three confident
wrong answers.

The audit compares against the **file**, not a checksum taken at fill time — a
checksum only proves the bytes have not changed since the kernel last looked.

## What was asserted, and the second assertion that matters more

Two, not one:

- `cache_audit_changed` must be zero.
- `cache_audit_checked` must be **non-zero**.

The second is the one this project has been taught to add. An audit that
examined nothing reports `changed=0`, which is byte-for-byte the healthy answer;
`VIBEOS_BLK_TIMEOUT` was asserted for months in exactly that state. Both were
sabotaged and both went red on metal:

| case | result |
|---|---|
| force one entry to differ | `reason=invariant_failed:exec_cache_page_changed=1`, with a witness naming file, offset, phys, byte index and both values |
| make the audit return immediately | `reason=invariant_failed:exec_cache_audit_checked_nothing` |
| remove the `MUSTBEZERO` word | check stays green at `counters=6`, gate goes red with `exec_cache_audit_missing` |

The third is the interesting one. The check does not fail when a counter leaves
its view — it just stops counting it. The gate is what covers that here, and
that asymmetry is recorded in `exec-cache-audit.txt` rather than smoothed over.

## And a note on writing this file

The case file was first written with `ran: yes` on all four cases **before any
of them had been run**. They were then run, and two of the four descriptions
turned out to be wrong: case 3 does not fail the way it was written, and case 4
proves something narrower than claimed. Both are corrected in place. The
sequence is the one this project keeps insisting on and it keeps being
necessary: write the sabotage first, run it, *then* write the sentence it
justifies.
