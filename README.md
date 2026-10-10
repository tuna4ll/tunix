# Tunix

[![ci](https://github.com/tuna4ll/tunix/actions/workflows/ci.yml/badge.svg)](https://github.com/tuna4ll/tunix/actions/workflows/ci.yml)

![screenshot](./assets/screenshots/gnome.png)

A Unix-like kernel for x86_64 and aarch64 that runs an unmodified Void Linux userland.

## Build and run

```sh
make        # kernel and disk image
make run    # boot it in QEMU
make test   # run the kernel tests
```

`make -C kernel` builds only the kernel and its modules, into `kernel/build/`.

Log in as `root` with the password `tunix`.

## Layout

```
kernel/       the kernel, with its own GNUmakefile and subprojects/
base-files/   files added to the Void userland
boot/         Limine boot menus for x86_64 and aarch64
tools/        image and sysroot builders, hardware report
testsuites/   kernel boot tests and the ACPI model test
utils/        small programs for Tunix images, such as the CI boot init
ci/           the scripts CI runs, each one runnable locally
docs/         kernel documentation
```

## License

MIT
