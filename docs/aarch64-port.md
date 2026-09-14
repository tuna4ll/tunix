# aarch64 Port

Tunix is being ported to 64-bit ARM (AArch64). This documents the state of that
port: what boots today, how it is built, and what remains. The x86-64 kernel is
unaffected — the ARM backend lives entirely under `kernel/arch/aarch64/` and has
its own build target.

## What works

On QEMU's `virt` machine (GICv3, Cortex-A72) the kernel boots and reaches a
timer-driven idle loop:

```
=== Tunix aarch64 ===
running at EL1
device tree @ 0x48000000: 2048 MiB RAM, 4 CPU(s)
MMU enabled (identity map, MAIR/TCR set)
GICv3 initialised
generic timer armed at 100 Hz, enabling IRQs
running; waiting for timer interrupts...
[aarch64] tick 100 (1 s)
[aarch64] tick 200 (2 s)
```

Bring-up covers, in order:

1. **Boot & relocation** (`boot.S`). Entered via the arm64 Linux Image protocol,
   so the firmware hands the DTB in `x0`. The loader may place the image at any
   2 MiB-aligned address, so the head code copies the image down to its link
   address before running — without this the vector table and globals point at
   the wrong physical memory.
2. **EL2 → EL1**. If entered at EL2, it enables the EL1 physical timer/counter
   (`CNTHCTL_EL2`) and GIC system-register access (`ICC_SRE_EL2`), then `eret`s
   to EL1h.
3. **Device tree** (`fdt.c`). A minimal flattened-device-tree reader pulls total
   RAM and the CPU count out of the DTB.
4. **MMU** (`mmu.c`). An identity map built from 1 GiB blocks — device memory
   for the low peripheral window, normal write-back cacheable for RAM — with
   `MAIR_EL1`/`TCR_EL1` set and `SCTLR_EL1.{M,C,I}` enabled.
5. **Exceptions** (`exceptions.S`). A 16-entry `VBAR_EL1` vector table with a
   full integer register frame saved on entry; synchronous and IRQ paths call
   into C.
6. **Interrupts** (`gic.c`). GICv3: distributor + redistributor wake, the timer
   PPI enabled as Group 1, and the CPU interface (`ICC_SRE/PMR/IGRPEN1_EL1`)
   brought up.
7. **Timer** (`timer.c`). The architected generic timer at 100 Hz, acknowledged
   and re-armed from the IRQ handler.
8. **Console** (`uart.c`). A PL011 driver with a small `kprintf`.
9. **Physical memory** (`pmm.c`). A frame bitmap over the DTB-reported RAM, with
   the kernel image and DTB reserved; `pmm_alloc_page`/`pmm_free_page`.
10. **Page mapping** (`vmm.c`). A 4 KiB, four-level `vmm_map_page`/`vmm_unmap_page`
    that grows intermediate tables from the PMM and shoots down the TLB entry —
    the paging infrastructure processes will need.
11. **Kernel heap** (`heap.c`). A first-fit `kmalloc`/`kfree` with block splitting
    and free-run coalescing over a 16 MiB arena reserved from the PMM.

## Building and running

```sh
make aarch64                 # -> build/kernel-aarch64.img (a flat arm64 Image)
make run-aarch64             # boot it under qemu-system-aarch64 -M virt
```

The toolchain is `aarch64-linux-gnu-gcc`; QEMU is `qemu-system-aarch64`. The
x86-64 build (`make`, `make kernel`) is untouched — its source glob prunes
`kernel/arch/aarch64`.

## What is next

This is P0-P3 of the port (a HAL bring-up), not a running userland yet. The
ladder from here:

- **Higher-half kernel** — move the kernel to a `TTBR1` virtual address and keep
  the identity map only for early boot.
- **SMP** — bring up secondary cores with PSCI `CPU_ON`, per-CPU via
  `TPIDR_EL1`, and GIC SGIs for IPIs/TLB shootdown.
- **Process & syscalls** — context switch, `SVC` entry, `TPIDR_EL0` for TLS,
  then run a first static AArch64 userland binary.
- **Userland** — an AArch64 Void glibc rootfs over virtio-mmio (disk + net).
- **Wiring the portable core** — the arch-neutral subsystems (vfs, ext2/3,
  scheduler logic, the module loader core minus relocations, pmm, heap) plug in
  behind a small arch interface; see the arch-coupling notes in the port plan.
- **Module loader** — `R_AARCH64_*` relocations and an AArch64 module area.
