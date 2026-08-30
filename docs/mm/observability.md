# Memory Management: Observability Contract

*Part of the [memory management rewrite plan](README.md): [architecture](architecture.md) · [observability](observability.md) · [maintainability](maintainability.md) · [phases](phases.md) · [decisions and risks](decisions.md).*
Every layer exports counters from its first commit, including counters for
subsystems that do not exist yet. This is deliberate: the reason the page cache
is easy to add later is that the place to report its hit rate already exists,
and the reason a regression is caught is that the number was being watched
before it moved.

Exposed through one structure, printed by the kernel console's `meminfo`
command and asserted by the boot gate.

| Counter | Layer | Meaning | Gate assertion |
| --- | --- | --- | --- |
| `frames_total` | L0 | frames the table describes | non-zero; covers the whole region |
| `frames_free` | L0 | owners == 0 | non-zero at every stage |
| `frames_allocated` | L0 | in use | matches total minus free |
| `frames_leaked` | L0 | put refused for lack of an entry | **must be zero** |
| `frames_double_put` | L0 | put on a frame with no owners | **must be zero** |
| `poison_hits` | L0 | write to a page after it was freed | **must be zero** |
| `maps` / `unmaps` | L1 | PTEs created and destroyed | maps ≥ unmaps; converge at teardown |
| `cow_shared` / `cow_copied` | L1 | pages shared by fork, copies forced | both non-zero |
| `tlb_shootdowns` / `tlb_acks` / `tlb_timeouts` | L1 | cross-core invalidation | acks ≥ shootdowns; timeouts zero |
| `faults_resolved` / `faults_fatal` | L1 | page faults by outcome | fatal count matches the deliberate ones |
| `vmas_live` | L2 | regions per process | returns to zero when a process exits |
| `cache_hits` / `cache_misses` | L3 | page cache | present from P0, zero until P4 |
| `cache_evictions` / `cache_writebacks` | L3 | | |
| `swap_ins` / `swap_outs` / `swap_used` | L3 | | present from P0, zero until P5 |
| `reclaim_scans` / `reclaim_freed` | L4 | | present from P0, zero until P6 |

Log events follow the existing levelled scheme: every allocation, mapping and
release is recorded at `DEBUG` in the ring, printed only at `INFO` and above.
The ring is already 2048 entries and a boot uses about 500.

