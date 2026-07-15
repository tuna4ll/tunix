# Persistent Root Filesystem (ext2)

Tunix uses the disk as the authoritative copy of the root filesystem, the
Linux way. The whole tree — `/home`, `/etc`, `/bin`, everything — lives on a
real ext2 filesystem on the boot disk and survives reboots. RAM acts as the
cache: reads, `mmap`, and `exec` are served from memory, while every mutation
is written through to disk as it happens.

## Boot flow

1. **First boot** (blank or invalid filesystem region): the kernel unpacks
   the initramfs into RAM as usual, formats the disk region as ext2, and
   seeds it with the entire tree. The superblock `s_state` flag is only set
   to "clean" after seeding completes, so an interrupted seed is re-done on
   the next boot instead of booting a half-written system.
2. **Every later boot**: the kernel probes the superblock early, skips
   loading the initramfs entirely, and rebuilds the VFS tree from the ext2
   filesystem. The system you boot is the disk, not the image — edits to
   `/etc`, packages dropped into `/usr`, files in `/home` all persist.

Volatile directories — `/tmp`, `/var/tmp`, `/run`, `/dev`, `/proc` — are
marked `VFS_VOLATILE` and behave like Linux tmpfs mounts: they exist every
boot but nothing under them touches the disk.

Rebuilding the disk image (`make all` after source changes) recreates
`build/tunix.img`, which erases the filesystem and triggers a fresh seed.
Plain `make run` without a rebuild keeps everything.

## On-disk format

The image builder reserves at least 64 MiB after the initramfs (1 MiB
aligned; `DATA_REGION_BYTES` in `scripts/build-image.py`), and the kernel
formats it as **rev-1 ext2**: 4 KiB blocks, 128-byte inodes, 8192 inodes,
a single block group (up to 128 MiB), and the `filetype` dirent feature —
the same core format the Linux ext4 driver mounts natively.

```
| block 0: superblock @1024 | 1: group desc | 2: block bitmap |
| 3: inode bitmap | 4..259: inode table | 260...: data |
```

Files use the classic 12 direct + single + double indirect block map.
Symlinks shorter than 60 bytes are stored inline in the inode.

Inspecting the filesystem from Linux:

```sh
# region start is printed by the image build ("ext2 data region: lba N")
dd if=build/tunix.img of=/tmp/root.ext2 bs=512 skip=40960
e2fsck -fn /tmp/root.ext2
debugfs -R 'ls -l /' /tmp/root.ext2
sudo mount -o loop /tmp/root.ext2 /mnt
```

## How writes reach the disk

The driver registers `vfs_persist_ops` hooks (`src/kernel/include/vfs.h`)
and the VFS invokes them on every mutation:

| VFS operation | ext2 action |
|---|---|
| create file/dir/symlink | allocate inode, write inode, add dirent |
| write | map/allocate blocks, write data, update size |
| truncate | free blocks, rewrite remaining content |
| rename | move dirent, fix `..` and link counts across dirs |
| move in/out of a volatile dir | persist/unpersist the whole subtree |
| unlink/rmdir | remove dirent, free blocks and inode |
| chmod | rewrite inode mode/uid/gid |

`sync`/`fsync`/`fdatasync`/`syncfs` force the ATA cache flush; `fsync` also
rewrites the file's blocks, which covers writes made through shared `mmap`
mappings that bypass the write path.

Because the kernel is never preempted while running in kernel context
(`process.c` only reschedules on interrupts arriving from user mode), each
operation's disk updates complete atomically with respect to other
processes; no locking is needed.

## Kernel pieces

- `src/kernel/ata.c` — `ata_pio_write28()` and `ata_flush_cache()`.
- `src/kernel/ext2.c` — superblock/group descriptor handling, bitmaps,
  inode table, block mapping, directory entries, in-kernel mkfs, the boot
  loader/seeder, and the write-through event handlers.
- `src/kernel/vfs.c` — `vfs_persist_ops` hook points on every tree mutation.
- `src/kernel/main.c` — probes the disk before touching the initramfs and
  picks the boot source.

## Limits

- No journal: like classic ext2, a power cut in the middle of a multi-block
  update can leave the filesystem inconsistent (repair with `e2fsck` from
  Linux, or rebuild the image for a fresh seed). Ordinary shutdowns and QEMU
  exits are safe because every operation is written through immediately.
- 8192 inodes, single block group (≤128 MiB). The tree is also cached in
  RAM, so total filesystem content is bounded by kernel heap headroom as
  well (96 MiB).
- Deleting system files persists too — `rm /bin/bash` really bricks the
  installed system, exactly like on Linux; rebuild the image to reinstall.
- Timestamps persist at second granularity; `utimensat` changes are not
  written back.
