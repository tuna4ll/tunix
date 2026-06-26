# Tunix

Tunix is a very small x86_64 unix-like operating system

## TinyCC

Initialize the compiler source once:

```sh
./ports/init-tcc-submodule.sh
```

Build and validate the port:

```sh
make tcc-check
```

The normal image build installs `tcc`, the `cc` alias, musl headers, static libraries, startup objects, and TinyCC's runtime. Inside Tunix, run `tcc-smoke` to compile and execute the bundled C example from `/tmp`.
