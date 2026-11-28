# Boot

Tunix is started by [Limine](https://github.com/limine-bootloader/limine), on
either BIOS or UEFI, from one disk image that carries both.

It used to have a bootloader of its own. That meant every change to the boot
contract was two projects wide, and the kernel could only be started from a disk
that loader had written -- no ISO, no USB stick somebody else prepared, no
firmware that only speaks EFI. Limine is a boot protocol rather than a program
to maintain: the kernel declares what it needs and gets it.

The menu is a `limine.conf` on the EFI system partition.

## The image

A bootable disk is GPT with two partitions:

| Partition | Contents |
| --- | --- |
| `sda1`, 64 MiB, FAT32, EFI system | `EFI/BOOT/BOOTX64.EFI`, `boot/limine/limine-bios.sys`, `boot/limine/limine.conf`, `boot/kernel.elf` |
| `sda2`, ext4 | the root filesystem |

The two firmwares take different paths into the same partition:

- **UEFI** loads `EFI/BOOT/BOOTX64.EFI`, which is Limine.
- **BIOS** runs the first stage that `limine bios-install` wrote into the gap
  between the protective MBR and the first partition, which is why `sda1`
  starts at sector 2048 rather than at 34. That stage finds
  `boot/limine/limine-bios.sys` on the ESP and continues.

Either way Limine reads `boot/limine/limine.conf`, loads `boot/kernel.elf`, and
enters it with the command line from the config.

## What the kernel asks for

`arch/x86_64/limine_entry.c` holds the request block. Limine finds it by
scanning the loaded image between two marker values, which is why the requests
sit in a section of their own that the linker script keeps.

| Request | What it is for |
| --- | --- |
| Memory map | Which physical pages the allocator may hand out |
| HHDM | Where all of memory is mapped, so `vmm_init` can reach a page table before the kernel's own direct map exists |
| Framebuffer | A linear framebuffer, its address, and its pixel format |
| Executable address | Where the image was loaded, physically and virtually |
| Command line | `root=`, `init=` |
| Module | The initramfs, `module_path` in `limine.conf` |
| RSDP | Where the ACPI tables start |
| Paging mode | Four levels, on a machine that could do five |

All of it lands in one `struct boot_info` (`include/tunix/boot.h`) and nothing
below `kmain` knows which loader filled it in.

Two of those need saying twice, because they are what the old bootloader let
the kernel assume and Limine does not:

- **There is no identity map.** Nothing is mapped at low addresses. A driver
  that reads a device register has to map the window first --
  `vmm_map_device()` -- rather than dereferencing what PCI told it.
- **The image is not at a fixed physical address.** `virtual - KERNEL_BASE` is
  no longer a physical address. `vmm_dma_physical()` answers that question, from
  the base pair the loader reported.

## Base revision 3

The kernel requests Limine base revision 3, which is the revision where the
higher-half direct map covers only usable, bootloader-reclaimable,
executable and framebuffer memory, and where the RSDP is reported as a physical
address. Bootloader-reclaimable memory is *not* reclaimed: the page tables the
kernel keeps using are in there, and so is every response it reads.

## The command line

```
root=/dev/sda2 init=/sbin/init
```

- `root=` names the block device the filesystem is on, by the name the block
  layer gives it. Partitions are devices of their own here (see
  `fs/partition.c`), so `sda2` is a device and the filesystem starts at its
  first sector. `root=LABEL=name` picks the ext2/ext3 volume with that label
  instead. When the named device is missing, the kernel rescans USB for ten
  seconds, so a slow stick can still arrive, and then stops with
  `root device not found` rather than mounting another disk. Without `root=` the
  kernel takes whatever registered first.
- `live` mounts the root without ever writing to its disk, as a live CD's is:
  changes are kept in memory and gone at the next boot (see
  [the root filesystem](filesystem.md#a-live-root)).
- `init=` names the first program. It defaults to `/sbin/init`; pointing it at
  something else is how a machine that will not finish booting gets debugged.
- `rd.driver.pre=a,b` loads those modules from the initramfs whether a device
  asks for them or not; `rd.driver.blacklist=` and `modprobe.blacklist=` keep
  them from loading. The names are dracut's.

## The initramfs

The bootloader may load an initramfs beside the kernel: a newc `cpio` archive,
uncompressed, the format Linux reads. Tunix mounts its root by itself, so the
archive is not a root to start from; it carries the drivers that have to be
there before the root is, which today means the display drivers that take the
console from its first line (`nv50`).

```
/Tulpar
    protocol: limine
    path: boot():/boot/kernel.elf
    module_path: boot():/boot/initramfs.img
    cmdline: root=LABEL=tulpar-root init=/sbin/init
```

Right after the heap is up, before the console is drawn, `core/initramfs.c`
reads every `.ko` in the archive and loads those a PCI device asks for -- the
device's modalias against the module's `alias=` lines, the match udev makes --
and those `rd.driver.pre=` names. A module that needs another's symbols waits
for it: each round loads what it can until one loads nothing more. A display
driver loaded so registers itself with `display_early_register()` and is asked
to take the screen as a built-in one would be. The modules show in
`/sys/module` and `lsmod` once sysfs is up, and `modprobe` then finds them
loaded.

| Loaded by | Where the archive comes from |
| --- | --- |
| Limine | the first `module_path` of the entry |
| GRUB, syslinux, QEMU's `-initrd` | the boot protocol's `ramdisk_image` and `ramdisk_size`, which the stub keeps out of usable memory |
| aarch64 | the device tree's `linux,initrd-start` and `linux,initrd-end` |

`make initramfs` builds `build/initramfs.img` with `tools/mkinitramfs`, a POSIX
shell script that writes the archive with `printf`, so it runs on the system
too:

```
tools/mkinitramfs initramfs.img /usr/lib/modules/0.1.0/kernel/nv50.ko
```

The UEFI-application entry of `vmlinuz` takes no initramfs yet: it would have
to read the file named by `initrd=` from the ESP itself.

## Without Limine: vmlinuz

`make` also builds `build/vmlinuz`, the same kernel wrapped so that any loader
that boots Linux can boot Tunix: GRUB on BIOS or UEFI, systemd-boot, rEFInd,
syslinux, QEMU's `-kernel`, or UEFI firmware starting it directly as an
application. It is one file with two headers, as Linux's bzImage is:

- a **PE/COFF header**, so UEFI can load it as an EFI application (the EFI stub);
- the **Linux x86 boot protocol's setup header**, version 2.15, with a real-mode
  entry, a 32-bit entry at `code32_start` and a 64-bit entry 0x200 after it.

| Started by | Entry | Where the memory map, framebuffer and RSDP come from |
| --- | --- | --- |
| UEFI, systemd-boot, rEFInd | `efi_main` | GetMemoryMap, the GOP, the configuration table; then ExitBootServices |
| GRUB on UEFI | 64-bit | `boot_params`: e820, `screen_info`, the EFI system table GRUB names |
| GRUB on BIOS | 32-bit | `boot_params`, filled in by GRUB |
| QEMU `-kernel` on BIOS, syslinux | real mode | the setup asks the BIOS: e820, and a 32-bit VBE linear framebuffer mode |

The code is in `arch/x86_64/bzimage`. Whichever way it was entered, the stub
does what Limine does for `kernel.elf`: it maps physical memory identity and at
`0xffff800000000000`, maps the kernel at its link address over the pages it was
loaded into -- the kernel runs where it lies in the file, its `.bss` included in
the size the header asks the loader for -- enables no-execute and write
protection, loads a GDT with the kernel's selectors, and enters
`stub_kernel_entry` with a `struct boot_stub_handoff`. The kernel copies the
handoff into the same `boot_info` Limine's answers fill, and from `kmain` on
nothing knows which loader it was.

The console needs a 32-bit linear framebuffer. GRUB keeps text mode unless told
otherwise, and may pick a 24-bit mode when only a size is given, so:

```
set gfxpayload=1280x800x32
linux /boot/vmlinuz root=LABEL=tulpar-root init=/sbin/init
```

Without one, the stub says so on the serial port (and the EFI console, before
it leaves boot services) rather than starting a kernel that would panic.
