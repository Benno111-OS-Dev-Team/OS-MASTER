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

## Graphical Installer

The staged installer image now includes:

- a loader splash bitmap during boot
- a live X11 installer session for the installer environment
- X11 installation into the installed FreeBSD system during `bsdinstall`
- a graphical XDM login on the installed system
- a first-boot retry service if the installed-system X11 package step could
  not complete during setup

The live installer root session launches an OS-MASTER graphical installer
desktop with shortcuts to:

- install OS-MASTER with `bsdinstall` inside a graphical terminal
- open a live shell
- browse files
- view installer help
- reboot the installer environment

This still uses official `bsdinstall` underneath, but it is wrapped in a
graphical X11 session.

The installed system is configured to present a graphical login on `ttyv8`
and launch the restored OS-MASTER desktop after sign-in.

If you land in a shell instead, you can still launch the default X11 session
manually with:

```sh
startx
```

If the current shell has not reloaded its login profile yet, the direct path is:

```sh
/usr/local/bin/startx
```

The seeded default installed session launches the restored OS-MASTER desktop
shell rather than plain `twm`.

## Build

The primary workflow is now to fetch, verify, customize, and stage an official
FreeBSD release image.

```sh
make image
```

That downloads the official compressed image, verifies it against the official
FreeBSD SHA256 checksum file, overlays the OS-MASTER X11 installer hooks, and
stages the customized result under `image/`.

## Local Windows Build

On this repository's Windows setup, the easiest local build path is through
WSL because the customization flow depends on Linux tools like `xorriso` and
`xz`.

From PowerShell in the repo root:

```powershell
.\scripts\build-local.ps1
```

That script:

- uses your current WSL installation
- installs missing Linux build dependencies with `apt`
- runs the same FreeBSD image staging flow locally

You can skip the dependency install on later runs:

```powershell
.\scripts\build-local.ps1 -SkipDependencyInstall
```

You can also override the release inputs:

```powershell
.\scripts\build-local.ps1 -FreebsdRelease 15.0-RELEASE -FreebsdImageBasename dvd1.iso
```

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
