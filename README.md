# OS-MASTER

OS-MASTER is now a FreeBSD-based distribution wrapper.

This repository no longer contains a custom kernel, custom bootloader stack,
custom libc, or custom userspace implementation. Instead, it stages official
FreeBSD release media and republishes it under this project with local assets
and release automation around it.

## Current Base

- Default pinned base: `FreeBSD 14.4-RELEASE`
- Default architecture: `amd64`
- Default image type: `disc1.iso`

The default image source is the official FreeBSD release mirror:

- `https://download.freebsd.org/releases/ISO-IMAGES/14.4/`

## Repository Layout

```text
.github/            CI and release automation
assets/             Branding and reusable artwork
scripts/            FreeBSD fetch, verify, and launch helpers
Makefile            Main entry point
Makefile.multiarch  Compatibility shim to the main Makefile
```

## What Changed

The old in-tree OS implementation has been removed from this repository:

- no custom kernel sources
- no custom boot manager
- no custom driver tree
- no custom libc or userspace tree
- no custom OS image construction pipeline

If you need to inspect or recover the previous custom OS implementation, use
git history.

## Build

The primary workflow is now to fetch and verify an official FreeBSD release
image.

```sh
make image
```

That downloads the official compressed image, verifies it against the official
FreeBSD SHA256 checksum file, expands it locally, and stages the result under
`image/`.

## Configuration

You can override the default release parameters:

```sh
make image \
  FREEBSD_RELEASE=15.0-RELEASE \
  FREEBSD_ARCH=amd64 \
  FREEBSD_IMAGE_BASENAME=disc1.iso
```

Supported values depend on what the official FreeBSD mirrors publish.

## QEMU

To boot the staged amd64 installer image in QEMU:

```sh
make qemu
```

## Cleanup

```sh
make clean
```

This removes locally staged downloads and generated image output under
`build/` and `image/`.
