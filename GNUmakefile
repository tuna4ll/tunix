# Tunix: the kernel, the Void sysroot the userland comes from, and the image Limine boots.

override MAKEFLAGS += -rR

# Settings for this machine rather than the repository, SYSROOT above all; ignored by git.
-include local.mk

# -R clears make's built-ins after this file is read, and a sub-make sees the origin as undefined.
ifneq ($(filter default undefined,$(origin CC)),)
  override CC := cc
endif

PYTHON  ?= python3
QEMU    ?= qemu-system-x86_64

BUILD   := build
KERNEL  := $(BUILD)/kernel.elf

# Limine is fetched rather than vendored, so the header and the bootloader are one version.
LIMINE_VERSION := v9.x-binary
LIMINE_DIR     := $(BUILD)/limine
LIMINE_HEADER  := $(LIMINE_DIR)/limine.h
LIMINE_EXE     := $(LIMINE_DIR)/limine

# --- the kernel -------------------------------------------------------------

TERMINAL_FONT_SOURCE ?= assets/fonts/jetbrains-mono/JetBrainsMono-Regular.ttf
TERMINAL_FONT_DATA   := $(BUILD)/generated/terminal_font_data.inc

# -mgeneral-regs-only keeps the kernel out of registers that belong to the running process.
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

# What CI builds, because two of these three configurations would otherwise rot unbuilt.
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

# Each configuration gets its own directory, since the generated dependencies do not know the flags.
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

# --- the sysroot: Void Linux, installed by Void's own package manager.

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

# The graphical session: weston, and what a compositor needs that nothing depends on.
VOID_INSTALL_GRAPHICAL ?= weston mesa-dri xkeyboard-config dejavu-fonts-ttf \
	seatd xcursor-vanilla-dmz

# A game, and the heaviest thing here; `make image VOID_INSTALL_GAMES=` leaves it out.
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

# gpt or mbr, where an old BIOS booting from a USB stick wants mbr.
IMAGE_TABLE ?= gpt

# Free space over what the tree needs; rebuild with `rm -f $(IMAGE)` after changing it.
IMAGE_SLACK_MIB ?= 4096

$(IMAGE): $(KERNEL) $(LIMINE_EXE) support/limine.conf support/image.sh $(SYSROOT_STAMP)
	TABLE='$(IMAGE_TABLE)' ROOT_SLACK_MIB='$(IMAGE_SLACK_MIB)' support/image.sh $@ $(KERNEL) $(LIMINE_DIR) support/limine.conf $(SYSROOT)

# --- running it -------------------------------------------------------------

# 4 GiB and four processors: the kernel starts every processor the firmware
# describes, and a desktop under a software rasteriser wants the memory.
QEMU_MEMORY ?= 4G
QEMU_SMP    ?= 4
# Where the sound goes, with a latency high enough to ride out a slow host.
COMMA := ,
PULSE_SOCKET := $(firstword $(wildcard /mnt/wslg/PulseServer $(XDG_RUNTIME_DIR)/pulse/native))
PULSE_TUNING := $(COMMA)out.latency=100000$(COMMA)out.buffer-length=200000
QEMU_AUDIO_BACKEND ?= $(if $(PULSE_SOCKET),pa$(COMMA)server=$(PULSE_SOCKET)$(PULSE_TUNING),none)
QEMU_AUDIO  ?= -audiodev $(QEMU_AUDIO_BACKEND)$(COMMA)id=snd0 \
	-device intel-hda -device hda-output,audiodev=snd0
# Modern-only makes QEMU expose the virtio 1.0 PCI device id the kernel drives;
# the transitional id names the legacy register layout it deliberately does not.
QEMU_NET    ?= -netdev user,id=net0 -device virtio-net-pci,disable-legacy=on,netdev=net0
# Named rather than left to QEMU, whose default depends on how the binary was built.
QEMU_DISPLAY ?= -display gtk,grab-on-hover=on
QEMU_COMMON  = -machine q35,accel=kvm:tcg -cpu host -smp $(QEMU_SMP) \
	-m $(QEMU_MEMORY) -drive format=raw,file=$(IMAGE),if=none,id=disk0 \
	-device ide-hd,drive=disk0,bus=ide.0 \
	$(QEMU_NET) $(QEMU_AUDIO)

# The firmware for the UEFI targets, from the nightlies the Limine templates point at.
OVMF_URL     ?= https://github.com/osdev0/edk2-ovmf-nightly/releases/latest/download/edk2-ovmf.tar.xz
OVMF_TARBALL := $(CACHE)/edk2-ovmf.tar.xz
OVMF         := $(CACHE)/edk2-ovmf/ovmf-code-x86_64.fd
# The variable store is written by the firmware, so it is a build artefact rather than a cache.
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

# virtio-vga rather than virtio-gpu-pci, because Limine sets the mode over the VGA adapter.
QEMU_GPU ?= -vga none -device virtio-vga,xres=1280,yres=720 \
	-display gtk,zoom-to-fit=on,grab-on-hover=on
run-gpu: $(IMAGE)
	rm -f $(BUILD)/serial.log
	$(QEMU) $(QEMU_COMMON) $(QEMU_GPU) -serial file:$(BUILD)/serial.log -monitor none

# The same adapter with the host's GL behind it, which wants SDL rather than GTK.
QEMU_VIRGL ?= -vga none -device virtio-vga-gl,xres=1280,yres=720 \
	-display sdl,gl=on
# WSL has no render node, so mesa needs Direct3D 12 named to reach the real card.
QEMU_GL_ENV ?= $(if $(wildcard /dev/dri),,\
	$(if $(wildcard /usr/lib/wsl/lib),env LD_LIBRARY_PATH=/usr/lib/wsl/lib GALLIUM_DRIVER=d3d12))
run-virgl: $(IMAGE)
	rm -f $(BUILD)/serial.log
	$(QEMU_GL_ENV) $(QEMU) $(QEMU_COMMON) $(QEMU_VIRGL) \
		-serial file:$(BUILD)/serial.log -monitor none

headless: $(IMAGE)
	$(QEMU) $(QEMU_COMMON) -nographic -monitor none -serial stdio

# --- measuring the scheduler: a machine whose entire userland is one static benchmark.
SCHEDBENCH_CPUS ?= 4

.PHONY: schedbench drmtest perftest inputtest testimage
schedbench: $(KERNEL) $(LIMINE_EXE)
	support/tests/schedbench.sh $(SCHEDBENCH_CPUS) $(KERNEL)

# The DRM device asked what a Linux graphics client asks, with Linux's own structures.
drmtest: $(KERNEL) $(LIMINE_EXE)
	support/tests/drmtest.sh $(SCHEDBENCH_CPUS) $(KERNEL)

# What the kernel costs: a syscall, a pipe, a page fault, a fork and a read.
perftest: $(KERNEL) $(LIMINE_EXE)
	support/tests/perftest.sh $(SCHEDBENCH_CPUS) $(KERNEL)

# Keys typed into the machine while several processors read the keyboard.
inputtest: $(KERNEL) $(LIMINE_EXE)
	support/tests/inputtest.sh $(SCHEDBENCH_CPUS) $(KERNEL)

# The same image, built and not booted, for writing to a stick and running on a
# real machine; TEST names which one and the results land on its root
# filesystem, so no serial cable is needed to read them.
# IMAGE_TABLE=mbr for an old BIOS booting from a stick; see support/image.sh.
TEST ?= schedbench
testimage: $(KERNEL) $(LIMINE_EXE)
	BOOT=0 IMAGE_TABLE='$(IMAGE_TABLE)' support/tests/$(TEST).sh $(SCHEDBENCH_CPUS) $(KERNEL)
