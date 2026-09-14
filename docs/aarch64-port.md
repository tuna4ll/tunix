# aarch64 Port

Tunix is being ported to 64-bit ARM (AArch64). This documents the state of that
port: what boots today, how it is built, and what remains. The x86-64 kernel is
unaffected — the ARM backend lives entirely under `kernel/arch/aarch64/` and has
its own build target.

## What works

On QEMU's `virt` machine (GICv3, Cortex-A72) the kernel boots into the high half,
sets up memory management and reaches a timer-driven idle loop:

```
=== Tunix aarch64 ===
running at EL1, kernel at 0xffff000040000000 (higher half)
device tree @ 0x48000000: 2048 MiB RAM @ 40000000, 4 CPU(s)
PMM: 523941 free frames (2046 MiB)
heap: 16383 KiB, alloc/free stress OK, reclaimed fully
address spaces: identity map dropped, TTBR0 = 0x4105b000
VMM: VA 0x400000 reads aaaa in A and bbbb in B, isolation OK
VMM: destroying a space reclaimed 4 page-table frames
GICv3 initialised
generic timer armed at 100 Hz, enabling IRQs
running; waiting for timer interrupts...
[aarch64] tick 100 (1 s)
```

Bring-up covers, in order:

1. **Boot & relocation** (`boot.S`). Entered via the arm64 Linux Image protocol,
   so the firmware hands the DTB in `x0`. The loader may place the image at any
   2 MiB-aligned address, so the head code copies it down to the physical base it
   was built for — without this the vector table and globals point at the wrong
   memory. Everything before the MMU is reached with `adr`/`adrp`, which yields
   physical addresses while the kernel runs below its virtual link base.
2. **EL2 → EL1**. If entered at EL2, it enables the EL1 physical timer/counter
   (`CNTHCTL_EL2`) and GIC system-register access (`ICC_SRE_EL2`), then `eret`s
   to EL1h.
3. **MMU and the high half** (`mmu.c`). The kernel is linked at
   `0xFFFF000040000000` and the high half is a *direct map*: `VA = PA +
   0xFFFF000000000000`. Because a high address's low 48 bits are exactly the
   physical address, one set of tables serves both the boot identity map
   (`TTBR0`) and the kernel map (`TTBR1`). The map is built from 1 GiB blocks —
   device memory for the peripheral window, normal write-back cacheable for RAM —
   with `MAIR_EL1`/`TCR_EL1` set and `SCTLR_EL1.{M,C,I}` enabled. Head code then
   branches to the virtual alias, moves `SP` and `VBAR_EL1` up, and the kernel
   runs virtual from there on.
4. **Device tree** (`fdt.c`). A minimal flattened-device-tree reader pulls the RAM
   base, total RAM and the CPU count out of the DTB, read through the direct map.
5. **Exceptions** (`exceptions.S`). A 16-entry `VBAR_EL1` vector table with a
   full integer register frame saved on entry; synchronous and IRQ paths call
   into C.
6. **Interrupts** (`gic.c`). GICv3: distributor + redistributor wake, the timer
   PPI enabled as Group 1, and the CPU interface (`ICC_SRE/PMR/IGRPEN1_EL1`)
   brought up.
7. **Timer** (`timer.c`). The architected generic timer at 100 Hz, acknowledged
   and re-armed from the IRQ handler.
8. **Console** (`uart.c`). A PL011 driver with a small `kprintf`. MMIO goes
   through `phys_to_virt`, so the same driver works before and after the MMU.
9. **Physical memory** (`pmm.c`). A frame bitmap over the DTB-reported RAM, with
   the kernel image, heap arena and DTB reserved; `pmm_alloc_page`/`pmm_free_page`.
10. **Page mapping** (`vmm.c`). A 4 KiB, four-level `vmm_map`/`vmm_unmap` that
    grows intermediate tables from the PMM and shoots down the TLB entry.
    Permissions are explicit: `VMM_WRITE`, `VMM_USER` and `VMM_EXEC` pick the
    `AP` bits and leave `PXN`/`UXN` set so only one exception level can execute
    any given page.
11. **Kernel heap** (`heap.c`). A first-fit `kmalloc`/`kfree` with block splitting
    and free-run coalescing over a 16 MiB arena reserved from the PMM.
12. **User address spaces** (`vmm.c`). `vmm_create_space`/`vmm_switch_space`/
    `vmm_destroy_space` give each future process a private `TTBR0` root while the
    kernel stays in `TTBR1`. Installing the first one is what retires the boot
    identity map — the kernel keeps running purely out of the high half, which is
    the proof that the split is real. The self-test maps one VA to two different
    frames in two spaces and confirms each space reads back its own data.

## Building and running

```sh
make aarch64                 # -> build/kernel-aarch64.img (a flat arm64 Image)
make run-aarch64             # boot it under qemu-system-aarch64 -M virt
```

The toolchain is `aarch64-linux-gnu-gcc`; QEMU is `qemu-system-aarch64`. The
x86-64 build (`make`, `make kernel`) is untouched — its source glob prunes
`kernel/arch/aarch64`.

## What is next

This is the HAL bring-up, not a running userland yet. The ladder from here:

- **Process & syscalls** — context switch, `SVC` entry on top of the existing
  vector table, `TPIDR_EL0` for TLS, then a first static AArch64 user binary
  dropped to EL0 in its own address space.
- **Storage & console** — virtio-mmio block and console, then ext2 on top.
- **Userland** — an AArch64 Void glibc rootfs, init, and a shell.
- **Wiring the portable core** — the arch-neutral subsystems (vfs, ext2/3,
  scheduler logic, the module loader core minus relocations) plug in behind a
  small arch interface; see the arch-coupling notes in the port plan.
- **Module loader** — `R_AARCH64_*` relocations and an AArch64 module area.

### A note on SMP

QEMU's `virt` exposes PSCI with `method = "hvc"`, i.e. the PSCI implementation
lives at EL2. Since Tunix takes over EL2 and drops to EL1, an `HVC` from EL1
would trap into our own (absent) EL2 handler rather than firmware, so secondary
cores cannot be started that way as things stand. Bringing up SMP needs either a
machine whose conduit is `smc` (an EL3 firmware, e.g. `virt,secure=on`) or a
minimal EL2 stub that forwards PSCI calls. This is why SMP is sequenced after
the single-core process work rather than before it.
