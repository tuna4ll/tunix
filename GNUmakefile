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
all: kernel
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

-include $(KERNEL_DEPS)

# --- limine -----------------------------------------------------------------

$(LIMINE_HEADER):
	rm -rf $(LIMINE_DIR)
	git clone --depth=1 --branch=$(LIMINE_VERSION) \
		https://github.com/limine-bootloader/limine.git $(LIMINE_DIR)

$(LIMINE_EXE): $(LIMINE_HEADER)
	$(MAKE) -C $(LIMINE_DIR)

clean:
	rm -rf $(BUILD)/kernel $(BUILD)/generated $(KERNEL)

distclean:
	rm -rf $(BUILD)
