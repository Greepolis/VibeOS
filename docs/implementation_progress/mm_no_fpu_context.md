# SSE was enabled and never saved (fixed)

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

## Done, and it took two halves

**One: the context switch saves and restores.** A 512-byte 16-byte-aligned area
per task, `fxsave` beside the line that already saved the integer context and
`fxrstor` beside the one that loaded it. Eager, not lazy: `CR0.TS` with a trap on
first use needs a fault handler, a per-core notion of who owns the registers, and
cross-core invalidation on migration — three mechanisms on the path this kernel
has the most defects in, against about a hundred cycles on a switch that already
costs far more.

The initial state is built by hand rather than zeroed, because `MXCSR` lives at
offset 24 and zero there unmasks every SSE exception: the first floating-point
operation in a fresh program would fault. `fork` copies the parent's area — "resume
exactly where the parent is" was only ever true of the integer registers — and a
new thread gets a clean one. The exit path restores without saving, since the
dying task's registers go with it but its successor must not inherit them.

`CR4.OSFXSR` is now set on the boot processor too. Only `ap_boot.S` ever set it,
so the BSP had it because UEFI leaves it on — a coincidence that had held, and
`fxsave` without it is `#UD`.

**Two: the kernel stops using vector registers at all**, which is what Linux does
and for the same reason. A syscall that returns to the same process never reaches
the scheduler, so nothing saves anything, and every XMM instruction the kernel
runs on that path destroys the caller's state regardless.

That half was found by measurement, not review. With only the context switch
fixed, the corruption went from three boots in ten to **one in twelve** — a large
improvement that is not a fix, and the number that would have been reported as
one had the sweep been five boots. The deterministic XMM test was already green
at that point, because it uses `getpid`, which is too trivial to touch XMM. Two
criteria, one green and one not, is what stopped the work at the halfway mark
being called finished.

The portable half needed it too, and that took a second round: `-mno-sse` on the
arch target alone left `kernel/core`, `kernel/mm`, `kernel/io` and `kernel/fs`
compiling with SSE, and they run in the kernel. They are a second library now
rather than a flag on the shared one, because the shared one is linked into the
host tests where the ABI passes floating point in XMM.

**The near-miss worth recording:** the first attempt was one library with
`if(IMAGE AND NOT TESTS)` around the flags. Both are on in every real build, so
it would have compiled to nothing while looking like a fix, under a comment
explaining why it was necessary. That is this project's most repeated defect —
configured and consulted by nobody — and it was caught by asking when the
condition is true rather than by re-reading the code inside it.

## Verified

| | before | after |
|---|---|---|
| `abi: xmm lost across a context switch` | 3 boots of 3 | **0 of 22** |
| corrupted ring-3 writes | 3–5 boots in 10 | **0 of 22** |
| host tests, clang build, boot gate | green | green |

One boot in the sweep panicked in `THREADS.ELF` with three cores on one address
space. The same failure, same phase, same signature appeared earlier the same day
on the unmodified kernel, so it is pre-existing and separate — recorded rather
than folded in.

## What to gate

`ring3_write_nul` and `cow_copy_changed` both belong in the boot gate's
must-be-zero set once the fix lands. Neither is asserted yet, because a check
that starts red is a check nobody reads.

The stronger gate is the one this finding suggests: a user program that fills
XMM with a known pattern, does syscalls and lets itself be preempted, and checks
the registers still hold it. That is a test the machine can fail deterministically
instead of three times in ten.
