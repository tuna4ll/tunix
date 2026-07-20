# shellcheck shell=bash
# Stage the host's Linux UAPI headers into a musl sysroot.
#
# Every Tunix toolchain compiles with -nostdinc, so it never sees the host's
# /usr/include/linux.  Plenty of ports need those headers anyway -- they are
# libc-independent kernel ABI definitions, not glibc ones:
#   * gnulib modules (copy-file-range, fiemap, ...)
#   * libdrm, which is nothing but a wrapper around <linux/drm.h>
#   * mesa's winsys code, which includes <linux/dma-buf.h> and friends
#
# So we copy them into whichever sysroot the caller is populating.  Callers:
#   ports/lib/gnu-port.sh          -> the static musl sysroot
#   ports/build-musl-cross.sh      -> the cross toolchain's sysroot
#
# Usage:
#   tunix_install_kernel_headers <include-dir> <fail-function>
# where <fail-function> is the caller's error reporter (it must not return).

# Idempotent: keyed on a sentinel written only after a complete install, so an
# aborted partial copy is retried on the next run rather than skipped.
tunix_install_kernel_headers() {
    local incdir="$1"
    local fail="${2:-tunix_kernel_headers_default_fail}"

    [[ -f "$incdir/.tunix-kernel-headers" ]] && return 0

    [[ -d /usr/include/linux ]] || "$fail" \
        "Linux kernel UAPI headers not found on the host; install them (e.g. 'sudo apt-get install linux-libc-dev' or 'pacman -S linux-api-headers')"

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

    [[ -f "$incdir/linux/version.h" ]] || "$fail" "failed to stage linux/version.h into the sysroot"
    [[ -f "$incdir/asm/unistd.h" || -f "$incdir/asm/types.h" ]] || \
        "$fail" "failed to stage asm/ kernel headers into the sysroot"
    : > "$incdir/.tunix-kernel-headers"
}

tunix_kernel_headers_default_fail() {
    echo "kernel-headers: $*" >&2
    exit 1
}
