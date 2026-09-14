# aarch64 Port

Tunix is being ported to 64-bit ARM (AArch64). This documents the state of that
port: what boots today, how it is built, and what remains. The x86-64 kernel is
unaffected — the ARM backend lives entirely under `kernel/arch/aarch64/` and has
its own build target.

## What works

On QEMU's `virt` machine (GICv3, Cortex-A72) the kernel boots into the high half,
sets up memory management, reads `/sbin/init` off an ext2 disk over virtio-blk,
and runs it at EL0 on a proper process stack under a preemptive scheduler:

```
=== Tunix aarch64 ===
running at EL1, kernel at 0xffff000040000000 (higher half)
device tree @ 0x48000000: 2048 MiB RAM @ 40000000, 4 CPU(s)
PMM: 523933 free frames (2046 MiB)
heap: 16383 KiB, alloc/free stress OK, reclaimed fully
address spaces: identity map dropped, TTBR0 = 0x41063000
VMM: VA 0x400000 reads aaaa in A and bbbb in B, isolation OK
VMM: destroying a space reclaimed 4 page-table frames
virtio-mmio slot 31: block (id 2, version 1, irq 79)
virtio-blk: 32768 sectors (16 MiB)
ext2: read /sbin/init from inode 15, 1200 bytes
GICv3 initialised
generic timer armed at 100 Hz, enabling IRQs
scheduler: 3 tasks queued behind the idle task
[task 1] round 0 at tick 1
[task 2] round 0 at tick 2
[task 3] loaded /sbin/init (1200 bytes), phdr 0x400040, sp 0x503e60
hello from a real ELF binary at EL0
[aarch64] EL0 task exited with status 7
[task 3] back at EL1
[task 1] round 1 at tick 22
[task 2] round 1 at tick 29
[task 1] round 2 at tick 43
[task 2] round 2 at tick 56
[task 1] finished
[task 2] finished
[aarch64] alive at 2 s, task 0 running, heap 16382 KiB free
```

The interleaved rounds are the timer preempting the workers. The heap settles one
KiB below where it started because the finished tasks' kernel stacks are handed
back while the buffer holding `/sbin/init` is kept for the life of the boot.

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
5. **Exceptions** (`exceptions.S`). A 16-entry `VBAR_EL1` vector table. Entry
   saves `x0`–`x30` plus `ELR_EL1`, `SPSR_EL1` and `SP_EL0`, which is what makes
   returning to EL0 — and taking interrupts while there — safe.
6. **Interrupts** (`gic.c`). GICv3: distributor + redistributor wake, the timer
   PPI enabled as Group 1, and the CPU interface (`ICC_SRE/PMR/IGRPEN1_EL1`)
   brought up.
7. **Timer** (`timer.c`). The architected generic timer at 100 Hz, acknowledged
   and re-armed from the IRQ handler; it also drives preemption.
8. **Console** (`uart.c`). A PL011 driver with a small `kprintf`. MMIO goes
   through `phys_to_virt`, so the same driver works before and after the MMU.
9. **Physical memory** (`pmm.c`). A frame bitmap over the DTB-reported RAM, with
   the kernel image, heap arena and DTB reserved; single frames or a contiguous
   run through `pmm_alloc_page`/`pmm_alloc_pages`.
10. **Page mapping** (`vmm.c`). A 4 KiB, four-level `vmm_map`/`vmm_unmap` that
    grows intermediate tables from the PMM and shoots down the TLB entry.
    Permissions are explicit: `VMM_WRITE`, `VMM_USER` and `VMM_EXEC` pick the
    `AP` bits and leave `PXN`/`UXN` set so only one exception level can execute
    any given page.
11. **Kernel heap** (`heap.c`). A first-fit `kmalloc`/`kfree` with block splitting
    and free-run coalescing over a 16 MiB arena reserved from the PMM.
12. **User address spaces** (`vmm.c`). `vmm_create_space`/`vmm_switch_space`/
    `vmm_destroy_space` give each process a private `TTBR0` root while the kernel
    stays in `TTBR1`. Installing the first one is what retires the boot identity
    map — the kernel keeps running purely out of the high half, which is the proof
    that the split is real. The self-test maps one VA to two different frames in
    two spaces and confirms each space reads back its own data.
13. **EL0 and syscalls** (`usermode.S`, `syscall.c`). `aarch64_enter_user` sets
    `SP_EL0`/`ELR_EL1`/`SPSR_EL1` and `eret`s to EL0; `aarch64_leave_user`
    restores the saved kernel stack so entering user mode looks like an ordinary
    call that returns. The save slot lives in the task, so an EL0 task that is
    preempted still returns to its own kernel context. `SVC` from EL0 is
    recognised by exception class `0x15` and dispatched with the Linux AArch64
    convention (`x8` = number, `x0`–`x5` = arguments, result in `x0`). `write`,
    `exit` and `exit_group` are implemented; anything else returns `-ENOSYS`.
14. **Tasks and preemption** (`sched.c`, `switch.S`). Each task gets a 16 KiB
    kernel stack and a saved-`SP` context of the callee-saved registers; the boot
    path becomes the idle task. `aarch64_context_switch` swaps stacks, and the
    timer IRQ calls into the round-robin scheduler, so tasks are preempted rather
    than cooperative. A finished task is marked done and its stack is freed by the
    next scheduler pass, once nothing is standing on it.
15. **ELF loading** (`elf.c`). A real static `ET_EXEC`/`EM_AARCH64` binary is
    validated, its `PT_LOAD` segments are backed with fresh frames, the file
    bytes are copied through the direct map, the remainder of each segment is
    left zeroed for `.bss`, and the pages are mapped with the permissions the
    segment asks for. It also reports where the program headers landed, which is
    what `AT_PHDR` needs. Loading is from a memory buffer, so the same call serves
    a file read off the disk and the copy embedded in the kernel image.
16. **virtio-mmio and virtio-blk** (`virtio.c`). QEMU's `virt` lays 32 virtio-mmio
    slots end to end at `0x0A000000`, SPI 16 upwards, and fills them from the top;
    the probe walks them and reports what is plugged in. The block driver then
    takes the device through the legacy (version 1) handshake — status bits,
    feature selection, guest page size, a split virtqueue published through the
    single page-frame-number register — and reads sectors with the usual
    three-descriptor chain (header, data, status), polling the used ring rather
    than taking the interrupt.
17. **ext2** (`ext2.c`). Enough of the filesystem to boot from: superblock, group
    descriptors, inodes, directory entries, and file data through the direct
    blocks plus one level of indirection. It is read-only, assumes 4 KiB blocks
    like the x86-64 driver does, and ignores the journal, which is safe for a
    cleanly unmounted image. Every field is read at its byte offset rather than
    through a packed struct, so nothing depends on how the compiler lays one out
    under `-mstrict-align`. `/sbin/init` is resolved, read into the heap, and
    handed to the ELF loader; the embedded binary is only the fallback when no
    disk is attached.
18. **The initial process stack** (`ustack.c`). The layout `kernel/elf.c` builds
    for x86-64, mirrored: the random bytes and platform string at the top, then
    the environment and argument strings, then a 16-byte-aligned block holding
    the auxiliary vector, the `envp` and `argv` arrays and `argc`. The same
    eighteen auxiliary entries are supplied, with `AT_PLATFORM` reading
    `"aarch64"`. `AT_RANDOM` is seeded from the cycle counter, which is not a
    cryptographic source and will have to be replaced. `support/aarch64/initargs.c`
    is a freestanding program that walks what it was given and prints it back:

    ```
    [task 3] loaded /sbin/init (2912 bytes), phdr 0x400040, sp 0x503e60
    initargs: argc=1
    initargs: argv[0]=/sbin/init
    initargs: envc=2
    initargs: env PATH=/bin:/sbin
    initargs: env TERM=linux
    initargs: auxv type=6 value=0x1000       (AT_PAGESZ)
    initargs: auxv type=3 value=0x400040     (AT_PHDR)
    initargs: auxv type=4 value=0x38         (AT_PHENT)
    initargs: auxv type=5 value=0x3          (AT_PHNUM)
    initargs: auxv type=9 value=0x400120     (AT_ENTRY)
    ...
    initargs: auxv entries=18
    INITARGS DONE
    ```

    `AT_PHDR`, `AT_PHENT`, `AT_PHNUM` and `AT_ENTRY` match what `readelf` reports
    for the binary, and the stack pointer handed to EL0 is 16-byte aligned.

## Building and running

```sh
make aarch64                 # -> build/kernel-aarch64.img (a flat arm64 Image)
make run-aarch64             # boot it under qemu-system-aarch64 -M virt
```

The toolchain is `aarch64-linux-gnu-gcc`; QEMU is `qemu-system-aarch64`. The
build also links two freestanding user programs: `hello.elf`, embedded in the
kernel image as the fallback init, and `initargs.elf`, the stack reporter above.

To boot from a disk instead, build an ext2 image with a `/sbin/init` in it — the
same `mkfs` options the x86-64 image uses — and attach it:

```sh
mkdir -p root/sbin && cp build/aarch64/initargs.elf root/sbin/init
truncate -s 16M disk.img
mkfs.ext3 -q -r 1 -b 4096 -I 128 -m 1 -L tunix-root \
    -O ^resize_inode,^dir_index,^ext_attr,^metadata_csum,^64bit,^huge_file,^dir_nlink,^extra_isize \
    -d root disk.img

qemu-system-aarch64 -M virt,gic-version=3 -cpu cortex-a72 -m 2048M -nographic \
    -kernel build/kernel-aarch64.img \
    -drive file=disk.img,if=none,id=d0,format=raw -device virtio-blk-device,drive=d0
```

The x86-64 build (`make`, `make kernel`) is untouched — its source glob prunes
`kernel/arch/aarch64`, and the two trees produce a bit-identical `kernel.elf`.

## What is next

A binary comes off a real filesystem, gets a conforming process stack, and runs
in its own address space. What it still cannot be is a real program:

- **The syscall surface** — three calls is enough for a freestanding binary;
  glibc needs a couple of hundred (`openat`, `mmap`, `brk`, `clone`, `futex`,
  `ioctl`, …). This is the bulk of the remaining work.
- **Writes and more devices** — the block path is read-only and polled, so
  writing, virtio-console and anything that cannot busy-wait need the device's
  SPI wired into the GIC (`GICD_ISENABLER`/`GICD_IROUTER`, which the PPI path
  does not touch yet).
- **Userland** — an AArch64 Void glibc rootfs, init, and a shell.
- **Wiring the portable core** — the arch-neutral subsystems (vfs, ext2/3,
  scheduler policy, the module loader core minus relocations) plug in behind a
  small arch interface.

  The first piece of that interface is in place: `kernel/include/syscall_abi.h`
  defines `SYSCALL_NR`, `SYSCALL_ARG0`–`SYSCALL_ARG5` and `SYSCALL_RET`, and
  `kernel/syscall.c` now reaches the register file only through them, so the
  5900-line dispatcher no longer names an x86-64 register. Each architecture
  supplies the mapping — x86-64 in that header, AArch64 in
  `kernel/arch/aarch64/arch.h` over `struct trap_frame`. They cannot share one
  accessor for the number and the result, because AArch64 takes the number in
  `x8` and returns in `x0` while x86-64 uses `rax` for both. `struct
  syscall_frame`'s layout is now pinned with static assertions against the
  offsets `syscall_entry.S` writes by hand.

  What still needs an arch-neutral shape: `kernel/elf.c`'s `elf_load_process`
  is written against `struct process` and the VFS, and `process.c` builds child
  frames and saves interrupt context by register name. The loaders in this port
  are deliberately the same algorithms over a buffer, so they can fold into the
  shared core once the VFS exists.
- **Module loader** — `R_AARCH64_*` relocations and an AArch64 module area.

### A note on SMP

QEMU's `virt` exposes PSCI with `method = "hvc"`, i.e. the PSCI implementation
lives at EL2. Since Tunix takes over EL2 and drops to EL1, an `HVC` from EL1
would trap into our own (absent) EL2 handler rather than firmware, so secondary
cores cannot be started that way as things stand. Bringing up SMP needs either a
machine whose conduit is `smc` (an EL3 firmware, e.g. `virt,secure=on`) or a
minimal EL2 stub that forwards PSCI calls. This is why SMP is sequenced after
the single-core process work rather than before it.
