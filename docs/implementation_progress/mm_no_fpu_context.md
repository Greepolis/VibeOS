# SSE is enabled and never saved

The root cause of the lost store, of the argv vector that reads as white pixels,
and very likely of a good part of this project's "one boot in N" family.

## The finding, in four numbers

```
fxsave / fxrstor / xsave, whole tree :     0
XMM instructions in the kernel image :  6955
XMM instructions in svc-stress       :   782
CR4.OSFXSR | CR4.OSXMMEXCPT          :   set, in ap_boot.S
```

SSE is turned on. Both the kernel and user programs use it heavily. **Nothing
anywhere saves or restores XMM or FPU state.**

So a user task's XMM registers survive a syscall, an interrupt, or a context
switch only by luck. Not a race with a fine window — the kernel executes
vectorised code on the way in and hands the registers back with whatever it left
there.

## How that produces exactly sixteen NUL bytes

`svc_stress` builds its output in `char line[128]` on the stack. Its
disassembly, at `0x800000021e`:

```asm
pxor   %xmm0,%xmm0
movdqa %xmm0,0x20(%rsp)      ; sixteen bytes of zeros, in one store
```

clang uses a 16-byte XMM store for stack buffers. `put_str` copying
`"STRESS_OK rounds="` does the same thing in the other direction: load sixteen
bytes into an XMM register, store them.

Preempt between the load and the store, let anything else run, and the store
writes what that something left behind. **Sixteen bytes, always exactly
sixteen** — one XMM register — which is the signature that had no explanation
for the whole investigation.

And the value is zero because svc-stress forks about a hundred children *running
the same binary*, and that binary opens with `pxor %xmm0,%xmm0`. The clobbering
value is overwhelmingly likely to be zero, which is why the corruption looked
like a freshly-zeroed page and sent ten hypotheses into the memory manager.

## And why the argv vector reads as white pixels

`gui.c`:

```c
#define COL_TITLETXT 0x00FFFFFFu
#define COL_CURSOR   0x00FFFFFFu
```

The desktop fills rectangles with that colour, from the kernel, on timer and
input interrupts — interrupts that land on whatever user task is running. A
vectorised pixel fill holds `0x00ffffff00ffffff` in an XMM register: two white
pixels in one 64-bit lane.

The task resumes with that register still loaded and stores it into its own
buffer. `mm-argv-poison.txt` records the value as
**`0x00ffffff00ffffff`**, and the same value turning up independently in the
block layer as an out-of-range sector.

It was read as "the desktop is painting over memory that belongs to something
else". It is the exact opposite: **the desktop is not touching that memory at
all.** It left a value in a register, and the process wrote it there itself.

That is why every memory detector read zero and was right, and why the code
comment above the back-buffer allocation - which describes a real, different,
already-fixed defect with the same symptom - sent the investigation into the
allocator four separate times.

## The ten hypotheses this closes

Every one was killed by measurement before the cause was found, and every one
was in the memory manager because the symptom looked like memory:

| | Killed by |
|---|---|
| back buffer shadows the low user window | `SHADOWS_USER=0`; `pmm_reserve` truncates the region |
| framebuffer inside managed RAM | `FB_IN_RAM=0` |
| desktop overruns its buffer | canary page, intact on failing boots |
| frames handed out twice | `double_allocs=0`, `backbuf_shared=0`, `backbuf_lost=0` |
| the kernel reads the page wrongly | `first8 == again` |
| the buffer crosses a page | offset `0xc60`, 21 bytes |
| the copy-on-write copy loses data | the page fold, validated by sabotage |
| a stale TLB entry survives migration | no `PTE_GLOBAL`, no `CR4.PGE` — CR3 reloads flush |
| compaction moves the frame | reclaim never runs: `scans=0` |
| a frame is freed while still mapped | **every** free checked, six boots, zero |

The last one mattered: that check samples one free in sixteen by default, so its
zero had been weak evidence for months. It is a build knob now
(`HW_FREE_CHECK_MASK`) rather than a literal somebody edits and puts back, which
is what had happened twice.

None of these was wrong to test. What is worth noticing is that the shape of the
evidence — a page reading zero where a process wrote — pointed at memory so
strongly that ten separate mechanisms were checked before anybody asked what
else is exactly sixteen bytes wide.

## What has to happen

Save and restore FPU/SSE state per task. The shape is standard and none of it is
subtle:

- a 512-byte, 16-byte-aligned `fxsave` area per task;
- `fxsave` on the way out of a task and `fxrstor` on the way in, in
  `hw_task_load_cpu_state` and its counterpart;
- an initial state for a new task, so it does not inherit its predecessor's
  registers;
- a decision about laziness. Eager save on every switch is simple and correct
  and costs ~100 cycles; `CR0.TS` with a trap on first use is the classic
  optimisation and is worth measuring against the C0 baseline rather than
  assuming.

**This is not written as done.** It is a real change to the context switch, the
single most dangerous function in this kernel, and it deserves its own phase
with its own sabotage cases — starting with the one that already exists in
effect: `ring3_write_nul`, which is non-zero three to five boots in ten today
and must read zero afterwards.

## What to gate

`ring3_write_nul` and `cow_copy_changed` both belong in the boot gate's
must-be-zero set once the fix lands. Neither is asserted yet, because a check
that starts red is a check nobody reads.

The stronger gate is the one this finding suggests: a user program that fills
XMM with a known pattern, does syscalls and lets itself be preempted, and checks
the registers still hold it. That is a test the machine can fail deterministically
instead of three times in ten.
