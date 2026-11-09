# Accessibility

VibeOS is a research kernel maintained by one person. This file states what can
be used without barriers today, what cannot, and how to tell us about a barrier.
It describes the current state of `main` honestly, not a target.

## Commitment

A barrier in something people are meant to use - the documentation, the
repository, the text console - is treated as a defect, like any other. A
barrier in the graphical desktop is recorded and planned, but the desktop is an
experiment and is not yet held to the same standard (see below).

## Supported environments

| Surface | How it is used | State |
| --- | --- | --- |
| Documentation (the mdBook site built from `docs/`) | Any browser | Plain HTML with headings, tables and text. Works with screen readers, browser zoom and the reader's own fonts and colours. mdBook's light and dark themes are both available. |
| Repository, issues and pull requests | GitHub | GitHub's own accessibility applies. Diagrams in the docs are described in the surrounding text, not only drawn. |
| Serial console and kernel CLI | A terminal on the host (QEMU `-serial stdio`, or any serial client) | Plain text, line by line, with no colour codes required to understand the output. Readable through the host terminal's screen reader, magnifier and font settings. **This is the recommended way to use VibeOS.** |
| Boot gate and dev scripts (`scripts/dev/`) | A terminal | Text output. Results are a `reason=` line and named counters, not colour alone. |
| Framebuffer desktop | QEMU or a VM window | Experimental; see limitations. |

## Known limitations

- **The framebuffer desktop has no assistive technology.** There is no screen
  reader, no magnifier, no high-contrast mode and no font scaling. Its text is
  pixels in a window and cannot be read by the host's screen reader.
- **Desktop input is a PS/2 keyboard and mouse.** Full keyboard operation of
  the desktop has not been assessed, and there are no sticky or slow keys.
- **Keyboard layouts** other than the default one the driver decodes have not
  been tested.
- **The desktop is not checked by any gate.** Its pixels are checked by hand
  (`scripts/dev/screenshot.py`), so an accessibility regression there would not
  turn CI red.
- **No accessibility audit has been done** of the documentation site against
  WCAG. The statements above come from how it is built, not from a
  measurement.

Anything a person needs from VibeOS today can be done from the serial console
and the documentation, both of which are text.

## Reporting a barrier

Open an issue at <https://github.com/Greepolis/VibeOS/issues> with
"Accessibility" in the title. Say:

- what you were trying to do and where (a doc page, the console, the desktop);
- what assistive technology or setting you use, if any;
- what happened instead.

If you would rather not report in public, use the private channel described in
[SECURITY.md](SECURITY.md) and say that the report is about accessibility.

A report is acknowledged within 7 days, as security reports are. A barrier in
the documentation or the console is fixed like any defect. One in the desktop
is recorded in `docs/implementation_progress.md`, so that it is visible in the
same place as the rest of the project's open work.
