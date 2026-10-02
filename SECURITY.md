# Security policy

VibeOS is a research kernel. It is not hardened for production use, and it has
no supported releases: only the current `main` branch is maintained, and a fix
lands there.

## Reporting a vulnerability

Please report a vulnerability privately, not in a public issue:

- <https://github.com/Greepolis/VibeOS/security/advisories/new>

Say what is affected, how to reproduce it (a program, a disk image, a syscall
sequence) and what an attacker gains. A serial log of the boot is the most
useful attachment there is.

## Disclosure and timeline

- A report is acknowledged within 7 days.
- An assessment - accepted or not, and why - follows within 30 days.
- The vulnerability is disclosed in a published advisory when the fix is on
  `main`, or 90 days after the report, whichever comes first. The reporter is
  credited unless they ask not to be.

## Scope

In scope: the kernel, the bootloader, the Linux ABI layer and the scripts and
workflows of this repository. `third_party/` is reported to its own upstream.

The design of the security model is described in
[docs/SECURITY.md](docs/SECURITY.md).
