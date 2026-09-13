# Memory Layout

Where the kernel puts things in the virtual address space, and why the machine
could only use 1792 MiB of RAM until it stopped putting them there. It reflects
the code as it exists today.

## The windows

Everything the kernel addresses lives in the top half. Four regions matter:

| Base | What | Size |
| --- | --- | --- |
| `0xFFFFFE8000000000` | direct map — all of physical memory | 512 GiB of room |
| `0xFFFFFF0000000000` | kernel heap | grows on demand |
| `0xFFFFFFFF80000000` | kernel image, and the first GiB of RAM | 1 GiB |
| `0xFFFFFFFFF0000000` | framebuffer | as large as the mode needs |
| `0xFFFFFFFFFF000000` | device registers | 16 MiB |

The last three share one PML4 entry — the top 2 GiB — because that is where
`-mcmodel=kernel` requires every symbol to be. The first two have entries of
their own and are only there because a pointer can be computed to them.

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
something `vmm_phys_to_virt` gave them, which is in the direct map. But the DMA
drivers hand it a *static* buffer: rtl8139's receive ring and transmit slots are
plain arrays in the kernel image, living at `KERNEL_BASE`. They only ever worked
because the direct map *was* `KERNEL_BASE`. Both windows map physical memory at
a fixed offset, so both can be answered — refusing the second would hand the
network card a garbage address to write into.

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

## The new ceiling, and what it is made of

`PMM_DIRECT_MAP_LIMIT` is 8 GiB, and it now limits bookkeeping rather than
address space. The allocator keeps a bitmap and a per-page reference count, both
reserved after the kernel image by the linker script, costing about 544 KiB per
gigabyte of RAM. 8 GiB needs 4.25 MiB of the 8 MiB reserved there. Raising one
without the other overruns the reserve silently, which is why the two constants
are commented against each other.

The structures span every address the firmware describes, not just the part of
it that is RAM, and that is deliberate. They used to stop at the top of the
highest usable region, which broke a machine with exactly 2 GiB:

```
PMM: 2035 MiB usable of 2035 MiB installed, ceiling 8192 MiB
PANIC: VMM: invalid boot CR3
```

Limine leaves its own page tables at 2046 MiB on every machine, in a region it
reports as reclaimable rather than usable. With 3 GiB or more the usable regions
run past that address, so the page is inside the tracked range and marked in use
like every other non-usable page. With exactly 2 GiB the usable memory *ends*
at 2046 MiB, the page falls outside the range, and `pmm_page_is_allocated`
answers what it answers for any address it does not track: no. `vmm_init` reads
that as a corrupt CR3 and stops.

The direct map had the same hole from the same cause -- it is built up to
`pmm_managed_limit()` -- so the page the kernel was about to walk was not
mapped either.

So `highest` is now the top of the last region of any kind, and only the
counters that report how much RAM there is stay usable-only. The cost is
bookkeeping for the whole 8 GiB on every machine, which is 4.25 MiB of a
reserve the linker script already sets aside in full.

`pmm_init` reports all of this on the console rather than behind the debug flag:

```
PMM: 4077 MiB managed of 4095 MiB installed, ceiling 8192 MiB
```

The middle number is what the firmware said the machine has, so a kernel that is
leaving memory on the table says so plainly instead of quietly capping.

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
