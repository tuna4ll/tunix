# shellcheck shell=bash
# Shared build logic for the GNU autotools userland ports:
#   coreutils, grep, sed, gawk, findutils, diffutils, tar, gzip
#
# Each ports/build-<name>.sh sources this file, sets a few PORT_* variables,
# and calls gnu_autotools_port.  The heavy lifting (static musl toolchain,
# gnulib bootstrap, configure/make/install, doc pruning) lives here so the
# per-port scripts stay tiny and consistent.
#
# Why a "native" musl build instead of a --host cross build: Tunix uses the
# same x86_64 Linux syscall ABI as the build host, only with musl instead of
# glibc.  A static binary produced by musl-gcc therefore runs on both the
# Linux build host and on Tunix, so configure can compile *and run* its probe
# programs (AC_RUN checks) exactly like a native build -- this is the same
# trick ports/build-bash.sh uses, and it avoids the cross-compile cache-var
# guesswork that gnulib's feature checks would otherwise require.
#
# Required before sourcing:
#   ROOT  - absolute path to the repository root.
# Set by the caller before calling gnu_autotools_port:
#   PORT_NAME             - short port name (also names the out/ dirs).
#   PORT_SRC              - absolute path to the upstream source tree.
# Optional:
#   PORT_CONFIGURE_ARGS   - bash array of extra ./configure arguments.
#   PORT_CONFIGURE_ENV    - bash array of extra KEY=VALUE configure env.
#   PORT_VERIFY           - name of a function run after install for checks.

: "${ROOT:?gnu-port.sh: ROOT must be set before sourcing}"

JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}
OUT=${OUT:-$ROOT/ports/out}
MUSL_SOURCE="$ROOT/ports/src/musl"
SYSROOT="$OUT/sysroot"
MUSL_BUILD="$OUT/musl-build"
MUSL_CC="$SYSROOT/usr/bin/musl-gcc"
# Shared with the other static ports; only the first port build pays the cost
# of compiling musl, the rest reuse the stamped sysroot.
TOOLCHAIN_STAMP="$SYSROOT/.tunix-musl-toolchain-v4"
HOST_CC=${CC:-gcc}
HOST_AR=${AR:-ar}
HOST_RANLIB=${RANLIB:-ranlib}

gnu_port_fail() {
    echo "build-${PORT_NAME:-gnu-port}: $*" >&2
    exit 1
}

# Detect whether the host gcc auto-links libatomic; if it does not, drop the
# flag so we never accidentally require a shared libatomic in a static image.
gnu_port_detect_flags() {
    NO_AUTO_ATOMIC=""
    local probe
    probe=$(mktemp)
    printf 'int main(void){return 0;}\n' > "$probe"
    if "$HOST_CC" -fno-link-libatomic -x c "$probe" -o "$probe.bin" >/dev/null 2>&1; then
        NO_AUTO_ATOMIC="-fno-link-libatomic"
    fi
    rm -f "$probe" "$probe.bin"
    COMMON_CFLAGS="-O2 -fno-stack-protector -fno-pie $NO_AUTO_ATOMIC"
    PORT_CFLAGS="-Os -fno-stack-protector -fno-pie $NO_AUTO_ATOMIC"
    COMMON_LDFLAGS="-static -no-pie $NO_AUTO_ATOMIC"
}

# Build the static musl toolchain into the shared sysroot if it is not there
# already.  Identical to the block in build-bash.sh / build-binutils.sh.
gnu_port_ensure_toolchain() {
    [[ -x "$MUSL_SOURCE/configure" ]] || \
        gnu_port_fail "missing musl source; run git submodule update --init --recursive"
    if [[ ! -x "$MUSL_CC" || ! -f "$TOOLCHAIN_STAMP" ]]; then
        rm -rf "$MUSL_BUILD" "$SYSROOT"
        mkdir -p "$MUSL_BUILD" "$SYSROOT"
        (
            cd "$MUSL_BUILD"
            env CC="$HOST_CC" AR="$HOST_AR" RANLIB="$HOST_RANLIB" \
                CFLAGS="$COMMON_CFLAGS" LDFLAGS="$NO_AUTO_ATOMIC" \
                "$MUSL_SOURCE/configure" --prefix="$SYSROOT/usr" --disable-shared
            make -j"$JOBS"
            make install
        )
        : > "$TOOLCHAIN_STAMP"
    fi
    gnu_port_ensure_kernel_headers
}

# The musl-gcc wrapper compiles with -nostdinc, so it never sees the host's
# /usr/include/linux. Several gnulib modules (copy-file-range, fiemap, ...)
# include Linux UAPI headers, which are libc-independent, so we drop the host's
# kernel headers into the shared musl sysroot. Idempotent: keyed on version.h.
gnu_port_ensure_kernel_headers() {
    local incdir="$SYSROOT/usr/include"
    # Sentinel (not version.h): only a fully-completed install counts, so an
    # aborted partial copy is retried rather than skipped.
    [[ -f "$incdir/.tunix-kernel-headers" ]] && return 0

    [[ -d /usr/include/linux ]] || gnu_port_fail \
        "Linux kernel UAPI headers not found on the host; install them (e.g. 'sudo apt-get install linux-libc-dev')"

    mkdir -p "$incdir"
    # The sysroot may live on a case-insensitive filesystem (e.g. a Windows
    # drive mounted under WSL), where the netfilter headers' upper/lowercase
    # pairs (xt_MARK.h vs xt_mark.h) collide. We never use those, so tolerate
    # the copy errors and assert the headers we actually depend on afterwards.
    cp -a /usr/include/linux "$incdir/" 2>/dev/null || true
    [[ -d /usr/include/asm-generic ]] && { cp -a /usr/include/asm-generic "$incdir/" 2>/dev/null || true; }
    # asm/ is arch-specific and lives under a multiarch path on Debian/Ubuntu;
    # prefer that real directory, and dereference if /usr/include/asm is a symlink.
    if [[ -d /usr/include/x86_64-linux-gnu/asm ]]; then
        cp -a /usr/include/x86_64-linux-gnu/asm "$incdir/" 2>/dev/null || true
    elif [[ -d /usr/include/asm ]]; then
        cp -aL /usr/include/asm "$incdir/" 2>/dev/null || true
    fi

    [[ -f "$incdir/linux/version.h" ]] || gnu_port_fail "failed to stage linux/version.h into the sysroot"
    [[ -f "$incdir/asm/unistd.h" || -f "$incdir/asm/types.h" ]] || gnu_port_fail "failed to stage asm/ kernel headers into the sysroot"
    : > "$incdir/.tunix-kernel-headers"
}

# GNU packages tracked from git ship only configure.ac; ./configure has to be
# generated by ./bootstrap, which needs the gnulib submodule populated first.
# Skip entirely when the tree already carries a configure (e.g. gawk).
# When a package already carries generated autotools output (gawk ships
# configure; a bootstrapped tree keeps it across rebuilds), a git checkout's
# arbitrary mtimes can make `make` think configure/aclocal.m4/Makefile.in are
# stale and try to re-run a pinned aclocal-<ver>/automake-<ver> the host may not
# have. Stamp the generated files strictly newer than their sources, with fixed
# timestamps so ordering holds regardless of filesystem timestamp granularity.
gnu_port_freeze_autotools() {
    local src="$1"
    (
        cd "$src" || exit 0
        # 1) sources oldest
        find . \( -name 'configure.ac' -o -name 'configure.in' \
                  -o -name 'acinclude.m4' -o -name '*.m4' -o -name 'Makefile.am' \) \
            -print0 2>/dev/null | xargs -0 -r touch -t 200001010000 2>/dev/null || true
        # 2) aclocal.m4 (depends on configure.ac + m4 files)
        [[ -f aclocal.m4 ]] && touch -t 200001010001 aclocal.m4
        # 3) configure and config header template (depend on aclocal.m4)
        [[ -x configure ]] && touch -t 200001010002 configure
        # 4) everything generated from *.in (Makefile.in, config.h.in) newest
        find . -name '*.in' -print0 2>/dev/null | xargs -0 -r touch -t 200001010003 2>/dev/null || true
    )
}

gnu_port_bootstrap() {
    local src="$1"
    if [[ -x "$src/configure" ]]; then
        gnu_port_freeze_autotools "$src"
        return 0
    fi
    [[ -f "$src/bootstrap" ]] || gnu_port_fail "no configure and no bootstrap in $src"
    command -v git >/dev/null 2>&1 || gnu_port_fail "git is required to bootstrap $PORT_NAME"
    for tool in autoconf automake autopoint makeinfo; do
        command -v "$tool" >/dev/null 2>&1 || \
            gnu_port_fail "$tool is required to bootstrap $PORT_NAME from a git checkout"
    done
    # Populate every submodule, not just gnulib: tar's bootstrap.conf copies
    # Make.rules and the paxlib sources out of paxutils, and an empty submodule
    # makes those copies silently no-ops that only surface much later as an
    # "automake: cannot open < Make.rules" failure.
    if [[ -f "$src/.gitmodules" ]]; then
        local sub
        while read -r sub; do
            [[ -n "$sub" ]] || continue
            # An initialised submodule has content; skip the ones already there
            # so a rebuild does not re-walk a populated gnulib checkout.
            [[ -n "$(ls -A "$src/$sub" 2>/dev/null)" ]] && continue
            git -C "$src" submodule update --init -- "$sub" || \
                gnu_port_fail "failed to fetch the $sub submodule for $PORT_NAME"
        done < <(git -C "$src" config --file .gitmodules --get-regexp '^submodule\..*\.path$' | cut -d' ' -f2-)
    fi
    # gnu_port_prepare_stubs (called from gnu_autotools_port) has already put
    # harmless wget/help2man stubs on PATH for us.
    (
        cd "$src"
        # --no-git:  use the gnulib checkout we just populated instead of letting
        #            bootstrap run its own git plumbing against the pinned submodule.
        # --skip-po: never download translation catalogs (they are stripped from
        #            the image anyway); this avoids a network fetch during build.
        ./bootstrap --no-git --skip-po --gnulib-srcdir="$src/gnulib"
    ) || gnu_port_fail "bootstrap failed for $PORT_NAME"
}

# Some GNU packages list doc/network tools (wget, help2man) as bootstrap
# prerequisites, but with --skip-po and install-exec (no man pages) we never
# actually need them. Provide harmless stubs so a host lacking them can still
# build, and keep them on PATH for the whole port build so configure/make see a
# consistent (do-nothing) help2man too. Echoes the stub dir.
gnu_port_prepare_stubs() {
    local stub_dir="$OUT/bootstrap-stubs"
    mkdir -p "$stub_dir"
    if ! command -v wget >/dev/null 2>&1; then
        printf '#!/bin/sh\nexit 0\n' > "$stub_dir/wget"
        chmod +x "$stub_dir/wget"
    fi
    if ! command -v help2man >/dev/null 2>&1; then
        cat > "$stub_dir/help2man" <<'EOF'
#!/bin/sh
# Stub: man pages are never shipped (install-exec). Satisfy prerequisite checks
# and, if actually invoked, emit a trivial page to -o <file> or to stdout.
out=""; prev=""
for a in "$@"; do case "$prev" in -o|--output) out="$a";; esac; prev="$a"; done
if [ -n "$out" ]; then printf '.TH STUB 1\n' > "$out"; else printf '.TH STUB 1\n'; fi
exit 0
EOF
        chmod +x "$stub_dir/help2man"
    fi
    printf '%s\n' "$stub_dir"
}

# Strip docs/locale that we never ship in the initramfs, keeping the staged
# root small.  Executables and libexec helpers are preserved.
gnu_port_prune_root() {
    local root="$1"
    rm -rf "$root/usr/share/man" "$root/usr/share/info" \
           "$root/usr/share/doc" "$root/usr/share/locale" \
           "$root/usr/share/emacs" 2>/dev/null || true
    # Drop now-empty share/ so the copy into rootfs stays clean.
    rmdir "$root/usr/share" 2>/dev/null || true
}

gnu_port_default_verify() {
    :
}

# Recreate a front-end that upstream ships as a shell script (egrep, fgrep,
# gunzip, zcat, ...).  `make install-exec` skips those because automake treats
# them as data, so we synthesize an equivalent wrapper.  Leaves any real file
# already at the path untouched.
#   gnu_port_make_wrapper <path> <command> [args...]
gnu_port_make_wrapper() {
    local path="$1"
    shift
    [[ -e "$path" ]] && return 0
    mkdir -p "$(dirname "$path")"
    cat > "$path" <<EOF
#!/bin/sh
exec $* "\$@"
EOF
    chmod 0755 "$path"
}

gnu_autotools_port() {
    : "${PORT_NAME:?PORT_NAME must be set}"
    : "${PORT_SRC:?PORT_SRC must be set}"
    # Default the optional arrays so referencing them is safe under `set -u`.
    declare -p PORT_CONFIGURE_ARGS >/dev/null 2>&1 || PORT_CONFIGURE_ARGS=()
    declare -p PORT_CONFIGURE_ENV  >/dev/null 2>&1 || PORT_CONFIGURE_ENV=()
    local build_dir="$OUT/$PORT_NAME-build"
    local root_dir="$OUT/$PORT_NAME-root"

    [[ -d "$PORT_SRC" ]] || \
        gnu_port_fail "missing source at $PORT_SRC; run git submodule update --init --recursive"
    command -v "$HOST_AR" >/dev/null 2>&1 || gnu_port_fail "$HOST_AR was not found"
    command -v "$HOST_RANLIB" >/dev/null 2>&1 || gnu_port_fail "$HOST_RANLIB was not found"

    gnu_port_detect_flags
    gnu_port_ensure_toolchain
    # Put the wget/help2man stubs on PATH for bootstrap, configure and make.
    local stub_dir
    stub_dir=$(gnu_port_prepare_stubs)
    export PATH="$stub_dir:$PATH"
    gnu_port_bootstrap "$PORT_SRC"
    [[ -x "$PORT_SRC/configure" ]] || gnu_port_fail "configure was not generated for $PORT_NAME"

    rm -rf "$build_dir" "$root_dir"
    mkdir -p "$build_dir" "$root_dir"

    local -a configure_args=(
        --prefix=/usr
        --disable-nls
        --disable-dependency-tracking
    )
    configure_args+=( "${PORT_CONFIGURE_ARGS[@]}" )

    (
        cd "$build_dir"
        export CC="$MUSL_CC"
        export CFLAGS="$PORT_CFLAGS"
        export LDFLAGS="$COMMON_LDFLAGS"
        # Export the port env (e.g. FORCE_UNSAFE_CONFIGURE=1) for the whole
        # build, not just configure: when run as root (WSL default), a
        # config.status --recheck triggered during `make` re-runs configure and
        # would otherwise lose it and hit coreutils/tar's "don't run as root".
        for kv in "${PORT_CONFIGURE_ENV[@]}"; do export "$kv"; done
        # Neutralize automake maintainer-mode regeneration. A git checkout's
        # mtimes make `make` think configure/aclocal.m4/Makefile.in are stale and
        # re-run a *version-specific* aclocal-/automake-/autoconf (e.g. gawk was
        # generated with automake 1.16, which the host may not have). Pointing
        # these at `true` makes those rules no-ops so the already-correct shipped
        # files are used as-is. MAKEINFO is left alone: its `missing` fallback is
        # non-fatal and install-exec never ships the docs anyway.
        local -a automake_stubs=(
            ACLOCAL=true AUTOCONF=true AUTOMAKE=true AUTOHEADER=true AUTOM4TE=true
        )
        "$PORT_SRC/configure" "${configure_args[@]}"
        make -j"$JOBS" "${automake_stubs[@]}"
        # install-exec stages the binaries and libexec helpers but skips the
        # man/info/locale data, so we never depend on doc tooling at install
        # time and the staged root stays lean.
        make install-exec DESTDIR="$root_dir" "${automake_stubs[@]}"
    )

    gnu_port_prune_root "$root_dir"
    "${PORT_VERIFY:-gnu_port_default_verify}" "$root_dir"
    echo "$PORT_NAME root staged at $root_dir"
}
