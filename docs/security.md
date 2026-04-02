# Security

## Security model

VibeOS uses a layered security model combining isolation, rights-based access, system policy, and hardening.

## Core principles

- least privilege by default
- small trusted computing base
- explicit privilege transitions
- defense in depth
- auditable security decisions

## Trust domains

- kernel core
- privileged system services
- standard system services
- compatibility runtimes
- sandboxed applications
- guest or VM environments

## Access control

### Baseline mechanism

- handles with rights masks
- capability-aware object access
- service-mediated access to sensitive resources

### Policy layer

The system should support a mandatory policy engine for:

- filesystem access
- device access
- network permissions
- process interaction rules
- compatibility subsystem restrictions

This policy layer can evolve toward a MAC framework.

## Sandboxing

Application sandboxes may restrict:

- filesystem view
- network access
- device classes
- IPC peers
- memory execution permissions

Compatibility runtimes should run inside restricted containers unless explicitly trusted.

## Hardening

- kernel ASLR where supported
- W^X and executable mapping controls
- stack canaries and compiler hardening
- structured syscall validation
- guarded kernel memory regions
- control-flow protection where toolchains allow

## Identity and authentication

Early phases should support local user and service identities with secure credential storage. Network and enterprise identity integration is explicitly deferred.

## Secure boot and integrity

The design should permit:

- measured boot
- signed kernel and core service images
- verified system partitions
- runtime attestation experiments in later phases

## Observability and audit

- auditable permission denials
- security event logging
- crash and tamper diagnostics
- trace correlation for compatibility subsystem behavior
