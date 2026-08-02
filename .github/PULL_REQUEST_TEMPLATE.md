## What this changes

<!-- One or two sentences. What is different afterwards. -->

## Why

<!-- The problem, not the patch. If it fixes a bug, say what the bug actually
     was - the visible symptom and the underlying cause are often nowhere near
     each other in this codebase. -->

## How it was verified

The important section. Green is a claim, and CI passing is not by itself
evidence that a new test works.

- [ ] Builds with **both** gcc and clang, with no new warnings
- [ ] Host suites pass (`ctest`)
- [ ] Boot gate passes (`scripts/dev/check.sh smoke <build-dir>`)
- [ ] **The new test was made to fail.** Break the thing it covers, watch it go
      red, put it back. A test that has never failed has not been shown to work.
- [ ] If the bug was intermittent, it was run repeatedly
      (`scripts/dev/repeat-boot.sh`) - not once

Paste the evidence rather than asserting it:

```
```

## Anything that stayed broken

<!-- Scope you did not cover, assumptions you made, things you are unsure
     about. This is more useful than a clean-looking description: an unstated
     gap becomes someone else's confusing afternoon. -->
