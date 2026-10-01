# The corpus

Real programs, and what they are made to do - the measure phase A0 of
[docs/abi/phases.md](../../docs/abi/phases.md) holds the syscall plan against.
A syscall is worth writing when a program in here asks for it.

| File | What |
| --- | --- |
| `workloads.txt` | One workload per line: a name and the command that runs it |
| `needs/<name>.txt` | The syscalls that workload made on Linux, sorted (generated) |
| `needs/<name>.files` | The system files it opened under `/proc`, `/sys`, `/dev`, `/etc` (generated) |
| `sources.txt` | SQLite, Lua and LTP: pinned URL and SHA-256 |
| `ltp-built.txt`, `ltp-failed.txt` | LTP's syscall tests that build against musl, and the directories that do not (generated) |

| `run-l1.sh` | The file workloads, run for their answers: on VibeOS at boot, and on Linux by `scripts/dev/corpus-expect.sh` |
| `l1-expected.txt` | What Linux answered `run-l1.sh` with (generated; the boot gate's oracle) |

The report is [docs/abi/corpus.md](../../docs/abi/corpus.md).

## Running it, not only measuring it

`workloads.txt` says what the programs ask for. `run-l1.sh` is what they are
expected to answer (docs/abi/ L1 step 8): the same commands, shaped so that the
answer leaves out what two machines may differ in - dates, owner names, the
size of a directory, the order it lists in. It runs under the same BusyBox in
the guest and on Linux, and the boot gate compares the two outputs line for
line. After changing it:

```bash
bash scripts/dev/corpus-expect.sh build-gcc-Release      # rewrite l1-expected.txt from Linux
```

Never edit `l1-expected.txt` by hand: it is Linux's answer, and an edited one
is somebody's opinion.

## Refreshing

```bash
bash scripts/dev/corpus-build.sh build-gcc-Release      # SQLite, Lua, LTP (about 6 minutes)
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

**SQLite and Lua**, built static against musl by `scripts/dev/corpus-build.sh`
from the pinned sources in `sources.txt` (`$CORPUS` in `workloads.txt`): a
database on a file and in memory, and a script that allocates, writes a file and
reads the clock.

**The Linux Test Project's syscall tests**, built the same way - not traced but
recorded: `ltp-built.txt` lists the 1,530 that build against musl and
`ltp-failed.txt` the directories that do not. The report matches each to the
syscall it tests, so every phase knows which of its syscalls already has a
conformance oracle.

## Method

`corpus-measure.sh` runs each command directly under `strace -f` - never through
the host's shell, whose own syscalls would be measured instead; a pipeline goes
through the program's own shell. It records every syscall asked for, including
the ones Linux answered with ENOSYS: a program that asks needs an answer.
