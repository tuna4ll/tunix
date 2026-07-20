SHELL := /bin/bash
.DELETE_ON_ERROR:

CC ?= gcc
LD ?= ld
NASM ?= nasm
STRIP ?= strip
AR ?= ar
PYTHON ?= python3
QEMU ?= qemu-system-x86_64

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
LIBDRM_ROOT := $(PORT_OUT)/libdrm-root
LIBDRM_STAMP := $(PORT_OUT)/.libdrm-ready
MESA_ROOT := $(PORT_OUT)/mesa-root
MESA_STAMP := $(PORT_OUT)/.mesa-ready
BOOT_CONFIG_STAMP := $(BUILD)/.boot-config-ready
MUSL_SHARED_STAMP := $(PORT_OUT)/.musl-shared-ready
WALLPAPER_CONVERTER := $(PORT_OUT)/tunix-wallpaper
MBEDTLS_ROOT := $(PORT_OUT)/mbedtls-root
MBEDTLS_STAMP := $(PORT_OUT)/.mbedtls-ready
HTTPS_GET := $(PORT_OUT)/https-get
SSL_HELPER := $(PORT_OUT)/openssl
TERMINAL_FONT_SOURCE ?= assets/fonts/jetbrains-mono/JetBrainsMono-Regular.ttf
TERMINAL_FONT_DATA := $(BUILD)/generated/terminal_font_data.inc

COMMON_CFLAGS := -std=gnu11 -Wall -Wextra -Werror -ffreestanding -fno-stack-protector \
	-fno-pic -fno-pie -fno-builtin -fno-asynchronous-unwind-tables -fno-unwind-tables \
	-mno-red-zone -m64 -Os -ffunction-sections -fdata-sections
KERNEL_CFLAGS := $(COMMON_CFLAGS) -mcmodel=kernel -Isrc/kernel/include -Isrc/include -I$(BUILD)/generated
KERNEL_LDFLAGS := -nostdlib -no-pie -Wl,-T,src/kernel/arch/x86_64/linker.ld \
	-Wl,--gc-sections -Wl,--build-id=none -Wl,-z,max-page-size=0x1000
USER_CFLAGS := $(COMMON_CFLAGS) -mcmodel=small -Isrc/libc/include -Isrc/include
USER_LDFLAGS := -nostdlib -static -T src/userspace/linker.ld --build-id=none \
	-z max-page-size=0x1000 --gc-sections

KERNEL_OBJS := \
	$(BUILD)/entry.o $(BUILD)/main.o $(BUILD)/serial.o \
	$(BUILD)/kprintf.o $(BUILD)/kstring.o $(BUILD)/gdt.o \
	$(BUILD)/idt.o $(BUILD)/isr.o $(BUILD)/isr_handler.o $(BUILD)/pic.o $(BUILD)/timer.o \
	$(BUILD)/pmm.o $(BUILD)/vmm.o $(BUILD)/framebuffer.o $(BUILD)/terminal_font.o $(BUILD)/terminal.o $(BUILD)/input.o \
	$(BUILD)/heap.o $(BUILD)/syscall.o $(BUILD)/syscall_entry.o \
	$(BUILD)/eventfd.o $(BUILD)/timerfd.o $(BUILD)/epoll.o $(BUILD)/inotify.o \
	$(BUILD)/memfd.o \
	$(BUILD)/vfs.o $(BUILD)/tarfs.o $(BUILD)/ext2.o $(BUILD)/devfs.o $(BUILD)/unix_socket.o $(BUILD)/pty.o \
	$(BUILD)/usercopy.o $(BUILD)/elf.o $(BUILD)/file.o \
	$(BUILD)/pipe.o $(BUILD)/tty.o $(BUILD)/process.o $(BUILD)/procfs.o $(BUILD)/time.o $(BUILD)/random.o $(BUILD)/ata.o \
	$(BUILD)/pci.o $(BUILD)/rtl8139.o $(BUILD)/net.o $(BUILD)/inet_socket.o $(BUILD)/netlink.o

USER_RUNTIME := $(BUILD)/user/crt0.o $(BUILD)/user/libc.o $(BUILD)/user/sigreturn.o
INIT := $(BUILD)/user/init
PROCUTIL := $(BUILD)/user/procutil.o
LOADKEYS := $(BUILD)/user/loadkeys
SLEEP := $(BUILD)/user/sleep
PREEMPT_TEST := $(BUILD)/user/preempt-test
INPUT_TEST := $(BUILD)/user/input-test
FB_TEST := $(BUILD)/user/fb-test
GLIB_COMPAT_TEST := $(BUILD)/user/glib-compat-test
SYSTEM_TOOLS := $(BUILD)/user/ps $(BUILD)/user/free $(BUILD)/user/uptime $(BUILD)/user/top $(LOADKEYS) $(SLEEP) $(PREEMPT_TEST) $(INPUT_TEST) $(FB_TEST) $(GLIB_COMPAT_TEST)
INITRD_FILES := $(shell find initrd -type f 2>/dev/null)
WALLPAPER_SOURCE ?= assets/tunix-mountain-lake.jpg
WALLPAPER_OUTPUT := initrd/usr/share/tunix/wallpaper.twl

.PHONY: all run headless qemu-ci wallpaper terminal-font dynamic-runtime-check shared-image-codecs-check gl-check clean
all: $(IMAGE)

wallpaper: $(WALLPAPER_OUTPUT)

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
	tools/dynamic-runtime/shm-test.c
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

$(LIBDRM_STAMP): $(MUSL_CROSS_STAMP) ports/build-libdrm.sh ports/lib/cross-port.sh \
	ports/src/libdrm/meson.build
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" bash ports/build-libdrm.sh
	@test -L $(LIBDRM_ROOT)/usr/lib/libdrm.so.2 || { echo "libdrm was not produced" >&2; exit 1; }
	@test -f $(GRAPHICS_SYSROOT)/usr/lib/pkgconfig/libdrm.pc || { echo "libdrm was not installed into the graphics sysroot" >&2; exit 1; }
	@touch $@

$(MESA_STAMP): $(LIBDRM_STAMP) ports/build-mesa.sh ports/lib/cross-port.sh \
	tools/tunix-gl-demo.c src/include/tunix/framebuffer.h \
	ports/src/mesa/meson.build
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" bash ports/build-mesa.sh
	@test -L $(MESA_ROOT)/usr/lib/libEGL.so.1 || { echo "mesa libEGL was not produced" >&2; exit 1; }
	@test -L $(MESA_ROOT)/usr/lib/libGLESv2.so.2 || { echo "mesa libGLESv2 was not produced" >&2; exit 1; }
	@test -L $(MESA_ROOT)/usr/lib/libgbm.so.1 || { echo "mesa libgbm was not produced" >&2; exit 1; }
	@test -x $(MESA_ROOT)/usr/bin/tunix-gl-demo || { echo "the GL demo was not produced" >&2; exit 1; }
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

$(WALLPAPER_CONVERTER): $(IMAGE_CODECS_STAMP)
	@test -x $@ || { echo "wallpaper converter was not produced" >&2; exit 1; }

$(WALLPAPER_OUTPUT): $(WALLPAPER_SOURCE) $(WALLPAPER_CONVERTER)
	@mkdir -p $(dir $@)
	$(WALLPAPER_CONVERTER) $(WALLPAPER_SOURCE) $(WALLPAPER_OUTPUT) --width 960 --height 540

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

# iproute2's ip/ss drive the kernel AF_NETLINK/rtnetlink implementation
# (src/kernel/net/netlink.c). Not an autotools port -- its own configure/make.
$(IPROUTE2_STAMP): $(BASH) ports/build-iproute2.sh ports/lib/gnu-port.sh | $(BUILD)/.tools
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" bash ports/build-iproute2.sh
	@test -x $(IPROUTE2_ROOT)/usr/sbin/ip || { echo "iproute2 ip was not produced" >&2; exit 1; }
	@test -x $(IPROUTE2_ROOT)/usr/sbin/ss || { echo "iproute2 ss was not produced" >&2; exit 1; }
	@touch $@

# libcurl (static, mbedTLS backend) gives git its https transport. Depends on
# the mbedtls stamp for the TLS libraries it links against.
$(CURL_STAMP): $(MBEDTLS_STAMP) ports/build-curl.sh ports/lib/gnu-port.sh | $(BUILD)/.tools
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" bash ports/build-curl.sh
	@test -f $(CURL_ROOT)/usr/lib/libcurl.a || { echo "libcurl was not produced" >&2; exit 1; }
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

$(BUILD)/stage1.bin: src/bootloader/stage1/boot.asm | $(BUILD) $(BUILD)/.tools
	$(NASM) -f bin $< -o $@

$(BUILD)/stage2.bin: src/bootloader/stage2/stage2.asm | $(BUILD) $(BUILD)/.tools
	$(NASM) -f bin $< -o $@

$(BUILD)/entry.o: src/kernel/arch/x86_64/entry.S | $(BUILD)
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/isr.o: src/kernel/arch/x86_64/isr.S | $(BUILD)
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
$(BUILD)/pty.o: src/kernel/include/pty.h src/kernel/include/tty.h src/kernel/include/file.h
$(BUILD)/file.o: src/kernel/include/file.h src/kernel/include/vfs.h src/kernel/include/pty.h src/kernel/include/input.h src/kernel/include/framebuffer.h src/kernel/include/eventfd.h src/kernel/include/timerfd.h src/kernel/include/epoll.h src/kernel/include/inotify.h src/kernel/include/memfd.h
$(BUILD)/syscall.o: src/kernel/include/vfs.h src/kernel/include/tty.h src/kernel/include/pty.h src/kernel/include/process.h src/kernel/include/random.h src/kernel/include/time.h src/kernel/include/input.h src/kernel/include/framebuffer.h src/kernel/include/eventfd.h src/kernel/include/timerfd.h src/kernel/include/epoll.h src/kernel/include/inotify.h src/kernel/include/memfd.h src/kernel/include/ext2.h
$(BUILD)/terminal_font.o: $(TERMINAL_FONT_DATA) src/kernel/include/terminal_font.h
$(BUILD)/terminal.o: src/kernel/include/terminal_font.h src/kernel/include/terminal.h src/kernel/include/framebuffer.h
$(BUILD)/tty.o: src/kernel/include/input.h src/kernel/include/tty.h src/kernel/include/terminal.h src/include/tunix/keymap.h
$(BUILD)/process.o: src/kernel/include/process.h src/kernel/include/signal.h src/kernel/include/interrupt.h
# struct vfs_node is embedded across the whole kernel; a layout change must
# rebuild every object or stale offsets corrupt the tree at runtime.
$(KERNEL_OBJS): src/kernel/include/vfs.h
$(BUILD)/vfs.o: src/kernel/include/vfs.h src/kernel/include/inotify.h
$(BUILD)/ext2.o: src/kernel/include/ext2.h src/kernel/include/ata.h src/kernel/include/vfs.h src/kernel/include/heap.h src/kernel/include/time.h src/kernel/include/random.h src/kernel/include/kstring.h src/kernel/include/build_config.h
$(BUILD)/random.o: src/kernel/include/random.h src/kernel/include/time.h src/kernel/include/spinlock.h
$(BUILD)/time.o: src/kernel/include/time.h src/kernel/include/io.h
$(BUILD)/ata.o: src/kernel/include/ata.h src/kernel/include/io.h src/kernel/include/pci.h
$(BUILD)/kprintf.o: src/kernel/include/klog.h

$(BUILD)/rtl8139.o: src/kernel/net/rtl8139.c | $(BUILD)
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

$(KERNEL): $(KERNEL_OBJS)
	$(CC) $(KERNEL_LDFLAGS) -o $@ $^
	$(STRIP) --strip-all $@

$(BUILD)/user/crt0.o: src/libc/crt0.S | $(BUILD)/user
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(BUILD)/user/libc.o: src/libc/libc.c src/libc/include/tunix_libc.h src/include/tunix/keymap.h | $(BUILD)/user
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(BUILD)/user/sigreturn.o: src/libc/sigreturn.S | $(BUILD)/user
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(BUILD)/user/init.o: src/userspace/init.c src/libc/include/tunix_libc.h | $(BUILD)/user
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

$(INPUT_TEST): $(BUILD)/user/input_test.o $(USER_RUNTIME) src/userspace/linker.ld
	$(LD) $(USER_LDFLAGS) -o $@ $(USER_RUNTIME) $(BUILD)/user/input_test.o
	$(STRIP) --strip-all $@

$(FB_TEST): $(BUILD)/user/fb_test.o $(USER_RUNTIME) src/userspace/linker.ld
	$(LD) $(USER_LDFLAGS) -o $@ $(USER_RUNTIME) $(BUILD)/user/fb_test.o
	$(STRIP) --strip-all $@

$(GLIB_COMPAT_TEST): $(BUILD)/user/glib_compat_test.o $(USER_RUNTIME) src/userspace/linker.ld
	$(LD) $(USER_LDFLAGS) -o $@ $(USER_RUNTIME) $(BUILD)/user/glib_compat_test.o
	$(STRIP) --strip-all $@

$(INIT): $(BUILD)/user/init.o $(USER_RUNTIME) src/userspace/linker.ld
	$(LD) $(USER_LDFLAGS) -o $@ $(USER_RUNTIME) $(BUILD)/user/init.o
	$(STRIP) --strip-all $@

$(INITRAMFS): $(INIT) $(SYSTEM_TOOLS) $(BASH) $(GNU_PORT_STAMPS) $(IPROUTE2_STAMP) $(GIT_STAMP) $(TCC_STAMP) $(BINUTILS_STAMP) $(NANO) $(TTY_CLOCK) $(TTY_TETRIS) $(HTOP) $(FASTFETCH_STAMP) $(LUA_STAMP) $(IMAGE_CODECS_STAMP) $(MUSL_SHARED_STAMP) $(IMAGE_CODECS_SHARED_STAMP) $(MBEDTLS_STAMP) $(LIBDRM_STAMP) $(MESA_STAMP) $(WALLPAPER_OUTPUT) $(INITRD_FILES)
	rm -rf $(ROOTFS)
	mkdir -p $(ROOTFS)/bin $(ROOTFS)/sbin $(ROOTFS)/dev $(ROOTFS)/tmp \
		$(ROOTFS)/run/dbus $(ROOTFS)/run/user/0 $(ROOTFS)/var/tmp \
		$(ROOTFS)/home/root/.config $(ROOTFS)/home/root/.cache
	chmod 1777 $(ROOTFS)/tmp $(ROOTFS)/var/tmp
	chmod 0700 $(ROOTFS)/run/user/0 $(ROOTFS)/home/root \
		$(ROOTFS)/home/root/.config $(ROOTFS)/home/root/.cache
	ln -sfn ../run $(ROOTFS)/var/run
	cp -R initrd/. $(ROOTFS)/
	cp $(INIT) $(ROOTFS)/sbin/init
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
	cp src/include/tunix/input_event.h $(ROOTFS)/usr/include/tunix/input_event.h
	cp src/include/tunix/framebuffer.h $(ROOTFS)/usr/include/tunix/framebuffer.h
	cp src/include/tunix/glib_compat.h $(ROOTFS)/usr/include/tunix/glib_compat.h
	cp -R $(IMAGE_CODECS_ROOT)/usr/include/. $(ROOTFS)/usr/include/
	cp -R $(IMAGE_CODECS_ROOT)/usr/lib/. $(ROOTFS)/usr/lib/
	cp -R $(IMAGE_CODECS_SHARED_ROOT)/. $(ROOTFS)/
	cp -R $(LIBDRM_ROOT)/. $(ROOTFS)/
	cp -R $(MESA_ROOT)/. $(ROOTFS)/
	cp $(WALLPAPER_CONVERTER) $(ROOTFS)/usr/bin/tunix-wallpaper
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
	@test -x $(ROOTFS)/lib/ld-musl-x86_64.so.1 || { echo "shared musl loader was not installed into the rootfs" >&2; exit 1; }
	@test -x $(ROOTFS)/usr/bin/dynamic-runtime-check || { echo "dynamic runtime checks were not installed into the rootfs" >&2; exit 1; }
	@test -x $(ROOTFS)/usr/bin/shm-test || { echo "shared-memory test was not installed into the rootfs" >&2; exit 1; }
	@test -x $(ROOTFS)/usr/bin/shared-image-codecs-check || { echo "shared image codec checks were not installed into the rootfs" >&2; exit 1; }
	@test -L $(ROOTFS)/usr/lib/libz.so.1 || { echo "shared zlib runtime was not installed into the rootfs" >&2; exit 1; }
	@test -L $(ROOTFS)/usr/lib/libpng16.so.16 || { echo "shared libpng runtime was not installed into the rootfs" >&2; exit 1; }
	@test -L $(ROOTFS)/usr/lib/libjpeg.so.62 || { echo "shared libjpeg runtime was not installed into the rootfs" >&2; exit 1; }
	@test -L $(ROOTFS)/usr/lib/libturbojpeg.so.0 || { echo "shared TurboJPEG runtime was not installed into the rootfs" >&2; exit 1; }
	@test -L $(ROOTFS)/usr/lib/libdrm.so.2 || { echo "libdrm was not installed into the rootfs" >&2; exit 1; }
	@test -L $(ROOTFS)/usr/lib/libEGL.so.1 || { echo "mesa libEGL was not installed into the rootfs" >&2; exit 1; }
	@test -L $(ROOTFS)/usr/lib/libGLESv2.so.2 || { echo "mesa libGLESv2 was not installed into the rootfs" >&2; exit 1; }
	@test -L $(ROOTFS)/usr/lib/libgbm.so.1 || { echo "mesa libgbm was not installed into the rootfs" >&2; exit 1; }
	@test -n "$$(ls $(ROOTFS)/usr/lib/libgallium-*.so 2>/dev/null)" || { echo "the gallium megadriver was not installed into the rootfs" >&2; exit 1; }
	@test -f $(ROOTFS)/usr/lib/gbm/dri_gbm.so || { echo "the GBM backend was not installed into the rootfs" >&2; exit 1; }
	@test -L $(ROOTFS)/usr/lib/libstdc++.so.6 || { echo "the C++ runtime was not installed into the rootfs" >&2; exit 1; }
	@test -f $(ROOTFS)/usr/lib/libgcc_s.so.1 || { echo "the gcc unwinder was not installed into the rootfs" >&2; exit 1; }
	@test -x $(ROOTFS)/usr/bin/tunix-gl-demo || { echo "tunix-gl-demo was not installed into the rootfs" >&2; exit 1; }
	ln -sfn ../usr/bin/tcc $(ROOTFS)/bin/tcc
	ln -sfn ../usr/bin/lua $(ROOTFS)/bin/lua
	ln -sfn ../usr/bin/fastfetch $(ROOTFS)/bin/fastfetch
	# coreutils df reads /proc/self/mountinfo, then falls back to /etc/mtab.
	# procfs has no per-process mountinfo, so provide the traditional symlink.
	ln -sfn ../proc/mounts $(ROOTFS)/etc/mtab
	for tool in as ld ar nm ranlib objcopy objdump readelf size strings strip addr2line; do \
		ln -sfn ../usr/bin/$$tool $(ROOTFS)/bin/$$tool; \
	done
	chmod 0755 $(ROOTFS)/sbin/init $(ROOTFS)/bin/bash $(ROOTFS)/bin/nano \
		$(ROOTFS)/bin/tty-clock $(ROOTFS)/bin/tty-tetris $(ROOTFS)/bin/htop \
		$(ROOTFS)/bin/neofetch $(ROOTFS)/bin/ps $(ROOTFS)/bin/free \
		$(ROOTFS)/bin/uptime $(ROOTFS)/bin/top $(ROOTFS)/bin/loadkeys $(ROOTFS)/bin/sleep $(ROOTFS)/bin/preempt-test $(ROOTFS)/bin/input-test $(ROOTFS)/bin/fb-test $(ROOTFS)/bin/glib-compat-test \
		$(ROOTFS)/usr/bin/tcc $(ROOTFS)/usr/bin/lua $(ROOTFS)/usr/bin/fastfetch \
		$(ROOTFS)/usr/bin/tunix-wallpaper \
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
	@test -x $(ROOTFS)/bin/input-test || { echo "input event test was not installed" >&2; exit 1; }
	@test -x $(ROOTFS)/bin/fb-test || { echo "framebuffer test was not installed" >&2; exit 1; }
	@test -x $(ROOTFS)/bin/glib-compat-test || { echo "GLib compatibility test was not installed" >&2; exit 1; }
	ln -s bash $(ROOTFS)/bin/sh
	tar --format=ustar --blocking-factor=1 --sort=name --mtime=@0 --owner=0 --group=0 --numeric-owner -cf $@ -C $(ROOTFS) .

$(IMAGE): $(BUILD)/stage1.bin $(BUILD)/stage2.bin $(KERNEL) $(INITRAMFS) scripts/build-image.py $(BOOT_CONFIG_STAMP)
	$(PYTHON) scripts/build-image.py $@ $(BUILD)/stage1.bin $(BUILD)/stage2.bin $(KERNEL) $(INITRAMFS)
	$(PYTHON) scripts/check-boot-image.py $@ $(INITRAMFS)

# 2 GiB (was 256M): Tunix keeps file data in RAM, so `git clone` of a real repo
# needs headroom for the pack plus git's own working set. The PMM only manages
# the first PMM_DIRECT_MAP_LIMIT (1 GiB) of physical RAM -- the kernel's direct
# map is exactly 1 GiB -- so at most ~1 GiB is usable and the surplus here is
# ignored; 2 GiB is given only so QEMU surely presents a full contiguous 1 GiB
# below the cap despite low-memory holes. The kernel heap grows from the PMM on
# demand up to HEAP_MAX_SIZE. The CI boot smoke test stays at 256M (clones nothing).
run: $(IMAGE)
	rm -f $(BUILD)/serial.log
	$(QEMU) -machine pc -m 2048M -drive format=raw,file=$(IMAGE) \
		-serial file:$(BUILD)/serial.log -monitor none -no-reboot -no-shutdown \
		-netdev user,id=net0 -device rtl8139,netdev=net0

headless: $(IMAGE)
	$(QEMU) -machine pc -m 2048M -drive format=raw,file=$(IMAGE) \
		-nographic -monitor none -serial stdio -no-reboot -no-shutdown \
		-netdev user,id=net0 -device rtl8139,netdev=net0

qemu-ci: $(IMAGE)
	QEMU="$(QEMU)" bash .github/scripts/qemu-ci-smoke.sh $(IMAGE) $(BUILD)/qemu-ci.log


clean:
	rm -rf $(BUILD) $(PORT_OUT)
	@echo "Clean complete."
