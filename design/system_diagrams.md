# System Diagrams

## Layered architecture

```text
+-----------------------------------------------------------+
| Applications                                              |
| Native apps | Linux apps | Windows apps | macOS runtimes  |
+------------------------+----------------------------------+
| Compatibility Services | Native Userland Services         |
| Linux runtime          | init/service manager             |
| Windows subsystem      | device manager                   |
| macOS runtime host     | fs manager | net manager         |
+------------------------+----------------------------------+
| User-space Driver Hosts | Filesystem Services | Security  |
+-----------------------------------------------------------+
| Kernel Core                                               |
| scheduler | vm | ipc | interrupts | objects | timers      |
+-----------------------------------------------------------+
| Arch / Platform                                           |
| x86_64 | future ARM64 | firmware | interrupt controller   |
+-----------------------------------------------------------+
| Hardware                                                  |
+-----------------------------------------------------------+
```

## Boot flow

```text
UEFI
  -> Bootloader
  -> Kernel early init
  -> Memory manager bootstrap
  -> Interrupts and timers
  -> SMP startup
  -> Init service
  -> Core services
  -> Compatibility services
  -> User sessions
```

## Compatibility model

```text
Foreign App
  -> Loader
  -> Compatibility runtime
  -> Native VibeOS service calls
  -> Kernel syscalls
```

When translation is insufficient:

```text
Foreign App
  -> Lightweight VM or guest container
  -> Shared integration services
  -> Native host resources under policy
```
