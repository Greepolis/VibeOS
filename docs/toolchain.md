# Toolchain and Build Strategy

## Goals

- deterministic and reproducible builds
- a clear split between host tools and target artifacts
- support for kernel-grade debugging and tracing
- a sustainable language strategy for C and C++

## Language

The project is C and assembly. There is no C++ anywhere in the tree, and the
CMake project declares only `C ASM`.

That started as a plan for C++ in the higher-level subsystems and did not
survive contact with the work. The kernel runs freestanding, with no runtime,
no allocator during early boot and no unwinder; every C++ feature worth having
there would have had to be disabled, and what remained would have been C with
different syntax. Assembly is used only where nothing else can express the
job: the AP trampoline, the syscall and interrupt entry stubs, and the program
entry point in `user/prog/crt0.S`.

Rules:

- minimize dynamic allocation during early boot
- keep the freestanding kernel environment separate from hosted user-space
  components; the hosted TLS adapter lives in its own target precisely so it
  cannot be dragged into the kernel image by a future reference
- put portable logic in portable files - the ELF parser, the TCP/IP stack and
  the syscall translation model are all host-tested because they were written
  without touching hardware

## Compilers

GCC and Clang are both first-class and both gated in CI, in Debug and Release.
Neither is the primary one.

This is not even-handedness for its own sake. The two disagree about enough to
have caught real bugs that the other missed: an entry stack misaligned by
eight bytes faults on the aligned SSE store Clang emits and not on the code
GCC generates; a page of SSE spills at `-O2` triple-faulted a core that GCC at
`-O0` started cleanly. Two compilers are a cheap second opinion about
undefined behaviour.

Linking follows the same split: the GCC path builds an ELF and converts it to
PE, the Clang path links PE natively with LLD. Both produce a bootable image
and both are boot-gated.

## Build system

CMake 3.21+ with Ninja. Targets are separated by what they are allowed to
assume about their environment:

| Target | Kind |
| --- | --- |
| `vibeos_kernel_core` | freestanding kernel logic |
| `vibeos_kernel` | the linked kernel image |
| `vibeos_bootloader_uefi` | UEFI bootloader |
| `vibeos_user_core` | portable code shared with user space |
| `vibeos_tls` | hosted Mbed TLS adapter, deliberately outside `vibeos_user_core` |
| `vibeos_user_hello` / `_sh` / `_net` / `_task` | ring-3 programs, linked with `user/prog/user.ld` |
| `vibeos_kernel_tests`, `vibeos_bootloader_tests` | host suites |
| `vibeos_image`, `vibeos_vm_images` | bootable media |
| `fuzz_inet_input` | libFuzzer harness over the network receive path |

The source list lives in `cmake/core_sources.cmake` and is read by both CMake
and `scripts/run-tests.ps1`. It is a single file because it used to be two,
and the copies drifted three times - each time breaking CI on Windows only,
after the change had already been declared verified on Linux.

The build system should produce:

- kernel ELF or PE image as required by the boot path
- bootable disk image
- symbol files for debugging
- emulator launch targets

## Cross-compilation model

- host builds run on Linux, macOS, or Windows development machines
- target triples are explicit and never inferred from the host
- freestanding kernel and hosted user-space builds use separate compiler flags and runtime assumptions

## Debug and observability tooling

The concrete list, with the reason for each, is in
[development_tools.md](development_tools.md).


- QEMU as the first emulator target
- GDB and LLDB support from the start
- serial logging as the first diagnostic channel
- symbolized crash output for kernel panics in debug builds

## Build variants

- `debug` for bring-up and diagnostics
- `release` for performance profiling
- `hardened` for security validation once the base system matures

## Early deliverables for phase 2

### M1: Toolchain and Build Skeleton (completed)

The following are available:

**Build System**
- CMake 3.21+ with Ninja support (primary generator)
- GCC fallback for environments without CMake/Ninja
- Reproducible host-to-target build for kernel core and user-space components
- Modular target separation: `vibeos_kernel_core`, `vibeos_user_core`, `vibeos_image`

**Commands**
```bash
# One-command configuration and build
cmake -S . -B build -G Ninja -DVIBEOS_BUILD_TESTS=ON -DVIBEOS_BUILD_KERNEL_IMAGE=ON

# Build all targets (kernel, user-space, boot image, tests)
cmake --build build

# Run host-side tests
powershell ./scripts/run-tests.ps1 -BuildDir build

# Run tests with QEMU smoke test (if available)
powershell ./scripts/run-tests.ps1 -BuildDir build -RunQemu

# Standalone QEMU boot test
powershell ./scripts/run-qemu.ps1 -BuildDir build -ImagePath build/artifacts/vibeos_boot.img
```

**Artifacts Generated**
- `build/artifacts/vibeos_kernel.elf` kernel ELF image (loadable by bootloader)
- `build/artifacts/efi_root/` bootable EFI media, including the ring-3 programs staged as `EFI/BOOT/*.ELF`
- `build/artifacts/vibeos_esp.img`, `vibeos.iso`, `vibeos.vdi`, `vibeos.vmdk` importable images (see the README)
- `build/artifacts/boot_manifest.txt` manifest of boot artifacts
- `build/vibeos_kernel_tests.exe` host-side unit test executable
- `artifacts/test-summary.json` structured test results for CI/agents
- `artifacts/qemu-serial.log` serial output from QEMU (if run)

**Verified Configuration**
- CMake generator availability detection
- Automatic fallback to manual GCC compilation if CMake/Ninja unavailable
- Toolchain version capture (cmake, gcc, qemu, ninja)
- Build artifact validation before test execution

### M2/M3: Boot and Early Kernel (completed)

- one-command QEMU boot path (`scripts/qemu-cli-smoke-linux.py`), which boots
  four vCPUs under pure TCG and asserts state rather than grepping for a token
- image packaging pipeline producing UEFI media and importable disk images

### Dependencies

`third_party/mbedtls` is the only external dependency, pinned as a submodule
and fetched recursively - since 4.x it carries nested submodules of its own.
It is optional: without it the build simply has no TLS, unless
`VIBEOS_REQUIRE_TLS` is set, which the Linux CI matrix does so that a missing
submodule fails loudly there instead of silently degrading.

### Debugging a hang

Serial output is the first channel, but a hung guest has stopped producing it.
The QEMU monitor is the second: `info registers -a` gives every core's RIP,
and resolving those against `nm` on the kernel ELF turns "it froze" into a
function name. Two intermittent hangs were diagnosed this way that no amount
of added logging would have found, because the hang was in code holding the
lock the logger needed.
