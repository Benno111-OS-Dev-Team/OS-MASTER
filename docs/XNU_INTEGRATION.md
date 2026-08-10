# XNU Integration

OS8 treats Apple XNU as an external kernel source tree under `External/xnu`.
That tree is read-only input. Do not patch, generate into, or commit files under
`External/xnu`; keep OS8 glue in project-owned files such as `Makefile.multiarch`,
`scripts/`, and `.github/workflows/`.

`make -f Makefile.multiarch kernel` now dispatches through `KERNEL_PROVIDER`.
The default provider is `xnu`; `KERNEL_PROVIDER=os8` keeps the previous OS8
freestanding kernel available while the XNU boot path is being brought up.

CI fetches XNU from `https://github.com/apple-oss-distributions/xnu.git`, marks
the checkout read-only, and runs the XNU provider path in source-validation mode
on Ubuntu. Real XNU compilation is driven by `scripts/build-xnu-kernel.sh` on
macOS with Xcode and matching Apple kernel dependencies, with all generated
objects rooted under `build/<arch>/xnu`.

Local usage:

```sh
git clone https://github.com/apple-oss-distributions/xnu.git External/xnu
chmod -R a-w External/xnu
make -f Makefile.multiarch xnu-kernel
```

For the compatibility OS8 media build:

```sh
make -f Makefile.multiarch KERNEL_PROVIDER=os8 ARCH=x86_64 image
```

The existing OS8 freestanding kernel build remains available through the normal
`KERNEL_PROVIDER=os8` path while the XNU boot and ABI adaptation layer is
developed in repository-owned integration code.
