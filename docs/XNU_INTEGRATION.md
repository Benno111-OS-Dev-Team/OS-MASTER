# XNU Integration

OS8 treats Apple XNU as an external kernel source tree under `External/xnu`.
That tree is read-only input. Do not patch, generate into, or commit files under
`External/xnu`; keep OS8 glue in project-owned files such as `Makefile.multiarch`,
`scripts/`, and `.github/workflows/`.

CI fetches XNU from `https://github.com/apple-oss-distributions/xnu.git`, marks
the checkout read-only, and runs `make -f Makefile.multiarch xnu-kernel` to
verify that the external kernel provider is present and isolated.

Local usage:

```sh
git clone https://github.com/apple-oss-distributions/xnu.git External/xnu
chmod -R a-w External/xnu
make -f Makefile.multiarch xnu-kernel
```

The existing OS8 freestanding kernel build remains available through the normal
`kernel`, `image`, and installer targets while the XNU boot and ABI adaptation
layer is developed in repository-owned integration code.
