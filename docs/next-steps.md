# Next Steps

The live roadmap: what comes next, in order. Updated every time something
changes; **when an item is done it is removed from here** and what it did is
recorded where it belongs (its plan, `implementation_progress.md`, the open
items registers). [roadmap.md](roadmap.md) keeps the history of how the order
was decided. What is still open in every register is gathered, generated, in
[open-items.md](open-items.md).

## Now

1. **The filesystem's hierarchy** - [filesystems/hierarchy/](filesystems/hierarchy/README.md),
   phases F0-F2 first: what lives where, the current root's tree laid out on it,
   and a mount manifest. F0 and F1 are done (2026-11-17,
   [inventory.md](filesystems/hierarchy/inventory.md)); F2 is done
   (2026-11-19, [f2.md](filesystems/hierarchy/f2.md)): volumes by UUID, the
   root's `/etc/vibeos/mounts`, mount options, EBUSY, mount(2) and umount2(2).
   What follows is the user's shell (item 2); F3, the production root's
   format, waits for the installer.

## Next

2. **The user's shell** - designed before written: login, prompt, line
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
3. **The GUI** - decided 2026-11-03: an existing Wayland compositor on a
   minimal DRM/KMS (dumb buffers, one output) and evdev, as the README's "a
   desktop from Linux" said; extended later. The desktop and the shell stay
   ours. [ui-design/](ui-design/README.md)'s own `displayd` is set aside, its
   requirements (safe failure, observability) kept as the bar.
4. **Windows' window system** - user32 as a client of that compositor, as
   Wine's Wayland driver is, and gdi32 drawing into shared buffers. Open items
   WO-101 to WO-105.

## Later

5. Security, all of it (isolation, privilege, sandboxing; L9's capabilities,
   seccomp and Landlock), and POSIX ACLs acted on: ext4 stores them since
   2026-11-16 (E4-011), and nothing yet decides a permission by one or gives
   a new file its directory's default list.
6. The applications that manage the system.
7. The rest of networking.
8. System administration (L8).

## Proposed by the community, to be placed

Ideas from GitHub Discussions, all accepted as worth doing; each gets a place in
the order above when its prerequisites are close.

- **Selectable desktop environments** (GNOME, KDE Plasma, Xfce) -
  [#153](https://github.com/Greepolis/VibeOS/discussions/153). Follows the GUI
  (item 3): the same DRM/KMS and evdev, plus epoll, eventfd, timerfd, inotify,
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

- Which Wayland compositor (item 3), chosen when that step is planned.
