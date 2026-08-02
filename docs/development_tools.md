# Development Tools

What a development machine needs, and what each thing is actually for. Nothing
here is required to *build* VibeOS beyond the first section - the rest exists
to answer questions that are otherwise answered by guessing.

## Install

On Ubuntu or WSL:

```bash
# build and boot
sudo apt install -y cmake ninja-build build-essential clang python3 \
                    qemu-system-x86 qemu-utils ovmf xorriso

# diagnosis and compatibility work
sudo apt install -y strace gdb musl-tools busybox-static gcovr lcov
```

## Build and boot

| Tool | Why |
| --- | --- |
| `cmake` (3.21+), `ninja` | The build. Ninja is the default generator; the Windows script falls back to a manual GCC compile when neither is present. |
| `gcc` **and** `clang` | Both are gated in CI, in Debug and Release. They disagree about enough - alignment assumptions, optimisation of inline assembly, PE linking - that each has caught bugs the other missed. Building with only one is building half the matrix. |
| `qemu-system-x86`, `ovmf` | The boot gates. OVMF is the UEFI firmware; without it there is no boot path to test. |
| `qemu-utils` | `qemu-img`, which produces the `.vdi` and `.vmdk` images. |
| `xorriso` | The bootable `.iso`. Optional: its absence only removes that one artifact. |
| `python3` | The boot smoke harness, the FAT16 image writer, and the ELF-to-C blob converter. |

Run everything through `scripts/`; see the README for the commands.

## Diagnosis

| Tool | Why |
| --- | --- |
| `strace` | The ground truth for Linux compatibility. Run a real static binary on real Linux and read the exact syscalls it makes, in order, instead of predicting them. This is how the startup set was confirmed rather than guessed - see below. |
| `gdb` | Remote kernel debugging against `qemu -s -S`. Reaches what serial output cannot: the state of a guest that has stopped talking. |
| `nm`, `readelf`, `objdump` (binutils) | Already present with the compiler. `nm` on the kernel ELF turns a raw RIP from the QEMU monitor into a function name, which is how two intermittent hangs were diagnosed - the hang was inside code holding the lock the logger needed, so no amount of extra logging would have found it. |
| QEMU monitor | Not a package. `info registers -a` dumps every core's state on a frozen guest. When the guest has gone quiet, this is the only channel left. |
| `gcovr`, `lcov` | Coverage over the host suites: which branches of the ELF parser and the TCP/IP stack the tests never reach. A test suite that passes says nothing about the code it never executed. |

## Compatibility targets

| Tool | Why |
| --- | --- |
| `musl-tools` (`musl-gcc`) | Builds a genuinely unmodified static Linux binary, small enough to load. A static musl hello is about 47 KB; the same program against static glibc is closer to a megabyte and calls a far larger startup set, so musl is both the easier target and the more honest first one. |
| `busybox-static` | A real, non-trivial static binary - shell, `ls`, `cat` - as the target after the musl hello. It is the step from "a libc starts" to "a program works". |

### What a static musl binary actually asks for

Recorded with `strace` on Linux, in order, for a `printf` and `malloc` program:

```
execve  arch_prctl  set_tid_address  brk  brk  mmap  mmap
ioctl  writev  munmap  writev  exit_group
```

Every one of those is implemented. That list is the reason the startup syscall
work targeted what it did, and it is worth re-recording whenever a new target
binary is chosen: the set is a property of the libc, not of the kernel, and
guessing at it produces stubs nobody needs alongside gaps that stop everything.

The remaining blocker is not a syscall. A Linux executable is linked at
`0x400000`, which falls in the first 512 GiB of the address space - the region
VibeOS currently maps as the supervisor-only kernel identity map, with user
memory living in the next PML4 slot instead. Loading a real binary means being
able to map user pages at the address it was linked for.
