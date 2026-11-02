override MAKEFLAGS += -rR
.DEFAULT_GOAL := all

-include local.mk

ifneq ($(filter default undefined,$(origin CC)),)
  override CC := cc
endif

BUILD  := build
KERNEL := $(BUILD)/kernel.elf

LIMINE_VERSION := v9.x-binary
LIMINE_DIR     := $(BUILD)/limine
LIMINE_HEADER  := $(LIMINE_DIR)/limine.h

# Kernel configuration from ../Kconfig: tools/genconfig.sh materializes a
# canonical .config plus a force-included CONFIG_* header per build tree.
# Paths are baked at parse time on purpose: each arch keeps its own config
# directory, and the x86 tree (including `check` variants, which override
# BUILD) gets its own as well.
KCONFIG    ?= ../Kconfig
GENCONFIG  ?= ../tools/genconfig.sh

CONFIG_DIR        := $(BUILD)/config
CONFIG_FILE       := $(CONFIG_DIR)/.config
CONFIG_GEN_H      := $(CONFIG_DIR)/kernel_config.h
CONFIG_INCLUDE    ?= -include $(CONFIG_GEN_H)

$(CONFIG_FILE): $(KCONFIG) $(GENCONFIG) $(wildcard ../.config)
	@sh $(GENCONFIG) $(KCONFIG) $(CONFIG_DIR) --arch x86_64

$(CONFIG_GEN_H): $(CONFIG_FILE) $(KCONFIG) $(GENCONFIG) $(wildcard ../.config)
	@sh $(GENCONFIG) $(KCONFIG) $(CONFIG_DIR) --arch x86_64

include subprojects/uacpi/RECIPE
include subprojects/rust/RECIPE

KERNEL_CFLAGS := -std=gnu11 -Wall -Wextra -Werror -ffreestanding \
	-fstack-protector-strong -mstack-protector-guard=global -fno-pic -fno-pie -fno-builtin \
	-fno-asynchronous-unwind-tables -fno-unwind-tables -mno-red-zone -m64 \
	-Os -ffunction-sections -fdata-sections -mcmodel=kernel -mgeneral-regs-only \
	-Iinclude -I$(LIMINE_DIR) -I$(UACPI_DIR)/include $(CONFIG_INCLUDE) \
	$(KERNEL_CFLAGS_EXTRA)
KERNEL_LDFLAGS := -nostdlib -no-pie -Wl,-T,arch/x86_64/linker.ld \
	-Wl,--gc-sections -Wl,--build-id=none -Wl,-z,max-page-size=0x1000

SOURCE_PRUNE := -path ./modules -prune -o -path ./subprojects -prune -o -path ./build -prune -o \
	-path ./rust -prune -o
find-sources = $(patsubst ./%,%,$(shell find . $1 $(SOURCE_PRUNE) \( -name '*.c' -o -name '*.S' \) -print))
KERNEL_SOURCES := $(call find-sources,-path ./arch/aarch64 -prune -o)
KERNEL_OBJECTS := $(KERNEL_SOURCES:%=$(BUILD)/kernel/%.o)
KERNEL_DEPS    := $(KERNEL_OBJECTS:.o=.d)

.PHONY: all kernel fetch check clean distclean
all: kernel modules
kernel: $(KERNEL)
fetch: $(LIMINE_HEADER) $(UACPI_STAMP) $(RUST_STAMP)

check:
	$(MAKE) kernel modules BUILD=$(BUILD)/check/default LIMINE_DIR=$(LIMINE_DIR) UACPI_DIR=$(UACPI_DIR) RUST_HOME=$(RUST_HOME)
	$(MAKE) kernel modules BUILD=$(BUILD)/check/debug LIMINE_DIR=$(LIMINE_DIR) UACPI_DIR=$(UACPI_DIR) RUST_HOME=$(RUST_HOME) KERNEL_CFLAGS_EXTRA=-DTUNIX_DEBUG_LOGS=1
	$(MAKE) kernel modules BUILD=$(BUILD)/check/timings LIMINE_DIR=$(LIMINE_DIR) UACPI_DIR=$(UACPI_DIR) RUST_HOME=$(RUST_HOME) KERNEL_CFLAGS_EXTRA=-DTUNIX_BOOT_TIMINGS=1

$(KERNEL): $(KERNEL_OBJECTS) $(UACPI_OBJECTS) $(RUST_X86_LIB) arch/x86_64/linker.ld
	$(CC) $(KERNEL_LDFLAGS) $(KERNEL_OBJECTS) $(UACPI_OBJECTS) $(RUST_X86_LIB) -o $@.tmp
	$(call rust_panic_free,nm,$@.tmp)
	mv $@.tmp $@

$(BUILD)/kernel/%.c.o: %.c | $(LIMINE_HEADER) $(UACPI_STAMP) $(CONFIG_GEN_H)
	@mkdir -p $(dir $@)
	$(CC) $(KERNEL_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/kernel/%.S.o: %.S | $(LIMINE_HEADER) $(UACPI_STAMP) $(CONFIG_GEN_H)
	@mkdir -p $(dir $@)
	$(CC) $(KERNEL_CFLAGS) -MMD -MP -c $< -o $@

MODULE_SOURCES := $(wildcard modules/*.c) $(wildcard modules/x86_64/*.c)
MODULES := $(patsubst modules/%.c,$(BUILD)/modules/%.ko,$(MODULE_SOURCES))
MODULE_CFLAGS = $(filter-out -ffunction-sections -fdata-sections,$(KERNEL_CFLAGS)) \
	-DTUNIX_MODULE_NAME='"$(notdir $*)"'

.PHONY: modules
modules: $(MODULES)

$(BUILD)/modules/%.ko: modules/%.c | $(LIMINE_HEADER) $(UACPI_STAMP) $(CONFIG_GEN_H)
	@mkdir -p $(dir $@)
	$(CC) $(MODULE_CFLAGS) -MMD -MP -c $< -o $@

$(MODULES) $(KERNEL_OBJECTS) $(UACPI_OBJECTS): GNUmakefile $(UACPI_RECIPE)

-include $(MODULES:.ko=.d)
-include $(KERNEL_DEPS)

$(LIMINE_HEADER):
	rm -rf $(LIMINE_DIR)
	git clone --depth=1 --branch=$(LIMINE_VERSION) \
		https://github.com/limine-bootloader/limine.git $(LIMINE_DIR)

IASL           ?= iasl
HOST_CC        ?= cc
ACPI_TEST      ?= ../testsuites/acpi-tests
ACPI_TEST_DIR  := $(BUILD)/acpi-test
ACPI_DRIVERS   := bus ec video button thermal_zone processor
ACPI_TEST_SRCS  = $(ACPI_TEST)/host.c $(ACPI_DRIVERS:%=drivers/acpi/%.c) $(UACPI_SOURCES)

.PHONY: acpi-test
acpi-test: $(UACPI_STAMP)
	@mkdir -p $(ACPI_TEST_DIR)
	$(IASL) -vw 3168 -p $(ACPI_TEST_DIR)/machine $(ACPI_TEST)/machine.asl >/dev/null
	$(HOST_CC) -std=gnu11 -O1 -Wall -Wno-unused-parameter -I$(ACPI_TEST)/stubs \
		-Iinclude -I$(UACPI_DIR)/include $(ACPI_TEST_SRCS) -o $(ACPI_TEST_DIR)/run
	$(ACPI_TEST_DIR)/run $(ACPI_TEST_DIR)/machine.aml

clean:
	rm -rf $(BUILD)/kernel $(BUILD)/modules $(BUILD)/uacpi-objects $(BUILD)/rust-target $(KERNEL)

distclean:
	rm -rf $(BUILD)


AARCH64_CC      ?= aarch64-linux-gnu-gcc
AARCH64_OBJCOPY ?= aarch64-linux-gnu-objcopy
AARCH64_NM      ?= aarch64-linux-gnu-nm

AARCH64_CORE_BUILD := $(BUILD)/aarch64-core
# The aarch64 build tree keeps its own configuration (ARCH choice = arm64).
AARCH64_CONFIG_DIR     := $(AARCH64_CORE_BUILD)/config
AARCH64_CONFIG_FILE    := $(AARCH64_CONFIG_DIR)/.config
AARCH64_CONFIG_GEN_H   := $(AARCH64_CONFIG_DIR)/kernel_config.h
AARCH64_CONFIG_INCLUDE ?= -include $(AARCH64_CONFIG_GEN_H)
AARCH64_CORE_CFLAGS := -std=gnu11 -Wall -Wextra -Werror -ffreestanding \
	-fstack-protector-strong -mstack-protector-guard=global -fno-pic -fno-pie -fno-builtin \
	-fno-asynchronous-unwind-tables -fno-unwind-tables -mgeneral-regs-only \
	-mstrict-align -march=armv8-a -mno-outline-atomics -Os \
	-ffunction-sections -fdata-sections \
	-Iinclude $(AARCH64_CONFIG_INCLUDE) $(KERNEL_CFLAGS_EXTRA)
AARCH64_CORE_EXCLUDE := drivers/storage/ata.c \
	$(filter-out drivers/acpi/acpi.c,$(wildcard drivers/acpi/*.c))
AARCH64_CORE_SOURCES := $(filter-out $(AARCH64_CORE_EXCLUDE),$(call find-sources,-path ./arch -prune -o)) \
	$(shell find arch/aarch64 \( -name '*.c' -o -name '*.S' \) -print)
AARCH64_CORE_OBJECTS := $(AARCH64_CORE_SOURCES:%=$(AARCH64_CORE_BUILD)/kernel/%.o)
AARCH64_CORE_KERNEL := $(BUILD)/kernel-aarch64-core.elf
AARCH64_CORE_IMAGE := $(BUILD)/kernel-aarch64-core.img

$(AARCH64_CONFIG_FILE) $(AARCH64_CONFIG_GEN_H): $(KCONFIG) $(GENCONFIG) $(wildcard ../.config)
	@sh $(GENCONFIG) $(KCONFIG) $(AARCH64_CONFIG_DIR) --arch arm64

.PHONY: aarch64-core
aarch64-core: $(AARCH64_CORE_IMAGE)
	@cp $(AARCH64_CORE_KERNEL) $(BUILD)/kernel-arm64.elf
	@if [ "$(ARM64_ONLY)" = "1" ]; then cp $(AARCH64_CORE_KERNEL) $(BUILD)/kernel.elf; fi

$(AARCH64_CORE_KERNEL): $(AARCH64_CORE_OBJECTS) $(RUST_ARM_LIB) arch/aarch64/linker.ld
	$(AARCH64_CC) -nostdlib -static -Wl,-T,arch/aarch64/linker.ld \
		-Wl,--gc-sections -Wl,--build-id=none -Wl,-z,max-page-size=0x1000 \
		$(AARCH64_CORE_OBJECTS) $(RUST_ARM_LIB) -lgcc -o $@.tmp
	$(call rust_panic_free,$(AARCH64_NM),$@.tmp)
	mv $@.tmp $@

$(AARCH64_CORE_IMAGE): $(AARCH64_CORE_KERNEL)
	$(AARCH64_OBJCOPY) -O binary $< $@

$(AARCH64_CORE_BUILD)/kernel/%.c.o: %.c | $(AARCH64_CONFIG_GEN_H)
	@mkdir -p $(dir $@)
	$(AARCH64_CC) $(AARCH64_CORE_CFLAGS) -MMD -MP -c $< -o $@

$(AARCH64_CORE_BUILD)/kernel/%.S.o: %.S | $(AARCH64_CONFIG_GEN_H)
	@mkdir -p $(dir $@)
	$(AARCH64_CC) $(AARCH64_CORE_CFLAGS) -MMD -MP -c $< -o $@

$(AARCH64_CORE_OBJECTS): GNUmakefile

-include $(AARCH64_CORE_OBJECTS:.o=.d)

AARCH64_MODULE_SOURCES := $(wildcard modules/*.c)
AARCH64_MODULES := $(patsubst modules/%.c,$(AARCH64_CORE_BUILD)/modules/%.ko,$(AARCH64_MODULE_SOURCES))
AARCH64_MODULE_CFLAGS = $(AARCH64_CORE_CFLAGS) -DTUNIX_MODULE_NAME='"$(notdir $*)"'

.PHONY: modules-aarch64
modules-aarch64: $(AARCH64_MODULES)

$(AARCH64_CORE_BUILD)/modules/%.ko: modules/%.c | $(AARCH64_CONFIG_GEN_H)
	@mkdir -p $(dir $@)
	$(AARCH64_CC) $(AARCH64_MODULE_CFLAGS) -MMD -MP -c $< -o $@

$(AARCH64_MODULES): GNUmakefile

-include $(AARCH64_MODULES:.ko=.d)

CLANG_TIDY ?= clang-tidy
NPROC      := $(shell nproc 2>/dev/null || echo 1)
LINT_FLAGS := -std=gnu11 -ffreestanding -m64 -mcmodel=kernel -mgeneral-regs-only \
	-mno-red-zone -Iinclude -I$(LIMINE_DIR) -I$(UACPI_DIR)/include $(CONFIG_INCLUDE)

.PHONY: lint
lint: rust-lint | $(LIMINE_HEADER) $(UACPI_STAMP) $(CONFIG_GEN_H)
	@printf '%s\n' $(filter %.c,$(KERNEL_SOURCES)) | \
		xargs -P$(NPROC) -I{} $(CLANG_TIDY) --quiet {} -- $(LINT_FLAGS)
	@printf '%s\n' $(MODULE_SOURCES) | \
		xargs -P$(NPROC) -I{} $(CLANG_TIDY) --quiet {} -- $(LINT_FLAGS) -DTUNIX_MODULE_NAME='"lint"'
