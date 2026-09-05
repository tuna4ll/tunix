# Tunix.
#
# One makefile for the whole system: the kernel, the Void Linux sysroot the
# userland comes from, and the disk image Limine boots.
#
# There is nothing to build from source but the kernel, because everything above
# it is a Void package.

override MAKEFLAGS += -rR

# Settings for this machine rather than for the repository, SYSROOT above all,
# which has to name a filesystem that can hold ownership and the setuid bit.
#
# It is ignored by git and absent by default, which the leading dash is what
# makes fine.
-include local.mk

# -R above clears make's built-in variables after this file is read, so a plain
# `CC ?= cc` would see the built-in, decline to assign, and then be wiped.
#
# Anything the environment or the command line set still wins.
#
# Both origins have to be caught rather than only `default`, because a recursive
# $(MAKE) such as `check` below inherits -R through MAKEFLAGS from the start.
#
# By the time such a sub-make reads this line the built-in is already gone and
# the origin is `undefined`.
#
# Testing only for `default` left CC empty in every sub-make, so the recipe began
# with `-std=gnu11`, which make reads as its own "ignore errors" prefix.
#
# Every compile then failed with "command not found" and every failure was
# ignored.
ifneq ($(filter default undefined,$(origin CC)),)
  override CC := cc
endif

PYTHON  ?= python3
QEMU    ?= qemu-system-x86_64

BUILD   := build
KERNEL  := $(BUILD)/kernel.elf

# Limine is fetched rather than vendored: the header the kernel builds its
# requests from and the bootloader that reads them have to be the same version,
# and taking both from one checkout is the only way to keep that true.
LIMINE_VERSION := v9.x-binary
LIMINE_DIR     := $(BUILD)/limine
LIMINE_HEADER  := $(LIMINE_DIR)/limine.h
LIMINE_EXE     := $(LIMINE_DIR)/limine

# --- the kernel -------------------------------------------------------------

TERMINAL_FONT_SOURCE ?= assets/fonts/jetbrains-mono/JetBrainsMono-Regular.ttf
TERMINAL_FONT_DATA   := $(BUILD)/generated/terminal_font_data.inc

# -mgeneral-regs-only keeps the kernel out of the FPU, SSE and MMX registers
# entirely, because those registers belong to whichever process is running.
#
# The kernel saves them only when it switches processes, so any use in between
# corrupts user state on a plain syscall with no context switch in sight.
#
# GCC will happily reach for %xmm0 to copy a 16-byte struct, which is how that
# happens.
KERNEL_CFLAGS := -std=gnu11 -Wall -Wextra -Werror -ffreestanding \
	-fno-stack-protector -fno-pic -fno-pie -fno-builtin \
	-fno-asynchronous-unwind-tables -fno-unwind-tables -mno-red-zone -m64 \
	-Os -ffunction-sections -fdata-sections -mcmodel=kernel -mgeneral-regs-only \
	-Ikernel/include -I$(BUILD)/generated -I$(LIMINE_DIR) \
	$(KERNEL_CFLAGS_EXTRA)
KERNEL_LDFLAGS := -nostdlib -no-pie -Wl,-T,kernel/arch/x86_64/linker.ld \
	-Wl,--gc-sections -Wl,--build-id=none -Wl,-z,max-page-size=0x1000

KERNEL_SOURCES := $(shell find kernel -name '*.c' -o -name '*.S')
KERNEL_OBJECTS := $(KERNEL_SOURCES:%=$(BUILD)/%.o)
KERNEL_DEPS    := $(KERNEL_OBJECTS:.o=.d)

.PHONY: all kernel check clean distclean
all: image
kernel: $(KERNEL)

# What continuous integration builds, and the reason it is not just `kernel`.
#
# Two of these three configurations are compiled only when asked for, so they rot
# quietly.
#
# TUNIX_DEBUG_LOGS and TUNIX_BOOT_TIMINGS wrap code nothing else refers to, and a
# change that breaks one stays invisible until somebody turns it on to debug
# something, which is the worst moment to find out it no longer compiles.
#
# Each goes in its own directory because the objects differ by flags and nothing
# in the generated dependencies says so.
#
# Limine is passed through rather than left to follow BUILD, so the three do not
# clone it three times.
check:
	$(MAKE) kernel BUILD=$(BUILD)/check/default LIMINE_DIR=$(LIMINE_DIR)
	$(MAKE) kernel BUILD=$(BUILD)/check/debug LIMINE_DIR=$(LIMINE_DIR) KERNEL_CFLAGS_EXTRA=-DTUNIX_DEBUG_LOGS=1
	$(MAKE) kernel BUILD=$(BUILD)/check/timings LIMINE_DIR=$(LIMINE_DIR) KERNEL_CFLAGS_EXTRA=-DTUNIX_BOOT_TIMINGS=1

$(KERNEL): $(KERNEL_OBJECTS) kernel/arch/x86_64/linker.ld
	$(CC) $(KERNEL_LDFLAGS) $(KERNEL_OBJECTS) -o $@

# The generated font is included by terminal_font.c, so it has to exist before
# the first compile rather than only before that one file's.
$(BUILD)/%.c.o: %.c $(TERMINAL_FONT_DATA) | $(LIMINE_HEADER)
	@mkdir -p $(dir $@)
	$(CC) $(KERNEL_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/%.S.o: %.S | $(LIMINE_HEADER)
	@mkdir -p $(dir $@)
	$(CC) $(KERNEL_CFLAGS) -MMD -MP -c $< -o $@

$(TERMINAL_FONT_DATA): $(TERMINAL_FONT_SOURCE) support/terminal-font.py
	@mkdir -p $(dir $@)
	$(PYTHON) support/terminal-font.py $< $@

# A change to the flags has to rebuild everything, and nothing in the generated
# dependencies says so: KERNEL_CFLAGS_EXTRA=-DTUNIX_DEBUG_LOGS=1 would otherwise
# leave a tree of objects half built with it and half without.
$(KERNEL_OBJECTS): GNUmakefile

-include $(KERNEL_DEPS)

# --- limine -----------------------------------------------------------------

$(LIMINE_HEADER):
	rm -rf $(LIMINE_DIR)
	git clone --depth=1 --branch=$(LIMINE_VERSION) \
		https://github.com/limine-bootloader/limine.git $(LIMINE_DIR)

$(LIMINE_EXE): $(LIMINE_HEADER)
	$(MAKE) -C $(LIMINE_DIR)

# What this build makes, without what it downloaded: the kernel takes a minute
# to rebuild and the sysroot is most of a gigabyte over the network.
clean:
	rm -rf $(BUILD)/kernel $(BUILD)/generated $(KERNEL) $(IMAGE)

distclean:
	rm -rf $(BUILD)

# --- the sysroot ------------------------------------------------------------
#
# Void Linux, installed by Void's own package manager, with nothing above the
# kernel built here.

CACHE         ?= $(BUILD)/cache
SYSROOT       ?= $(BUILD)/sysroot
SYSROOT_STAMP := $(BUILD)/.sysroot

VOID_MIRROR      ?= https://repo-default.voidlinux.org
VOID_ROOTFS_DATE ?= 20250202
# The glibc set, not the musl one: the ROOTFS tarball without -musl in its name
# is the glibc build, and every package installed on top of it follows.
VOID_INSTALL ?= base-files bash coreutils util-linux findutils diffutils \
	grep sed gawk tar gzip xz procps-ng psmisc iproute2 iputils file less \
	which ncurses shadow sudo runit runit-void tzdata ca-certificates \
	e2fsprogs kbd nano htop curl fastfetch \
	$(VOID_INSTALL_GRAPHICAL) $(VOID_INSTALL_GAMES)

# The graphical session. weston pulls most of its own world in; what is listed
# beside it is what a compositor needs and nothing depends on: the software
# renderer, the keymaps libxkbcommon compiles, a font for weston-terminal, and
# the seat daemon weston asks for a device through.
VOID_INSTALL_GRAPHICAL ?= weston mesa-dri xkeyboard-config dejavu-fonts-ttf \
	seatd xcursor-vanilla-dmz

# A game, and the only thing on the image that is there to be enjoyed rather
# than to prove something works.
#
# It is also the heaviest thing here by a long way, at 677 MB of track and
# character data, which is most of the download and most of the image.
#
# `make image VOID_INSTALL_GAMES=` leaves it out.
VOID_INSTALL_GAMES ?= supertuxkart

VOID_REMOVE ?=

BASE_FILES := $(shell find base-files -type f 2>/dev/null)

.PHONY: sysroot
sysroot: $(SYSROOT_STAMP)

$(SYSROOT_STAMP): support/sysroot.sh $(BASE_FILES) GNUmakefile | $(BUILD)
	VOID_MIRROR='$(VOID_MIRROR)' VOID_ROOTFS_DATE='$(VOID_ROOTFS_DATE)' \
	VOID_INSTALL='$(VOID_INSTALL)' VOID_REMOVE='$(VOID_REMOVE)' \
		support/sysroot.sh $(SYSROOT) $(CACHE)
	@touch $@

$(BUILD):
	@mkdir -p $@

# --- the image --------------------------------------------------------------

IMAGE := $(BUILD)/tunix.img

.PHONY: image
image: $(IMAGE)

# gpt or mbr, where an old BIOS booting from a USB stick wants mbr; see the
# header of support/image.sh.
#
# Switching it does not change a file make looks at, so rebuild with
# `rm -f $(IMAGE)` afterwards.
IMAGE_TABLE ?= gpt

# Free space in the root filesystem, over and above what the tree needs.
#
# The same caveat as above applies, so rebuild with `rm -f $(IMAGE)` after
# changing it.
IMAGE_SLACK_MIB ?= 4096

$(IMAGE): $(KERNEL) $(LIMINE_EXE) support/limine.conf support/image.sh $(SYSROOT_STAMP)
	TABLE='$(IMAGE_TABLE)' ROOT_SLACK_MIB='$(IMAGE_SLACK_MIB)' support/image.sh $@ $(KERNEL) $(LIMINE_DIR) support/limine.conf $(SYSROOT)

# --- running it -------------------------------------------------------------

# 4 GiB and four processors: the kernel starts every processor the firmware
# describes, and a desktop under a software rasteriser wants the memory.
QEMU_MEMORY ?= 4G
QEMU_SMP    ?= 4
# Where the sound goes.
#
# The card was always here and what was missing was anywhere for it to play.
#
# With `none` the guest drives a card whose samples are thrown away, which is a
# working driver and a silent machine, and the two are hard to tell apart.
#
# A PulseAudio socket is what both hosts this runs on offer: WSL publishes one
# for Windows at /mnt/wslg/PulseServer and a Linux desktop has its own under
# XDG_RUNTIME_DIR.
#
# Where there is neither, `none` is still right, because QEMU exits rather than
# starts if it is told to open a server that is not there.
#
# The two numbers matter as much as the socket does.
#
# QEMU asks PulseAudio for 15 milliseconds of latency by default, which is a fine
# bargain on a machine where the sound server is a local process.
#
# Under WSL it is not, because the samples go over an RDP channel to Windows and
# when that channel takes its time it stops the emulator with it.
#
# Measured from inside the guest, the application fed the card every 45
# milliseconds through a file sink and went as long as 896 milliseconds without
# feeding it through this one, which is five times the whole buffer and so the
# sound arrived in pieces.
#
# A tenth of a second of latency and twice that of buffer is enough to ride out
# those pauses, and nothing here is interactive enough to miss it.
COMMA := ,
PULSE_SOCKET := $(firstword $(wildcard /mnt/wslg/PulseServer $(XDG_RUNTIME_DIR)/pulse/native))
PULSE_TUNING := $(COMMA)out.latency=100000$(COMMA)out.buffer-length=200000
QEMU_AUDIO_BACKEND ?= $(if $(PULSE_SOCKET),pa$(COMMA)server=$(PULSE_SOCKET)$(PULSE_TUNING),none)
QEMU_AUDIO  ?= -audiodev $(QEMU_AUDIO_BACKEND)$(COMMA)id=snd0 \
	-device intel-hda -device hda-output,audiodev=snd0
# Modern-only makes QEMU expose the virtio 1.0 PCI device id the kernel drives;
# the transitional id names the legacy register layout it deliberately does not.
QEMU_NET    ?= -netdev user,id=net0 -device virtio-net-pci,disable-legacy=on,netdev=net0
# Named rather than left to QEMU, whose default depends on how the binary was
# built.
#
# A distribution package usually opens a window, but one built without a UI --
# which is what a server or a container image ships -- falls back to VNC and
# prints a port number instead, so the machine appears not to start.
#
# `make run QEMU_DISPLAY="-display sdl"` covers a QEMU without GTK, and
# `-display none` one with no user interface at all.
#
# grab-on-hover, because the pointer the guest has is a relative one that is told
# how far the mouse moved and never where it is.
#
# QEMU only sends that while it holds the host pointer, so without this nothing
# moves until you click in the window, which looks exactly like a machine whose
# mouse does not work.
#
# Hovering grabs, leaving releases, and ctrl+alt+g releases by hand.
QEMU_DISPLAY ?= -display gtk,grab-on-hover=on
QEMU_COMMON  = -machine q35,accel=kvm:tcg -cpu host -smp $(QEMU_SMP) \
	-m $(QEMU_MEMORY) -drive format=raw,file=$(IMAGE),if=none,id=disk0 \
	-device ide-hd,drive=disk0,bus=ide.0 \
	$(QEMU_NET) $(QEMU_AUDIO)

# The firmware for the UEFI targets, taken from the osdev0 nightlies where the
# Limine templates point rather than from a distribution package half the
# machines building this will not have.
OVMF_URL     ?= https://github.com/osdev0/edk2-ovmf-nightly/releases/latest/download/edk2-ovmf.tar.xz
OVMF_TARBALL := $(CACHE)/edk2-ovmf.tar.xz
OVMF         := $(CACHE)/edk2-ovmf/ovmf-code-x86_64.fd
# The variable store is writable and the firmware writes to it, so it is a
# build artefact rather than a cached download: a corrupted one is fixed by
# `make clean`, which would not touch the cache.
OVMF_VARS    := $(BUILD)/ovmf-vars-x86_64.fd

$(OVMF_TARBALL):
	@mkdir -p $(dir $@)
	curl -fL --retry 3 -o $@ $(OVMF_URL)

$(OVMF): $(OVMF_TARBALL)
	tar -xJf $< -C $(CACHE)
	@touch $@

$(OVMF_VARS): $(OVMF)
	@mkdir -p $(dir $@)
	cp $(CACHE)/edk2-ovmf/ovmf-vars-x86_64.fd $@

.PHONY: run run-uefi run-gpu run-virgl headless
run: $(IMAGE)
	rm -f $(BUILD)/serial.log
	$(QEMU) $(QEMU_COMMON) $(QEMU_DISPLAY) -serial file:$(BUILD)/serial.log -monitor none

run-uefi: $(IMAGE) $(OVMF) $(OVMF_VARS)
	rm -f $(BUILD)/serial.log
	$(QEMU) $(QEMU_COMMON) $(QEMU_DISPLAY) -serial file:$(BUILD)/serial.log -monitor none \
		-drive if=pflash,unit=0,format=raw,readonly=on,file=$(OVMF) \
		-drive if=pflash,unit=1,format=raw,file=$(OVMF_VARS)

# virtio-vga rather than virtio-gpu-pci, because Limine sets the mode over the
# VGA adapter and the kernel's text console draws into that framebuffer, both of
# which only exist on the VGA-compatible variant.
#
# The display is here rather than in QEMU_DISPLAY because this one wants an
# option of its own: the guest picks 1280x720 and zoom-to-fit keeps that readable
# in a window the host may have made smaller.
QEMU_GPU ?= -vga none -device virtio-vga,xres=1280,yres=720 \
	-display gtk,zoom-to-fit=on,grab-on-hover=on
run-gpu: $(IMAGE)
	rm -f $(BUILD)/serial.log
	$(QEMU) $(QEMU_COMMON) $(QEMU_GPU) -serial file:$(BUILD)/serial.log -monitor none

# The same adapter with the host's GL behind it.
#
# The `-gl` suffix is what makes QEMU load virglrenderer and offer the VIRGL
# feature, and without it the device is identical and the guest sees a 2D card.
#
# The host needs a GL context of its own to render into, which is what `gl=on`
# asks the display for.
#
# A host that cannot give one leaves the device in 2D, so the guest still boots
# and just finds no capset.
#
# SDL rather than GTK, and only here, because GTK's GL widget does not deliver
# pointer motion to the guest.
#
# The same image under `-display gtk` with a 2D card has a working mouse and
# under `-display sdl,gl=on` it has a working mouse, but `-display gtk,gl=on` has
# none, with or without scaling and with grab-on-hover either way.
#
# Nothing on the guest side differs between the three, and the pointer is drawn
# and moved the moment motion arrives.
#
# No zoom-to-fit either, because the guest now comes up at exactly the size asked
# for and there is nothing to scale or leave over as a black band.
QEMU_VIRGL ?= -vga none -device virtio-vga-gl,xres=1280,yres=720 \
	-display sdl,gl=on
# WSL has no render node, so mesa cannot find a GPU the ordinary way and falls
# back to software, which would put the host's rasteriser behind the guest's and
# be slower than not doing this at all.
#
# It does have Direct3D 12 and the libraries Windows exposes, and naming both
# reaches the real card.
#
# On a Linux host with a render node the defaults are already right and this is
# empty.
QEMU_GL_ENV ?= $(if $(wildcard /dev/dri),,\
	$(if $(wildcard /usr/lib/wsl/lib),env LD_LIBRARY_PATH=/usr/lib/wsl/lib GALLIUM_DRIVER=d3d12))
run-virgl: $(IMAGE)
	rm -f $(BUILD)/serial.log
	$(QEMU_GL_ENV) $(QEMU) $(QEMU_COMMON) $(QEMU_VIRGL) \
		-serial file:$(BUILD)/serial.log -monitor none

headless: $(IMAGE)
	$(QEMU) $(QEMU_COMMON) -nographic -monitor none -serial stdio

# --- measuring the scheduler ------------------------------------------------
#
# A machine whose entire userland is one static benchmark, which is why this
# needs neither the Void download nor a filesystem that can hold ownership.
#
# It builds and runs as an ordinary user in a few seconds.
#
# SCHEDBENCH_CPUS is the processor count to boot with, and the point of it is the
# comparison, because most of what the scheduler gets wrong is only visible in
# the difference between one processor and several.
SCHEDBENCH_CPUS ?= 4

.PHONY: schedbench
schedbench: $(KERNEL) $(LIMINE_EXE)
	support/tests/schedbench.sh $(SCHEDBENCH_CPUS) $(KERNEL)
