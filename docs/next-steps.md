# Next Steps

The live roadmap: what comes next, in order. Updated every time something
changes; **when an item is done it is removed from here** and what it did is
recorded where it belongs (its plan, `implementation_progress.md`, the open
items registers). [roadmap.md](roadmap.md) keeps the history of how the order
was decided. What is still open in every register is gathered, generated, in
[open-items.md](open-items.md).

## Now

Order decided 2026-11-19, with the hierarchy's F0-F2 done ([filesystems/hierarchy/](filesystems/hierarchy/README.md); F2 on 2026-11-19, [f2.md](filesystems/hierarchy/f2.md): volumes by UUID, the root's `/etc/vibeos/mounts`, mount options, EBUSY, mount(2) and umount2(2)): the shell, then F3, then the installer.

1. **The user's shell** - designed before written: login, prompt, line
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
2. **F3, the production root** - the hierarchy's next phase ([implementation-phases.md](filesystems/hierarchy/implementation-phases.md)): the writable native root's format approved, host tools to make, inspect and repair it, persistent `/usr`, `/etc`, `/var` and `/home`, power cuts over its metadata. No installer in it.
3. **The installer** - puts F3's root on a disk: partitions, the ESP, the root by UUID (F2 step 2), the manifest (F2 step 3) - with `/boot/efi` read-only on a production root, which F2 left to it.

## Next

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
   seccomp and Landlock), and POSIX ACLs acted on: ext4 stores them since
   2026-11-16 (E4-011), and nothing yet decides a permission by one or gives
   a new file its directory's default list.
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
