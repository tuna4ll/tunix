#!/usr/bin/env bash
set -euo pipefail

# Build Mesa for Tunix: EGL + OpenGL ES on the softpipe software rasteriser.
#
# What this configuration is, and why:
#
#   softpipe        Tunix has no GPU driver, so rendering has to happen on the
#                   CPU. softpipe is gallium's reference rasteriser: pure C,
#                   no dependencies. The faster alternative, llvmpipe, would
#                   drag in a musl cross-build of LLVM.
#   EGL surfaceless The EGL surfaceless platform needs no window system and no
#                   DRM device -- it renders into framebuffer objects -- which
#                   is exactly what Tunix can offer today. Everything reaches
#                   the screen by reading pixels back and blitting them to
#                   /dev/fb0 (see tools/tunix-gl-demo.c).
#   GLES2           Desktop libGL is the GLX library, and GLX needs X11. With
#                   GLX disabled and GLVND off, the client API mesa exposes is
#                   OpenGL ES 2 through libEGL/libGLESv2. Desktop GL is still
#                   compiled into gallium, it just has no shippable entrypoint
#                   library until there is a window system.
#   GBM             Built and shipped because it was asked for and because it
#                   is the piece a future kernel DRM driver would plug into.
#                   It is honestly non-functional right now: gbm_create_device
#                   needs a /dev/dri file descriptor and Tunix has no such node.
#
# Deliberately off: LLVM, Vulkan, GLX, X11/Wayland, video codecs, and the
# zlib/zstd/expat-backed features (shader cache and driconf), which would each
# add a cross-built dependency for something a software rasteriser on a
# read-only initramfs does not benefit from.
#
# Output layout:
#   $OUT/graphics-sysroot/usr/{include,lib}  headers + .pc, for building clients
#   $OUT/mesa-root/usr/lib                   libEGL/libGLESv2/libgbm + dri/
#   $OUT/mesa-root/usr/bin/tunix-gl-demo     renders a frame to /dev/fb0

PORT_NAME=mesa
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${OUT:-$ROOT/ports/out}

# shellcheck source=ports/lib/cross-port.sh
source "$ROOT/ports/lib/cross-port.sh"

SOURCE="$ROOT/ports/src/mesa"
BUILD="$OUT/mesa-build"
ROOT_DIR="$OUT/mesa-root"
CROSS_FILE="$OUT/tunix-meson-cross.ini"
DEMO_SOURCE="$ROOT/tools/tunix-gl-demo.c"

EXPECTED_VERSION_PREFIX=26.2

[[ -f "$SOURCE/meson.build" ]] || cross_port_fail \
    "missing mesa source at $SOURCE; run git submodule update --init --recursive"
[[ -f "$DEMO_SOURCE" ]] || cross_port_fail "missing the GL demo source at $DEMO_SOURCE"

cross_port_require_toolchain
cross_port_require_tools meson ninja pkg-config python3 flex bison "$READELF"
# Mesa generates a large part of its source from Mako templates, and drives some
# of that generation from YAML descriptions.
cross_port_require_python_module mako "pacman -S python-mako, or pip install mako"
cross_port_require_python_module yaml "pacman -S python-yaml, or pip install pyyaml"

version=$(tr -d '[:space:]' < "$SOURCE/VERSION")
[[ "$version" == "$EXPECTED_VERSION_PREFIX"* ]] || \
    cross_port_fail "expected mesa $EXPECTED_VERSION_PREFIX.x, found ${version:-unknown}"

# Mesa's GBM links against libdrm, so the libdrm port has to have populated the
# graphics sysroot first.
[[ -f "$GRAPHICS_SYSROOT/usr/lib/pkgconfig/libdrm.pc" ]] || cross_port_fail \
    "libdrm is not in the graphics sysroot; run ports/build-libdrm.sh first"
# The staged libdrm root matters too: the runtime closure check below needs to
# see what libdrm contributes to the image, not just what mesa links against.
LIBDRM_ROOT="$OUT/libdrm-root"
[[ -d "$LIBDRM_ROOT/usr/lib" ]] || cross_port_fail \
    "the staged libdrm root is missing; run ports/build-libdrm.sh first"

rm -rf "$BUILD" "$ROOT_DIR"
mkdir -p "$BUILD" "$ROOT_DIR"

cross_port_write_meson_cross "$CROSS_FILE"
cross_port_export_pkg_config

meson setup "$BUILD" "$SOURCE" \
    --cross-file "$CROSS_FILE" \
    --prefix=/usr \
    --libdir=lib \
    --buildtype=release \
    --default-library=shared \
    -Dgallium-drivers=softpipe \
    -Dvulkan-drivers= \
    -Dplatforms= \
    -Degl=enabled \
    -Degl-native-platform=surfaceless \
    -Dgbm=enabled \
    -Dglx=disabled \
    -Dopengl=true \
    -Dgles1=disabled \
    -Dgles2=enabled \
    -Dshared-glapi=enabled \
    -Dglvnd=disabled \
    -Dllvm=disabled \
    -Ddraw-use-llvm=false \
    -Dgallium-rusticl=false \
    -Dshader-cache=disabled \
    -Dzlib=disabled \
    -Dzstd=disabled \
    -Dexpat=disabled \
    -Dxmlconfig=disabled \
    -Ddisplay-info=disabled \
    -Dlibunwind=disabled \
    -Dlmsensors=disabled \
    -Dvalgrind=disabled \
    -Dselinux=false \
    -Dperfetto=false \
    -Dvideo-codecs= \
    -Dtools= \
    -Dbuild-tests=false \
    -Dhtml-docs=disabled

meson compile -C "$BUILD" -j "$JOBS"

DESTDIR="$GRAPHICS_SYSROOT" meson install -C "$BUILD" --no-rebuild
DESTDIR="$ROOT_DIR" meson install -C "$BUILD" --no-rebuild

for header in EGL/egl.h GLES2/gl2.h gbm.h; do
    [[ -f "$GRAPHICS_SYSROOT/usr/include/$header" ]] || \
        cross_port_fail "$header was not installed into the graphics sysroot"
done

check_shared() {
    local name="$1" soname="$2"
    local library
    library=$(find "$ROOT_DIR/usr/lib" -maxdepth 1 -type f -name "$name" -print -quit)
    [[ -n "$library" ]] || cross_port_fail "$name was not installed"
    cross_port_check_library "$library" "$soname"
}

check_shared 'libEGL.so.1.*' libEGL.so.1
check_shared 'libGLESv2.so.2.*' libGLESv2.so.2
check_shared 'libgbm.so.1.*' libgbm.so.1

# The gallium megadriver and the GBM backend are dlopen()ed rather than linked,
# so a missing one shows up only at runtime as "failed to load driver". Note
# that mesa no longer installs per-driver modules under /usr/lib/dri: since the
# megadriver merge there is a single versioned libgallium-<version>.so sitting
# directly in the library directory.
driver=$(find "$ROOT_DIR/usr/lib" -maxdepth 1 -name 'libgallium-*.so' -print -quit)
[[ -n "$driver" ]] || cross_port_fail "the gallium megadriver was not installed"
[[ -f "$ROOT_DIR/usr/lib/gbm/dri_gbm.so" ]] || \
    cross_port_fail "the GBM backend was not installed"

# libgallium is C++, so the image needs the C++ runtime that nothing else in
# Tunix pulls in.
cross_port_stage_cxx_runtime "$ROOT_DIR/usr/lib"

# Build the framebuffer demo against the sysroot we just populated. It is the
# only end-to-end proof that the libraries actually initialise a GL context.
mkdir -p "$ROOT_DIR/usr/bin"
"$CROSS_CC" -std=c11 -Wall -Wextra -O2 -fPIE -pie \
    -I"$GRAPHICS_SYSROOT/usr/include" \
    -I"$ROOT/src/include" \
    "$DEMO_SOURCE" \
    -L"$GRAPHICS_SYSROOT/usr/lib" \
    -Wl,-rpath-link,"$GRAPHICS_SYSROOT/usr/lib" \
    -lEGL -lGLESv2 -lm \
    -o "$ROOT_DIR/usr/bin/tunix-gl-demo"

interp=$("$READELF" -l "$ROOT_DIR/usr/bin/tunix-gl-demo" | \
    sed -n 's/.*Requesting program interpreter: \([^]]*\).*/\1/p')
[[ "$interp" == "/lib/ld-musl-x86_64.so.1" ]] || \
    cross_port_fail "tunix-gl-demo has unexpected interpreter '${interp:-missing}'"

needed=$("$READELF" -d "$ROOT_DIR/usr/bin/tunix-gl-demo" | \
    sed -n 's/.*Shared library: \[\([^]]*\)\].*/\1/p')
for dependency in libEGL.so.1 libGLESv2.so.2; do
    grep -Fxq "$dependency" <<<"$needed" || \
        cross_port_fail "tunix-gl-demo is missing NEEDED $dependency"
done

# Run it here before shipping it. A Tunix binary uses the same syscall ABI as
# the build host, so the target loader can run it in place; --probe renders one
# frame into memory and reports the GL strings without touching /dev/fb0, which
# does not exist on the build host.
# The library path has to cover libdrm too, and it has to be exhaustive: musl's
# loader falls back to the host's default search path, where a distribution
# libdrm built against glibc is waiting to be picked up and fail to relocate.
probe_output=$("$CROSS_LOADER" \
    --library-path "$CROSS_SYSROOT/lib:$ROOT_DIR/usr/lib:$GRAPHICS_SYSROOT/usr/lib" \
    "$ROOT_DIR/usr/bin/tunix-gl-demo" --probe)
printf '%s\n' "$probe_output"
grep -q 'softpipe' <<<"$probe_output" || \
    cross_port_fail "the GL renderer is not softpipe: $probe_output"

# Development-only files never reach the image. The unversioned .so symlinks go
# too: they exist for the linker, and everything at runtime resolves either by
# SONAME (libEGL.so.1) or by the exact filename mesa dlopen()s.
rm -rf "$ROOT_DIR/usr/include" "$ROOT_DIR/usr/lib/pkgconfig" \
       "$ROOT_DIR/usr/share/drirc.d" "$ROOT_DIR/usr/share/doc"
find "$ROOT_DIR/usr/lib" -maxdepth 1 -name '*.a' -delete
rm -f "$ROOT_DIR/usr/lib/libEGL.so" "$ROOT_DIR/usr/lib/libGLESv2.so" \
      "$ROOT_DIR/usr/lib/libgbm.so"

cross_port_finalize_root "$ROOT_DIR"
"$CROSS_STRIP" --strip-all "$ROOT_DIR/usr/bin/tunix-gl-demo"

# Last: everything the image will contain is now staged, so check that the
# graphics libraries can actually resolve each other on Tunix.
cross_port_check_runtime_closure "$ROOT_DIR" "$LIBDRM_ROOT"

printf 'mesa %s (softpipe, EGL surfaceless, GLES2, GBM) staged at %s\n' \
    "$version" "$ROOT_DIR"
