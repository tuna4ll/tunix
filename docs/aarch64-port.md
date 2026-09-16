# aarch64 Port

Tunix is being ported to 64-bit ARM. The port is no longer a separate bring-up
kernel: the same portable kernel that runs the x86-64 desktop — process model,
scheduler, signals, VFS, ext2/ext3, the block layer, PCI, NVMe, pipes, sockets
and TTYs — is built for AArch64 and boots on QEMU's `virt` machine, mounts an
ext3 root from an NVMe disk and runs `/sbin/init` at EL0.

```
TUNIX: aarch64 kernel at 0xffffffff80000000, loaded at 0x40200000, device tree at 0x48000000
TUNIX: 1 memory range(s), 5 region(s), GICv3, timer 27, 1 cpu(s)
PMM: 2036 MiB usable of 2036 MiB installed, ceiling 8192 MiB
GIC: v3 with 288 lines
PCI: ECAM at 0x4010000000, buses 0-255
BLOCK: sda (nvme0), 131072 sectors
BLOCK: root on sda
EXT3: journal ready, 1024 blocks
TUNIX: starting /sbin/init
PROCTEST fork ok
PROCTEST parent tls+fpu across switches ok
PROCTEST clone thread ok
PROCTEST thread tls and stack ok
PROCTEST sigreturn reads the context ok
PROCTEST read restarted with SA_RESTART ok
PROCTEST execve ok
PROCTEST PASS
```

`support/tests/proctest.c` builds for both architectures from one source, so the
same checks — fork, CLONE_SETTLS threads, TLS and FPU state across context
switches, SA_SIGINFO handlers and their context, EINTR against SA_RESTART,
execve — run on either kernel.

The userland above it is unmodified Void Linux for aarch64. From the published
`void-aarch64-ROOTFS` tarball written to an ext3 disk, runit comes up as PID 1,
stage 1 mounts the pseudo-filesystems, starts eudev, seeds the random number
generator and applies sysctl settings, and stage 2 starts agetty on every
virtual terminal:

```
- runit: enter stage: /etc/runit/1
=> Welcome to Void!
=> Starting udev and waiting for devices to settle...
=> Seeding random number generator...
=> Initialization complete, running stage 2...
- runit: enter stage: /etc/runit/2
Void 0.1.0 (void-live) (tty1)
void-live login:
```

Dynamically linked glibc programs run through `ld-linux-aarch64.so.1`: bash
5.2, coreutils, sed, grep, background jobs and `wait`. There is no display on
`virt`, so the virtual terminals run headless and their output is mirrored to
the serial console, which is also where their keyboard input comes from.

The same kernel image has been checked on GICv2 (`-M virt,gic-version=2`) and on
Cortex-A53, an ARMv8.0 core with a 40-bit physical address space, as well as the
default Cortex-A72.

Secondary processors come up through PSCI `CPU_ON` or, on boards whose firmware
parks them, through the spin table named by `cpu-release-addr`. `-smp 4` runs
four processors; eight parallel `tar | cksum` passes over `/usr/bin` produce the
same checksum as a single one.

### Raspberry Pi 4

The same image boots QEMU's `raspi4b` machine with the Raspberry Pi firmware's
own `bcm2711-rpi-4-b.dtb`. That device tree puts the mini UART, the GIC-400 and
the SD controllers behind `/soc`'s `ranges`, parks the secondary processors on
spin tables, and names the console through the `serial0` alias. The kernel
translates every address through the bus ranges, takes the mini UART as a
16550 with `reg-shift = 2`, finds the SD card through the SDHCI driver, mounts
ext3 from it and starts all four processors:

```sh
qemu-system-aarch64 -M raspi4b -kernel build/kernel-aarch64-core.img \
    -dtb bcm2711-rpi-4-b.dtb -append root=LABEL=tunix-root \
    -nographic -serial null -serial stdio -drive file=sd.img,if=sd,format=raw
```

```
TUNIX: 1 memory range(s), 6 region(s), GICv2, timer 27, 4 cpu(s)
SDHCI: controller at 0xfe300000
SDHCI: high-capacity card, 4096 MiB
EXT3: journal ready, 16384 blocks
SMP: 4 of 4 processors running
PROCTEST PASS
```

The SDHCI driver (`kernel/drivers/storage/sdhci.c`) is portable: PIO transfers,
standard- and high-capacity cards, 32-bit register access throughout, and a
write delay for the BCM2835 controller that needs one.

### Display

A framebuffer comes from the device tree's `simple-framebuffer` node, which is
how the Raspberry Pi firmware hands over HDMI, or on QEMU from `-device ramfb`,
configured through fw_cfg's DMA interface with memory carved off the top of RAM.
Either one brings up the same console the x86-64 kernel draws, and the terminals
stop being headless. ramfb memory is left out of the direct map so the same
pages are never mapped with two different cache attributes.

### The desktop image

`make image-aarch64` builds the same image the x86-64 build does — Void's base,
the package list in `GNUmakefile` with Weston, Mesa and Firefox, and
`base-files/` over it — from Void's aarch64 repository. It needs no root: the
sysroot and the ext3 image are assembled inside a user namespace
(`unshare --map-auto --map-root-user`), which is enough for `chown` and for
`mke2fs -d` to record real ownership, and the packages' install scripts run
through the host's `qemu-aarch64` binfmt handler.

```sh
make image-aarch64           # -> build/tunix-aarch64.img, GPT with an ESP and an ext3 root
make run-aarch64-image       # virt, ramfb, xHCI keyboard and mouse, virtio-net, NVMe
```

It boots to the Weston session: seatd hands over the display and the xHCI
keyboard and mouse, Mesa renders through llvmpipe onto the DRM device backed by
the ramfb framebuffer, the HD Audio codec is found on PCI, and DHCP and HTTPS
work through virtio-net.

## Building and running

```sh
make aarch64-core            # -> build/kernel-aarch64-core.img, an arm64 Image
make run-aarch64-core QEMU_AARCH64_CORE_DISKS="-drive file=disk.img,if=none,id=nv0,format=raw \
    -device nvme,drive=nv0,serial=tunix -append root=LABEL=tunix-root"
```

The toolchain is `aarch64-linux-gnu-gcc`. A root disk is an ext2/ext3 image made
with the same `mkfs` options `support/image.sh` uses for x86-64:

```sh
mkdir -p root/sbin root/dev root/proc root/sys root/tmp
aarch64-linux-gnu-gcc -static -nostdlib -nostartfiles -ffreestanding -O2 \
    support/tests/proctest.c -o root/sbin/init
truncate -s 64M disk.img
mkfs.ext3 -q -r 1 -b 4096 -I 128 -m 1 -L tunix-root \
    -O ^resize_inode,^dir_index,^ext_attr,^metadata_csum,^64bit,^huge_file,^dir_nlink,^extra_isize \
    -d root disk.img
```

`make aarch64` still builds the original bring-up kernel from
`kernel/arch/aarch64/bringup/`, which is kept only as a reference until the
portable kernel covers everything it demonstrated.

## How the port is put together

The portable kernel reaches the machine through a small set of seams, each with
an x86-64 and an AArch64 side. Every step of that work was checked by building
the x86-64 kernel before and after and comparing the objects: where the seam
only renames an operation, x86-64 compiles to the same instructions.

| Seam | x86-64 | AArch64 |
| --- | --- | --- |
| `include/cpu.h` | `pause`, `cli`/`sti`, `rdtsc`, `cpuid` | `yield`, DAIF, `cntvct_el0`, `MIDR_EL1` |
| `include/percpu.h` | GS base | `TPIDR_EL1` |
| `include/syscall_abi.h` | `rax`, `rdi`…`r9`, `syscall` is 2 bytes | `x8`, `x0`…`x5`, `svc` is 4 bytes |
| `include/process_arch.h` | register file, FS base, `ucontext` | `x0`–`x30`, `TPIDR_EL0`, arm64 `ucontext` |
| `include/vmm_arch.h` | x86 page table entries, CR3 | arm64 descriptors, TTBR0/TTBR1 |
| `include/platform.h` | PIC, GDT/IDT, IOAPIC routing | GIC, PCIe ECAM, generic timer |
| `time.h` hooks | TSC calibration, CMOS | `CNTFRQ_EL0`, PL031 |
| `random.h` hooks | RDSEED/RDRAND | RNDRRS/RNDR |

**Paging.** `kernel/vmm.c` is shared. AArch64 with a 4 KiB granule and 48-bit
virtual addresses walks four levels indexed exactly like x86-64, so copy-on-write,
fork, pruning, translation and user copies are one implementation; only the entry
encoding differs. Every address space has one root that is written to both
`TTBR0_EL1` and `TTBR1_EL1`, which reproduces the CR3 model the core was written
for. `T0SZ` is 17, so user space ends at the same `USER_ADDRESS_LIMIT` as on
x86-64. The logical `PAGE_*` flags become AP, UXN/PXN, nG and AF bits, with
copy-on-write, shared and file-backed pages in the software bits 55–58. The
direct map covers RAM only, never the MMIO between memory ranges.

**Boot.** `head.S` carries the arm64 Image header with the "place anywhere" flag,
so any loader that speaks that protocol can start it at any 2 MiB boundary. It
drops from EL2 to EL1 when entered there, enabling GICv3 system registers only
when `ID_AA64PFR0_EL1` says they exist, sets `IPS` from `ID_AA64MMFR0_EL1`, and
builds just enough tables — the image in both halves and the device tree — to
reach `aarch64_start()` at the kernel's link address. Everything else comes from
the device tree: memory ranges, `/memreserve/` and `/reserved-memory`, the
initrd, the command line, the console named by `stdout-path` (PL011 or MMIO
16550/DesignWare with `reg-shift` and `reg-io-width`), the GIC, the timer
interrupt, the RTC, the PCIe host bridge and its windows, and PSCI.

**Exceptions.** `vectors.S` saves the same 288-byte `struct syscall_frame` the
dispatcher reads. The x86-64 rule that a returning frame is copied to the top of
the current process's kernel stack carries over: the path back to EL0 loads
`SP` from the per-CPU `kernel_rsp`, so `SP_EL1` is already correct on the next
entry. System calls use the generic AArch64 numbers; `arch/aarch64/syscalls.c`
maps them onto the kernel's table and keeps the result in the frame's reserved
slot, so a restarted call does not overwrite `x8`.

**Devices.** On a machine without firmware PCI setup, `pci_assign_resources()`
sizes and places memory BARs from the host bridge's windows. NVMe runs as-is.

## What is next

- **Userland ABI.** `struct stat`, the `O_*` values that differ, `epoll_event`
  packing and `uname` already follow AArch64. glibc never sets `SA_RESTORER`
  there, so every process gets a sigreturn trampoline page at `0x7FFFFFFFF000`.
  Remaining differences show up as Void's services are exercised.
- **Interrupts for devices.** Drivers currently poll. Wiring INTx through the
  device tree's `interrupt-map` and MSI through the GICv3 ITS comes next.
- **Real boards.** Non-ECAM PCIe hosts (the Pi 4's own), USB, Ethernet, and
  UEFI/ACPI firmware.
