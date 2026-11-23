override MAKEFLAGS += -rR
COMMA := ,
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

include subprojects/uacpi/RECIPE
include subprojects/rust/RECIPE

KERNEL_CFLAGS := -std=gnu11 -Wall -Wextra -Werror -ffreestanding \
	-fstack-protector-strong -mstack-protector-guard=global -fno-pic -fno-pie -fno-builtin \
	-fno-asynchronous-unwind-tables -fno-unwind-tables -mno-red-zone -m64 \
	-O2 -ffunction-sections -fdata-sections -mcmodel=kernel -mgeneral-regs-only \
	-Iinclude -I$(LIMINE_DIR) -I$(UACPI_DIR)/include \
	$(KERNEL_CFLAGS_EXTRA)
# The commit, for uname -v and /proc/version.
TUNIX_COMMIT := $(shell git rev-parse --short=12 HEAD 2>/dev/null)
ifneq ($(TUNIX_COMMIT),)
  VERSION_CFLAGS := -DTUNIX_COMMIT='"$(TUNIX_COMMIT)"'
endif

ifeq ($(FRAME_POINTERS),1)
  DEBUG_CFLAGS += -fno-omit-frame-pointer -mno-omit-leaf-frame-pointer -DTUNIX_FRAME_POINTERS=1
endif

ifeq ($(KASAN),1)
  DEBUG_CFLAGS += -DTUNIX_KASAN=1
  SANITIZE_CFLAGS += -fsanitize=kernel-address --param asan-stack=0 --param asan-globals=0 \
	--param asan-instrumentation-with-call-threshold=0
endif

SANITIZE_EXCLUDE := arch/% mm/heap.c mm/pmm.c mm/vmm.c mm/kasan.c mm/usercopy.c mm/dma.c \
	lib/kstring.c lib/kprintf.c core/oops.c core/percpu.c \
	init/stack_guard.c
sanitize-flags = $(if $(filter $(SANITIZE_EXCLUDE),$(1)),,$(SANITIZE_CFLAGS))

KERNEL_CFLAGS += $(DEBUG_CFLAGS)
KERNEL_LDFLAGS := -nostdlib -no-pie -Wl,-T,arch/x86_64/linker.ld \
	-Wl,--gc-sections -Wl,--build-id=none -Wl,-z,max-page-size=0x1000

SOURCE_PRUNE := -path ./subprojects -prune -o -path ./build -prune -o \
	-path ./arch/x86_64/bzimage -prune -o \
	-path ./rust -prune -o
find-sources = $(patsubst ./%,%,$(shell find . $1 $(SOURCE_PRUNE) \( -name '*.c' -o -name '*.S' \) -print))
# A driver is a module when its source says so with MODULE_MAIN(); the kernel image
# leaves it out and it is built as a .ko instead.
MODULE_SOURCES := $(shell grep -rl --include='*.c' '^MODULE_MAIN\b' drivers)
KERNEL_SOURCES := $(filter-out $(MODULE_SOURCES),$(call find-sources,-path ./arch/aarch64 -prune -o))
KERNEL_OBJECTS := $(KERNEL_SOURCES:%=$(BUILD)/kernel/%.o)
KERNEL_DEPS    := $(KERNEL_OBJECTS:.o=.d)

.PHONY: all kernel fetch check clean distclean
all: kernel modules vmlinuz
kernel: $(KERNEL)
fetch: $(LIMINE_HEADER) $(UACPI_STAMP) $(RUST_STAMP)

check:
	$(MAKE) kernel modules vmlinuz BUILD=$(BUILD)/check/default LIMINE_DIR=$(LIMINE_DIR) UACPI_DIR=$(UACPI_DIR) RUST_HOME=$(RUST_HOME)
	$(MAKE) kernel modules vmlinuz BUILD=$(BUILD)/check/debug LIMINE_DIR=$(LIMINE_DIR) UACPI_DIR=$(UACPI_DIR) RUST_HOME=$(RUST_HOME) KERNEL_CFLAGS_EXTRA=-DTUNIX_DEBUG_LOGS=1
	$(MAKE) kernel modules vmlinuz BUILD=$(BUILD)/check/timings LIMINE_DIR=$(LIMINE_DIR) UACPI_DIR=$(UACPI_DIR) RUST_HOME=$(RUST_HOME) KERNEL_CFLAGS_EXTRA=-DTUNIX_BOOT_TIMINGS=1

NM ?= nm

$(KERNEL): $(KERNEL_OBJECTS) $(UACPI_OBJECTS) $(RUST_X86_LIB) arch/x86_64/linker.ld
	$(CC) $(KERNEL_LDFLAGS) $(KERNEL_OBJECTS) $(UACPI_OBJECTS) $(RUST_X86_LIB) -o $@.tmp
	$(call rust_panic_free,$(NM),$@.tmp)
	mv $@.tmp $@

$(BUILD)/kernel/%.c.o: %.c | $(LIMINE_HEADER) $(UACPI_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(KERNEL_CFLAGS) $(call sanitize-flags,$<) -MMD -MP -c $< -o $@

$(BUILD)/kernel/%.S.o: %.S | $(LIMINE_HEADER) $(UACPI_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(KERNEL_CFLAGS) -MMD -MP -c $< -o $@

MODULES := $(patsubst drivers/%.c,$(BUILD)/modules/%.ko,$(MODULE_SOURCES))
MODULE_CFLAGS = $(filter-out -ffunction-sections -fdata-sections,$(KERNEL_CFLAGS)) \
	-DTUNIX_MODULE_NAME='"$(notdir $*)"'

# Every symbol a module leaves undefined has to be in core/ksyms.c or exported by a
# module; the loader would otherwise refuse it only when something loads it.
check-module-symbols = export LC_ALL=C; known=$$(sed -n 's/^ *EXPORT(\([A-Za-z0-9_]*\)),.*/\1/p' core/ksyms.c; \
	$(1) $(2) | sed -n 's/.* __ksymtab_\(.*\)/\1/p'); bad=0; \
	for ko in $(2); do for symbol in $$($(1) -u $$ko | awk '{print $$2}'); do \
	printf '%s\n' "$$known" | grep -qxF "$$symbol" || \
	{ echo "$$ko: $$symbol is not exported to modules"; bad=1; }; done; done; [ $$bad = 0 ]

# vmlinuz: the kernel wrapped for the Linux x86 boot protocol and as an EFI application,
# so that loaders other than Limine can start it. See arch/x86_64/bzimage.
OBJCOPY ?= objcopy
BZIMAGE_DIR := arch/x86_64/bzimage
BZIMAGE_BUILD := $(BUILD)/bzimage
BZIMAGE_OBJECTS := $(addprefix $(BZIMAGE_BUILD)/,header.o head.o stub.o blob.o)
BZIMAGE_CFLAGS := -std=gnu11 -Wall -Wextra -Werror -ffreestanding -fno-builtin -m64 -O2 \
	-fpie -fno-stack-protector -fno-jump-tables -fno-tree-loop-distribute-patterns \
	-mno-red-zone -mgeneral-regs-only -masm=intel -fno-asynchronous-unwind-tables \
	-fno-unwind-tables -Iinclude

.PHONY: vmlinuz
vmlinuz: $(BUILD)/vmlinuz

$(BUILD)/kernel.bin: $(KERNEL)
	$(OBJCOPY) -O binary $< $@

$(BZIMAGE_BUILD)/kernel-symbols.ld: $(KERNEL)
	@mkdir -p $(dir $@)
	$(NM) $< | awk '$$3 == "stub_kernel_entry" || $$3 == "kernel_image_start" || \
		$$3 == "kernel_reserve_end" { print $$3 " = 0x" $$1 ";" }' > $@.tmp
	@[ $$(wc -l < $@.tmp) -eq 3 ] || { echo "$(KERNEL) lacks the symbols vmlinuz needs"; exit 1; }
	mv $@.tmp $@

$(BZIMAGE_BUILD)/blob.o: $(BZIMAGE_DIR)/blob.S $(BUILD)/kernel.bin
	@mkdir -p $(dir $@)
	$(CC) -c -DKERNEL_BIN='"$(abspath $(BUILD)/kernel.bin)"' $< -o $@

$(BZIMAGE_BUILD)/%.o: $(BZIMAGE_DIR)/%.S
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@

$(BZIMAGE_BUILD)/%.o: $(BZIMAGE_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(BZIMAGE_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/vmlinuz.elf: $(BZIMAGE_OBJECTS) $(BZIMAGE_DIR)/bzimage.ld $(BZIMAGE_BUILD)/kernel-symbols.ld
	$(CC) -nostdlib -static -no-pie -Wl,--build-id=none -Wl,-L,$(BZIMAGE_BUILD) \
		-Wl,-T,$(BZIMAGE_DIR)/bzimage.ld -o $@ $(BZIMAGE_OBJECTS)

$(BUILD)/vmlinuz: $(BUILD)/vmlinuz.elf
	$(OBJCOPY) -O binary $< $@

$(BZIMAGE_OBJECTS): GNUmakefile

-include $(BZIMAGE_BUILD)/stub.d

.PHONY: modules
modules: $(BUILD)/modules/symbols.ok

$(BUILD)/modules/symbols.ok: $(MODULES) core/ksyms.c
	@$(call check-module-symbols,$(NM),$(MODULES))
	@touch $@

$(BUILD)/modules/%.ko: drivers/%.c | $(LIMINE_HEADER) $(UACPI_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(MODULE_CFLAGS) -MMD -MP -c $< -o $@

$(MODULES) $(KERNEL_OBJECTS) $(UACPI_OBJECTS): GNUmakefile $(UACPI_RECIPE)

-include $(MODULES:.ko=.d)
-include $(KERNEL_DEPS)

$(LIMINE_HEADER):
	rm -rf $(LIMINE_DIR)
	git clone --depth=1 --branch=$(LIMINE_VERSION) \
		https://github.com/limine-bootloader/limine.git $(LIMINE_DIR)

clean:
	rm -rf $(BUILD)/kernel $(BUILD)/modules $(BUILD)/uacpi-objects $(BUILD)/rust-target $(KERNEL) \
		$(BUILD)/bzimage $(BUILD)/kernel.bin $(BUILD)/vmlinuz.elf $(BUILD)/vmlinuz

distclean:
	rm -rf $(BUILD)


AARCH64_CC      ?= aarch64-linux-gnu-gcc
AARCH64_OBJCOPY ?= aarch64-linux-gnu-objcopy
AARCH64_NM      ?= aarch64-linux-gnu-nm

AARCH64_CORE_BUILD := $(BUILD)/aarch64-core
AARCH64_CORE_CFLAGS := -std=gnu11 -Wall -Wextra -Werror -ffreestanding \
	-fstack-protector-strong -mstack-protector-guard=global -fno-pic -fno-pie -fno-builtin \
	-fno-asynchronous-unwind-tables -fno-unwind-tables -mgeneral-regs-only \
	-mstrict-align -march=armv8-a -mno-outline-atomics -Os \
	-ffunction-sections -fdata-sections \
	-Iinclude $(KERNEL_CFLAGS_EXTRA) $(DEBUG_CFLAGS)
# x86-only drivers: port I/O, or ACPI, which the aarch64 build does not have.
AARCH64_SKIP := drivers/storage/ata.c drivers/net/rtl8139.c \
	$(filter-out drivers/acpi/acpi.c,$(wildcard drivers/acpi/*.c))
AARCH64_CORE_EXCLUDE := $(AARCH64_SKIP) $(MODULE_SOURCES)
AARCH64_CORE_SOURCES := $(filter-out $(AARCH64_CORE_EXCLUDE),$(call find-sources,-path ./arch -prune -o)) \
	$(shell find arch/aarch64 \( -name '*.c' -o -name '*.S' \) -print)
AARCH64_CORE_OBJECTS := $(AARCH64_CORE_SOURCES:%=$(AARCH64_CORE_BUILD)/kernel/%.o)
AARCH64_CORE_KERNEL := $(BUILD)/kernel-aarch64-core.elf
AARCH64_CORE_IMAGE := $(BUILD)/kernel-aarch64-core.img

.PHONY: aarch64-core
aarch64-core: $(AARCH64_CORE_IMAGE)

$(AARCH64_CORE_KERNEL): $(AARCH64_CORE_OBJECTS) $(RUST_ARM_LIB) arch/aarch64/linker.ld
	$(AARCH64_CC) -nostdlib -static -Wl,-T,arch/aarch64/linker.ld -Wl,--gc-sections \
		-Wl,--build-id=none -Wl,-z,max-page-size=0x1000 \
		$(AARCH64_CORE_OBJECTS) $(RUST_ARM_LIB) -lgcc -o $@.tmp
	$(call rust_panic_free,$(AARCH64_NM),$@.tmp)
	mv $@.tmp $@

$(AARCH64_CORE_IMAGE): $(AARCH64_CORE_KERNEL)
	$(AARCH64_OBJCOPY) -O binary $< $@

$(AARCH64_CORE_BUILD)/kernel/%.c.o: %.c
	@mkdir -p $(dir $@)
	$(AARCH64_CC) $(AARCH64_CORE_CFLAGS) $(call sanitize-flags,$<) -MMD -MP -c $< -o $@

# Only the files that print the version are rebuilt when the commit changes.
VERSIONED_OBJECTS := kernel/core/syscall.c.o kernel/fs/procfs.c.o
$(addprefix $(BUILD)/,$(VERSIONED_OBJECTS)): KERNEL_CFLAGS += $(VERSION_CFLAGS)
$(addprefix $(AARCH64_CORE_BUILD)/,$(VERSIONED_OBJECTS)): AARCH64_CORE_CFLAGS += $(VERSION_CFLAGS)

$(AARCH64_CORE_BUILD)/kernel/%.S.o: %.S
	@mkdir -p $(dir $@)
	$(AARCH64_CC) $(AARCH64_CORE_CFLAGS) -MMD -MP -c $< -o $@

$(AARCH64_CORE_OBJECTS): GNUmakefile

-include $(AARCH64_CORE_OBJECTS:.o=.d)

AARCH64_MODULE_SOURCES := $(filter-out $(AARCH64_SKIP),$(MODULE_SOURCES))
AARCH64_MODULES := $(patsubst drivers/%.c,$(AARCH64_CORE_BUILD)/modules/%.ko,$(AARCH64_MODULE_SOURCES))
AARCH64_MODULE_CFLAGS = $(AARCH64_CORE_CFLAGS) -DTUNIX_MODULE_NAME='"$(notdir $*)"'

.PHONY: modules-aarch64
modules-aarch64: $(AARCH64_CORE_BUILD)/modules/symbols.ok

$(AARCH64_CORE_BUILD)/modules/symbols.ok: $(AARCH64_MODULES) core/ksyms.c
	@$(call check-module-symbols,$(AARCH64_NM),$(AARCH64_MODULES))
	@touch $@

$(AARCH64_CORE_BUILD)/modules/%.ko: drivers/%.c
	@mkdir -p $(dir $@)
	$(AARCH64_CC) $(AARCH64_MODULE_CFLAGS) -MMD -MP -c $< -o $@

$(AARCH64_MODULES): GNUmakefile

-include $(AARCH64_MODULES:.ko=.d)

