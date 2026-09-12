override MAKEFLAGS += -rR

-include local.mk

ifneq ($(filter default undefined,$(origin CC)),)
  override CC := cc
endif

PYTHON  ?= python3
QEMU    ?= qemu-system-x86_64

BUILD   := build
KERNEL  := $(BUILD)/kernel.elf

LIMINE_VERSION := v9.x-binary
LIMINE_DIR     := $(BUILD)/limine
LIMINE_HEADER  := $(LIMINE_DIR)/limine.h
LIMINE_EXE     := $(LIMINE_DIR)/limine

TERMINAL_FONT_SOURCE ?= assets/fonts/jetbrains-mono/JetBrainsMono-Regular.ttf
TERMINAL_FONT_DATA   := $(BUILD)/generated/terminal_font_data.inc

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

check:
	$(MAKE) kernel BUILD=$(BUILD)/check/default LIMINE_DIR=$(LIMINE_DIR)
	$(MAKE) kernel BUILD=$(BUILD)/check/debug LIMINE_DIR=$(LIMINE_DIR) KERNEL_CFLAGS_EXTRA=-DTUNIX_DEBUG_LOGS=1
	$(MAKE) kernel BUILD=$(BUILD)/check/timings LIMINE_DIR=$(LIMINE_DIR) KERNEL_CFLAGS_EXTRA=-DTUNIX_BOOT_TIMINGS=1

$(KERNEL): $(KERNEL_OBJECTS) kernel/arch/x86_64/linker.ld
	$(CC) $(KERNEL_LDFLAGS) $(KERNEL_OBJECTS) -o $@

$(BUILD)/%.c.o: %.c $(TERMINAL_FONT_DATA) | $(LIMINE_HEADER)
	@mkdir -p $(dir $@)
	$(CC) $(KERNEL_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/%.S.o: %.S | $(LIMINE_HEADER)
	@mkdir -p $(dir $@)
	$(CC) $(KERNEL_CFLAGS) -MMD -MP -c $< -o $@

$(TERMINAL_FONT_DATA): $(TERMINAL_FONT_SOURCE) support/terminal-font.py
	@mkdir -p $(dir $@)
	$(PYTHON) support/terminal-font.py $< $@

$(KERNEL_OBJECTS): GNUmakefile

-include $(KERNEL_DEPS)

$(LIMINE_HEADER):
	rm -rf $(LIMINE_DIR)
	git clone --depth=1 --branch=$(LIMINE_VERSION) \
		https://github.com/limine-bootloader/limine.git $(LIMINE_DIR)

$(LIMINE_EXE): $(LIMINE_HEADER)
	$(MAKE) -C $(LIMINE_DIR)

clean:
	rm -rf $(BUILD)/kernel $(BUILD)/generated $(KERNEL) $(IMAGE)

distclean:
	rm -rf $(BUILD)

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

VOID_INSTALL_GRAPHICAL ?= weston mesa-dri xorg-server-xwayland xkeyboard-config dejavu-fonts-ttf \
	seatd xcursor-vanilla-dmz

VOID_INSTALL_BROWSER ?= firefox

VOID_INSTALL_TOOLCHAIN ?= gcc make python3 python3-Pillow git binutils mtools

VOID_INSTALL_GAMES ?= supertuxkart

VOID_REMOVE ?=

BASE_FILES := $(shell find base-files -type f 2>/dev/null)

.PHONY: sysroot
sysroot: $(SYSROOT_STAMP)

SYSROOT_RECIPE := $(BUILD)/sysroot-recipe
$(shell mkdir -p $(BUILD); printf '%s\n' '$(VOID_MIRROR)' '$(VOID_ROOTFS_DATE)' \
	'$(VOID_INSTALL)' '$(VOID_REMOVE)' > $(SYSROOT_RECIPE).tmp; \
	cmp -s $(SYSROOT_RECIPE).tmp $(SYSROOT_RECIPE) 2>/dev/null \
		&& rm -f $(SYSROOT_RECIPE).tmp \
		|| mv $(SYSROOT_RECIPE).tmp $(SYSROOT_RECIPE))

$(SYSROOT_STAMP): support/sysroot.sh $(BASE_FILES) $(SYSROOT_RECIPE) | $(BUILD)
	VOID_MIRROR='$(VOID_MIRROR)' VOID_ROOTFS_DATE='$(VOID_ROOTFS_DATE)' \
	VOID_INSTALL='$(VOID_INSTALL)' VOID_REMOVE='$(VOID_REMOVE)' \
		support/sysroot.sh $(SYSROOT) $(CACHE)
	@touch $@

$(BUILD):
	@mkdir -p $@

IMAGE := $(BUILD)/tunix.img

.PHONY: image
image: $(IMAGE)

IMAGE_TABLE ?= gpt

IMAGE_SLACK_MIB ?= 4096

$(IMAGE): $(KERNEL) $(LIMINE_EXE) support/limine.conf support/image.sh $(SYSROOT_STAMP)
	TABLE='$(IMAGE_TABLE)' ROOT_SLACK_MIB='$(IMAGE_SLACK_MIB)' support/image.sh $@ $(KERNEL) $(LIMINE_DIR) support/limine.conf $(SYSROOT)

QEMU_MEMORY ?= 4G
QEMU_SMP    ?= 4
COMMA := ,
SUDO_RUNTIME := $(if $(SUDO_UID),/run/user/$(SUDO_UID))
PULSE_SOCKET := $(firstword $(wildcard /mnt/wslg/PulseServer \
	$(XDG_RUNTIME_DIR)/pulse/native $(SUDO_RUNTIME)/pulse/native))
PULSE_TUNING := $(COMMA)out.latency=100000$(COMMA)out.buffer-length=200000
QEMU_AUDIO_BACKEND ?= $(if $(PULSE_SOCKET),pa$(COMMA)server=$(PULSE_SOCKET)$(PULSE_TUNING),none)
AUDIO_NOTE = $(if $(PULSE_SOCKET),@echo ":: audio -> $(PULSE_SOCKET)",\
	@echo ":: audio -> nowhere: no PulseAudio socket found, so the machine will be silent." \
	; echo ":: run as yourself rather than under sudo, or set PULSE_SOCKET=/path/to/native")
QEMU_AUDIO  ?= -audiodev $(QEMU_AUDIO_BACKEND)$(COMMA)id=snd0 \
	-device intel-hda -device hda-output,audiodev=snd0
QEMU_NET    ?= -netdev user,id=net0 -device virtio-net-pci,disable-legacy=on,netdev=net0
QEMU_DISPLAY ?= -display gtk,grab-on-hover=on
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
	$(QEMU) $(QEMU_COMMON) $(QEMU_DISPLAY) -serial file:$(BUILD)/serial.log -monitor none

run-uefi: $(IMAGE) $(OVMF) $(OVMF_VARS)
	$(AUDIO_NOTE)
	rm -f $(BUILD)/serial.log
	$(QEMU) $(QEMU_COMMON) $(QEMU_DISPLAY) -serial file:$(BUILD)/serial.log -monitor none \
		-drive if=pflash,unit=0,format=raw,readonly=on,file=$(OVMF) \
		-drive if=pflash,unit=1,format=raw,file=$(OVMF_VARS)

QEMU_GPU ?= -vga none -device virtio-vga,xres=1280,yres=720 \
	-display gtk,zoom-to-fit=on,grab-on-hover=on
run-gpu: $(IMAGE)
	$(AUDIO_NOTE)
	rm -f $(BUILD)/serial.log
	$(QEMU) $(QEMU_COMMON) $(QEMU_GPU) -serial file:$(BUILD)/serial.log -monitor none

QEMU_VIRGL ?= -vga none -device virtio-vga-gl,xres=1280,yres=720 \
	-display sdl,gl=on
QEMU_GL_ENV ?= $(if $(wildcard /dev/dri),,\
	$(if $(wildcard /usr/lib/wsl/lib),env LD_LIBRARY_PATH=/usr/lib/wsl/lib GALLIUM_DRIVER=d3d12))
run-virgl: $(IMAGE)
	$(AUDIO_NOTE)
	rm -f $(BUILD)/serial.log
	$(QEMU_GL_ENV) $(QEMU) $(QEMU_COMMON) $(QEMU_VIRGL) \
		-serial file:$(BUILD)/serial.log -monitor none

headless: $(IMAGE)
	$(AUDIO_NOTE)
	$(QEMU) $(QEMU_COMMON) -nographic -monitor none -serial stdio

SCHEDBENCH_CPUS ?= 4

.PHONY: schedbench drmtest perftest inputtest soundtest testimage
schedbench: $(KERNEL) $(LIMINE_EXE)
	support/tests/schedbench.sh $(SCHEDBENCH_CPUS) $(KERNEL)

drmtest: $(KERNEL) $(LIMINE_EXE)
	support/tests/drmtest.sh $(SCHEDBENCH_CPUS) $(KERNEL)

perftest: $(KERNEL) $(LIMINE_EXE)
	support/tests/perftest.sh $(SCHEDBENCH_CPUS) $(KERNEL)

inputtest: $(KERNEL) $(LIMINE_EXE)
	support/tests/inputtest.sh $(SCHEDBENCH_CPUS) $(KERNEL)

soundtest: $(KERNEL) $(LIMINE_EXE)
	support/tests/soundtest.sh $(SCHEDBENCH_CPUS) $(KERNEL)

TEST ?= schedbench
testimage: $(KERNEL) $(LIMINE_EXE)
	BOOT=0 IMAGE_TABLE='$(IMAGE_TABLE)' support/tests/$(TEST).sh $(SCHEDBENCH_CPUS) $(KERNEL)
