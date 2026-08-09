# Package manager

Tunix ships **xbps**, the Void Linux package manager, and can install binary
packages from Void's own `x86_64-musl` repository.

That is possible for one reason: Void is one of the few distributions that
builds a complete **musl** package set, and Tunix's userland is musl too — the
same version, `1.2.6`. A Void musl binary asks for `/lib/ld-musl-x86_64.so.1`,
which is exactly what the image already provides.

```sh
xbps-install -r /void -S       # sync the repository index
xbps-install -r /void -y tree  # install into /void
/void/usr/bin/tree --version
```

## Why `-r`

`-r` is an install-time prefix: xbps writes the package tree, and its own
database, under the directory given rather than under `/`.

```
/void/usr/bin/…
/void/usr/lib/…
/void/var/db/xbps/     the record of what is installed there
```

This keeps Void's file set away from Tunix's own. The image already carries
hand-built glib, gtk3, cairo and musl; installing Void's versions of the same
libraries over them would replace what a running desktop is linked against.

**`-r` moves the configuration too**, which is easy to be caught by: xbps reads
`<root>/etc/xbps.d`, so `xbps-install -r /void` never looks at `/etc/xbps.d` and
finds no repository at all — the symptom is `Package 'x' not found in repository
pool` with nothing else wrong. The image therefore ships the same configuration
at `/void/etc/xbps.d/00-repository-main.conf`. For any other root, either seed
it the same way or pass the config directory explicitly:

```sh
xbps-install -r /elsewhere -C /etc/xbps.d -S
```

**`-r` isolates the install, not the execution.** A binary's ELF interpreter
path is absolute and baked in, so `/void/usr/bin/jq` runs with *Tunix's* loader
and looks for its libraries in Tunix's `/usr/lib`. Programs whose only
dependency is libc are unaffected. Anything else needs the library path spelled
out:

```sh
LD_LIBRARY_PATH=/void/usr/lib /void/usr/bin/jq .
```

or, equivalently, by invoking the loader directly — the form
`make dynamic-runtime-check` already uses:

```sh
/lib/ld-musl-x86_64.so.1 --library-path /void/usr/lib /void/usr/bin/jq .
```

On a normal Linux system `chroot /void` would make this unnecessary. Tunix has
no `chroot` syscall, so it does not arise. It turns out xbps does not need one
either: it chroots before running a package's `INSTALL` script only when the
target root contains `bin/sh`, which an `-r` tree of ordinary tools does not.

## What was built for it

| Port | Why |
| --- | --- |
| `zstd` | an `.xbps` package is a zstd-compressed tar |
| `libarchive` | opens that tar; configured for zstd + zlib only |
| `openssl` | xbps verifies repository signatures and speaks HTTPS with it |
| `xbps` | the package manager itself |

OpenSSL is installed as **libraries only** (`no-apps`). The image's
`/usr/bin/openssl` is not OpenSSL: it is the mbedTLS-backed `s_client` shim in
`tools/ssl-helper.c` that HTTPS clients shell out to, and it stays.

Two things needed care beyond an ordinary port:

**libarchive is built with ACLs and xattrs off.** Tunix has no xattr syscalls,
and a libarchive that believes it can set them fails an extraction rather than
skipping the attribute.

**xbps is a tarball port, not a submodule.** It carries its repository signing
key as `data/60:ae:…plist`, and a colon cannot appear in a filename on NTFS — a
submodule checkout of it fails outright on a Windows clone. Unpacking into
`/var/tmp`, as the gnutls and gmp ports already do, avoids the Windows
filesystem entirely. For the same reason the key is not staged into the image;
nothing is lost, because xbps offers to import a repository's key on first use
and writes it under the root it was given, on the Tunix ext2 filesystem where a
colon is an ordinary character.

## Configuration

`/etc/xbps.d/00-repository-main.conf` carries the name xbps ships in
`/usr/share/xbps.d`, so it replaces that file rather than adding to it:

```
architecture=x86_64-musl
repository=https://tunixos.github.io/tunix-ports/current/x86_64-musl
repository=https://repo-default.voidlinux.org/current/musl
```

`architecture` is not optional. `uname(2)` reports `x86_64` and can report
nothing else, but a musl userland is a different package set from a glibc one
and Void names it separately. Without the override xbps would ask for `x86_64`
packages and install binaries linked against a libc this system does not have.

**The order of the two repositories is the priority.** xbps searches the pool in
the order it was configured and takes the first repository that has the package,
regardless of which one has the newer version — `xbps-install tty-clock` installs
the Tunix build even though Void's `tty-clock-2.3_2` is newer. Void's set is
there for everything Tunix does not package itself.

The image also grew an `/etc/ssl/certs` directory — the same trust as the
existing `/etc/ssl/cert.pem`, split per certificate and hash-linked, because
xbps's bundled libfetch asks OpenSSL for its default certificate *directory*
rather than for a bundle.

## Verified

A boot with QEMU's user networking, installing `tree` from Void's own server:

```
[*] Updating repository `https://repo-default.voidlinux.org/current/musl/x86_64-musl-repodata'
x86_64-musl-repodata: 2118KB [avg rate: 1013KB/s]
`…/musl' repository has been RSA signed by "Void Linux"
Fingerprint: 60:ae:0c:d6:f0:95:17:80:bc:93:46:7a:89:af:a3:2d
musl-1.2.6_1: verifying RSA signature...
tree-2.2.1_1: verifying RSA signature...
2 downloaded, 2 installed, 0 updated, 2 configured, 0 removed, 0 on hold.

$ /void/usr/bin/tree --version
tree v2.2.1 © 1996 - 2024 by Steve Baker, Thomas Moore, …
```

Every layer is exercised there: HTTPS through OpenSSL against the new
`/etc/ssl/certs`, dependency resolution, RSA signature verification, zstd
decompression through libarchive, and finally a binary Void built, running.

The imported key lands at
`/void/var/db/xbps/keys/60:ae:…:2d.plist` — the same name that cannot exist on
the build host, written without trouble onto Tunix's ext2 filesystem.

One thing to expect on a first sync: xbps asks

```
Do you want to import this public key? [Y/n]
```

and `-y` does **not** answer it — upstream calls that prompt unconditionally.
Answer it once per repository.

## Tunix's own repository

Void's repository is somebody else's package set.
[tunix-ports](https://github.com/tunixos/tunix-ports) is ours: a separate
repository of package templates that GitHub Actions builds into `.xbps` packages
and publishes over GitHub Pages, so a Tunix machine can install software that
was built *for* Tunix rather than borrowed.

It is the **first** repository in the image's configuration, so nothing has to be
passed on the command line:

```sh
xbps-install -S
xbps-install -y tty-clock
```

The packages are compiled in a Void musl container, because its compiler already
targets the same musl the image is built with and the image ships `xbps-create`
— the same two facts that make Void's binaries usable here in the first place.
They are built **static**, so they depend on nothing: Tunix's own libc and
ncurses live in the image and no package owns them.

Two details are worth carrying over from that repository's
[README](https://github.com/tunixos/tunix-ports#readme): xbps demands an RSA
signature on every package served from a *remote* repository, so the publishing
key is not optional; and a package containing hard links cannot be unpacked
here, which the build checks for before it ever reaches the machine.

The published URL has been exercised end to end — from a musl root rather than
from Tunix, so it says nothing about the syscall surface, but it does prove the
repository itself:

```
[*] Updating repository `https://tunixos.github.io/tunix-ports/current/x86_64-musl/x86_64-musl-repodata'
`…/x86_64-musl' repository has been RSA signed by "Tunix ports (github.com/tunixos/tunix-ports)"
Fingerprint: fd:2b:de:5f:0e:5b:68:fd:4a:79:3e:c0:d5:80:ca:6f
tty-clock-2.0.20211121_1: verifying RSA signature...
2 downloaded, 2 installed, 0 updated, 2 configured, 0 removed, 0 on hold.
```

## Limits

**No hard links.** `struct vfs_node` has one `parent` and one `name`, so one
file cannot appear in two directories: the VFS is a strict tree, and a hard link
is not representable in it without separating inodes from directory entries.
`link()` is therefore not implemented, and xbps extracts through libarchive's
`archive_read_extract()`, which uses it. A package containing hard links will
fail to unpack.

**No chroot.** As above, this does not affect an `-r` install of ordinary tools,
but it does rule out populating an alternate root with a full Void base system
and entering it.

**Void binaries meet Tunix's syscall surface.** An unimplemented syscall returns
`ENOSYS`, so each new program can expose its own small tail of missing calls.
That is the ongoing cost of this route, and it is paid one program at a time.
