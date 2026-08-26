# Graphics

What exists on screen, how it gets there, and - the part worth stating plainly
- how little of it any gate would catch if it broke.

## The path

The bootloader asks UEFI's Graphics Output Protocol for a linear framebuffer
and passes its address, dimensions, pitch and pixel format to the kernel in the
boot info structure. Nothing later re-negotiates the mode: the resolution the
firmware chose is the resolution VibeOS runs at, for the whole boot.

Everything above that is software drawing into that one buffer. There is no
GPU driver, no acceleration and no second buffer. A pixel is written by
computing its address from the pitch and storing to it.

The desktop is composed in the order a painter would: background, then window
frames, then their contents, then the pointer last so it is never covered by
what was drawn after it. Redraw is by region rather than whole-screen where the
region is known, because at these resolutions a full repaint per mouse move is
visible as a smear.

## Input

Two PS/2 devices, both on the legacy controller:

- **Keyboard**, IRQ1. Scan codes are translated in the handler and pushed into
  a ring buffer that `read()` drains. The ring holds 2048 bytes; it held 512
  until a paste-sized burst of input during a BusyBox pipeline overran it and
  dropped characters, which looked like the shell mis-parsing its input rather
  than like a full buffer.
- **Mouse**, IRQ12. Three-byte packets carrying button state and signed X/Y
  deltas. Y is inverted relative to screen coordinates, and the deltas are
  sign-extended out of a flags byte rather than being signed on the wire, which
  is the detail that turns "the pointer only moves down and right" into a
  working pointer.

## The on-screen terminal

The desktop hosts a terminal window that shares the shell with the serial
console rather than running a second one. It is a view onto the same session:
the same ring buffer, the same process. A character typed on the PS/2 keyboard
and a character arriving on the serial line are the same event by the time
anything above the driver sees them.

## What is verified, and what is not

This is the honest part. The GUI is **not** gated.

The boot smoke test asserts state rather than markers - a DHCP lease, four
cores online, a TCP round trip, a Linux binary that ran - and it can assert
that the graphics stack initialised and that the desktop reports itself ready,
because those are serial-visible. It cannot assert that anything appeared. A
framebuffer full of black pixels and a correctly drawn desktop produce the same
serial output.

So the pixels are checked by hand, with `scripts/dev/screenshot.py`, which
boots under QEMU and captures the framebuffer to a PPM file. Nothing runs it
automatically. A change that draws the desktop entirely off-screen, or in the
wrong pixel format, or not at all, would pass CI.

That is a deliberate gap and not an oversight - a pixel-comparison gate is
brittle in a way that costs more than it catches, this early - but it means
"the GUI works" is a claim resting on somebody having looked recently, and it
should be read that way.

## Limits worth knowing before extending this

- **One mode, chosen by firmware.** No mode setting, so no resolution change at
  runtime and no response to a monitor being swapped.
- **One buffer.** Drawing is visible as it happens. Anything large enough to be
  slow will be seen being drawn.
- **No clipping discipline in depth.** Windows are drawn in order; there is no
  general clip-rectangle stack, so a new kind of overlapping element needs its
  own thinking rather than inheriting one.
- **Pixel format is read from boot info, not assumed.** Code that hardcodes
  BGRA will work on QEMU and fail on hardware that reports RGBA.
