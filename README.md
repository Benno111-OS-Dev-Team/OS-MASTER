# OS-MASTER

OS-MASTER is now a FreeBSD-based distribution wrapper that stages a customized
FreeBSD installer image.

This repository no longer contains a custom kernel, custom bootloader stack,
custom libc, or custom userspace implementation. Instead, it starts from
official FreeBSD release media and overlays a small amount of installer-side
customization around it.

## Current Base

- Default pinned base: `FreeBSD 14.4-RELEASE`
- Default architecture: `amd64`
- Default image type: `dvd1.iso`

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

## X11 Behavior

The staged installer image is customized to preinstall `xorg` in two places:

- in the live installer environment
- in the installed FreeBSD system during `bsdinstall`

This uses official `bsdinstall` local hook files overlaid onto the media. The
default source media is `dvd1.iso` because the FreeBSD Handbook describes it as
including the files needed to install FreeBSD plus a set of popular binary
packages for building a graphical workstation from the media.

After logging into either the installer shell or the installed system shell,
you can launch the default X11 session with:

```sh
startx
```

The seeded default session is a simple `twm` session from the `xorg` meta
package.

## Build

The primary workflow is now to fetch, verify, customize, and stage an official
FreeBSD release image.

```sh
make image
```

That downloads the official compressed image, verifies it against the official
FreeBSD SHA256 checksum file, overlays the OS-MASTER X11 installer hooks, and
stages the customized result under `image/`.

## Configuration

You can override the default release parameters:

```sh
make image \
  FREEBSD_RELEASE=15.0-RELEASE \
  FREEBSD_ARCH=amd64 \
  FREEBSD_IMAGE_BASENAME=dvd1.iso
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
