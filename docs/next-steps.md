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
2. **The native root format** (F3) - a writable POSIX filesystem for `/`,
   `/var` and `/home`. Recommended: ext2, then ext4's journal, because mkfs and
   fsck exist on every host and are the oracle; to be approved before F3
   starts.

## Next

3. **The user's shell** - designed before written: login, prompt, line
   editing and completion, job control, which commands are built in, how it
   finds programs in the new hierarchy. It is also the terminal inside the GUI.
4. **The GUI** - decision needed first: our own compositor
   ([ui-design/](ui-design/README.md), `displayd`) or an existing Linux
   compositor on a minimal DRM/KMS and evdev (README's "a desktop from
   Linux"). The desktop and the shell stay ours either way.
5. **Windows' window system** - user32 as a client of that compositor, as
   Wine's Wayland driver is, and gdi32 drawing into shared buffers. Open items
   WO-101 to WO-105.

## Later

6. Security, all of it (isolation, privilege, sandboxing; L9's capabilities,
   seccomp and Landlock).
7. The applications that manage the system.
8. The rest of networking.
9. System administration (L8).

## Decisions waiting

- The native root format (item 2).
- Our compositor or an existing one (item 4).
