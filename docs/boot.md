# Boot

Tunix is started by [Limine](https://github.com/limine-bootloader/limine), on
either BIOS or UEFI, from one disk image that carries both.

It used to have a bootloader of its own. That meant every change to the boot
contract was two projects wide, and the kernel could only be started from a disk
that loader had written -- no ISO, no USB stick somebody else prepared, no
firmware that only speaks EFI. Limine is a boot protocol rather than a program
to maintain: the kernel declares what it needs and gets it.

## The image

`support/image.sh` builds a GPT disk with two partitions:

| Partition | Contents |
| --- | --- |
| `sda1`, 64 MiB, FAT32, EFI system | `EFI/BOOT/BOOTX64.EFI`, `boot/limine/limine-bios.sys`, `boot/limine/limine.conf`, `boot/kernel.elf` |
| `sda2`, ext2 | the root filesystem |

The two firmwares take different paths into the same partition:

- **UEFI** loads `EFI/BOOT/BOOTX64.EFI`, which is Limine.
- **BIOS** runs the first stage that `limine bios-install` wrote into the gap
  between the protective MBR and the first partition, which is why `sda1`
  starts at sector 2048 rather than at 34. That stage finds
  `boot/limine/limine-bios.sys` on the ESP and continues.

Either way Limine reads `boot/limine/limine.conf`, loads `boot/kernel.elf`, and
enters it with the command line from the config.

## What the kernel asks for

`kernel/arch/x86_64/limine_entry.c` holds the request block. Limine finds it by
scanning the loaded image between two marker values, which is why the requests
sit in a section of their own that the linker script keeps.

| Request | What it is for |
| --- | --- |
| Memory map | Which physical pages the allocator may hand out |
| HHDM | Where all of memory is mapped, so `vmm_init` can reach a page table before the kernel's own direct map exists |
| Framebuffer | A linear framebuffer, its address, and its pixel format |
| Executable address | Where the image was loaded, physically and virtually |
| Command line | `root=`, `init=` |
| RSDP | Where the ACPI tables start |
| Paging mode | Four levels, on a machine that could do five |

All of it lands in one `struct boot_info` (`kernel/include/boot.h`) and nothing
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
  `kernel/partition.c`), so `sda2` is a device and the filesystem starts at its
  first sector. Without `root=` the kernel takes whatever registered first.
- `init=` names the first program. It defaults to `/sbin/init`; pointing it at
  something else is how a machine that will not finish booting gets debugged.

## Running it

```sh
make run        # BIOS
make run-uefi   # UEFI, against OVMF fetched into build/
```

Both use the same `build/tunix.img`.
