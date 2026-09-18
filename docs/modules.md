# Modules

Drivers that are not in the kernel image. `insmod`, `modprobe`, `lsmod`,
`modinfo` and `rmmod` here are Void's own `kmod`, talking to the kernel through
`init_module(2)`, `finit_module(2)` and `delete_module(2)`, and reading
`/proc/modules` and `/sys/module` for what is loaded. `depmod` runs on the build
host and writes the same `modules.dep`, `modules.alias` and `.bin` indexes a
Linux kernel's modules get, so `modprobe` resolves dependencies and udev
autoloads by device alias without anything here knowing it is not Linux.

```
# lsmod
Module                  Size  Used by
snd_hda                16384  0
rtl8139                12288  0
```

## What a module file is

A `.ko` is an ordinary ELF relocatable object -- `cc -c` with the kernel's own
flags, nothing linked, nothing stripped. `kernel/modules/*.c` are built for both
architectures and `kernel/modules/x86_64/*.c` only for x86-64; the build walks
neither directory when it links the kernel, which is the whole difference
between a module and a driver.

Three sections make it a module rather than an object file:

| Section | What the loader reads from it |
| --- | --- |
| `.modinfo` | `name=`, `vermagic=`, `license=`, `alias=` -- the same NUL-separated strings `modinfo` prints |
| `.tunix_module` | the name, init and exit of the module: `MODULE_MAIN(start, stop)` |
| `.tunix_ksym` | what this module exports for other modules, from `MODULE_EXPORT(symbol)` |
| `.tunix_param` | its parameters, from `MODULE_PARAMETER(variable, kind)` |

`MODULE_EXPORT` also writes the symbol's name into `__ksymtab_strings` and names
its entry `__ksymtab_<symbol>`, because that is where `depmod` looks for what a
module exports. Without it `modules.dep` has every module and no edges.

`vermagic` is `0.1.0 x86_64` -- the release `uname -r` reports and the machine.
The kernel refuses a module whose string is not its own, which is what stops a
`.ko` built against another tree from being relocated into this one.

## Loading one

`finit_module(fd, args, 0)` reads the whole file, and then:

**Placement.** Allocating sections is a bump through three regions --
executable, read-only, writable -- each starting on a page, with `.bss` at the
end of the writable one. The pages behind them are one physically contiguous
run from the page allocator, mapped into the kernel's own address space; the
whole module gets `PAGE_WRITE` while it is being relocated and then the text
becomes read-execute, the rodata read-only and the data writable and
non-executable.

**Symbols.** Undefined symbols are looked up in `kernel/ksyms.c` -- a plain
table of what the kernel offers a module, roughly sixty entries -- and then in
the exports of every module already loaded. A module that resolves a symbol
from another one takes a reference on it, which is what puts it in the second
module's `holders/` directory and in `lsmod`'s "Used by" column.

**Relocations.** `R_X86_64_64`, `PC32`, `PLT32`, `32`, `32S` and `PC64` on
x86-64; `ABS64`, `ABS32`, `PREL32/64`, the `MOVW_UABS` family, `ADR_PREL_PG_HI21`,
`ADD/LDST*_ABS_LO12_NC`, `CALL26`, `JUMP26`, `CONDBR19`, `TSTBR14` and
`LD_PREL_LO19` on aarch64. Anything else is refused by number rather than
silently mis-applied.

**Where modules live** is the one place the two architectures differ, and the
reason is the relocations above:

| | Window | Why there |
| --- | --- | --- |
| x86-64 | `0xFFFFFFFFC0000000`, 16 MiB | `PC32` reaches ±2 GiB, and `32S` addresses the top 2 GiB, so anywhere in the kernel's own PML4 entry works |
| aarch64 | `0xFFFFFFFF84000000`, 16 MiB | `CALL26` reaches ±128 MiB, so a module has to sit next to the image, 64 MiB above its base |

Both windows hang off a page-table entry the kernel already owns, so a module
mapped after processes exist is visible in every address space without touching
any of them.

**Parameters** are assigned before `init` runs, from the string `insmod` passes:

```
# insmod tunix_probe.ko number=7 flag=1 text=hello
# cat /sys/module/tunix_probe/parameters/number
7
```

`int`, `unsigned`, `bool` and `char *` are the four kinds. An unknown name
fails the load rather than being ignored, because a parameter that silently
does nothing is worse than a module that does not load.

## What userspace sees

`/proc/modules` is the Linux format -- name, bytes, reference count, who holds
it, state, and the address the module was mapped at -- and `lsmod` parses it
unchanged. `/sys/module/<name>/` carries `initstate` (which is how `modprobe`
knows a module is already in), `refcnt`, `coresize`, `sections/.text`,
`holders/` and `parameters/`.

The directory disappears when the module does, and `rmmod` of a module another
one holds fails with `EBUSY` before its exit function runs.

## Drivers

A module that drives hardware registers a `struct pci_driver` with the ids it
matches, and `pci_register_driver()` walks every device on the bus and probes
the ones it recognises. Binding is what publishes `driver` in the device's sysfs
directory:

```
# lspci -k
00:03.0 Audio device: Intel Corporation 82801FB/FBM/FR/FW/FRW (ICH6 Family) High Definition Audio Controller
	Kernel driver in use: snd_hda
	Kernel modules: snd_hda
```

The autoload path is the one Linux uses, end to end. Every PCI device is
published under `/sys/devices/pci0000:00/` with a `modalias` and a writable
`uevent`; `udevadm trigger` writes `add` to it, the kernel broadcasts the
device's properties over netlink, and udev's own rule --
`ENV{MODALIAS}=="?*", RUN{builtin}+="kmod load '$env{MODALIAS}'"` -- runs
`modprobe`, which matches the alias against `modules.alias` and loads the
module:

```
pci:v00008086d00002668sv00001AF4sd00001100bc04sc03i00   the device
pci:v*d*sv*sd*bc04sc03i*                                snd_hda's alias
```

Nothing in the kernel asks for a module. The device describes itself, and
userspace decides what that description is worth -- which is why a module can be
blacklisted or given options in `/etc/modprobe.d` without the kernel having an
opinion.

## What is a module and what is not

`snd_hda` and `rtl8139` are modules. Everything the machine needs before there
is a root filesystem is not: IDE, AHCI, NVMe, USB, the framebuffer, virtio-gpu
and virtio-net are in the image. That is the same division a distribution
kernel makes, for the same reason -- a driver that has to be loaded from the
disk it drives cannot be.

Making a driver a module is not free of consequences, and both of them are
visible in this tree:

**A device can arrive after boot.** `snd_register_card()` used to be called
during `devfs_init()` and could assume nobody had looked yet. It now creates
`/dev/snd/controlC0` and `/dev/snd/pcmC0D0p`, publishes the sound class in
sysfs and broadcasts an `add` uevent, whenever the card registers -- and
removes them again when the module goes. The same applies to the network:
`net_register_adapter()` replaced a compile-time choice between two drivers, so
`eth0` exists from the moment a driver claims a card rather than from boot.

**A driver has to undo itself.** A module's exit runs
`pci_unregister_driver()`, which calls its `remove` for every device it bound.
That has to stop DMA, hand back the interrupt vector (`irq_release()`, which
exists for this) and free what the probe allocated -- an interrupt arriving
into unmapped module text is a fault with no handler, and the fault report names
the module it came from so it can at least be read.

## Testing it

`support/tests/moduletest.sh` boots a root made of Void's own `kmod`,
`pciutils`, `udevd` and `dhcpcd` -- copied out of the sysroot with their
libraries -- and drives the whole surface on either architecture:

```
sh support/tests/moduletest.sh build/kernel.elf
ARCH=aarch64 sh support/tests/moduletest.sh build/kernel-aarch64-core.img
```

It checks `insmod` with parameters, the sysfs files, loading a second module
that needs the first's symbols, `EBUSY` on the module in use, `modprobe`
pulling a dependency in, `modinfo`, `lspci` and `lspci -k`, the sound card
appearing and disappearing with its module, udev autoloading it from the
device's modalias, and -- with `NIC=rtl8139`, which is the default where that
module exists -- a DHCP lease over a network card whose driver was loaded by
udev.

`support/tests/soundtest.sh` loads `snd_hda.ko` itself with `finit_module`
before it opens the PCM device, which is the same path with no userland at all.

## Not here

The kernel never asks userspace for a module: there is no `request_module()`,
so `mount -t vfat` on a filesystem that was a module would fail rather than
load it. Every filesystem here is built in, so nothing wants it yet.

There is no `/proc/kallsyms`, no module signatures and no taint flags.
