# Rust

Rust in this kernel is kept to code that reads what someone else wrote: input
that arrives from a file, a disk or a device and decides what the kernel then
does with memory. The first such piece is the ELF loader's parser; the second
is the LZ4 codec that zram stores swapped pages with, whose decoder reads back
bytes the kernel must not trust blindly. Everything that maps pages, takes
locks or touches `struct process` stays in C; Rust gets bytes and hands back a
plan or a buffer.

## Layout

```
rust/
  rust-toolchain.toml   the pinned compiler, its targets and components
  Cargo.toml            the workspace and the release profile
  .cargo/config.toml    code model flags for the x86-64 kernel
  kernel/               tunix-rust: the one static library the kernel links
  elf/                  tunix-elf: the ELF parser
  elf-check/            host-only tests against the C loader it replaced
  lz4/                  tunix-lz4: the LZ4 block codec
  lz4-check/            host-only tests against the system liblz4
subprojects/rust/RECIPE
```

`tunix-rust` builds as a static library for `x86_64-unknown-none` and
`aarch64-unknown-none-softfloat` and is linked into `kernel.elf` and the arm64
image. It holds the panic handler and pulls in the other crates, which are
plain Rust libraries: two static libraries would each carry their own copy of
`core` and clash at link time.

## The ELF parser

`tunix-elf`'s C face is `include/tunix/elf_plan.h`:

- `tunix_elf_header_span()` checks the ELF header and says how many bytes of
  headers to read: the program header table, at most 64 KiB, inside the file.
- `tunix_elf_plan()` validates every program header and returns the load bias,
  entry point, `AT_PHDR`, the image range, the interpreter's location and one
  record per `PT_LOAD` -- its address, file range, page range and permissions.
  Called once with no segment array it reports how many there are.
- `tunix_elf_interp_valid()` checks the interpreter path read from the file.

`core/elf.c` reads the headers, asks for the plan, then maps, copies and
protects exactly what the plan lists and builds the initial stack. It never
reads a program header itself.

## The LZ4 codec

`tunix-lz4` implements the LZ4 block format (no frame headers) for inputs up
to 64 KiB. Its C face is `include/tunix/lz4.h`:

- `tunix_lz4_compress()` takes the caller's 4096-entry hash table, so the
  kernel stack never holds it, and returns the compressed size or 0 when the
  result does not fit the output buffer.
- `tunix_lz4_decompress()` must produce exactly the expected number of bytes;
  any stream that is truncated, points before the start of the output, uses
  the reserved zero offset or ends with a match is refused.

## The toolchain

`make fetch` (and so any kernel build) downloads `rustup-init`, checks it
against the SHA-256 in the recipe, and installs the version named in
`rust-toolchain.toml` with both kernel targets, `rustfmt` and `clippy` into
`build/rust`. Nothing outside the build tree is touched, the system
`rustc` is not used, and CI installs the same compiler the same way.
`RUST_HOME=<dir>` points the build at an existing install, which is how
`make check` shares one between its three configurations.

## Rules the kernel code follows

- **No panics.** The library denies `indexing_slicing`, `arithmetic_side_effects`,
  `unwrap_used`, `expect_used`, `panic`, `as_conversions` and the like; every
  read is a bounds-checked `get()` and every sum a `checked_add`. The link step
  enforces it: if `rust_begin_unwind` or any `core` panicking symbol survives
  `--gc-sections` in the kernel image, the build fails and deletes the image.
  The panic handler that exists calls the kernel's `panic()`, and nothing
  reaches it.
- **No floating point or SIMD.** The targets are soft-float, like the C side's
  `-mgeneral-regs-only`; the kernel does not save FP state on kernel entry.
- **The C memory model.** On x86-64 the library is built with
  `code-model=kernel` and `relocation-model=static`, like `-mcmodel=kernel`;
  the image has no GOT or PLT. On arm64 the code only runs with the MMU on and
  reads ordinary kernel heap memory, where `SCTLR_EL1.A` is clear, so it does not
  need the C side's `-mstrict-align` (which is for code that runs before the
  MMU or on device memory).
- **No allocation, no globals, no locks.** The parser works on the slice it is
  given and writes into the caller's buffers.
- **Nothing from `core` is linked.** The library's object refers only to
  `memcpy` and `panic` in the kernel; `make` builds with `--locked`.

## Tests

```sh
make rust-test     # unit tests, then the differential tests
make rust-lint     # clippy, host and both kernel targets, warnings are errors
make format-check  # also runs cargo fmt --check
```

`elf-check` compiles `reference.c` -- the validation from the C loader this
replaced, unchanged -- and compares the two:

- every ELF file under `RUST_TEST_CORPUS` (by default `/usr/bin`, `/usr/lib`
  and `/usr/aarch64-linux-gnu`) is planned for both machines at both preferred
  bases, and the plans and segment lists must be identical;
- `RUST_FUZZ_ITERATIONS` mutated headers (200 000 by default, seeded by
  `RUST_FUZZ_SEED`) aim at the fields the loader trusts -- offsets, sizes,
  addresses, alignments, the entry point, the file size -- and the two must
  accept and reject the same inputs and agree on every result;
- every accepted plan is checked against invariants of its own: pages aligned
  and inside user space, file ranges inside the file, the entry point inside a
  loaded page, the program headers inside a loaded segment.

`lz4-check` links the system liblz4 and checks, for every page of the files
under `RUST_TEST_CORPUS` and for synthetic pages: our output decodes with
liblz4, liblz4's output decodes with ours, and both give back the input. It
then feeds `RUST_FUZZ_ITERATIONS` mutated streams to both decoders, which must
accept and reject the same streams and produce the same bytes. The one
intended difference is the zero offset: the LZ4 format calls it corrupt and
the kernel refuses it, while liblz4 accepts it; the test counts those apart.

For the ELF parser, the one intended difference: the old loader let an entry point overflow past
the top of the address space and wrap around into a mapping; the new one
refuses. No real binary does this, and the test fails if one in the corpus
does. A preferred base that is not page aligned is also refused; the kernel
only passes its two aligned constants.

## Adding a crate

Put it under `rust/` as an `rlib`, make it `no_std` on
`target_os = "none"`, give it the same lint list, and expose `extern "C"`
functions with `#[repr(C)]` types mirrored in a header with `_Static_assert`ed
sizes. Add it to `tunix-rust`'s dependencies and re-export its FFI module there.
Keep it panic-free; the link check will say so otherwise.
