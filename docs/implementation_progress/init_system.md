# Init System Progress

Status: In Progress (native ring-3 init is PID 1 and supervises a manifest of four services in the guest - start, clean stop, bounded restart, and a real crash - all gated; dependency ordering and backoff remain a host-tested model)
Last review: 2026-08-29

## Implemented
- Init service bootstrap stub in `user/init/init_system.c`.
- Service startup orchestration integrated with service manager.
- Start/stop lifecycle controls for core user-space services.
- Dependency-aware init graph execution (`vibeos_init_graph_start`) with explicit unresolved-node reporting.
- Restart-class policy controls for core vs optional services (`vibeos_init_restart_policy`, `vibeos_init_restart_note`, `vibeos_init_restart_allowed`).
- Dependency resolution now evaluates only enabled nodes; disabled optional services no longer make an otherwise valid startup graph fail. Regression coverage verifies the enabled/failed counts.
- Service-manager startup now commits the manager to `RUNNING` only after all dependencies start; each failure path rolls back already-started services and resets the supervised count.
- Native service manifest and runtime snapshot contracts are defined in `include/vibeos/userland.h` and validated host-side.
- `user/servicemgr/supervisor.c` provides a bounded runtime supervisor model with dependency ordering, `STARTING`/`RUNNING`/`FAILED` states, restart limits and exponential backoff. The model is covered by the kernel host regression suite.
- **The native ring-3 init is PID 1.** `user/prog/init.c` is what the kernel
  loads from the boot volume as `EFI/BOOT/INIT.ELF`; it forks, execs the
  bring-up workload as a child (`EFI/BOOT/SELFTEST.ELF`), reports the pid it is
  now responsible for, and waits for it. Gated on `NATIVE_INIT_READY` and on
  `NATIVE_INIT_CHILD_PID=` - the second because an init that exec'd the
  workload in place would produce an otherwise identical boot, so without it
  the gate would prove only that init ran and nothing about what it is for.

  The workload is supervised rather than replaced on purpose: it *was* PID 1,
  which is why there had been no process left over to supervise anything.
  Making init its parent moved the boundary without trading away any of the
  coverage that already existed.
- Media generation now exposes an explicit `VIBEOS_USE_NATIVE_INIT=ON` switch to select that candidate as `INIT.ELF`; the default remains the legacy bring-up workload until the native-init QEMU acceptance gate is green.
- Supervisor startup now fails closed when dependency resolution stops before all manifest nodes are running, including cyclic dependency graphs; host regression coverage verifies the bounded failure.
- Supervisor lifecycle now distinguishes clean `STOPPED` exits from `FAILED` faults, applies restart policy accordingly and marks a service failed only after its bounded restart limit is exhausted.
- Supervisor runtime snapshots now support explicit service-to-PID binding, reverse lookup and unbind with collision rejection; this is host-verified groundwork for process-backed init supervision.
- Exit events can now be reported by PID (`vibeos_service_supervisor_report_exit_pid`), atomically unbinding the process and applying service restart policy. Supervisor clock ticks remain successful while exposing terminal `FAILED` state through health snapshots.
- The x86_64 bring-up now initializes a bounded native supervisor manifest and binds the initial runtime task to the init service PID; task exit reports are forwarded through the PID-based API. Automatic process restart and QEMU acceptance remain pending.
- Supervisor adapters may now provide bounded spawn/stop hooks. After a fault, the replacement PID is bound only after the spawn hook succeeds, then the service returns to `RUNNING`; host tests cover the complete PID replacement transition.


### Runtime supervision (verified in the guest, not only on the host)

`user/prog/init.c` runs a manifest of four services and applies each one's own
policy to whatever exits. The boot gate asserts the behaviour rather than the
log markers, and every assertion below has a sabotage case in
`scripts/dev/cases/services.txt` that was confirmed to turn it red:

| Service | What it does | What it proves |
| --- | --- | --- |
| `selftest` | the bring-up workload; ends by exec'ing the shell | a session, not a daemon: whatever status it leaves with ends the run and is not a fault |
| `svc-ok` | exits 0 | a clean stop is left stopped. A supervisor that restarts everything produces a busy, plausible log and an infinite loop |
| `svc-flap` | exits 3, every time | restarts are applied *and* bounded: twice, then `FAILED` and left alone |
| `svc-crash` | dereferences null | the kernel kills the faulting task and the machine keeps running |

`svc-crash` is the one that changed the kernel. Every other service dies by
exiting, which is a cooperative death that never reaches the trap handler, so
"the kernel stays up after a service crashes" was gated, green, and false: a
ring-3 fault panicked the machine, under a comment saying there were no user
processes on metal. That had been true once. One null dereference - or one
`hlt` - from any unprivileged program halted VibeOS.

It was found from the other end. A boot wedged in the BusyBox phase; the trap
dump named a #GP at a ring-3 address, which turned out to be the `hlt` inside
musl's `__stack_chk_fail` stub in a test binary. The stack-check failure itself
is still open (see Pending); what it exposed was that the kernel's response to
*any* ring-3 fault was to stop.

A ring-3 fault now kills the task with the signal Linux reports for that vector
(#PF and #GP to `SIGSEGV`, #UD to `SIGILL`, #DE to `SIGFPE`). A kernel-mode
fault is still fatal - there is nothing to kill but itself.

That fix in turn exposed one in init: a service killed by a signal carries the
signal in the low seven bits of the wait status and leaves the exit-code byte
zero, so an init reading only the code byte reported a segfault as a clean
stop. `svc-crash` came back `STOPPED` until `SVC_KILLED` existed.


### Ctrl-C and process groups

The foreground-process-group machinery (`g_console_foreground_pgid`,
`vibeos_x86_64_console_interrupt`, `setpgid`, `TIOCSPGRP`) has been present and
correct for some time, and was reachable only from the PS/2 keyboard IRQ. The
serial console - the one VibeOS is actually driven through, in CI and by hand -
discarded `0x03` along with every other control byte in `kernel_cli_read_line`.
The mechanism existed and no input could get to it, which is why nothing
noticed. Ctrl-C is now routed to the signal path from there too, and the boot
gate asserts it: sabotaging the branch turns the boot red.

What is **not** gated is the delivery itself. By the time the kernel CLI runs,
userland has finished - init exits when its one session service does - so there
is no foreground process for the signal to reach. The instrumentation says so
in the log rather than leaving it to be assumed:

    [SIG] console interrupt: delivered=0x0 newest_pid=0x0 fg_pgid=0x1

A sabotage case that removes the delivery call was written and came back NOT
RED, which is a fact about this boot rather than a hole in the gate; it is kept
in `scripts/dev/cases/ctrl-c.txt` as a comment so the next person does not
rediscover it by hand. Gating delivery needs a live foreground job at the
moment the key arrives.

## Pending
- Dependency ordering and exponential restart backoff exist only in the host-tested model (`user/servicemgr/supervisor.c`). The manifest the guest actually runs has no dependencies between its services and restarts immediately; the two are not yet the same code.
- Declarative/service-config source of truth for init graph nodes (currently call-site supplied).
- Ctrl-C delivery to a live foreground job is not gated (see above); only the
  routing of the key into the signal path is.
- Stronger failure domains. A crashing service is now contained to its own task, but nothing yet limits what one service can do to shared kernel state before it faults.
- **Intermittent corruption of a user process's memory after fork, ~1 boot in
  32.** Two sightings, one family: musl tripping over its own malloc free list
  (`nontrivial_free`, writing through a chunk pointer read out of the heap),
  and a local variable holding a stale user pointer where `fork()`'s return
  value should be - the child took the parent's branch and the signal test
  never completed.

  One cause found and fixed: `frame_ref_inc`/`frame_ref_dec` were a plain
  read-modify-write on a byte, called from several cores at once. A lost
  increment leaves the count one owner short, so a shared page is freed while
  another process is still running from it. They are compare-exchange now. The
  musl-heap signature has not reappeared in 48 boots; the second signature
  appeared once in 32 after the fix, so the family is not closed.

  Second cause found and fixed: there was no TLB shootdown. `hw_share_user_leaf`
  clears PTE_WRITE to mark a page copy-on-write and called `hw_invlpg`, which
  reaches the calling core's TLB and nothing else. A thread of the same process
  on another core kept its cached writable entry and wrote straight through
  into a page the child had just been given a share of - no fault, no copy, and
  the damage surfacing much later in whichever program the page ended up
  serving.

  fork now sends a fixed-delivery IPI (vector 0xFE) and waits for each target
  to reload CR3, once per fork rather than once per page. The wait is what
  makes it a barrier: when it returns, no other core can still be writing
  through the permission just revoked. It is bounded and says so if a core does
  not answer, because a shootdown that hangs is a silent machine.

  It goes only to the cores actually running that address space. The first
  version broadcast to everybody and timed out about three boots in
  thirty-two: `syscall` clears IF (SFMASK is 0x200), so a core sitting in a
  syscall cannot take the IPI until it returns to ring 3, and with this much
  serial output that is most cores most of the time. Waiting on cores with no
  stake in the mapping bought nothing and was paid for in stalls. A
  single-threaded fork - nearly all of them - now sends nothing and waits for
  nothing.

  The copy-on-write *fault* needs the same treatment, and that was found the
  hard way. When it copies, the page's physical address changes; a thread on
  another core keeps the old frame and from then on reads and writes a page
  nobody else can see. `TFORK.ELF` hung outright on it: the flag its worker
  was spinning on lived on a copied page, so the store the parent made was
  invisible to the worker forever.

  That exposed a second requirement: once one core resolves a copy-on-write
  fault, another thread can still fault on a stale entry for a page that is now
  plainly writable. `hw_handle_cow_fault` used to see no COW bit and let the
  task be killed for a protection violation it had not committed; it now
  recognises the already-writable case and just invalidates.

  `tests/linux/musl_tfork.c` (staged as `TFORK.ELF`) is what exercises any of
  this: it forks while a worker thread is still running and checks that the
  child keeps the pages it forked with. Nothing else in the boot produces that
  shape, and before it existed the shootdown count was zero on every boot -
  the code was correct in principle and never once executed.

  What is gated is the mechanism, not the absence of the corruption:
  `COW_STATS` reports `tlb_shootdowns` and `tlb_acks`, and the boot fails if
  the threaded fork shot down nothing, if acknowledgements came back short, or
  if a shootdown timed out. Three sabotage cases in
  `scripts/dev/cases/tlb-shootdown.txt` each turn the boot red - and the case
  file records which check caught which, because removing the shootdown is
  caught by the counter rather than by TFORK's own data check.

  A corruption that appears one boot in thirty cannot be asserted on in a
  single boot. Pretending otherwise is how the earlier claims here came to be
  wrong, twice.

- **A hang in the bootloader phase, ~1 boot in 24.** Previously written up here
  as a benign cold-start stall; that was wrong. Raising the per-boot budget
  from 180s to 300s does not help - the guest stays silent for 296 of 300
  seconds at `bootloader_exit_boot_services` - so it is a hang and not
  slowness. Not yet investigated. Two signatures seen, and they
  may be one bug: a stack-check failure in the musl signal test (the canary is
  read through `%fs:0x28`, and the sigframe saves the trapframe, which does not
  carry `FS_BASE`), and init faulting inside its own `say()` while reading a
  pointer that landed in kernel memory - `rip` in `say`, `cr2=0x21b6000`,
  `err=0x5`, meaning the page is present but supervisor-only. Both are a user
  program dereferencing an address that is not its own, which is the family
  described under copy-on-write and the exec staging buffer in CLAUDE.md.

  Two things changed about it rather than one. It used to halt the machine
  silently; a ring-3 fault now kills the task, and the boot gate asserts that
  exactly one deliberate ring-3 fault occurs, so this shows up as a named
  failure (`deliberate_ring3_faults=2_expected=1`) instead of a wedge. Not yet
  instrumented far enough to say which pointer, so this is evidence and not a
  diagnosis. The canary is read through `%fs:0x28`, so a TLS base that is not preserved across signal delivery or fork would produce exactly this, and the sigframe itself saves the trapframe, which does not carry `FS_BASE`. Not yet instrumented, so this is a lead and not a diagnosis.
- Declarative init configuration format and validation.
- Runtime health probes and process-backed failure-domain isolation remain pending.

## Next checkpoint
- Replace fixed runtime program selection with ring-3 init execution and service supervision.
