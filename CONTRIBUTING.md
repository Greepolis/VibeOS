# Contributing

## Current project phase

VibeOS boots, runs on every core, and executes ring-3 programs through the Linux
system-call ABI. The architecture documents still lead, but code now decides
whether they were right, so a change that contradicts a design document should
correct the document rather than quietly diverge from it.

## Contribution priorities

- keep architectural boundaries explicit
- document trade-offs, not just conclusions
- prefer small logical commits
- put portable logic in portable files so host tests can reach it; hardware
  files should be the part that cannot be tested any other way

## Workflow

1. Start from a focused problem statement.
2. Update the relevant design documents first when the change affects architecture or subsystem boundaries.
3. Keep commits scoped to one logical concern.
4. Record assumptions and open questions in the same patch where practical.

## Verification expectations

Green is a claim, and a claim has to be earned:

- A test that has never failed has not been shown to work. Break the thing it
  covers, watch it go red, then put it back.
- Verify what the change actually did, not that a call returned success. A
  syscall that reports a result it did not produce is worse than one that
  fails.
- Local runs and CI must exercise the same configuration. Verifying under
  hardware acceleration while CI runs pure emulation validates a setup that CI
  never executes.
- When a fix is based on a hypothesis, say which evidence would disprove it,
  and go looking for that evidence.

## Review expectations

- kernel-boundary changes need explicit rationale
- compatibility changes must state which support tier they target
- milestone changes should update exit criteria, not only task lists
- broad claims about macOS compatibility require legal and technical scrutiny

## Style direction

- prefer clear, direct English in technical docs
- keep diagrams and repository structure in sync with the actual tree
- avoid marketing language in design documents
