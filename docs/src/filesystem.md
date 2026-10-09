# The root filesystem (ext3)

The root is a real ext3 filesystem on the second partition of the boot disk.
The kernel mounts it at boot and writes back to it the way Linux does: reads,
`mmap` and `exec` are served from RAM, a `write()` returns once the bytes are in
the page cache, and the disk catches up within a few seconds -- or at once, for
`fsync`, `sync` and `umount`. What you edit is still there next boot.

## Where it comes from

`mkfs.ext3 -d` on the build host, from the Void sysroot. That is a change: the
kernel used to make the filesystem itself, formatting a region of the disk on
first boot and seeding it from an initramfs. Both are gone -- there is no
initramfs, and there is no format code in the kernel.

Which means the driver no longer gets to assume the layout it would have
written. It used to check that every group's bitmaps and inode table were
exactly where its own format code would have put them, and refuse the disk
otherwise. mke2fs does none of that reliably: most groups have no superblock
backup (`sparse_super`), and reserved growth blocks push the rest along. So the
group descriptors are read and believed, and what is checked is only that each
one points inside the filesystem.

## What the driver can mount

`superblock_usable()` in `kernel/fs/ext2.c` takes what mke2fs makes for ext2 and
ext3 by default, and refuses only what it genuinely cannot write correctly:

- blocks of 1, 2 or 4 KiB. A 4 KiB page is read and written as the blocks that
  cover it, so a smaller block only means more of them per page.
- inodes of any power-of-two size from 128 bytes up. The first 128 are the
  classic inode; a new inode gets its extra space zeroed and `i_extra_isize`
  set, which is what `extra_isize` asks for.
- files through triple-indirect blocks, and sizes past 4 GiB through
  `i_size_high`, setting `large_file` the first time a file needs it.
- `dir_index` directories are read as the linear directories they also are.
  The first change to one clears its index flag, so the directory stays valid
  and Linux simply stops using the hash for it.
- `ext_attr` blocks are released, or their reference count dropped, when the
  inode goes. `resize_inode` needs nothing: its blocks are already allocated.
- `dir_nlink`: a directory past 65000 subdirectories has its link count pinned
  to 1, as Linux does.
- any other incompatible feature -- extents and 64-bit block numbers above all
  -- and any other read-only-compatible one (`huge_file`, `metadata_csum`) stop
  the mount. A driver that does not understand one of those may still read the
  filesystem, but must not write to it.

So `tools/image.sh` asks only for what mke2fs would otherwise add on a
system whose `mke2fs.conf` leans towards ext4:

```sh
mkfs.ext3 -r 1 -b 4096 -I 256 -O ^metadata_csum,^64bit,^huge_file
```

## More than one filesystem

Every mounted ext2 or ext3 filesystem is a volume of its own: its block device,
superblock, group descriptors, block caches, allocation cursors and journal.
`mount -t ext2 /dev/sdb /mnt` (or `ext3`) reads it into a tree the way the root
is read at boot, and the nodes it creates remember their volume, so every
change goes to the disk it came from.

A rename between two volumes becomes what `mv` would do by hand: the contents
are read in, the inode is released on the old disk and created on the new one.
A file that is still open when its last name goes keeps its inode, with a link
count of zero, until the last descriptor closes; that is also when its blocks
are freed. `umount` refuses with `EBUSY` while anything under the mount point is
open, mapped, used as a working directory or mounted on, and otherwise commits
the journal and marks the filesystem clean.

## The journal

`kernel/fs/ext3.c` is the journal, and it is the thing that makes this ext3
rather than ext2 with a spare file in it. It writes the log that `mkfs.ext3`
laid down, and it replays one it finds.

The format is JBD, the same log ext3 and ext4 use on Linux, and every field in
it is big-endian. A transaction is a descriptor block naming the filesystem
blocks it carries, then those blocks, then a commit block. All three carry the
magic `0xC03B3998` and the transaction's sequence number, and the journal
superblock says where the oldest live transaction starts and what sequence to
expect there. That is the whole protocol; there is nothing else to agree on,
which is why a log this kernel wrote is one e2fsck reads.

### How changes reach the disk

The journal runs in ext3's default mode, `data=ordered`: metadata goes through
the log, file contents are written straight to where they belong, and they are
written before the transaction that makes them reachable commits.

Metadata lives in a buffer cache (`buf_get()`, `buf_zero()`, `buf_mark()` in
`kernel/fs/ext2.c`): one entry per block, hashed, on an LRU list, eight
megabytes per volume. A create, unlink or rename changes buffers and marks them
dirty; nothing is written yet. The superblock and group descriptors are kept in
memory and compared against the last copy written, so only a descriptor block
whose counts changed goes out.

A commit copies every dirty buffer into a snapshot, sorted by block number,
while it holds the ext2 lock, marks the buffers clean and in flight, and lets
the lock go. The snapshot is then handed to `ext3_journal_commit()` as one
transaction:

1. descriptor blocks and the blocks they name, gathered into runs, then a
   device flush
2. the commit block, flush -- from here the transaction survives a crash
3. the blocks written to where they belong, flush
4. the journal superblock moved past the transaction, flush

A transaction may carry more blocks than one descriptor names; it simply has
several descriptors before its commit block. `needs_recovery` is set in the
filesystem superblock when the volume is mounted and cleared when it is
unmounted, not once per transaction.

None of those writes happens under the ext2 lock. A buffer changed while its
snapshot is on the way to the disk is simply dirty again and goes out with the
next transaction, and a buffer in flight is never evicted, so nothing reads the
old copy back from the disk before the new one lands. Commits are ordered
against each other and against file data by a second lock, `ext2 io`, which is
held only by whoever is writing: the commit thread, a data write-back, `fsync`
and unmount. Creating, renaming and deleting files, and reading them, need only
the ext2 lock and wait for memory work, not for the stick.

A commit happens when the volume has had something dirty for five seconds (the
`ext2commit` thread checks once a second), when the dirty buffers reach half
the journal or half the buffer cache, on `fsync`, `sync` and `msync(MS_SYNC)`,
and on unmount and power-off. xbps unpacking a package therefore costs one
commit every few seconds instead of four cache flushes per file. A program that
changes metadata faster than the disk takes it -- twice the threshold without a
commit -- does the next commit itself at the end of its system call, holding no
other lock, the way a writer is throttled for file data.

A block freed in the running transaction is not handed out again until that
transaction has committed (`free_block()` queues it; `volume_commit()` releases
it). Otherwise a crash could replay the old owner's metadata on top of data the
new owner had already written there -- the reason ext3 on Linux has the same
rule.

### Writing file contents

`write()` copies into the page cache, marks the pages dirty and puts the file on
a list. The `flush` kernel thread (`kernel/fs/vfs.c`) writes a file back once it
has been dirty for five seconds, or sooner when the dirty pages pass five per
cent of memory. Each batch of up to 32 pages is copied out under the VFS lock;
its blocks are allocated and the inode updated under the ext2 lock, and the data
is written with only `ext2 io` held. The metadata that points at new blocks
cannot commit before those blocks are written, since the commit needs the same
lock -- ordered mode, as on Linux. A program that writes faster than the disk takes it is made
to wait in `vfs_balance_dirty()` once dirty pages pass ten per cent of memory
(128 MiB at most), the way Linux throttles a writer.

A batch knows which inode and which truncate generation it was taken from, and
`ext2_writeback()` drops it if the file has since been deleted or truncated;
the pages it carried are marked dirty again if they still exist.

Reading a page that is not in the cache goes to the disk without the VFS lock
held (`vfs_prefetch()`), and the page is put in the cache only if nothing
filled that slot meanwhile. The ext2 lock is held only while the page's blocks
are looked up, not while they are read.

### What the old way cost

Before this, every `write()` went through the log with its contents
(`data=journal`) and committed on the spot, with four cache flushes and the
superblock and every group descriptor rewritten each time, all while one lock
over the whole kernel was held. Writing 24 MiB to a USB stick limited to
2 MB/s and 60 writes a second took
307 seconds, and in that time a thread sleeping 5 ms woke up to 3.2 seconds
late -- which on the real laptop was the mouse and the clock stopping during
`xbps-install`. The same write now returns from `write()` in 17 ms, the sleeping
thread is at most 19 ms late, and reading a cached file on the root meanwhile
takes at most 104 ms. The `fsync` at the end takes as long as the stick needs.

That is one file. `xbps-install -Sy gimp` onto a root on a USB stick
(QEMU, xHCI, 8 MB/s and 100 writes a second) still stopped the Weston clock for
three to thirteen seconds at a time, dozens of times. Measuring every lock held
or waited on for more than 50 ms showed why: the commit thread and the data
write-back held the ext2 lock for up to 400 ms while the stick worked, every
metadata change asked for an immediate commit, and renames and deletes holding
the VFS lock waited behind them -- with `weston-desktop-shell` queued behind
those in its own lookups and page faults. With the disk work moved out from
under the lock the same install ran for fifteen minutes and the clock never
stood still for more than two seconds -- and that once, just after switching
back to the desktop.

### Replay

`ext3_journal_recover()` runs before the tree is read, because replaying
changes the inodes and directory blocks the tree is built from. It walks the
log twice: once to find where it ends and to collect revoked blocks, once to
write the blocks of every transaction that has a commit block. A descriptor
without a commit after it is a transaction that was interrupted, and it is
where the replay stops.

A transaction whose blocks do not fit in one descriptor has several, and the
walk continues through them to the commit block.

Two details of the format matter and are handled. A block whose first four
bytes happen to be the journal's own magic is written to the log with those
bytes zeroed and an `ESCAPE` flag on its tag, and put back on the way out. And
a revoke block cancels a replay for the blocks it names, which is how a journal
written by Linux avoids restoring a metadata block that has since been freed
and reused for file data.

```
EXT3: replaying the journal from transaction 6581
```

That line is from a real crash: `kill -9` on the emulator in the middle of the
churn, chosen by repetition until one landed between the commit and the
checkpoint. Linux's own `e2fsck -fn` on that image said it was skipping journal
recovery because it had been asked not to write; Tunix booted, replayed, and
`e2fsck` afterwards found nothing wrong.

The same done over a `dd` loop catches a transaction with a file in it. Reading
the descriptor block off the disk of one such crash:

```
descriptor magic 0xc03b3998 type 1 sequence 4656
the transaction carries 37 blocks
EXT3: replaying the journal from transaction 4656
```

Thirty-seven blocks, most of them the contents of the file being written.

### What is refused

Any JBD feature the code does not implement -- checksums, 64-bit block numbers,
asynchronous commit, fast commit -- stops the mount rather than being ignored,
the same rule the filesystem superblock's incompatible features get. `mkfs.ext3`
sets none of them.

The image is a plain ext3 filesystem, so it mounts on Linux:

```sh
sudo mount -o loop,offset=$((133120*512)) build/tunix.img /mnt
```

## Volatile directories

`/tmp`, `/var/tmp`, `/run`, `/dev` and `/proc` are marked `VFS_VOLATILE` and
behave like tmpfs mounts: they exist every boot and nothing under them touches
the disk. So does a FIFO -- `mkfifo` makes an in-memory node, because a FIFO
carries nothing across a reboot and persisting one would mean teaching the
driver a file type whose contents do not exist.

The sysroot's own `/dev`, `/proc`, `/sys`, `/run` and `/tmp` are emptied before
the image is made. Anything left in them would sit underneath the kernel's own
and shadow it: an install script that redirected to `/dev/null` once left a
twenty-byte regular file there, and every `>/dev/null` in the system then failed
with `EACCES`.

## Partitions

`kernel/fs/partition.c` reads GPT and MBR partition tables and registers what they
describe as block devices of their own -- `sda1`, `sda2` -- so the filesystem
starts where its device does and `root=/dev/sda2` names it. Both schemes are
read because both are used: the image carries a protective MBR in front of the
GPT so that one disk boots either firmware.

## What is not here

- **One live transaction.** A transaction is checkpointed as soon as it has
  committed, so the log never holds more than one; batching comes from how long
  a transaction stays open, not from keeping several in the log.
- **No revoke blocks are written.** They are understood on the way in, which is
  what matters for replaying a log Linux left behind. This kernel does not need
  them: a freed block is not reused before its transaction commits, and a
  committed transaction is checkpointed before the next one starts.
- **One lock for every ext2 volume.** The ext2 lock and `ext2 io` are shared by
  all mounted ext2 and ext3 volumes, so two disks are not written in parallel.
- **No extents, no 64-bit block numbers.** Block numbers are 32 bits, so a
  filesystem ends at 16 TiB with 4 KiB blocks, and `i_blocks` counts sectors in
  32 bits, so a single file ends at 2 TiB.
- **No hashed lookups on disk.** The tree is in memory and hashed there; on the
  disk a directory is written linearly.
- **No orphan list.** A file deleted while open is freed when it closes; if the
  machine dies first, `e2fsck` finds the inode and frees it.
- **No LRU for clean pages.** The cache is trimmed by how long ago a file was
  last used, whole files at a time, once it passes half of memory. Dirty pages
  are bounded separately: a writer waits once they pass ten per cent of memory.
- **`fsck` on the running root** is not something to do; the fstab entry has
  pass 0 for exactly that reason.

## The page cache

A file's contents are not one buffer any more. They are 4 KiB pages, each its
own physical frame from the page allocator, held in a per-file array with a
dirty bit each.

That is what `kernel/fs/vfs.c` now does and it changes four things.

**A file no longer has to fit in the heap.** It used to live in a single
contiguous allocation, so the ceiling was the largest run the heap could hand
out, and growing it meant holding the old copy and the new one at once while
the bytes were copied. A write crossing 512 MiB asked for a gigabyte while
still holding half of one, failed, and the program saw an I/O error:

```
supertuxkart-data-1.5_1.x86_64.xbps.part   537001984 bytes
ERROR: [trans] failed to download ...: Input/output error
```

A 2.6 GiB file now writes on a 3 GiB machine, survives a reboot and reads back
with the checksum it was written with.

**Only what is touched is read.** Opening a file used to pull all of it off the
disk. A page is fetched when something reads or writes it, and a hole reads as
zeros without touching the medium at all. `truncate -s 2G` costs nothing: the
file is two gigabytes long and holds no pages.

**What is read is read in bulk.** A page fault or a `read()` fetches the whole
aligned 128 KiB window around the page (`VFS_READAHEAD_PAGES`), the way Linux
reads ahead and reads around. `ext2_fetch_pages()` maps every block of the
window under the ext2 lock in one pass and then reads each run of adjacent
blocks with one request, without the lock. Before, starting a program faulted
its binary and libraries in one 4 KiB page at a time, and each page was a USB
command of its own. On a root stick where a command costs 2.5 ms (QEMU,
xHCI), the first run of each program took:

| | a page per command | read ahead, 64 KiB commands |
| --- | --- | --- |
| `fastfetch` | 1930 ms | 60 ms |
| `python3` importing five modules | 5292 ms | 724 ms |
| `git --version` | 3956 ms | 178 ms |
| `cat` of a 45 MB library | 23.8 s | 1.3 s |

The second run of each takes milliseconds either way; it is served from the
cache.

**Only what changed is written.** Write-back takes the pages whose dirty bit is
set and clears it; a clean page is never written. Before, every write-back
rewrote the whole file.

**Memory comes back a page at a time.** `vfs_drop_clean_pages()` frees the
pages of a file that are clean, because the disk can hand them back. A dirty
page stays until it has been written. Clean pages of files nobody has touched
for a while are dropped once the cache passes half of memory, and all of them
when free memory runs low. The ceiling used to be a sixteenth of memory and at
most 128 MiB, which on a laptop meant reading the same libraries off the stick
again every few minutes.

### What the pages cost

An array of pointers and a bitmap, one entry per page: about two megabytes of
pointers for a gigabyte of file. The array doubles as the file grows, and that
copy is of the pointers, not of the contents.

`mmap` got simpler rather than harder. A shared mapping used to need the whole
file moved onto a page boundary first -- `vfs_align_data()`, now gone -- because
the heap does not align what it hands out. A page from the page allocator is a
physical frame already, so mapping one into a process is a page-table entry and
nothing else.

The ELF loader no longer reads through a flat pointer either. It reads the
header and program headers into a small buffer and copies each segment through
`vfs_read()`, which means starting a program touches the pages of the segments
it actually loads.
