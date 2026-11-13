# Tunix

A Unix-like kernel for x86_64 and aarch64 with a Linux-compatible system call
interface, so it runs unmodified Linux userland binaries.

## Build

```sh
make               # x86_64 kernel and modules -> build/kernel.elf, build/modules/
make aarch64-core  # aarch64 kernel -> build/kernel-aarch64-core.img
make check         # the default, debug-log and boot-timing configurations
make rust-test     # unit and differential tests of the Rust code
```

`KASAN=1` and `FRAME_POINTERS=1` build debugging kernels. The first build
fetches Limine's header, uACPI and the pinned Rust toolchain into `build/`.

The kernel boots through Limine on BIOS or UEFI and mounts its root from
`root=` on the command line; see `docs/`.

## License

GPL-3.0. The console font glyphs in `tty/terminal_font.c` are from Terminus
Font, under the SIL Open Font License 1.1 reproduced at the top of that file.
