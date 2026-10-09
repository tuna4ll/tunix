# Memory Layout

Where the kernel puts things in the virtual address space, and why the machine
could only use 1792 MiB of RAM until it stopped putting them there. It reflects
the code as it exists today.

## The windows

Everything the kernel addresses lives in the top half. Four regions matter:

| Base | What | Size |
| --- | --- | --- |
| `0xFFFFC00000000000` | direct map — all of physical memory | 62.5 TiB of room |
| `0xFFFFFE8000000000` | device registers mapped with `vmm_map_device` | 512 GiB |
| `0xFFFFFF0000000000` | kernel heap | up to 512 GiB |
| `0xFFFFFFFF80000000` | kernel image, and the first GiB of RAM | 1 GiB |
| `0xFFFFFFFFC0000000` | loadable modules (x86-64) | 16 MiB |
| `0xFFFFFFFFF0000000` | framebuffer | as large as the mode needs |
| `0xFFFFFFFFFF000000` | early device registers (APIC, ACPI window) | 16 MiB |

The last four share one PML4 entry — the top 2 GiB — because that is where
`-mcmodel=kernel` requires every symbol to be. The first two have entries of
their own and are only there because a pointer can be computed to them.

A module's window has to satisfy its relocations rather than the code model,
which is why aarch64 puts it somewhere else entirely — 64 MiB above the image,
at `0xFFFFFFFF84000000`, because `CALL26` reaches only ±128 MiB. See
[Modules](modules.md).

## Why the ceiling existed

The direct map used to start at `KERNEL_BASE` and run upward, and the
framebuffer window sat at a fixed address 1792 MiB above it. So the amount of
physical memory the machine could use was not a decision anybody made; it was
the distance between two constants:

```
KERNEL_BASE          0xFFFFFFFF80000000
+ 1792 MiB           0xFFFFFFFFF0000000   framebuffer starts here
```

`PMM_DIRECT_MAP_LIMIT` was that number written down. Give QEMU 2 GiB and the
kernel used 1792 MiB of it; give it 8 and it still used 1792.

That was reachable by ordinary use. A WebKit tab costs most of a gigabyte on a
blank page, so opening a browser on the desktop left a few hundred megabytes,
and a real site ran the machine out — reported by the dynamic loader as "Out of
memory" against every shared library it tried to map.

## Moving it out

The direct map is reached by computed pointers, never by a symbol, so nothing
requires it to be in the top 2 GiB — the same argument that moved the heap out
earlier. It now has a PML4 entry of its own and 512 GiB of room, and
`PMM_DIRECT_MAP_LIMIT` stops describing the address space.

Three things had to be true for the move to be safe:

**The kernel image stays where it is.** `-mcmodel=kernel` puts every symbol in
the top 2 GiB, so the first gigabyte of RAM is still mapped at `KERNEL_BASE` —
that mapping is how the image is reachable at all.

**`vmm_virt_to_phys_direct` has to accept two windows.** Most callers hand it
something `vmm_phys_to_virt` gave them, which is in the direct map. But a DMA
driver could hand it a *static* buffer in the kernel image, living at
`KERNEL_BASE` — rtl8139's receive ring and transmit slots were such arrays until
the driver became a module, and they only ever worked because the direct map
*was* `KERNEL_BASE`. Both windows map physical memory at a fixed offset, so both
can be answered — refusing the second would hand the network card a garbage
address to write into. A module cannot do this at all: its pages are neither
window, which is why `dma_alloc()` is the only way a module gets memory a device
can be pointed at.

**The map has to be built through the old window.** `page_table_pointer` answers
with an address in the direct map, which does not exist while the direct map is
being built. `vmm_init` uses the loader's `KERNEL_BASE` mapping for the
bootstrap and switches once the tables are in place.

There was a fourth thing, and it only showed up at 4 GiB: the loader maps as
much RAM at `KERNEL_BASE` as fits above it, which on a 4 GiB machine is 2047 MiB
— straight through the framebuffer and device windows in the second gigabyte.
The old code hid this by replacing that directory entry with one that stopped
exactly at the framebuffer. With the direct map gone the whole gigabyte is
simply given back, and the 4 KiB device mappings can be made there. On a 2 GiB
machine the loader's mapping ended exactly at the framebuffer, so the collision
was invisible until there was more memory to collide with.

## No ceiling of its own

There is no `PMM_DIRECT_MAP_LIMIT` any more. The direct map spans as many
top-level entries as physical memory needs, up to the 62.5 TiB between
`DIRECT_MAP_BASE` and the device window, and `vmm_init` only builds the gigabytes that
hold something the architecture wants mapped.

The allocator's bitmap and per-page reference counts (32-bit) used to live in
an 8 MiB reserve after the kernel image, which is what capped the machine at
8 GiB. `pmm_init` now sizes them from the memory map and carves them out of the
top of the highest usable region that fits, reaching them through the loader's
direct map until `vmm_init` calls `pmm_use_direct_map()`. The cost is one bit
plus four bytes per page of the tracked range: about 1.1 MiB per GiB.

The tracked range stops at the end of the last region that is RAM. Limine also
reports reserved, bad and framebuffer ranges, and on QEMU a reserved range sits
at 1 TiB; `limine_entry.c` drops those, because counting them would have the
allocator track a terabyte of address space it can never hand out. Reclaimable
regions stay in, which keeps Limine's page tables at 2046 MiB inside the range
on a 2 GiB machine.

## Changing a kernel mapping

The top half of every address space is the same memory: `vmm_create_address_space`
copies the kernel's upper entries into each new table, so the directories below
them are shared by pointer and a mapping added later is visible everywhere
without touching any process.

That sharing is also why unmapping or re-protecting one is not a local
operation. The processor doing it is usually running some process's address
space -- `finit_module` runs in udev's -- so a condition like "invalidate if
this is the address space I have loaded" skips the one processor that most
needs it, and a shootdown aimed at "everyone running this address space" finds
nobody. `vmm.c` treats an address in the kernel half as what it is: it
invalidates locally whatever is loaded, and asks every other processor, through
`smp_flush_kernel_mappings()`. Both are batched by
`vmm_flush_batch_begin/end`, so releasing a heap block or loading a module
costs one interrupt rather than one per page.

A real machine found this and an emulator could not: see
[Modules](modules.md).

### Nothing goes back to the allocator before its shootdown

Batching the shootdown makes the order of the last two steps load-bearing. An
unmap that ends with `pmm_free_page()` hands the page to whoever asks next
while the other processors are still a few instructions away from being told,
and each of them is holding a translation to it that says writable. What lands
in the new owner's page is whatever the old one was still finishing.

So an unmap inside a batch calls `vmm_free_page_after_flush()` instead: the
page is parked until `settle_batch()` has sent the interrupts and waited for
them, and only then goes back to the allocator. Sixty-four of them fit; a
larger unmap settles the batch in the middle rather than growing the list.
`heap_release_pages`, `munmap` and `vmm_prune_empty_tables` all free this way,
and the last of them opens a batch of its own so the page tables it drops are
covered too.

The rule is the one-liner: **nothing goes back to `pmm_free_page` between an
unmap and its shootdown.** An emulator cannot fail it -- QEMU has no TLB to go
stale -- and the laptop in [Modules](modules.md) failed it twice, once as a
root filesystem full of garbage and once as scrambled console glyphs and a dead
init.

## The kernel heap

The heap is the other ceiling, and the one that runs out first on a machine
doing file work: file contents live in `kmalloc`'d buffers, so reading a lot can
exhaust it while the physical allocator still has gigabytes free.

It is 2 GiB now, up from 768 MiB — the old comment justified 768 by a 1 GiB
virtual window that stopped being true when the heap got its own PML4 entry.

More important than the ceiling is when reclaim starts. It used to be three
quarters of the ceiling, which is a fraction of a number that has nothing to do
with the machine: on one with 2 GiB of RAM the heap would exhaust physical
memory long before reaching 1.5 GiB, and reclaim would never run at all. The
condition now asks the question that matters — whether the machine is running
out — with a floor of 64 MiB of free pages, comfortably more than the largest
single thing anything here allocates.

`/proc/meminfo` reports the heap as `Slab`, with `KernelHeapMax` alongside it,
because without the limit the number says nothing: it is not bounded by
`MemTotal`.

## What is still missing

File data lives in the heap as whole-file `kmalloc`'d buffers, reclaimed all at
once per file. A real page cache — pages, an LRU, and eviction by page rather
than by file — is the remaining half of this. It would stop file I/O competing
with everything else for the same allocator, and it is what `vfs_reclaim_file_data`
is standing in for today.

## The heap ceiling follows the machine

Two things are counted at the top of `heap.c` and only one of them is scarce.
The heap extends by mapping fresh pages above what it already has and never
shrinks that virtual extent -- but it does hand the physical pages under a
freed block back to the PMM (`heap_release_pages`). So the extent is address
space, of which there are terabytes, while the memory behind it is returned as
soon as it is not wanted.

A constant 2 GiB ceiling therefore rationed the wrong thing. How that showed up:
a file lives in one contiguous buffer and grows by allocating a bigger one and
copying, which leaves holes behind and pushes the extent to roughly twice the
file. A 677 MB download died asking for 672 MiB with **666 MiB of the heap in
use** --

```
VFS: ...xbps.part cannot grow to 688128 KiB: heap 666 of 2048 MiB
```

-- and the program writing the file was told `Input/output error`, which is a
description of an exhausted address-space quota so misleading it cost a day.

The ceiling is twice the machine's usable memory now, with a 2 GiB floor, and
physical memory is left to be the real limit: `heap_grow()` already fails
gracefully when `pmm_alloc_page()` does. The pressure signal that starts
reclaiming cached file data was measured against the same constant and is now
measured against physical memory, for the same reason -- the extent says
nothing about how close the machine is to running out of anything.

With that, `xbps-install -Sy supertuxkart` finishes: 32 packages, 782 MB of
game data, no kernel complaint on the way through.

The heap owns one top-level entry, so its extent also stops at
`HEAP_VIRTUAL_BYTES` (512 GiB) however much memory the machine has.
