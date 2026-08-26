# Tunix.
#
# One makefile for the whole system: the kernel, the Void Linux sysroot the
# userland comes from, and the disk image Limine boots. There is nothing to
# build from source but the kernel -- everything above it is a Void package.

override MAKEFLAGS += -rR

# -R above clears make's built-in variables, and it does so after this file is
# read: a plain `CC ?= cc` would see the built-in, decline to assign, and then
# be wiped. Anything the environment or the command line set still wins.
ifeq ($(origin CC),default)
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
# entirely. Those registers belong to whichever process is running: the kernel
# saves them only when it switches processes, so any use in between -- GCC will
# happily reach for %xmm0 to copy a 16-byte struct -- corrupts user state on a
# plain syscall, with no context switch in sight.
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

.PHONY: all kernel clean distclean
all: image
kernel: $(KERNEL)

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
# Void Linux, installed by Void's own package manager. Nothing above the kernel
# is built here.

CACHE         := $(BUILD)/cache
SYSROOT       ?= $(BUILD)/sysroot
SYSROOT_STAMP := $(BUILD)/.sysroot

VOID_MIRROR      ?= https://repo-default.voidlinux.org
VOID_ROOTFS_DATE ?= 20250202
# The glibc set, not the musl one: the ROOTFS tarball without -musl in its name
# is the glibc build, and every package installed on top of it follows.
VOID_INSTALL ?= base-files bash coreutils util-linux findutils diffutils \
	grep sed gawk tar gzip xz procps-ng psmisc iproute2 iputils file less \
	which ncurses shadow sudo runit runit-void tzdata ca-certificates \
	e2fsprogs kbd nano htop curl fastfetch
VOID_REMOVE ?=

BASE_FILES := $(shell find base-files -type f 2>/dev/null)

.PHONY: sysroot
sysroot: $(SYSROOT_STAMP)

$(SYSROOT_STAMP): support/sysroot.sh $(BASE_FILES) | $(BUILD)
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

$(IMAGE): $(KERNEL) $(LIMINE_EXE) support/limine.conf support/image.sh $(SYSROOT_STAMP)
	support/image.sh $@ $(KERNEL) $(LIMINE_DIR) support/limine.conf $(SYSROOT)

# --- running it -------------------------------------------------------------

# 4 GiB and four processors: the kernel starts every processor the firmware
# describes, and a desktop under a software rasteriser wants the memory.
QEMU_MEMORY ?= 4G
QEMU_SMP    ?= 4
QEMU_AUDIO  ?= -audiodev none,id=snd0 -device intel-hda -device hda-output,audiodev=snd0
QEMU_NET    ?= -netdev user,id=net0 -device rtl8139,netdev=net0
QEMU_COMMON  = -machine q35,accel=kvm:tcg -cpu host -smp $(QEMU_SMP) \
	-m $(QEMU_MEMORY) -drive format=raw,file=$(IMAGE),if=none,id=disk0 \
	-device ide-hd,drive=disk0,bus=ide.0 \
	$(QEMU_NET) $(QEMU_AUDIO)

# The firmware for the UEFI targets. Taken from the osdev0 nightlies, which is
# where the Limine templates point, rather than from a distribution package
# that half the machines building this will not have.
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

.PHONY: run run-uefi run-gpu headless
run: $(IMAGE)
	rm -f $(BUILD)/serial.log
	$(QEMU) $(QEMU_COMMON) -serial file:$(BUILD)/serial.log -monitor none

run-uefi: $(IMAGE) $(OVMF) $(OVMF_VARS)
	rm -f $(BUILD)/serial.log
	$(QEMU) $(QEMU_COMMON) -serial file:$(BUILD)/serial.log -monitor none \
		-drive if=pflash,unit=0,format=raw,readonly=on,file=$(OVMF) \
		-drive if=pflash,unit=1,format=raw,file=$(OVMF_VARS)

# virtio-vga rather than virtio-gpu-pci: Limine sets the mode over the VGA
# adapter and the kernel's text console draws into that framebuffer, both of
# which only exist on the VGA-compatible variant.
QEMU_GPU ?= -vga none -device virtio-vga,xres=1280,yres=720 \
	-display gtk,zoom-to-fit=on
run-gpu: $(IMAGE)
	rm -f $(BUILD)/serial.log
	$(QEMU) $(QEMU_COMMON) $(QEMU_GPU) -serial file:$(BUILD)/serial.log -monitor none

headless: $(IMAGE)
	$(QEMU) $(QEMU_COMMON) -nographic -monitor none -serial stdio
