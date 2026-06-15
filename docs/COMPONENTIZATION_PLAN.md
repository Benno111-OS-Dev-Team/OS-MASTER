# Componentization Plan

This document lays out a practical path for splitting the current OS8 codebase into several independently maintained repositories.

## Goal

Turn the current monorepo-style layout into a set of smaller repos with clear ownership boundaries, stable interfaces, and reproducible integration builds.

## Recommended Repo Split

Start with a small number of well-defined repos instead of splitting everything at once.

### 1. `shared-api`

Shared contracts used by multiple components.

- Syscall and ABI headers
- Kernel/userspace interface types
- Common protocol definitions
- Application SDK headers and build-exported public interfaces
- Any header that must remain versioned independently

### 2. `kernel-core`

The kernel implementation itself.

- `kernel/core`
- `kernel/mm`
- `kernel/fs`
- `kernel/net`
- `kernel/sched`
- `kernel/syscall`
- `kernel/ipc`
- `kernel/sync`
- `kernel/sandbox`
- `kernel/lib`
- architecture-specific kernel code under `kernel/arch/*`

### 3. `drivers`

Hardware and virtual device support.

- `drivers/video`
- `drivers/gpu`
- `drivers/input`
- `drivers/network`
- `drivers/nvme`
- `drivers/usb`
- `drivers/platform`
- `drivers/bluetooth`
- `drivers/uart`

### 4. `libc`

Userspace C library and startup objects.

- `libc/src`
- `libc/include`
- `libc/crt`
- build and install scripts for the runtime library

### 5. `userland`

Userspace programs and support code.

- `userspace/init`
- `userspace/login`
- `userspace/shell`
- `userspace/lib`
- `userspace/examples`
- application packaging and seed content

### 6. `boot-platform`

Bootloader integration and image assembly.

- `boot/`
- `bootmanager/`
- `os-x86_64/`
- boot configs and platform-specific launch assets

### 7. `runtimes`

Language/runtime build helpers.

- `runtimes/python`
- `runtimes/nodejs`
- packaging scripts for runtime images

### 8. `assets`

Visual and branding assets.

- `assets/`
- `assets/boot-assets/`
- `assets/screenshots/`
- generated logos and wallpapers

### 9. `build-tooling`

Repo orchestration and developer tooling.

- root `Makefile`
- `Makefile.multiarch`
- `scripts/`
- CI helpers
- release/push helpers

## Split Order

Split the codebase in this order to minimize churn.

1. Define and freeze shared interfaces.
2. Extract `shared-api`.
3. Extract `libc`.
4. Extract `userland`.
5. Extract `boot-platform`.
6. Extract `drivers`.
7. Extract `runtimes`.
8. Extract `assets`.
9. Split `kernel-core` once dependencies are stable.

## Interface Rules

The split only works if repo boundaries are enforced with explicit contracts.

- Kernel ABI changes must be versioned.
- Userspace should depend on installed headers, not sibling source trees.
- Drivers should only use exported kernel interfaces.
- Boot code should consume published kernel artifacts.
- Generated assets should be produced during builds, not edited by hand.

## Build Strategy

The current build should evolve from "compile everything in one tree" to "assemble pinned artifacts."

### Before splitting

- Move shared public headers into a dedicated package or export directory.
- Remove direct source inclusion across component boundaries.
- Make generated files and assets explicit build outputs.
- Ensure each major component can build from installed dependencies.

### After splitting

- Each repo produces a versioned artifact.
- An integration repo pins exact component versions.
- Image assembly happens in the integration layer, not inside component repos.
- CI verifies each repo independently and then performs end-to-end boot tests.

## Suggested First Milestone

The safest first milestone is a five-repo layout:

- `kernel-core`
- `drivers`
- `libc`
- `userland`
- `boot-platform`

Keep `assets`, `runtimes`, and `tooling` either inside the integration layer or as follow-up repos once the first split is stable.

## Practical Migration Steps

1. Inventory all cross-directory dependencies.
2. Identify every header or file that is shared across layers.
3. Decide which shared interfaces belong in `shared-api`.
4. Make the build consume installed artifacts instead of sibling source.
5. Extract the least coupled component first.
6. Add a manifest that pins component versions for integration builds.
7. Add CI for each repo plus a full image assembly pipeline.

## Notes

This plan assumes the goal is modularity and independent maintenance, not a large-scale rewrite. If a boundary turns out to be unstable, keep that part inside the integration repo until the interface settles.
