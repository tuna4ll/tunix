# The root filesystem (ext3)

The root is a real ext3 filesystem on the second partition of the boot disk.
The kernel mounts it at boot and writes through to it: reads, `mmap` and `exec`
are served from RAM, every mutation reaches the disk as it happens, and what
you edit is still there next boot.

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

`superblock_usable()` in `kernel/fs/ext2.c` refuses anything it genuinely cannot
read:

- a block size other than 4 KiB -- every buffer in the file is sized to it
- an inode larger than the classic 128 bytes
- a group whose bitmap would not fit in one block
- any incompatible feature but `filetype` -- extents and 64-bit block numbers
  above all
- any read-only-compatible feature but `sparse_super` and `large_file`. The
  name says what the rule is: a driver that does not understand one of these
  may still read the filesystem, but must not write to it. This one writes, so
  it refuses instead of quietly corrupting.

Which is why `support/image.sh` makes the filesystem with an explicit feature
set and an explicit revision rather than mke2fs's defaults:

```sh
mkfs.ext3 -r 1 -b 4096 -I 128 \
    -O ^resize_inode,^dir_index,^ext_attr,^metadata_csum,^64bit,^huge_file,^dir_nlink,^extra_isize
```

`dir_index` is off because the driver reads linear directory entries and writing
into a hashed directory without maintaining the tree would corrupt it for Linux.
`metadata_csum` is off for the same reason: a write that does not update the
checksum makes e2fsck complain about a filesystem that is otherwise fine.

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

### What a mutation costs now

Every metadata write goes to the log first. `meta_write()` no longer reaches
the disk: it stages the block, and `flush_meta()` -- which already ran at the
end of every create, unlink and rename -- commits the staged blocks as one
transaction:

1. descriptor, blocks, commit block, then a device flush
2. `needs_recovery` set in the filesystem superblock, the journal superblock
   pointed at the transaction, flush
3. the blocks written where they actually belong, flush
4. the journal emptied, `needs_recovery` cleared, flush

Step 3 is the only part that existed before. The rest is the price, and it is
a real price: the same metadata is written twice and there are four cache
flushes where there used to be none. A shell loop creating and deleting files
manages about a hundred and forty mutations a second.

What it buys is that there is no moment when the disk holds half of a change.
Crash before step 2 and the transaction never happened; crash during step 3 --
the window that used to be the dangerous one -- and the next mount finds a
committed transaction the disk has not caught up with, and applies it.

Because the log is checkpointed at the end of every transaction rather than
left to fill, only one transaction is ever live, and it always starts at the
first log block. A 64 MiB journal is far more than this needs; it is the size
mke2fs chose and there is no reason to argue with it.

### Replay

`ext3_journal_recover()` runs before the tree is read, because replaying
changes the inodes and directory blocks the tree is built from. It walks the
log twice: once to find where it ends and to collect revoked blocks, once to
write the blocks of every transaction that has a commit block. A descriptor
without a commit after it is a transaction that was interrupted, and it is
where the replay stops.

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

- **Metadata only.** File contents are not journalled, which is what `ext3`
  calls `data=ordered` -- except that the ordering is not enforced either, so a
  file's blocks may reach the disk after the metadata that points at them.
- **One transaction at a time.** The log is checkpointed immediately rather
  than batched, so mutations do not amortise.
- **No extents, no 64-bit block numbers.** 16 GiB is the ceiling.
- **No `dir_index`.** A directory is a linear scan.
- **`fsck` on the running root** is not something to do; the fstab entry has
  pass 0 for exactly that reason.

## A big file grows by a step, not by doubling

A file lives in one contiguous kernel allocation. Growing it means holding the
old buffer and the new one at once while the contents are copied, so doubling
makes the peak one and a half times the file.

That is affordable until it is not. A write crossing 512 MiB asked for a
gigabyte while still holding half of one -- 1.5 GiB of a 2 GiB heap with a
compositor already in it. The allocation failed, the write failed with it, and
what the program saw was an I/O error:

```
supertuxkart-data-1.5_1.x86_64.xbps.part   537001984 bytes
ERROR: [trans] failed to download ...: Input/output error
```

537001984 is 512 MiB and one 128 KiB staging chunk: the first write past the
boundary.

Above 32 MiB the buffer grows by a fixed 32 MiB step instead. The peak becomes
the file plus one step rather than half the file again, and the slack left at
the end is bounded by the step rather than by the file. It does not make the
underlying arrangement right -- a file still has to fit in the heap, contiguously
-- but it moves the ceiling from two thirds of the heap to nearly all of it.
