# Contributing

Thanks for taking an interest in Tunix. This project is a x86_64
Unix-like operating system experiment, so changes should stay focused,
understandable, and easy to test.

## Getting Started

Initialize third-party sources:

```sh
git submodule update --init --recursive
```

Build the image:

```sh
make all
```

Run it in QEMU:

```sh
make run
```

For a headless boot:

```sh
make headless
```

If generated files get stale:

```sh
make clean
make all
```

## Development Guidelines

- Keep changes small and scoped to one subsystem when possible.
- Prefer simple C code over clever abstractions.
- Follow the style of the surrounding file.
- Keep kernel code freestanding-friendly.
- Avoid pulling host headers, libraries, or assumptions into target builds.
- Do not commit generated output from `build/` or `ports/out/`.
- Do not patch vendored source under `ports/src/` unless there is a clear
  reason. Prefer wrapper code, compatibility headers, or port build-script
  changes.

## Testing

At minimum, run:

```sh
make all
```

For boot or runtime changes, also run:

```sh
make run
```

or:

```sh
make headless
```

For dynamic runtime or shared-library changes, run the relevant checks:

```sh
make dynamic-runtime-check
make shared-image-codecs-check
```

If you cannot run a relevant check, mention that in the commit or pull request.

## Documentation

Update docs when behavior, build steps, or development workflow changes.

Useful starting points:

- [README.md](README.md)
- [docs/build-and-run.md](docs/build-and-run.md)
- [docs/ports.md](docs/ports.md)

## Commit Messages

Use short, direct commit messages:

```text
area: describe the change
```

Examples:

```text
fix(kernel): fix file descriptor cleanup
feat(ports): add build checks for nano
docs: clarify boot instructions
```

## Pull Requests

A good pull request should include:

- What changed.
- Why it changed.
- What was tested.
- Any known limitations or follow-up work.

Keep unrelated cleanup out of functional changes unless it is necessary for the
patch.
