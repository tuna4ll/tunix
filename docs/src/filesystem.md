# The root filesystem (ext4)

The root is a real ext4 filesystem on the second partition of the boot disk,
made with the same defaults `mkfs.ext4` uses on any current Linux system:
`metadata_csum`, `64bit`, extents, `flex_bg`, `huge_file`, `orphan_file`,
hashed directories and a journal with checksums. The kernel mounts it at boot
and writes back to it the way Linux does: reads, `mmap` and `exec` are served
from RAM, a `write()` returns once the bytes are in the page cache, and the
disk catches up within a few seconds -- or at once, for `fsync`, `sync` and
`umount`. What you edit is still there next boot, and Linux's `e2fsck` finds
nothing to fix in it.

## Where it comes from

`mkfs.ext4 -d` on the build host, from a root directory. The kernel finds and
mounts it itself, with no initramfs in between (the one there is carries
drivers, not a root; see [Boot](boot.md#the-initramfs)), and there is no format
code in the kernel; the group descriptors are read and
believed, and what is checked is that each one points inside the filesystem
and carries the right checksum.

## The driver

`fs/ext4/` is one driver for ext2, ext3 and ext4, the way Linux has
one. `mount -t ext2`, `ext3` and `ext4` all reach it, and `/proc/mounts` names
the root after the features it actually has.

| File | What it holds |
| --- | --- |
| `super.c` | volumes, the metadata buffer cache, commits, superblock and group descriptor checksums, mount and unmount |
| `balloc.c` | block and inode bitmaps, `BLOCK_UNINIT`/`INODE_UNINIT` groups, `bg_itable_unused` |
| `inode.c` | inodes and their checksums, 32-bit owners, nanosecond and creation times, `huge_file` block counts, indirect blocks |
| `extents.c` | the extent tree: lookup, insertion with merging, leaf and index splits, growing the tree, unwritten extents, truncation |
| `dir.c` | directory entries and their checksum tails, hashed (`dir_index`) directories and the three Linux name hashes |
| `xattr.c` | extended attributes in the inode and in shared blocks, EA inodes, POSIX ACLs |
| `inline.c` | `inline_data` files and directories |
| `orphan.c` | the orphan list and the orphan file |
| `mmp.c` | multi-mount protection |
| `persist.c` | what the VFS asks for: create, unlink, rename, truncate, write-back, read and loading the tree |

`fs/jbd2.c` is the journal and `lib/crc.c` the CRC32C, CRC32 and
CRC16 everything is checksummed with.

## What the driver can mount

Every feature a current `mkfs.ext4` can turn on falls in one of three rows:

| | Features |
| --- | --- |
| read and written | `has_journal`, `ext_attr`, `resize_inode`, `dir_index`, `filetype`, `sparse_super`, `sparse_super2`, `large_file`, `huge_file`, `extent`, `64bit`, `flex_bg`, `meta_bg`, `uninit_bg`, `metadata_csum`, `metadata_csum_seed`, `dir_nlink`, `extra_isize`, `inline_data`, `large_dir`, `ea_inode`, `mmp`, `orphan_file`, `fast_commit` (the journal area is kept, no fast commits are written) |
| mounted read-only | `bigalloc`, `quota`, `project`, `verity`, `casefold`, `encrypt` (encrypted directories are left out of the tree), and any read-only-compatible feature the driver does not know |
| refused | `journal_dev` (an external journal), `compression`, `dirdata`, blocks larger than a page, and any incompatible feature the driver does not know |

A volume is also read-only when it is mounted with `MS_RDONLY` or its device
cannot be written; every node it loads is then marked read-only, so writes
fail with `EROFS`.

### What is on the disk

- **Block numbers are 64 bits** everywhere: the buffer cache, the allocator,
  extents, the journal's tags and revoke records, group descriptors of 64
  bytes.
- **Checksums**: the superblock, every group descriptor, both bitmaps of a
  group, every inode, extent tree blocks, directory blocks (in the twelve-byte
  tail entry), hashed directory index blocks, extended attribute blocks, orphan
  file blocks, the MMP block and the journal. They are verified when read and
  set when written; group descriptors and bitmaps are sealed at commit time,
  everything else when it is changed.
- **Groups mkfs never initialised** (`BLOCK_UNINIT`, `INODE_UNINIT`) get their
  bitmaps built the first time something is allocated in them, the way Linux
  does it, and `bg_itable_unused` follows the highest inode handed out.
- **Extents**: new files and directories are extent-mapped. Writing next to an
  existing extent extends it, so a file written front to back is a handful of
  extents; a full leaf splits at the insertion point, a full index splits in
  half, and a full root moves into a block of its own and the tree grows a
  level. Writing into an unwritten (`fallocate`d) extent splits it so only the
  block written becomes initialised. Indirect-mapped files from ext2 and ext3
  keep working.
- **Hashed directories**: a directory that outgrows its first block becomes an
  htree, entries go to the leaf their hash selects, full leaves split by hash,
  full index blocks split and, with `large_dir`, the tree grows to three
  levels. The legacy, half-MD4 and TEA hashes, signed and unsigned, are all
  there, so directories Linux indexed are kept indexed.
- **Inodes** carry 32-bit owners, nanosecond `atime`, `mtime` and `ctime`,
  the creation time `statx()` reports as `stx_btime`, and the epoch bits that
  keep times valid past 2038.
- **Extended attributes**: `user.`, `trusted.` and `security.` attributes and
  POSIX access ACLs live in the inode while they fit and in a block after
  that. A block shared by several inodes is copied before it is changed;
  values Linux put in EA inodes are read, and their inodes freed with the last
  reference. Attributes the VFS has no use for (default ACLs, `system.data`)
  are kept as they were.
- **Inline data**: small files and directories Linux stored inside the inode
  are read in place and moved to a block the first time they are written.
- **Orphans**: a file unlinked while open goes on the orphan file (or the
  classic `s_last_orphan` list on older filesystems), so a crash before the
  last `close()` frees it at the next mount, and the mount does.
- **FIFOs, sockets and device nodes** are stored like any other inode. `mknod`
  makes character and block device nodes for root; they keep their numbers but
  are not bound to a driver.
- **MMP**: a filesystem with multi-mount protection is claimed with the same
  wait-and-check Linux does, its sequence is renewed by the commit thread, and
  it is marked clean on unmount.

## More than one filesystem

Every mounted filesystem is a volume of its own: its block device, superblock,
group descriptors, block caches, allocation cursors and journal.
`mount -t ext4 /dev/sdb /mnt` reads it into a tree the way the root is read at
boot, and the nodes it creates remember their volume, so every change goes to
the disk it came from.

A rename between two volumes becomes what `mv` would do by hand: the contents
are read in, the inode is released on the old disk and created on the new one.
`umount` refuses with `EBUSY` while anything under the mount point is open,
mapped, used as a working directory or mounted on, and otherwise commits the
journal and marks the filesystem clean.

## The journal

`fs/jbd2.c` is the journal. It writes the log that `mkfs` laid down,
and it replays one it finds, whoever wrote it.

The format is JBD2, the log ext3 and ext4 use on Linux, and every field in it
is big-endian. A transaction is a descriptor block naming the filesystem blocks
it carries, then those blocks, then a commit block. All three carry the magic
`0xC03B3998` and the transaction's sequence number, and the journal superblock
says where the oldest live transaction starts and what sequence to expect
there.

What ext4 adds on top is understood both ways:

- **64-bit block numbers** in descriptor tags and revoke records
  (`journal_64bit`).
- **Checksums** (`journal_checksum_v2`, `v3`): CRC32C over the journal
  superblock, a tail on every descriptor and revoke block, a checksum of every
  logged block in its tag, and one over the commit block. The older
  `COMPAT_CHECKSUM` (a CRC32 over the whole transaction in the commit block) is
  checked and written too.
- **Asynchronous commit** needs nothing: a commit block is only written after
  the blocks before it have reached the disk.
- **Fast commits** (`fast_commit`) are never written, and the area they live in
  is kept out of the log. A journal Linux left with fast commits still to
  replay is refused rather than half replayed.

As Linux does, mounting an ext4 filesystem read-write turns on `64bit` and
`checksum_v3` in its journal; ext3 journals are left as they are.

### How changes reach the disk

The journal runs in ext3's default mode, `data=ordered`: metadata goes through
the log, file contents are written straight to where they belong, and they are
written before the transaction that makes them reachable commits.

Metadata lives in a buffer cache (`buf_get()`, `buf_zero()`, `buf_mark()` in
`fs/ext4/super.c`): one entry per block, hashed, on an LRU list, eight
megabytes per volume. A create, unlink or rename changes buffers and marks them
dirty; nothing is written yet. The superblock and group descriptors are kept in
memory and compared against the last copy written, so only a descriptor block
whose counts changed goes out.

A commit copies every dirty buffer into a snapshot, sorted by block number,
while it holds the ext4 lock, marks the buffers clean and in flight, and lets
the lock go. The snapshot is then handed to `jbd2_journal_commit()` as one
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

None of those writes happens under the ext4 lock. A buffer changed while its
snapshot is on the way to the disk is simply dirty again and goes out with the
next transaction, and a buffer in flight is never evicted, so nothing reads the
old copy back from the disk before the new one lands. Commits are ordered
against each other and against file data by a second lock, `ext4 io`, which is
held only by whoever is writing: the commit thread, a data write-back, `fsync`
and unmount. Creating, renaming and deleting files, and reading them, need only
the ext4 lock and wait for memory work, not for the stick.

A commit happens when the volume has had something dirty for five seconds (the
`ext4commit` thread checks once a second), when the dirty buffers reach half
the journal or half the buffer cache, on `fsync`, `sync` and `msync(MS_SYNC)`,
and on unmount and power-off. xbps unpacking a package therefore costs one
commit every few seconds instead of four cache flushes per file. A program that
changes metadata faster than the disk takes it -- twice the threshold without a
commit -- does the next commit itself at the end of its system call, holding no
other lock, the way a writer is throttled for file data.

A block freed in the running transaction is not handed out again until that
transaction has committed (`free_block()` queues it; the next commit releases
it). Otherwise a crash could replay the old owner's metadata on top of data the
new owner had already written there -- the reason ext3 on Linux has the same
rule.

### Writing file contents

`write()` copies into the page cache, marks the pages dirty and puts the file on
a list. The `flush` kernel thread (`fs/vfs.c`) writes a file back once it
has been dirty for five seconds, or sooner when the dirty pages pass five per
cent of memory. Each batch of up to 32 pages is copied out under the VFS lock;
its blocks are allocated and the inode updated under the ext4 lock, and the data
is written with only `ext4 io` held. The metadata that points at new blocks
cannot commit before those blocks are written, since the commit needs the same
lock -- ordered mode, as on Linux. A program that writes faster than the disk takes it is made
to wait in `vfs_balance_dirty()` once dirty pages pass ten per cent of memory
(128 MiB at most), the way Linux throttles a writer.

A batch knows which inode and which truncate generation it was taken from, and
`writeback()` in `persist.c` drops it if the file has since been deleted or truncated;
the pages it carried are marked dirty again if they still exist.

Reading a page that is not in the cache goes to the disk without the VFS lock
held (`vfs_prefetch()`), and the page is put in the cache only if nothing
filled that slot meanwhile. The ext4 lock is held only while the page's blocks
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
write-back held the filesystem lock for up to 400 ms while the stick worked, every
metadata change asked for an immediate commit, and renames and deletes holding
the VFS lock waited behind them -- with `weston-desktop-shell` queued behind
those in its own lookups and page faults. With the disk work moved out from
under the lock the same install ran for fifteen minutes and the clock never
stood still for more than two seconds -- and that once, just after switching
back to the desktop.

### Replay

`jbd2_journal_recover()` runs before the tree is read, because replaying
changes the inodes and directory blocks the tree is built from. It walks the
log three times, as Linux does: once to find where it ends, once to collect the
revoke records of the transactions that committed, and once to write their
blocks. A descriptor without a valid commit after it is a transaction that was
interrupted, and it is where the replay stops; a descriptor or commit block
whose checksum is wrong ends the log the same way, and a logged block whose own
checksum is wrong is skipped and reported.

A transaction whose blocks do not fit in one descriptor has several, and the
walk continues through them to the commit block.

Two details of the format matter and are handled. A block whose first four
bytes happen to be the journal's own magic is written to the log with those
bytes zeroed and an `ESCAPE` flag on its tag, and put back on the way out. And
a revoke block cancels a replay for the blocks it names, which is how a journal
written by Linux avoids restoring a metadata block that has since been freed
and reused for file data.

```
JBD2: replaying transactions 6581 to 6581
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
JBD2: replaying transactions 4656 to 4656
```

Thirty-seven blocks, most of them the contents of the file being written.

### Talking to Linux

Both directions are checked against Linux. `debugfs` writes a checksummed
64-bit transaction into a second disk and leaves it unreplayed;
Tunix replays it at mount and the file it changed reads back changed. And after
Tunix has run, the test rewinds the root's journal superblock to the last
transaction Tunix wrote and has `e2fsck` replay it: a single wrong byte in a
descriptor, a tag or the commit block makes `e2fsck` report a checksum error
or a corrupt transaction, and it reports nothing.

Any journal feature the code does not know stops the mount rather than being
ignored, the same rule the filesystem superblock's incompatible features get.

The image is a plain ext4 filesystem, so Linux tools read it without
mounting it:

```sh
debugfs -R "ls -l /" "tunix.img?offset=$((133120*512))"
```

## Volatile directories

`/tmp`, `/var/tmp`, `/run`, `/dev` and `/proc` are marked `VFS_VOLATILE` and
behave like tmpfs mounts: they exist every boot and nothing under them touches
the disk. Anywhere else a FIFO, a socket or a device node is an inode on the
disk like on Linux: its name, owner and mode survive a reboot, its contents
never had any.

A root directory's own `/dev`, `/proc`, `/sys`, `/run` and `/tmp` must be empty
when the image is made. Anything left in them would sit underneath the kernel's own
and shadow it: an install script that redirected to `/dev/null` once left a
twenty-byte regular file there, and every `>/dev/null` in the system then failed
with `EACCES`.

## A live root

With `live` on the command line, the root is mounted as a read-only volume
whose files can still be changed, which is how Tulpar's ISO runs from a disc
or a USB stick it never writes to. The tree is loaded as for any mount, and
nothing is written to the disk: creating, removing, renaming files and changing
their owners and modes all happen in the tree in memory, as on tmpfs. A file's
contents stay on the disk until the first write, truncation or writable shared
mapping, which reads in the part of it that is kept and turns it into a file
in memory (`VFS_LIVE`, cleared by `live_copy_up` in `fs/vfs.c`). Until then its
pages are a cache like any other, dropped under memory pressure and read again.

`/tmp`, `/var/tmp` and `/run` are tmpfs as on a writable root, and `statfs`
reports the root as writable, since writes to it succeed.

## Locks

`flock` locks belong to the open file. `fcntl` record locks are kept per inode
in `fs/posix_lock.c`, with Linux's two kinds of owner: a classic POSIX
lock (`F_SETLK`, `F_SETLKW`, `F_GETLK`) belongs to the descriptor table, so
closing any descriptor of the file drops every lock the process holds on it,
and an open-file-description lock (`F_OFD_*`) belongs to the open file and goes
when its last descriptor closes. Ranges split and merge the way Linux's do,
`F_GETLK` reports the first conflicting lock and its owner's pid, and a waiter
whose sleep would close a cycle of POSIX lock owners gets `EDEADLK`.

## FIFOs

Opening a FIFO waits for the other end, as POSIX asks: a reader until a writer
has opened it and a writer until a reader has. `O_NONBLOCK` makes the reader
return at once and the writer fail with `ENXIO`, and `O_RDWR` opens both ends
without waiting. A read end reports `POLLHUP` only once a writer has come and
gone. Writes of up to `PIPE_BUF` bytes to any pipe are atomic.

## Partitions

`fs/partition.c` reads GPT and MBR partition tables and registers what they
describe as block devices of their own -- `sda1`, `sda2` -- so the filesystem
starts where its device does and `root=/dev/sda2` names it. Both schemes are
read because both are used: the image carries a protective MBR in front of the
GPT so that one disk boots either firmware, and Tulpar's ISO has an MBR alone.

A disc is read the same way. An optical drive (`sr0`), on AHCI or on either
IDE channel, master or slave, answers SCSI commands through ATAPI and has
2048-byte blocks, which `drivers/storage/cdrom.c` turns into the 512-byte
sectors the block layer counts, so the partition table of a hybrid ISO is found
on a CD just as on a USB stick: `sr0p3` is the ISO's third partition.
Partitions of a device whose name ends in a digit get a `p`, as on Linux. The
drive is read-only.

Discs come and go. A drive is registered with or without one, and a thread
asks every drive whether it is ready every two seconds (as Linux polls one).
When the drive reports that the disc may have changed, through its unit
attention, reads fail until the block layer has caught up: `sr0`'s size
becomes the new disc's, or 0 with none, the old disc's partitions die -- a file
open or a file system mounted on one gets errors from then on, and never the
new disc's data -- and the new disc's partitions are registered, by the same
names, which resolve to the newest. `/dev` and the device events follow. The
drive with the root on it has its tray locked, as Linux locks a mounted disc's.

## What is not here

- **One live transaction.** A transaction is checkpointed as soon as it has
  committed, so the log never holds more than one; batching comes from how long
  a transaction stays open, not from keeping several in the log.
- **No revoke blocks are written.** They are understood on the way in, which is
  what matters for replaying a log Linux left behind. This kernel does not need
  them: a freed block is not reused before its transaction commits, and a
  committed transaction is checkpointed before the next one starts.
- **One lock for every volume.** The ext4 lock and `ext4 io` are shared by all
  mounted volumes, so two disks are not written in parallel.
- **The whole tree is read at mount.** Lookups never go to the disk; the hashed
  index is maintained for Linux and for adding entries, not for finding them.
- **No delayed allocation, no preallocation.** Blocks are allocated when a page
  is written back, next to the file's last block when it is free.
  `fallocate()` modes other than plain allocation are not offered.
- **No writing what needs a key or a table.** Encrypted directories are left
  out of the tree, casefolded names are compared as they are, and quota files
  are not updated, which is why those filesystems mount read-only.
- **`MS_REMOUNT` does not switch a volume between read-only and read-write.**
- **Blocks no larger than a page.** 8 KiB to 64 KiB blocks are refused.
- **No LRU for clean pages.** The cache is trimmed by how long ago a file was
  last used, whole files at a time, once it passes half of memory. Dirty pages
  are bounded separately: a writer waits once they pass ten per cent of memory.
- **`fsck` on the running root** is not something to do; the fstab entry has
  pass 0 for exactly that reason.

## The page cache

A file's contents are not one buffer any more. They are 4 KiB pages, each its
own physical frame from the page allocator, held in a per-file array with a
dirty bit each.

That is what `fs/vfs.c` now does and it changes four things.

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
reads ahead and reads around. `fetch_pages()` in `persist.c` maps every block
of the window under the ext4 lock in one pass and then reads each run of adjacent
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
