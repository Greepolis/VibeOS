# VibeOS

VibeOS is a from-scratch operating system project focused on:
- modular architecture
- security-first design
- high-performance kernel/runtime paths
- multi-platform compatibility strategy (Linux/Windows/macOS software layers)

The system boots on UEFI firmware, brings up every core, and runs real ELF
programs in ring 3 through the Linux system-call ABI.

## What runs today

- **Boot**: a UEFI bootloader hands off to the kernel, which sets up its own
  GDT/IDT, page tables and TSS.
- **SMP**: ACPI/MADT is parsed, the local and I/O APICs are programmed, and the
  application processors are started with INIT-SIPI-SIPI. Every core runs the
  scheduler against a shared run queue.
- **Preemptive multitasking**: per-process address spaces, a private kernel
  stack per task so a task can block inside a syscall, and `fork`, `execve`,
  `wait4` and `exit`.
- **Linux ABI in ring 3**: programs are loaded by a host-tested ELF64 parser and
  entered on a real System V startup stack - `argc`, `argv`, `envp` and an
  auxiliary vector carrying `AT_PHDR`/`AT_PHNUM`/`AT_PHENT`. Thread-local
  storage works: `arch_prctl(ARCH_SET_FS)` writes the MSR and the base is
  restored on every context switch.
- **Unmodified static Linux programs**: a static musl binary and a static
  BusyBox, neither built by this project, run from the boot volume. BusyBox
  dispatches on its own name, reads and lists files, and its shell parses
  scripts, searches `PATH`, forks and execs. Dynamic binaries are refused, so
  this means static ones.
- **Copy-on-write fork**: `fork` shares the parent's pages read-only instead of
  copying them, and a write faults and duplicates just that page. Measured at
  boot: 1221 pages shared, 24 later copied.
- **Signals**: raised, masked, queued, and delivered on the way back to user
  space with the frame built on the process's own stack below the red zone;
  `rt_sigreturn` restores it. Verified by a program built against a real C
  library, so the handler returns through that library's own trampoline.
- **Pipes**: `pipe2`, `dup`, `dup2`, descriptors inherited across `fork` and
  released on exit, and `SIGPIPE` for a write with no reader. The boot
  self-test runs `ls /EFI/BOOT | wc -l` in BusyBox's shell - 13 on a full
  build - and the gate fails if the pipeline does not complete.
- **Storage**: virtio-blk with a FAT reader and writer; programs are loaded
  from the boot volume.
- **Networking**: a portable TCP/IP stack over virtio-net - ARP, IPv4, ICMP,
  UDP, TCP, DHCP and DNS - verified end to end against a host echo server in
  CI on every push.
- **A serial shell** with line editing and file commands, and `Ctrl-C` that
  becomes a real `SIGINT`.
- **A screen**: a PS/2 mouse on IRQ12 and a framebuffer desktop with a pointer
  and a window that mirrors the console. The boot gate checks that characters
  reached the on-screen terminal; the pixels themselves are checked by hand
  with `scripts/dev/screenshot.py`, so the GUI is not gated.

Everything above is asserted by the boot gate unless it says otherwise: the
GUI's pixels are not, and the copy-on-write page counts are reported rather
than asserted.

## Current Focus

- a ring-3 program that draws its own window and receives its own input; the
  GUI is currently kernel code
- keyboard into that window, so the machine is usable without a serial cable
- dynamic executables, then threads with real futexes

## Build and Test

### Windows (PowerShell)

```powershell
cmake -S . -B build -DVIBEOS_BUILD_TESTS=ON -DVIBEOS_BUILD_KERNEL_IMAGE=ON
cmake --build build
powershell -ExecutionPolicy Bypass -File scripts\run-tests.ps1 -BuildDir build -Generator Ninja -Profile agent
# Optional OVMF smoke (if QEMU+OVMF firmware are available on host):
powershell -ExecutionPolicy Bypass -File scripts\run-qemu-ovmf.ps1 -BuildDir build -ExpectToken BOOT_OK -Timeout 60 -Strict
```

### Linux/WSL

```bash
cmake -S . -B build -G Ninja -DVIBEOS_BUILD_TESTS=ON -DVIBEOS_BUILD_KERNEL_IMAGE=ON
cmake --build build
ctest --test-dir build --output-on-failure
# Required UEFI smoke gate (boots EFI/BOOT/BOOTX64.EFI from artifacts/efi_root):
./scripts/qemu-smoke-ovmf-linux.sh build "BOOT_OK" 60
# Interactive boot-to-CLI gate:
./scripts/qemu-smoke-cli-linux.sh build 90
# Informational direct-loader probe:
./scripts/qemu-smoke-linux.sh build
```

Notes:
- CI uses required boot gates: host tests + OVMF smoke + boot-to-CLI smoke, while the direct-loader probe is informational.
- The boot-to-CLI gate (`scripts/qemu-cli-smoke-linux.py`) is not a token grep: it boots four vCPUs under pure TCG, asserts state invariants (a non-zero DHCP lease, all cores online, no unexpected fault, no panic) and completes a TCP round trip to a host echo server. It also asserts that the programs did their work: the unmodified Linux binary ran with the right `argv`, BusyBox dispatched an applet and touched files, its interactive shell ran a builtin and exec'd through `PATH`, a pipeline completed, signals were delivered, and the on-screen terminal was not empty. It distinguishes "produced nothing", "went quiet" and "still talking" so a hang is diagnosable from the log alone.
- Beyond the boot gates, CI runs a gcc/clang x Debug/Release matrix, the host suites under AddressSanitizer and UndefinedBehaviorSanitizer, CodeQL static analysis, a libFuzzer harness over the network receive path, and a nightly job that boots repeatedly and fuzzes for longer. Dependabot tracks the actions and the submodules.
- `qemu-system-x86_64 -kernel` has known compatibility limits with ELF64/Multiboot2 images in newer QEMU builds. The direct-loader probe classifies these loader-side incompatibilities separately from real boot regressions.
- Bootloader artifacts use a validated EFI conversion flow (`bootloader.elf` -> `bootloader.efi`) with fail-fast PE32+ and relocation checks.
- OVMF smoke emits structured diagnostics (`qemu-ovmf-summary.txt`) with explicit status reasons, selected boot profile, and last observed boot phase.
- EFI media generation includes `startup.nsh` fallback in `artifacts/efi_root` to make firmware-shell boot path deterministic.
- Boot-to-CLI smoke waits for `BOOT_OK`, verifies `CLI_READY`, sends `help`, `status`, `echo vibeos`, and `halt`, and stores `qemu-cli-summary.txt`.

## Bootable VM Images (VirtualBox / VMware)

After a Linux/WSL image build, generate importable disk/CD images:

```bash
./scripts/make-vm-images-linux.sh build
# or: cmake --build build --target vibeos_vm_images
```

This produces, in `build/artifacts/`:

| File | Use |
| --- | --- |
| `vibeos_esp.img` | Raw FAT16 UEFI EFI System Partition (boots directly in QEMU+OVMF) |
| `vibeos.vdi` | VirtualBox disk |
| `vibeos.vmdk` | VMware disk |
| `vibeos.iso` | UEFI El Torito CD image (needs `xorriso`; the most portable option) |

The ESP image is built by a self-contained FAT16 writer (`scripts/make_esp_image.py`, no `mtools` required); `.vdi`/`.vmdk` are produced with `qemu-img`. The images are boot-tested under QEMU+OVMF by `scripts/qemu-boot-image-linux.py`, and CI boot-tests the generated `.iso`.

**Importing (UEFI is required — VibeOS has no legacy BIOS boot):**
- **VirtualBox**: New VM → Type *Other/Unknown (64-bit)* → in *Settings → System → Motherboard* tick **Enable EFI**. Then either attach `vibeos.vdi` as the hard disk, or attach `vibeos.iso` to the optical drive.
- **VMware**: create a VM with firmware type **UEFI**, then attach `vibeos.vmdk` as the disk (or `vibeos.iso` as a CD/DVD).
- Output is on the **first serial port** (COM1). Enable a serial port in the VM to see the `[BOOT]`/`[CLI]` console; the VibeOS CLI is serial-only for now.

## Project Structure

- `boot/`: bootloader and firmware handoff code (UEFI path + helpers)
- `kernel/`: kernel subsystems (`core`, `sched`, `mm`, `ipc`, `proc`, `arch`)
- `user/`: user-space services/runtime scaffolding (`init`, `servicemgr`, `fs`, `net`, `drivers`, `compat`)
- `include/`: public/internal subsystem headers
- `tests/`: host-side integration and subsystem regression suite
- `docs/`: architecture specs and progress tracking

## Development Tools

What to install and what each thing is for - including the tools used to answer
questions rather than to build: [docs/development_tools.md](docs/development_tools.md).

## License

MIT - see [LICENSE](LICENSE). The vendored dependency under `third_party/` keeps
its own license.

## Progress Tracking

Implementation progress is tracked by macro area:

- [docs/implementation_progress.md](docs/implementation_progress.md)
- [docs/implementation_progress/bootloader.md](docs/implementation_progress/bootloader.md)
- [docs/implementation_progress/kernel_core.md](docs/implementation_progress/kernel_core.md)
- [docs/implementation_progress/process_scheduler.md](docs/implementation_progress/process_scheduler.md)
- [docs/implementation_progress/memory_manager.md](docs/implementation_progress/memory_manager.md)
- [docs/implementation_progress/virtual_memory.md](docs/implementation_progress/virtual_memory.md)
- [docs/implementation_progress/interrupt_handling.md](docs/implementation_progress/interrupt_handling.md)
- [docs/implementation_progress/system_call_interface.md](docs/implementation_progress/system_call_interface.md)
- [docs/implementation_progress/ipc_subsystem.md](docs/implementation_progress/ipc_subsystem.md)
- [docs/implementation_progress/driver_framework.md](docs/implementation_progress/driver_framework.md)
- [docs/implementation_progress/filesystem_layer.md](docs/implementation_progress/filesystem_layer.md)
- [docs/implementation_progress/networking_stack.md](docs/implementation_progress/networking_stack.md)
- [docs/implementation_progress/user_space_interface.md](docs/implementation_progress/user_space_interface.md)
- [docs/implementation_progress/init_system.md](docs/implementation_progress/init_system.md)

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for contribution workflow and coding standards.
