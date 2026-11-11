# Next Steps

The live roadmap: what comes next, in order. Updated every time something
changes; **when an item is done it is removed from here** and what it did is
recorded where it belongs (its plan, `implementation_progress.md`, the open
items registers). [roadmap.md](roadmap.md) keeps the history of how the order
was decided.

## Now

1. **The filesystem's hierarchy** - [filesystem-design/](filesystem-design/README.md),
   phases F0-F2 first: what lives where, the current root's tree laid out on it,
   and a mount manifest. F0-F2 need no choice of on-disk format.
2. **ext4 as the native root** (F3, decided 2026-11-03) - **done 2026-11-09**:
   read, write, the journal, the root at `/` with the programs in `/usr/bin`,
   and the intensive test against Linux in the nightly (steps 1 to 5,
   [filesystem-design/ext4.md](filesystem-design/ext4.md)). What is left is the
   open items in [fs_ext4.md](implementation_progress/fs_ext4.md).

## Next

3. **The user's shell** - designed before written: login, prompt, line
   editing and completion, job control, which commands are built in, how it
   finds programs in the new hierarchy. It is also the terminal inside the GUI.
   Decided 2026-11-11, before the design:
   - **Three shells.** A native VibeOS shell with a syntax of its own - the
     login shell and the terminal's - designed by taking what is good in
     bash's and in cmd's, not by choosing one; and beside it bash and cmd for
     the scripts written for them.
   - **bash is the real one**, built from source against musl as SQLite and
     Lua are, at `/usr/bin/bash`. Until then `/bin/sh` is BusyBox's ash; it
     stays a POSIX shell either way, because Linux programs' `system()` and
     `#!/bin/sh` need one.
   - **cmd** is `C:\Windows\System32\cmd.exe`, run by the NT personality:
     an interpreter of our own or Wine's, to be decided with the design.
   - **A script runs by its type**, as on Windows: opening a `.bat` or `.cmd`
     runs cmd, a `.sh` runs bash, from the shell and from the desktop alike.
   - The names in `/usr/bin` (E4-021) are decided with this design: the
     native programs `SH.ELF`, `CAT`, `LS`, `ECHO` and `WC` must not take the
     names Linux programs expect.
4. **The GUI** - decided 2026-11-03: an existing Wayland compositor on a
   minimal DRM/KMS (dumb buffers, one output) and evdev, as the README's "a
   desktop from Linux" said; extended later. The desktop and the shell stay
   ours. [ui-design/](ui-design/README.md)'s own `displayd` is set aside, its
   requirements (safe failure, observability) kept as the bar.
5. **Windows' window system** - user32 as a client of that compositor, as
   Wine's Wayland driver is, and gdi32 drawing into shared buffers. Open items
   WO-101 to WO-105.

## Later

6. Security, all of it (isolation, privilege, sandboxing; L9's capabilities,
   seccomp and Landlock).
7. The applications that manage the system.
8. The rest of networking.
9. System administration (L8).

## Proposed by the community, to be placed

Ideas from GitHub Discussions, all accepted as worth doing; each gets a place in
the order above when its prerequisites are close.

- **Selectable desktop environments** (GNOME, KDE Plasma, Xfce) -
  [#153](https://github.com/Greepolis/VibeOS/discussions/153). Follows the GUI
  (item 4): the same DRM/KMS and evdev, plus epoll, eventfd, timerfd, inotify,
  `SCM_RIGHTS` and memfd at the Linux ABI.
- **ARM64 (AArch64)** - [#154](https://github.com/Greepolis/VibeOS/discussions/154).
  A second architecture, and the test of `kernel/arch`'s boundary.
- **A native JDK** - [#155](https://github.com/Greepolis/VibeOS/discussions/155).
  OpenJDK through the Linux ABI first: a demanding compatibility workload.
- **A native Python** - [#156](https://github.com/Greepolis/VibeOS/discussions/156).
  CPython built against musl, as SQLite and Lua are in the corpus today.
- **An HTML/CSS/JS application runtime** -
  [#157](https://github.com/Greepolis/VibeOS/discussions/157). Needs the GUI;
  a distinctive application model of VibeOS's own.

## Decisions waiting

- Which Wayland compositor (item 4), chosen when that step is planned.
