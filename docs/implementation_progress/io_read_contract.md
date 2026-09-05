# I3 — a length is not a byte count

## The conflation

`hw_read_file_cached` returned one number that meant two things: how long the
file is, and how many bytes it put in the caller's buffer. They were equal only
because it refused to proceed otherwise —

```c
if (size > (uint64_t)cap) {
    return -1;
}
```

— a refusal about the *buffer*, applied to a file the cache did not need a
buffer for. That single line is why `execve` carried six megabytes of staging:
not because anything needed the bytes contiguously, but because this function
would not run without room for all of them.

## What changed

The loop still walks every page, because that is what populates the cache, and
the cache is what the page fill and the interpreter path read through. What
changed is that **the copy stops at the caller's window** and the return value
keeps describing the file.

`hw_proc_create` now takes both numbers, and the signature says which is which:

```c
static int hw_proc_create(hw_proc_t *p, const unsigned char *elf,
                          uint64_t len, uint64_t staged, ...)
```

One caller has a program built into the kernel and passes `len` for both. The
other passes the window. Leaving a caller to assume they are equal is how they
came to be one number in the first place.

### The header extent, asked rather than recomputed

`vibeos_elf_header_extent()` says how far into a file the parser will reach for
the ELF header and the program headers. A caller that stages only a window has
to know whether its window is big enough, and the alternative to asking is
duplicating the header layout at the call site — which is how two pieces of
code come to disagree about where a program header table starts.

A file whose headers exceed the window is **refused by name**
(`headers_past_window`), not truncated. Truncation would hand the parser a
partial header table and it would describe a program that does not exist — the
same shape as the short read that once made `execve` parse the previous
program's bytes.

`vibeos_fs_size` was not needed: `vibeos_fs_node_t` already carries the file's
size, so asking how long a file is has never required a buffer that could hold
it. The conflation was one layer up.

## The buffers are still six megabytes, and that is the interesting part

Nothing needs them. Setting both windows to 64 KiB works, boots, and runs every
program on the media — and turns the gate red with **`mm_poison_hits=3019`,
deterministically, on every run**.

It is not the shrink's defect:

- shrinking **either** window alone is clean;
- only both together fail;
- and deliberately wasting the six megabytes the two shrinks free — allocating
  them at boot and never using them — makes the poison disappear **while the
  windows stay small**.

So what the shrink changes is *which frames the allocator hands out*, and
something writes to a frame it has already released. The change reveals a
latent defect rather than causing one.

### Why this matters more than the six megabytes

This is the **first deterministic reproduction** of a defect this project has
been chasing at roughly one boot in sixteen. Every previous investigation into
the `mappers=2 owners=1` family — four of them, three producing confident wrong
answers — was fighting an event that appeared once in dozens of boots. This one
appears 3019 times, on demand, every run.

The recipe is recorded in `arch_hw.c` beside the constant, where somebody
changing it will read it: set both windows to `64u * 1024u`.

The buffers stay large until the defect is closed, because six megabytes of
kernel memory on a four-hundred-megabyte machine is a smaller cost than a gate
nobody can trust.

## Verified

- `rc=0`, no warnings on gcc or clang, host tests and fuzz green, boot verified
  with the windows at their original size.
- The shrink verified to fail, and verified to fail *deterministically*, which
  is what makes it useful.
- The waste experiment verified to make it pass again, which is what makes the
  attribution sound rather than plausible.

## Not done

The buffers do not shrink, so mm's P4 step 3 stays open — but its blocker has
moved. It was "the read contract conflates two numbers"; it is now "there is a
use-after-free that only shows when the allocator's layout changes", which is a
defect with a reproduction rather than a design problem.
