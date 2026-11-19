# The shell: design

Decided 2026-11-19. Before it: `ash` from BusyBox was the shell of the
self-test and the one service of the VM images; `SH.ELF` a native shell of
`help`, `echo`, `exit` and running a program; the console a terminal (TCGETS,
raw mode, ISIG, TIOCGWINSZ) with `setsid`, `setpgid` and TIOCSPGRP. Nothing
stopped on Ctrl+Z, there was one terminal and no pseudo-terminal, and a shell
started as root without asking anybody.

## 1. The session and login

- `init` runs `login` on the console. `login` reads `/etc/passwd`, checks the
  password against `/etc/shadow` with a modern hash (yescrypt or
  sha512-crypt, the ones Linux uses), and starts the session: a new session
  and process group, the terminal made its controlling terminal, the user's
  credentials, the home directory as the working directory, and the shell
  `/etc/passwd` names.
- Login is there from the first version: there is no stage at which a shell
  starts as root unasked.
- Users' homes are casefolded (E4-030); the rest of the tree is not.

## 2. vsh, the native shell

`/usr/bin/vsh`: the login shell and the terminal's. It takes what is good in
bash and in cmd rather than choosing one, and adds what both lack.

| | bash | cmd | vsh |
|---|---|---|---|
| Variables | `$x`, `${x}` | `%x%` | `$x`, `${x}`; `%x%` read as a courtesy |
| Assignment | `x=1`, no spaces | `set x=1` | `set x = 1`, spaces allowed |
| Conditions | `if [ ... ]; then ... fi` | `if ... ( ... )` | `if cond { ... } else { ... }` |
| Loops | `for i in ...; do ... done` | `for %i in (...) do` | `for i in a b c { ... }` |
| Comparison | `[ "$a" = "$b" ]`, `-eq` | `==`, `EQU` | `==`, `!=`, `<`, `>` as expressions, no `[` |
| Pipes, redirection | `\|`, `>`, `2>&1` | the same | as bash |
| Command substitution | `$(...)` | `for /f` | `$(...)` |
| Errors | `$?`, `set -e` | `%ERRORLEVEL%` | `$?`; a script stops at its first failure unless told otherwise |
| Paths | `/` | `\`, `C:` | `/`; `C:\...` accepted and translated for Windows programs |
| Case of commands | distinct | ignored | ignored for builtins and aliases, kept for files |
| Completion | Tab | Tab cycles | Tab lists, as bash; arrows for history; Ctrl+R searches it |
| Scripts | `.sh` | `.bat`, `.cmd` | `.vsh` |

A command vsh does not know is looked up in `PATH`, as bash does. The grammar
is written down in full in S4, before its parser.

## 3. bash and cmd beside it

- **bash** is the real one, built from source against musl as SQLite and Lua
  are, at `/usr/bin/bash`. `/bin/sh` stays a POSIX shell - BusyBox's ash until
  then - because Linux programs' `system()` and `#!/bin/sh` need one.
- **cmd** is Wine's, at `C:\Windows\System32\cmd.exe`, run by the NT
  personality: decades of `.bat` files are compatible with it, and it is a
  workload that tests that personality. Writing one would be chasing its
  quirks one at a time.

## 4. A script runs by its type

As on Windows, and from the shell, the desktop and any program that calls
`execve` alike: a table in `/etc/vibeos/binfmt` - the shape of Linux's
`binfmt_misc` - says which interpreter opens a file by its extension: `.bat`
and `.cmd` cmd, `.sh` bash, `.vsh` vsh. `execve` of a file with no `#!` and an
extension in the table runs the interpreter with the file as its argument; a
`#!` line wins when there is one.

## 5. The names in /usr/bin (E4-021)

- Linux programs take Linux's names: `sh`, `ls`, `cat`, ... - BusyBox's links,
  coreutils if ever needed.
- The native test programs (`SH.ELF`, `CAT`, `LS`, `ECHO`, `WC`, the `SVC_*`)
  move to `/usr/lib/vibeos/test/`, where the gate runs them from.
- `vsh` is the one native program with a short name.

## The phases

S1 the kernel's job control and pseudo-terminals ([s1.md](s1.md)); S2 login;
S3 the names; S4 vsh; S5 bash and binfmt; S6 Wine's cmd; S7 FinalCommander
(suggested 2026-11-19), Linux's build and Windows', as the full-screen
terminal program that proves the console, the pseudo-terminal and both
personalities at once.
