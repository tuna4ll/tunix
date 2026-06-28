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
BUSYBOX := $(PORT_OUT)/busybox
TCC_ROOT := $(PORT_OUT)/tcc-root
TCC_STAMP := $(PORT_OUT)/.tcc-ready
NCURSES_ROOT := $(PORT_OUT)/ncurses-root
NCURSES_STAMP := $(PORT_OUT)/.ncurses-ready
NANO := $(PORT_OUT)/nano

COMMON_CFLAGS := -std=gnu11 -Wall -Wextra -Werror -ffreestanding -fno-stack-protector \
	-fno-pic -fno-pie -fno-builtin -fno-asynchronous-unwind-tables -fno-unwind-tables \
	-mno-red-zone -m64 -Os -ffunction-sections -fdata-sections
KERNEL_CFLAGS := $(COMMON_CFLAGS) -mcmodel=kernel -Isrc/kernel/include -Isrc/include
KERNEL_LDFLAGS := -nostdlib -no-pie -Wl,-T,src/kernel/arch/x86_64/linker.ld \
	-Wl,--gc-sections -Wl,--build-id=none -Wl,-z,max-page-size=0x1000
USER_CFLAGS := $(COMMON_CFLAGS) -mcmodel=small -Isrc/libc/include -Isrc/include
USER_LDFLAGS := -nostdlib -static -T src/userspace/linker.ld --build-id=none \
	-z max-page-size=0x1000 --gc-sections

KERNEL_OBJS := \
	$(BUILD)/entry.o $(BUILD)/main.o $(BUILD)/serial.o \
	$(BUILD)/kprintf.o $(BUILD)/kstring.o $(BUILD)/gdt.o \
	$(BUILD)/idt.o $(BUILD)/isr.o $(BUILD)/isr_handler.o \
	$(BUILD)/pmm.o $(BUILD)/vmm.o $(BUILD)/framebuffer.o $(BUILD)/terminal_font.o $(BUILD)/terminal.o $(BUILD)/input.o \
	$(BUILD)/heap.o $(BUILD)/syscall.o $(BUILD)/syscall_entry.o \
	$(BUILD)/vfs.o $(BUILD)/tarfs.o $(BUILD)/devfs.o $(BUILD)/unix_socket.o $(BUILD)/pty.o \
	$(BUILD)/usercopy.o $(BUILD)/elf.o $(BUILD)/file.o \
	$(BUILD)/pipe.o $(BUILD)/tty.o $(BUILD)/process.o $(BUILD)/procfs.o $(BUILD)/time.o $(BUILD)/random.o $(BUILD)/ata.o

USER_RUNTIME := $(BUILD)/user/crt0.o $(BUILD)/user/libc.o $(BUILD)/user/sigreturn.o
INIT := $(BUILD)/user/init
PROCUTIL := $(BUILD)/user/procutil.o
SYSTEM_TOOLS := $(BUILD)/user/ps $(BUILD)/user/free $(BUILD)/user/uptime $(BUILD)/user/top
BUSYBOX_APPLETS := awk basename cat chmod clear cp cut date dd dirname du echo egrep env expr false \
	fgrep find grep head id ls md5sum mkdir mv printenv printf pwd readlink realpath rm \
	rmdir sed seq sha256sum sort stat tail tee test touch tr true uname uniq wc which xargs yes hwclock
INITRD_FILES := $(shell find initrd -type f 2>/dev/null)
WALLPAPER_SOURCE ?= assets/tunix-mountain-lake.jpg
WALLPAPER_OUTPUT := initrd/usr/share/tunix/wallpaper.twl

.PHONY: all run headless wallpaper editor-check editor-qemu-check clean
all: $(IMAGE)

wallpaper: $(WALLPAPER_OUTPUT)

$(WALLPAPER_OUTPUT): $(WALLPAPER_SOURCE) scripts/convert-wallpaper.py
	$(PYTHON) scripts/convert-wallpaper.py $(WALLPAPER_SOURCE) $(WALLPAPER_OUTPUT) --width 960 --height 540

$(BUILD)/.tools:
	@mkdir -p $(BUILD)
	@for tool in $(CC) $(LD) $(NASM) $(STRIP) $(AR) $(PYTHON) make tar; do \
		command -v $$tool >/dev/null 2>&1 || { echo "missing required tool: $$tool" >&2; exit 1; }; \
	done
	@touch $@


$(BASH): ports/build-bash.sh | $(BUILD)/.tools
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" ./ports/build-bash.sh

$(BUSYBOX): $(BASH) ports/build-busybox.sh | $(BUILD)/.tools
	OUT="$(abspath $(PORT_OUT))" ./ports/build-busybox.sh

$(NCURSES_STAMP): $(BASH) ports/build-ncurses.sh ports/terminfo/tunix.ti | $(BUILD)/.tools
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" ./ports/build-ncurses.sh
	@touch $@

$(NANO): $(NCURSES_STAMP) ports/build-nano.sh | $(BUILD)/.tools
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" ./ports/build-nano.sh

$(TCC_STAMP): ports/build-tcc.sh | $(BUILD)/.tools
	@mkdir -p $(PORT_OUT)
	OUT="$(abspath $(PORT_OUT))" ./ports/build-tcc.sh
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

$(BUILD)/main.o: src/kernel/include/input.h src/kernel/include/tty.h
$(BUILD)/input.o: src/kernel/include/input.h src/kernel/include/io.h src/kernel/include/tty.h
$(BUILD)/devfs.o: src/kernel/include/vfs.h src/kernel/include/pty.h src/kernel/include/random.h src/kernel/include/time.h src/kernel/include/ata.h src/kernel/include/klog.h src/kernel/include/input.h
$(BUILD)/unix_socket.o: src/kernel/include/unix_socket.h src/kernel/include/pipe.h
$(BUILD)/pty.o: src/kernel/include/pty.h src/kernel/include/tty.h src/kernel/include/file.h
$(BUILD)/file.o: src/kernel/include/file.h src/kernel/include/vfs.h src/kernel/include/pty.h
$(BUILD)/syscall.o: src/kernel/include/vfs.h src/kernel/include/tty.h src/kernel/include/pty.h src/kernel/include/process.h src/kernel/include/random.h src/kernel/include/time.h
$(BUILD)/tty.o: src/kernel/include/input.h src/kernel/include/tty.h src/include/tunix/keymap.h
$(BUILD)/process.o: src/kernel/include/process.h src/kernel/include/signal.h
$(BUILD)/random.o: src/kernel/include/random.h src/kernel/include/time.h src/kernel/include/spinlock.h
$(BUILD)/time.o: src/kernel/include/time.h src/kernel/include/io.h
$(BUILD)/ata.o: src/kernel/include/ata.h src/kernel/include/io.h
$(BUILD)/kprintf.o: src/kernel/include/klog.h

$(BUILD)/kprintf.o: src/kernel/lib/kprintf.c | $(BUILD)
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kstring.o: src/kernel/lib/kstring.c | $(BUILD)
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/gdt.o: src/kernel/arch/x86_64/gdt.c | $(BUILD)
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/idt.o: src/kernel/arch/x86_64/idt.c | $(BUILD)
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/isr_handler.o: src/kernel/arch/x86_64/isr_handler.c | $(BUILD)
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

$(BUILD)/user/ps $(BUILD)/user/free $(BUILD)/user/uptime $(BUILD)/user/top: $(BUILD)/user/%: $(BUILD)/user/%.o $(PROCUTIL) $(USER_RUNTIME) src/userspace/linker.ld
	$(LD) $(USER_LDFLAGS) -o $@ $(USER_RUNTIME) $(PROCUTIL) $(BUILD)/user/$*.o
	$(STRIP) --strip-all $@

$(INIT): $(BUILD)/user/init.o $(USER_RUNTIME) src/userspace/linker.ld
	$(LD) $(USER_LDFLAGS) -o $@ $(USER_RUNTIME) $(BUILD)/user/init.o
	$(STRIP) --strip-all $@

$(INITRAMFS): $(INIT) $(SYSTEM_TOOLS) $(BASH) $(BUSYBOX) $(TCC_STAMP) $(NANO) $(WALLPAPER_OUTPUT) $(INITRD_FILES)
	rm -rf $(ROOTFS)
	mkdir -p $(ROOTFS)/bin $(ROOTFS)/sbin $(ROOTFS)/dev $(ROOTFS)/tmp $(ROOTFS)/home
	cp -R initrd/. $(ROOTFS)/
	cp $(INIT) $(ROOTFS)/sbin/init
	cp $(BASH) $(ROOTFS)/bin/bash
	cp $(BUSYBOX) $(ROOTFS)/bin/busybox
	cp $(NANO) $(ROOTFS)/bin/nano
	cp $(SYSTEM_TOOLS) $(ROOTFS)/bin/
	cp -R $(TCC_ROOT)/. $(ROOTFS)/
	mkdir -p $(ROOTFS)/usr/share
	cp -R $(NCURSES_ROOT)/usr/share/terminfo $(ROOTFS)/usr/share/
	mkdir -p $(ROOTFS)/usr/share/nano
	cp ports/src/nano/syntax/*.nanorc $(ROOTFS)/usr/share/nano/
	@test -x $(ROOTFS)/usr/bin/tcc || { echo "TinyCC was not installed into the rootfs" >&2; exit 1; }
	ln -sfn ../usr/bin/tcc $(ROOTFS)/bin/tcc
	chmod 0755 $(ROOTFS)/sbin/init $(ROOTFS)/bin/bash $(ROOTFS)/bin/busybox $(ROOTFS)/bin/nano \
		$(ROOTFS)/bin/neofetch $(ROOTFS)/bin/ps $(ROOTFS)/bin/free \
		$(ROOTFS)/bin/uptime $(ROOTFS)/bin/top \
		$(ROOTFS)/usr/bin/tcc
	ln -s bash $(ROOTFS)/bin/sh
	@for app in $(BUSYBOX_APPLETS); do ln -s busybox $(ROOTFS)/bin/$$app; done
	tar --format=ustar --blocking-factor=1 --sort=name --mtime=@0 --owner=0 --group=0 --numeric-owner -cf $@ -C $(ROOTFS) .

$(IMAGE): $(BUILD)/stage1.bin $(BUILD)/stage2.bin $(KERNEL) $(INITRAMFS) scripts/build-image.py
	$(PYTHON) scripts/build-image.py $@ $(BUILD)/stage1.bin $(BUILD)/stage2.bin $(KERNEL) $(INITRAMFS)

run: $(IMAGE)
	rm -f $(BUILD)/serial.log
	$(QEMU) -machine pc -m 128M -drive format=raw,file=$(IMAGE) \
		-serial file:$(BUILD)/serial.log -monitor none -no-reboot -no-shutdown

headless: $(IMAGE)
	$(QEMU) -machine pc -m 128M -drive format=raw,file=$(IMAGE) \
		-nographic -monitor none -serial stdio -no-reboot -no-shutdown

editor-check:
	ITERATIONS=20 ./scripts/test-editor-ports.sh

editor-qemu-check: $(IMAGE)
	$(PYTHON) scripts/nano-qemu-smoke.py $(IMAGE) --qemu $(QEMU) --iterations 20

clean:
	rm -rf $(BUILD) $(PORT_OUT)
	@echo "Clean complete."
