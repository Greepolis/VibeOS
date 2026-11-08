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
2. **ext4 as the native root** (F3, decided 2026-11-03; steps 1 to 3 - read, write,
   the journal - done) - [filesystem-design/ext4.md](filesystem-design/ext4.md): the
   root on it (step 4: the partition at `/` and the ESP at `/boot/efi`, gated, done; the programs into `/usr/bin`, next), then the intensive test (step 5). `mkfs.ext4` and `e2fsck` are the oracle.

## Next

3. **The user's shell** - designed before written: login, prompt, line
   editing and completion, job control, which commands are built in, how it
   finds programs in the new hierarchy. It is also the terminal inside the GUI.
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
