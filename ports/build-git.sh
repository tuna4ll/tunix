#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
# Reuse the shared static musl toolchain, but not gnu_autotools_port: git has no
# usable autotools build (its ./configure only writes config.mak.autogen for the
# same hand-rolled Makefile) and it cannot build out of tree, so we drive its
# Makefile directly the way build-iproute2.sh does.
source "$ROOT/ports/lib/gnu-port.sh"

PORT_NAME=git
SRC="$ROOT/ports/src/git"
BUILD="$OUT/git-build"
ROOT_DIR="$OUT/git-root"
DEPS="$OUT/git-deps"
ZLIB_SOURCE="$ROOT/ports/src/zlib"
ZLIB_BUILD="$OUT/git-zlib-build"
ZLIB_STAMP="$DEPS/.zlib-ready"
CURL_ROOT="$OUT/curl-root"
MBEDTLS_ROOT="$OUT/mbedtls-root"

[[ -f "$SRC/Makefile" ]] || gnu_port_fail "missing git source; run git submodule update --init --recursive"
[[ -x "$ZLIB_SOURCE/configure" ]] || \
    gnu_port_fail "missing zlib source; run git submodule update --init --recursive"
# https:// support is libcurl (built against mbedTLS) linked into git-remote-http.
# Both are separate ports the Makefile builds first; assert them so a hand-run
# fails loudly instead of quietly falling back to an http-less git.
[[ -f "$CURL_ROOT/usr/lib/libcurl.a" ]] || \
    gnu_port_fail "libcurl not found at $CURL_ROOT; build curl first (ports/build-curl.sh)"
[[ -f "$MBEDTLS_ROOT/usr/lib/libmbedtls.a" ]] || \
    gnu_port_fail "mbedTLS not found at $MBEDTLS_ROOT; build mbedtls first"

# Same reason as the make port: git's bundled compat/ sources predate C23, and
# since GCC 14 the default -std=gnu23 reads a K&R `extern char *foo ();` as
# `(void)`, which then clashes with musl's real prototypes. Pin the older
# standard for this port only; PORT_EXTRA_CFLAGS never leaks into musl's build.
PORT_EXTRA_CFLAGS="-std=gnu17"

gnu_port_detect_flags
gnu_port_ensure_toolchain

# zlib is git's one hard external dependency (loose objects and packfiles are
# deflate streams), so it has to exist as a static musl archive. The shared
# image-codecs port also builds zlib, but that is a different staged root and
# this script has to stand on its own, so build a private copy here. Stamped, so
# only the first git build pays for it.
if [[ ! -f "$ZLIB_STAMP" ]]; then
    rm -rf "$ZLIB_BUILD" "$DEPS"
    mkdir -p "$ZLIB_BUILD" "$DEPS"
    (
        cd "$ZLIB_BUILD"
        env CC="$MUSL_CC" AR="$HOST_AR" RANLIB="$HOST_RANLIB" \
            CFLAGS="$COMMON_CFLAGS" LDFLAGS="$COMMON_LDFLAGS" \
            "$ZLIB_SOURCE/configure" --prefix=/usr --static
        make -j"$JOBS"
        make install DESTDIR="$DEPS"
    )
    [[ -f "$DEPS/usr/lib/libz.a" ]] || gnu_port_fail "static zlib was not produced"
    : > "$ZLIB_STAMP"
fi

# GIT-VERSION-GEN runs `git describe` inside the source tree. We build from a
# copy (below) whose .git file points at a now-wrong relative gitdir, so resolve
# the version from the real submodule first and pass it in. Upstream's own
# transformation: drop the leading v, turn the describe dashes into dots.
GIT_VERSION=$(git -C "$SRC" describe --match='v[0-9]*' 2>/dev/null | sed -e 's/^v//' -e 's/-/./g' || true)
[[ -n "$GIT_VERSION" ]] || gnu_port_fail "could not determine the git version from $SRC"

# git builds in-tree; work from a throwaway copy so the submodule checkout stays
# clean across rebuilds. --exclude .git keeps the copy cheap and stops the stale
# gitdir pointer from confusing the build.
rm -rf "$BUILD" "$ROOT_DIR"
mkdir -p "$BUILD" "$ROOT_DIR"
tar -C "$SRC" --exclude=./.git -cf - . | tar -C "$BUILD" -xf -

# config.mak is read after the Makefile's own defaults and before config.mak.uname
# is allowed to matter, so this is the supported place to both set the toolchain
# and switch off features. Everything here is a deliberate choice about what
# Tunix can actually run.
cat > "$BUILD/config.mak" <<EOF
CC = $MUSL_CC
CFLAGS = $PORT_CFLAGS
LDFLAGS = $COMMON_LDFLAGS
AR = $HOST_AR
prefix = /usr

# --- external libraries ----------------------------------------------------
# libcurl (static, mbedTLS backend) provides the http/https transport. CURLDIR
# gives git the headers (-I) and an -L for the lib dir; CURL_CONFIG points at
# our staged curl-config for the version probe. CURL_LDFLAGS is set explicitly
# to the full static dependency chain because git otherwise runs
# `curl-config --libs`, which prints only -lcurl and would leave mbedTLS/x509/
# crypto unresolved at static link time. Order matters: libcurl.a before the
# mbedTLS archives, and libmbedtls -> libmbedx509 -> libmbedcrypto among them.
CURLDIR = $CURL_ROOT/usr
CURL_CONFIG = $CURL_ROOT/usr/bin/curl-config
CURL_LDFLAGS = $CURL_ROOT/usr/lib/libcurl.a \\
	$MBEDTLS_ROOT/usr/lib/libmbedtls.a \\
	$MBEDTLS_ROOT/usr/lib/libmbedx509.a \\
	$MBEDTLS_ROOT/usr/lib/libmbedcrypto.a
# NO_EXPAT stays: expat only feeds git-http-push (dumb-protocol pushing over
# WebDAV), which nothing uses. Smart-protocol fetch and push over https work
# without it.
NO_EXPAT = YesPlease
# No OpenSSL either. Dropping it selects git's bundled sha1collisiondetection
# for SHA-1 and its own SHA-256, which is the recommended default anyway.
NO_OPENSSL = YesPlease
# No perl, python or tcl/tk interpreters in the image. This drops send-email,
# svn, cvs, p4, gitweb, instaweb and gitk; the interactive porcelain that used
# to be Perl (add -i, add -p) has been C for many releases, so it stays.
NO_PERL = YesPlease
NO_PYTHON = YesPlease
NO_TCLTK = YesPlease
NO_GETTEXT = YesPlease
# Rust is on by default in this snapshot and would need cargo plus an
# x86_64-unknown-linux-musl static target. Building the C varint.o instead.
NO_RUST = YesPlease
ZLIB_PATH = $DEPS/usr

# --- libc gaps ------------------------------------------------------------
# musl's regex has no REG_STARTEND, which git relies on to match over buffers
# that are not NUL-terminated. Use the regex implementation git bundles.
NO_REGEX = NeedsStartEnd

# --- kernel gaps ----------------------------------------------------------
# sys_mmap on Tunix eagerly faults the whole mapping in by copying through a
# 256-byte bounce buffer (src/kernel/syscall.c), so mmap of a large packfile is
# both slow and as memory-hungry as a read. git's compat/mmap.c does the same
# job with bulk pread() and, importantly, NO_MMAP also drops the default
# core.packedGitWindowSize from 32 MiB to 1 MiB -- which is what we want on a
# 256 MiB machine.
NO_MMAP = YesPlease
# The kernel implements no link(2)/linkat(2) at all, so every object write
# would issue a doomed link() and fall back to rename(). Tell git to rename
# from the start.
OBJECT_CREATION_USES_RENAMES = YesPlease
# Threads exist (CLONE_THREAD + futex), but they are untested under the load
# pack-objects/index-pack would put on them. Single-threaded git is fully
# functional, only slower; flip this once kernel threading is trusted. This
# also disables the Simple-IPC fsmonitor daemon, which we do not want either.
NO_PTHREADS = YesPlease
# ...but NO_PTHREADS alone is not enough to turn the daemon off. config.mak.uname
# is included *before* this file (Makefile:1053-1055), so its Linux block has
# already picked FSMONITOR_DAEMON_BACKEND=linux while NO_PTHREADS was still
# unset. Later rules then drop the Simple-IPC objects (they do see NO_PTHREADS)
# but keep -DHAVE_FSMONITOR_DAEMON_BACKEND, so fsmonitor-ipc.c compiles its real
# implementation against ipc_client_* symbols that no longer exist. Clearing
# both here makes `ifdef` false and selects the trivial stubs.
FSMONITOR_DAEMON_BACKEND =
FSMONITOR_OS_SETTINGS =

# --- image size -----------------------------------------------------------
# The default install hardlinks ~130 dashed built-ins to the git binary. The
# initramfs is assembled with cp -R into a ustar archive, which would turn
# those into ~130 full copies of a multi-megabyte static binary. Skip the
# dashed forms entirely (modern git dispatches them internally).
SKIP_DASHED_BUILT_INS = YesPlease
# INSTALL_SYMLINKS rather than NO_INSTALL_HARDLINKS: the latter still falls
# back to a plain cp for the bin/ -> libexec/ copy of git itself, so the image
# would carry the binary twice. This symlinks to the final target instead.
INSTALL_SYMLINKS = YesPlease
EOF

(
    cd "$BUILD"
    make -j"$JOBS" GIT_VERSION="$GIT_VERSION"
    make install DESTDIR="$ROOT_DIR" GIT_VERSION="$GIT_VERSION"
)

# Docs are never built here, but prune defensively and consistently with the
# other ports. share/git-core/templates is deliberately kept: `git init` copies
# it into every new repository.
rm -rf "$ROOT_DIR/usr/share/man" "$ROOT_DIR/usr/share/doc" \
       "$ROOT_DIR/usr/share/locale" 2>/dev/null || true

[[ -x "$ROOT_DIR/usr/bin/git" ]] || gnu_port_fail "git binary was not produced"
[[ -d "$ROOT_DIR/usr/share/git-core/templates" ]] || \
    gnu_port_fail "git templates were not installed; git init would produce an empty repository"
# The https transport is a separate helper binary (git-remote-http, aliased to
# git-remote-https/ftp/ftps). If NO_CURL had silently re-activated, this would
# be missing and https clones would fail at runtime -- exactly the bug we set
# out to fix -- so make its absence a build error.
GIT_REMOTE_HTTP="$ROOT_DIR/usr/libexec/git-core/git-remote-http"
[[ -e "$GIT_REMOTE_HTTP" ]] || \
    gnu_port_fail "git-remote-http was not built; the https transport is missing"
[[ -e "$ROOT_DIR/usr/libexec/git-core/git-remote-https" ]] || \
    gnu_port_fail "git-remote-https alias was not installed"

if command -v readelf >/dev/null 2>&1; then
    for bin in "$ROOT_DIR/usr/bin/git" "$GIT_REMOTE_HTTP"; do
        # git-remote-https is an INSTALL_SYMLINKS symlink to git-remote-http;
        # resolve to the real file before inspecting the ELF.
        [[ -L "$bin" ]] && bin=$(readlink -f "$bin")
        if readelf -l "$bin" | grep -q 'INTERP'; then
            gnu_port_fail "$(basename "$bin") unexpectedly contains a dynamic interpreter"
        fi
        if readelf -d "$bin" 2>/dev/null | grep -q 'NEEDED'; then
            gnu_port_fail "$(basename "$bin") unexpectedly contains dynamic dependencies"
        fi
    done
fi

# Prove libcurl is actually linked into the https helper. Since the whole point
# of this port is the https transport, a helper that built but does not carry
# libcurl (e.g. a stray -DNO_CURL) is a silent regression we want to catch here.
# Capture nm's output into a variable first rather than piping into grep: under
# `set -o pipefail` nm's non-zero exit on any symbol-less section would
# otherwise be taken as the pipeline result and fire a false negative even when
# grep matched.
if command -v nm >/dev/null 2>&1; then
    http_real="$GIT_REMOTE_HTTP"
    [[ -L "$http_real" ]] && http_real=$(readlink -f "$http_real")
    http_syms=$(nm "$http_real" 2>/dev/null || true)
    grep -q 'curl_easy_' <<<"$http_syms" || \
        gnu_port_fail "git-remote-http does not reference libcurl; https would not work"
fi

# Static musl binaries run on the Linux build host too, so the built git can
# prove it works before it ever reaches Tunix. This is a real repository
# lifecycle -- init, add, commit, log -- not just --version.
smoke=$(mktemp -d)
trap 'rm -rf "$smoke"' EXIT
(
    export GIT_EXEC_PATH="$ROOT_DIR/usr/libexec/git-core"
    export GIT_TEMPLATE_DIR="$ROOT_DIR/usr/share/git-core/templates"
    export GIT_CONFIG_NOSYSTEM=1
    export HOME="$smoke"
    git_bin="$ROOT_DIR/usr/bin/git"

    reported=$("$git_bin" --version) || gnu_port_fail "built git did not report a version"
    [[ "$reported" == "git version $GIT_VERSION" ]] || \
        gnu_port_fail "built git reports '$reported', expected 'git version $GIT_VERSION'"

    cd "$smoke"
    "$git_bin" init -q repo || gnu_port_fail "git init failed"
    cd repo
    printf 'hello tunix\n' > file.txt
    "$git_bin" add file.txt || gnu_port_fail "git add failed"
    "$git_bin" -c user.name=tunix -c user.email=tunix@localhost \
        commit -q -m 'smoke test' || gnu_port_fail "git commit failed"
    subject=$("$git_bin" log -1 --format=%s) || gnu_port_fail "git log failed"
    [[ "$subject" == "smoke test" ]] || \
        gnu_port_fail "git log reported '$subject', expected 'smoke test'"
    # Reading the object back exercises zlib inflate and the compat mmap path.
    blob=$("$git_bin" cat-file -p HEAD:file.txt) || gnu_port_fail "git cat-file failed"
    [[ "$blob" == "hello tunix" ]] || gnu_port_fail "git cat-file returned '$blob'"
)

echo "git $GIT_VERSION root staged at $ROOT_DIR"
