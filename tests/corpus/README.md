# The corpus

Real programs, and what they are made to do - the measure phase A0 of
[docs/abi/phases.md](../../docs/abi/phases.md) holds the syscall plan against.
A syscall is worth writing when a program in here asks for it.

| File | What |
| --- | --- |
| `workloads.txt` | One workload per line: a name and the command that runs it |
| `needs/<name>.txt` | The syscalls that workload made on Linux, sorted (generated) |
| `needs/<name>.files` | The system files it opened under `/proc`, `/sys`, `/dev`, `/etc` (generated) |

The report is [docs/abi/corpus.md](../../docs/abi/corpus.md).

## Refreshing

```bash
bash scripts/dev/corpus-measure.sh build-gcc-Release    # tests/corpus/needs/
python3 scripts/dev/corpus-report.py                     # docs/abi/corpus.md
```

Both lists are checked in on purpose. A program's needs are a property of the
program and its C library, so they change when either does - and the nightly
(`corpus-needs`) measures again and fails if any workload's set moved, naming
the workload and the syscalls that came or went. When that happens because the
program changed, re-measure and commit. When it happens because the CI runner's
kernel answered differently from the machine these were measured on (a fallback
taken on one and not the other), the lists the nightly uploads are the ones to
commit: CI is the configuration this project verifies against.

## What is in it

**BusyBox** - the binary the boot image stages, which is Ubuntu's
`busybox-static`: statically linked against **glibc**, not musl. Sixteen
workloads over the shell and the file, text, process and identity applets.

**The musl test programs** the boot gate already runs (`tests/linux/`). They are
the check on the report itself: they run on VibeOS today, so they must come out
*ready*. If one does not, the report is wrong, not the kernel.

**Still to come**, and why each is in the plan: `sqlite3` and `lua` built against
musl from their released sources, and the Linux Test Project's syscall tests. All
three need their sources downloaded, which has not been done yet.

## Method

`corpus-measure.sh` runs each command directly under `strace -f` - never through
the host's shell, whose own syscalls would be measured instead; a pipeline goes
through the program's own shell. It records every syscall asked for, including
the ones Linux answered with ENOSYS: a program that asks needs an answer.
