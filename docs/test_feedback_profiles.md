# Test Feedback Profiles

## Purpose

Different development activities need different feedback density. This document defines recommended profiles for automated test output.

## Profiles

### `fast`

Use for local edit-compile-run loops.

Characteristics:

- minimal suite
- concise console output
- only top failure artifact references

### `debug`

Use when a regression is under active investigation.

Characteristics:

- includes trace excerpts
- captures additional subsystem counters
- keeps larger serial and emulator logs

### `agent`

Use when the output is intended to guide an implementation agent.

Characteristics:

- always emits `test-summary.json`
- includes `likely_causes`
- includes `suggested_entrypoints`
- preserves stable test IDs and subsystem mapping

### `ci`

Use for repository-level gates.

Characteristics:

- reproducible suite selection
- artifact retention policy
- explicit pass, fail, timeout, and infra-error distinction

## Recommended default mapping

- local human iteration: `fast`
- difficult bug investigation: `debug`
- autonomous or assisted remediation loop: `agent`
- pull request or branch gate: `ci`
