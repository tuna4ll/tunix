# Memory Management

How user memory is tracked, reclaimed and swapped, and how the kernel keeps
itself out of user pages. The code is in `kernel/mm/` (`vmm.c`, `swap.c`,
`reclaim.c`), `kernel/core/process.c` (memory areas, faults, the OOM killer),
`kernel/fs/vfs.c` (the page cache) and `kernel/drivers/storage/zram.c`.

## Memory areas

Every address space keeps its areas — what `mmap` created — in a sorted list
and, for lookups, in a treap keyed by start address. Faults, `mprotect`,
`munmap` and the search for a free range all start from a tree descent, so a
process with tens of thousands of mappings (a browser or a JIT) costs
O(log n) per fault instead of a walk. The list stays for the callers that
print or copy every area (`/proc/<pid>/maps`, `fork`).

## Usage counters

The page-table registry keeps four counters per address space: anonymous,
file-backed and shared pages that are resident, and swap entries. Every write
of a user page-table entry updates them, so `VmRSS`, `RssAnon`, `RssFile`,
`RssShmem` and `VmSwap` in `/proc/<pid>/status`, `statm`, `stat` and the OOM
killer read them in O(1). When an address space is torn down the counters are
compared with what the teardown actually found; a mismatch prints
`VMM: usage drift`, which the tests treat as a failure.

## Swap

`swapon(2)` accepts a block device or a regular file that carries a Linux swap
header (`mkswap`, `SWAPSPACE2`, version 1, bad-page list honoured). Up to 32
areas are active at once; the highest priority area with free slots is used,
and areas of equal priority take turns. A swap file is mapped to disk sectors
once, at `swapon`, through the filesystem's extent map, and is then written
directly on the block device; while it is active it cannot be written,
truncated (`ETXTBSY`), unlinked or renamed over (`EPERM`). `/proc/swaps`,
`SwapTotal`, `SwapFree` and `SwapCached` in `/proc/meminfo` and `sysinfo(2)`
report the areas.

A swapped page leaves a non-present entry in the page table: bit 0 clear,
bit 1 set, the slot number stored inverted in bits 12–51 (so that a
speculative walk on CPUs affected by L1TF sees an address beyond physical
memory), and the page's write, no-execute, copy-on-write and shared bits kept
in bits 3–6. Every page-table path knows these entries: mapping refuses an
occupied entry, `fork` duplicates the slot reference, teardown and `munmap`
release it, `mprotect` rewrites the kept bits.

Pages pass through a swap cache, hashed both by slot and by physical page. A
page that is mapped by several processes (after `fork`, or a shared anonymous
mapping) is swapped out by each of them separately: the first one moves it
into the cache with a fresh slot, the others take another reference on the
same slot. It is written out only once nobody maps it, and dropped from memory
once it is clean. A fault on a swap entry finds the page in the cache or reads
it back; the page is mapped writable only when this entry was the last
reference to the slot and nobody else maps the page, otherwise private pages
come back copy-on-write.

## Reclaim

`kswapd0` wakes when free memory falls below the low watermark and works until
it is above the high one (`min` is usable pages / 128, between 1 MiB and 64 MiB;
low and high are two and three times that). A user allocation below `min`
reclaims for itself first, and below `min / 2` it does not take a page at all,
which leaves a reserve for the page cache, page tables and zram.

A reclaim round:

1. writes back dirty swap-cache pages and frees clean ones;
2. drops clean page-cache pages that no process maps, least recently used
   files first, from a list of files that have cached pages;
3. scans page tables, round robin over address spaces, with a cursor kept per
   address space. A page whose accessed bit is set has it cleared (x86 `A`,
   arm64 `AF`; arm64 without hardware access flags takes an access-flag fault
   that sets it again). A page found idle is swapped out if it is anonymous,
   or unmapped if it is a page-cache page of a file mapping, which refaults
   from the cache. Device mappings (DRM buffers, the sound ring, framebuffers),
   memfd mappings and shared writable file mappings are left alone.

An address space is scanned when its lock can be taken, or when its owner is
parked: a thread that waits for memory while holding its address-space lock
marks the address space as parked and does not touch it until every scanner
has left. Without this the process that needs memory most would be the one
reclaim could never reach.

Remote TLBs are flushed before any page a scan or an `munmap` removed is
freed.

## OOM killer

When reclaim makes no progress sixteen times in a row the OOM killer chooses
the thread group with the most resident and swapped pages, adjusted by
`oom_score_adj` (`/proc/<pid>/oom_score_adj`, `oom_adj`, read-only
`oom_score`; `-1000` exempts a process, only root lowers a value). It sends
`SIGKILL` to the group and waits up to three seconds for it to exit before
choosing another. A fault that fails because its own process was chosen
delivers that `SIGKILL` rather than a `SIGSEGV`. The kernel log line names the
victim, its anonymous, file, shared and swapped memory, free memory, free swap
and the reclaim counters.

## zram

`/dev/zram0` (block major 252) is a compressed RAM disk. Writing a size to
`/sys/block/zram0/disksize` allocates it; `reset` frees it once nothing swaps
to it. Pages are compressed with the LZ4 codec from `kernel/rust/lz4` (see
[Rust](rust.md)); pages filled with one repeated 64-bit word are stored as
that word, and pages that compress to more than three quarters are stored as
they are. `mm_stat`, `io_stat`, `mem_limit`, `mem_used_max`, `comp_algorithm`
and `max_comp_streams` follow Linux, so `zramctl` works, and `modprobe zram`
succeeds because the image lists zram as built in. When a swap slot on zram
is freed, the swap code tells the device, so memory a process gave back is
returned within about a second.

The images enable it at boot from
`/etc/runit/core-services/04-zram-swap.sh`: half of RAM, at most 4 GiB,
priority 100. `nozram` on the kernel command line turns that off.

## Keeping the kernel out of user pages

The kernel never dereferences a user pointer: `copy_from_user` and friends
translate the address and copy through the direct map. The hardware enforces
it where it can: SMEP and SMAP on x86-64 (CR4, enabled on every CPU, `AC` is
cleared on every entry), PAN on arm64 when `ID_AA64MMFR1_EL1` reports it, and
PXN on every arm64 user page. The kernel and modules are built with
`-fstack-protector-strong`; the guard is randomised before the first protected
function returns. The boot log says which guards are active:
`VMM: user pages guarded from the kernel (execute, access)`.

## Tests

`make test` runs `memory` (areas, usage counters, `oom_score_adj`, the OOM
killer), `swap` (a 768 MiB swap file made in the guest; more memory than RAM,
`fork`, shared mappings, `mprotect`, `munmap` and `swapoff` on swapped pages,
the swap-file guards) and `zram` (block I/O, same-filled pages, swap on
zram0 with compression and discard checks). The `swap` and `zram` tests boot
with 320 MiB of RAM; `MEMORY=` and `AARCH64_CPU=` choose the guest for any
test (`AARCH64_CPU=max` exercises PAN).
