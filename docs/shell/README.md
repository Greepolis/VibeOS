# The user's shell

Item 1 of [next-steps.md](../next-steps.md). Designed before written, and
decided with the project's owner on 2026-11-19.

| File | What |
|---|---|
| [design.md](design.md) | The design: the session and login, the native shell `vsh` and its syntax, bash and cmd beside it, scripts run by their type, the names in `/usr/bin` |
| [s1.md](s1.md) | S1: job control and pseudo-terminals in the kernel - the plan, step by step, and its status |
| [s2.md](s2.md) | S2: login - the accounts, first boot, the session |
| [s3.md](s3.md) | S3: the names in `/usr/bin` |

The phases, each with its own file once it starts:

1. **S1** - the kernel: stop and continue for a process, the terminal's
   job-control signals, pseudo-terminals.
2. **S2** - login: `/etc/passwd`, `/etc/shadow`, `login` on the console, the
   session.
3. **S3** - the names in `/usr/bin` (E4-021).
4. **S4** - `vsh`: the language and the line editor.
5. **S5** - bash built against musl, and scripts run by their type.
6. **S6** - cmd, Wine's, through the NT personality.
7. **S7** - FinalCommander, Linux's build and Windows', as the workload that
   proves the rest.
