# Contributing

## Current project phase

VibeOS is in architecture-definition stage. Before significant code lands, contributors should prefer improving design documents, clarifying interfaces, and tightening milestone criteria.

## Contribution priorities

- keep architectural boundaries explicit
- document trade-offs, not just conclusions
- prefer small logical commits
- avoid introducing implementation complexity before the relevant design is written down

## Workflow

1. Start from a focused problem statement.
2. Update the relevant design documents first when the change affects architecture or subsystem boundaries.
3. Keep commits scoped to one logical concern.
4. Record assumptions and open questions in the same patch where practical.

## Review expectations

- kernel-boundary changes need explicit rationale
- compatibility changes must state which support tier they target
- milestone changes should update exit criteria, not only task lists
- broad claims about macOS compatibility require legal and technical scrutiny

## Style direction

- prefer clear, direct English in technical docs
- keep diagrams and repository structure in sync with the actual tree
- avoid marketing language in design documents
