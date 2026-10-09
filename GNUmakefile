override MAKEFLAGS += -rR

-include local.mk

ifneq ($(filter default undefined,$(origin CC)),)
  override CC := cc
endif

QEMU    ?= qemu-system-x86_64

BUILD   := build
KERNEL  := $(BUILD)/kernel.elf

LIMINE_VERSION := v9.x-binary
LIMINE_DIR     := $(BUILD)/limine
LIMINE_HEADER  := $(LIMINE_DIR)/limine.h
LIMINE_EXE     := $(LIMINE_DIR)/limine

KERNEL_CFLAGS := -std=gnu11 -Wall -Wextra -Werror -ffreestanding \
	-fno-stack-protector -fno-pic -fno-pie -fno-builtin \
	-fno-asynchronous-unwind-tables -fno-unwind-tables -mno-red-zone -m64 \
	-Os -ffunction-sections -fdata-sections -mcmodel=kernel -mgeneral-regs-only \
	-Ikernel/include -I$(LIMINE_DIR) \
	$(KERNEL_CFLAGS_EXTRA)
KERNEL_LDFLAGS := -nostdlib -no-pie -Wl,-T,kernel/arch/x86_64/linker.ld \
	-Wl,--gc-sections -Wl,--build-id=none -Wl,-z,max-page-size=0x1000

KERNEL_SOURCES := $(shell find kernel -path kernel/arch/aarch64 -prune -o -path kernel/modules -prune -o \( -name '*.c' -o -name '*.S' \) -print)
KERNEL_OBJECTS := $(KERNEL_SOURCES:%=$(BUILD)/%.o)
KERNEL_DEPS    := $(KERNEL_OBJECTS:.o=.d)

.PHONY: all kernel check clean distclean
all: image
kernel: $(KERNEL)

check:
	$(MAKE) kernel modules BUILD=$(BUILD)/check/default LIMINE_DIR=$(LIMINE_DIR)
	$(MAKE) kernel modules BUILD=$(BUILD)/check/debug LIMINE_DIR=$(LIMINE_DIR) KERNEL_CFLAGS_EXTRA=-DTUNIX_DEBUG_LOGS=1
	$(MAKE) kernel modules BUILD=$(BUILD)/check/timings LIMINE_DIR=$(LIMINE_DIR) KERNEL_CFLAGS_EXTRA=-DTUNIX_BOOT_TIMINGS=1

$(KERNEL): $(KERNEL_OBJECTS) kernel/arch/x86_64/linker.ld
	$(CC) $(KERNEL_LDFLAGS) $(KERNEL_OBJECTS) -o $@

$(BUILD)/%.c.o: %.c | $(LIMINE_HEADER)
	@mkdir -p $(dir $@)
	$(CC) $(KERNEL_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/%.S.o: %.S | $(LIMINE_HEADER)
	@mkdir -p $(dir $@)
	$(CC) $(KERNEL_CFLAGS) -MMD -MP -c $< -o $@


KERNEL_RELEASE := $(shell sed -n 's/^#define UTS_RELEASE "\(.*\)"/\1/p' kernel/include/tunix/uts.h)

MODULE_SOURCES := $(wildcard kernel/modules/*.c) $(wildcard kernel/modules/x86_64/*.c)
MODULES := $(patsubst kernel/modules/%.c,$(BUILD)/modules/%.ko,$(MODULE_SOURCES))
MODULE_CFLAGS = $(filter-out -ffunction-sections -fdata-sections,$(KERNEL_CFLAGS)) \
	-DTUNIX_MODULE_NAME='"$(notdir $*)"'

.PHONY: modules
modules: $(MODULES)

$(BUILD)/modules/%.ko: kernel/modules/%.c | $(LIMINE_HEADER)
	@mkdir -p $(dir $@)
	$(CC) $(MODULE_CFLAGS) -MMD -MP -c $< -o $@

$(MODULES): GNUmakefile

-include $(MODULES:.ko=.d)

$(KERNEL_OBJECTS): GNUmakefile

-include $(KERNEL_DEPS)

$(LIMINE_HEADER):
	rm -rf $(LIMINE_DIR)
	git clone --depth=1 --branch=$(LIMINE_VERSION) \
		https://github.com/limine-bootloader/limine.git $(LIMINE_DIR)

$(LIMINE_EXE): $(LIMINE_HEADER)
	$(MAKE) -C $(LIMINE_DIR)

clean:
	rm -rf $(BUILD)/kernel $(BUILD)/modules $(KERNEL) $(IMAGE)

distclean:
	$(USERNS) rm -rf $(BUILD)

ifneq ($(SUDO_UID),)
$(error do not run make with sudo: the build runs in a user namespace and needs no privileges)
endif

USERNS ?= $(if $(filter 0,$(shell id -u)),,unshare --map-auto --map-root-user)

CACHE         ?= $(BUILD)/cache
SYSROOT       ?= $(BUILD)/sysroot
SYSROOT_STAMP := $(BUILD)/.sysroot

VOID_MIRROR      ?= https://repo-default.voidlinux.org
VOID_ROOTFS_DATE ?= 20250202
VOID_INSTALL ?= base-files bash coreutils util-linux findutils diffutils \
	grep sed gawk tar gzip xz procps-ng psmisc iproute2 iputils file less \
	which ncurses shadow sudo runit runit-void tzdata ca-certificates \
	e2fsprogs kbd nano htop curl fastfetch pciutils \
	$(VOID_INSTALL_GRAPHICAL) $(VOID_INSTALL_BROWSER) $(VOID_INSTALL_GAMES) \
	$(VOID_INSTALL_TOOLCHAIN)

DESKTOP ?= weston
DESKTOP_NAME := $(shell printf '%s' '$(DESKTOP)' | tr '[:upper:]' '[:lower:]')

VOID_INSTALL_GRAPHICAL ?= mesa-dri xorg-server-xwayland xkeyboard-config dejavu-fonts-ttf \
	xcursor-vanilla-dmz $(VOID_INSTALL_DESKTOP_$(DESKTOP_NAME))

VOID_INSTALL_DESKTOP_gnome ?= dbus elogind polkit gdm gnome-core gnome-console \
	gnome-text-editor gnome-system-monitor gnome-calculator

VOID_INSTALL_DESKTOP_weston ?= weston seatd

ifeq ($(filter $(DESKTOP_NAME),gnome weston),)
$(error DESKTOP must be weston or gnome, not '$(DESKTOP)')
endif

VOID_INSTALL_BROWSER ?= firefox

VOID_INSTALL_TOOLCHAIN ?= gcc make python3 python3-Pillow git binutils mtools

VOID_INSTALL_GAMES ?= supertuxkart

VOID_REMOVE ?=

BASE_FILES := $(shell find base-files -type f 2>/dev/null)

.PHONY: sysroot
sysroot: $(SYSROOT_STAMP)

SYSROOT_RECIPE := $(BUILD)/sysroot-recipe
$(shell mkdir -p $(BUILD); printf '%s\n' '$(VOID_MIRROR)' '$(VOID_ROOTFS_DATE)' \
	'$(VOID_INSTALL)' '$(VOID_REMOVE)' '$(DESKTOP_NAME)' > $(SYSROOT_RECIPE).tmp; \
	cmp -s $(SYSROOT_RECIPE).tmp $(SYSROOT_RECIPE) 2>/dev/null \
		&& rm -f $(SYSROOT_RECIPE).tmp \
		|| mv $(SYSROOT_RECIPE).tmp $(SYSROOT_RECIPE))

$(SYSROOT_STAMP): tools/sysroot.sh $(BASE_FILES) $(SYSROOT_RECIPE) | $(BUILD)
	$(USERNS) env VOID_MIRROR='$(VOID_MIRROR)' VOID_ROOTFS_DATE='$(VOID_ROOTFS_DATE)' \
	VOID_INSTALL='$(VOID_INSTALL)' VOID_REMOVE='$(VOID_REMOVE)' DESKTOP='$(DESKTOP_NAME)' \
		tools/sysroot.sh $(SYSROOT) $(CACHE)
	@touch $@

$(BUILD):
	@mkdir -p $@

IMAGE := $(BUILD)/tunix.img

.PHONY: image
image: $(IMAGE)

IMAGE_TABLE ?= gpt

IMAGE_SLACK_MIB ?= 4096

$(IMAGE): $(KERNEL) $(MODULES) $(LIMINE_EXE) tools/limine.conf tools/image.sh $(SYSROOT_STAMP)
	$(USERNS) env TABLE='$(IMAGE_TABLE)' ROOT_SLACK_MIB='$(IMAGE_SLACK_MIB)' \
	MODULES='$(BUILD)/modules' RELEASE='$(KERNEL_RELEASE)' \
		tools/image.sh $@ $(KERNEL) $(LIMINE_DIR) tools/limine.conf $(SYSROOT)

QEMU_MEMORY ?= 4G
QEMU_SMP    ?= 4
COMMA := ,
PULSE_SOCKET := $(firstword $(wildcard /mnt/wslg/PulseServer \
	$(XDG_RUNTIME_DIR)/pulse/native))
PULSE_TUNING := $(COMMA)out.latency=100000$(COMMA)out.buffer-length=200000
QEMU_AUDIO_BACKEND ?= $(if $(PULSE_SOCKET),pa$(COMMA)server=$(PULSE_SOCKET)$(PULSE_TUNING),none)
AUDIO_NOTE = $(if $(PULSE_SOCKET),@echo ":: audio -> $(PULSE_SOCKET)",\
	@echo ":: audio -> nowhere: no PulseAudio socket found, so the machine will be silent." \
	; echo ":: set PULSE_SOCKET=/path/to/native to choose one")
QEMU_AUDIO  ?= -audiodev $(QEMU_AUDIO_BACKEND)$(COMMA)id=snd0 \
	-device intel-hda -device hda-output,audiodev=snd0
QEMU_NET    ?= -netdev user,id=net0 -device virtio-net-pci,disable-legacy=on,netdev=net0
QEMU_DISPLAY ?= -display gtk,grab-on-hover=on
QEMU_POINTER ?= -device qemu-xhci,id=pointer -device usb-tablet,bus=pointer.0
QEMU_COMMON  = -machine q35,accel=kvm:tcg -cpu host -smp $(QEMU_SMP) \
	-m $(QEMU_MEMORY) -drive format=raw,file=$(IMAGE),if=none,id=disk0 \
	-device ide-hd,drive=disk0,bus=ide.0 \
	$(QEMU_NET) $(QEMU_AUDIO)

OVMF_URL     ?= https://github.com/osdev0/edk2-ovmf-nightly/releases/latest/download/edk2-ovmf.tar.xz
OVMF_TARBALL := $(CACHE)/edk2-ovmf.tar.xz
OVMF         := $(CACHE)/edk2-ovmf/ovmf-code-x86_64.fd
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
	$(AUDIO_NOTE)
	rm -f $(BUILD)/serial.log
	$(QEMU) $(QEMU_COMMON) $(QEMU_POINTER) $(QEMU_DISPLAY) -serial file:$(BUILD)/serial.log -monitor none

run-uefi: $(IMAGE) $(OVMF) $(OVMF_VARS)
	$(AUDIO_NOTE)
	rm -f $(BUILD)/serial.log
	$(QEMU) $(QEMU_COMMON) $(QEMU_POINTER) $(QEMU_DISPLAY) -serial file:$(BUILD)/serial.log -monitor none \
		-drive if=pflash,unit=0,format=raw,readonly=on,file=$(OVMF) \
		-drive if=pflash,unit=1,format=raw,file=$(OVMF_VARS)

QEMU_GPU ?= -vga none -device virtio-vga,xres=1280,yres=720 \
	-display gtk,zoom-to-fit=on,grab-on-hover=on
run-gpu: $(IMAGE)
	$(AUDIO_NOTE)
	rm -f $(BUILD)/serial.log
	$(QEMU) $(QEMU_COMMON) $(QEMU_POINTER) $(QEMU_GPU) -serial file:$(BUILD)/serial.log -monitor none

QEMU_VIRGL ?= -vga none -device virtio-vga-gl,xres=1280,yres=720 \
	-display sdl,gl=on
QEMU_GL_ENV ?= $(if $(wildcard /dev/dri),,\
	$(if $(wildcard /usr/lib/wsl/lib),env LD_LIBRARY_PATH=/usr/lib/wsl/lib GALLIUM_DRIVER=d3d12))
run-virgl: $(IMAGE)
	$(AUDIO_NOTE)
	rm -f $(BUILD)/serial.log
	$(QEMU_GL_ENV) $(QEMU) $(QEMU_COMMON) $(QEMU_POINTER) $(QEMU_VIRGL) \
		-serial file:$(BUILD)/serial.log -monitor none

headless: $(IMAGE)
	$(AUDIO_NOTE)
	$(QEMU) $(QEMU_COMMON) -nographic -monitor none -serial stdio

AARCH64_CC     ?= aarch64-linux-gnu-gcc
AARCH64_OBJCOPY ?= aarch64-linux-gnu-objcopy

AARCH64_CORE_BUILD := $(BUILD)/aarch64-core
AARCH64_CORE_CFLAGS := -std=gnu11 -Wall -Wextra -Werror -ffreestanding \
	-fno-stack-protector -fno-pic -fno-pie -fno-builtin \
	-fno-asynchronous-unwind-tables -fno-unwind-tables -mgeneral-regs-only \
	-mstrict-align -march=armv8-a -mno-outline-atomics -Os \
	-ffunction-sections -fdata-sections \
	-Ikernel/include $(KERNEL_CFLAGS_EXTRA)
AARCH64_CORE_EXCLUDE := kernel/drivers/storage/ata.c
AARCH64_CORE_SOURCES := $(filter-out $(AARCH64_CORE_EXCLUDE),$(shell find kernel -path kernel/arch -prune -o -path kernel/modules -prune -o \( -name '*.c' -o -name '*.S' \) -print)) \
	$(shell find kernel/arch/aarch64 \( -name '*.c' -o -name '*.S' \) -print)
AARCH64_CORE_OBJECTS := $(AARCH64_CORE_SOURCES:%=$(AARCH64_CORE_BUILD)/%.o)
AARCH64_CORE_KERNEL := $(BUILD)/kernel-aarch64-core.elf
AARCH64_CORE_IMAGE := $(BUILD)/kernel-aarch64-core.img

.PHONY: aarch64-core run-aarch64-core
aarch64-core: $(AARCH64_CORE_IMAGE)

$(AARCH64_CORE_KERNEL): $(AARCH64_CORE_OBJECTS) kernel/arch/aarch64/linker.ld
	$(AARCH64_CC) -nostdlib -static -Wl,-T,kernel/arch/aarch64/linker.ld \
		-Wl,--gc-sections -Wl,--build-id=none -Wl,-z,max-page-size=0x1000 \
		$(AARCH64_CORE_OBJECTS) -lgcc -o $@

$(AARCH64_CORE_IMAGE): $(AARCH64_CORE_KERNEL)
	$(AARCH64_OBJCOPY) -O binary $< $@

$(AARCH64_CORE_BUILD)/%.c.o: %.c
	@mkdir -p $(dir $@)
	$(AARCH64_CC) $(AARCH64_CORE_CFLAGS) -MMD -MP -c $< -o $@

$(AARCH64_CORE_BUILD)/%.S.o: %.S
	@mkdir -p $(dir $@)
	$(AARCH64_CC) $(AARCH64_CORE_CFLAGS) -MMD -MP -c $< -o $@

$(AARCH64_CORE_OBJECTS): GNUmakefile

-include $(AARCH64_CORE_OBJECTS:.o=.d)

AARCH64_MODULE_SOURCES := $(wildcard kernel/modules/*.c)
AARCH64_MODULES := $(patsubst kernel/modules/%.c,$(AARCH64_CORE_BUILD)/modules/%.ko,$(AARCH64_MODULE_SOURCES))
AARCH64_MODULE_CFLAGS = $(AARCH64_CORE_CFLAGS) -DTUNIX_MODULE_NAME='"$(notdir $*)"'

.PHONY: modules-aarch64
modules-aarch64: $(AARCH64_MODULES)

$(AARCH64_CORE_BUILD)/modules/%.ko: kernel/modules/%.c
	@mkdir -p $(dir $@)
	$(AARCH64_CC) $(AARCH64_MODULE_CFLAGS) -MMD -MP -c $< -o $@

$(AARCH64_MODULES): GNUmakefile

-include $(AARCH64_MODULES:.ko=.d)

QEMU_AARCH64_CORE_DISKS ?=

SYSROOT_AARCH64 ?= $(BUILD)/sysroot-aarch64
SYSROOT_AARCH64_STAMP := $(BUILD)/.sysroot-aarch64
IMAGE_AARCH64 := $(BUILD)/tunix-aarch64.img

.PHONY: sysroot-aarch64 image-aarch64 run-aarch64-image
sysroot-aarch64: $(SYSROOT_AARCH64_STAMP)

$(SYSROOT_AARCH64_STAMP): tools/sysroot.sh $(BASE_FILES) $(SYSROOT_RECIPE) | $(BUILD)
	$(USERNS) env VOID_ARCH=aarch64 VOID_MIRROR='$(VOID_MIRROR)' \
		VOID_ROOTFS_DATE='$(VOID_ROOTFS_DATE)' VOID_INSTALL='$(VOID_INSTALL)' \
		VOID_REMOVE='$(VOID_REMOVE)' DESKTOP='$(DESKTOP_NAME)' tools/sysroot.sh $(SYSROOT_AARCH64) $(CACHE)
	@touch $@

image-aarch64: $(IMAGE_AARCH64)

$(IMAGE_AARCH64): $(AARCH64_CORE_IMAGE) $(AARCH64_MODULES) $(LIMINE_EXE) tools/limine-aarch64.conf tools/image.sh $(SYSROOT_AARCH64_STAMP)
	$(USERNS) env ARCH=aarch64 TABLE='$(IMAGE_TABLE)' ROOT_SLACK_MIB='$(IMAGE_SLACK_MIB)' \
	MODULES='$(AARCH64_CORE_BUILD)/modules' RELEASE='$(KERNEL_RELEASE)' \
		tools/image.sh $@ $(AARCH64_CORE_IMAGE) $(LIMINE_DIR) tools/limine-aarch64.conf $(SYSROOT_AARCH64)

QEMU_AARCH64_DEVICES ?= -device ramfb -device qemu-xhci -device usb-kbd -device usb-mouse \
	-netdev user,id=net0 -device virtio-net-pci,disable-legacy=on,netdev=net0
OVMF_AARCH64      := $(CACHE)/edk2-ovmf/ovmf-code-aarch64.fd
OVMF_AARCH64_VARS := $(BUILD)/ovmf-vars-aarch64.fd

$(OVMF_AARCH64): $(OVMF_TARBALL)
	tar -xJf $< -C $(CACHE)
	@touch $@

$(OVMF_AARCH64_VARS): $(OVMF_AARCH64)
	@mkdir -p $(dir $@)
	cp $(CACHE)/edk2-ovmf/ovmf-vars-aarch64.fd $@

.PHONY: run-aarch64-image-kernel
run-aarch64-image: $(IMAGE_AARCH64) $(OVMF_AARCH64) $(OVMF_AARCH64_VARS)
	$(QEMU_AARCH64) -M virt,gic-version=3 -cpu cortex-a72 -smp $(QEMU_SMP) -m $(QEMU_MEMORY) \
		-drive if=pflash,format=raw,readonly=on,file=$(OVMF_AARCH64) \
		-drive if=pflash,format=raw,file=$(OVMF_AARCH64_VARS) \
		-drive file=$(IMAGE_AARCH64),if=none,id=disk0,format=raw -device nvme,drive=disk0,serial=tunix \
		$(QEMU_AARCH64_DEVICES) -serial stdio -display gtk
run-aarch64-image-kernel: $(IMAGE_AARCH64)
	$(QEMU_AARCH64) -M virt,gic-version=3 -cpu cortex-a72 -smp $(QEMU_SMP) -m $(QEMU_MEMORY) \
		-kernel $(AARCH64_CORE_IMAGE) -append root=LABEL=tunix-root \
		-drive file=$(IMAGE_AARCH64),if=none,id=disk0,format=raw -device nvme,drive=disk0,serial=tunix \
		$(QEMU_AARCH64_DEVICES) -serial stdio -display gtk
run-aarch64-core: $(AARCH64_CORE_IMAGE)
	$(QEMU_AARCH64) -M virt,gic-version=3 -cpu cortex-a72 -m 2G -nographic \
		-no-reboot -kernel $(AARCH64_CORE_IMAGE) $(QEMU_AARCH64_CORE_DISKS)

QEMU_AARCH64 ?= qemu-system-aarch64

TESTS ?=

.PHONY: test test-aarch64
test: $(KERNEL) $(LIMINE_EXE)
	KERNEL=$(KERNEL) LIMINE=$(LIMINE_DIR) tools/tests/run.sh $(TESTS)

test-aarch64: $(AARCH64_CORE_IMAGE)
	ARCH=aarch64 KERNEL=$(AARCH64_CORE_IMAGE) LIMINE=$(LIMINE_DIR) tools/tests/run.sh $(TESTS)

CLANG_FORMAT ?= clang-format
CLANG_TIDY   ?= clang-tidy
NPROC        := $(shell nproc 2>/dev/null || echo 1)

FORMAT_SOURCES := $(shell find kernel tools/tests -name '*.[ch]')
LINT_FLAGS := -std=gnu11 -ffreestanding -m64 -mcmodel=kernel -mgeneral-regs-only \
	-mno-red-zone -Ikernel/include -I$(LIMINE_DIR)

.PHONY: format format-check lint
format:
	@$(CLANG_FORMAT) -i $(FORMAT_SOURCES)

format-check:
	@$(CLANG_FORMAT) --dry-run --Werror $(FORMAT_SOURCES)

lint: | $(LIMINE_HEADER)
	@printf '%s\n' $(filter %.c,$(KERNEL_SOURCES)) | \
		xargs -P$(NPROC) -I{} $(CLANG_TIDY) --quiet {} -- $(LINT_FLAGS)
	@printf '%s\n' $(MODULE_SOURCES) | \
		xargs -P$(NPROC) -I{} $(CLANG_TIDY) --quiet {} -- $(LINT_FLAGS) -DTUNIX_MODULE_NAME='"lint"'
