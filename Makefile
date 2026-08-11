SHELL := /bin/bash
.DELETE_ON_ERROR:

CC ?= gcc
LD ?= ld
NASM ?= nasm
STRIP ?= strip
AR ?= ar
PYTHON ?= python3
QEMU ?= qemu-system-x86_64
# The HDA controller the sound driver drives. The "none" backend still clocks
# samples out in real time, so the driver is exercised exactly as it would be
# with speakers; override QEMU_AUDIO with a real -audiodev to actually hear it.
QEMU_AUDIO ?= -audiodev none,id=snd0 -device intel-hda -device hda-output,audiodev=snd0

BUILD := build
IMAGE := $(BUILD)/tunix.img
KERNEL := $(BUILD)/kernel.elf
INITRAMFS := $(BUILD)/initramfs.img
ROOTFS := $(BUILD)/rootfs
PORT_OUT := ports/out
BASH := $(PORT_OUT)/bash
COREUTILS_ROOT := $(PORT_OUT)/coreutils-root
COREUTILS_STAMP := $(PORT_OUT)/.coreutils-ready
GREP_ROOT := $(PORT_OUT)/grep-root
GREP_STAMP := $(PORT_OUT)/.grep-ready
SED_ROOT := $(PORT_OUT)/sed-root
SED_STAMP := $(PORT_OUT)/.sed-ready
GAWK_ROOT := $(PORT_OUT)/gawk-root
GAWK_STAMP := $(PORT_OUT)/.gawk-ready
FINDUTILS_ROOT := $(PORT_OUT)/findutils-root
FINDUTILS_STAMP := $(PORT_OUT)/.findutils-ready
DIFFUTILS_ROOT := $(PORT_OUT)/diffutils-root
DIFFUTILS_STAMP := $(PORT_OUT)/.diffutils-ready
TAR_ROOT := $(PORT_OUT)/tar-root
TAR_STAMP := $(PORT_OUT)/.tar-ready
GZIP_ROOT := $(PORT_OUT)/gzip-root
GZIP_STAMP := $(PORT_OUT)/.gzip-ready
# GNUMAKE_ rather than MAKE_: MAKE is special to GNU make itself.
GNUMAKE_ROOT := $(PORT_OUT)/make-root
GNUMAKE_STAMP := $(PORT_OUT)/.make-ready
IPROUTE2_ROOT := $(PORT_OUT)/iproute2-root
IPROUTE2_STAMP := $(PORT_OUT)/.iproute2-ready
CURL_ROOT := $(PORT_OUT)/curl-root
CURL_STAMP := $(PORT_OUT)/.curl-ready
GIT_ROOT := $(PORT_OUT)/git-root
GIT_STAMP := $(PORT_OUT)/.git-ready
GNU_PORT_STAMPS := $(COREUTILS_STAMP) $(GREP_STAMP) $(SED_STAMP) $(GAWK_STAMP) \
	$(FINDUTILS_STAMP) $(DIFFUTILS_STAMP) $(TAR_STAMP) $(GZIP_STAMP) \
	$(GNUMAKE_STAMP)
GNU_PORT_ROOTS := $(COREUTILS_ROOT) $(GREP_ROOT) $(SED_ROOT) $(GAWK_ROOT) \
	$(FINDUTILS_ROOT) $(DIFFUTILS_ROOT) $(TAR_ROOT) $(GZIP_ROOT) \
	$(GNUMAKE_ROOT)
TCC_ROOT := $(PORT_OUT)/tcc-root
TCC_STAMP := $(PORT_OUT)/.tcc-ready
BINUTILS_ROOT := $(PORT_OUT)/binutils-root
BINUTILS_STAMP := $(PORT_OUT)/.binutils-ready
NCURSES_ROOT := $(PORT_OUT)/ncurses-root
NCURSES_STAMP := $(PORT_OUT)/.ncurses-ready
NANO := $(PORT_OUT)/nano
TTY_CLOCK := $(PORT_OUT)/tty-clock
TTY_TETRIS := $(PORT_OUT)/tty-tetris
HTOP := $(PORT_OUT)/htop
FASTFETCH_ROOT := $(PORT_OUT)/fastfetch-root
FASTFETCH_STAMP := $(PORT_OUT)/.fastfetch-ready
LUA := $(PORT_OUT)/lua
LUA_ROOT := $(PORT_OUT)/lua-root
LUA_STAMP := $(PORT_OUT)/.lua-ready
IMAGE_CODECS_ROOT := $(PORT_OUT)/image-codecs-root
IMAGE_CODECS_STAMP := $(PORT_OUT)/.image-codecs-ready
IMAGE_CODECS_SHARED_ROOT := $(PORT_OUT)/image-codecs-shared-root
IMAGE_CODECS_SHARED_STAMP := $(PORT_OUT)/.image-codecs-shared-ready
DESKTOP_SYSROOT := $(PORT_OUT)/desktop-sysroot
MUSL_SHARED_ROOT := $(PORT_OUT)/musl-shared-root
# Graphics stack. These are the only ports built with a real cross toolchain
# rather than the musl-gcc wrapper, because mesa is C++ and the host libstdc++
# is unusable against musl; see ports/build-musl-cross.sh.
MUSL_CROSS := $(PORT_OUT)/musl-cross
MUSL_CROSS_STAMP := $(PORT_OUT)/.musl-cross-ready
GRAPHICS_SYSROOT := $(PORT_OUT)/graphics-sysroot
LIBFFI_ROOT := $(PORT_OUT)/libffi-root
LIBFFI_STAMP := $(PORT_OUT)/.libffi-ready
WAYLAND_ROOT := $(PORT_OUT)/wayland-root
WAYLAND_STAMP := $(PORT_OUT)/.wayland-ready
PIXMAN_ROOT := $(PORT_OUT)/pixman-root
PIXMAN_STAMP := $(PORT_OUT)/.pixman-ready
LIBXKBCOMMON_ROOT := $(PORT_OUT)/libxkbcommon-root
LIBXKBCOMMON_STAMP := $(PORT_OUT)/.libxkbcommon-ready
XKEYBOARD_CONFIG_ROOT := $(PORT_OUT)/xkeyboard-config-root
XKEYBOARD_CONFIG_STAMP := $(PORT_OUT)/.xkeyboard-config-ready
WAYLAND_PROTOCOLS_STAMP := $(PORT_OUT)/.wayland-protocols-ready
LIBEVDEV_ROOT := $(PORT_OUT)/libevdev-root
LIBEVDEV_STAMP := $(PORT_OUT)/.libevdev-ready
ALSA_LIB_ROOT := $(PORT_OUT)/alsa-lib-root
ALSA_LIB_STAMP := $(PORT_OUT)/.alsa-lib-ready
LIBUDEV_ZERO_ROOT := $(PORT_OUT)/libudev-zero-root
LIBUDEV_ZERO_STAMP := $(PORT_OUT)/.libudev-zero-ready
LIBINPUT_ROOT := $(PORT_OUT)/libinput-root
LIBINPUT_STAMP := $(PORT_OUT)/.libinput-ready
CAIRO_ROOT := $(PORT_OUT)/cairo-root
CAIRO_STAMP := $(PORT_OUT)/.cairo-ready
LIBDISPLAY_INFO_ROOT := $(PORT_OUT)/libdisplay-info-root
LIBDISPLAY_INFO_STAMP := $(PORT_OUT)/.libdisplay-info-ready
SEATD_ROOT := $(PORT_OUT)/seatd-root
SEATD_STAMP := $(PORT_OUT)/.seatd-ready
WESTON_ROOT := $(PORT_OUT)/weston-root
WESTON_STAMP := $(PORT_OUT)/.weston-ready
LIBDRM_ROOT := $(PORT_OUT)/libdrm-root
LIBDRM_STAMP := $(PORT_OUT)/.libdrm-ready
MESA_ROOT := $(PORT_OUT)/mesa-root
MESA_STAMP := $(PORT_OUT)/.mesa-ready
# LLVM (musl cross-build), just the X86 codegen + JIT in one libLLVM.so, for
# mesa's llvmpipe rasteriser. The compile is long, so it builds in /var/tmp.
LLVM_ROOT := $(PORT_OUT)/llvm-root
LLVM_STAMP := $(PORT_OUT)/.llvm-ready
# The X11 / Xorg foundation, toward a native Xfce desktop. xorgproto is the
# protocol headers everything else in the X stack builds against; it stages only
# headers and pkg-config data into the sysroot, no image payload.
XORGPROTO_STAMP := $(PORT_OUT)/.xorgproto-ready
# The base of the X client stack (xtrans, libXau, libXdmcp, xcb-proto, libxcb):
# the first cross-autotools ports, built against the graphics sysroot.
XCB_ROOT := $(PORT_OUT)/xcb-root
XCB_STAMP := $(PORT_OUT)/.xcb-ready
# libX11: the core Xlib, on the xcb stack. Ships the X11 locale/error databases
# the runtime needs, not just the .so.
LIBX11_ROOT := $(PORT_OUT)/libX11-root
LIBX11_STAMP := $(PORT_OUT)/.libX11-ready
# The X extension client libraries (Xext, Xrender, Xfixes, Xdamage, Xcomposite,
# Xrandr, Xi, Xcursor, Xtst, XRes, Xinerama, Xkbfile) X clients link.
XEXT_ROOT := $(PORT_OUT)/xext-root
XEXT_STAMP := $(PORT_OUT)/.xext-ready
# The Xorg server's font stack: font-util, encodings, libfontenc, libXfont2
# (freetype-backed). The server loads fonts through libXfont2.
FONTSTACK_ROOT := $(PORT_OUT)/fontstack-root
FONTSTACK_STAMP := $(PORT_OUT)/.fontstack-ready
# The Xorg X server (with libxcvt + libpciaccess bundled): Xorg (modesetting DDX,
# software shadow fb) + Xvfb, on the X libraries below. SHA1 from libgcrypt.
XSERVER_ROOT := $(PORT_OUT)/xserver-root
XSERVER_STAMP := $(PORT_OUT)/.xserver-ready
# xcb-util: the small XCB utility library (xcb-aux/xcb-event/xcb-atom); nothing in
# the weston stack wanted it, but startup-notification below needs it.
XCB_UTIL_ROOT := $(PORT_OUT)/xcb-util-root
XCB_UTIL_STAMP := $(PORT_OUT)/.xcb-util-ready
# startup-notification: the freedesktop launch-feedback library, a libwnck (and
# later xfce4-panel) dependency; links libxcb + xcb-util + x11-xcb.
STARTUP_NOTIFICATION_ROOT := $(PORT_OUT)/startup-notification-root
STARTUP_NOTIFICATION_STAMP := $(PORT_OUT)/.startup-notification-ready
# libICE + libSM: X11 Inter-Client Exchange + Session Management, for
# libxfce4ui's XfceSMClient (xfwm4 / xfce4-session save-and-restore).
LIBSM_ROOT := $(PORT_OUT)/libsm-root
LIBSM_STAMP := $(PORT_OUT)/.libsm-ready
# libwnck-3: the window-list / pager / tasklist library, the first GTK3 consumer
# that needs the x11 backend and a hard xfwm4 dependency.
LIBWNCK_ROOT := $(PORT_OUT)/libwnck-root
LIBWNCK_STAMP := $(PORT_OUT)/.libwnck-ready
# xfwm4: the Xfce window manager, the first real Xfce X11 component on the Tunix
# Xorg server (an ordinary X client with an Xrender compositor).
XFWM4_ROOT := $(PORT_OUT)/xfwm4-root
XFWM4_STAMP := $(PORT_OUT)/.xfwm4-ready
# D-Bus: libdbus-1 + the dbus-daemon message bus. The Xfce session's config store
# (xfconfd) and its components reach each other over a D-Bus session bus; GLib's
# GDBus is only the client side and still needs this daemon.
DBUS_ROOT := $(PORT_OUT)/dbus-root
DBUS_STAMP := $(PORT_OUT)/.dbus-ready
# garcon: the freedesktop menu library (libgarcon-1 + libgarcon-gtk3-1), used by
# the panel and xfdesktop for the applications menu.
GARCON_ROOT := $(PORT_OUT)/garcon-root
GARCON_STAMP := $(PORT_OUT)/.garcon-ready
# libxfce4windowing: Xfce's windowing abstraction (x11 backend), needed by the
# panel, xfdesktop and the session manager.
LIBXFCE4WINDOWING_ROOT := $(PORT_OUT)/libxfce4windowing-root
LIBXFCE4WINDOWING_STAMP := $(PORT_OUT)/.libxfce4windowing-ready
# xfce4-panel: the Xfce panel (taskbar), the most visible session component.
XFCE4_PANEL_ROOT := $(PORT_OUT)/xfce4-panel-root
XFCE4_PANEL_STAMP := $(PORT_OUT)/.xfce4-panel-ready
# xfce4-session: the session manager (xfce4-session + startxfce4), on libSM/libICE.
XFCE4_SESSION_ROOT := $(PORT_OUT)/xfce4-session-root
XFCE4_SESSION_STAMP := $(PORT_OUT)/.xfce4-session-ready
# xfdesktop: the desktop manager (wallpaper + desktop menu).
XFDESKTOP_ROOT := $(PORT_OUT)/xfdesktop-root
XFDESKTOP_STAMP := $(PORT_OUT)/.xfdesktop-ready
# libxml2: the GNOME XML library. Only libxkbcommon's libxkbregistry wants it
# (built minimal), which xfce4-settings' keyboard support needs.
LIBXML2_ROOT := $(PORT_OUT)/libxml2-root
LIBXML2_STAMP := $(PORT_OUT)/.libxml2-ready
# xfce4-settings: the settings daemon (xfsettingsd) + the settings dialogs.
# xfsettingsd applies the GTK theme/font/cursor/keyboard from xfconf.
XFCE4_SETTINGS_ROOT := $(PORT_OUT)/xfce4-settings-root
XFCE4_SETTINGS_STAMP := $(PORT_OUT)/.xfce4-settings-ready
# ICU: Unicode. libvte links libicuuc, and webkit links the whole set.
ICU_ROOT := $(PORT_OUT)/icu-root
ICU_STAMP := $(PORT_OUT)/.icu-ready
# The rest of the WebKit stack: storage, image/font codecs, crypto, the HTTP
# client, then the engine (webkit2gtk-4.0 on GTK3 + libsoup2).
SQLITE_ROOT := $(PORT_OUT)/sqlite-root
SQLITE_STAMP := $(PORT_OUT)/.sqlite-ready
LIBWEBP_ROOT := $(PORT_OUT)/libwebp-root
LIBWEBP_STAMP := $(PORT_OUT)/.libwebp-ready
WOFF2_ROOT := $(PORT_OUT)/woff2-root
WOFF2_STAMP := $(PORT_OUT)/.woff2-ready
LIBGCRYPT_ROOT := $(PORT_OUT)/libgcrypt-root
LIBGCRYPT_STAMP := $(PORT_OUT)/.libgcrypt-ready
LIBTASN1_ROOT := $(PORT_OUT)/libtasn1-root
LIBTASN1_STAMP := $(PORT_OUT)/.libtasn1-ready
# TLS. gnutls stands on nettle, which stands on gmp; glib-networking is the
# module that registers it as GIO's TLS backend. Without that last piece
# https:// does not exist as far as libsoup and WebKit are concerned.
GMP_ROOT := $(PORT_OUT)/gmp-root
GMP_STAMP := $(PORT_OUT)/.gmp-ready
NETTLE_ROOT := $(PORT_OUT)/nettle-root
NETTLE_STAMP := $(PORT_OUT)/.nettle-ready
GNUTLS_ROOT := $(PORT_OUT)/gnutls-root
GNUTLS_STAMP := $(PORT_OUT)/.gnutls-ready
GLIB_NETWORKING_ROOT := $(PORT_OUT)/glib-networking-root
GLIB_NETWORKING_STAMP := $(PORT_OUT)/.glib-networking-ready
LIBSOUP_ROOT := $(PORT_OUT)/libsoup-root
LIBSOUP_STAMP := $(PORT_OUT)/.libsoup-ready
WEBKITGTK_ROOT := $(PORT_OUT)/webkitgtk-root
WEBKITGTK_STAMP := $(PORT_OUT)/.webkitgtk-ready
SDL2_ROOT := $(PORT_OUT)/sdl2-root
SDL2_STAMP := $(PORT_OUT)/.sdl2-ready
SDL2_NET_ROOT := $(PORT_OUT)/sdl2-net-root
SDL2_NET_STAMP := $(PORT_OUT)/.sdl2-net-ready
SDL2_MIXER_ROOT := $(PORT_OUT)/sdl2-mixer-root
SDL2_MIXER_STAMP := $(PORT_OUT)/.sdl2-mixer-ready
CHOCOLATE_DOOM_ROOT := $(PORT_OUT)/chocolate-doom-root
CHOCOLATE_DOOM_STAMP := $(PORT_OUT)/.chocolate-doom-ready
# VTE: the GNOME terminal widget (libvte-2.91), pinned to 0.72 to avoid the fmt/
# simdutf/fast_float deps newer VTE needs. The engine behind xfce4-terminal.
VTE_ROOT := $(PORT_OUT)/vte-root
VTE_STAMP := $(PORT_OUT)/.vte-ready
# xfce4-terminal: the Xfce terminal emulator, on VTE.
XFCE4_TERMINAL_ROOT := $(PORT_OUT)/xfce4-terminal-root
XFCE4_TERMINAL_STAMP := $(PORT_OUT)/.xfce4-terminal-ready
# The welcome screen the session opens on a first boot.
WELCOME_ROOT := $(PORT_OUT)/welcome-root
WELCOME_STAMP := $(PORT_OUT)/.welcome-ready
# The GTK stack, layered on the graphics sysroot: glib (with pcre2), the text
# shapers (fribidi, harfbuzz, pango), the image loader (gdk-pixbuf with a
# shared libjpeg), and gtk3 itself (with cairo-gobject, atk and libepoxy).
GLIB_ROOT := $(PORT_OUT)/glib-root
GLIB_STAMP := $(PORT_OUT)/.glib-ready
PANGO_ROOT := $(PORT_OUT)/pango-root
PANGO_STAMP := $(PORT_OUT)/.pango-ready
GDK_PIXBUF_ROOT := $(PORT_OUT)/gdk-pixbuf-root
GDK_PIXBUF_STAMP := $(PORT_OUT)/.gdk-pixbuf-ready
GTK3_ROOT := $(PORT_OUT)/gtk3-root
GTK3_STAMP := $(PORT_OUT)/.gtk3-ready
# The Xfce stack, layered on the GTK3 sysroot, leading up to the Thunar file
# manager: libxfce4util (base utilities) is the first rung.
LIBXFCE4UTIL_ROOT := $(PORT_OUT)/libxfce4util-root
LIBXFCE4UTIL_STAMP := $(PORT_OUT)/.libxfce4util-ready
# xfconf: the Xfce configuration store. Talks GDBus (no libdbus needed);
# libxfce4ui and Thunar link its client library libxfconf.
XFCONF_ROOT := $(PORT_OUT)/xfconf-root
XFCONF_STAMP := $(PORT_OUT)/.xfconf-ready
# libxfce4ui: the Xfce GTK widget library (wayland backend), plus the private
# keyboard library. The last dependency below Thunar.
LIBXFCE4UI_ROOT := $(PORT_OUT)/libxfce4ui-root
LIBXFCE4UI_STAMP := $(PORT_OUT)/.libxfce4ui-ready
# Thunar: the Xfce file manager, the top of the stack.
THUNAR_ROOT := $(PORT_OUT)/thunar-root
THUNAR_STAMP := $(PORT_OUT)/.thunar-ready
# The init system: dinit is PID 1 (via the /sbin/init symlink), linked
# statically so init can never break on a missing loader or libstdc++.
DINIT_ROOT := $(PORT_OUT)/dinit-root
DINIT_STAMP := $(PORT_OUT)/.dinit-ready
# Accounts and privilege: shadow owns /etc/shadow and the login/su/passwd
# family, sudo is the setuid path to root.
SHADOW_ROOT := $(PORT_OUT)/shadow-root
SHADOW_STAMP := $(PORT_OUT)/.shadow-ready
SUDO_ROOT := $(PORT_OUT)/sudo-root
SUDO_STAMP := $(PORT_OUT)/.sudo-ready
# Linux-PAM: the authentication stack. One policy in /etc/pam.d instead of a
# copy of the logic in every program that logs somebody in, and the hard
# requirement LightDM has no way around.
LINUX_PAM_ROOT := $(PORT_OUT)/linux-pam-root
LINUX_PAM_STAMP := $(PORT_OUT)/.linux-pam-ready
# LightDM: the display manager. The daemon owns the X server and the session;
# liblightdm-gobject is the library the greeter talks to it through.
LIGHTDM_ROOT := $(PORT_OUT)/lightdm-root
LIGHTDM_STAMP := $(PORT_OUT)/.lightdm-ready
# The login screen: an ordinary GTK3 X client with no privileges of its own.
LIGHTDM_GREETER_ROOT := $(PORT_OUT)/lightdm-gtk-greeter-root
LIGHTDM_GREETER_STAMP := $(PORT_OUT)/.lightdm-gtk-greeter-ready
# CPython, shared so ctypes and third-party extensions can be dlopen'd.
CPYTHON_ROOT := $(PORT_OUT)/cpython-root
CPYTHON_STAMP := $(PORT_OUT)/.cpython-ready
# xbps, the Void Linux package manager, and the three libraries under it. An
# .xbps package is a zstd-compressed tar that libarchive opens; OpenSSL is what
# xbps verifies repository signatures and speaks HTTPS with.
ZSTD_ROOT := $(PORT_OUT)/zstd-root
ZSTD_STAMP := $(PORT_OUT)/.zstd-ready
OPENSSL_ROOT := $(PORT_OUT)/openssl-root
OPENSSL_STAMP := $(PORT_OUT)/.openssl-ready
LIBARCHIVE_ROOT := $(PORT_OUT)/libarchive-root
LIBARCHIVE_STAMP := $(PORT_OUT)/.libarchive-ready
XBPS_ROOT := $(PORT_OUT)/xbps-root
XBPS_STAMP := $(PORT_OUT)/.xbps-ready
BOOT_CONFIG_STAMP := $(BUILD)/.boot-config-ready
MUSL_SHARED_STAMP := $(PORT_OUT)/.musl-shared-ready
MBEDTLS_ROOT := $(PORT_OUT)/mbedtls-root
MBEDTLS_STAMP := $(PORT_OUT)/.mbedtls-ready
HTTPS_GET := $(PORT_OUT)/https-get
SSL_HELPER := $(PORT_OUT)/openssl
TERMINAL_FONT_SOURCE ?= assets/fonts/jetbrains-mono/JetBrainsMono-Regular.ttf
TERMINAL_FONT_DATA := $(BUILD)/generated/terminal_font_data.inc

COMMON_CFLAGS := -std=gnu11 -Wall -Wextra -Werror -ffreestanding -fno-stack-protector \
	-fno-pic -fno-pie -fno-builtin -fno-asynchronous-unwind-tables -fno-unwind-tables \
	-mno-red-zone -m64 -Os -ffunction-sections -fdata-sections
# -mgeneral-regs-only keeps the kernel out of the FPU, SSE and MMX registers
# entirely. Those registers belong to whichever process is running: the kernel
# saves them only when it switches processes, so any use in between -- GCC will
# happily reach for %xmm0 to copy a 16-byte struct -- corrupts user state on a
# plain syscall, with no context switch in sight. That is not theoretical: it is
# what produced NaNs in weston-smoke's simulation.
KERNEL_CFLAGS := $(COMMON_CFLAGS) -mcmodel=kernel -mgeneral-regs-only \
	-Isrc/kernel/include -Isrc/include -I$(BUILD)/generated
KERNEL_LDFLAGS := -nostdlib -no-pie -Wl,-T,src/kernel/arch/x86_64/linker.ld \
	-Wl,--gc-sections -Wl,--build-id=none -Wl,-z,max-page-size=0x1000
USER_CFLAGS := $(COMMON_CFLAGS) -mcmodel=small -Isrc/libc/include -Isrc/include
USER_LDFLAGS := -nostdlib -static -T src/userspace/linker.ld --build-id=none \
	-z max-page-size=0x1000 --gc-sections

KERNEL_OBJS := \
	$(BUILD)/tunix_boot.o \
	$(BUILD)/entry.o $(BUILD)/main.o $(BUILD)/serial.o \
	$(BUILD)/kprintf.o $(BUILD)/kstring.o $(BUILD)/gdt.o $(BUILD)/percpu.o $(BUILD)/klock.o $(BUILD)/idle.o $(BUILD)/smp.o $(BUILD)/trampoline.o \
	$(BUILD)/idt.o $(BUILD)/isr.o $(BUILD)/isr_handler.o $(BUILD)/pic.o $(BUILD)/timer.o \
	$(BUILD)/pmm.o $(BUILD)/vmm.o $(BUILD)/framebuffer.o $(BUILD)/terminal_font.o $(BUILD)/terminal.o $(BUILD)/input.o \
	$(BUILD)/heap.o $(BUILD)/syscall.o $(BUILD)/syscall_entry.o \
	$(BUILD)/eventfd.o $(BUILD)/timerfd.o $(BUILD)/epoll.o $(BUILD)/inotify.o \
	$(BUILD)/memfd.o $(BUILD)/sysvshm.o $(BUILD)/signalfd.o $(BUILD)/drm.o $(BUILD)/sysfs.o \
	$(BUILD)/vfs.o $(BUILD)/tarfs.o $(BUILD)/ext2.o $(BUILD)/devfs.o $(BUILD)/unix_socket.o $(BUILD)/pty.o \
	$(BUILD)/usercopy.o $(BUILD)/elf.o $(BUILD)/file.o $(BUILD)/cred.o \
	$(BUILD)/pipe.o $(BUILD)/tty.o $(BUILD)/process.o $(BUILD)/procfs.o $(BUILD)/time.o $(BUILD)/random.o $(BUILD)/ata.o \
	$(BUILD)/acpi.o $(BUILD)/apic.o $(BUILD)/xhci.o \
	$(BUILD)/sound.o $(BUILD)/hda.o \
	$(BUILD)/virtio_pci.o $(BUILD)/virtio_ring.o $(BUILD)/virtio_gpu.o \
	$(BUILD)/pci.o $(BUILD)/rtl8139.o $(BUILD)/net.o $(BUILD)/inet_socket.o $(BUILD)/netlink.o

USER_RUNTIME := $(BUILD)/user/crt0.o $(BUILD)/user/libc.o $(BUILD)/user/sigreturn.o
PROCUTIL := $(BUILD)/user/procutil.o
LOADKEYS := $(BUILD)/user/loadkeys
SLEEP := $(BUILD)/user/sleep
PREEMPT_TEST := $(BUILD)/user/preempt-test
SMP_TEST := $(BUILD)/user/smp-test
INPUT_TEST := $(BUILD)/user/input-test
FB_TEST := $(BUILD)/user/fb-test
FB_SHOT := $(BUILD)/user/fb-shot
GLIB_COMPAT_TEST := $(BUILD)/user/glib-compat-test
SND_TEST := $(BUILD)/user/snd-test
LINK_TEST := $(BUILD)/user/link-test
TCP_TEST := $(BUILD)/user/tcp-test
MOUNT_TOOLS := $(BUILD)/user/mount $(BUILD)/user/umount
SYSTEM_TOOLS := $(BUILD)/user/ps $(BUILD)/user/free $(BUILD)/user/uptime $(BUILD)/user/top $(LOADKEYS) $(SLEEP) $(PREEMPT_TEST) $(SMP_TEST) $(INPUT_TEST) $(FB_TEST) $(FB_SHOT) $(GLIB_COMPAT_TEST) $(SND_TEST) $(LINK_TEST) $(TCP_TEST) $(MOUNT_TOOLS)
INITRD_FILES := $(shell find initrd -type f 2>/dev/null)

.PHONY: all run run-gpu headless qemu-ci terminal-font dynamic-runtime-check shared-image-codecs-check gl-check clean
all: $(IMAGE)

terminal-font: $(TERMINAL_FONT_DATA)

$(IMAGE_CODECS_STAMP): $(BASH) ports/build-image-codecs.sh tools/tunix-wallpaper.c | $(BUILD)/.tools
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" bash ports/build-image-codecs.sh
	@touch $@


$(MUSL_SHARED_STAMP): ports/build-musl-shared.sh \
	tools/dynamic-runtime/dynamic-hello.c \
	tools/dynamic-runtime/libtunix-dynamic.c \
	tools/dynamic-runtime/dlopen-test.c \
	tools/dynamic-runtime/pthread-test.c \
	tools/dynamic-runtime/shm-test.c \
	tools/dynamic-runtime/signalfd-test.c \
	tools/dynamic-runtime/kill-blocked-test.c \
	tools/dynamic-runtime/pty-test.c \
	tools/dynamic-runtime/exec-mem-test.c
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" bash ports/build-musl-shared.sh
	@test -x $(MUSL_SHARED_ROOT)/lib/ld-musl-x86_64.so.1 || { echo "shared musl loader was not produced" >&2; exit 1; }
	@test -x $(MUSL_SHARED_ROOT)/usr/bin/dynamic-runtime-check || { echo "dynamic runtime checks were not produced" >&2; exit 1; }
	@touch $@

dynamic-runtime-check: $(MUSL_SHARED_STAMP)
	$(MUSL_SHARED_ROOT)/lib/ld-musl-x86_64.so.1 \
		--library-path $(MUSL_SHARED_ROOT)/lib:$(PORT_OUT)/musl-shared-sysroot/usr/lib:$(MUSL_SHARED_ROOT)/usr/lib \
		$(MUSL_SHARED_ROOT)/usr/bin/dynamic-hello make-check
	$(MUSL_SHARED_ROOT)/lib/ld-musl-x86_64.so.1 \
		--library-path $(MUSL_SHARED_ROOT)/lib:$(PORT_OUT)/musl-shared-sysroot/usr/lib:$(MUSL_SHARED_ROOT)/usr/lib \
		$(MUSL_SHARED_ROOT)/usr/bin/dlopen-test $(abspath $(MUSL_SHARED_ROOT)/usr/lib/libtunix_dynamic.so.1)

$(IMAGE_CODECS_SHARED_STAMP): $(MUSL_SHARED_STAMP) \
	ports/build-image-codecs-shared.sh \
	tools/shared-image-codecs-test.c \
	ports/src/zlib/configure \
	ports/src/libpng/CMakeLists.txt \
	ports/src/libjpeg-turbo/CMakeLists.txt
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" bash ports/build-image-codecs-shared.sh
	@test -f $(DESKTOP_SYSROOT)/usr/lib/libz.so || { echo "shared zlib was not installed into the desktop sysroot" >&2; exit 1; }
	@test -f $(DESKTOP_SYSROOT)/usr/lib/libpng16.so || { echo "shared libpng was not installed into the desktop sysroot" >&2; exit 1; }
	@test -f $(DESKTOP_SYSROOT)/usr/lib/libjpeg.so || { echo "shared libjpeg was not installed into the desktop sysroot" >&2; exit 1; }
	@test -f $(DESKTOP_SYSROOT)/usr/lib/libturbojpeg.so || { echo "shared TurboJPEG was not installed into the desktop sysroot" >&2; exit 1; }
	@test -x $(IMAGE_CODECS_SHARED_ROOT)/usr/bin/shared-image-codecs-check || { echo "shared image codec checks were not produced" >&2; exit 1; }
	@touch $@

$(MUSL_CROSS_STAMP): ports/build-musl-cross.sh ports/lib/kernel-headers.sh \
	ports/src/musl-cross-make/Makefile
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" bash ports/build-musl-cross.sh
	@test -x $(MUSL_CROSS)/bin/x86_64-linux-musl-g++ || { echo "the musl cross toolchain was not produced" >&2; exit 1; }
	@touch $@

$(LIBFFI_STAMP): $(MUSL_CROSS_STAMP) ports/build-libffi.sh ports/lib/cross-port.sh \
	ports/src/libffi/configure.ac
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" bash ports/build-libffi.sh
	@test -L $(LIBFFI_ROOT)/usr/lib/libffi.so.8 || { echo "libffi was not produced" >&2; exit 1; }
	@touch $@

# libwayland is the protocol library every compositor and client is built
# against; it needs libffi for its message dispatch.
$(WAYLAND_STAMP): $(LIBFFI_STAMP) ports/build-wayland.sh ports/lib/cross-port.sh \
	tools/wayland-roundtrip-test.c ports/src/wayland/meson.build
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" bash ports/build-wayland.sh
	@test -L $(WAYLAND_ROOT)/usr/lib/libwayland-server.so.0 || { echo "libwayland-server was not produced" >&2; exit 1; }
	@test -L $(WAYLAND_ROOT)/usr/lib/libwayland-client.so.0 || { echo "libwayland-client was not produced" >&2; exit 1; }
	@test -x $(WAYLAND_ROOT)/usr/bin/wayland-roundtrip-test || { echo "the wayland roundtrip test was not produced" >&2; exit 1; }
	@touch $@

# pixman is weston's software renderer; libxkbcommon is its keyboard layer.
# Neither needs a display, which is what makes a headless bring-up possible.
$(PIXMAN_STAMP): $(MUSL_CROSS_STAMP) ports/build-pixman.sh ports/lib/cross-port.sh \
	ports/src/pixman/meson.build
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" bash ports/build-pixman.sh
	@test -L $(PIXMAN_ROOT)/usr/lib/libpixman-1.so.0 || { echo "pixman was not produced" >&2; exit 1; }
	@touch $@

$(LIBXKBCOMMON_STAMP): $(MUSL_CROSS_STAMP) $(LIBXML2_STAMP) ports/build-libxkbcommon.sh \
	ports/lib/cross-port.sh tools/xkb-test.c ports/src/libxkbcommon/meson.build
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" bash ports/build-libxkbcommon.sh
	@test -L $(LIBXKBCOMMON_ROOT)/usr/lib/libxkbcommon.so.0 || { echo "libxkbcommon was not produced" >&2; exit 1; }
	@test -x $(LIBXKBCOMMON_ROOT)/usr/bin/xkb-test || { echo "the xkb test was not produced" >&2; exit 1; }
	@touch $@

# The keyboard database libxkbcommon reads to build a keymap from names.
# Data only -- upstream declares no language, so nothing is compiled.
$(XKEYBOARD_CONFIG_STAMP): ports/build-xkeyboard-config.sh \
	ports/src/xkeyboard-config/meson.build
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" bash ports/build-xkeyboard-config.sh
	@test -f $(XKEYBOARD_CONFIG_ROOT)/usr/share/xkeyboard-config-2/rules/evdev || { echo "the xkb database was not produced" >&2; exit 1; }
	@touch $@

# Weston's remaining dependencies. wayland-protocols is build-time only (XML
# that wayland-scanner turns into C), so it stages nothing into the image.
$(WAYLAND_PROTOCOLS_STAMP): ports/build-wayland-protocols.sh \
	ports/src/wayland-protocols/meson.build
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" bash ports/build-wayland-protocols.sh
	@touch $@

$(LIBEVDEV_STAMP): $(MUSL_CROSS_STAMP) ports/build-libevdev.sh ports/lib/cross-port.sh \
	ports/src/libevdev/meson.build
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" bash ports/build-libevdev.sh
	@test -L $(LIBEVDEV_ROOT)/usr/lib/libevdev.so.2 || { echo "libevdev was not produced" >&2; exit 1; }
	@touch $@

$(ALSA_LIB_STAMP): $(MUSL_CROSS_STAMP) ports/build-alsa-lib.sh ports/lib/cross-port.sh \
	tools/alsa-test.c ports/src/alsa-lib/configure.ac
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" bash ports/build-alsa-lib.sh
	@test -f $(ALSA_LIB_ROOT)/usr/share/alsa/alsa.conf || { echo "the alsa configuration tree was not produced" >&2; exit 1; }
	@test -x $(ALSA_LIB_ROOT)/usr/bin/alsa-test || { echo "alsa-test was not produced" >&2; exit 1; }
	@touch $@

$(LIBUDEV_ZERO_STAMP): $(MUSL_CROSS_STAMP) ports/build-libudev-zero.sh \
	ports/lib/cross-port.sh ports/src/libudev-zero/Makefile
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" bash ports/build-libudev-zero.sh
	@test -f $(LIBUDEV_ZERO_ROOT)/usr/lib/libudev.so.1 || { echo "libudev-zero was not produced" >&2; exit 1; }
	@touch $@

$(LIBINPUT_STAMP): $(LIBEVDEV_STAMP) $(LIBUDEV_ZERO_STAMP) ports/build-libinput.sh \
	ports/lib/cross-port.sh ports/src/libinput/meson.build
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" bash ports/build-libinput.sh
	@test -L $(LIBINPUT_ROOT)/usr/lib/libinput.so.10 || { echo "libinput was not produced" >&2; exit 1; }
	@touch $@

# zlib, libpng, freetype, expat, fontconfig and cairo, plus JetBrains Mono.
# One rule because the chain is strictly ordered: fontconfig cannot be built
# before freetype, and cairo cannot be built before fontconfig.
# XEXT_STAMP: cairo builds its xlib backend (cairo-xlib, for GTK3's x11 backend),
# so the X libraries have to be in the sysroot first.
$(CAIRO_STAMP): $(PIXMAN_STAMP) $(XEXT_STAMP) ports/build-cairo.sh ports/lib/cross-port.sh \
	ports/src/cairo/meson.build ports/src/freetype/meson.build \
	ports/src/fontconfig/meson.build ports/src/libexpat/expat/CMakeLists.txt
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" bash ports/build-cairo.sh
	@test -L $(CAIRO_ROOT)/usr/lib/libcairo.so.2 || { echo "cairo was not produced" >&2; exit 1; }
	@test -f $(CAIRO_ROOT)/usr/lib/libfontconfig.so.1 || { echo "fontconfig was not produced" >&2; exit 1; }
	@test -f $(CAIRO_ROOT)/usr/share/fonts/jetbrains-mono/JetBrainsMono-Regular.ttf || { echo "JetBrains Mono was not installed" >&2; exit 1; }
	@touch $@

# EDID parsing. Nothing on Tunix supplies an EDID, but weston's drm backend
# requires the library unconditionally.
$(LIBDISPLAY_INFO_STAMP): $(MUSL_CROSS_STAMP) ports/build-libdisplay-info.sh \
	ports/lib/cross-port.sh ports/src/libdisplay-info/meson.build
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" bash ports/build-libdisplay-info.sh
	@test -L $(LIBDISPLAY_INFO_ROOT)/usr/lib/libdisplay-info.so.2 || { echo "libdisplay-info was not produced" >&2; exit 1; }
	@touch $@

# The init system. dinit builds with only the cross toolchain (static, no
# libraries), so it does not depend on any other port.
$(DINIT_STAMP): $(MUSL_CROSS_STAMP) ports/build-dinit.sh ports/lib/cross-port.sh \
	ports/src/patches/dinit/0001-control-socket-chmod-enoent.patch \
	ports/src/dinit/configure
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" bash ports/build-dinit.sh
	@test -x $(DINIT_ROOT)/usr/bin/dinit || { echo "dinit was not produced" >&2; exit 1; }
	@touch $@

# Session management. weston 14 has no launcher other than libseat, so the drm
# backend cannot run without it; the builtin backend keeps it in-process.
$(SEATD_STAMP): $(MUSL_CROSS_STAMP) ports/build-seatd.sh ports/lib/cross-port.sh \
	ports/src/seatd/meson.build
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" bash ports/build-seatd.sh
	@test -f $(SEATD_ROOT)/usr/lib/libseat.so.1 || { echo "libseat was not produced" >&2; exit 1; }
	@touch $@

# The compositor. DRM and headless backends with the pixman software renderer,
# so it drives /dev/dri/card0 without needing a GPU.
$(WESTON_STAMP): $(WAYLAND_STAMP) $(WAYLAND_PROTOCOLS_STAMP) $(PIXMAN_STAMP) \
	$(LIBXKBCOMMON_STAMP) $(LIBINPUT_STAMP) $(CAIRO_STAMP) $(LIBDRM_STAMP) \
	$(LIBDISPLAY_INFO_STAMP) $(SEATD_STAMP) $(MESA_STAMP) \
	ports/build-weston.sh ports/lib/cross-port.sh \
	ports/src/patches/weston/0001-shared-make-cairo-optional.patch \
	ports/src/weston/meson.build
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" bash ports/build-weston.sh
	@test -x $(WESTON_ROOT)/usr/bin/weston || { echo "weston was not produced" >&2; exit 1; }
	@test -f $(WESTON_ROOT)/usr/lib/libweston-14/headless-backend.so || { echo "the headless backend was not produced" >&2; exit 1; }
	@test -f $(WESTON_ROOT)/usr/lib/libweston-14/drm-backend.so || { echo "the drm backend was not produced" >&2; exit 1; }
	@test -x $(WESTON_ROOT)/usr/bin/weston-terminal || { echo "weston-terminal was not produced" >&2; exit 1; }
	@test -f $(WESTON_ROOT)/usr/lib/libweston-14/gl-renderer.so || { echo "the gl renderer was not produced" >&2; exit 1; }
	@touch $@

$(ZSTD_STAMP): $(MUSL_CROSS_STAMP) ports/build-zstd.sh ports/lib/cross-port.sh \
	ports/src/zstd/lib/zstd.h
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" bash ports/build-zstd.sh
	@test -f $(ZSTD_ROOT)/usr/lib/libzstd.so.1 || { echo "libzstd was not produced" >&2; exit 1; }
	@touch $@

$(OPENSSL_STAMP): $(MUSL_CROSS_STAMP) ports/build-openssl.sh ports/lib/cross-port.sh \
	ports/src/openssl/Configure
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" bash ports/build-openssl.sh
	@test -f $(OPENSSL_ROOT)/usr/lib/libcrypto.so.3 || { echo "libcrypto was not produced" >&2; exit 1; }
	@test -f $(OPENSSL_ROOT)/usr/lib/libssl.so.3 || { echo "libssl was not produced" >&2; exit 1; }
	@touch $@

# IMAGE_CODECS_SHARED_STAMP: libarchive links the shared zlib from there.
$(LIBARCHIVE_STAMP): $(ZSTD_STAMP) $(OPENSSL_STAMP) $(IMAGE_CODECS_SHARED_STAMP) \
	ports/build-libarchive.sh ports/lib/cross-port.sh \
	ports/src/libarchive/configure.ac
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" bash ports/build-libarchive.sh
	@test -f $(LIBARCHIVE_ROOT)/usr/lib/libarchive.so.13 || { echo "libarchive was not produced" >&2; exit 1; }
	@touch $@

# A tarball port: xbps ships a signing key whose name contains a colon, which
# cannot be checked out on Windows. See ports/build-xbps.sh.
$(XBPS_STAMP): $(LIBARCHIVE_STAMP) ports/build-xbps.sh ports/lib/cross-port.sh
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" bash ports/build-xbps.sh
	@test -x $(XBPS_ROOT)/usr/bin/xbps-install || { echo "xbps-install was not produced" >&2; exit 1; }
	@test -x $(XBPS_ROOT)/usr/bin/xbps-query || { echo "xbps-query was not produced" >&2; exit 1; }
	@touch $@

$(LIBDRM_STAMP): $(MUSL_CROSS_STAMP) ports/build-libdrm.sh ports/lib/cross-port.sh \
	tools/drm-test.c src/include/tunix/framebuffer.h ports/src/libdrm/meson.build
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" bash ports/build-libdrm.sh
	@test -L $(LIBDRM_ROOT)/usr/lib/libdrm.so.2 || { echo "libdrm was not produced" >&2; exit 1; }
	@test -f $(GRAPHICS_SYSROOT)/usr/lib/pkgconfig/libdrm.pc || { echo "libdrm was not installed into the graphics sysroot" >&2; exit 1; }
	@touch $@

# LLVM for llvmpipe. Depends only on the cross toolchain; the host llvm-tblgen
# (same 22.1.8) bootstraps the cross build. Stages libLLVM.so and a host
# llvm-config wrapper the mesa build consumes.
$(LLVM_STAMP): $(MUSL_CROSS_STAMP) ports/build-llvm.sh ports/lib/cross-port.sh \
	ports/src/llvm-project/llvm/CMakeLists.txt
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" bash ports/build-llvm.sh
	@test -f $(LLVM_ROOT)/usr/lib/libLLVM.so || { echo "libLLVM was not produced" >&2; exit 1; }
	@test -x $(PORT_OUT)/llvm-config-wrapper/llvm-config || { echo "the llvm-config wrapper was not produced" >&2; exit 1; }
	@touch $@

# xorgproto: the X11 protocol headers, the base of the Xorg foundation. Data
# only (headers + .pc into the sysroot), so it needs no toolchain and ships
# nothing to the image; the X libraries above it consume it at build time.
$(XORGPROTO_STAMP): ports/build-xorgproto.sh ports/lib/cross-port.sh \
	ports/src/xorgproto/meson.build
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" bash ports/build-xorgproto.sh
	@test -f $(GRAPHICS_SYSROOT)/usr/share/pkgconfig/xproto.pc || { echo "xorgproto was not produced" >&2; exit 1; }
	@touch $@

# The xcb base stack: the first cross-autotools ports (libXau is meson). Needs
# xorgproto's headers and the cross toolchain; stages libXau/libXdmcp/libxcb and
# the per-extension libxcb-*.so for the image.
$(XCB_STAMP): $(XORGPROTO_STAMP) $(MUSL_CROSS_STAMP) ports/build-libxcb.sh \
	ports/lib/cross-port.sh ports/src/libxcb/configure.ac
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" bash ports/build-libxcb.sh
	@test -e $(XCB_ROOT)/usr/lib/libxcb.so.1 || { echo "libxcb was not produced" >&2; exit 1; }
	@touch $@

# libX11: the core Xlib. Cross-autotools on the xcb stack; stages libX11.so.6 +
# libX11-xcb plus the /usr/share/X11 locale data and /usr/lib/X11 databases.
$(LIBX11_STAMP): $(XCB_STAMP) ports/build-libX11.sh ports/lib/cross-port.sh \
	ports/src/libX11/configure.ac
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" bash ports/build-libX11.sh
	@test -e $(LIBX11_ROOT)/usr/lib/libX11.so.6 || { echo "libX11 was not produced" >&2; exit 1; }
	@touch $@

# The X extension libraries, mixed autotools/meson, in dependency order. On
# libX11; stages the dozen libX*.so the X clients (xfwm4, the panel) link.
$(XEXT_STAMP): $(LIBX11_STAMP) ports/build-xext.sh ports/lib/cross-port.sh \
	ports/src/libXext/configure.ac ports/src/libXfixes/meson.build
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" bash ports/build-xext.sh
	@test -e $(XEXT_ROOT)/usr/lib/libXext.so.6 || { echo "the X extension libs were not produced" >&2; exit 1; }
	@touch $@

# The font stack for the Xorg server. font-util (autotools) + encodings/libfontenc
# (meson) + libXfont2 (autotools); links freetype from the cairo chain.
$(FONTSTACK_STAMP): $(XORGPROTO_STAMP) $(CAIRO_STAMP) ports/build-fontstack.sh \
	ports/lib/cross-port.sh ports/src/libXfont2/configure.ac
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" bash ports/build-fontstack.sh
	@test -e $(FONTSTACK_ROOT)/usr/lib/libXfont2.so.2 || { echo "the font stack was not produced" >&2; exit 1; }
	@touch $@

# The Xorg server. Builds libxcvt + libpciaccess (bundled) then the server with
# the modesetting DDX; links libXfont2 (font stack), libXau/libXdmcp (xcb),
# pixman/libdrm/libudev and libgcrypt (in the sysroot). Xorg still needs the
# VT-less bring-up work before it can run -- see the notes in build-xserver.sh.
$(XSERVER_STAMP): $(FONTSTACK_STAMP) $(XCB_STAMP) $(LIBDRM_STAMP) $(LIBUDEV_ZERO_STAMP) \
	ports/build-xserver.sh ports/lib/cross-port.sh ports/src/xserver/meson.build \
	ports/src/libxcvt/meson.build ports/src/libpciaccess/meson.build
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" bash ports/build-xserver.sh
	@test -x $(XSERVER_ROOT)/usr/bin/Xorg || { echo "Xorg was not produced" >&2; exit 1; }
	@test -x $(XSERVER_ROOT)/usr/bin/Xvfb || { echo "Xvfb was not produced" >&2; exit 1; }
	@touch $@

$(MESA_STAMP): $(LIBDRM_STAMP) $(LLVM_STAMP) ports/build-mesa.sh ports/lib/cross-port.sh \
	tools/tunix-gl-demo.c tools/gbm-test.c src/include/tunix/framebuffer.h \
	ports/src/mesa/meson.build
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" bash ports/build-mesa.sh
	@test -L $(MESA_ROOT)/usr/lib/libEGL.so.1 || { echo "mesa libEGL was not produced" >&2; exit 1; }
	@test -L $(MESA_ROOT)/usr/lib/libGLESv2.so.2 || { echo "mesa libGLESv2 was not produced" >&2; exit 1; }
	@test -L $(MESA_ROOT)/usr/lib/libgbm.so.1 || { echo "mesa libgbm was not produced" >&2; exit 1; }
	@test -x $(MESA_ROOT)/usr/bin/tunix-gl-demo || { echo "the GL demo was not produced" >&2; exit 1; }
	@touch $@

# GLib sits on zlib (from the cairo chain) and libffi; pcre2 is built inside
# the same script because GRegex is its only consumer.
$(GLIB_STAMP): $(CAIRO_STAMP) $(LIBFFI_STAMP) ports/build-glib.sh \
	ports/lib/cross-port.sh ports/src/glib/meson.build ports/src/pcre2/CMakeLists.txt
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" bash ports/build-glib.sh
	@test -e $(GLIB_ROOT)/usr/lib/libglib-2.0.so.0 || { echo "glib was not produced" >&2; exit 1; }
	@test -e $(GLIB_ROOT)/usr/lib/libgio-2.0.so.0 || { echo "gio was not produced" >&2; exit 1; }
	@touch $@

# Text shaping for GTK: fribidi, harfbuzz and pango in one strictly ordered
# chain, mirroring the cairo script's structure.
$(PANGO_STAMP): $(GLIB_STAMP) $(CAIRO_STAMP) $(ICU_STAMP) ports/build-pango.sh \
	ports/lib/cross-port.sh ports/src/pango/meson.build \
	ports/src/patches/pango/0001-pangofc-fontmap-include-fcfreetype.patch \
	ports/src/harfbuzz/meson.build ports/src/fribidi/meson.build
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" bash ports/build-pango.sh
	@test -e $(PANGO_ROOT)/usr/lib/libpangocairo-1.0.so.0 || { echo "pango was not produced" >&2; exit 1; }
	@test -e $(PANGO_ROOT)/usr/lib/libharfbuzz.so.0 || { echo "harfbuzz was not produced" >&2; exit 1; }
	@touch $@

# gdk-pixbuf with builtin loaders, plus the shared libjpeg it decodes with.
$(GDK_PIXBUF_STAMP): $(GLIB_STAMP) ports/build-gdk-pixbuf.sh \
	ports/lib/cross-port.sh ports/src/gdk-pixbuf/meson.build \
	ports/src/libjpeg-turbo/CMakeLists.txt
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" bash ports/build-gdk-pixbuf.sh
	@test -e $(GDK_PIXBUF_ROOT)/usr/lib/libgdk_pixbuf-2.0.so.0 || { echo "gdk-pixbuf was not produced" >&2; exit 1; }
	@touch $@

# GTK3 with both the wayland and x11 backends (the x11 one lets Xfce run under
# Xorg as well as weston). MESA_STAMP is here for libepoxy; XEXT_STAMP for the
# x11 backend's X libraries. One patch makes atk-bridge (AT-SPI/D-Bus) optional.
$(GTK3_STAMP): $(GLIB_STAMP) $(PANGO_STAMP) $(GDK_PIXBUF_STAMP) $(CAIRO_STAMP) \
	$(WAYLAND_STAMP) $(WAYLAND_PROTOCOLS_STAMP) $(LIBXKBCOMMON_STAMP) $(MESA_STAMP) \
	$(XEXT_STAMP) ports/build-gtk3.sh ports/lib/cross-port.sh ports/src/gtk/meson.build \
	ports/src/patches/gtk/0001-make-atk-bridge-optional.patch \
	ports/src/atk/meson.build ports/src/libepoxy/meson.build
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" bash ports/build-gtk3.sh
	@test -e $(GTK3_ROOT)/usr/lib/libgtk-3.so.0 || { echo "gtk3 was not produced" >&2; exit 1; }
	@test -x $(GTK3_ROOT)/usr/bin/gtk3-widget-factory || { echo "gtk3-widget-factory was not produced" >&2; exit 1; }
	@test -f $(GTK3_ROOT)/usr/share/glib-2.0/schemas/gschemas.compiled || { echo "gsettings schemas were not compiled" >&2; exit 1; }
	@touch $@

# libxfce4util: the base Xfce utility library. A pure GLib consumer, so it needs
# only the glib stamp; it stages libxfce4util.so.7 for the image and its headers
# and .pc into the graphics sysroot for the Xfce ports above it.
$(LIBXFCE4UTIL_STAMP): $(GLIB_STAMP) ports/build-libxfce4util.sh ports/lib/cross-port.sh \
	ports/src/libxfce4util/meson.build
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" bash ports/build-libxfce4util.sh
	@test -e $(LIBXFCE4UTIL_ROOT)/usr/lib/libxfce4util.so.7 || { echo "libxfce4util was not produced" >&2; exit 1; }
	@touch $@

# xfconf: the configuration store. Links glib and libxfce4util only (GDBus, not
# libdbus), staging libxfconf plus the xfconfd daemon and xfconf-query.
$(XFCONF_STAMP): $(LIBXFCE4UTIL_STAMP) ports/build-xfconf.sh ports/lib/cross-port.sh \
	ports/src/xfconf/meson.build
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" bash ports/build-xfconf.sh
	@test -e $(XFCONF_ROOT)/usr/lib/libxfconf-0.so.3 || { echo "xfconf was not produced" >&2; exit 1; }
	@touch $@

# libxfce4ui: the Xfce widget library. Links the GTK3 stack, libxfce4util and
# libxfconf; both the x11 and wayland backends, with session-management
# (XfceSMClient, on libSM/libICE) and startup-notification enabled for xfwm4.
$(LIBXFCE4UI_STAMP): $(XFCONF_STAMP) $(GTK3_STAMP) $(LIBSM_STAMP) \
	$(STARTUP_NOTIFICATION_STAMP) $(XEXT_STAMP) ports/build-libxfce4ui.sh \
	ports/lib/cross-port.sh ports/src/libxfce4ui/meson.build
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" bash ports/build-libxfce4ui.sh
	@test -e $(LIBXFCE4UI_ROOT)/usr/lib/libxfce4ui-2.so.0 || { echo "libxfce4ui was not produced" >&2; exit 1; }
	@test -e $(LIBXFCE4UI_ROOT)/usr/lib/libxfce4kbd-private-3.so.0 || { echo "libxfce4kbd-private was not produced" >&2; exit 1; }
	@touch $@

# Thunar: the file manager. Links the whole stack below it; wayland GTK3
# backend, x11 off. One patch drops the case-insensitive Thunar->thunar symlink.
$(THUNAR_STAMP): $(LIBXFCE4UI_STAMP) ports/build-thunar.sh ports/lib/cross-port.sh \
	ports/src/patches/thunar/0001-drop-uppercase-Thunar-symlink.patch \
	ports/src/thunar/meson.build
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" bash ports/build-thunar.sh
	@test -x $(THUNAR_ROOT)/usr/bin/thunar || { echo "thunar was not produced" >&2; exit 1; }
	@test -e $(THUNAR_ROOT)/usr/lib/libthunarx-3.so.0 || { echo "libthunarx was not produced" >&2; exit 1; }
	@touch $@

# xcb-util: xcb-aux/xcb-event/xcb-atom on libxcb, needed by startup-notification.
$(XCB_UTIL_STAMP): $(XCB_STAMP) ports/build-xcb-util.sh ports/lib/cross-port.sh \
	ports/src/xcb-util/configure.ac
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" bash ports/build-xcb-util.sh
	@test -e $(XCB_UTIL_ROOT)/usr/lib/libxcb-util.so.1 || { echo "xcb-util was not produced" >&2; exit 1; }
	@touch $@

# startup-notification: launch feedback on libxcb + xcb-util + x11-xcb.
$(STARTUP_NOTIFICATION_STAMP): $(XCB_UTIL_STAMP) $(LIBX11_STAMP) \
	ports/build-startup-notification.sh ports/lib/cross-port.sh \
	ports/src/startup-notification/configure.in
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" bash ports/build-startup-notification.sh
	@test -e $(STARTUP_NOTIFICATION_ROOT)/usr/lib/libstartup-notification-1.so.0 || { echo "startup-notification was not produced" >&2; exit 1; }
	@touch $@

# libICE + libSM: X11 session management for libxfce4ui's XfceSMClient.
$(LIBSM_STAMP): $(XCB_STAMP) ports/build-libsm.sh ports/lib/cross-port.sh \
	ports/src/libICE/configure.ac ports/src/libSM/configure.ac
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" bash ports/build-libsm.sh
	@test -e $(LIBSM_ROOT)/usr/lib/libSM.so.6 || { echo "libSM was not produced" >&2; exit 1; }
	@touch $@

# libwnck-3: the window-list/pager library, a hard xfwm4 dependency. The first
# GTK3 consumer that needs the x11 backend (GTK3_STAMP) plus startup-notification.
$(LIBWNCK_STAMP): $(GTK3_STAMP) $(STARTUP_NOTIFICATION_STAMP) $(XEXT_STAMP) \
	ports/build-libwnck.sh ports/lib/cross-port.sh ports/src/libwnck/meson.build
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" bash ports/build-libwnck.sh
	@test -e $(LIBWNCK_ROOT)/usr/lib/libwnck-3.so.0 || { echo "libwnck was not produced" >&2; exit 1; }
	@touch $@

# xfwm4: the Xfce window manager. Links libwnck, libxfce4ui and the X libraries;
# the Xrender compositor is on, GLX/epoxy off. Needs a running xfconfd at runtime.
$(XFWM4_STAMP): $(LIBWNCK_STAMP) $(LIBXFCE4UI_STAMP) ports/build-xfwm4.sh \
	ports/lib/cross-port.sh ports/src/xfwm4/meson.build
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" bash ports/build-xfwm4.sh
	@test -x $(XFWM4_ROOT)/usr/bin/xfwm4 || { echo "xfwm4 was not produced" >&2; exit 1; }
	@touch $@

# D-Bus: libdbus-1 + dbus-daemon + tools, on expat (cairo chain) and pthreads.
# The message bus for the Xfce session; xfconfd and the components activate over
# it. systemd/selinux/apparmor/x11-autolaunch/docs/tests all off.
$(DBUS_STAMP): $(CAIRO_STAMP) ports/build-dbus.sh ports/lib/cross-port.sh \
	ports/src/dbus/meson.build
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" bash ports/build-dbus.sh
	@test -e $(DBUS_ROOT)/usr/lib/libdbus-1.so.3 || { echo "libdbus was not produced" >&2; exit 1; }
	@test -x $(DBUS_ROOT)/usr/bin/dbus-daemon || { echo "dbus-daemon was not produced" >&2; exit 1; }
	@touch $@

# garcon: the freedesktop menu library, on the GTK3 + libxfce4ui stack.
$(GARCON_STAMP): $(LIBXFCE4UI_STAMP) ports/build-garcon.sh ports/lib/cross-port.sh \
	ports/src/garcon/meson.build
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" bash ports/build-garcon.sh
	@test -e $(GARCON_ROOT)/usr/lib/libgarcon-1.so.0 || { echo "garcon was not produced" >&2; exit 1; }
	@touch $@

# libxfce4windowing: Xfce's windowing abstraction (x11 backend); reads monitor
# info through libdisplay-info and tracks windows through libwnck.
$(LIBXFCE4WINDOWING_STAMP): $(GTK3_STAMP) $(LIBWNCK_STAMP) $(LIBDISPLAY_INFO_STAMP) \
	ports/build-libxfce4windowing.sh ports/lib/cross-port.sh \
	ports/src/libxfce4windowing/meson.build
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" bash ports/build-libxfce4windowing.sh
	@test -e $(LIBXFCE4WINDOWING_ROOT)/usr/lib/libxfce4windowing-0.so.0 || { echo "libxfce4windowing was not produced" >&2; exit 1; }
	@touch $@

# xfce4-panel: the panel. Links garcon, libwnck + libxfce4windowing, libxfce4ui.
$(XFCE4_PANEL_STAMP): $(GARCON_STAMP) $(LIBXFCE4WINDOWING_STAMP) \
	ports/build-xfce4-panel.sh ports/lib/cross-port.sh ports/src/xfce4-panel/meson.build
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" bash ports/build-xfce4-panel.sh
	@test -x $(XFCE4_PANEL_ROOT)/usr/bin/xfce4-panel || { echo "xfce4-panel was not produced" >&2; exit 1; }
	@touch $@

# xfce4-session: the session manager (xfce4-session + startxfce4).
$(XFCE4_SESSION_STAMP): $(LIBXFCE4WINDOWING_STAMP) $(LIBSM_STAMP) \
	ports/build-xfce4-session.sh ports/lib/cross-port.sh ports/src/xfce4-session/meson.build
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" bash ports/build-xfce4-session.sh
	@test -x $(XFCE4_SESSION_ROOT)/usr/bin/xfce4-session || { echo "xfce4-session was not produced" >&2; exit 1; }
	@touch $@

# xfdesktop: the desktop manager (wallpaper + menu); links garcon + libxfce4windowing.
$(XFDESKTOP_STAMP): $(GARCON_STAMP) $(LIBXFCE4WINDOWING_STAMP) \
	ports/build-xfdesktop.sh ports/lib/cross-port.sh ports/src/xfdesktop/meson.build
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" bash ports/build-xfdesktop.sh
	@test -x $(XFDESKTOP_ROOT)/usr/bin/xfdesktop || { echo "xfdesktop was not produced" >&2; exit 1; }
	@touch $@

# libxml2 (no python/http/icu) for libxkbcommon's xkbregistry, plus libxslt;
# WebCore parses XML and XSLT with both.
$(LIBXML2_STAMP): $(CAIRO_STAMP) ports/build-libxml2.sh ports/lib/cross-port.sh \
	ports/src/libxml2/meson.build ports/src/libxslt/CMakeLists.txt
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" bash ports/build-libxml2.sh
	@test -e $(LIBXML2_ROOT)/usr/lib/libxml2.so.16 || { echo "libxml2 was not produced" >&2; exit 1; }
	@test -e $(LIBXML2_ROOT)/usr/lib/libxslt.so.1 || { echo "libxslt was not produced" >&2; exit 1; }
	@touch $@

# --- the WebKit stack ----------------------------------------------------
# ICU cross-builds in two passes (host tools first); mesa is in the closure
# list only because it ships libstdc++.
$(ICU_STAMP): $(MUSL_CROSS_STAMP) $(MESA_STAMP) ports/build-icu.sh \
	ports/lib/cross-port.sh ports/src/icu/icu4c/source/configure
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" bash ports/build-icu.sh
	@test -e $(ICU_ROOT)/usr/lib/libicuuc.so.77 || { echo "icu was not produced" >&2; exit 1; }
	@touch $@

$(SQLITE_STAMP): $(MUSL_CROSS_STAMP) $(MUSL_SHARED_STAMP) ports/build-sqlite.sh \
	ports/lib/cross-port.sh ports/src/sqlite/configure
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" bash ports/build-sqlite.sh
	@test -x $(SQLITE_ROOT)/usr/bin/sqlite3 || { echo "sqlite was not produced" >&2; exit 1; }
	@touch $@

$(LIBWEBP_STAMP): $(MUSL_CROSS_STAMP) $(MUSL_SHARED_STAMP) ports/build-libwebp.sh \
	ports/lib/cross-port.sh ports/src/libwebp/CMakeLists.txt
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" bash ports/build-libwebp.sh
	@test -e $(LIBWEBP_ROOT)/usr/lib/libwebpdemux.so.2 || { echo "libwebp was not produced" >&2; exit 1; }
	@touch $@

$(WOFF2_STAMP): $(MUSL_CROSS_STAMP) $(MUSL_SHARED_STAMP) $(MESA_STAMP) \
	ports/build-woff2.sh ports/lib/cross-port.sh \
	ports/src/patches/woff2/0001-output-h-include-cstdint.patch \
	ports/src/brotli/CMakeLists.txt ports/src/woff2/CMakeLists.txt
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" bash ports/build-woff2.sh
	@test -e $(WOFF2_ROOT)/usr/lib/libbrotlidec.so.1 || { echo "brotli was not produced" >&2; exit 1; }
	@test -e $(WOFF2_ROOT)/usr/lib/libwoff2dec.so.1.0.2 || { echo "woff2 was not produced" >&2; exit 1; }
	@touch $@

$(LIBGCRYPT_STAMP): $(MUSL_CROSS_STAMP) $(MUSL_SHARED_STAMP) ports/build-libgcrypt.sh \
	ports/lib/cross-port.sh ports/src/libgpg-error/configure.ac \
	ports/src/libgcrypt/configure.ac
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" bash ports/build-libgcrypt.sh
	@test -e $(LIBGCRYPT_ROOT)/usr/lib/libgcrypt.so.20 || { echo "libgcrypt was not produced" >&2; exit 1; }
	@touch $@

$(LIBTASN1_STAMP): $(MUSL_CROSS_STAMP) $(MUSL_SHARED_STAMP) ports/build-libtasn1.sh \
	ports/lib/cross-port.sh ports/src/libtasn1/configure.ac
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" bash ports/build-libtasn1.sh
	@test -e $(LIBTASN1_ROOT)/usr/lib/libtasn1.so.6 || { echo "libtasn1 was not produced" >&2; exit 1; }
	@touch $@

$(GMP_STAMP): $(MUSL_CROSS_STAMP) $(MUSL_SHARED_STAMP) ports/build-gmp.sh \
	ports/lib/cross-port.sh
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" bash ports/build-gmp.sh
	@test -e $(GMP_ROOT)/usr/lib/libgmp.so.10 || { echo "gmp was not produced" >&2; exit 1; }
	@touch $@

$(NETTLE_STAMP): $(GMP_STAMP) ports/build-nettle.sh ports/lib/cross-port.sh \
	ports/src/nettle/configure.ac
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" bash ports/build-nettle.sh
	@test -e $(NETTLE_ROOT)/usr/lib/libnettle.so.8 || { echo "nettle was not produced" >&2; exit 1; }
	@test -e $(NETTLE_ROOT)/usr/lib/libhogweed.so.6 || { echo "hogweed was not produced" >&2; exit 1; }
	@touch $@

$(GNUTLS_STAMP): $(NETTLE_STAMP) $(LIBTASN1_STAMP) ports/build-gnutls.sh \
	ports/lib/cross-port.sh
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" bash ports/build-gnutls.sh
	@test -e $(GNUTLS_ROOT)/usr/lib/libgnutls.so.30 || { echo "gnutls was not produced" >&2; exit 1; }
	@touch $@

# The module is what makes gnutls reachable: gio dlopens it, it registers the
# TLS backend, and only then does https:// work anywhere on the image.
$(GLIB_NETWORKING_STAMP): $(GNUTLS_STAMP) $(GLIB_STAMP) ports/build-glib-networking.sh \
	ports/lib/cross-port.sh ports/src/glib-networking/meson.build tools/gio-tls-test.c
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" bash ports/build-glib-networking.sh
	@test -e $(GLIB_NETWORKING_ROOT)/usr/lib/gio/modules/libgiognutls.so || { echo "the gio tls module was not produced" >&2; exit 1; }
	@touch $@

$(LIBSOUP_STAMP): $(GLIB_STAMP) $(SQLITE_STAMP) $(LIBXML2_STAMP) $(WOFF2_STAMP) \
	$(GLIB_NETWORKING_STAMP) \
	ports/build-libsoup.sh ports/lib/cross-port.sh \
	ports/src/libpsl/meson.build ports/src/libsoup/meson.build
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" bash ports/build-libsoup.sh
	@test -e $(LIBSOUP_ROOT)/usr/lib/libsoup-2.4.so.1 || { echo "libsoup was not produced" >&2; exit 1; }
	@touch $@

# The engine. Compiles on native ext4 (rsynced there) because a few thousand
# C++ TUs over drvfs is not a build, it is an archaeology dig.
$(WEBKITGTK_STAMP): $(GTK3_STAMP) $(ICU_STAMP) $(LIBXML2_STAMP) $(SQLITE_STAMP) \
	$(LIBWEBP_STAMP) $(WOFF2_STAMP) $(LIBGCRYPT_STAMP) $(LIBTASN1_STAMP) \
	$(LIBSOUP_STAMP) ports/build-webkitgtk.sh ports/lib/cross-port.sh \
	ports/src/webkit/Source/cmake/OptionsGTK.cmake
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" bash ports/build-webkitgtk.sh
	@test -e $(WEBKITGTK_ROOT)/usr/lib/libwebkit2gtk-4.0.so.37 || { echo "webkitgtk was not produced" >&2; exit 1; }
	@test -x $(WEBKITGTK_ROOT)/usr/libexec/webkit2gtk-4.0/MiniBrowser || { echo "MiniBrowser was not produced" >&2; exit 1; }
	@touch $@

$(SDL2_STAMP): $(LIBX11_STAMP) $(XEXT_STAMP) $(ALSA_LIB_STAMP) $(LIBUDEV_ZERO_STAMP) \
	ports/build-sdl2.sh ports/lib/cross-port.sh tools/sdl2-test.c \
	ports/src/SDL2/configure ports/src/patches/SDL2/0001-alsa-snd_pcm_info_free-returns-void.patch
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" bash ports/build-sdl2.sh
	@test -e $(SDL2_ROOT)/usr/lib/libSDL2-2.0.so.0 || { echo "libSDL2 was not produced" >&2; exit 1; }
	@test -x $(SDL2_ROOT)/usr/bin/sdl2-test || { echo "sdl2-test was not produced" >&2; exit 1; }
	@touch $@

$(SDL2_NET_STAMP): $(SDL2_STAMP) ports/build-sdl2-net.sh ports/lib/cross-port.sh \
	ports/src/SDL2_net/configure
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" bash ports/build-sdl2-net.sh
	@test -e $(SDL2_NET_ROOT)/usr/lib/libSDL2_net-2.0.so.0 || { echo "libSDL2_net was not produced" >&2; exit 1; }
	@touch $@

$(SDL2_MIXER_STAMP): $(SDL2_STAMP) ports/build-sdl2-mixer.sh ports/lib/cross-port.sh \
	ports/src/SDL2_mixer/configure
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" bash ports/build-sdl2-mixer.sh
	@test -e $(SDL2_MIXER_ROOT)/usr/lib/libSDL2_mixer-2.0.so.0 || { echo "libSDL2_mixer was not produced" >&2; exit 1; }
	@touch $@

$(CHOCOLATE_DOOM_STAMP): $(SDL2_STAMP) $(SDL2_MIXER_STAMP) $(SDL2_NET_STAMP) \
	$(IMAGE_CODECS_SHARED_STAMP) ports/build-chocolate-doom.sh ports/lib/cross-port.sh \
	ports/src/chocolate-doom/configure.ac
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" bash ports/build-chocolate-doom.sh
	@test -x $(CHOCOLATE_DOOM_ROOT)/usr/bin/chocolate-doom || { echo "chocolate-doom was not produced" >&2; exit 1; }
	@test -s $(CHOCOLATE_DOOM_ROOT)/usr/share/games/doom/freedoom1.wad || { echo "the IWAD was not staged" >&2; exit 1; }
	@touch $@

# xfce4-settings: the settings daemon + dialogs. Links garcon and the X input
# extensions; xfsettingsd applies the session's appearance from xfconf. Needs
# libxkbregistry, hence LIBXML2_STAMP is pulled in through LIBXKBCOMMON below.
$(XFCE4_SETTINGS_STAMP): $(GARCON_STAMP) $(LIBXKBCOMMON_STAMP) \
	ports/build-xfce4-settings.sh ports/lib/cross-port.sh \
	ports/src/xfce4-settings/meson.build
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" bash ports/build-xfce4-settings.sh
	@test -x $(XFCE4_SETTINGS_ROOT)/usr/bin/xfsettingsd || { echo "xfsettingsd was not produced" >&2; exit 1; }
	@touch $@

# VTE: the terminal widget (C++), on the GTK3 stack + pcre2/fribidi/icu. One
# patch defines W_EXITCODE, a glibc macro musl omits.
$(VTE_STAMP): $(GTK3_STAMP) ports/build-vte.sh ports/lib/cross-port.sh \
	ports/src/vte/meson.build ports/src/patches/vte/0001-define-W_EXITCODE-for-musl.patch
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" bash ports/build-vte.sh
	@test -e $(VTE_ROOT)/usr/lib/libvte-2.91.so.0 || { echo "vte was not produced" >&2; exit 1; }
	@touch $@

# xfce4-terminal: the terminal emulator, on VTE + the Xfce widget stack.
$(XFCE4_TERMINAL_STAMP): $(VTE_STAMP) $(LIBXFCE4UI_STAMP) ports/build-xfce4-terminal.sh \
	ports/lib/cross-port.sh ports/src/xfce4-terminal/meson.build
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" bash ports/build-xfce4-terminal.sh
	@test -x $(XFCE4_TERMINAL_ROOT)/usr/bin/xfce4-terminal || { echo "xfce4-terminal was not produced" >&2; exit 1; }
	@touch $@

# The welcome screen: one GTK3 C file, so the port script compiles it directly.
$(WELCOME_STAMP): $(GTK3_STAMP) ports/build-welcome.sh ports/lib/cross-port.sh \
	ports/src/welcome/tunix-welcome.c
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" bash ports/build-welcome.sh
	@test -x $(WELCOME_ROOT)/usr/bin/tunix-welcome || { echo "tunix-welcome was not produced" >&2; exit 1; }
	@touch $@

# Renders one offscreen frame on the build host, using the target loader. Proves
# the shipped libraries initialise a softpipe context without needing to boot.
gl-check: $(MESA_STAMP)
	$(MUSL_CROSS)/x86_64-linux-musl/lib/libc.so \
		--library-path $(MUSL_CROSS)/x86_64-linux-musl/lib:$(abspath $(MESA_ROOT))/usr/lib:$(abspath $(LIBDRM_ROOT))/usr/lib \
		$(MESA_ROOT)/usr/bin/tunix-gl-demo --probe

shared-image-codecs-check: $(IMAGE_CODECS_SHARED_STAMP)
	$(MUSL_SHARED_ROOT)/lib/ld-musl-x86_64.so.1 \
		--library-path $(MUSL_SHARED_ROOT)/lib:$(IMAGE_CODECS_SHARED_ROOT)/usr/lib:$(DESKTOP_SYSROOT)/usr/lib \
		$(IMAGE_CODECS_SHARED_ROOT)/usr/bin/shared-image-codecs-test

$(TERMINAL_FONT_DATA): $(TERMINAL_FONT_SOURCE) scripts/generate-terminal-font.py | $(BUILD)
	@mkdir -p $(dir $@)
	$(PYTHON) scripts/generate-terminal-font.py $(TERMINAL_FONT_SOURCE) $@ --width 8 --height 18 --size 13

$(BUILD)/.tools:
	@mkdir -p $(BUILD)
	@for tool in $(CC) $(LD) $(NASM) $(STRIP) $(AR) $(PYTHON) bash make tar; do \
		command -v $$tool >/dev/null 2>&1 || { echo "missing required tool: $$tool" >&2; exit 1; }; \
	done
	@touch $@


$(BASH): ports/build-bash.sh | $(BUILD)/.tools
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" bash ports/build-bash.sh

# GNU userland ports. Each stages a self-contained /usr tree into
# $(PORT_OUT)/<name>-root via the shared ports/lib/gnu-port.sh helper. They
# depend on $(BASH) only to serialize the one-time static musl toolchain build.
$(COREUTILS_STAMP): $(BASH) ports/build-coreutils.sh ports/lib/gnu-port.sh | $(BUILD)/.tools
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" bash ports/build-coreutils.sh
	@test -x $(COREUTILS_ROOT)/usr/bin/ls || { echo "coreutils was not produced" >&2; exit 1; }
	@touch $@

$(GREP_STAMP): $(BASH) ports/build-grep.sh ports/lib/gnu-port.sh | $(BUILD)/.tools
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" bash ports/build-grep.sh
	@test -x $(GREP_ROOT)/usr/bin/grep || { echo "grep was not produced" >&2; exit 1; }
	@touch $@

$(SED_STAMP): $(BASH) ports/build-sed.sh ports/lib/gnu-port.sh | $(BUILD)/.tools
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" bash ports/build-sed.sh
	@test -x $(SED_ROOT)/usr/bin/sed || { echo "sed was not produced" >&2; exit 1; }
	@touch $@

$(GAWK_STAMP): $(BASH) ports/build-gawk.sh ports/lib/gnu-port.sh | $(BUILD)/.tools
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" bash ports/build-gawk.sh
	@test -x $(GAWK_ROOT)/usr/bin/gawk || { echo "gawk was not produced" >&2; exit 1; }
	@touch $@

$(FINDUTILS_STAMP): $(BASH) ports/build-findutils.sh ports/lib/gnu-port.sh | $(BUILD)/.tools
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" bash ports/build-findutils.sh
	@test -x $(FINDUTILS_ROOT)/usr/bin/find || { echo "findutils was not produced" >&2; exit 1; }
	@touch $@

$(DIFFUTILS_STAMP): $(BASH) ports/build-diffutils.sh ports/lib/gnu-port.sh | $(BUILD)/.tools
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" bash ports/build-diffutils.sh
	@test -x $(DIFFUTILS_ROOT)/usr/bin/diff || { echo "diffutils was not produced" >&2; exit 1; }
	@touch $@

$(TAR_STAMP): $(BASH) ports/build-tar.sh ports/lib/gnu-port.sh | $(BUILD)/.tools
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" bash ports/build-tar.sh
	@test -x $(TAR_ROOT)/usr/bin/tar || { echo "tar was not produced" >&2; exit 1; }
	@touch $@

$(GZIP_STAMP): $(BASH) ports/build-gzip.sh ports/lib/gnu-port.sh | $(BUILD)/.tools
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" bash ports/build-gzip.sh
	@test -x $(GZIP_ROOT)/usr/bin/gzip || { echo "gzip was not produced" >&2; exit 1; }
	@touch $@

$(GNUMAKE_STAMP): $(BASH) ports/build-make.sh ports/lib/gnu-port.sh | $(BUILD)/.tools
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" bash ports/build-make.sh
	@test -x $(GNUMAKE_ROOT)/usr/bin/make || { echo "make was not produced" >&2; exit 1; }
	@touch $@

$(SHADOW_STAMP): $(BASH) ports/build-shadow.sh ports/lib/gnu-port.sh | $(BUILD)/.tools
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" bash ports/build-shadow.sh
	@test -x $(SHADOW_ROOT)/bin/login || { echo "shadow login was not produced" >&2; exit 1; }
	@touch $@

$(SUDO_STAMP): $(BASH) ports/build-sudo.sh ports/lib/gnu-port.sh | $(BUILD)/.tools
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" bash ports/build-sudo.sh
	@test -x $(SUDO_ROOT)/usr/bin/sudo || { echo "sudo was not produced" >&2; exit 1; }
	@touch $@

$(LINUX_PAM_STAMP): $(MUSL_CROSS_STAMP) ports/build-linux-pam.sh ports/lib/cross-port.sh | $(BUILD)/.tools
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" bash ports/build-linux-pam.sh
	@test -f $(LINUX_PAM_ROOT)/usr/lib/security/pam_unix.so || { echo "pam_unix was not produced" >&2; exit 1; }
	@touch $@

$(LIGHTDM_STAMP): $(LINUX_PAM_STAMP) $(GLIB_STAMP) $(LIBX11_STAMP) $(LIBGCRYPT_STAMP) \
		ports/build-lightdm.sh ports/lib/cross-port.sh \
		$(wildcard ports/src/patches/lightdm/*.patch) | $(BUILD)/.tools
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" bash ports/build-lightdm.sh
	@test -x $(LIGHTDM_ROOT)/usr/sbin/lightdm || { echo "lightdm was not produced" >&2; exit 1; }
	@touch $@

$(LIGHTDM_GREETER_STAMP): $(LIGHTDM_STAMP) $(GTK3_STAMP) ports/build-lightdm-gtk-greeter.sh \
		ports/lib/cross-port.sh | $(BUILD)/.tools
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" bash ports/build-lightdm-gtk-greeter.sh
	@test -x $(LIGHTDM_GREETER_ROOT)/usr/sbin/lightdm-gtk-greeter || { echo "the greeter was not produced" >&2; exit 1; }
	@touch $@

$(CPYTHON_STAMP): $(MUSL_CROSS_STAMP) $(LIBFFI_STAMP) $(SQLITE_STAMP) $(CAIRO_STAMP) \
		ports/build-cpython.sh ports/lib/cross-port.sh | $(BUILD)/.tools
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" bash ports/build-cpython.sh
	@test -x $(CPYTHON_ROOT)/usr/bin/python3.14 || { echo "python was not produced" >&2; exit 1; }
	@touch $@

# iproute2's ip/ss drive the kernel AF_NETLINK/rtnetlink implementation
# (src/kernel/net/netlink.c). Not an autotools port -- its own configure/make.
$(IPROUTE2_STAMP): $(BASH) ports/build-iproute2.sh ports/lib/gnu-port.sh | $(BUILD)/.tools
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" bash ports/build-iproute2.sh
	@test -x $(IPROUTE2_ROOT)/usr/sbin/ip || { echo "iproute2 ip was not produced" >&2; exit 1; }
	@test -x $(IPROUTE2_ROOT)/usr/sbin/ss || { echo "iproute2 ss was not produced" >&2; exit 1; }
	@touch $@

# curl (static, mbedTLS backend): the library gives git its https transport,
# the tool gives the shell one. Depends on the mbedtls stamp for the TLS
# libraries both link against.
$(CURL_STAMP): $(MBEDTLS_STAMP) ports/build-curl.sh ports/lib/gnu-port.sh | $(BUILD)/.tools
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" bash ports/build-curl.sh
	@test -f $(CURL_ROOT)/usr/lib/libcurl.a || { echo "libcurl was not produced" >&2; exit 1; }
	@test -x $(CURL_ROOT)/usr/bin/curl || { echo "the curl tool was not produced" >&2; exit 1; }
	@touch $@

# git drives its own Makefile (its ./configure only feeds the same one), builds
# a private static zlib (its one hard dependency), and links libcurl+mbedTLS for
# https:// -- hence the curl and mbedtls stamp prerequisites.
$(GIT_STAMP): $(BASH) $(CURL_STAMP) $(MBEDTLS_STAMP) ports/build-git.sh ports/lib/gnu-port.sh | $(BUILD)/.tools
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" bash ports/build-git.sh
	@test -x $(GIT_ROOT)/usr/bin/git || { echo "git was not produced" >&2; exit 1; }
	@test -e $(GIT_ROOT)/usr/libexec/git-core/git-remote-https || { echo "git https helper was not produced" >&2; exit 1; }
	@touch $@

$(NCURSES_STAMP): $(BASH) ports/build-ncurses.sh ports/terminfo/tunix.ti | $(BUILD)/.tools
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" bash ports/build-ncurses.sh
	@touch $@

$(NANO): $(NCURSES_STAMP) ports/build-nano.sh | $(BUILD)/.tools
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" bash ports/build-nano.sh

$(TTY_CLOCK): $(NCURSES_STAMP) ports/build-tty-clock.sh | $(BUILD)/.tools
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" bash ports/build-tty-clock.sh

$(TTY_TETRIS): $(BASH) ports/build-tty-tetris.sh | $(BUILD)/.tools
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" bash ports/build-tty-tetris.sh

$(HTOP): $(NCURSES_STAMP) ports/build-htop.sh | $(BUILD)/.tools
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" bash ports/build-htop.sh

# Recipe, not a submodule port: fetches from GitHub, so a cold build needs
# network. The patch dependency rebuilds when a patch changes.
$(FASTFETCH_STAMP): $(BASH) ports/src/recipes/fastfetch.sh $(wildcard ports/src/patches/fastfetch/*.patch) | $(BUILD)/.tools
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" bash ports/src/recipes/fastfetch.sh
	@touch $@

$(LUA_STAMP): $(BASH) ports/build-lua.sh | $(BUILD)/.tools
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" bash ports/build-lua.sh
	@touch $@

$(LUA): $(LUA_STAMP)
	@test -x $@ || { echo "Lua interpreter was not produced" >&2; exit 1; }

$(TCC_STAMP): ports/build-tcc.sh | $(BUILD)/.tools
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" bash ports/build-tcc.sh
	@touch $@

$(BINUTILS_STAMP): ports/build-binutils.sh | $(BUILD)/.tools
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" bash ports/build-binutils.sh
	@test -x $(BINUTILS_ROOT)/usr/bin/as || { echo "binutils assembler was not produced" >&2; exit 1; }
	@test -x $(BINUTILS_ROOT)/usr/bin/ld || { echo "binutils linker was not produced" >&2; exit 1; }
	@touch $@

$(MBEDTLS_STAMP): $(BASH) ports/build-mbedtls.sh tools/https-get.c tools/ssl-helper.c ports/src/mbedtls/CMakeLists.txt | $(BUILD)/.tools
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" bash ports/build-mbedtls.sh
	@touch $@

$(BUILD):
	mkdir -p $@

$(BUILD)/user:
	mkdir -p $@

# The bootloader is a project of its own with its own build system and its own
# tests. What comes back here is the two pieces the image needs; everything
# about how they are made lives over there.
TUNIX_BOOT := tunix-boot
TUNIX_BOOT_BUILD := $(TUNIX_BOOT)/build-bios
TUNIX_BOOT_CROSS := meson/cross/x86_64-bios.txt

.PHONY: bootloader
bootloader:
	@test -f $(TUNIX_BOOT)/meson.build || { \
		echo "$(TUNIX_BOOT) is empty; run: git submodule update --init"; \
		exit 1; }
	@test -d $(TUNIX_BOOT_BUILD) || \
		(cd $(TUNIX_BOOT) && meson setup build-bios --cross-file $(TUNIX_BOOT_CROSS))
	ninja -C $(TUNIX_BOOT_BUILD)

$(BUILD)/stage1.bin: bootloader | $(BUILD)
	cp $(TUNIX_BOOT_BUILD)/src/fw/bios/stage1.bin $@

$(BUILD)/stage2.bin: bootloader | $(BUILD)
	cp $(TUNIX_BOOT_BUILD)/src/fw/bios/stage2.bin $@

$(BUILD)/entry.o: src/kernel/arch/x86_64/entry.S | $(BUILD)
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/tunix_boot.o: src/kernel/arch/x86_64/tunix_boot.c \
	src/kernel/include/boot_manifest.h src/kernel/include/boot_framebuffer.h | $(BUILD)
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/isr.o: src/kernel/arch/x86_64/isr.S | $(BUILD)
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/idle.o: src/kernel/arch/x86_64/idle.S | $(BUILD)
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/trampoline.o: src/kernel/arch/x86_64/trampoline.S | $(BUILD)
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/syscall_entry.o: src/kernel/arch/x86_64/syscall_entry.S | $(BUILD)
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/%.o: src/kernel/%.c | $(BUILD)
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BOOT_CONFIG_STAMP): src/kernel/include/boot_manifest.h scripts/build-image.py scripts/check-boot-config.py | $(BUILD)
	$(PYTHON) scripts/check-boot-config.py src/kernel/include/boot_manifest.h scripts/build-image.py $@

$(BUILD)/main.o: src/kernel/include/input.h src/kernel/include/tty.h src/kernel/include/pic.h \
	src/kernel/include/boot_manifest.h src/kernel/include/ext2.h $(BOOT_CONFIG_STAMP)
$(BUILD)/input.o: src/kernel/include/input.h src/kernel/include/io.h src/kernel/include/tty.h src/include/tunix/input_event.h
$(BUILD)/pic.o: src/kernel/include/pic.h src/kernel/include/io.h
$(BUILD)/timer.o: src/kernel/include/timer.h src/kernel/include/interrupt.h src/kernel/include/process.h src/kernel/include/io.h
$(BUILD)/devfs.o: src/kernel/include/vfs.h src/kernel/include/pty.h src/kernel/include/random.h src/kernel/include/time.h src/kernel/include/ata.h src/kernel/include/klog.h src/kernel/include/input.h src/kernel/include/framebuffer.h src/include/tunix/input_event.h src/include/tunix/framebuffer.h
$(BUILD)/unix_socket.o: src/kernel/include/unix_socket.h src/kernel/include/pipe.h src/kernel/include/file.h
$(BUILD)/eventfd.o: src/kernel/include/eventfd.h
$(BUILD)/timerfd.o: src/kernel/include/timerfd.h src/kernel/include/time.h
$(BUILD)/epoll.o: src/kernel/include/epoll.h src/kernel/include/file.h
$(BUILD)/inotify.o: src/kernel/include/inotify.h src/kernel/include/vfs.h
$(BUILD)/memfd.o: src/kernel/include/memfd.h src/kernel/include/pmm.h src/kernel/include/vmm.h
$(BUILD)/sysvshm.o: src/kernel/include/sysvshm.h src/kernel/include/memfd.h src/kernel/include/file.h src/kernel/include/time.h
$(BUILD)/signalfd.o: src/kernel/include/signalfd.h src/kernel/include/process.h
$(BUILD)/pty.o: src/kernel/include/pty.h src/kernel/include/tty.h src/kernel/include/file.h
$(BUILD)/file.o: src/kernel/include/file.h src/kernel/include/vfs.h src/kernel/include/pty.h src/kernel/include/input.h src/kernel/include/framebuffer.h src/kernel/include/eventfd.h src/kernel/include/timerfd.h src/kernel/include/epoll.h src/kernel/include/inotify.h src/kernel/include/memfd.h src/kernel/include/signalfd.h
$(BUILD)/syscall.o: src/kernel/include/vfs.h src/kernel/include/tty.h src/kernel/include/pty.h src/kernel/include/process.h src/kernel/include/random.h src/kernel/include/time.h src/kernel/include/input.h src/kernel/include/framebuffer.h src/kernel/include/eventfd.h src/kernel/include/timerfd.h src/kernel/include/epoll.h src/kernel/include/inotify.h src/kernel/include/memfd.h src/kernel/include/signalfd.h src/kernel/include/ext2.h
$(BUILD)/terminal_font.o: $(TERMINAL_FONT_DATA) src/kernel/include/terminal_font.h
$(BUILD)/terminal.o: src/kernel/include/terminal_font.h src/kernel/include/terminal.h src/kernel/include/framebuffer.h
$(BUILD)/tty.o: src/kernel/include/input.h src/kernel/include/tty.h src/kernel/include/terminal.h src/include/tunix/keymap.h
$(BUILD)/process.o: src/kernel/include/process.h src/kernel/include/signal.h src/kernel/include/interrupt.h
# struct vfs_node is embedded across the whole kernel; a layout change must
# rebuild every object or stale offsets corrupt the tree at runtime.
# Every kernel header, not just vfs.h. Editing a header the compiler reads but
# make does not know about produces the worst kind of build: one that succeeds
# and runs the previous code, which costs a debugging session to notice.
KERNEL_HEADERS := $(wildcard src/kernel/include/*.h) $(wildcard src/include/tunix/*.h)
$(KERNEL_OBJS): $(KERNEL_HEADERS)
# And so must a change to KERNEL_CFLAGS. Without this a flag that alters code
# generation -- -mgeneral-regs-only, say -- applies only to the objects that
# happen to be rebuilt for other reasons, and the result is a kernel that is
# half compiled one way and half the other, with nothing to show for it.
$(KERNEL_OBJS): Makefile
$(BUILD)/vfs.o: src/kernel/include/vfs.h src/kernel/include/inotify.h
$(BUILD)/ext2.o: src/kernel/include/ext2.h src/kernel/include/ata.h src/kernel/include/vfs.h src/kernel/include/heap.h src/kernel/include/time.h src/kernel/include/random.h src/kernel/include/kstring.h src/kernel/include/build_config.h
$(BUILD)/random.o: src/kernel/include/random.h src/kernel/include/time.h src/kernel/include/spinlock.h
$(BUILD)/time.o: src/kernel/include/time.h src/kernel/include/io.h
$(BUILD)/ata.o: src/kernel/include/ata.h src/kernel/include/io.h src/kernel/include/pci.h
$(BUILD)/kprintf.o: src/kernel/include/klog.h

$(BUILD)/rtl8139.o: src/kernel/net/rtl8139.c | $(BUILD)
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/xhci.o: src/kernel/usb/xhci.c | $(BUILD)
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/sound.o: src/kernel/audio/sound.c | $(BUILD)
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/hda.o: src/kernel/audio/hda.c | $(BUILD)
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/virtio_pci.o: src/kernel/virtio/virtio_pci.c | $(BUILD)
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/virtio_ring.o: src/kernel/virtio/virtio_ring.c | $(BUILD)
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/virtio_gpu.o: src/kernel/virtio/virtio_gpu.c | $(BUILD)
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/acpi.o: src/kernel/acpi.c | $(BUILD)
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/apic.o: src/kernel/apic.c | $(BUILD)
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/net.o: src/kernel/net/net.c | $(BUILD)
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/inet_socket.o: src/kernel/net/inet_socket.c | $(BUILD)
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/netlink.o: src/kernel/net/netlink.c src/kernel/include/net/netlink.h | $(BUILD)
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kprintf.o: src/kernel/lib/kprintf.c | $(BUILD)
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kstring.o: src/kernel/lib/kstring.c | $(BUILD)
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/gdt.o: src/kernel/arch/x86_64/gdt.c | $(BUILD)
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/idt.o: src/kernel/arch/x86_64/idt.c | $(BUILD)
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/isr_handler.o: src/kernel/arch/x86_64/isr_handler.c src/kernel/include/input.h src/kernel/include/interrupt.h src/kernel/include/pic.h src/kernel/include/timer.h | $(BUILD)
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

# The linker script decides the layout and the reserved regions, so a change to
# it has to relink. Without this the old binary survives an edit to it and looks
# like the edit did nothing.
$(KERNEL): $(KERNEL_OBJS) src/kernel/arch/x86_64/linker.ld
	$(CC) $(KERNEL_LDFLAGS) -o $@ $(KERNEL_OBJS)
	$(STRIP) --strip-all $@

$(BUILD)/user/crt0.o: src/libc/crt0.S | $(BUILD)/user
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(BUILD)/user/libc.o: src/libc/libc.c src/libc/include/tunix_libc.h src/include/tunix/keymap.h | $(BUILD)/user
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(BUILD)/user/sigreturn.o: src/libc/sigreturn.S | $(BUILD)/user
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(BUILD)/user/procutil.o: src/userspace/procutil.c src/userspace/procutil.h src/libc/include/tunix_libc.h | $(BUILD)/user
	$(CC) $(USER_CFLAGS) -Isrc/userspace -c $< -o $@

$(BUILD)/user/%.o: src/userspace/%.c src/userspace/procutil.h src/libc/include/tunix_libc.h | $(BUILD)/user
	$(CC) $(USER_CFLAGS) -Isrc/userspace -c $< -o $@

$(BUILD)/user/loadkeys.o $(BUILD)/user/loadkeys_parser.o: src/userspace/loadkeys_parser.h src/include/tunix/keymap.h
$(BUILD)/user/input_test.o: src/include/tunix/input_event.h
$(BUILD)/user/fb_test.o: src/include/tunix/input_event.h src/include/tunix/framebuffer.h
$(BUILD)/user/glib_compat_test.o: src/include/tunix/glib_compat.h

$(BUILD)/user/ps $(BUILD)/user/free $(BUILD)/user/uptime $(BUILD)/user/top: $(BUILD)/user/%: $(BUILD)/user/%.o $(PROCUTIL) $(USER_RUNTIME) src/userspace/linker.ld
	$(LD) $(USER_LDFLAGS) -o $@ $(USER_RUNTIME) $(PROCUTIL) $(BUILD)/user/$*.o
	$(STRIP) --strip-all $@

$(LOADKEYS): $(BUILD)/user/loadkeys.o $(BUILD)/user/loadkeys_parser.o $(USER_RUNTIME) src/userspace/linker.ld
	$(LD) $(USER_LDFLAGS) -o $@ $(USER_RUNTIME) $(BUILD)/user/loadkeys.o $(BUILD)/user/loadkeys_parser.o
	$(STRIP) --strip-all $@

$(SLEEP): $(BUILD)/user/sleep.o $(USER_RUNTIME) src/userspace/linker.ld
	$(LD) $(USER_LDFLAGS) -o $@ $(USER_RUNTIME) $(BUILD)/user/sleep.o
	$(STRIP) --strip-all $@

$(PREEMPT_TEST): $(BUILD)/user/preempt_test.o $(USER_RUNTIME) src/userspace/linker.ld
	$(LD) $(USER_LDFLAGS) -o $@ $(USER_RUNTIME) $(BUILD)/user/preempt_test.o
	$(STRIP) --strip-all $@

$(SMP_TEST): $(BUILD)/user/smp_test.o $(USER_RUNTIME) src/userspace/linker.ld
	$(LD) $(USER_LDFLAGS) -o $@ $(USER_RUNTIME) $(BUILD)/user/smp_test.o
	$(STRIP) --strip-all $@

$(LINK_TEST): $(BUILD)/user/link_test.o $(USER_RUNTIME) src/userspace/linker.ld
	$(LD) $(USER_LDFLAGS) -o $@ $(USER_RUNTIME) $(BUILD)/user/link_test.o
	$(STRIP) --strip-all $@

$(TCP_TEST): $(BUILD)/user/tcp_test.o $(USER_RUNTIME) src/userspace/linker.ld
	$(LD) $(USER_LDFLAGS) -o $@ $(USER_RUNTIME) $(BUILD)/user/tcp_test.o
	$(STRIP) --strip-all $@

$(BUILD)/user/mount:$(BUILD)/user/mount.o $(USER_RUNTIME) src/userspace/linker.ld
	$(LD) $(USER_LDFLAGS) -o $@ $(USER_RUNTIME) $(BUILD)/user/mount.o
	$(STRIP) --strip-all $@

$(BUILD)/user/umount: $(BUILD)/user/umount.o $(USER_RUNTIME) src/userspace/linker.ld
	$(LD) $(USER_LDFLAGS) -o $@ $(USER_RUNTIME) $(BUILD)/user/umount.o
	$(STRIP) --strip-all $@

$(INPUT_TEST): $(BUILD)/user/input_test.o $(USER_RUNTIME) src/userspace/linker.ld
	$(LD) $(USER_LDFLAGS) -o $@ $(USER_RUNTIME) $(BUILD)/user/input_test.o
	$(STRIP) --strip-all $@

$(FB_TEST): $(BUILD)/user/fb_test.o $(USER_RUNTIME) src/userspace/linker.ld
	$(LD) $(USER_LDFLAGS) -o $@ $(USER_RUNTIME) $(BUILD)/user/fb_test.o
	$(STRIP) --strip-all $@

$(FB_SHOT): $(BUILD)/user/fb_shot.o $(USER_RUNTIME) src/userspace/linker.ld
	$(LD) $(USER_LDFLAGS) -o $@ $(USER_RUNTIME) $(BUILD)/user/fb_shot.o
	$(STRIP) --strip-all $@

$(GLIB_COMPAT_TEST): $(BUILD)/user/glib_compat_test.o $(USER_RUNTIME) src/userspace/linker.ld
	$(LD) $(USER_LDFLAGS) -o $@ $(USER_RUNTIME) $(BUILD)/user/glib_compat_test.o
	$(STRIP) --strip-all $@

$(SND_TEST): $(BUILD)/user/snd_test.o $(USER_RUNTIME) src/userspace/linker.ld
	$(LD) $(USER_LDFLAGS) -o $@ $(USER_RUNTIME) $(BUILD)/user/snd_test.o
	$(STRIP) --strip-all $@

$(INITRAMFS): $(DINIT_STAMP) $(SHADOW_STAMP) $(SUDO_STAMP) $(LINUX_PAM_STAMP) $(LIGHTDM_STAMP) $(LIGHTDM_GREETER_STAMP) $(CPYTHON_STAMP) $(SYSTEM_TOOLS) $(BASH) $(GNU_PORT_STAMPS) $(IPROUTE2_STAMP) $(CURL_STAMP) $(GIT_STAMP) $(TCC_STAMP) $(BINUTILS_STAMP) $(NANO) $(TTY_CLOCK) $(TTY_TETRIS) $(HTOP) $(FASTFETCH_STAMP) $(LUA_STAMP) $(IMAGE_CODECS_STAMP) $(MUSL_SHARED_STAMP) $(IMAGE_CODECS_SHARED_STAMP) $(MBEDTLS_STAMP) $(LIBFFI_STAMP) $(WAYLAND_STAMP) $(PIXMAN_STAMP) $(LIBXKBCOMMON_STAMP) $(XKEYBOARD_CONFIG_STAMP) $(LIBEVDEV_STAMP) $(ALSA_LIB_STAMP) $(LIBUDEV_ZERO_STAMP) $(LIBINPUT_STAMP) $(CAIRO_STAMP) $(LIBDISPLAY_INFO_STAMP) $(SEATD_STAMP) $(WESTON_STAMP) $(LIBDRM_STAMP) $(MESA_STAMP) $(LLVM_STAMP) $(GLIB_STAMP) $(PANGO_STAMP) $(GDK_PIXBUF_STAMP) $(GTK3_STAMP) $(LIBXFCE4UTIL_STAMP) $(XFCONF_STAMP) $(LIBXFCE4UI_STAMP) $(THUNAR_STAMP) $(XCB_STAMP) $(LIBX11_STAMP) $(XEXT_STAMP) $(FONTSTACK_STAMP) $(XSERVER_STAMP) $(XCB_UTIL_STAMP) $(STARTUP_NOTIFICATION_STAMP) $(LIBSM_STAMP) $(LIBWNCK_STAMP) $(XFWM4_STAMP) $(DBUS_STAMP) $(GARCON_STAMP) $(LIBXFCE4WINDOWING_STAMP) $(XFCE4_PANEL_STAMP) $(XFCE4_SESSION_STAMP) $(XFDESKTOP_STAMP) $(LIBXML2_STAMP) $(XFCE4_SETTINGS_STAMP) $(VTE_STAMP) $(XFCE4_TERMINAL_STAMP) $(WELCOME_STAMP) $(ICU_STAMP) $(SQLITE_STAMP) $(LIBWEBP_STAMP) $(WOFF2_STAMP) $(LIBGCRYPT_STAMP) $(LIBTASN1_STAMP) $(GMP_STAMP) $(NETTLE_STAMP) $(GNUTLS_STAMP) $(GLIB_NETWORKING_STAMP) $(LIBSOUP_STAMP) $(WEBKITGTK_STAMP) $(SDL2_STAMP) $(SDL2_NET_STAMP) $(SDL2_MIXER_STAMP) $(CHOCOLATE_DOOM_STAMP) $(XBPS_STAMP) $(INITRD_FILES)
	rm -rf $(ROOTFS)
	mkdir -p $(ROOTFS)/bin $(ROOTFS)/sbin $(ROOTFS)/dev $(ROOTFS)/tmp \
		$(ROOTFS)/run/dbus $(ROOTFS)/run/user/0 $(ROOTFS)/run/user/1000 \
		$(ROOTFS)/var/tmp $(ROOTFS)/var/log $(ROOTFS)/var/lib/sudo \
		$(ROOTFS)/var/lib/lightdm $(ROOTFS)/var/cache/lightdm \
		$(ROOTFS)/var/log/lightdm $(ROOTFS)/run/lightdm \
		$(ROOTFS)/root/.config $(ROOTFS)/root/.cache \
		$(ROOTFS)/home/tunix/.config $(ROOTFS)/home/tunix/.cache \
		$(ROOTFS)/home/tunix/.local/share $(ROOTFS)/etc/sudoers.d
	ln -sfn ../run $(ROOTFS)/var/run
	cp -R initrd/. $(ROOTFS)/
	cp -R initrd/etc/skel/. $(ROOTFS)/home/tunix/
	cp -R initrd/etc/skel/. $(ROOTFS)/root/
	cp -R $(DINIT_ROOT)/. $(ROOTFS)/
	ln -sfn ../usr/bin/dinit $(ROOTFS)/sbin/init
	cp -R $(SHADOW_ROOT)/. $(ROOTFS)/
	cp -R $(SUDO_ROOT)/. $(ROOTFS)/
	cp -R $(LINUX_PAM_ROOT)/. $(ROOTFS)/
	cp -R $(LIGHTDM_ROOT)/. $(ROOTFS)/
	cp -R $(LIGHTDM_GREETER_ROOT)/. $(ROOTFS)/
	cp -R $(CPYTHON_ROOT)/. $(ROOTFS)/
	# shadow ships its own login.defs and sudo its own sudoers; both land in
	# /etc after the initrd copy, so the Tunix versions are put back on top.
	cp initrd/etc/login.defs $(ROOTFS)/etc/login.defs
	cp initrd/etc/sudoers $(ROOTFS)/etc/sudoers
	cp $(BASH) $(ROOTFS)/bin/bash
	cp $(NANO) $(ROOTFS)/bin/nano
	cp $(TTY_CLOCK) $(ROOTFS)/bin/tty-clock
	cp $(TTY_TETRIS) $(ROOTFS)/bin/tty-tetris
	cp $(HTOP) $(ROOTFS)/bin/htop
	cp $(SYSTEM_TOOLS) $(ROOTFS)/bin/
	cp -R $(TCC_ROOT)/. $(ROOTFS)/
	cp -R $(BINUTILS_ROOT)/. $(ROOTFS)/
	cp -R $(LUA_ROOT)/. $(ROOTFS)/
	cp -R $(FASTFETCH_ROOT)/. $(ROOTFS)/
	cp -R $(MUSL_SHARED_ROOT)/. $(ROOTFS)/
	for root in $(GNU_PORT_ROOTS); do cp -R $$root/. $(ROOTFS)/; done
	cp -R $(IPROUTE2_ROOT)/. $(ROOTFS)/
	cp -R $(GIT_ROOT)/. $(ROOTFS)/
	mkdir -p $(ROOTFS)/usr/bin $(ROOTFS)/usr/include/tunix $(ROOTFS)/usr/lib $(ROOTFS)/usr/share
	# Only the tool: git is already linked against the archive, so shipping
	# curl-root wholesale would add headers and a static library nothing reads.
	cp $(CURL_ROOT)/usr/bin/curl $(ROOTFS)/usr/bin/curl
	cp src/include/tunix/input_event.h $(ROOTFS)/usr/include/tunix/input_event.h
	cp src/include/tunix/framebuffer.h $(ROOTFS)/usr/include/tunix/framebuffer.h
	cp src/include/tunix/glib_compat.h $(ROOTFS)/usr/include/tunix/glib_compat.h
	cp -R $(IMAGE_CODECS_ROOT)/usr/include/. $(ROOTFS)/usr/include/
	cp -R $(IMAGE_CODECS_ROOT)/usr/lib/. $(ROOTFS)/usr/lib/
	cp -R $(IMAGE_CODECS_SHARED_ROOT)/. $(ROOTFS)/
	cp -R $(LIBFFI_ROOT)/. $(ROOTFS)/
	cp -R $(WAYLAND_ROOT)/. $(ROOTFS)/
	cp -R $(PIXMAN_ROOT)/. $(ROOTFS)/
	cp -R $(LIBXKBCOMMON_ROOT)/. $(ROOTFS)/
	cp -R $(XKEYBOARD_CONFIG_ROOT)/. $(ROOTFS)/
	cp -R $(LIBEVDEV_ROOT)/. $(ROOTFS)/
	cp -R $(ALSA_LIB_ROOT)/. $(ROOTFS)/
	cp -R $(LIBUDEV_ZERO_ROOT)/. $(ROOTFS)/
	cp -R $(LIBINPUT_ROOT)/. $(ROOTFS)/
	cp -R $(CAIRO_ROOT)/. $(ROOTFS)/
	cp -R $(LIBDISPLAY_INFO_ROOT)/. $(ROOTFS)/
	cp -R $(SEATD_ROOT)/. $(ROOTFS)/
	cp -R $(WESTON_ROOT)/. $(ROOTFS)/
	cp -R $(LIBDRM_ROOT)/. $(ROOTFS)/
	cp -R $(MESA_ROOT)/. $(ROOTFS)/
	cp -R $(LLVM_ROOT)/. $(ROOTFS)/
	cp -R $(GLIB_ROOT)/. $(ROOTFS)/
	cp -R $(PANGO_ROOT)/. $(ROOTFS)/
	cp -R $(GDK_PIXBUF_ROOT)/. $(ROOTFS)/
	cp -R $(GTK3_ROOT)/. $(ROOTFS)/
	cp -R $(LIBXFCE4UTIL_ROOT)/. $(ROOTFS)/
	cp -R $(XFCONF_ROOT)/. $(ROOTFS)/
	cp -R $(LIBXFCE4UI_ROOT)/. $(ROOTFS)/
	cp -R $(THUNAR_ROOT)/. $(ROOTFS)/
	cp -R $(XCB_ROOT)/. $(ROOTFS)/
	cp -R $(LIBX11_ROOT)/. $(ROOTFS)/
	cp -R $(XEXT_ROOT)/. $(ROOTFS)/
	cp -R $(FONTSTACK_ROOT)/. $(ROOTFS)/
	cp -R $(XSERVER_ROOT)/. $(ROOTFS)/
	cp -R $(PORT_OUT)/libgcrypt-root/. $(ROOTFS)/
	cp -R $(XCB_UTIL_ROOT)/. $(ROOTFS)/
	cp -R $(STARTUP_NOTIFICATION_ROOT)/. $(ROOTFS)/
	cp -R $(LIBSM_ROOT)/. $(ROOTFS)/
	cp -R $(LIBWNCK_ROOT)/. $(ROOTFS)/
	cp -R $(XFWM4_ROOT)/. $(ROOTFS)/
	cp -R $(DBUS_ROOT)/. $(ROOTFS)/
	cp -R $(GARCON_ROOT)/. $(ROOTFS)/
	cp -R $(LIBXFCE4WINDOWING_ROOT)/. $(ROOTFS)/
	cp -R $(XFCE4_PANEL_ROOT)/. $(ROOTFS)/
	cp -R $(XFCE4_SESSION_ROOT)/. $(ROOTFS)/
	cp -R $(XFDESKTOP_ROOT)/. $(ROOTFS)/
	cp -R $(LIBXML2_ROOT)/. $(ROOTFS)/
	cp -R $(XFCE4_SETTINGS_ROOT)/. $(ROOTFS)/
	cp -R $(ICU_ROOT)/usr/lib/. $(ROOTFS)/usr/lib/
	cp -R $(VTE_ROOT)/. $(ROOTFS)/
	cp -R $(XFCE4_TERMINAL_ROOT)/. $(ROOTFS)/
	cp -R $(WELCOME_ROOT)/. $(ROOTFS)/
	cp -R $(SQLITE_ROOT)/. $(ROOTFS)/
	cp -R $(LIBWEBP_ROOT)/. $(ROOTFS)/
	cp -R $(WOFF2_ROOT)/. $(ROOTFS)/
	cp -R $(LIBGCRYPT_ROOT)/. $(ROOTFS)/
	cp -R $(LIBTASN1_ROOT)/. $(ROOTFS)/
	cp -R $(GMP_ROOT)/. $(ROOTFS)/
	cp -R $(NETTLE_ROOT)/. $(ROOTFS)/
	cp -R $(GNUTLS_ROOT)/. $(ROOTFS)/
	cp -R $(GLIB_NETWORKING_ROOT)/. $(ROOTFS)/
	cp -R $(LIBSOUP_ROOT)/. $(ROOTFS)/
	cp -R $(WEBKITGTK_ROOT)/. $(ROOTFS)/
	cp -R $(SDL2_ROOT)/. $(ROOTFS)/
	cp -R $(SDL2_NET_ROOT)/. $(ROOTFS)/
	cp -R $(SDL2_MIXER_ROOT)/. $(ROOTFS)/
	cp -R $(CHOCOLATE_DOOM_ROOT)/. $(ROOTFS)/
	cp -R $(ZSTD_ROOT)/. $(ROOTFS)/
	cp -R $(OPENSSL_ROOT)/. $(ROOTFS)/
	cp -R $(LIBARCHIVE_ROOT)/. $(ROOTFS)/
	cp -R $(XBPS_ROOT)/. $(ROOTFS)/
	# The shared MIME database, which is how GIO answers g_content_type_guess()
	# and therefore how WebKit decides a file:// URL is html rather than plain
	# text. Only the compiled lookup tables are needed, not the per-type XML the
	# package also ships, so this is 424 KiB rather than 8 MiB. Taken from the
	# build host: the format is architecture independent.
	mkdir -p $(ROOTFS)/usr/share/mime
	for f in mime.cache globs globs2 magic aliases subclasses types icons \
		generic-icons treemagic XMLnamespaces; do \
		cp /usr/share/mime/$$f $(ROOTFS)/usr/share/mime/ 2>/dev/null || true; \
	done
	@test -f $(ROOTFS)/usr/share/mime/mime.cache || { echo "the shared MIME database was not installed into the rootfs (install shared-mime-info on the build host)" >&2; exit 1; }
	cp $(HTTPS_GET) $(ROOTFS)/usr/bin/https-get
	ln -sfn ../usr/bin/https-get $(ROOTFS)/bin/https-get
	cp $(SSL_HELPER) $(ROOTFS)/usr/bin/openssl
	ln -sfn ../usr/bin/openssl $(ROOTFS)/bin/openssl
	cp -R $(IMAGE_CODECS_ROOT)/usr/share/. $(ROOTFS)/usr/share/
	cp -R $(NCURSES_ROOT)/usr/share/terminfo $(ROOTFS)/usr/share/
	# ncurses ships the terminal utilities; reset is a symlink to tset upstream.
	cp $(NCURSES_ROOT)/usr/bin/clear $(ROOTFS)/bin/clear
	cp $(NCURSES_ROOT)/usr/bin/tput $(ROOTFS)/bin/tput
	cp $(NCURSES_ROOT)/usr/bin/tset $(ROOTFS)/bin/tset
	ln -sfn tset $(ROOTFS)/bin/reset
	chmod 0755 $(ROOTFS)/bin/clear $(ROOTFS)/bin/tput $(ROOTFS)/bin/tset
	mkdir -p $(ROOTFS)/usr/share/nano
	cp ports/src/nano/syntax/*.nanorc $(ROOTFS)/usr/share/nano/
	@test -x $(ROOTFS)/usr/bin/tcc || { echo "TinyCC was not installed into the rootfs" >&2; exit 1; }
	@test -x $(ROOTFS)/usr/bin/as || { echo "binutils assembler was not installed into the rootfs" >&2; exit 1; }
	@test -x $(ROOTFS)/usr/bin/ld || { echo "binutils linker was not installed into the rootfs" >&2; exit 1; }
	@test -x $(ROOTFS)/usr/bin/ar || { echo "binutils archiver was not installed into the rootfs" >&2; exit 1; }
	@test -x $(ROOTFS)/usr/bin/lua || { echo "Lua was not installed into the rootfs" >&2; exit 1; }
	@test -x $(ROOTFS)/usr/bin/ls || { echo "coreutils was not installed into the rootfs" >&2; exit 1; }
	@test -x $(ROOTFS)/usr/bin/grep || { echo "grep was not installed into the rootfs" >&2; exit 1; }
	@test -x $(ROOTFS)/usr/bin/sed || { echo "sed was not installed into the rootfs" >&2; exit 1; }
	@test -x $(ROOTFS)/usr/bin/awk || { echo "gawk was not installed into the rootfs" >&2; exit 1; }
	@test -x $(ROOTFS)/usr/bin/find || { echo "findutils was not installed into the rootfs" >&2; exit 1; }
	@test -x $(ROOTFS)/usr/bin/diff || { echo "diffutils was not installed into the rootfs" >&2; exit 1; }
	@test -x $(ROOTFS)/usr/bin/tar || { echo "tar was not installed into the rootfs" >&2; exit 1; }
	@test -x $(ROOTFS)/usr/bin/gzip || { echo "gzip was not installed into the rootfs" >&2; exit 1; }
	@test -x $(ROOTFS)/usr/sbin/ip || { echo "iproute2 ip was not installed into the rootfs" >&2; exit 1; }
	@test -x $(ROOTFS)/usr/sbin/ss || { echo "iproute2 ss was not installed into the rootfs" >&2; exit 1; }
	@test -x $(ROOTFS)/usr/bin/git || { echo "git was not installed into the rootfs" >&2; exit 1; }
	@test -d $(ROOTFS)/usr/share/git-core/templates || { echo "git templates were not installed into the rootfs" >&2; exit 1; }
	@test -e $(ROOTFS)/usr/libexec/git-core/git-remote-https || { echo "git https helper was not installed into the rootfs" >&2; exit 1; }
	@test -x $(ROOTFS)/usr/bin/https-get || { echo "https-get was not installed into the rootfs" >&2; exit 1; }
	@test -x $(ROOTFS)/usr/bin/openssl || { echo "openssl (ssl-helper) was not installed into the rootfs" >&2; exit 1; }
	@test -f $(ROOTFS)/etc/ssl/cert.pem || { echo "TLS CA bundle was not installed into the rootfs" >&2; exit 1; }
	# The same trust in OpenSSL's layout: a directory of certificates found by a
	# hash of the subject. GnuTLS and mbedTLS read the bundle above, but a
	# program that asks OpenSSL for its default cert *directory* -- xbps's
	# bundled libfetch does exactly that -- finds nothing without this.
	$(PYTHON) scripts/split-ca-bundle.py $(ROOTFS)/etc/ssl/cert.pem $(ROOTFS)/etc/ssl/certs
	openssl rehash $(ROOTFS)/etc/ssl/certs
	@test -n "$$(find $(ROOTFS)/etc/ssl/certs -name '*.0' -print -quit)" || { echo "the CA directory has no hash links (is the host openssl too old?)" >&2; exit 1; }
	@test -x $(ROOTFS)/lib/ld-musl-x86_64.so.1 || { echo "shared musl loader was not installed into the rootfs" >&2; exit 1; }
	@test -x $(ROOTFS)/usr/bin/dynamic-runtime-check || { echo "dynamic runtime checks were not installed into the rootfs" >&2; exit 1; }
	@test -x $(ROOTFS)/usr/bin/shm-test || { echo "shared-memory test was not installed into the rootfs" >&2; exit 1; }
	@test -x $(ROOTFS)/usr/bin/signalfd-test || { echo "signalfd test was not installed into the rootfs" >&2; exit 1; }
	@test -x $(ROOTFS)/usr/bin/exec-mem-test || { echo "the executable-memory test was not installed into the rootfs" >&2; exit 1; }
	@test -x $(ROOTFS)/usr/bin/shared-image-codecs-check || { echo "shared image codec checks were not installed into the rootfs" >&2; exit 1; }
	@test -L $(ROOTFS)/usr/lib/libz.so.1 || { echo "shared zlib runtime was not installed into the rootfs" >&2; exit 1; }
	@test -L $(ROOTFS)/usr/lib/libpng16.so.16 || { echo "shared libpng runtime was not installed into the rootfs" >&2; exit 1; }
	@test -L $(ROOTFS)/usr/lib/libjpeg.so.62 || { echo "shared libjpeg runtime was not installed into the rootfs" >&2; exit 1; }
	@test -L $(ROOTFS)/usr/lib/libturbojpeg.so.0 || { echo "shared TurboJPEG runtime was not installed into the rootfs" >&2; exit 1; }
	@test -L $(ROOTFS)/usr/lib/libffi.so.8 || { echo "libffi was not installed into the rootfs" >&2; exit 1; }
	@test -L $(ROOTFS)/usr/lib/libwayland-server.so.0 || { echo "libwayland-server was not installed into the rootfs" >&2; exit 1; }
	@test -L $(ROOTFS)/usr/lib/libwayland-client.so.0 || { echo "libwayland-client was not installed into the rootfs" >&2; exit 1; }
	@test -x $(ROOTFS)/usr/bin/wayland-roundtrip-test || { echo "the wayland roundtrip test was not installed into the rootfs" >&2; exit 1; }
	@test -L $(ROOTFS)/usr/lib/libpixman-1.so.0 || { echo "pixman was not installed into the rootfs" >&2; exit 1; }
	@test -L $(ROOTFS)/usr/lib/libxkbcommon.so.0 || { echo "libxkbcommon was not installed into the rootfs" >&2; exit 1; }
	@test -x $(ROOTFS)/usr/bin/xkb-test || { echo "the xkb test was not installed into the rootfs" >&2; exit 1; }
	@test -f $(ROOTFS)/usr/share/xkeyboard-config-2/rules/evdev || { echo "the xkb database was not installed into the rootfs" >&2; exit 1; }
	@test -L $(ROOTFS)/usr/share/X11/xkb || { echo "the xkb config root symlink is missing from the rootfs" >&2; exit 1; }
	@test -L $(ROOTFS)/usr/lib/libinput.so.10 || { echo "libinput was not installed into the rootfs" >&2; exit 1; }
	@test -L $(ROOTFS)/usr/lib/libcairo.so.2 || { echo "cairo was not installed into the rootfs" >&2; exit 1; }
	@test -x $(ROOTFS)/usr/bin/weston || { echo "weston was not installed into the rootfs" >&2; exit 1; }
	@test -f $(ROOTFS)/usr/lib/libweston-14/headless-backend.so || { echo "the weston headless backend was not installed into the rootfs" >&2; exit 1; }
	@test -f $(ROOTFS)/usr/lib/libweston-14/drm-backend.so || { echo "the weston drm backend was not installed into the rootfs" >&2; exit 1; }
	@test -f $(ROOTFS)/etc/fonts/fonts.conf || { echo "fontconfig's configuration was not installed into the rootfs" >&2; exit 1; }
	@test -f $(ROOTFS)/usr/share/fonts/jetbrains-mono/JetBrainsMono-Regular.ttf || { echo "JetBrains Mono was not installed into the rootfs" >&2; exit 1; }
	@test -f $(ROOTFS)/etc/xdg/weston/weston.ini || { echo "weston.ini was not installed into the rootfs" >&2; exit 1; }
	@test -f $(ROOTFS)/usr/lib/libseat.so.1 || { echo "libseat was not installed into the rootfs" >&2; exit 1; }
	@test -e $(ROOTFS)/usr/lib/libdisplay-info.so.2 || { echo "libdisplay-info was not installed into the rootfs" >&2; exit 1; }
	@test -L $(ROOTFS)/usr/lib/libdrm.so.2 || { echo "libdrm was not installed into the rootfs" >&2; exit 1; }
	@test -x $(ROOTFS)/usr/bin/drm-test || { echo "the drm test was not installed into the rootfs" >&2; exit 1; }
	@test -L $(ROOTFS)/usr/lib/libEGL.so.1 || { echo "mesa libEGL was not installed into the rootfs" >&2; exit 1; }
	@test -L $(ROOTFS)/usr/lib/libGLESv2.so.2 || { echo "mesa libGLESv2 was not installed into the rootfs" >&2; exit 1; }
	@test -L $(ROOTFS)/usr/lib/libgbm.so.1 || { echo "mesa libgbm was not installed into the rootfs" >&2; exit 1; }
	@test -n "$$(ls $(ROOTFS)/usr/lib/libgallium-*.so 2>/dev/null)" || { echo "the gallium megadriver was not installed into the rootfs" >&2; exit 1; }
	@test -f $(ROOTFS)/usr/lib/libLLVM.so || { echo "libLLVM (llvmpipe) was not installed into the rootfs" >&2; exit 1; }
	@test -f $(ROOTFS)/usr/lib/gbm/dri_gbm.so || { echo "the GBM backend was not installed into the rootfs" >&2; exit 1; }
	@test -L $(ROOTFS)/usr/lib/libstdc++.so.6 || { echo "the C++ runtime was not installed into the rootfs" >&2; exit 1; }
	@test -f $(ROOTFS)/usr/lib/libgcc_s.so.1 || { echo "the gcc unwinder was not installed into the rootfs" >&2; exit 1; }
	@test -x $(ROOTFS)/usr/bin/tunix-gl-demo || { echo "tunix-gl-demo was not installed into the rootfs" >&2; exit 1; }
	@test -e $(ROOTFS)/usr/lib/libglib-2.0.so.0 || { echo "glib was not installed into the rootfs" >&2; exit 1; }
	@test -e $(ROOTFS)/usr/lib/libpangocairo-1.0.so.0 || { echo "pango was not installed into the rootfs" >&2; exit 1; }
	@test -e $(ROOTFS)/usr/lib/libgdk_pixbuf-2.0.so.0 || { echo "gdk-pixbuf was not installed into the rootfs" >&2; exit 1; }
	@test -e $(ROOTFS)/usr/lib/libgtk-3.so.0 || { echo "gtk3 was not installed into the rootfs" >&2; exit 1; }
	@test -x $(ROOTFS)/usr/bin/gtk3-widget-factory || { echo "gtk3-widget-factory was not installed into the rootfs" >&2; exit 1; }
	@test -f $(ROOTFS)/usr/share/glib-2.0/schemas/gschemas.compiled || { echo "gsettings schemas were not installed into the rootfs" >&2; exit 1; }
	@test -e $(ROOTFS)/usr/lib/libwebkit2gtk-4.0.so.37 || { echo "webkitgtk was not installed into the rootfs" >&2; exit 1; }
	@test -e $(ROOTFS)/usr/lib/libjavascriptcoregtk-4.0.so.18 || { echo "javascriptcoregtk was not installed into the rootfs" >&2; exit 1; }
	@test -x $(ROOTFS)/usr/libexec/webkit2gtk-4.0/MiniBrowser || { echo "MiniBrowser was not installed into the rootfs" >&2; exit 1; }
	@test -x $(ROOTFS)/usr/libexec/webkit2gtk-4.0/WebKitWebProcess || { echo "WebKitWebProcess was not installed into the rootfs" >&2; exit 1; }
	@test -e $(ROOTFS)/usr/lib/libsoup-2.4.so.1 || { echo "libsoup was not installed into the rootfs" >&2; exit 1; }
	@test -e $(ROOTFS)/usr/lib/libgnutls.so.30 || { echo "gnutls was not installed into the rootfs" >&2; exit 1; }
	@test -e $(ROOTFS)/usr/lib/libhogweed.so.6 || { echo "hogweed was not installed into the rootfs" >&2; exit 1; }
	@test -e $(ROOTFS)/usr/lib/libgmp.so.10 || { echo "gmp was not installed into the rootfs" >&2; exit 1; }
	@test -f $(ROOTFS)/usr/lib/gio/modules/libgiognutls.so || { echo "the gio tls module was not installed into the rootfs; https would not work" >&2; exit 1; }
	@test -x $(ROOTFS)/usr/bin/gio-tls-check || { echo "the gio tls check was not installed into the rootfs" >&2; exit 1; }
	@test -e $(ROOTFS)/usr/lib/libxslt.so.1 || { echo "libxslt was not installed into the rootfs" >&2; exit 1; }
	@test -e $(ROOTFS)/usr/lib/libharfbuzz-icu.so.0 || { echo "harfbuzz-icu was not installed into the rootfs" >&2; exit 1; }
	@test -e $(ROOTFS)/usr/lib/libicuuc.so.77 || { echo "icu was not installed into the rootfs" >&2; exit 1; }
	@test -f $(ROOTFS)/usr/share/tunix/welcome.html || { echo "the browser welcome page was not installed into the rootfs" >&2; exit 1; }
	@test -x $(ROOTFS)/usr/bin/tunix-welcome || { echo "tunix-welcome was not installed into the rootfs" >&2; exit 1; }
	@test -f $(ROOTFS)/usr/share/tunix-welcome/assets/tunix.png || { echo "the tunix-welcome logo was not installed into the rootfs" >&2; exit 1; }
	@test -e $(ROOTFS)/usr/lib/libxfce4util.so.7 || { echo "libxfce4util was not installed into the rootfs" >&2; exit 1; }
	@test -e $(ROOTFS)/usr/lib/libxfconf-0.so.3 || { echo "xfconf was not installed into the rootfs" >&2; exit 1; }
	@test -e $(ROOTFS)/usr/lib/libxfce4ui-2.so.0 || { echo "libxfce4ui was not installed into the rootfs" >&2; exit 1; }
	@test -e $(ROOTFS)/usr/lib/libxfce4kbd-private-3.so.0 || { echo "libxfce4kbd-private was not installed into the rootfs" >&2; exit 1; }
	@test -x $(ROOTFS)/usr/bin/thunar || { echo "thunar was not installed into the rootfs" >&2; exit 1; }
	@test -e $(ROOTFS)/usr/lib/libthunarx-3.so.0 || { echo "libthunarx was not installed into the rootfs" >&2; exit 1; }
	@test -e $(ROOTFS)/usr/lib/libxcb.so.1 || { echo "libxcb was not installed into the rootfs" >&2; exit 1; }
	@test -e $(ROOTFS)/usr/lib/libX11.so.6 || { echo "libX11 was not installed into the rootfs" >&2; exit 1; }
	@test -d $(ROOTFS)/usr/share/X11/locale || { echo "the X11 locale data was not installed into the rootfs" >&2; exit 1; }
	@test -e $(ROOTFS)/usr/lib/libXext.so.6 || { echo "the X extension libs were not installed into the rootfs" >&2; exit 1; }
	@test -e $(ROOTFS)/usr/lib/libXfont2.so.2 || { echo "libXfont2 was not installed into the rootfs" >&2; exit 1; }
	@test -x $(ROOTFS)/usr/bin/Xorg || { echo "Xorg was not installed into the rootfs" >&2; exit 1; }
	@test -x $(ROOTFS)/usr/bin/xkbcomp || { echo "xkbcomp was not installed into the rootfs" >&2; exit 1; }
	@test -f $(ROOTFS)/usr/lib/xorg/modules/drivers/modesetting_drv.so || { echo "the modesetting DDX was not installed into the rootfs" >&2; exit 1; }
	@test -e $(ROOTFS)/usr/lib/libgcrypt.so.20 || { echo "libgcrypt was not installed into the rootfs" >&2; exit 1; }
	@test -x $(ROOTFS)/usr/bin/xfwm4 || { echo "xfwm4 was not installed into the rootfs" >&2; exit 1; }
	@test -x $(ROOTFS)/usr/bin/dbus-daemon || { echo "dbus-daemon was not installed into the rootfs" >&2; exit 1; }
	@test -x $(ROOTFS)/usr/bin/xfce4-panel || { echo "xfce4-panel was not installed into the rootfs" >&2; exit 1; }
	@test -x $(ROOTFS)/usr/bin/xfce4-session || { echo "xfce4-session was not installed into the rootfs" >&2; exit 1; }
	@test -x $(ROOTFS)/usr/bin/xfdesktop || { echo "xfdesktop was not installed into the rootfs" >&2; exit 1; }
	@test -x $(ROOTFS)/usr/bin/xfsettingsd || { echo "xfsettingsd was not installed into the rootfs" >&2; exit 1; }
	@test -x $(ROOTFS)/bin/tunix-session || { echo "the Tunix session launcher was not installed into the rootfs" >&2; exit 1; }
	@test -x $(ROOTFS)/bin/console-login || { echo "the console login wrapper was not installed into the rootfs" >&2; exit 1; }
	@test -x $(ROOTFS)/bin/login || { echo "shadow login was not installed into the rootfs" >&2; exit 1; }
	@test -x $(ROOTFS)/bin/su || { echo "su was not installed into the rootfs" >&2; exit 1; }
	@test -x $(ROOTFS)/usr/bin/passwd || { echo "passwd was not installed into the rootfs" >&2; exit 1; }
	@test -x $(ROOTFS)/usr/sbin/useradd || { echo "the shadow account tools were not installed into the rootfs" >&2; exit 1; }
	@test -x $(ROOTFS)/usr/bin/sudo || { echo "sudo was not installed into the rootfs" >&2; exit 1; }
	@test -x $(ROOTFS)/usr/sbin/visudo || { echo "visudo was not installed into the rootfs" >&2; exit 1; }
	@test -f $(ROOTFS)/etc/shadow || { echo "/etc/shadow is missing from the rootfs" >&2; exit 1; }
	@test -f $(ROOTFS)/etc/sudoers || { echo "/etc/sudoers is missing from the rootfs" >&2; exit 1; }
	@grep -q '^tunix:' $(ROOTFS)/etc/passwd || { echo "the tunix account is missing from /etc/passwd" >&2; exit 1; }
	@test -x $(ROOTFS)/usr/bin/xfce4-terminal || { echo "xfce4-terminal was not installed into the rootfs" >&2; exit 1; }
	@test -e $(ROOTFS)/usr/lib/libvte-2.91.so.0 || { echo "vte was not installed into the rootfs" >&2; exit 1; }
	@test -e $(ROOTFS)/usr/lib/libicuuc.so.77 || { echo "ICU (libvte's Unicode dependency) was not installed into the rootfs" >&2; exit 1; }
	@test -e $(ROOTFS)/usr/share/backgrounds/xfce/xfce-blue.jpg || { echo "the Xfce wallpaper was not installed into the rootfs" >&2; exit 1; }
	@test -e $(ROOTFS)/usr/lib/libgarcon-1.so.0 || { echo "garcon was not installed into the rootfs" >&2; exit 1; }
	@test -e $(ROOTFS)/usr/lib/libwnck-3.so.0 || { echo "libwnck was not installed into the rootfs" >&2; exit 1; }
	@test -e $(ROOTFS)/usr/lib/libSM.so.6 || { echo "libSM was not installed into the rootfs" >&2; exit 1; }
	@test -e $(ROOTFS)/usr/lib/libstartup-notification-1.so.0 || { echo "startup-notification was not installed into the rootfs" >&2; exit 1; }
	ln -sfn ../usr/bin/tcc $(ROOTFS)/bin/tcc
	ln -sfn ../usr/bin/lua $(ROOTFS)/bin/lua
	ln -sfn ../usr/bin/fastfetch $(ROOTFS)/bin/fastfetch
	# coreutils df reads /proc/self/mountinfo, then falls back to /etc/mtab.
	# procfs has no per-process mountinfo, so provide the traditional symlink.
	ln -sfn ../proc/mounts $(ROOTFS)/etc/mtab
	for tool in as ld ar nm ranlib objcopy objdump readelf size strings strip addr2line; do \
		ln -sfn ../usr/bin/$$tool $(ROOTFS)/bin/$$tool; \
	done
	chmod 0755 $(ROOTFS)/usr/bin/dinit $(ROOTFS)/usr/bin/dinitctl \
		$(ROOTFS)/usr/bin/dinit-check $(ROOTFS)/usr/bin/dinit-monitor \
		$(ROOTFS)/sbin/shutdown \
		$(ROOTFS)/etc/rc.d/rcS $(ROOTFS)/etc/rc.d/rc.keymap \
		$(ROOTFS)/bin/bash $(ROOTFS)/bin/nano \
		$(ROOTFS)/bin/tty-clock $(ROOTFS)/bin/tty-tetris $(ROOTFS)/bin/htop \
		$(ROOTFS)/bin/neofetch $(ROOTFS)/bin/startx $(ROOTFS)/bin/tunix-session $(ROOTFS)/bin/console-login $(ROOTFS)/bin/fb-shot $(ROOTFS)/bin/ps $(ROOTFS)/bin/free \
		$(ROOTFS)/bin/uptime $(ROOTFS)/bin/top $(ROOTFS)/bin/loadkeys $(ROOTFS)/bin/sleep $(ROOTFS)/bin/preempt-test $(ROOTFS)/bin/smp-test $(ROOTFS)/bin/input-test $(ROOTFS)/bin/fb-test $(ROOTFS)/bin/glib-compat-test $(ROOTFS)/bin/snd-test $(ROOTFS)/bin/link-test $(ROOTFS)/bin/tcp-test $(ROOTFS)/bin/mount $(ROOTFS)/bin/umount \
		$(ROOTFS)/usr/bin/tcc $(ROOTFS)/usr/bin/lua $(ROOTFS)/usr/bin/fastfetch \
		$(ROOTFS)/usr/bin/browse \
		$(ROOTFS)/usr/bin/as $(ROOTFS)/usr/bin/ld $(ROOTFS)/usr/bin/ar \
		$(ROOTFS)/usr/bin/nm $(ROOTFS)/usr/bin/ranlib $(ROOTFS)/usr/bin/objcopy \
		$(ROOTFS)/usr/bin/objdump $(ROOTFS)/usr/bin/readelf $(ROOTFS)/usr/bin/size \
		$(ROOTFS)/usr/bin/strings $(ROOTFS)/usr/bin/strip $(ROOTFS)/usr/bin/addr2line \
		$(ROOTFS)/usr/bin/dynamic-hello $(ROOTFS)/usr/bin/dynamic-nopie \
		$(ROOTFS)/usr/bin/dlopen-test $(ROOTFS)/usr/bin/pthread-test \
		$(ROOTFS)/usr/bin/dynamic-runtime-check \
		$(ROOTFS)/usr/bin/shared-image-codecs-test $(ROOTFS)/usr/bin/shared-image-codecs-check \
		$(ROOTFS)/lib/ld-musl-x86_64.so.1 \
		$(ROOTFS)/lib/libc.so
	@test -x $(ROOTFS)/bin/tty-clock || { echo "tty-clock was not installed into the rootfs" >&2; exit 1; }
	@test -x $(ROOTFS)/bin/tty-tetris || { echo "tty-tetris was not installed into the rootfs" >&2; exit 1; }
	@test -x $(ROOTFS)/bin/htop || { echo "htop was not installed into the rootfs" >&2; exit 1; }
	@test -x $(ROOTFS)/usr/bin/fastfetch || { echo "fastfetch was not installed into the rootfs" >&2; exit 1; }
	@test -x $(ROOTFS)/bin/clear || { echo "ncurses clear was not installed into the rootfs" >&2; exit 1; }
	@test -x $(ROOTFS)/bin/sleep || { echo "native sleep utility was not installed" >&2; exit 1; }
	@test -x $(ROOTFS)/bin/preempt-test || { echo "scheduler preemption test was not installed" >&2; exit 1; }
	@test -x $(ROOTFS)/bin/smp-test || { echo "multiprocessor test was not installed" >&2; exit 1; }
	@test -x $(ROOTFS)/bin/input-test || { echo "input event test was not installed" >&2; exit 1; }
	@test -x $(ROOTFS)/bin/fb-test || { echo "framebuffer test was not installed" >&2; exit 1; }
	@test -x $(ROOTFS)/bin/glib-compat-test || { echo "GLib compatibility test was not installed" >&2; exit 1; }
	@test -x $(ROOTFS)/bin/snd-test || { echo "sound test was not installed" >&2; exit 1; }
	@test -x $(ROOTFS)/bin/link-test || { echo "hard link test was not installed" >&2; exit 1; }
	@test -x $(ROOTFS)/bin/tcp-test || { echo "loopback/TCP server test was not installed" >&2; exit 1; }
	@test -x $(ROOTFS)/bin/mount ||{ echo "the mount utility was not installed" >&2; exit 1; }
	@test -x $(ROOTFS)/usr/bin/alsa-test || { echo "alsa-test was not installed" >&2; exit 1; }
	@test -f $(ROOTFS)/usr/share/alsa/alsa.conf || { echo "the alsa configuration tree was not installed" >&2; exit 1; }
	@test -L $(ROOTFS)/sbin/init || { echo "/sbin/init is not the dinit symlink" >&2; exit 1; }
	ln -s bash $(ROOTFS)/bin/sh
	# /bin compatibility links. The userland installs into /usr/bin, but a great
	# deal of software spells these paths out: LightDM shells out to /bin/rm and
	# Python's subprocess tests reach for /bin/echo. Only the names that get
	# hardcoded are linked -- this is not a usr-merge.
	@for tool in echo cat ls cp mv rm mkdir rmdir ln chmod chown true false pwd \
	             sleep kill sed grep date uname; do \
		 if [ -x $(ROOTFS)/usr/bin/$$tool ] && [ ! -e $(ROOTFS)/bin/$$tool ]; then \
		         ln -s ../usr/bin/$$tool $(ROOTFS)/bin/$$tool; \
		 fi; \
	 done
	@test -x $(ROOTFS)/bin/echo || { echo "/bin/echo was not linked" >&2; exit 1; }
	tar --format=ustar --blocking-factor=1 --sort=name --mtime=@0 --owner=0 --group=0 --numeric-owner -cf $@ -C $(ROOTFS) .
	# The staging tree lives on a Windows drive, which reports every file as
	# 0777 root:root, so the real modes and owners are stamped onto the archive
	# instead of being carried by it.
	$(PYTHON) scripts/apply-permissions.py $@ scripts/rootfs-permissions.conf

$(IMAGE): $(BUILD)/stage1.bin $(BUILD)/stage2.bin $(KERNEL) $(INITRAMFS) scripts/build-image.py $(BOOT_CONFIG_STAMP)
	$(PYTHON) scripts/build-image.py $@ $(BUILD)/stage1.bin $(BUILD)/stage2.bin $(KERNEL) $(INITRAMFS)
	$(PYTHON) scripts/check-boot-image.py $@ $(INITRAMFS)

# 4 GiB: Tunix keeps file data in RAM, so `git clone` of a real repo
# needs headroom for the pack plus git's own working set, and a browser tab is
# most of a gigabyte on its own. The direct map has its own PML4 entry now, so
# PMM_DIRECT_MAP_LIMIT is 8 GiB and all of what QEMU is given is usable, where
# 2 GiB used to become 1792 MiB. The kernel heap grows from the PMM on demand up
# to HEAP_MAX_SIZE.
# -enable-kvm -cpu host: the whole X11 Xfce desktop is software-rendered
# (llvmpipe + X core), which is unusably slow under plain TCG emulation. KVM runs
# the guest at near-native speed. Falls back to TCG automatically if /dev/kvm is
# absent (accel kvm:tcg).
# -smp: the kernel starts every processor the firmware describes and schedules
# across all of them. Override QEMU_SMP=1 to run the machine as it was before
# there was more than one.
QEMU_SMP ?= 4
run: $(IMAGE)
	rm -f $(BUILD)/serial.log
	$(QEMU) -machine pc,accel=kvm:tcg -cpu host -smp $(QEMU_SMP) -m 4096M -drive format=raw,file=$(IMAGE) \
		-serial file:$(BUILD)/serial.log -monitor none -no-reboot -no-shutdown \
		-netdev user,id=net0 -device rtl8139,netdev=net0 $(QEMU_AUDIO)

# virtio-vga rather than virtio-gpu-pci: the bootloader sets the mode over VBE
# and the text console draws into the VGA framebuffer, both of which only exist
# on the VGA-compatible variant. QEMU shows that framebuffer until the driver
# sets a scanout, and again once it gives one back.
# xres/yres is what the device answers GET_DISPLAY_INFO with, and only until a
# window manager tells QEMU how big the window is -- from then on it answers
# with the window. Nothing in the guest scans out at that size, so this only
# keeps the number coherent when there is no window to override it.
QEMU_GPU_RESOLUTION ?= xres=1280,yres=720
# zoom-to-fit: QEMU opens the window at the VGA adapter's 640x480 and does not
# necessarily grow it when the bootloader sets the real mode, which leaves a
# 1280x720 desktop letterboxed into a small window. This makes the guest fill
# whatever the window is, so maximising it gives a full-size desktop.
QEMU_GPU_DISPLAY ?= gtk,zoom-to-fit=on
run-gpu: $(IMAGE)
	rm -f $(BUILD)/serial.log
	$(QEMU) -machine pc,accel=kvm:tcg -cpu host -smp $(QEMU_SMP) -m 4096M -drive format=raw,file=$(IMAGE) \
		-vga none -device virtio-vga,$(QEMU_GPU_RESOLUTION) -display $(QEMU_GPU_DISPLAY) \
		-serial file:$(BUILD)/serial.log -monitor none -no-reboot -no-shutdown \
		-netdev user,id=net0 -device rtl8139,netdev=net0 $(QEMU_AUDIO)

headless: $(IMAGE)
	$(QEMU) -machine pc,accel=kvm:tcg -cpu host -smp $(QEMU_SMP) -m 4096M -drive format=raw,file=$(IMAGE) \
		-nographic -monitor none -serial stdio -no-reboot -no-shutdown \
		-netdev user,id=net0 -device rtl8139,netdev=net0 $(QEMU_AUDIO)

qemu-ci: $(IMAGE)
	QEMU="$(QEMU)" bash .github/scripts/qemu-ci-smoke.sh $(IMAGE) $(BUILD)/qemu-ci.log


# The ports take hours to build and are rarely the thing being changed. One
# edited build script -- or a drvfs mtime that comes back skewed -- is enough to
# make `make run` decide the whole graph is stale. Stamping every port output
# with a single timestamp is what "already built, leave them alone" looks like
# to make: nothing is newer than anything else, so nothing rebuilds.
.PHONY: ports-uptodate
ports-uptodate:
	@touch $(PORT_OUT)/.timestamp-reference
	@find $(PORT_OUT) -maxdepth 1 \( -type f -o -name '.*-ready' \) \
		-exec touch -r $(PORT_OUT)/.timestamp-reference {} +
	@echo "port outputs marked up to date"

clean:
	rm -rf $(BUILD) $(PORT_OUT)
	@echo "Clean complete."
